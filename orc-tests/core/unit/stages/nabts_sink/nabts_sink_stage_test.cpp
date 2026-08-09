/*
 * File:        nabts_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NABTS sink stage
 *
 * Covers: parameter descriptors and project-format filtering, parsing
 * (including the 1-based UI to 0-based field-line conversion),
 * configuration-status transitions, trigger validation and dispatch to the
 * deps seam, and execute() returning no artifacts.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_sink_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "nabts_sink_deps_interface_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Return;
using testing::SaveArg;

namespace orc_unit_test {
namespace {

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  for (const auto& descriptor : descriptors) {
    if (descriptor.name == name) {
      return &descriptor;
    }
  }
  return nullptr;
}

std::map<std::string, orc::ParameterValue> default_parameters() {
  return {
      {"output_path", std::string("/tmp/nabts-test")},
      {"first_vbi_line", int32_t{10}},
      {"last_vbi_line", int32_t{21}},
      {"keep_empty_packets", false},
      {"detector", std::string("Automatic")},
      {"tolerant_framing", false},
      {"require_valid_prefix", true},
      {"pin_data_phase", true},
      {"learn_active_lines", true},
      {"decode_threads", int32_t{0}},
      {"write_report", false},
  };
}

class NabtsSinkStage : public ::testing::Test {
 protected:
  orc::NabtsSinkStage stage_{nullptr};
};

// ---------------------------------------------------------------------------
// Node type and parameters
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, NodeType_IsASinkWithOneInputAndNoOutputs) {
  const auto info = stage_.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::SINK);
  EXPECT_EQ(info.stage_name, "nabts_sink");
  EXPECT_EQ(stage_.required_input_count(), 1u);
  EXPECT_EQ(stage_.output_count(), 0u);
}

// CEA-516 §1.1.1 specifies NABTS on the 525-line NTSC signal, so a 625-line
// project has nothing this stage can recover and is offered nothing to
// configure rather than a parameter set that cannot be used.
TEST_F(NabtsSinkStage, Descriptors_AreWithheldFromA625LineProject) {
  EXPECT_TRUE(stage_
                  .get_parameter_descriptors(orc::VideoSystem::PAL,
                                             orc::SourceType::Unknown)
                  .empty());

  for (const auto system : {orc::VideoSystem::NTSC, orc::VideoSystem::PAL_M,
                            orc::VideoSystem::Unknown}) {
    EXPECT_FALSE(
        stage_.get_parameter_descriptors(system, orc::SourceType::Unknown)
            .empty())
        << "video system " << static_cast<int>(system);
  }
}

TEST_F(NabtsSinkStage, Descriptors_DefaultToTheBt653LineWindow) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);

  const auto* first = find_descriptor(descriptors, "first_vbi_line");
  const auto* last = find_descriptor(descriptors, "last_vbi_line");
  ASSERT_NE(first, nullptr);
  ASSERT_NE(last, nullptr);

  // ITU-R BT.653 §2 broadcast lines 10-21, presented 1-based.
  ASSERT_TRUE(first->constraints.default_value.has_value());
  ASSERT_TRUE(last->constraints.default_value.has_value());
  EXPECT_EQ(std::get<int32_t>(*first->constraints.default_value), 10);
  EXPECT_EQ(std::get<int32_t>(*last->constraints.default_value), 21);
}

TEST_F(NabtsSinkStage, Descriptors_NameTheT33Stream) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);
  const auto* path = find_descriptor(descriptors, "output_path");
  ASSERT_NE(path, nullptr);
  EXPECT_EQ(path->file_extension_hint, ".t33");
  EXPECT_FALSE(path->constraints.required);
}

// CEA-516 §3.3 makes byte parity conditional on the data group type, so there
// is no parity-repair option to offer — nor a row squasher, which is a Level 1
// teletext idea with no System C equivalent.
TEST_F(NabtsSinkStage, Descriptors_OfferNoLevel1TeletextOptions) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Unknown);
  for (const char* absent : {"repair_damaged_bytes", "squash_repeated_rows",
                             "export_subtitles", "subtitle_page"}) {
    EXPECT_EQ(find_descriptor(descriptors, absent), nullptr) << absent;
  }
}

// ---------------------------------------------------------------------------
// Configuration status
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Status_IsYellowWithoutAnOutputPathAndGreenWithOne) {
  EXPECT_EQ(stage_.get_configuration_status(),
            orc::ConfigurationStatus::Yellow);

  stage_.set_parameters({{"output_path", std::string("/tmp/out.t33")}});
  EXPECT_EQ(stage_.get_configuration_status(), orc::ConfigurationStatus::Green);

  // An empty path is the report-only run, which is supported, not a fault.
  stage_.set_parameters({{"output_path", std::string("")}});
  EXPECT_EQ(stage_.get_configuration_status(),
            orc::ConfigurationStatus::Yellow);
}

// ---------------------------------------------------------------------------
// Trigger
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Trigger_FailsWithNoInput) {
  MockObservationContext observations;
  EXPECT_FALSE(stage_.trigger({}, default_parameters(), observations));
  EXPECT_NE(stage_.get_trigger_status().find("No input"), std::string::npos);
  EXPECT_FALSE(stage_.is_trigger_in_progress());
}

TEST_F(NabtsSinkStage, Trigger_ConvertsTheUiLineWindowToFieldLines) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkOptions captured;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _))
      .WillOnce(testing::DoAll(SaveArg<1>(&captured),
                               Return(orc::NabtsSinkResult{})));
  stage_.set_deps_override(deps);

  auto parameters = default_parameters();
  parameters["first_vbi_line"] = int32_t{10};
  parameters["last_vbi_line"] = int32_t{21};

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  stage_.trigger({input}, parameters, observations);

  // 1-based UI lines, 0-based field lines: the ITU-R BT.653 §2 window.
  EXPECT_EQ(captured.first_field_line, 9);
  EXPECT_EQ(captured.last_field_line, 20);
}

TEST_F(NabtsSinkStage, Trigger_RejectsAnInvertedLineWindow) {
  auto parameters = default_parameters();
  parameters["first_vbi_line"] = int32_t{18};
  parameters["last_vbi_line"] = int32_t{12};

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("Invalid VBI line window"),
            std::string::npos);
}

// The report is named after the packet stream and written beside it, so it has
// nowhere to go without one. Refused rather than dropped.
TEST_F(NabtsSinkStage, Trigger_RefusesAReportWithNoOutputFile) {
  auto parameters = default_parameters();
  parameters["output_path"] = std::string("");
  parameters["write_report"] = true;

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("needs an output file"),
            std::string::npos);
}

// The caption document is named after the packet stream too, so it is refused
// on the same grounds — an export silently not happening is the worse outcome.
TEST_F(NabtsSinkStage, Trigger_RefusesCaptionExportWithNoOutputFile) {
  auto parameters = default_parameters();
  parameters["output_path"] = std::string("");
  parameters["export_captions"] = true;

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, parameters, observations));
  EXPECT_NE(stage_.get_trigger_status().find("needs an output file"),
            std::string::npos);
}

// Every parameter the stage advertises has to be readable back, or the GUI
// dialogue would drop it on save.
TEST_F(NabtsSinkStage, ExportCaptionsIsAdvertisedAsAParameter) {
  const auto descriptors = stage_.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  const auto found = std::find_if(descriptors.begin(), descriptors.end(),
                                  [](const orc::ParameterDescriptor& d) {
                                    return d.name == "export_captions";
                                  });
  ASSERT_NE(found, descriptors.end());
  EXPECT_EQ(found->type, orc::ParameterType::BOOL);
  ASSERT_TRUE(found->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<bool>(*found->constraints.default_value));
  EXPECT_FALSE(std::get<bool>(*found->constraints.default_value));
}

TEST_F(NabtsSinkStage, Trigger_ReportsTheDepsResult) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = true;
  result.packets_written = 1234;
  result.fields_with_data = 567;
  result.output_path = "/tmp/out.t33";
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(stage_.trigger({input}, default_parameters(), observations));

  const std::string status = stage_.get_trigger_status();
  EXPECT_NE(status.find("1234"), std::string::npos);
  EXPECT_NE(status.find("567"), std::string::npos);
  EXPECT_NE(status.find("/tmp/out.t33"), std::string::npos);
}

TEST_F(NabtsSinkStage, Trigger_ReportsAFailedRun) {
  auto deps = std::make_shared<orc::tests::MockNabtsSinkStageDeps>();
  orc::NabtsSinkResult result;
  result.success = false;
  result.message = "Input carries no NABTS service";
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, analyse(_, _)).WillOnce(Return(result));
  stage_.set_deps_override(deps);

  MockObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_FALSE(stage_.trigger({input}, default_parameters(), observations));
  EXPECT_NE(stage_.get_trigger_status().find("no NABTS service"),
            std::string::npos);
}

// ---------------------------------------------------------------------------
// execute()
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkStage, Execute_ProducesNoArtifacts) {
  orc::ObservationContext observations;
  auto input = std::make_shared<MockVideoFrameRepresentationArtifact>();
  EXPECT_TRUE(
      stage_.execute({input}, default_parameters(), observations).empty());
}

}  // namespace
}  // namespace orc_unit_test
