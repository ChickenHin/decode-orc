/*
 * File:        teletext_frame_slicer_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink's per-frame slicer
 *
 * Covers: which systems carry a WST service, the per-system candidate window
 * and its override, the black/white level fallback, the luma-vs-composite line
 * fetch, and frame-height clamping. Every line of video is synthesised in
 * memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_frame_slicer.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

#include "../../common/vbi-services/teletext_line_synthesizer.h"
#include "../../include/video_frame_representation_artifact_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

namespace {

orc::SourceParameters make_pal_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  p.frame_height = orc::kPalFrameLines;
  p.black_level = orc::kPalBlack;
  p.white_level = orc::kPalWhite;
  return p;
}

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = orc::kNtscSamplesPerLine;
  p.frame_height = orc::kNtscFrameLines;
  p.black_level = orc::kNtscBlack;
  p.white_level = orc::kNtscWhite;
  return p;
}

}  // namespace

class TeletextFrameSlicer : public ::testing::Test {
 protected:
  void serve(const orc::SourceParameters& params) {
    EXPECT_CALL(mockRepresentation_, get_video_parameters())
        .WillRepeatedly(Return(params));
    EXPECT_CALL(mockRepresentation_, get_line(_, _))
        .WillRepeatedly(
            Invoke([this](orc::FrameID frame, size_t line) -> const int16_t* {
              const auto it = lines_.find({frame, line});
              return it == lines_.end() ? nullptr : it->second.data();
            }));
  }

  void serve_luma(const orc::SourceParameters& params) {
    EXPECT_CALL(mockRepresentation_, get_video_parameters())
        .WillRepeatedly(Return(params));
    EXPECT_CALL(mockRepresentation_, has_separate_channels())
        .WillRepeatedly(Return(true));
    EXPECT_CALL(mockRepresentation_, get_line_luma(_, _))
        .WillRepeatedly(
            Invoke([this](orc::FrameID frame, size_t line) -> const int16_t* {
              const auto it = lines_.find({frame, line});
              return it == lines_.end() ? nullptr : it->second.data();
            }));
    // A YC source must never be read through the composite path.
    EXPECT_CALL(mockRepresentation_, get_line(_, _)).Times(0);
  }

  void put_line(orc::FrameID frame, size_t flat_line,
                std::vector<int16_t> samples) {
    lines_[{frame, flat_line}] = std::move(samples);
  }

  // A mock that reports YC separation needs get_line_luma; the base mock only
  // declares get_line, so the luma accessor is stubbed through a derived mock.
  class YcRepresentation : public MockVideoFrameRepresentationArtifact {
   public:
    MOCK_METHOD(bool, has_separate_channels, (), (const, override));
    MOCK_METHOD(const sample_type*, get_line_luma, (orc::FrameID, size_t),
                (const, override));
  };

  NiceMock<YcRepresentation> mockRepresentation_;
  std::map<std::pair<orc::FrameID, size_t>, std::vector<int16_t>> lines_;
};

////////////////////////////////////////////////////////////////////////////////////////////

// ITU-R BT.653 System B is defined on 625 lines (ETSI EN 300 706) and on 525
// (Table 1b); a source of unknown system carries neither.
TEST_F(TeletextFrameSlicer, AppliesTo_CoversBothSystemBTelevisionSystems) {
  EXPECT_TRUE(orc::TeletextFrameSlicer::applies_to(orc::VideoSystem::PAL));
  EXPECT_TRUE(orc::TeletextFrameSlicer::applies_to(orc::VideoSystem::NTSC));
  EXPECT_TRUE(orc::TeletextFrameSlicer::applies_to(orc::VideoSystem::PAL_M));
  EXPECT_FALSE(orc::TeletextFrameSlicer::applies_to(orc::VideoSystem::Unknown));
}

TEST_F(TeletextFrameSlicer, ProfileFor_SelectsTheSystemsServiceAndWindow) {
  const auto pal = orc::TeletextFrameSlicer::profile_for(orc::VideoSystem::PAL);
  EXPECT_EQ(pal.teletext_system, orc::TeletextSystem::kWst625);
  EXPECT_EQ(pal.first_field_line, orc::kTeletextFirstFieldLine625);
  EXPECT_EQ(pal.last_field_line, orc::kTeletextLastFieldLine625);

  // NTSC and PAL_M share the 525-line structure and therefore the service.
  for (const auto system : {orc::VideoSystem::NTSC, orc::VideoSystem::PAL_M}) {
    const auto profile = orc::TeletextFrameSlicer::profile_for(system);
    EXPECT_EQ(profile.teletext_system, orc::TeletextSystem::kWst525);
    EXPECT_EQ(profile.first_field_line, orc::kTeletextFirstFieldLine525);
    EXPECT_EQ(profile.last_field_line, orc::kTeletextLastFieldLine525);
  }
}

TEST_F(TeletextFrameSlicer, EffectiveProfile_AppliesTheWindowOverride) {
  orc::TeletextFrameSlicerOptions options;
  options.first_field_line = 9;
  options.last_field_line = 11;
  const orc::TeletextFrameSlicer slicer(options);

  const auto profile = slicer.effective_profile(orc::VideoSystem::PAL);
  // The service is still the system's; only the window moves.
  EXPECT_EQ(profile.teletext_system, orc::TeletextSystem::kWst625);
  EXPECT_EQ(profile.first_field_line, 9);
  EXPECT_EQ(profile.last_field_line, 11);
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextFrameSlicer, SliceField_ReportsEveryCandidateLineOfTheWindow) {
  const auto params = make_pal_params();
  serve(params);

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::TeletextFrameSlicer slicer(options);

  // Every candidate line of field 1 reads as blank, but each is still reported
  // so a caller accumulating recovery diagnostics sees what was looked at.
  const std::vector<int16_t> blank(
      static_cast<size_t>(params.frame_width_nominal),
      static_cast<int16_t>(orc::kPalBlack));
  for (int32_t line = orc::kTeletextFirstFieldLine625;
       line <= orc::kTeletextLastFieldLine625; ++line) {
    put_line(0, static_cast<size_t>(line), blank);
  }

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(),
            static_cast<size_t>(orc::kTeletextLastFieldLine625 -
                                orc::kTeletextFirstFieldLine625 + 1));
  EXPECT_EQ(scan.lines.front().field_line, orc::kTeletextFirstFieldLine625);
  EXPECT_EQ(scan.lines.back().field_line, orc::kTeletextLastFieldLine625);
  for (const auto& result : scan.lines) {
    EXPECT_FALSE(result.sliced.valid);
    EXPECT_EQ(result.sliced.packet_bytes, orc::kTeletextPacketBytes);
  }
}

TEST_F(TeletextFrameSlicer, SliceField_RecoversA625LinePacketFromField1) {
  const auto params = make_pal_params();
  serve(params);
  const auto payload = orc::tests::make_parity_coded_payload();
  put_line(3, 7, orc::tests::synthesize_teletext_line(payload));

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 7;
  options.last_field_line = 7;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 3, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  EXPECT_EQ(scan.lines[0].flat_line, 7u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.packet_bytes, orc::kTeletextPacketBytes);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// Field 2's candidate lines sit one field-1 line count further into the
// frame-flat buffer.
TEST_F(TeletextFrameSlicer, SliceField_OffsetsField2ByTheField1LineCount) {
  const auto params = make_pal_params();
  serve(params);
  const auto payload = orc::tests::make_parity_coded_payload();
  const size_t expected_flat = orc::field1_lines(orc::VideoSystem::PAL) + 7;
  put_line(0, expected_flat, orc::tests::synthesize_teletext_line(payload));

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 7;
  options.last_field_line = 7;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 1, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  EXPECT_EQ(scan.lines[0].flat_line, expected_flat);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

TEST_F(TeletextFrameSlicer, SliceField_RecoversThe525LineServiceOnNtsc) {
  const auto params = make_ntsc_params();
  serve(params);
  const auto payload = orc::tests::make_525_test_payload();
  put_line(0, 10,
           orc::tests::synthesize_teletext_line(
               payload, orc::tests::ntsc_wst_synth_options(),
               orc::kTeletext525PacketBytes));

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  // Only the line that carried samples is reported; the 525-line window of
  // ITU-R BT.653 §2 (field lines 9-20) is where it was looked for.
  ASSERT_EQ(scan.lines.size(), 1u);
  EXPECT_EQ(scan.lines[0].field_line, 10);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.packet_bytes, orc::kTeletext525PacketBytes);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// A source that states no levels is sliced against the spec constants, which
// is what a TBC written before the fields existed relies on.
TEST_F(TeletextFrameSlicer, SliceField_FallsBackToTheSpecDataLevels) {
  auto params = make_pal_params();
  params.black_level = -1;
  params.white_level = -1;
  serve(params);
  const auto payload = orc::tests::make_parity_coded_payload();
  put_line(0, 7, orc::tests::synthesize_teletext_line(payload));

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 7;
  options.last_field_line = 7;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// YC sources carry the data burst in the luma channel; reading them through
// the composite accessor would slice a channel the data is not in.
TEST_F(TeletextFrameSlicer, SliceField_ReadsLumaOnSeparateChannelSources) {
  const auto params = make_pal_params();
  serve_luma(params);
  const auto payload = orc::tests::make_parity_coded_payload();
  put_line(0, 7, orc::tests::synthesize_teletext_line(payload));

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 7;
  options.last_field_line = 7;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// A window reaching past the frame is clamped rather than reading off the end.
TEST_F(TeletextFrameSlicer, SliceField_SkipsLinesBeyondTheFrame) {
  auto params = make_pal_params();
  params.frame_height = 10;  // field 2 starts well past this
  serve(params);

  orc::TeletextFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::TeletextFrameSlicer slicer(options);

  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 1, scan);
  EXPECT_TRUE(scan.lines.empty());
}

TEST_F(TeletextFrameSlicer, SliceField_YieldsNothingWithoutVideoParameters) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(std::nullopt));

  const orc::TeletextFrameSlicer slicer;
  orc::TeletextFieldScan scan;
  scan.lines.emplace_back();  // must be cleared even on the early return
  slicer.slice_field(mockRepresentation_, 0, 0, scan);
  EXPECT_TRUE(scan.lines.empty());
}

TEST_F(TeletextFrameSlicer, SliceField_YieldsNothingForASystemWithoutWst) {
  orc::SourceParameters params{};
  params.system = orc::VideoSystem::Unknown;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(params));

  const orc::TeletextFrameSlicer slicer;
  orc::TeletextFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);
  EXPECT_TRUE(scan.lines.empty());
}

}  // namespace orc_unit_test
