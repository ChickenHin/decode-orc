/*
 * File:        vbi_frame_builder_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for placing VBI records on an otherwise blank frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "vbi_source_format.h"
#include "vbi_synthetic_line.h"
#include "vbi_teletext_service.h"

namespace orc {
namespace {

using orc::testing::render_synthetic_vbi_line;
using orc::testing::SyntheticVBILine;

// The offset measured on the reference bt8x8 sample (design §9).
constexpr double kCaptureOffsetSamples = 261.6;

// Stored frame lines a 625-line WST capture places data on: broadcast lines
// 7-22 and 320-335 (design §5.1).
constexpr uint32_t kFirstField1Line = 6;
constexpr uint32_t kLastField1Line = 21;
constexpr uint32_t kFirstField2Line = 319;
constexpr uint32_t kLastField2Line = 334;

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  format.capture_offset_samples = kCaptureOffsetSamples;
  format.capture_offset_is_auto = false;
  return format;
}

VBITeletextService wst_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kPAL, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

VBIFrameBuilder builder_for(const VBISourceFormat& format,
                            VBILevelMapperConfig levels = {}) {
  VBIFrameBuilder builder;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_builder(format, levels, {kCaptureOffsetSamples},
                                     builder, error))
      << error;
  return builder;
}

// One stored frame of records, every one of them carrying teletext.
std::vector<VBILineRecord> frame_records(const VBISourceFormat& format,
                                         bool carries_data = true) {
  const VBITeletextService service = wst_service();
  const double anchor =
      service.cri_start_samples(format.sample_rate_hz, kCaptureOffsetSamples);

  std::vector<VBILineRecord> records;
  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t index = format.field_range.start;
         index <= format.field_range.end; ++index) {
      VBILineRecord record;
      record.field_index = field;
      record.record_index = index;

      SyntheticVBILine line;
      line.sample_rate_hz = format.sample_rate_hz;
      line.valid_samples = format.valid_samples;
      line.anchor_position_samples = anchor;
      line.carries_data = carries_data;
      line.seed = field * 32u + index + 1u;
      record.samples = render_synthetic_vbi_line(line);

      records.push_back(std::move(record));
    }
  }
  return records;
}

bool is_data_line(uint32_t line) {
  return (line >= kFirstField1Line && line <= kLastField1Line) ||
         (line >= kFirstField2Line && line <= kLastField2Line);
}

TEST(VBIFrameBuilder, ABlankFrameIsTheNormativeSizeAndBlankingThroughout) {
  const VBIFrameBuilder builder = builder_for(bt8x8_pal_format());

  std::vector<int16_t> frame;
  builder.build_blank_frame(frame);

  ASSERT_EQ(frame.size(),
            static_cast<size_t>(builder.output_frame().samples_per_frame));
  const auto bounds = std::minmax_element(frame.begin(), frame.end());
  EXPECT_EQ(*bounds.first, builder.output_frame().levels.blanking);
  EXPECT_EQ(*bounds.second, builder.output_frame().levels.blanking);
}

// The whole of the stage's job: the records land on the teletext lines, at the
// right amplitude, and nothing else in the frame is touched.
TEST(VBIFrameBuilder, RecordsLandOnTheTeletextLinesAndNowhereElse) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameBuilder builder = builder_for(format);

  std::vector<int16_t> frame;
  uint32_t data_lines = 0;
  std::string error;
  ASSERT_TRUE(
      builder.build_frame(frame_records(format), frame, data_lines, error))
      << error;

  EXPECT_EQ(data_lines, format.field_range.count() * 2u);

  const VBIOutputFrame& output = builder.output_frame();
  const auto blanking = static_cast<int16_t>(output.levels.blanking);

  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    const size_t begin = output.line_offset(line);
    const size_t length = output.line_length(line);
    const auto bounds = std::minmax_element(frame.begin() + begin,
                                            frame.begin() + begin + length);

    if (is_data_line(line)) {
      // A teletext line swings from logic 0 up to logic 1.
      EXPECT_GT(*bounds.second, output.levels.logic1 - 40) << "line " << line;
    } else {
      EXPECT_EQ(*bounds.first, blanking) << "line " << line;
      EXPECT_EQ(*bounds.second, blanking) << "line " << line;
    }
  }
}

// Only the data region of a data line is written: the rest of the line — the
// back porch ahead of the run-in and the front porch after the packet — stays
// exactly as blank as any other line (design §5.6).
TEST(VBIFrameBuilder, OnlyTheDataRegionOfADataLineIsWritten) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameBuilder builder = builder_for(format);

  std::vector<int16_t> frame;
  uint32_t data_lines = 0;
  std::string error;
  ASSERT_TRUE(
      builder.build_frame(frame_records(format), frame, data_lines, error))
      << error;

  const VBIOutputFrame& output = builder.output_frame();
  const VBIDataPlacement& placement = builder.placement();
  const auto blanking = static_cast<int16_t>(output.levels.blanking);
  const size_t line_begin = output.line_offset(kFirstField1Line);
  const size_t line_length = output.line_length(kFirstField1Line);

  for (size_t sample = 0; sample < placement.output_begin; ++sample) {
    ASSERT_EQ(frame[line_begin + sample], blanking) << "sample " << sample;
  }
  for (size_t sample = placement.output_end; sample < line_length; ++sample) {
    ASSERT_EQ(frame[line_begin + sample], blanking) << "sample " << sample;
  }

  // And the window itself sits where the standard puts the packet.
  EXPECT_LT(placement.output_begin, 190u);
  EXPECT_GT(placement.output_end, 1100u);
}

// A record with no measurable data service has no honest scale to map by, so
// its line is left as blanking rather than carrying amplified noise
// (design §5.3.4).
TEST(VBIFrameBuilder, ALineWithNoEstablishedLevelsStaysBlank) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameBuilder builder = builder_for(format);

  std::vector<int16_t> frame;
  uint32_t data_lines = 0;
  std::string error;
  ASSERT_TRUE(builder.build_frame(frame_records(format, /*carries_data=*/false),
                                  frame, data_lines, error))
      << error;

  EXPECT_EQ(data_lines, 0u);
  const auto blanking =
      static_cast<int16_t>(builder.output_frame().levels.blanking);
  const auto bounds = std::minmax_element(frame.begin(), frame.end());
  EXPECT_EQ(*bounds.first, blanking);
  EXPECT_EQ(*bounds.second, blanking);
}

