/*
 * File:        snr_analysis_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for SNRAnalysisSinkStageDeps per-frame capture and
 *              the frame_interval sampling parameter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/snr_analysis_sink/snr_analysis_sink_deps.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/observation_service_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::_;
using testing::NiceMock;
using testing::Return;

namespace {

// Observation value keyed off the field id so each analysed frame gets a
// distinct, predictable metric: white SNR == frame_number, black PSNR ==
// frame_number + 100. field_id == frame_id * 2, frame_number == frame_id + 1.
std::optional<orc::ObservationValue> white_for(orc::FieldID fid,
                                               const std::string&,
                                               const std::string&) {
  const double frame_number = static_cast<double>(fid.value() / 2 + 1);
  return orc::ObservationValue(frame_number);
}
std::optional<orc::ObservationValue> black_for(orc::FieldID fid,
                                               const std::string&,
                                               const std::string&) {
  const double frame_number = static_cast<double>(fid.value() / 2 + 1);
  return orc::ObservationValue(frame_number + 100.0);
}

// Builds a deps instance wired to a service that hands out no-op observer
// handles, plus a context that answers white/black queries via the helpers
// above. Ownership of the mocks is returned to the caller.
struct Harness {
  std::shared_ptr<NiceMock<MockObservationService>> service;
  std::shared_ptr<NiceMock<MockObservationContext>> context;
  std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>> vfr;
  std::unique_ptr<orc::SNRAnalysisSinkStageDeps> deps;
  std::atomic<bool> cancel{false};
};

std::unique_ptr<Harness> make_harness(orc::FrameIDRange range) {
  auto h = std::make_unique<Harness>();
  h->service = std::make_shared<NiceMock<MockObservationService>>();
  h->context = std::make_shared<NiceMock<MockObservationContext>>();
  h->vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*h->service, create_observer(_))
      .WillByDefault([](const std::string&) {
        return std::make_unique<NiceMock<MockObserverHandle>>();
      });
  ON_CALL(*h->context, get(_, "white_snr", "snr_db")).WillByDefault(white_for);
  ON_CALL(*h->context, get(_, "black_psnr", "psnr_db"))
      .WillByDefault(black_for);
  ON_CALL(*h->vfr, frame_range()).WillByDefault(Return(range));

  h->deps = std::make_unique<orc::SNRAnalysisSinkStageDeps>(h->service.get());
  h->deps->init(nullptr, &h->cancel);
  return h;
}

orc::SNRAnalysisComputeOptions options_with_interval(int32_t interval) {
  orc::SNRAnalysisComputeOptions opts;
  opts.snr_mode = orc::SNRAnalysisMode::BOTH;
  opts.frame_interval = interval;
  return opts;
}

}  // namespace

TEST(SNRAnalysisSinkDepsTest, IntervalOneAnalysesEveryFrame) {
  auto h = make_harness(orc::FrameIDRange{0, 4});  // 5 frames
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   options_with_interval(1));

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 5u);
  EXPECT_EQ(result.total_frames, 5);
  for (size_t i = 0; i < result.frame_stats.size(); ++i) {
    const auto& fs = result.frame_stats[i];
    EXPECT_EQ(fs.frame_number, static_cast<int32_t>(i) + 1);
    EXPECT_TRUE(fs.has_white_snr);
    EXPECT_TRUE(fs.has_black_psnr);
    EXPECT_DOUBLE_EQ(fs.white_snr, static_cast<double>(fs.frame_number));
    EXPECT_DOUBLE_EQ(fs.black_psnr,
                     static_cast<double>(fs.frame_number) + 100.0);
  }
}

TEST(SNRAnalysisSinkDepsTest, IntervalGreaterThanOneSubsamplesTrueFrames) {
  auto h = make_harness(orc::FrameIDRange{0, 4});  // 5 frames
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   options_with_interval(2));

  ASSERT_TRUE(result.success);
  // offsets 0,2,4 → frame numbers 1,3,5
  ASSERT_EQ(result.frame_stats.size(), 3u);
  EXPECT_EQ(result.total_frames, 5);
  EXPECT_EQ(result.frame_stats[0].frame_number, 1);
  EXPECT_EQ(result.frame_stats[1].frame_number, 3);
  EXPECT_EQ(result.frame_stats[2].frame_number, 5);
  EXPECT_DOUBLE_EQ(result.frame_stats[1].white_snr, 3.0);
}

TEST(SNRAnalysisSinkDepsTest, IntervalLargerThanTotalAnalysesFirstFrameOnly) {
  auto h = make_harness(orc::FrameIDRange{0, 4});  // 5 frames
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   options_with_interval(10));

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 1u);
  EXPECT_EQ(result.frame_stats[0].frame_number, 1);
  EXPECT_EQ(result.total_frames, 5);
}

TEST(SNRAnalysisSinkDepsTest, NonZeroFirstFramePreservesTrueFrameNumbers) {
  auto h = make_harness(orc::FrameIDRange{10, 12});  // frames at ids 10,11,12
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   options_with_interval(1));

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 3u);
  EXPECT_EQ(result.frame_stats[0].frame_number, 11);
  EXPECT_EQ(result.frame_stats[1].frame_number, 12);
  EXPECT_EQ(result.frame_stats[2].frame_number, 13);
}

TEST(SNRAnalysisSinkDepsTest, EmptyRangeYieldsNoRecords) {
  auto h = make_harness(orc::FrameIDRange{1, 0});  // count() == 0
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   options_with_interval(1));

  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.frame_stats.empty());
  EXPECT_EQ(result.total_frames, 0);
}

}  // namespace orc_unit_test
