/*
 * File:        vbi_secam_source_test.cpp
 * Module:      orc-tests
 * Purpose:     Functional tests for the SECAM bt8x8 capture format on real
 * media
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_line_reader.h"
#include "vbi_offset_calibration.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"
#include "vbi_transport.h"

namespace orc {
namespace {

// The reference SECAM sample: a thirty-hour FLAC-wrapped bt8x8 dump of a
// Russian SECAM VHS tape carrying 42-byte World System Teletext. It is not
// checked into the repository (test-data/ is ignored), so every test here
// skips when it is absent.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/secam sample/secam_wst_russian.flac";

// What the container has to factorise into. The SECAM entry is the PAL one
// byte for byte, and this capture is an exact multiple of that frame size,
// which is the first thing that would break if the driver's SECAM norm used a
// different vbipack or sampling clock than its PAL one.
constexpr uint64_t kReferenceRawBytes = 5287247872ull;
constexpr uint64_t kReferenceFrames = 80677ull;

// What calibration measures on this capture: the clock run-in sits about
// 124.6 samples into every record, putting sample 0 of a record about
// 240.7 samples after 0H. That is within four samples of the driver's own
// folklore figure of 244, and twenty-one from what the PAL reference capture
// measures — which is the chip-revision spread design §5.3.3 warns about, and
// well inside the search window either way.
constexpr double kMeasuredOffsetSamples = 240.7;
constexpr double kMeasuredRunInSamples = 124.6;
constexpr double kMeasurementTolerance = 2.0;

// Teletext is on three records of each stored field, so the run-in is found on
// about a ninth of the records the calibrator looks at. The lines are there:
// records 1-8 carry the SECAM vertical colour identification signal, records
// 9-11 a VITS and a white bar, and records 12-14 the teletext.
constexpr double kMeasuredAcceptanceFraction = 0.111;
constexpr double kAcceptanceTolerance = 0.03;

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

VBISourceFormat preset(const char* name) {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset(name, format, error)) << error;
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

std::unique_ptr<IVBIByteSource> open_reference(std::string& error) {
  return open_vbi_byte_source(kReferenceCapture, error);
}

TEST(VBISECAMSource, ReferenceCaptureFactorisesIntoWholePALFrames) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const std::optional<uint64_t> size = source->size_bytes();
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(*size, kReferenceRawBytes);

  const VBISourceFormat format =
      preset("bt8x8 card dump, 8-bit (WST, SECAM source)");
  EXPECT_EQ(*size % format.bytes_per_frame(), 0u);
  EXPECT_EQ(*size / format.bytes_per_frame(), kReferenceFrames);
}

// The fit itself, which is healthy by every measure the stage has: it lands on
// a repeatable position with well under a sample of spread and no drift. The
// only thing unusual about it is how few of the records it locked on.
TEST(VBISECAMSource, ReferenceCaptureConvergesOnItsMeasuredCaptureOffset) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format =
      preset("bt8x8 card dump, 8-bit (WST, SECAM source)");
  VBILineReader reader(format, *source);

  VBIOffsetCalibration calibration;
  ASSERT_TRUE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error))
      << error;

  EXPECT_TRUE(calibration.converged) << calibration.summary;
  EXPECT_NEAR(calibration.anchor_position_samples, kMeasuredRunInSamples,
              kMeasurementTolerance)
      << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, kMeasuredOffsetSamples,
              kMeasurementTolerance)
      << calibration.summary;

  // Tape played into a capture card, so several samples of scatter is expected
  // and only the format's own limit has to hold.
  EXPECT_LE(calibration.spread_samples,
            format.calibration.maximum_spread_samples)
      << calibration.summary;
  EXPECT_FALSE(calibration.drift_detected) << calibration.summary;

  EXPECT_NEAR(calibration.acceptance_fraction, kMeasuredAcceptanceFraction,
              kAcceptanceTolerance)
      << calibration.summary;
}

// Why the SECAM entry exists at all. The same capture, the same bytes and the
// same fit, judged by the PAL entry's expectation of how many lines carry
// teletext, is rejected — and rejected on that count alone, which is what the
// diagnostic has to say so the user knows which entry to pick.
TEST(VBISECAMSource, ThePALPresetRejectsTheSameFitOnItsLineCountAlone) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat pal_format = preset("bt8x8 card dump, 8-bit (WST)");
  VBILineReader reader(pal_format, *source);

  VBIOffsetCalibration calibration;
  ASSERT_TRUE(calibrate_vbi_capture_offset(
      reader, wst_service(), VBICalibrationConfig{}, calibration, error))
      << error;

  EXPECT_FALSE(calibration.converged) << calibration.summary;
  ASSERT_EQ(calibration.diagnostics.size(), 1u) << calibration.summary;
  EXPECT_NE(calibration.diagnostics.front().find("of the sampled records"),
            std::string::npos)
      << calibration.diagnostics.front();

  // The position it rejects is the same one the SECAM entry accepts: the fit
  // was never in question, only the number of records behind it.
  EXPECT_NEAR(calibration.anchor_position_samples, kMeasuredRunInSamples,
              kMeasurementTolerance)
      << calibration.summary;
}

}  // namespace
}  // namespace orc