// A record's logic levels map onto the standard's, whatever gain the capture
// card applied to them.
TEST(VBIFrameBuilder, LevelsAreMappedIntoTheStandardsAmplitudeDomain) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBIFrameBuilder builder = builder_for(format);
  const VBIOutputFrame& output = builder.output_frame();

  // The same lines at two very different source gains must produce the same
  // output amplitude.
  const auto peak_amplitude = [&](double logic0, double logic1) {
    const VBITeletextService service = wst_service();
    const double anchor =
        service.cri_start_samples(format.sample_rate_hz, kCaptureOffsetSamples);

    std::vector<VBILineRecord> records;
    VBILineRecord record;
    record.field_index = 0;
    record.record_index = format.field_range.start;
    SyntheticVBILine line;
    line.sample_rate_hz = format.sample_rate_hz;
    line.valid_samples = format.valid_samples;
    line.anchor_position_samples = anchor;
    line.logic0 = logic0;
    line.logic1 = logic1;
    record.samples = render_synthetic_vbi_line(line);
    records.push_back(std::move(record));

    std::vector<int16_t> frame;
    uint32_t data_lines = 0;
    std::string error;
    EXPECT_TRUE(builder.build_frame(records, frame, data_lines, error))
        << error;

    const size_t begin = output.line_offset(kFirstField1Line);
    const size_t length = output.line_length(kFirstField1Line);
    return *std::max_element(frame.begin() + begin,
                             frame.begin() + begin + length);
  };

  const int16_t quiet = peak_amplitude(40.0, 120.0);
  const int16_t loud = peak_amplitude(10.0, 240.0);

  EXPECT_NEAR(quiet, output.levels.logic1, 20);
  EXPECT_NEAR(loud, output.levels.logic1, 20);
}

