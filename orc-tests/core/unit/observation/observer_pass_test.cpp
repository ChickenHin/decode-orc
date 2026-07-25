/*
 * File:        observer_pass_test.cpp
 * Module:      orc-core tests
 * Purpose:     Read-through observer pass + store survives DAG rebuild
 *              (Tasks 2.2 and 2.3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/stage.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_executor.h"
#include "dag_frame_renderer.h"
#include "frame_provenance.h"
#include "observation_store.h"
#include "observation_test_doubles.h"

namespace orc {
namespace {

using test::FakeVideoFrameRepresentation;
using test::make_observer_info;
using test::SpyObservationService;

std::vector<ObserverInfo> two_observers() {
  return {make_observer_info("white_snr", "1.0.0"),
          make_observer_info("biphase", "2.0.0")};
}

// Compare the full observation payload of two contexts across both fields of a
// frame.
void expect_contexts_equal(const IObservationContext& a,
                           const IObservationContext& b, FrameID frame_id) {
  for (FieldID::value_type f = 0; f < 2; ++f) {
    const FieldID field(frame_id * 2 + f);
    EXPECT_EQ(a.get_all_observations(field), b.get_all_observations(field))
        << "field " << field.value();
  }
}

// ---------------------------------------------------------------------------
// Task 2.2 — read-through observer pass
// ---------------------------------------------------------------------------

TEST(RunFrameObserverPass_SecondRenderSameFrame_RunsNoObservers, ReadThrough) {
  SpyObservationService spy;
  spy.observers = two_observers();
  ObservationStore store;
  FakeVideoFrameRepresentation vfr;
  const NodeFingerprint fp{"nodefingerprint-A"};
  const FrameID frame = 3;

  ObservationContext first;
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fp, &store, first);
  EXPECT_EQ(spy.total_runs, 2);  // one run per observer (both misses)

  ObservationContext second;
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fp, &store, second);
  EXPECT_EQ(spy.total_runs, 2);  // no additional runs: pure store hits

  // Values loaded from the store match freshly computed ones.
  expect_contexts_equal(first, second, frame);
}

TEST(RunFrameObserverPass_NoStore_AlwaysRuns, BehavesAsBefore) {
  SpyObservationService spy;
  spy.observers = two_observers();
  FakeVideoFrameRepresentation vfr;
  const FrameID frame = 0;

  ObservationContext ctx1;
  run_frame_observer_pass(spy, spy.observers, vfr, frame,
                          /*fingerprint=*/nullptr,
                          /*store=*/nullptr, ctx1);
  ObservationContext ctx2;
  run_frame_observer_pass(spy, spy.observers, vfr, frame, nullptr, nullptr,
                          ctx2);

  EXPECT_EQ(spy.total_runs, 4);  // 2 observers × 2 passes, nothing cached
  expect_contexts_equal(ctx1, ctx2, frame);
}

TEST(RunFrameObserverPass_FingerprintWithoutStore_DoesNotCache, NoStore) {
  SpyObservationService spy;
  spy.observers = two_observers();
  FakeVideoFrameRepresentation vfr;
  const NodeFingerprint fp{"x"};

  ObservationContext ctx;
  run_frame_observer_pass(spy, spy.observers, vfr, 0, &fp, nullptr, ctx);
  run_frame_observer_pass(spy, spy.observers, vfr, 0, &fp, nullptr, ctx);
  EXPECT_EQ(spy.total_runs, 4);  // no store => always runs
}

TEST(RunFrameObserverPass_EmptyObserverRecord_StillHits, EmptyCached) {
  SpyObservationService spy;
  spy.observers = {make_observer_info("silent", "1.0.0")};
  ObservationStore store;
  FakeVideoFrameRepresentation vfr;
  const NodeFingerprint fp{"fp"};

  ObservationContext ctx;
  run_frame_observer_pass(spy, spy.observers, vfr, 0, &fp, &store, ctx);
  EXPECT_EQ(spy.total_runs, 1);
  run_frame_observer_pass(spy, spy.observers, vfr, 0, &fp, &store, ctx);
  EXPECT_EQ(spy.total_runs, 1);  // empty record still suppresses re-run
}

TEST(RunFrameObserverPass_DifferentFrame_IsSeparateEntry, PerFrame) {
  SpyObservationService spy;
  spy.observers = {make_observer_info("white_snr", "1.0.0")};
  ObservationStore store;
  FakeVideoFrameRepresentation vfr;
  const NodeFingerprint fp{"fp"};

  ObservationContext ctx;
  run_frame_observer_pass(spy, spy.observers, vfr, 0, &fp, &store, ctx);
  run_frame_observer_pass(spy, spy.observers, vfr, 1, &fp, &store, ctx);
  EXPECT_EQ(spy.total_runs, 2);  // frame 0 and frame 1 are distinct keys
}

