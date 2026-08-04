/*
 * File:        vbi_frame_synthesis_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised CVBS frame assembler
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_synthesis.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "vbi_line_placement.h"

namespace orc {
namespace {

// EBU Tech. 3280-E Section 1.2: the normative PAL frame.
constexpr uint32_t kPALFrameSamples = 709379;
constexpr uint32_t kPALFrameLines = 625;

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

VBIFrameSynthesiser pal_synthesiser(bool synthesise_burst = true) {
  VBIFrameSynthesisConfig config;
  config.synthesise_burst = synthesise_burst;

  VBIFrameSynthesiser synthesiser;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_synthesiser(bt8x8_pal_format(), config,
                                         synthesiser, error))
      << error;
  return synthesiser;
}

// One mapped record whose samples are already in the output amplitude domain,
// as the level mapper leaves them.
VBIMappedLine mapped_line(uint32_t field_index, uint32_t record_index,
                          uint32_t valid_samples, double logic0,
                          double logic1) {
  VBIMappedLine line;
  line.field_index = field_index;
  line.record_index = record_index;
  line.levels_established = true;
  line.samples.assign(valid_samples, logic0);

  // A slow sinusoid between the two logic levels: well inside the output band,
  // so what lands in the frame can be recognised without the resampler's own
  // response being the thing under test.
  const double middle = 0.5 * (logic0 + logic1);
  const double swing = 0.5 * (logic1 - logic0);
  for (uint32_t index = 0; index < valid_samples; ++index) {
    line.samples[index] =
        middle + swing * std::sin(2.0 * 3.14159265358979323846 *
                                  static_cast<double>(index) / 128.0);
  }
  return line;
}

TEST(VBIFrameSynthesis, NormativeFrameSizeIsThePALFrame) {
  uint32_t samples = 0;
  std::string error;

  EXPECT_TRUE(vbi_normative_frame_samples(VBITVSystem::kPAL, samples, error));
  EXPECT_EQ(samples, kPALFrameSamples);

  EXPECT_FALSE(vbi_normative_frame_samples(VBITVSystem::kNTSC, samples, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(samples, 0u);
}

// The frame the standard requires, assembled line by line through the geometry
// module and counted rather than assumed (design §2.1, §8).
TEST(VBIFrameSynthesis, ABlankFrameHoldsTheNormativeSampleCount) {
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();

  VBISynthesisedFrame frame;
  std::string error;
  ASSERT_TRUE(synthesiser.synthesise_blank_frame(0, frame, error)) << error;

  EXPECT_EQ(frame.samples.size(), kPALFrameSamples);
  EXPECT_EQ(frame.output_frame_index, 0u);
  EXPECT_TRUE(frame.data_frame_lines.empty());
  EXPECT_TRUE(frame.padding);
  EXPECT_TRUE(frame.burst_synthesised);
}

// Design §2.2: nothing written may reach the protected ends of the sample
// encoding, and this is asserted of every frame the assembler produces.
TEST(VBIFrameSynthesis, EverySampleIsInsideTheLegalRange) {
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();

  VBISynthesisedFrame frame;
  std::string error;
  ASSERT_TRUE(synthesiser.synthesise_blank_frame(0, frame, error)) << error;

  uint16_t lowest = kVBIOutputSampleMax;
  uint16_t highest = kVBIOutputSampleMin;
  for (const uint16_t value : frame.samples) {
    ASSERT_GE(value, kVBIOutputSampleMin);
    ASSERT_LE(value, kVBIOutputSampleMax);
    lowest = std::min(lowest, value);
    highest = std::max(highest, value);
  }

  // Sync tip and the burst's positive excursion are both present.
  EXPECT_EQ(lowest, kVBIOutputSampleMin);
  EXPECT_GT(highest, 300);
}

// A run of frames must all be the same size with no cumulative drift: a frame
// four samples short displaces every frame after it in the file.
TEST(VBIFrameSynthesis, ATenFrameRunProducesIdenticallySizedFrames) {
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();

  std::string error;
  for (uint64_t index = 0; index < 10; ++index) {
    VBISynthesisedFrame frame;
    ASSERT_TRUE(synthesiser.synthesise_blank_frame(index, frame, error))
        << error;
    EXPECT_EQ(frame.samples.size(), kPALFrameSamples) << "frame " << index;
    EXPECT_EQ(frame.output_frame_index, index);
  }
}

// Design §8: a geometry that is not the standard's frame is a reported stage
// error rather than a crash or a silently short frame.  A PAL frame indexed at
// a constant 1135 samples per line is the mistake this catches.
TEST(VBIFrameSynthesis, AConstantStridePALGeometryIsRefused) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBITeletextService service;
  VBIVerticalInterval vertical_interval;
  VBITeletextLineMap line_map;
  VBIOutputLevels levels;
  VBIBurstTiming burst_timing;
  std::string error;
  ASSERT_TRUE(
      vbi_teletext_service(format.tv_system, format.tt_system, service, error));
  ASSERT_TRUE(
      make_vbi_vertical_interval(format.tv_system, vertical_interval, error));
  ASSERT_TRUE(make_vbi_teletext_line_map(format.tv_system, format.tt_system,
                                         line_map, error));
  ASSERT_TRUE(vbi_output_levels(format.tv_system, levels, error));
  ASSERT_TRUE(
      make_vbi_burst_timing(format.tv_system, levels, burst_timing, error));

  const VBIFrameSynthesiser synthesiser(
      format, service, VBIFrameGeometry(kPALFrameLines, kPALFrameLines * 1135u),
      vertical_interval, line_map, levels, burst_timing,
      VBIFrameSynthesisConfig{}, 17734475.0);

  VBISynthesisedFrame frame;
  error.clear();
  EXPECT_FALSE(synthesiser.synthesise_blank_frame(0, frame, error));
  EXPECT_NE(error.find("709375"), std::string::npos) << error;
  EXPECT_NE(error.find("709379"), std::string::npos) << error;
}

TEST(VBIFrameSynthesis, AnUnbuiltSynthesiserReportsRatherThanCrashes) {
  const VBIFrameSynthesiser synthesiser;

  VBISynthesisedFrame frame;
  std::string error;
  EXPECT_FALSE(synthesiser.synthesise_blank_frame(0, frame, error));
  EXPECT_FALSE(error.empty());
}

// Data lands on the frame lines the standard assigns to the service, and
// nowhere else.
TEST(VBIFrameSynthesis, RecordsLandOnTheStandardsTeletextFrameLines) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();
  const VBIBandLimitedResampler resampler(2.0);

  std::vector<VBIMappedLine> mapped;
  for (uint32_t field = 0; field < 2; ++field) {
    for (uint32_t record = format.field_range.start;
         record <= format.field_range.end; ++record) {
      mapped.push_back(
          mapped_line(field, record, format.valid_samples, 256.0, 644.0));
    }
  }

  VBISynthesisedFrame frame;
  std::string error;
  ASSERT_TRUE(
      synthesiser.synthesise_frame(0, mapped, resampler, 262.0, frame, error))
      << error;

  ASSERT_EQ(frame.data_frame_lines.size(), 32u);
  EXPECT_EQ(frame.data_frame_lines.front(), 6u);
  EXPECT_EQ(frame.data_frame_lines[15], 21u);
  EXPECT_EQ(frame.data_frame_lines[16], 319u);
  EXPECT_EQ(frame.data_frame_lines.back(), 334u);
  EXPECT_FALSE(frame.padding);
  EXPECT_EQ(frame.samples.size(), kPALFrameSamples);
}

// The data region of a carrying line holds the source's waveform at the
// service's anchor, and the line's blanking either side of it is untouched.
TEST(VBIFrameSynthesis, DataOccupiesTheServicesWindowAndNothingElse) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();
  const VBIBandLimitedResampler resampler(2.0);
  const uint32_t frame_line = 6;
  const double capture_offset = 262.0;

  std::vector<VBIMappedLine> mapped{mapped_line(
      0, format.field_range.start, format.valid_samples, 256.0, 644.0)};

  VBISynthesisedFrame frame;
  std::string error;
  ASSERT_TRUE(synthesiser.synthesise_frame(0, mapped, resampler, capture_offset,
                                           frame, error))
      << error;
  ASSERT_EQ(frame.data_frame_lines, std::vector<uint32_t>{frame_line});

  VBITeletextService service;
  ASSERT_TRUE(
      vbi_teletext_service(format.tv_system, format.tt_system, service, error));
  VBILinePlacement placement;
  ASSERT_TRUE(make_vbi_line_placement(format, service, synthesiser.geometry(),
                                      capture_offset, frame_line, placement,
                                      error));

  const uint32_t line_start = synthesiser.geometry().line_start(frame_line);
  const uint32_t line_length = synthesiser.geometry().line_length(frame_line);
  const VBIDataWindow window = vbi_data_region_window(
      placement, synthesiser.data_guard_samples(), line_length);
  ASSERT_GT(window.count(), 0u);

  // The data region swings between the mapped logic levels.
  uint16_t lowest = kVBIOutputSampleMax;
  uint16_t highest = kVBIOutputSampleMin;
  for (uint32_t index = window.begin; index < window.end; ++index) {
    const uint16_t value = frame.samples[line_start + index];
    lowest = std::min(lowest, value);
    highest = std::max(highest, value);
  }
  EXPECT_NEAR(lowest, synthesiser.levels().logic0, 4);
  EXPECT_NEAR(highest, synthesiser.levels().logic1, 4);

  // Everything before the window is the manufactured line: the sync tip is
  // still there, the colour burst still swings about blanking, and the back
  // porch just before the data is blanking.
  EXPECT_EQ(frame.samples[line_start + 40], synthesiser.levels().sync_tip);
  EXPECT_NE(frame.samples[line_start + 120], synthesiser.levels().blanking);
  ASSERT_GT(window.begin, 150u);
  EXPECT_EQ(frame.samples[line_start + window.begin - 2u],
            synthesiser.levels().blanking);

  // And the front porch after the packet ends is blanking again.
  ASSERT_LT(window.end + 2u, line_length);
  EXPECT_EQ(frame.samples[line_start + window.end + 2u],
            synthesiser.levels().blanking);
}

// A line whose levels could not be established carries no data: it is emitted
// as ordinary blanking rather than as an arbitrarily scaled waveform.
TEST(VBIFrameSynthesis, RecordsWithoutEstablishedLevelsBecomeBlankLines) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();
  const VBIBandLimitedResampler resampler(2.0);

  VBIMappedLine line = mapped_line(0, format.field_range.start,
                                   format.valid_samples, 256.0, 644.0);
  line.levels_established = false;

  VBISynthesisedFrame frame;
  std::string error;
  ASSERT_TRUE(
      synthesiser.synthesise_frame(0, {line}, resampler, 262.0, frame, error))
      << error;

  EXPECT_TRUE(frame.data_frame_lines.empty());
  EXPECT_TRUE(frame.padding);

  VBISynthesisedFrame blank;
  ASSERT_TRUE(synthesiser.synthesise_blank_frame(0, blank, error)) << error;
  EXPECT_EQ(frame.samples, blank.samples);
}

// Two records cannot share a frame line: that is a configuration mistake, and
// silently letting one overwrite the other would lose a line of provenance.
TEST(VBIFrameSynthesis, TwoRecordsOnOneFrameLineAreRefused) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();
  const VBIBandLimitedResampler resampler(2.0);

  const VBIMappedLine line = mapped_line(0, format.field_range.start,
                                         format.valid_samples, 256.0, 644.0);

  VBISynthesisedFrame frame;
  std::string error;
  EXPECT_FALSE(synthesiser.synthesise_frame(0, {line, line}, resampler, 262.0,
                                            frame, error));
  EXPECT_FALSE(error.empty());
}

// Burst omission is supported, and it shows: the burst window falls back to
// blanking while the rest of the line is unchanged.
TEST(VBIFrameSynthesis, OmittingBurstLeavesTheBurstWindowAtBlanking) {
  const VBIFrameSynthesiser with_burst = pal_synthesiser(true);
  const VBIFrameSynthesiser without_burst = pal_synthesiser(false);

  VBISynthesisedFrame burst_frame;
  VBISynthesisedFrame plain_frame;
  std::string error;
  ASSERT_TRUE(with_burst.synthesise_blank_frame(0, burst_frame, error))
      << error;
  ASSERT_TRUE(without_burst.synthesise_blank_frame(0, plain_frame, error))
      << error;

  EXPECT_TRUE(burst_frame.burst_synthesised);
  EXPECT_FALSE(plain_frame.burst_synthesised);
  EXPECT_EQ(plain_frame.samples.size(), kPALFrameSamples);
  EXPECT_NE(burst_frame.samples, plain_frame.samples);

  // Without burst, a teletext line's burst window is flat blanking.
  const uint32_t line_start = without_burst.geometry().line_start(6);
  for (uint32_t index = 100; index < 140; ++index) {
    EXPECT_EQ(plain_frame.samples[line_start + index],
              without_burst.levels().blanking)
        << "sample " << index;
  }
}

// Frames are synthesised on demand and in whatever order they are asked for,
// so the same index must always produce the same frame — and the PAL colour
// sequence must repeat over four of them, not sooner.
TEST(VBIFrameSynthesis, TheColourSequenceRepeatsEveryFourFrames) {
  const VBIFrameSynthesiser synthesiser = pal_synthesiser();

  std::vector<VBISynthesisedFrame> frames(9);
  std::string error;
  for (uint64_t index = 0; index < frames.size(); ++index) {
    ASSERT_TRUE(synthesiser.synthesise_blank_frame(
        index, frames[static_cast<size_t>(index)], error))
        << error;
  }

  VBISynthesisedFrame repeat;
  ASSERT_TRUE(synthesiser.synthesise_blank_frame(3, repeat, error)) << error;
  EXPECT_EQ(frames[3].samples, repeat.samples);

  EXPECT_EQ(frames[0].samples, frames[4].samples);
  EXPECT_EQ(frames[1].samples, frames[5].samples);
  EXPECT_EQ(frames[4].samples, frames[8].samples);

  EXPECT_NE(frames[0].samples, frames[1].samples);
  EXPECT_NE(frames[0].samples, frames[2].samples);
  EXPECT_NE(frames[0].samples, frames[3].samples);
}

}  // namespace
}  // namespace orc
