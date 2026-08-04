/*
 * File:        vbi_source_validation_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for raw VBI capture configuration validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_validation.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {
namespace {

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

// Does any reported violation name the given parameter or phrase?
bool mentions(const std::vector<std::string>& errors,
              const std::string& needle) {
  for (const std::string& error : errors) {
    if (error.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string joined(const std::vector<std::string>& errors) {
  std::string text;
  for (const std::string& error : errors) {
    text += "\n  " + error;
  }
  return text;
}

TEST(VBISourceValidation, ReferenceCaptureConfigurationIsAccepted) {
  const VBISourceFormat format = bt8x8_pal_format();
  constexpr uint64_t kReferenceRawBytes = 24117706752ull;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, kReferenceRawBytes);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

TEST(VBISourceValidation, StreamLengthMustFactoriseIntoWholeFrames) {
  const VBISourceFormat format = bt8x8_pal_format();

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, 65536ull * 10 + 7);

  EXPECT_TRUE(mentions(errors, "not an exact multiple")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "65536")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "7 bytes")) << joined(errors);
}

TEST(VBISourceValidation, EmptyCaptureIsRejected) {
  const VBISourceFormat format = bt8x8_pal_format();

  const std::vector<std::string> errors = validate_vbi_source_config(format, 0);

  EXPECT_TRUE(mentions(errors, "empty")) << joined(errors);
}

TEST(VBISourceValidation, UnknownStreamLengthSkipsTheFactorisationCheck) {
  const VBISourceFormat format = bt8x8_pal_format();

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(errors.empty()) << joined(errors);
}

// The excess records would have no frame line to map to, so this is an error
// rather than a truncation.
TEST(VBISourceValidation, FieldRangeLongerThanTheStandardLineListIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_lines = 20;
  format.field_range = VBIFieldRange{0, 17};  // 18 records; WST allows 16

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.field_range")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "16")) << joined(errors);
}

TEST(VBISourceValidation, FieldRangeBeyondTheStoredRecordsIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{4, 19};  // field only stores 16 records

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.field_lines")) << joined(errors);
}

TEST(VBISourceValidation, InvertedFieldRangeIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{10, 4};

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "inverted")) << joined(errors);
}

TEST(VBISourceValidation, ValidSamplesBeyondTheRecordStrideIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.valid_samples = 2100;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.valid_samples")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.line_length")) << joined(errors);
}

// The trailer overlaps the final record's padding rather than extending the
// frame, so it can never be larger than that padding.
TEST(VBISourceValidation, FrameTrailerLargerThanTheRecordPaddingIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 8;  // only four padding bytes exist

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.frame_trailer_bytes"))
      << joined(errors);
}

TEST(VBISourceValidation, FrameCounterTrailerMustBeWideEnoughToHoldIt) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_bytes = 2;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "4-byte frame sequence number"))
      << joined(errors);
}

// Sample 0 of a TBC-derived record is 0H by construction, so the rule is
// enforced now even though those formats are not yet readable.
TEST(VBISourceValidation, TBCDerivedSourceMustNotCalibrateItsCaptureOffset) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = true;
  format.capture_offset_samples = 244.0;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "calibration.capture_offset")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "'auto'")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "0H")) << joined(errors);
}

TEST(VBISourceValidation, TBCDerivedSourceWithAZeroOffsetIsAccepted) {
  VBISourceFormat format = bt8x8_pal_format();
  format.family = VBISourceFamily::kTBCDerived;
  format.capture_offset_is_auto = false;
  format.capture_offset_samples = 0.0;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_FALSE(mentions(errors, "calibration.capture_offset"))
      << joined(errors);
}

TEST(VBISourceValidation, CardCaptureMayCalibrateItsCaptureOffset) {
  VBISourceFormat format = bt8x8_pal_format();
  ASSERT_EQ(format.family, VBISourceFamily::kCardCapture);
  ASSERT_TRUE(format.capture_offset_is_auto);

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_FALSE(mentions(errors, "calibration.capture_offset"))
      << joined(errors);
}

TEST(VBISourceValidation, UnsupportedSystemPairingIsRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.tt_system = VBITeletextSystem::kNABTS;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "teletext.system")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "NABTS")) << joined(errors);
}

// An unconfigured "custom" container must name every field the user has to
// fill in, not just the first one.
TEST(VBISourceValidation, UnconfiguredCustomContainerNamesEveryMissingField) {
  VBISourceFormat format;
  std::string error;
  ASSERT_TRUE(expand_vbi_source_preset("custom", format, error)) << error;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt);

  EXPECT_TRUE(mentions(errors, "container.sample_rate")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.line_length")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "container.field_lines")) << joined(errors);
}

TEST(VBISourceValidation, TransportBitsPerSampleMustMatchTheConfiguredFormat) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBITransportHints hints;
  hints.bits_per_sample = 16;

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, std::nullopt, hints);

  EXPECT_TRUE(mentions(errors, "container.sample_format")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "u8")) << joined(errors);
  EXPECT_TRUE(mentions(errors, "16 bits")) << joined(errors);
}

// A FLAC wrapper's sample rate is a conventional placeholder and carries no
// VBI timing whatsoever, so it must never affect the container descriptor.
TEST(VBISourceValidation, TransportSampleRateIsIgnoredEntirely) {
  const VBISourceFormat format = bt8x8_pal_format();

  VBITransportHints hints;
  hints.bits_per_sample = 8;
  hints.declared_sample_rate_hz = 48000;  // the community placeholder

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, 24117706752ull, hints);

  EXPECT_TRUE(errors.empty()) << joined(errors);
  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 35468950.0);
}

}  // namespace
}  // namespace orc
