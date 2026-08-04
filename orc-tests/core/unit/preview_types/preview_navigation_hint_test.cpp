/*
 * File:        preview_navigation_hint_test.cpp
 * Module:      orc-core-tests
 * Purpose:     PreviewRenderer forwards the caller's navigation hint to stages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/artifact.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/preview/colour_preview_provider.h>
#include <orc/stage/preview/stage_custom_preview_renderer.h>
#include <orc/stage/stage.h>
#include <orc/stage/video_frame_representation.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "preview_renderer.h"

namespace orc {
namespace {

// Minimal VFR so a source node has something to hand downstream. The preview
// under test never reads it — the stages below render their own images.
class EmptyVfr final : public VideoFrameRepresentation, public Artifact {
 public:
  EmptyVfr() : Artifact(ArtifactID("hint_probe"), Provenance{}) {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override { return {0, 0}; }
  size_t frame_count() const override { return 1; }
  bool has_frame(FrameID id) const override { return id == 0; }
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
};

PreviewImage make_solid_image() {
  PreviewImage image{};
  image.width = 2;
  image.height = 2;
  image.rgb_data.assign(2 * 2 * 3, 64);
  return image;
}

// A stage on the custom-preview path (the SourceAlignStage shape): it renders
// its own images and receives the hint directly.
class CustomPreviewProbeStage final : public DAGStage,
                                      public IStageCustomPreviewRenderer {
 public:
  std::string version() const override { return "1.0.0"; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "custom_preview_probe",
                        "Custom Preview Probe", "", 0, 0, 1, 1,
                        VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    return {std::make_shared<EmptyVfr>()};
  }

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  std::vector<PreviewOption> get_preview_options() const override {
    return {PreviewOption{"probe", "Probe", true, 2, 2, 1, 1.0}};
  }

  PreviewImage render_preview(const std::string& /*option_id*/,
                              uint64_t /*index*/,
                              PreviewNavigationHint hint) const override {
    observed_hint = hint;
    return make_solid_image();
  }

  mutable std::optional<PreviewNavigationHint> observed_hint;
};

// A stage on the colour-carrier path: the hint reaches it through
// IColourPreviewProvider instead.
class ColourPreviewProbeStage final : public DAGStage,
                                      public IStagePreviewCapability,
                                      public IColourPreviewProvider {
 public:
  std::string version() const override { return "1.0.0"; }

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo(NodeType::SOURCE, "colour_preview_probe",
                        "Colour Preview Probe", "", 0, 0, 1, 1,
                        VideoFormatCompatibility::ALL);
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& /*inputs*/,
      const std::map<std::string, ParameterValue>& /*parameters*/,
      ObservationContext& /*ctx*/) override {
    return {std::make_shared<EmptyVfr>()};
  }

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  StagePreviewCapability get_preview_capability() const override {
    StagePreviewCapability capability{};
    capability.supported_data_types = {VideoDataType::ColourNTSC};
    capability.navigation_extent = {1, 1, "frame"};
    capability.geometry = {2, 2, 4.0 / 3.0, 1.0};
    return capability;
  }

  std::optional<ColourFrameCarrier> get_colour_preview_carrier(
      uint64_t frame_index, PreviewNavigationHint hint) const override {
    observed_hint = hint;

    ColourFrameCarrier carrier{};
    carrier.data_type = VideoDataType::ColourNTSC;
    carrier.colorimetry = ColorimetricMetadata::default_ntsc();
    carrier.frame_index = frame_index;
    carrier.width = 2;
    carrier.height = 2;
    carrier.y_plane = {0.25, 0.5, 0.75, 1.0};
    carrier.u_plane = {0.0, 0.0, 0.0, 0.0};
    carrier.v_plane = {0.0, 0.0, 0.0, 0.0};
    carrier.cvbs_white = 65535.0;
    carrier.cvbs_blanking = 0.0;
    return carrier;
  }

  mutable std::optional<PreviewNavigationHint> observed_hint;
};

template <typename StageT>
std::shared_ptr<DAG> build_single_node_dag(std::shared_ptr<StageT> stage) {
  DAGNode node;
  node.node_id = NodeID(1);
  node.stage = std::move(stage);

  auto dag = std::make_shared<DAG>();
  dag->add_node(std::move(node));
  return dag;
}

// Playback renders a run of adjacent frames, and the SDK contract lets a stage
// pre-fetch around the requested one when it is told so. The hint was declared
// end to end but never sent — every render arrived as Random.
TEST(PreviewNavigationHintTest, CustomPreviewStage_ReceivesSequentialHint) {
  auto stage = std::make_shared<CustomPreviewProbeStage>();
  PreviewRenderer renderer(build_single_node_dag(stage));

  const auto result =
      renderer.render_output(NodeID(1), PreviewOutputType::Frame_Field1_First,
                             0, "probe", PreviewNavigationHint::Sequential);

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(stage->observed_hint.has_value());
  EXPECT_EQ(*stage->observed_hint, PreviewNavigationHint::Sequential);
}

TEST(PreviewNavigationHintTest, CustomPreviewStage_ReceivesRandomHint) {
  auto stage = std::make_shared<CustomPreviewProbeStage>();
  PreviewRenderer renderer(build_single_node_dag(stage));

  const auto result =
      renderer.render_output(NodeID(1), PreviewOutputType::Frame_Field1_First,
                             0, "probe", PreviewNavigationHint::Random);

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(stage->observed_hint.has_value());
  EXPECT_EQ(*stage->observed_hint, PreviewNavigationHint::Random);
}

// Scrubbing and one-off navigations must keep the old behaviour, so an
// unspecified hint stays Random rather than inheriting the playback value.
TEST(PreviewNavigationHintTest, DefaultsToRandomWhenTheCallerSaysNothing) {
  auto stage = std::make_shared<CustomPreviewProbeStage>();
  PreviewRenderer renderer(build_single_node_dag(stage));

  const auto result = renderer.render_output(
      NodeID(1), PreviewOutputType::Frame_Field1_First, 0, "probe");

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(stage->observed_hint.has_value());
  EXPECT_EQ(*stage->observed_hint, PreviewNavigationHint::Random);
}

TEST(PreviewNavigationHintTest, ColourCarrierStage_ReceivesSequentialHint) {
  auto stage = std::make_shared<ColourPreviewProbeStage>();
  PreviewRenderer renderer(build_single_node_dag(stage));

  const auto result =
      renderer.render_output(NodeID(1), PreviewOutputType::Frame_Field1_First,
                             0, "", PreviewNavigationHint::Sequential);

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(stage->observed_hint.has_value());
  EXPECT_EQ(*stage->observed_hint, PreviewNavigationHint::Sequential);
}

TEST(PreviewNavigationHintTest, ColourCarrierStage_DefaultsToRandom) {
  auto stage = std::make_shared<ColourPreviewProbeStage>();
  PreviewRenderer renderer(build_single_node_dag(stage));

  const auto result = renderer.render_output(
      NodeID(1), PreviewOutputType::Frame_Field1_First, 0, "");

  ASSERT_TRUE(result.success) << result.error_message;
  ASSERT_TRUE(stage->observed_hint.has_value());
  EXPECT_EQ(*stage->observed_hint, PreviewNavigationHint::Random);
}

}  // namespace
}  // namespace orc