// Getting the field order backwards swaps the two line ranges, which is what
// the configuration exists to control (design §6.1).
TEST(VBIFrameBuilder, TheFirstStoredFieldDecidesWhichLineRangeItLandsOn) {
  VBISourceFormat field1_first = bt8x8_pal_format();
  field1_first.first_field = 1;
  VBISourceFormat field2_first = bt8x8_pal_format();
  field2_first.first_field = 2;

  const VBIFrameBuilder first = builder_for(field1_first);
  const VBIFrameBuilder second = builder_for(field2_first);

  EXPECT_EQ(first.frame_lines(0).front(), kFirstField1Line);
  EXPECT_EQ(first.frame_lines(1).front(), kFirstField2Line);
  EXPECT_EQ(second.frame_lines(0).front(), kFirstField2Line);
  EXPECT_EQ(second.frame_lines(1).front(), kFirstField1Line);
}

// A field range longer than the standard's line list has records with nowhere
// to go, which means the configuration is wrong and is never truncated
// silently (design §5.1).
TEST(VBIFrameBuilder, AFieldRangeLongerThanTheStandardIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range.end = format.field_range.start + 20u;

  VBIFrameBuilder builder;
  std::string error;
  EXPECT_FALSE(make_vbi_frame_builder(format, VBILevelMapperConfig{},
                                      {kCaptureOffsetSamples}, builder, error));
  EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// 525-line captures
// ---------------------------------------------------------------------------

VBISourceFormat tbc_vbi_ntsc_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;
  return format;
}

VBITeletextService wst_525_service() {
  VBITeletextService service;
  std::string error;
  EXPECT_TRUE(vbi_teletext_service(VBITVSystem::kNTSC, VBITeletextSystem::kWST,
                                   service, error))
      << error;
  return service;
}

// One stored frame of a TBC-derived NTSC capture: records in the 16-bit
// decoder domain, at the levels a real one carries.
std::vector<VBILineRecord> ntsc_frame_records(const VBISourceFormat& format) {
  const VBITeletextService service = wst_525_service();
  const double anchor = service.cri_start_samples(format.sample_rate_hz, 0.0);

  std::vector<VBILineRecord> records;
  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t index = format.field_range.start;
         index <= format.field_range.end; ++index) {
      VBILineRecord record;
      record.field_index = field;
      record.record_index = index;

      SyntheticVBILine line;
      line.sample_rate_hz = format.sample_rate_hz;
      line.valid_samples = format.valid_samples;
      line.bit_rate_hz = service.bit_rate_hz;
      line.payload_bits = service.payload_bytes * 8u;
      line.anchor_position_samples = anchor;
      // The 16-bit domain: 10-bit black and the WST one level, times 64.
      line.logic0 = 282.0 * 64.0;
      line.logic1 = 624.0 * 64.0;
      line.seed = field * 32u + index + 1u;
      record.samples = render_synthetic_vbi_line(line);

      records.push_back(std::move(record));
    }
  }
  return records;
}

