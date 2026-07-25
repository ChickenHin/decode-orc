/*
 * File:        dropout_analysis_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the DropoutAnalysisSinkStageDeps CSV writer
 *              (canonical per-frame schema, filesystem-free stream formatter)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_deps.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {

using testing::NiceMock;
using testing::Return;

TEST(DropoutAnalysisSinkCsvTest, HeaderIsSelfDescribing) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::ostringstream os;
  deps.write_csv(os, {});
  EXPECT_EQ(os.str(), "frame_number,dropout_count,dropout_length_samples\n");
}

TEST(DropoutAnalysisSinkCsvTest, WritesOneRowPerAnalysedFrame) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::vector<orc::FrameDropoutStats> stats;
  orc::FrameDropoutStats a{};
  a.frame_number = 1;
  a.dropout_count = 3;
  a.dropout_length_samples = 128;
  a.has_data = true;
  orc::FrameDropoutStats b{};
  b.frame_number = 2;
  b.dropout_count = 1;
  b.dropout_length_samples = 20;
  b.has_data = true;
  stats.push_back(a);
  stats.push_back(b);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,dropout_count,dropout_length_samples\n"
            "1,3,128\n"
            "2,1,20\n");
}

// A zero-dropout frame is genuine data ("analysed, no dropouts") and must
// appear as a zero row; only unanalysed frames are absent from the CSV.
TEST(DropoutAnalysisSinkCsvTest, ZeroDropoutFramesAreEmittedAsZeroRows) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::vector<orc::FrameDropoutStats> stats;
  orc::FrameDropoutStats with_dropouts{};
  with_dropouts.frame_number = 5;
  with_dropouts.dropout_count = 2;
  with_dropouts.dropout_length_samples = 64;
  with_dropouts.has_data = true;
  orc::FrameDropoutStats clean{};
  clean.frame_number = 6;
  clean.dropout_count = 0;
  clean.dropout_length_samples = 0;
  clean.has_data = false;  // analysed, but no dropouts
  stats.push_back(with_dropouts);
  stats.push_back(clean);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,dropout_count,dropout_length_samples\n"
            "5,2,64\n"
            "6,0,0\n");
}

// When cancellation is requested, analysis reports failure and produces no
// stats so the stage never proceeds to write a (partial) CSV. This is the
// deps-level guard behind the "cancelling leaves no truncated output file"
// requirement.
TEST(DropoutAnalysisSinkDepsTest, CancelledRunReportsFailureAndWritesNothing) {
  orc::DropoutAnalysisSinkStageDeps deps;
  std::atomic<bool> cancel{true};
  deps.init(nullptr, &cancel);

  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*vfr, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0, 4}));  // 5 frames
  NiceMock<MockObservationContext> ctx;

  const auto result = deps.compute_and_analyze(
      vfr.get(), ctx, orc::DropoutAnalysisComputeOptions{});

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Cancelled by user");
  EXPECT_TRUE(result.frame_stats.empty());
  EXPECT_EQ(result.total_frames, 0);
}

}  // namespace orc_unit_test