TEST(RunFrameObserverPass_ObserverVersionBump_Invalidates, VersionKeyed) {
  SpyObservationService spy;
  ObservationStore store;
  FakeVideoFrameRepresentation vfr;
  const NodeFingerprint fp{"fp"};

  std::vector<ObserverInfo> v1 = {make_observer_info("white_snr", "1.0.0")};
  ObservationContext c1;
  run_frame_observer_pass(spy, v1, vfr, 0, &fp, &store, c1);
  EXPECT_EQ(spy.total_runs, 1);

  // Same observer id, newer version → miss, re-run.
  std::vector<ObserverInfo> v2 = {make_observer_info("white_snr", "2.0.0")};
  ObservationContext c2;
  run_frame_observer_pass(spy, v2, vfr, 0, &fp, &store, c2);
  EXPECT_EQ(spy.total_runs, 2);
}

// ---------------------------------------------------------------------------
// Task 2.3 — store survives DAG rebuilds (fingerprint-keyed reuse)
// ---------------------------------------------------------------------------

// Minimal non-source stage for provenance fingerprinting.
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
    return {};
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

class NullFileIdentityProvider final : public IFileIdentityProvider {
 public:
  FileIdentity identify(const std::string& /*path*/) const override {
    return FileIdentity{};
  }
};

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

// Two independent branches: A = node 1 -> 2, B = node 3 -> 4.
std::shared_ptr<DAG> build_two_branch_dag(double gain_on_branch_a) {
  auto dag = std::make_shared<DAG>();
  dag->add_node(make_node(
      NodeID(1),
      std::make_shared<MockStage>(NodeType::SOURCE, "SrcA", "1.0.0", 0, 1)));
  dag->add_node(make_node(
      NodeID(2),
      std::make_shared<MockStage>(NodeType::TRANSFORM, "TxA", "1.0.0", 1, 1),
      {{"gain", gain_on_branch_a}}, {NodeID(1)}));
  dag->add_node(make_node(
      NodeID(3),
      std::make_shared<MockStage>(NodeType::SOURCE, "SrcB", "1.0.0", 0, 1)));
  dag->add_node(make_node(
      NodeID(4),
      std::make_shared<MockStage>(NodeType::TRANSFORM, "TxB", "1.0.0", 1, 1),
      {{"gain", 5.0}}, {NodeID(3)}));
  return dag;
}

TEST(ObservationStore_SurvivesRebuild_UnaffectedBranchReused,
     ContentAddressed) {
  SpyObservationService spy;
  spy.observers = {make_observer_info("white_snr", "1.0.0")};
  ObservationStore store;  // persists across the "rebuild"
  FakeVideoFrameRepresentation vfr;
  NullFileIdentityProvider provider;
  const FrameID frame = 0;

  // Initial build: observe leaf of branch A (node 2) and branch B (node 4).
  const auto dag1 = build_two_branch_dag(/*gain_on_branch_a=*/2.0);
  const auto fps1 = compute_node_fingerprints(*dag1, provider);

  ObservationContext ctx;
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fps1.at(NodeID(2)),
                          &store, ctx);
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fps1.at(NodeID(4)),
                          &store, ctx);
  ASSERT_EQ(spy.total_runs, 2);

  // Rebuild after editing a parameter on branch A only. Branch B's fingerprint
  // is unchanged; branch A's changes.
  const auto dag2 = build_two_branch_dag(/*gain_on_branch_a=*/9.0);
  const auto fps2 = compute_node_fingerprints(*dag2, provider);
  ASSERT_EQ(fps1.at(NodeID(4)), fps2.at(NodeID(4)));  // branch B stable
  ASSERT_NE(fps1.at(NodeID(2)), fps2.at(NodeID(2)));  // branch A changed

  // Requesting the unaffected branch B triggers no observer runs (store hit).
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fps2.at(NodeID(4)),
                          &store, ctx);
  EXPECT_EQ(spy.total_runs, 2);

  // Requesting the edited branch A recomputes (fingerprint miss).
  run_frame_observer_pass(spy, spy.observers, vfr, frame, &fps2.at(NodeID(2)),
                          &store, ctx);
  EXPECT_EQ(spy.total_runs, 3);
}

}  // namespace
}  // namespace orc