// The whole 525-line path: twelve records per field onto the twelve lines the
// standard defines, on a 477 750-sample frame.
TEST(VBIFrameBuilder, NTSCRecordsArePlacedOnTheStandardLines) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  VBIFrameBuilder builder;
  std::string error;
  ASSERT_TRUE(make_vbi_frame_builder(format, VBILevelMapperConfig{}, {0.0},
                                     builder, error))
      << error;

  ASSERT_EQ(builder.output_frame().samples_per_frame, 477750u);
  EXPECT_EQ(
      builder.frame_lines(0),
      (std::vector<uint32_t>{9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}));
  EXPECT_EQ(builder.frame_lines(1),
            (std::vector<uint32_t>{272, 273, 274, 275, 276, 277, 278, 279, 280,
                                   281, 282, 283}));

  std::vector<int16_t> frame;
  uint32_t data_lines = 0;
  ASSERT_TRUE(
      builder.build_frame(ntsc_frame_records(format), frame, data_lines, error))
      << error;
  EXPECT_EQ(data_lines, 24u);

  // WST puts logic 0 at black, which on NTSC sits above blanking, so a data
  // line never dips below blanking — it is written above it and leaves the
  // rest of the line untouched.
  const VBIOutputFrame& output = builder.output_frame();
  for (uint32_t line = 0; line < output.lines_per_frame; ++line) {
    const bool data =
        (line >= 9u && line <= 20u) || (line >= 272u && line <= 283u);
    const int16_t* samples = frame.data() + output.line_offset(line);
    const auto bounds =
        std::minmax_element(samples, samples + output.line_length(line));
    EXPECT_EQ(*bounds.first, output.levels.blanking) << "line " << line;
    if (data) {
      EXPECT_GT(*bounds.second, output.levels.logic0) << "line " << line;
    } else {
      EXPECT_EQ(*bounds.second, output.levels.blanking) << "line " << line;
    }
  }
}

// A time-base corrected capture's levels are absolute, so the mapping onto the
// output domain is the ld-decode scale factor and nothing is estimated: the
// configured level policy has no effect at all.
TEST(VBIFrameBuilder, NTSCTBCLevelsAreMappedAbsolutelyWhateverThePolicy) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  const std::vector<VBILineRecord> records = ntsc_frame_records(format);

  std::vector<int16_t> per_line;
  std::vector<int16_t> fixed;
  uint32_t data_lines = 0;
  std::string error;

  VBIFrameBuilder builder;
  VBILevelMapperConfig policy;
  policy.mode = VBILevelMode::kPerLine;
  ASSERT_TRUE(make_vbi_frame_builder(format, policy, {0.0}, builder, error))
      << error;
  ASSERT_TRUE(builder.build_frame(records, per_line, data_lines, error))
      << error;

  // A policy that would produce completely different levels if it were applied.
  policy.mode = VBILevelMode::kFixed;
  policy.fixed_logic0 = 0.0;
  policy.fixed_logic1 = 65535.0;
  ASSERT_TRUE(make_vbi_frame_builder(format, policy, {0.0}, builder, error))
      << error;
  ASSERT_TRUE(builder.build_frame(records, fixed, data_lines, error)) << error;

  EXPECT_EQ(per_line, fixed);

  // And the mapping really is the scale factor: every sample of the data
  // region is its record sample divided by 64, to the rounding of the output
  // word.  The capture is already on the output's lattice, so nothing is
  // interpolated or filtered on the way.
  const VBIOutputFrame& output = builder.output_frame();
  const VBIDataPlacement& placement = builder.placement();
  const int16_t* line = per_line.data() + output.line_offset(9);
  for (uint32_t index = 0; index < placement.output_count(); ++index) {
    const uint32_t at = placement.output_begin + index;
    const double source = records.front().samples[at];
    EXPECT_NEAR(static_cast<double>(line[at]), source / 64.0, 0.51)
        << "sample " << at;
  }
}

// A capture offset that puts the record outside the data region entirely is a
// wrong configuration, not a frame of blanking.
TEST(VBIFrameBuilder, AnOffsetThatPlacesNothingIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBIFrameBuilder builder;
  std::string error;
  EXPECT_FALSE(make_vbi_frame_builder(format, VBILevelMapperConfig{},
                                      {100000.0}, builder, error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace orc
