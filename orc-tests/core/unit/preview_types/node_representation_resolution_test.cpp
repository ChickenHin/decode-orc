/*
 * File:        node_representation_resolution_test.cpp
 * Module:      orc-tests/core/unit/preview_types
 * Purpose:     Resolving a node's VideoFrameRepresentation costs a DAG
 *              execution and nothing more — no frame render, no observer pass
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/stage.h>
#include <orc/stage/video_frame_representation.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dag_frame_renderer.h"
#include "preview_renderer.h"

namespace orc {
namespace {

// A VFR that counts the calls the observer pass makes. run_frame_observer_pass
// begins by reading the frame's descriptor (the padding-frame check), so a
// non-zero descriptor count is a reliable marker that observers ran.
class ProbeVfr final : public VideoFrameRepresentation, public Artifact {
 public:
  ProbeVfr(const std::string& id, size_t frames)
      : Artifact(ArtifactID(id), Provenance{}), frames_(frames) {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override {
    return {0, static_cast<FrameID>(frames_ == 0 ? 0 : frames_ - 1)};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(FrameID id) const override { return id < frames_; }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    ++descriptor_calls;
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    ++frame_calls;
    return nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return {};
  }

  mutable int descriptor_calls = 0;
  mutable int frame_calls = 0;

 private:
  size_t frames_;
};

// Source with two outputs, so a sink's edge index is distinguishable: output 0
// carries one frame, output 1 carries two.
class TwoOutputSource final : public DAGStage {
 public:
  std::string version() const override { return "1.0.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "two_output_source",
                        "two_output_source", "", 0, 0, 2, 2,
                        VideoFormatCompatibility::ALL);
  }
  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    ++executes;
    out0 = std::make_shared<ProbeVfr>("branch_0", 1);
    out1 = std::make_shared<ProbeVfr>("branch_1", 2);
    return {out0, out1};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 2; }

  int executes = 0;
  std::shared_ptr<ProbeVfr> out0;
  std::shared_ptr<ProbeVfr> out1;
};

class Sink final : public DAGStage {
 public:
  std::string version() const override { return "1.0.0"; }
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SINK, "probe_sink", "probe_sink", "", 1, 1, 0,
                        0, VideoFormatCompatibility::ALL);
  }
  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    return {};
  }
  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }
};

DAGNode make_node(NodeID id, DAGStagePtr stage, std::vector<NodeID> inputs,
                  std::vector<size_t> input_indices) {
  DAGNode node;
  node.node_id = id;
  node.stage = std::move(stage);
  node.input_node_ids = std::move(inputs);
  node.input_indices = std::move(input_indices);
  return node;
}

// Source at node 1; sink at node 2 fed from the given source output.
std::shared_ptr<DAG> build_dag(size_t connected_output,
                               std::shared_ptr<TwoOutputSource> source) {
  auto dag = std::make_shared<DAG>();
  dag->add_node(make_node(NodeID(1), std::move(source), {}, {}));
  dag->add_node(make_node(NodeID(2), std::make_shared<Sink>(), {NodeID(1)},
                          {connected_output}));
  return dag;
}

// ---------------------------------------------------------------------------
// resolve_node_vfr
// ---------------------------------------------------------------------------

TEST(ResolveNodeVfr, ResolvesANodesOwnFirstOutput) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(1, source);
  DAGExecutor executor;
  const auto outputs = executor.execute_to_node(*dag, NodeID(1));

  const auto resolved = resolve_node_vfr(*dag, outputs, NodeID(1));
  EXPECT_EQ(resolved.status, NodeVfrResolution::kOk);
  EXPECT_EQ(resolved.source_node, NodeID(1));
  EXPECT_EQ(resolved.output_index, 0u);
  EXPECT_FALSE(resolved.substituted_upstream);
  ASSERT_TRUE(resolved.representation);
  EXPECT_EQ(resolved.representation->frame_count(), 1u);
}

// A sink produces no output of its own, so resolution substitutes the upstream
// output its edge names — not output 0, which may be a different branch.
TEST(ResolveNodeVfr, SinkSubstitutesTheConnectedUpstreamOutput) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(1, source);
  DAGExecutor executor;
  const auto outputs = executor.execute_to_node(*dag, NodeID(2));

  const auto resolved = resolve_node_vfr(*dag, outputs, NodeID(2));
  EXPECT_EQ(resolved.status, NodeVfrResolution::kOk);
  EXPECT_TRUE(resolved.substituted_upstream);
  EXPECT_EQ(resolved.source_node, NodeID(1));
  EXPECT_EQ(resolved.output_index, 1u);
  ASSERT_TRUE(resolved.representation);
  EXPECT_EQ(resolved.representation->frame_count(), 2u);
}

TEST(ResolveNodeVfr, ReportsANodeThatProducedNothing) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(0, source);
  const std::map<NodeID, std::vector<ArtifactPtr>> empty;

  EXPECT_EQ(resolve_node_vfr(*dag, empty, NodeID(1)).status,
            NodeVfrResolution::kNoOutput);
  EXPECT_EQ(resolve_node_vfr(*dag, empty, NodeID(2)).status,
            NodeVfrResolution::kSinkWithoutUpstream);
}

// ---------------------------------------------------------------------------
// PreviewRenderer metadata probes
// ---------------------------------------------------------------------------

// Metadata queries (video parameters, audio pairs, line samples) only need the
// node's artifact. Obtaining it by rendering frame 0 also ran the full observer
// pass over that frame — recomputed every time and discarded, since this
// renderer has no observation store to serve it from or write it to.
TEST(PreviewRendererRepresentation, ProbeRunsNoObserverPass) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(0, source);
  PreviewRenderer renderer(dag);

  auto repr = renderer.get_representation_at_node(NodeID(1));
  ASSERT_TRUE(repr);

  const auto* probe = dynamic_cast<const ProbeVfr*>(repr.get());
  ASSERT_TRUE(probe);
  EXPECT_EQ(probe->descriptor_calls, 0)
      << "observer pass ran for a metadata-only query";
  EXPECT_EQ(probe->frame_calls, 0);
}

// Repeat probes must not re-execute the DAG per call.
TEST(PreviewRendererRepresentation, RepeatProbesDoNotReExecuteTheDag) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(0, source);
  PreviewRenderer renderer(dag);

  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(renderer.get_representation_at_node(NodeID(1)));
  }
  EXPECT_EQ(source->executes, 1)
      << "each probe executed the DAG again instead of reusing the executor's "
         "artifact cache";
}

// The metadata path and the preview path must resolve the same artifact for a
// sink, or a query would describe a different branch than the preview shows.
TEST(PreviewRendererRepresentation, SinkResolvesTheSameOutputAsThePreview) {
  auto source = std::make_shared<TwoOutputSource>();
  auto dag = build_dag(/*connected_output=*/1, source);
  PreviewRenderer renderer(dag);

  auto repr = renderer.get_representation_at_node(NodeID(2));
  ASSERT_TRUE(repr);
  EXPECT_EQ(repr->frame_count(), 2u)
      << "sink resolved upstream output 0 rather than the connected output 1";

  DAGFrameRenderer frame_renderer(dag);
  const auto rendered = frame_renderer.render_frame_at_node(NodeID(2), 0);
  ASSERT_TRUE(rendered.is_valid) << rendered.error_message;
  ASSERT_TRUE(rendered.representation);
  EXPECT_EQ(rendered.representation->frame_count(), repr->frame_count());

  // The render path does run the observer pass, which proves the descriptor
  // counter ProbeRunsNoObserverPass asserts on is a live signal rather than one
  // that never moves.
  const auto* rendered_probe =
      dynamic_cast<const ProbeVfr*>(rendered.representation.get());
  ASSERT_TRUE(rendered_probe);
  EXPECT_GT(rendered_probe->descriptor_calls, 0);
}

}  // namespace
}  // namespace orc
