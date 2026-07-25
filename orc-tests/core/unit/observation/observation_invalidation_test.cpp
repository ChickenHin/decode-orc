/*
 * File:        observation_invalidation_test.cpp
 * Module:      orc-core tests
 * Purpose:     Unit tests for fingerprint diffing and the retention window
 *              (Phase 3 Tasks 3.1 and 3.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_invalidation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_executor.h"
#include "frame_provenance.h"
#include "observation_store.h"

namespace orc {
namespace {

// ---------------------------------------------------------------------------
// Test doubles (mirrors frame_provenance_test's minimal DAG builders)
// ---------------------------------------------------------------------------

class FakeArtifact final : public Artifact {
 public:
  explicit FakeArtifact(std::string id)
      : Artifact(ArtifactID(std::move(id)), Provenance{}) {}
  std::string type_name() const override { return "FakeArtifact"; }
};

class MockStage : public DAGStage {
 public:
  MockStage(NodeType type, std::string name, std::string version,
            size_t input_count, size_t output_count)
      : type_(type),
        name_(std::move(name)),
        version_(std::move(version)),
        input_count_(input_count),
        output_count_(output_count) {}

  std::string version() const override { return version_; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(
        type_, name_, name_, "", static_cast<uint32_t>(input_count_),
        static_cast<uint32_t>(input_count_),
        static_cast<uint32_t>(output_count_),
        static_cast<uint32_t>(output_count_), VideoFormatCompatibility::ALL,
        SinkCategory::CORE, "Test");
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*observation_context*/) override {
    std::vector<ArtifactPtr> outputs;
    outputs.reserve(output_count_);
    for (size_t i = 0; i < output_count_; ++i) {
      outputs.push_back(std::make_shared<FakeArtifact>(name_ + "_out"));
    }
    return outputs;
  }

  size_t required_input_count() const override { return input_count_; }
  size_t output_count() const override { return output_count_; }

 private:
  NodeType type_;
  std::string name_;
  std::string version_;
  size_t input_count_;
  size_t output_count_;
};

class MockFileIdentityProvider final : public IFileIdentityProvider {
 public:
  FileIdentity identify(const std::string& /*path*/) const override {
    return FileIdentity{};
  }
};

DAGStagePtr make_source() {
  return std::make_shared<MockStage>(NodeType::SOURCE, "Source", "1.0.0", 0, 1);
}

DAGStagePtr make_transform(std::string name = "Transform") {
  return std::make_shared<MockStage>(NodeType::TRANSFORM, std::move(name),
                                     "1.0.0", 1, 1);
}

DAGNode make_node(NodeID id, DAGStagePtr stage,
                  std::map<std::string, ParameterValue> params = {},
                  std::vector<NodeID> inputs = {}) {
  DAGNode node;
  node.node_id = id;
  node.stage = std::move(stage);
  node.parameters = std::move(params);
  node.input_indices.assign(inputs.size(), 0);
  node.input_node_ids = std::move(inputs);
  return node;
}

std::vector<NodeID::value_type> node_values(const std::vector<NodeID>& nodes) {
  std::vector<NodeID::value_type> out;
  out.reserve(nodes.size());
  for (const auto& n : nodes) {
    out.push_back(n.value());
  }
  return out;
}

NodeFingerprint fp(const std::string& value) { return NodeFingerprint{value}; }

// ---------------------------------------------------------------------------
// diff_node_fingerprints — Task 3.1
// ---------------------------------------------------------------------------

TEST(DiffNodeFingerprints_NoOpRebuild_YieldsEmptyDiff, Empty) {
  MockFileIdentityProvider provider;
  auto build = [] {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), make_source()));
    dag->add_node(
        make_node(NodeID(2), make_transform(), {{"gain", 2.0}}, {NodeID(1)}));
    return dag;
  };

  const auto before = compute_node_fingerprints(*build(), provider);
  const auto after = compute_node_fingerprints(*build(), provider);

  const auto diff = diff_node_fingerprints(before, after);
  EXPECT_TRUE(diff.empty());
  EXPECT_TRUE(diff.changed_nodes.empty());
  EXPECT_TRUE(diff.removed_fingerprints.empty());
}

