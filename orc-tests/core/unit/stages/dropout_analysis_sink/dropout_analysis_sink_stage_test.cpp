/*
 * File:        dropout_analysis_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for DropoutAnalysisSinkStage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "../../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_deps_interface.h"
#include "../../../../orc/plugins/stages/dropout_map/dropout_map_stage.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../mocks/mock_video_frame_representation.h"

namespace orc_unit_test {
using testing::_;
using testing::NiceMock;
using testing::NotNull;
using testing::Return;
using testing::StrictMock;

// Mock for IDropoutAnalysisSinkStageDeps using VFrameR pointer.
class MockDropoutAnalysisSinkStageDeps
    : public orc::IDropoutAnalysisSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));

  MOCK_METHOD(orc::DropoutAnalysisComputeResult, compute_and_analyze,
              (orc::VideoFrameRepresentation * representation,
               orc::IObservationContext& observation_context,
               orc::DropoutAnalysisComputeOptions options),
              (override));

  MOCK_METHOD(bool, write_csv,
              (const std::string& path,
               const std::vector<orc::FrameDropoutStats>& frame_stats),
              (override));
};

TEST(DropoutAnalysisSinkStageTest,
     Descriptor_DefaultsIncludeExpectedOutputAndWriteCsv) {
  orc::DropoutAnalysisSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  auto output_it = std::find_if(descriptors.begin(), descriptors.end(),
                                [](const orc::ParameterDescriptor& d) {
                                  return d.name == "output_path";
                                });
  auto write_csv_it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [](const orc::ParameterDescriptor& d) { return d.name == "write_csv"; });

  ASSERT_NE(output_it, descriptors.end());
  EXPECT_EQ(output_it->type, orc::ParameterType::FILE_PATH);
  ASSERT_TRUE(output_it->constraints.default_value.has_value());
  ASSERT_TRUE(write_csv_it->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*output_it->constraints.default_value), "");

  ASSERT_NE(write_csv_it, descriptors.end());
  EXPECT_EQ(write_csv_it->type, orc::ParameterType::BOOL);
  EXPECT_FALSE(std::get<bool>(*write_csv_it->constraints.default_value));
}

TEST(DropoutAnalysisSinkStageTest, Trigger_FailsWhenNoInputProvided) {
  orc::DropoutAnalysisSinkStage stage;
  MockObservationContext ctx;

  const bool result = stage.trigger({}, {}, ctx);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Error: No input connected");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(DropoutAnalysisSinkStageTest, Trigger_FailsWhenInputIsWrongType) {
  struct FakeArt : public orc::Artifact {
    FakeArt() : Artifact(orc::ArtifactID("x"), orc::Provenance{}) {}
    std::string type_name() const override { return "x"; }
  };
  orc::DropoutAnalysisSinkStage stage;
  MockObservationContext ctx;
  const bool result = stage.trigger({std::make_shared<FakeArt>()}, {}, ctx);
  EXPECT_FALSE(result);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(DropoutAnalysisSinkStageTest, Trigger_UsesDepsSeamAndReportsSuccess) {
  orc::DropoutAnalysisSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockDropoutAnalysisSinkStageDeps>>();
  stage.set_deps_override(deps);

  orc::ObservationContext observation_context;
  // Use the artifact-compatible mock so it can be passed as ArtifactPtr.
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  std::vector<orc::FrameDropoutStats> expected_stats;
  orc::FrameDropoutStats stat{};
  stat.frame_number = 12;
  stat.dropout_length_samples = 128;
  stat.dropout_count = 3;
  stat.has_data = true;
  expected_stats.push_back(stat);

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, compute_and_analyze(NotNull(), _, _))
      .WillOnce(Return(orc::DropoutAnalysisComputeResult{
          true, "Dropout analysis complete", expected_stats, 240}));
  EXPECT_CALL(*deps, write_csv(_, _)).Times(0);

  const bool result = stage.trigger({vfr}, {}, observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Dropout analysis complete");
  EXPECT_TRUE(stage.has_results());
  ASSERT_EQ(stage.frame_stats().size(), 1u);
  EXPECT_EQ(stage.frame_stats()[0].frame_number, 12);
  EXPECT_EQ(stage.frame_stats()[0].dropout_length_samples, 128);
  EXPECT_EQ(stage.frame_stats()[0].dropout_count, 3);
  EXPECT_EQ(stage.total_frames(), 240);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(DropoutAnalysisSinkStageTest, Trigger_UsesDepsSeamAndPropagatesFailure) {
  orc::DropoutAnalysisSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockDropoutAnalysisSinkStageDeps>>();
  stage.set_deps_override(deps);

  orc::ObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, compute_and_analyze(NotNull(), _, _))
      .WillOnce(Return(
          orc::DropoutAnalysisComputeResult{false, "observer failed", {}, 0}));
  EXPECT_CALL(*deps, write_csv(_, _)).Times(0);

  const bool result = stage.trigger({vfr}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Error: observer failed");
  EXPECT_FALSE(stage.has_results());
  EXPECT_TRUE(stage.frame_stats().empty());
  EXPECT_EQ(stage.total_frames(), 0);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(DropoutAnalysisSinkStageTest, Trigger_WritesCSVWhenDepsSucceeds) {
  orc::DropoutAnalysisSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockDropoutAnalysisSinkStageDeps>>();
  stage.set_deps_override(deps);

  orc::ObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  std::vector<orc::FrameDropoutStats> expected_stats;
  orc::FrameDropoutStats stat{};
  stat.frame_number = 4;
  stat.dropout_length_samples = 64;
  stat.dropout_count = 2;
  stat.has_data = true;
  expected_stats.push_back(stat);

  EXPECT_CALL(*deps, init(_, _));
  EXPECT_CALL(*deps, compute_and_analyze(NotNull(), _, _))
      .WillOnce(Return(orc::DropoutAnalysisComputeResult{
          true, "Dropout analysis complete", expected_stats, 8}));
  EXPECT_CALL(*deps, write_csv("out.csv", _))
      .WillOnce(testing::Invoke(
          [](const std::string& path,
             const std::vector<orc::FrameDropoutStats>& frame_stats) {
            EXPECT_EQ(path, "out.csv");
            EXPECT_EQ(frame_stats.size(), 1u);
            EXPECT_EQ(frame_stats[0].frame_number, 4);
            EXPECT_EQ(frame_stats[0].dropout_length_samples, 64);
            EXPECT_EQ(frame_stats[0].dropout_count, 2);
            return true;
          }));

  const bool result = stage.trigger(
      {vfr}, {{"write_csv", true}, {"output_path", std::string("out.csv")}},
      observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Dropout analysis complete");
  EXPECT_TRUE(stage.has_results());
  EXPECT_EQ(stage.total_frames(), 8);
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

// Freshness after a dropout_map edit (issue #216, plan Phase 3 Task 3.3): the
// sink re-analyses its input on every trigger, so a "trigger → edit map →
// trigger" sequence must yield updated stats and never leak pre-edit data.
// The map edit is modelled by swapping the input
// DropoutMappedFrameRepresentation (which is what the DAG rebuilds when the
// dropout_map parameter changes).
TEST(DropoutAnalysisSinkStageTest, Trigger_ReflectsMapEditOnRetrigger) {
  // NTSC source with one sidecar dropout run: line 10, samples 100-200 →
  // flat start 9200, count 101.
  auto source = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 0u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(1u));
  orc::FrameDescriptor desc;
  desc.frame_id = 0;
  desc.system = orc::VideoSystem::NTSC;
  desc.height = 525;
  desc.samples_total = 525u * 910u;
  desc.samples_per_line_nominal = 910;
  ON_CALL(*source, get_frame_descriptor(orc::FrameID{0}))
      .WillByDefault(Return(desc));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  params.frame_width_nominal = 910;
  params.frame_height = 525;
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));
  ON_CALL(*source, get_dropout_hints(orc::FrameID{0}))
      .WillByDefault(
          Return(std::vector<orc::DropoutRun>{{0u, 9200u, 101u, 128}}));

  orc::DropoutAnalysisSinkStage stage;  // real deps (no override)
  orc::ObservationContext ctx;

  // First trigger: empty map — the source run is reported.
  std::map<uint64_t, orc::FrameDropoutMapEntry> empty_map;
  auto rep1 = std::make_shared<orc::DropoutMappedFrameRepresentation>(
      source, empty_map);
  ASSERT_TRUE(stage.trigger({rep1}, {}, ctx));
  ASSERT_EQ(stage.frame_stats().size(), 1u);
  EXPECT_EQ(stage.frame_stats()[0].dropout_count, 1);
  EXPECT_EQ(stage.frame_stats()[0].dropout_length_samples, 101);

  // Edit the map to remove that run, then re-trigger with the rebuilt input.
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.removals.push_back({10u, 100u, 200u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> removal_map{{0, entry}};
  auto rep2 = std::make_shared<orc::DropoutMappedFrameRepresentation>(
      source, removal_map);
  ASSERT_TRUE(stage.trigger({rep2}, {}, ctx));
  ASSERT_EQ(stage.frame_stats().size(), 1u);
  // Fresh result — the removed run must not survive from the first trigger.
  EXPECT_EQ(stage.frame_stats()[0].dropout_count, 0);
  EXPECT_EQ(stage.frame_stats()[0].dropout_length_samples, 0);
}

}  // namespace orc_unit_test
