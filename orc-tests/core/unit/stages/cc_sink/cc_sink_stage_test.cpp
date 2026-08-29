/*
 * File:        cc_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CCSinkStage parameter contracts and trigger
 * dependency seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/cc_sink/cc_sink_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/cc_sink/cc_sink_stage_deps_interface.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

class MockCCSinkStageDeps : public orc::ICCSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));

  MOCK_METHOD(orc::CCExportResult, export_cc,
              (orc::VideoFrameRepresentation * representation,
               orc::IObservationContext& observation_context,
               orc::CCExportOptions options),
              (override));
};

namespace {
const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&name](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descriptors.end() ? nullptr : &*it;
}
}  // namespace

TEST(CCSinkStageTest, Descriptor_DefaultsIncludeExportFormatOptions) {
  orc::CCSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* output = find_descriptor(descriptors, "output_path");
  const auto* format = find_descriptor(descriptors, "format");

  ASSERT_NE(output, nullptr);
  EXPECT_EQ(output->type, orc::ParameterType::FILE_PATH);
  if (!output->constraints.default_value.has_value()) {
    FAIL() << "Expected output default_value to have a value";
    return;
  }
  EXPECT_EQ(std::get<std::string>(*output->constraints.default_value), "");

  ASSERT_NE(format, nullptr);
  EXPECT_EQ(format->type, orc::ParameterType::STRING);
  EXPECT_THAT(format->constraints.allowed_strings,
              testing::ElementsAre("Scenarist SCC", "SubRip SRT", "Plain Text",
                                   "HTML"));
  if (!format->constraints.default_value.has_value()) {
    FAIL() << "Expected format default_value to have a value";
    return;
  }
  EXPECT_EQ(std::get<std::string>(*format->constraints.default_value),
            "Scenarist SCC");
}

TEST(CCSinkStageTest, Descriptor_OffersTheFourFieldOneServices) {
  orc::CCSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto* service = find_descriptor(descriptors, "service");

  ASSERT_NE(service, nullptr);
  EXPECT_EQ(service->type, orc::ParameterType::STRING);
  EXPECT_TRUE(service->constraints.required);
  // CC3/CC4/TEXT3/TEXT4 ride on the second field, which the host's
  // closed_caption observer does not decode, so they are not offered.
  EXPECT_THAT(service->constraints.allowed_strings,
              testing::ElementsAre("CC1", "CC2", "TEXT1", "TEXT2"));
  ASSERT_TRUE(service->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*service->constraints.default_value), "CC1");
}

TEST(CCSinkStageTest, TriggerStatus_IsIdleWhenNotProcessing) {
  orc::CCSinkStage stage;
  EXPECT_EQ(stage.get_trigger_status(), "Idle");
}

TEST(CCSinkStageTest, Trigger_FailsWhenNoInputProvided) {
  orc::CCSinkStage stage;
  MockObservationContext observation_context;

  const bool result = stage.trigger({}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
  EXPECT_EQ(stage.get_trigger_status(), "Idle");
}

TEST(CCSinkStageTest, Trigger_FailsWhenInputIsNotVideoFrameRepresentation) {
  orc::CCSinkStage stage;
  MockObservationContext observation_context;

  const bool result =
      stage.trigger({nullptr}, {{"output_path", std::string("out.scc")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
  EXPECT_EQ(stage.get_trigger_status(), "Idle");
}

TEST(CCSinkStageTest, Trigger_UsesDepsSeamAndReportsSuccess) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  orc::CCExportOptions captured_options;

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, export_cc(vfr.get(), _, _))
      .WillOnce(testing::DoAll(testing::SaveArg<2>(&captured_options),
                               Return(orc::CCExportResult{true, "ok", 42})));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("captions.scc")},
                     {"format", std::string("Scenarist SCC")}},
                    observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(captured_options.output_path, "captions.scc");
  EXPECT_EQ(captured_options.export_format, orc::CCExportFormat::SCC);
  EXPECT_EQ(captured_options.service, orc::EIA608Service::CC1);
  EXPECT_FALSE(stage.is_trigger_in_progress());
  EXPECT_EQ(stage.get_trigger_status(), "Idle");
}

TEST(CCSinkStageTest, Trigger_UsesDepsSeamAndPropagatesFailure) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, export_cc(vfr.get(), _, _))
      .WillOnce(Return(orc::CCExportResult{false, "export failed", 0}));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("captions.scc")},
                     {"format", std::string("Scenarist SCC")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
  EXPECT_EQ(stage.get_trigger_status(), "Idle");
}

TEST(CCSinkStageTest, Trigger_UsesDepsSeamWithSCCFormat) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps,
              export_cc(vfr.get(), _,
                        testing::Field(&orc::CCExportOptions::export_format,
                                       orc::CCExportFormat::SCC)))
      .WillOnce(Return(orc::CCExportResult{true, "ok", 1}));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("captions.scc")},
                     {"format", std::string("Scenarist SCC")}},
                    observation_context);

  EXPECT_TRUE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CCSinkStageTest, Trigger_UsesDepsSeamWithPlainTextFormat) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps,
              export_cc(vfr.get(), _,
                        testing::Field(&orc::CCExportOptions::export_format,
                                       orc::CCExportFormat::PLAIN_TEXT)))
      .WillOnce(Return(orc::CCExportResult{true, "ok", 1}));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("captions.txt")},
                     {"format", std::string("Plain Text")}},
                    observation_context);

  EXPECT_TRUE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}
TEST(CCSinkStageTest, Trigger_PassesTheSelectedServiceThrough) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  orc::CCExportOptions captured_options;
  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, export_cc(vfr.get(), _, _))
      .WillOnce(testing::DoAll(testing::SaveArg<2>(&captured_options),
                               Return(orc::CCExportResult{true, "ok", 1})));

  const bool result = stage.trigger({vfr},
                                    {{"output_path", std::string("text.html")},
                                     {"format", std::string("HTML")},
                                     {"service", std::string("TEXT2")}},
                                    observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(captured_options.export_format, orc::CCExportFormat::HTML);
  EXPECT_EQ(captured_options.service, orc::EIA608Service::T2);
}

TEST(CCSinkStageTest, Trigger_FailsOnAServiceCarriedOnTheUndecodedField) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  // Parsing throws before the deps seam is reached, so a StrictMock with no
  // expectations is the assertion: export_cc must not be called.
  const bool result = stage.trigger({vfr},
                                    {{"output_path", std::string("out.srt")},
                                     {"format", std::string("SubRip SRT")},
                                     {"service", std::string("CC3")}},
                                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CCSinkStageTest, Trigger_FailsOnAnUnknownService) {
  orc::CCSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCCSinkStageDeps>>();
  stage.set_deps_override(deps);

  NiceMock<MockObservationContext> observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  const bool result = stage.trigger({vfr},
                                    {{"output_path", std::string("out.scc")},
                                     {"service", std::string("CC9")}},
                                    observation_context);

  EXPECT_FALSE(result);
}
}  // namespace orc_unit_test