TEST(DiffNodeFingerprints_ParameterEdit_ChangesEditedNodeAndDescendants,
     Scoped) {
  MockFileIdentityProvider provider;

  // Chain: 1 -> 2 -> 3, plus an unrelated branch: 4 -> 5.
  auto build = [](double gain_on_node2) {
    auto dag = std::make_shared<DAG>();
    dag->add_node(make_node(NodeID(1), make_source()));
    dag->add_node(make_node(NodeID(2), make_transform("T2"),
                            {{"gain", gain_on_node2}}, {NodeID(1)}));
    dag->add_node(make_node(NodeID(3), make_transform("T3"), {}, {NodeID(2)}));
    dag->add_node(make_node(NodeID(4), make_source()));
    dag->add_node(make_node(NodeID(5), make_transform("T5"), {}, {NodeID(4)}));
    return dag;
  };

  const auto before = compute_node_fingerprints(*build(1.0), provider);
  const auto after = compute_node_fingerprints(*build(2.0), provider);

  const auto diff = diff_node_fingerprints(before, after);

  // Exactly the edited node and its descendant, sorted.
  EXPECT_EQ(node_values(diff.changed_nodes),
            (std::vector<NodeID::value_type>{2, 3}));

  // The two now-unreachable content identities (old fingerprints of 2 and 3).
  ASSERT_EQ(diff.removed_fingerprints.size(), 2u);
  std::vector<NodeFingerprint> expected_removed{before.at(NodeID(2)),
                                                before.at(NodeID(3))};
  std::sort(expected_removed.begin(), expected_removed.end());
  EXPECT_EQ(diff.removed_fingerprints, expected_removed);
}

TEST(DiffNodeFingerprints_TopologyEdit_YieldsAffectedChain, Scoped) {
  MockFileIdentityProvider provider;

  // Base: 1 -> 2.
  auto base = std::make_shared<DAG>();
  base->add_node(make_node(NodeID(1), make_source()));
  base->add_node(make_node(NodeID(2), make_transform("T2"), {}, {NodeID(1)}));

  // Modified: 1 -> 3(new) -> 2.
  auto modified = std::make_shared<DAG>();
  modified->add_node(make_node(NodeID(1), make_source()));
  modified->add_node(
      make_node(NodeID(3), make_transform("T3"), {}, {NodeID(1)}));
  modified->add_node(
      make_node(NodeID(2), make_transform("T2"), {}, {NodeID(3)}));

  const auto before = compute_node_fingerprints(*base, provider);
  const auto after = compute_node_fingerprints(*modified, provider);

  const auto diff = diff_node_fingerprints(before, after);

  // The inserted node (3) and the node whose input changed (2); the untouched
  // source (1) is not reported.
  EXPECT_EQ(node_values(diff.changed_nodes),
            (std::vector<NodeID::value_type>{2, 3}));

  // Node 2's old fingerprint is no longer reachable.
  ASSERT_EQ(diff.removed_fingerprints.size(), 1u);
  EXPECT_EQ(diff.removed_fingerprints.front(), before.at(NodeID(2)));
}

TEST(DiffNodeFingerprints_NodeRemoval_ReportsRemovedFingerprintOnly, Scoped) {
  MockFileIdentityProvider provider;

  auto before_dag = std::make_shared<DAG>();
  before_dag->add_node(make_node(NodeID(1), make_source()));
  before_dag->add_node(
      make_node(NodeID(2), make_transform("T2"), {}, {NodeID(1)}));

  auto after_dag = std::make_shared<DAG>();
  after_dag->add_node(make_node(NodeID(1), make_source()));

  const auto before = compute_node_fingerprints(*before_dag, provider);
  const auto after = compute_node_fingerprints(*after_dag, provider);

  const auto diff = diff_node_fingerprints(before, after);

  // Nothing new to compute; node 2's fingerprint is now unreachable.
  EXPECT_TRUE(diff.changed_nodes.empty());
  ASSERT_EQ(diff.removed_fingerprints.size(), 1u);
  EXPECT_EQ(diff.removed_fingerprints.front(), before.at(NodeID(2)));
}

// ---------------------------------------------------------------------------
// ObservationRetentionWindow — Task 3.2
// ---------------------------------------------------------------------------

TEST(RetentionWindow_RetainSet_UnionsCurrentAndWindow, Basic) {
  ObservationRetentionWindow window;
  window.record_unreachable({fp("old_a"), fp("old_b")});

  NodeFingerprintMap current;
  current[NodeID(1)] = fp("cur_a");
  current[NodeID(2)] = fp("cur_b");

  const auto keep = window.retain_set(current);
  EXPECT_EQ(keep.count(fp("cur_a")), 1u);
  EXPECT_EQ(keep.count(fp("cur_b")), 1u);
  EXPECT_EQ(keep.count(fp("old_a")), 1u);
  EXPECT_EQ(keep.count(fp("old_b")), 1u);
  EXPECT_EQ(keep.count(fp("absent")), 0u);
}

