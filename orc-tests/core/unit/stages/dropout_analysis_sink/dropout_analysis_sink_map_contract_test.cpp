/*
 * File:        dropout_analysis_sink_map_contract_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Contract tests proving the dropout analysis sink reports the
 *              dropout hints visible at its input, including dropout_map
 *              additions and removals (issue #216, plan Phase 3 Task 3.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <map>
#include <memory>
#include <vector>

#include "../../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_deps.h"
#include "../../../../orc/plugins/stages/dropout_map/dropout_map_stage.h"
#include "../../mocks/mock_video_frame_representation.h"

using ::testing::NiceMock;
using ::testing::Return;

namespace orc_unit_test {
namespace {

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = 910;
  p.frame_height = 525;
  p.blanking_level = 240;
  p.white_level = 800;
  return p;
}

orc::FrameDescriptor make_ntsc_descriptor(orc::FrameID id) {
  orc::FrameDescriptor desc;
  desc.frame_id = id;
  desc.system = orc::VideoSystem::NTSC;
  desc.height = 525;
  desc.samples_total = 525u * 910u;
  desc.samples_per_line_nominal = 910;
  return desc;
}

// Single-frame NTSC source mock returning `runs` as its dropout hints.
std::shared_ptr<NiceMock<MockVideoFrameRepresentation>>
make_single_frame_source(std::vector<orc::DropoutRun> runs) {
  auto source = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 0u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(1u));
  ON_CALL(*source, get_frame_descriptor(orc::FrameID{0}))
      .WillByDefault(Return(make_ntsc_descriptor(orc::FrameID{0})));
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(orc::FrameID{0}))
      .WillByDefault(Return(std::move(runs)));
  return source;
}

}  // namespace

// The sink must report exactly the hints returned by its input representation —
// count and summed length per analysed frame, with the frame's true 1-based
// frame number. No caching or upstream state is involved.
TEST(DropoutAnalysisSinkMapContractTest,
     ComputeAndAnalyze_ReportsHintsFromInputRepresentation) {
  auto rep = make_single_frame_source({orc::DropoutRun{0u, 100u, 40u, 128},
                                       orc::DropoutRun{0u, 500u, 10u, 128}});

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result = deps.compute_and_analyze(rep.get(), ctx, {});

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 1u);
  EXPECT_EQ(result.frame_stats[0].frame_number, 1);
  EXPECT_EQ(result.frame_stats[0].dropout_count, 2);
  EXPECT_EQ(result.frame_stats[0].dropout_length_samples, 50);
}

// A run added by the dropout_map appears in the sink stats even though the
// underlying source has no dropouts. Wiring: DropoutMappedFrameRepresentation
// (the merge produced by DropoutMapStage) → sink deps.
TEST(DropoutAnalysisSinkMapContractTest, MapAddition_AppearsInSinkStats) {
  auto source = make_single_frame_source({});  // no source dropouts

  // Add a dropout on NTSC frame 0, line 10, samples 100-200.
  // Line 10 offset = 10 * 910 = 9100; count = 200 - 100 + 1 = 101.
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.additions.push_back({10u, 100u, 200u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
  auto rep =
      std::make_shared<orc::DropoutMappedFrameRepresentation>(source, dmap);

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);
  const auto result = deps.compute_and_analyze(rep.get(), ctx, {});

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 1u);
  EXPECT_EQ(result.frame_stats[0].dropout_count, 1);
  EXPECT_EQ(result.frame_stats[0].dropout_length_samples, 101);
}

// A sidecar run removed by the dropout_map must not appear in the sink stats.
// Baseline: the same source run is counted when the map does not remove it.
TEST(DropoutAnalysisSinkMapContractTest,
     MapRemoval_RemovesSourceRunFromSinkStats) {
  // Source run: NTSC line 10, samples 100-200 → flat start 9200, count 101.
  const orc::DropoutRun source_run{0u, 9200u, 101u, 128};

  orc::ObservationContext ctx;
  orc::DropoutAnalysisSinkStageDeps deps;
  deps.init({}, nullptr);

  // Baseline: no removal → the source run is reported.
  {
    auto source = make_single_frame_source({source_run});
    std::map<uint64_t, orc::FrameDropoutMapEntry> empty_map;
    auto rep = std::make_shared<orc::DropoutMappedFrameRepresentation>(
        source, empty_map);
    const auto baseline = deps.compute_and_analyze(rep.get(), ctx, {});
    ASSERT_EQ(baseline.frame_stats.size(), 1u);
    EXPECT_EQ(baseline.frame_stats[0].dropout_count, 1);
    EXPECT_EQ(baseline.frame_stats[0].dropout_length_samples, 101);
  }

  // With a removal covering that region → the run disappears from the stats.
  {
    auto source = make_single_frame_source({source_run});
    orc::FrameDropoutMapEntry entry;
    entry.frame_id = 0;
    entry.removals.push_back({10u, 100u, 200u});
    std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
    auto rep =
        std::make_shared<orc::DropoutMappedFrameRepresentation>(source, dmap);
    const auto result = deps.compute_and_analyze(rep.get(), ctx, {});
    ASSERT_EQ(result.frame_stats.size(), 1u);
    EXPECT_EQ(result.frame_stats[0].dropout_count, 0);
    EXPECT_EQ(result.frame_stats[0].dropout_length_samples, 0);
  }
}

}  // namespace orc_unit_test
