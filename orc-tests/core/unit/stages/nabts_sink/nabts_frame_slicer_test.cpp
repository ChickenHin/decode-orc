/*
 * File:        nabts_frame_slicer_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NABTS sink's per-frame slicer
 *
 * Covers: which systems carry a System C service, the candidate window and its
 * override, the black/white level fallback, the luma-vs-composite line fetch,
 * and frame-height clamping. Every line of video is synthesised in memory; no
 * I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_frame_slicer.h"

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

class NabtsFrameSlicer : public ::testing::Test {
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

// CEA-516 §1.1.1 specifies NABTS on the 525-line signal. PAL therefore carries
// no System C service at all — unlike World System Teletext, which is defined
// on both line structures — and a source of unknown system carries none either.
TEST_F(NabtsFrameSlicer, AppliesTo_CoversThe525LineSystemsOnly) {
  EXPECT_TRUE(orc::NabtsFrameSlicer::applies_to(orc::VideoSystem::NTSC));
  EXPECT_TRUE(orc::NabtsFrameSlicer::applies_to(orc::VideoSystem::PAL_M));
  EXPECT_FALSE(orc::NabtsFrameSlicer::applies_to(orc::VideoSystem::PAL));
  EXPECT_FALSE(orc::NabtsFrameSlicer::applies_to(orc::VideoSystem::Unknown));
}

// NTSC and PAL_M share the 525-line structure and therefore the window; only
// the 4FSC sample rate separates them.
TEST_F(NabtsFrameSlicer, ProfileFor_SelectsTheBt653WindowOnBothSystems) {
  const auto ntsc = orc::NabtsFrameSlicer::profile_for(orc::VideoSystem::NTSC);
  const auto pal_m =
      orc::NabtsFrameSlicer::profile_for(orc::VideoSystem::PAL_M);

  for (const auto& profile : {ntsc, pal_m}) {
    EXPECT_TRUE(profile.carries_nabts);
    EXPECT_EQ(profile.first_field_line, orc::kNabtsFirstFieldLine);
    EXPECT_EQ(profile.last_field_line, orc::kNabtsLastFieldLine);
  }

  EXPECT_EQ(ntsc.sample_rate, orc::kNtscSampleRate);
  EXPECT_EQ(pal_m.sample_rate, orc::kPalMSampleRate);
  EXPECT_NE(ntsc.slicer_index, pal_m.slicer_index);
}

TEST_F(NabtsFrameSlicer, ProfileFor_ReportsNoServiceOn625Lines) {
  EXPECT_FALSE(
      orc::NabtsFrameSlicer::profile_for(orc::VideoSystem::PAL).carries_nabts);
  EXPECT_FALSE(orc::NabtsFrameSlicer::profile_for(orc::VideoSystem::Unknown)
                   .carries_nabts);
}

TEST_F(NabtsFrameSlicer, EffectiveProfile_AppliesTheWindowOverride) {
  orc::NabtsFrameSlicerOptions options;
  options.first_field_line = 13;
  options.last_field_line = 16;
  const orc::NabtsFrameSlicer slicer(options);

  const auto profile = slicer.effective_profile(orc::VideoSystem::NTSC);
  EXPECT_TRUE(profile.carries_nabts);
  EXPECT_EQ(profile.first_field_line, 13);
  EXPECT_EQ(profile.last_field_line, 16);
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(NabtsFrameSlicer, SliceField_ReportsEveryCandidateLineOfTheWindow) {
  const auto params = make_ntsc_params();
  serve(params);

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::NabtsFrameSlicer slicer(options);

  // Every candidate line of field 1 reads as blank, but each is still reported
  // so a caller accumulating recovery diagnostics sees what was looked at.
  const std::vector<int16_t> blank(
      static_cast<size_t>(params.frame_width_nominal),
      static_cast<int16_t>(orc::kNtscBlack));
  for (int32_t line = orc::kNabtsFirstFieldLine;
       line <= orc::kNabtsLastFieldLine; ++line) {
    put_line(0, static_cast<size_t>(line), blank);
  }

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(),
            static_cast<size_t>(orc::kNabtsLastFieldLine -
                                orc::kNabtsFirstFieldLine + 1));
  EXPECT_EQ(scan.lines.front().field_line, orc::kNabtsFirstFieldLine);
  EXPECT_EQ(scan.lines.back().field_line, orc::kNabtsLastFieldLine);
  for (const auto& result : scan.lines) {
    EXPECT_FALSE(result.sliced.valid);
    EXPECT_EQ(result.sliced.packet_bytes, orc::kNabtsPacketBytes);
  }
}

TEST_F(NabtsFrameSlicer, SliceField_RecoversA33BytePacketFromField1) {
  const auto params = make_ntsc_params();
  serve(params);
  const auto payload = orc::tests::make_nabts_test_payload();
  put_line(3, 14, orc::tests::synthesize_nabts_line(payload));

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::NabtsFrameSlicer slicer(options);

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 3, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  EXPECT_EQ(scan.lines[0].field_line, 14);
  EXPECT_EQ(scan.lines[0].flat_line, 14u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.packet_bytes, orc::kNabtsPacketBytes);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// Field 2's candidate lines sit one field-1 line count further into the
// frame-flat buffer.
TEST_F(NabtsFrameSlicer, SliceField_OffsetsField2ByTheField1LineCount) {
  const auto params = make_ntsc_params();
  serve(params);
  const auto payload = orc::tests::make_nabts_test_payload();
  const size_t expected_flat = orc::field1_lines(orc::VideoSystem::NTSC) + 14;
  put_line(0, expected_flat, orc::tests::synthesize_nabts_line(payload));

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::NabtsFrameSlicer slicer(options);

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 1, scan);

  const auto* recovered = [&]() -> const orc::NabtsFrameLineResult* {
    for (const auto& line : scan.lines) {
      if (line.sliced.valid) {
        return &line;
      }
    }
    return nullptr;
  }();

  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->flat_line, expected_flat);
  EXPECT_EQ(recovered->sliced.bytes, payload);
}

// A source that states no levels is sliced against the spec constants, which
// is what a TBC written before the fields existed relies on.
TEST_F(NabtsFrameSlicer, SliceField_FallsBackToTheSpecDataLevels) {
  auto params = make_ntsc_params();
  params.black_level = -1;
  params.white_level = -1;
  serve(params);
  const auto payload = orc::tests::make_nabts_test_payload();
  put_line(0, 14, orc::tests::synthesize_nabts_line(payload));

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 14;
  options.last_field_line = 14;
  const orc::NabtsFrameSlicer slicer(options);

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// YC sources carry the data burst in the luma channel; reading them through
// the composite accessor would slice a channel the data is not in.
TEST_F(NabtsFrameSlicer, SliceField_ReadsLumaOnSeparateChannelSources) {
  const auto params = make_ntsc_params();
  serve_luma(params);
  const auto payload = orc::tests::make_nabts_test_payload();
  put_line(0, 14, orc::tests::synthesize_nabts_line(payload));

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  options.first_field_line = 14;
  options.last_field_line = 14;
  const orc::NabtsFrameSlicer slicer(options);

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);

  ASSERT_EQ(scan.lines.size(), 1u);
  ASSERT_TRUE(scan.lines[0].sliced.valid);
  EXPECT_EQ(scan.lines[0].sliced.bytes, payload);
}

// A window reaching past the frame is clamped rather than reading off the end.
TEST_F(NabtsFrameSlicer, SliceField_SkipsLinesBeyondTheFrame) {
  auto params = make_ntsc_params();
  params.frame_height = 10;  // field 2 starts well past this
  serve(params);

  orc::NabtsFrameSlicerOptions options;
  options.detector = orc::TeletextDetector::kThreshold;
  const orc::NabtsFrameSlicer slicer(options);

  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 1, scan);
  EXPECT_TRUE(scan.lines.empty());
}

TEST_F(NabtsFrameSlicer, SliceField_YieldsNothingWithoutVideoParameters) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(std::nullopt));

  const orc::NabtsFrameSlicer slicer;
  orc::NabtsFieldScan scan;
  scan.lines.emplace_back();  // must be cleared even on the early return
  slicer.slice_field(mockRepresentation_, 0, 0, scan);
  EXPECT_TRUE(scan.lines.empty());
}

// A 625-line source is read as carrying nothing rather than being sliced
// against a System C profile that does not exist for it.
TEST_F(NabtsFrameSlicer, SliceField_YieldsNothingOnA625LineSource) {
  orc::SourceParameters params{};
  params.system = orc::VideoSystem::PAL;
  params.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  params.frame_height = orc::kPalFrameLines;
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(params));

  const orc::NabtsFrameSlicer slicer;
  orc::NabtsFieldScan scan;
  slicer.slice_field(mockRepresentation_, 0, 0, scan);
  EXPECT_TRUE(scan.lines.empty());
}

}  // namespace orc_unit_test