TEST(RetentionWindow_EvictsOldestBeyondCapacity, Bounded) {
  ObservationRetentionWindow window(2);
  window.record_unreachable({fp("a")});
  window.record_unreachable({fp("b")});
  window.record_unreachable({fp("c")});  // "a" ages out.

  EXPECT_EQ(window.size(), 2u);
  const auto keep = window.retain_set(NodeFingerprintMap{});
  EXPECT_EQ(keep.count(fp("a")), 0u);
  EXPECT_EQ(keep.count(fp("b")), 1u);
  EXPECT_EQ(keep.count(fp("c")), 1u);
}

TEST(RetentionWindow_ReRecording_MovesFingerprintToNewest, Rearmed) {
  ObservationRetentionWindow window(2);
  window.record_unreachable({fp("a"), fp("b")});
  window.record_unreachable({fp("a")});  // Re-arm "a": window is now [b, a].
  window.record_unreachable({fp("c")});  // Drops oldest ("b"): window [a, c].

  EXPECT_EQ(window.size(), 2u);
  const auto keep = window.retain_set(NodeFingerprintMap{});
  EXPECT_EQ(keep.count(fp("a")), 1u);
  EXPECT_EQ(keep.count(fp("b")), 0u);
  EXPECT_EQ(keep.count(fp("c")), 1u);
}

// ---------------------------------------------------------------------------
// Retention window driving ObservationStore GC — Task 3.2 (end to end)
// ---------------------------------------------------------------------------

ObservationRecord tiny_record() {
  ObservationRecord record;
  record["ns"]["k"] = static_cast<int32_t>(1);
  return record;
}

// Records observed for a fingerprint survive an edit that makes the fingerprint
// unreachable, so an undo (which restores the fingerprint) reuses them without
// re-running any observer.
TEST(RetentionWindow_UndoAfterEdit_ReusesStoredObservations, ZeroRecompute) {
  ObservationStore store;
  ObservationRetentionWindow window;

  // State A: node 1 has fingerprint "A" with a stored observation.
  NodeFingerprintMap map_a;
  map_a[NodeID(1)] = fp("A");
  const ObservationRecordKey key_a{fp("A"), FieldID(0), "white_snr", "1.0.0"};
  store.put(key_a, tiny_record());

  // Edit to state B: node 1 now fingerprints "B". "A" becomes unreachable.
  NodeFingerprintMap map_b;
  map_b[NodeID(1)] = fp("B");
  const auto edit_diff = diff_node_fingerprints(map_a, map_b);
  window.record_unreachable(edit_diff.removed_fingerprints);
  store.retain_only(window.retain_set(map_b), store.memory_budget_bytes());

  // The observation for the previous content is retained by the undo window.
  EXPECT_TRUE(store.has(key_a));

  // Undo back to state A: fingerprint "A" is reachable again and its stored
  // record is still present — no observer needs to run.
  const auto undo_diff = diff_node_fingerprints(map_b, map_a);
  window.record_unreachable(undo_diff.removed_fingerprints);
  store.retain_only(window.retain_set(map_a), store.memory_budget_bytes());
  EXPECT_TRUE(store.has(key_a));
}

// A fingerprint that stays unreachable long enough to fall out of the retention
// window is garbage-collected from the store.
TEST(RetentionWindow_UnreachableBeyondWindow_EvictsFromStore, Collected) {
  ObservationStore store;
  ObservationRetentionWindow window(1);  // Keep only the most recent removal.

  const ObservationRecordKey key_a{fp("A"), FieldID(0), "white_snr", "1.0.0"};
  store.put(key_a, tiny_record());

  // Edit 1: "A" removed, "B" reachable. Window now holds "A".
  window.record_unreachable({fp("A")});
  NodeFingerprintMap only_b;
  only_b[NodeID(1)] = fp("B");
  store.retain_only(window.retain_set(only_b), store.memory_budget_bytes());
  EXPECT_TRUE(store.has(key_a));

  // Edit 2: "B" removed, "C" reachable. Capacity-1 window drops "A".
  window.record_unreachable({fp("B")});
  NodeFingerprintMap only_c;
  only_c[NodeID(1)] = fp("C");
  store.retain_only(window.retain_set(only_c), store.memory_budget_bytes());
  EXPECT_FALSE(store.has(key_a));
}

}  // namespace
}  // namespace orc
