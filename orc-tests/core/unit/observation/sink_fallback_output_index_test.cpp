/*
 * File:        sink_fallback_output_index_test.cpp
 * Module:      orc-core tests
 * Purpose:     A sink substitutes the upstream output its edge connects to,
 *              not the upstream node's first output
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

namespace orc {
namespace {

// A VFR whose frame count identifies which output it came from: output N
// carries N + 1 frames, so a test can tell the branches apart without
// inspecting samples.
class BranchVfr final : public VideoFrameRepresentation, public Artifact {
 public:
  BranchVfr(const std::string& id, size_t frames)
      : Artifact(ArtifactID(id), Provenance{}), frames_(frames) {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override {
    return {0, static_cast<FrameID>(frames_ - 1)};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(FrameID id) const override { return id < frames_; }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    return nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return {};
  }

 private:
  size_t frames_;
};

// Source with two outputs: output 0 carries one frame, output 1 carries two.
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
    return {std::make_shared<BranchVfr>("branch_0", 1),
            std::make_shared<BranchVfr>("branch_1", 2)};
  }
  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 2; }
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

std::shared_ptr<DAG> build_dag(size_t connected_output) {
  auto dag = std::make_shared<DAG>();
  dag->add_node(
      make_node(NodeID(1), std::make_shared<TwoOutputSource>(), {}, {}));
  dag->add_node(make_node(NodeID(2), std::make_shared<Sink>(), {NodeID(1)},
                          {connected_output}));
  return dag;
}

// A sink produces no output of its own, so the renderer substitutes its
// input's. Which input: the one the edge names. Taking output 0 regardless
// showed (and observed) a different branch than the sink consumes.
TEST(DAGFrameRendererSinkFallback, SubstitutesTheConnectedUpstreamOutput) {
  DAGFrameRenderer renderer(build_dag(/*connected_output=*/1));

  const auto result = renderer.render_frame_at_node(NodeID(2), FrameID(0));
  ASSERT_TRUE(result.is_valid) << result.error_message;
  ASSERT_TRUE(result.representation);
  EXPECT_EQ(result.representation->frame_count(), 2u)
      << "sink fell back to upstream output 0 rather than the connected "
         "output 1";

  // Frame 1 exists only on the connected branch, so rendering it is the same
  // assertion from the caller's side.
  EXPECT_TRUE(renderer.render_frame_at_node(NodeID(2), FrameID(1)).is_valid);
}

TEST(DAGFrameRendererSinkFallback, SubstitutesOutputZeroWhenThatIsConnected) {
  DAGFrameRenderer renderer(build_dag(/*connected_output=*/0));

  const auto result = renderer.render_frame_at_node(NodeID(2), FrameID(0));
  ASSERT_TRUE(result.is_valid) << result.error_message;
  ASSERT_TRUE(result.representation);
  EXPECT_EQ(result.representation->frame_count(), 1u);
}

}  // namespace
}  // namespace orc
