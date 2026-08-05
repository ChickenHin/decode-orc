/*
 * File:        vbi_calibration_test.cpp
 * Module:      orc-tests
 * Purpose:     Functional tests for capture-offset calibration on real media
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_cri_correlator.h"
#include "vbi_cri_template.h"
#include "vbi_line_reader.h"
#include "vbi_offset_calibration.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"
#include "vbi_transport.h"

namespace orc {
namespace {

// The reference sample: a four-hour FLAC-wrapped bt8x8 PAL capture carrying
// world system teletext. It is not checked into the repository (test-data/ is
// ignored), so every test here skips when it is absent.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

// What calibration measures on this capture: the clock run-in sits about
// 103.7 samples into every record, which puts sample 0 of a record about
// 261.6 samples after 0H.
//
// The Linux bttv driver's own comment says "the value is measured to be about
// 244" while calling the datasheet figure Just Plain Wrong and noting that the
// real value differs between chip revisions (design §5.3.3). This measurement
// is the empirical answer for this capture, and it is 18 samples — half a
// microsecond — from that folklore. It sits comfortably inside the
// vhs-teletext search window for this card, which is what says both figures
// are in the right region and the fit is not an artefact.
constexpr double kMeasuredOffsetSamples = 261.6;
constexpr double kMeasuredRunInSamples = 103.7;
constexpr double kMeasurementTolerance = 2.0;

// The design's folkloric figure, kept here so the distance between the two is
// visible in the test rather than only in a document.
constexpr double kFolkloricOffsetSamples = 244.0;

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
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

std::vector<VBILineRecord> sampled_records(const VBILineReader& reader,
                                           uint32_t frames) {
  std::vector<VBILineRecord> records;
  std::string error;
  const std::optional<uint64_t> frame_count = reader.frame_count();
  EXPECT_TRUE(frame_count.has_value());
  for (const uint64_t frame :
       vbi_calibration_frame_indices(frame_count.value_or(0), frames)) {
    VBIFrameRecords frame_records;
    EXPECT_TRUE(reader.read_frame(frame, frame_records, error)) << error;
    for (VBILineRecord& record : frame_records.lines) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

// Calibration on the real capture, sampled across the whole four hours.
TEST(VBICalibration, ReferenceCaptureConvergesOnItsMeasuredCaptureOffset) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source =
      open_vbi_byte_source(kReferenceCapture, error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  const VBILineReader reader(format, *source);

  VBICalibrationConfig config;
  config.sample_frames = 16;

  VBIOffsetCalibration calibration;
  ASSERT_TRUE(calibrate_vbi_capture_offset(reader, wst_service(), config,
                                           calibration, error))
      << error;

  // Recorded so the measurement is visible in the test log; it is the evidence
  // behind the figures asserted below.
  GTEST_LOG_(INFO) << calibration.summary;
  for (const std::string& warning : calibration.warnings) {
    GTEST_LOG_(INFO) << warning;
  }

  for (const std::string& diagnostic : calibration.diagnostics) {
    ADD_FAILURE() << diagnostic;
  }
  ASSERT_TRUE(calibration.converged) << calibration.summary;

  EXPECT_NEAR(calibration.anchor_position_samples, kMeasuredRunInSamples,
              kMeasurementTolerance)
      << calibration.summary;
  EXPECT_NEAR(calibration.capture_offset_samples, kMeasuredOffsetSamples,
              kMeasurementTolerance)
      << calibration.summary;

  // The measurement and the driver's folklore land in the same half
  // microsecond of back porch, which is the sanity bound on the whole model.
  EXPECT_LT(
      std::abs(calibration.capture_offset_samples - kFolkloricOffsetSamples),
      32.0)
      << calibration.summary;

  // A tape or off-air source played into a capture card, so several samples of
  // line-to-line scatter are expected and are not a failure (design §5.3.6).
  // What must hold is that it stays inside what this format declares.
  EXPECT_EQ(calibration.spread_class, VBIOffsetSpreadClass::kMild)
      << calibration.summary;
  EXPECT_LE(calibration.spread_samples,
            format.calibration.maximum_spread_samples);
  EXPECT_FALSE(calibration.drift_detected) << calibration.summary;

  // Teletext is carried on most of the captured lines of this sample, so the
  // acceptance rate should be high rather than merely adequate.
  EXPECT_GT(calibration.acceptance_fraction, 0.5) << calibration.summary;
}

// Per-line precision may be a sample or worse on a source like this, but the
// offset is fitted over hundreds of lines, so the global figure must be tight
// however the capture is sampled (design §5.3.6).
TEST(VBICalibration, ReferenceCaptureFitIsStableAcrossDifferentSamples) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source =
      open_vbi_byte_source(kReferenceCapture, error);
  ASSERT_NE(source, nullptr) << error;

  const VBILineReader reader(bt8x8_pal_format(), *source);

  VBICalibrationConfig narrow;
  narrow.sample_frames = 4;
  VBICalibrationConfig wide;
  wide.sample_frames = 12;

  VBIOffsetCalibration first;
  VBIOffsetCalibration second;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, wst_service(), narrow, first, error))
      << error;
  ASSERT_TRUE(
      calibrate_vbi_capture_offset(reader, wst_service(), wide, second, error))
      << error;

  ASSERT_TRUE(first.converged) << first.summary;
  ASSERT_TRUE(second.converged) << second.summary;
  EXPECT_NEAR(first.capture_offset_samples, second.capture_offset_samples, 1.0)
      << first.summary << " / " << second.summary;
}

// The lock has to be at the right bit, not merely near the right place: an
// alternating run-in correlates with itself two bits away, and a position two
// bits out would put every byte in the wrong phase (design §5.3.1).
//
// This slices the framing code at the fitted position and at the neighbouring
// bit alignments and sees which one reads back 0xE4. Only the fitted one does,
// which is the direct evidence that the framing code in the template is doing
// the job it is there for.
TEST(VBICalibration, ReferenceCaptureLocksToTheCorrectBitAlignment) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source =
      open_vbi_byte_source(kReferenceCapture, error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextService service = wst_service();
  const VBILineReader reader(format, *source);

  VBICRITemplate cri_template;
  ASSERT_TRUE(make_vbi_cri_frc_template(service, format.sample_rate_hz,
                                        VBICRITemplateConfig{}, cri_template,
                                        error))
      << error;
  const VBICRISearchWindow window = vbi_cri_search_window(format, service);
  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);

  constexpr uint32_t kFramingCode = 0xE4u;
  constexpr int kPatternBits = 24;

  int locked = 0;
  int framing_code_at_fit = 0;
  int framing_code_elsewhere = 0;
  for (const VBILineRecord& record : sampled_records(reader, 8)) {
    const VBICRIDetection detection =
        detect_vbi_cri_position(record.samples, cri_template, window,
                                format.calibration.acceptance_correlation);
    if (!detection.accepted) {
      continue;
    }
    ++locked;

    for (int shift = -2; shift <= 2; ++shift) {
      const double start = detection.anchor_position_samples +
                           static_cast<double>(shift) * samples_per_bit;

      std::vector<double> bit_centres;
      for (int bit = 0; bit < kPatternBits; ++bit) {
        const double position =
            start + (static_cast<double>(bit) + 0.5) * samples_per_bit;
        const size_t index = static_cast<size_t>(position);
        if (index + 1u >= record.samples.size()) {
          bit_centres.clear();
          break;
        }
        const double fraction = position - static_cast<double>(index);
        bit_centres.push_back(record.samples[index] * (1.0 - fraction) +
                              record.samples[index + 1u] * fraction);
      }
      if (bit_centres.size() != static_cast<size_t>(kPatternBits)) {
        continue;
      }

      const double low =
          *std::min_element(bit_centres.begin(), bit_centres.end());
      const double high =
          *std::max_element(bit_centres.begin(), bit_centres.end());
      const double slice = 0.5 * (low + high);
      uint32_t bits = 0;
      for (const double value : bit_centres) {
        bits = (bits << 1) | ((value > slice) ? 1u : 0u);
      }

      if ((bits & 0xFFu) == kFramingCode) {
        if (shift == 0) {
          ++framing_code_at_fit;
        } else {
          ++framing_code_elsewhere;
        }
      }
    }
  }

  ASSERT_GT(locked, 100);
  GTEST_LOG_(INFO) << "framing code recovered on " << framing_code_at_fit
                   << " of " << locked << " locked records";

  // A midpoint slicer is crude, so not every line reads back cleanly; what
  // matters is that the fitted alignment is the only one that ever does.
  EXPECT_GT(framing_code_at_fit, locked / 4);
  EXPECT_EQ(framing_code_elsewhere, 0);
}

}  // namespace
}  // namespace orc
