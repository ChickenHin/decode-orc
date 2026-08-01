/*
 * File:        teletext_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink stage
 *
 * Covers: parameter descriptors and parsing (including the 1-based UI to
 * 0-based field-line conversion), configuration-status transitions, trigger
 * validation and dispatch to the deps seam, and execute() returning no
 * artifacts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "teletext_sink_stage_deps_interface_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Ref;
using testing::Return;
using testing::SaveArg;
using testing::StrictMock;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

class TeletextSinkStage : public ::testing::Test {
 public:
  void SetUp() override {
    pMockDeps_ = std::make_shared<StrictMock<MockTeletextSinkStageDeps>>();
    pMockRepresentation_ =
        std::make_shared<StrictMock<MockVideoFrameRepresentationArtifact>>();

    instance_ = std::make_unique<orc::TeletextSinkStage>(
        static_cast<orc::IStageServices*>(nullptr));
  }

  void TearDown() override { instance_.reset(); }

 protected:
  std::vector<orc::ArtifactPtr> make_valid_input() const {
    return {std::static_pointer_cast<orc::Artifact>(pMockRepresentation_)};
  }

  // Expect a dispatch to the deps seam and capture the options it receives.
  void expect_export(const orc::TeletextSinkResult& result,
                     orc::TeletextSinkOptions& captured_options) {
    instance_->set_deps_override(pMockDeps_);
    EXPECT_CALL(*pMockDeps_, init(_, _)).Times(1);
    EXPECT_CALL(*pMockDeps_, export_t42(pMockRepresentation_.get(),
                                        Ref(mockObservationContext_), _))
        .Times(1)
        .WillOnce(
            testing::DoAll(SaveArg<2>(&captured_options), Return(result)));
  }

  std::shared_ptr<StrictMock<MockTeletextSinkStageDeps>> pMockDeps_;
  std::shared_ptr<StrictMock<MockVideoFrameRepresentationArtifact>>
      pMockRepresentation_;
  MockObservationContext mockObservationContext_;

  std::unique_ptr<orc::TeletextSinkStage> instance_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkStage, NodeTypeInfo_MatchesDesign) {
  const auto info = instance_->get_node_type_info();

  EXPECT_EQ(info.type, orc::NodeType::SINK);
  EXPECT_EQ(info.stage_name, "teletext_sink");
  EXPECT_EQ(info.display_name, "Teletext Sink");
  EXPECT_EQ(info.description,
            "Extracts teletext from the VBI and exports a T42 packet stream");
  EXPECT_EQ(info.min_inputs, 1u);
  EXPECT_EQ(info.max_inputs, 1u);
  EXPECT_EQ(info.min_outputs, 0u);
  EXPECT_EQ(info.max_outputs, 0u);
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::PAL_ONLY);
}

TEST_F(TeletextSinkStage, ParameterDescriptors_MatchSpecTable) {
  const auto descriptors = instance_->get_parameter_descriptors();
  ASSERT_EQ(descriptors.size(), 10u);

  EXPECT_EQ(descriptors[0].name, "output_path");
  EXPECT_EQ(descriptors[0].type, orc::ParameterType::FILE_PATH);
  EXPECT_TRUE(descriptors[0].constraints.required);
  EXPECT_EQ(descriptors[0].file_extension_hint, ".t42");

  EXPECT_EQ(descriptors[1].name, "first_vbi_line");
  EXPECT_EQ(descriptors[1].type, orc::ParameterType::INT32);
  ASSERT_TRUE(descriptors[1].constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*descriptors[1].constraints.default_value), 6);

  EXPECT_EQ(descriptors[2].name, "last_vbi_line");
  EXPECT_EQ(descriptors[2].type, orc::ParameterType::INT32);
  ASSERT_TRUE(descriptors[2].constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*descriptors[2].constraints.default_value), 22);

  EXPECT_EQ(descriptors[3].name, "keep_empty_packets");
  EXPECT_EQ(descriptors[3].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[3].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[3].constraints.default_value), false);

  EXPECT_EQ(descriptors[4].name, "tolerant_framing");
  EXPECT_EQ(descriptors[4].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[4].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[4].constraints.default_value), false);

  EXPECT_EQ(descriptors[5].name, "require_valid_mrag");
  EXPECT_EQ(descriptors[5].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[5].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[5].constraints.default_value), true);

  EXPECT_EQ(descriptors[6].name, "squash_repeated_rows");
  EXPECT_EQ(descriptors[6].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[6].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[6].constraints.default_value), true);

  EXPECT_EQ(descriptors[7].name, "export_subtitles");
  EXPECT_EQ(descriptors[7].type, orc::ParameterType::BOOL);
  ASSERT_TRUE(descriptors[7].constraints.default_value.has_value());
  EXPECT_EQ(std::get<bool>(*descriptors[7].constraints.default_value), false);

  EXPECT_EQ(descriptors[8].name, "subtitle_page");
  EXPECT_EQ(descriptors[8].type, orc::ParameterType::STRING);
  ASSERT_TRUE(descriptors[8].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[8].constraints.default_value),
            "888");
  ASSERT_TRUE(descriptors[8].constraints.depends_on.has_value());
  EXPECT_EQ(descriptors[8].constraints.depends_on->parameter_name,
            "export_subtitles");

  EXPECT_EQ(descriptors[9].name, "subtitle_format");
  EXPECT_EQ(descriptors[9].type, orc::ParameterType::STRING);
  ASSERT_EQ(descriptors[9].constraints.allowed_strings.size(), 1u);
  EXPECT_EQ(descriptors[9].constraints.allowed_strings[0], "SRT");
  ASSERT_TRUE(descriptors[9].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descriptors[9].constraints.default_value),
            "SRT");
  ASSERT_TRUE(descriptors[9].constraints.depends_on.has_value());
  EXPECT_EQ(descriptors[9].constraints.depends_on->parameter_name,
            "export_subtitles");
}

TEST_F(TeletextSinkStage, ConfigurationStatus_RedUntilOutputPathSet) {
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Red);

  EXPECT_TRUE(
      instance_->set_parameters({{"output_path", std::string("out.t42")}}));
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Green);

  EXPECT_TRUE(instance_->set_parameters({{"output_path", std::string("")}}));
  EXPECT_EQ(instance_->get_configuration_status(),
            orc::ConfigurationStatus::Red);
}

TEST_F(TeletextSinkStage, SetParameters_RoundTripsValues) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out.t42")},
      {"first_vbi_line", int32_t{7}},
      {"keep_empty_packets", true}};

  EXPECT_TRUE(instance_->set_parameters(params));
  EXPECT_EQ(instance_->get_parameters(), params);
}

TEST_F(TeletextSinkStage, Execute_ReturnsNoArtifacts) {
  orc::ObservationContext context;
  EXPECT_TRUE(instance_->execute(make_valid_input(), {}, context).empty());
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenNoInputConnected) {
  const bool result = instance_->trigger(
      {}, {{"output_path", std::string("out")}}, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(), "Error: No input connected");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenInputNotRepresentation) {
  const std::vector<orc::ArtifactPtr> inputs = {nullptr};

  const bool result = instance_->trigger(
      inputs, {{"output_path", std::string("out")}}, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Input is not a video frame representation");
}

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenOutputPathMissing) {
  const bool result =
      instance_->trigger(make_valid_input(), {}, mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(), "Error: No output path specified");
}

TEST_F(TeletextSinkStage, Trigger_ReturnsFalseWhenOutputPathEmpty) {
  const bool result =
      instance_->trigger(make_valid_input(), {{"output_path", std::string("")}},
                         mockObservationContext_);

  EXPECT_FALSE(result);
  EXPECT_EQ(instance_->get_trigger_status(), "Error: Output path is empty");
}

TEST_F(TeletextSinkStage, Trigger_RejectsInvalidLineWindows) {
  const std::vector<std::map<std::string, orc::ParameterValue>> bad_params = {
      // first > last
      {{"output_path", std::string("out")},
       {"first_vbi_line", int32_t{10}},
       {"last_vbi_line", int32_t{5}}},
      // below the 1-based window
      {{"output_path", std::string("out")}, {"first_vbi_line", int32_t{0}}},
      // above the EN 300 706 §4.1 window (1-based field line 22)
      {{"output_path", std::string("out")}, {"last_vbi_line", int32_t{23}}},
  };

  for (const auto& params : bad_params) {
    const bool result =
        instance_->trigger(make_valid_input(), params, mockObservationContext_);
    EXPECT_FALSE(result);
    EXPECT_EQ(instance_->get_trigger_status(),
              "Error: Invalid VBI line window");
  }
}

TEST_F(TeletextSinkStage, Trigger_ConvertsUiLinesToZeroBasedFieldLines) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"first_vbi_line", int32_t{7}},
      {"last_vbi_line", int32_t{20}},      {"keep_empty_packets", true},
      {"tolerant_framing", true},          {"require_valid_mrag", false}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_EQ(captured.output_path, "out");
  EXPECT_EQ(captured.first_field_line, 6);  // 1-based 7 → 0-based 6
  EXPECT_EQ(captured.last_field_line, 19);  // 1-based 20 → 0-based 19
  EXPECT_TRUE(captured.keep_empty_packets);
  EXPECT_TRUE(captured.tolerant_framing);
  EXPECT_FALSE(captured.require_valid_mrag);
}

TEST_F(TeletextSinkStage, Trigger_UsesSpecDefaultsWhenParametersAbsent) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  // EN 300 706 §4.1 window: 1-based 6-22 → 0-based field lines 5-21.
  EXPECT_EQ(captured.first_field_line, 5);
  EXPECT_EQ(captured.last_field_line, 21);
  EXPECT_FALSE(captured.keep_empty_packets);
  EXPECT_FALSE(captured.tolerant_framing);
  EXPECT_TRUE(captured.require_valid_mrag);
}

TEST_F(TeletextSinkStage, Trigger_ReportsCountsFromDepsResult) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Exported 84 teletext packets (2 fields with data) to out.t42");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

TEST_F(TeletextSinkStage, Trigger_PassesSubtitleOptionsToDeps) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_page", std::string("150")},
      {"subtitle_format", std::string("SRT")}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_TRUE(captured.export_subtitles);
  EXPECT_EQ(captured.subtitle_page, "150");
}

TEST_F(TeletextSinkStage, Trigger_SubtitleExportDisabledByDefault) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_TRUE(instance_->trigger(make_valid_input(),
                                 {{"output_path", std::string("out")}},
                                 mockObservationContext_));

  EXPECT_FALSE(captured.export_subtitles);
}

TEST_F(TeletextSinkStage, Trigger_RejectsMalformedSubtitlePage) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_page", std::string("98X")}};

  EXPECT_FALSE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Invalid subtitle page \"98X\" (expected magazine digit "
            "1-8 plus two hex digits, e.g. 888)");
}

TEST_F(TeletextSinkStage, Trigger_RejectsUnsupportedSubtitleFormat) {
  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")},
      {"export_subtitles", true},
      {"subtitle_format", std::string("VTT")}};

  EXPECT_FALSE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));
  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Unsupported subtitle format: VTT");
}

TEST_F(TeletextSinkStage, Trigger_ReportsSubtitleCountsFromDepsResult) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = true;
  deps_result.output_path = "out.t42";
  deps_result.packets_written = 84;
  deps_result.fields_with_data = 2;
  deps_result.subtitle_path = "out.srt";
  deps_result.subtitle_cues_written = 3;
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  const std::map<std::string, orc::ParameterValue> params = {
      {"output_path", std::string("out")}, {"export_subtitles", true}};

  EXPECT_TRUE(
      instance_->trigger(make_valid_input(), params, mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Exported 84 teletext packets (2 fields with data) to out.t42; "
            "3 subtitle cues to out.srt");
}

TEST_F(TeletextSinkStage, Trigger_ReportsDepsFailure) {
  orc::TeletextSinkResult deps_result;
  deps_result.success = false;
  deps_result.message = "Input is not PAL (teletext sink is PAL WST only)";
  orc::TeletextSinkOptions captured;
  expect_export(deps_result, captured);

  EXPECT_FALSE(instance_->trigger(make_valid_input(),
                                  {{"output_path", std::string("out")}},
                                  mockObservationContext_));

  EXPECT_EQ(instance_->get_trigger_status(),
            "Error: Input is not PAL (teletext sink is PAL WST only)");
  EXPECT_FALSE(instance_->is_trigger_in_progress());
}

}  // namespace orc_unit_test
