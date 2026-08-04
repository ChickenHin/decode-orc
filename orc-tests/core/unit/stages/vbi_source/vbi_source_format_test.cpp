/*
 * File:        vbi_source_format_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the raw VBI container descriptor and presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_format.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace orc {
namespace {

// The bt8x8 PAL preset must expand to the values in the design's known-format
// table, field by field. These are the only numbers the reader ever sees, so
// a wrong entry here is a silently wrong decode everywhere downstream.
TEST(VBISourceFormat, Bt8x8PALPresetExpandsToTheDocumentedContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  EXPECT_TRUE(error.empty());

  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 35468950.0);
  EXPECT_EQ(format.line_length, 2048u);
  EXPECT_EQ(format.valid_samples, 2044u);
  EXPECT_EQ(format.sample_format, VBISampleFormat::kU8);
  EXPECT_EQ(format.field_lines, 16u);
  EXPECT_EQ(format.field_range.start, 0u);
  EXPECT_EQ(format.field_range.end, 15u);
  EXPECT_EQ(format.frame_trailer_bytes, 4u);
  EXPECT_TRUE(format.frame_trailer_is_counter);
  EXPECT_EQ(format.first_field, 1u);
  EXPECT_EQ(format.tv_system, VBITVSystem::kPAL);
  EXPECT_EQ(format.tt_system, VBITeletextSystem::kWST);
  EXPECT_EQ(format.family, VBISourceFamily::kCardCapture);
}

// The bt8x8 capture offset is explicitly unreliable hardware folklore, so the
// preset carries it as a calibration starting point rather than a value to be
// used as configured.
TEST(VBISourceFormat, Bt8x8PALPresetCalibratesItsCaptureOffset) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;

  EXPECT_TRUE(format.capture_offset_is_auto);
  EXPECT_DOUBLE_EQ(format.capture_offset_samples, 244.0);
}

// What the calibration should expect of a source is a property of the source
// format, not a global constant: a cleanly time-base corrected capture locks
// to a fraction of a sample, where material played off tape into a capture
// card legitimately scatters by several (design §5.3.4, §5.3.6).
TEST(VBISourceFormat, Bt8x8PALPresetCarriesCleanSourceCalibrationThresholds) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;

  EXPECT_DOUBLE_EQ(format.calibration.tight_spread_samples, 0.5);

  // A bt8x8 card is how tape and off-air material is captured, so the preset
  // has to tolerate the line-to-line scatter such a source really shows while
  // still classifying it as jitter rather than as clean.
  EXPECT_DOUBLE_EQ(format.calibration.maximum_spread_samples, 8.0);
  EXPECT_DOUBLE_EQ(format.calibration.maximum_drift_samples, 8.0);

  // The search has to reach the whole span the vhs-teletext windows in
  // design §5.3.3 imply for this card, without straying into the data.
  EXPECT_DOUBLE_EQ(format.calibration.search_tolerance_samples, 48.0);
  EXPECT_GT(format.calibration.acceptance_correlation, 0.0);
  EXPECT_LT(format.calibration.acceptance_correlation, 1.0);
  EXPECT_GT(format.calibration.minimum_acceptance_fraction, 0.0);
}

// A bt8x8 PAL frame is 65 536 bytes: sixteen 2048-byte records in each of two
// fields, with the frame counter inside the final record's padding rather
// than appended to the frame.
TEST(VBISourceFormat, Bt8x8PALFrameGeometryDerivesTheDocumentedByteCounts) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;

  EXPECT_EQ(format.bytes_per_sample(), 1u);
  EXPECT_EQ(format.bytes_per_record(), 2048u);
  EXPECT_EQ(format.bytes_per_field(), 32768u);
  EXPECT_EQ(format.bytes_per_frame(), 65536u);
  EXPECT_EQ(format.record_padding_bytes(), 4u);
  EXPECT_GE(format.record_padding_bytes(), format.frame_trailer_bytes);
}

// The reference sample is 24 117 706 752 raw bytes, which must factorise into
// exactly 368 007 whole bt8x8 PAL frames.
TEST(VBISourceFormat, Bt8x8PALFrameSizeFactorisesTheReferenceCaptureLength) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;

  constexpr uint64_t kReferenceRawBytes = 24117706752ull;
  EXPECT_EQ(kReferenceRawBytes % format.bytes_per_frame(), 0u);
  EXPECT_EQ(kReferenceRawBytes / format.bytes_per_frame(), 368007u);
}

// "custom" leaves the geometry unset so an incompletely specified container
// is rejected by validation instead of quietly behaving like some other
// format.
TEST(VBISourceFormat, CustomPresetExpandsToAnUnconfiguredContainer) {
  VBISourceFormat format;
  std::string error;

  ASSERT_TRUE(expand_vbi_source_preset("custom", format, error)) << error;

  EXPECT_EQ(format.line_length, 0u);
  EXPECT_EQ(format.field_lines, 0u);
  EXPECT_EQ(format.valid_samples, 0u);
  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 0.0);
  EXPECT_EQ(format.bytes_per_frame(), 0u);
}

TEST(VBISourceFormat, UnknownPresetIsRejectedAndListsTheKnownNames) {
  VBISourceFormat format;
  std::string error;

  EXPECT_FALSE(expand_vbi_source_preset("bt8x8-secam", format, error));
  EXPECT_NE(error.find("bt8x8-secam"), std::string::npos);
  EXPECT_NE(error.find("bt8x8-pal"), std::string::npos);
}

TEST(VBISourceFormat, PresetNamesCoverEveryImplementedPresetAndCustom) {
  const std::vector<std::string> names = vbi_source_preset_names();

  EXPECT_NE(std::find(names.begin(), names.end(), "bt8x8-pal"), names.end());
  ASSERT_FALSE(names.empty());
  EXPECT_EQ(names.back(), "custom");

  // Every advertised name must expand.
  for (const std::string& name : names) {
    VBISourceFormat format;
    std::string error;
    EXPECT_TRUE(expand_vbi_source_preset(name, format, error)) << name;
  }
}

// The standard's teletext line lists (design §5.1) bound how many records a
// source may usefully carry per field.
TEST(VBISourceFormat, StandardTeletextLineCountsMatchTheSystemDefinitions) {
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPAL,
                                              VBITeletextSystem::kWST),
            16u);
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kNABTS),
            12u);
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPALM,
                                              VBITeletextSystem::kNABTS),
            12u);
}

// A pairing with no defined line list reports zero rather than guessing one.
TEST(VBISourceFormat, MismatchedSystemPairingHasNoDefinedLineList) {
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kNTSC,
                                              VBITeletextSystem::kWST),
            0u);
  EXPECT_EQ(standard_teletext_lines_per_field(VBITVSystem::kPAL,
                                              VBITeletextSystem::kNABTS),
            0u);
}

TEST(VBISourceFormat, SampleFormatNamesRoundTrip) {
  for (const VBISampleFormat sample_format :
       {VBISampleFormat::kU8, VBISampleFormat::kU16LE,
        VBISampleFormat::kS16LE}) {
    VBISampleFormat parsed = VBISampleFormat::kU8;
    ASSERT_TRUE(parse_vbi_sample_format(to_string(sample_format), parsed));
    EXPECT_EQ(parsed, sample_format);
  }

  VBISampleFormat unused = VBISampleFormat::kU8;
  EXPECT_FALSE(parse_vbi_sample_format("f32", unused));
}

TEST(VBISourceFormat, SixteenBitContainersMeasureTwoBytesPerSample) {
  VBISourceFormat format;
  format.line_length = 1135;
  format.valid_samples = 1135;
  format.field_lines = 16;
  format.sample_format = VBISampleFormat::kU16LE;

  EXPECT_EQ(format.bytes_per_sample(), 2u);
  EXPECT_EQ(format.bytes_per_record(), 2270u);
  EXPECT_EQ(format.bytes_per_frame(), 72640u);
  EXPECT_EQ(format.record_padding_bytes(), 0u);
}

}  // namespace
}  // namespace orc
