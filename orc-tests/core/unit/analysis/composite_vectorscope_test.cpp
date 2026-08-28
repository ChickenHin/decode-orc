/*
 * File:        composite_vectorscope_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for composite-carrier vectorscope demodulation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/core/analysis/vectorscope/composite_vectorscope.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>

#include <cmath>
#include <map>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr double kTwoPi = 2.0 * M_PI;
constexpr double kDisplayFullScale = 32767.0;

// ITU-R BT.470-6 §1.1.2 / EBU Tech. 3280-E §2.1 modulation factors.
constexpr double kU = 0.492111;
constexpr double kV = 0.877283;

struct Uv {
  double u;
  double v;
};

// Normalised linear R'G'B' (0..1) → the U/V a composite encoder modulates.
Uv rgb_to_uv(double red, double green, double blue) {
  const double y = (0.299 * red) + (0.587 * green) + (0.114 * blue);
  return {kU * (blue - y), kV * (red - y)};
}

orc::SourceParameters pal_parameters() {
  orc::SourceParameters parameters;
  parameters.system = orc::VideoSystem::PAL;
  parameters.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  parameters.frame_height = orc::kPalFrameLines;
  parameters.blanking_level = orc::kPalBlanking;
  parameters.black_level = orc::kPalBlack;
  parameters.white_level = orc::kPalWhite;
  parameters.active_video_start = orc::kPalActiveVideoStart;
  parameters.active_video_end = orc::kPalActiveVideoEnd;
  parameters.first_active_frame_line = orc::kPalFirstActiveFrameLine;
  parameters.last_active_frame_line = orc::kPalLastActiveFrameLine;
  return parameters;
}

orc::SourceParameters ntsc_parameters() {
  orc::SourceParameters parameters;
  parameters.system = orc::VideoSystem::NTSC;
  parameters.frame_width_nominal = orc::kNtscSamplesPerLine;
  parameters.frame_height = orc::kNtscFrameLines;
  parameters.blanking_level = orc::kNtscBlanking;
  parameters.white_level = orc::kNtscWhite;
  parameters.active_video_start = orc::kNtscActiveVideoStart;
  parameters.active_video_end = orc::kNtscActiveVideoEnd;
  parameters.first_active_frame_line = orc::kNtscFirstActiveFrameLine;
  parameters.last_active_frame_line = orc::kNtscLastActiveFrameLine;
  return parameters;
}

// Where a synthesised line places the subcarrier at its first sample.
enum class LineGrid {
  // The subcarrier advances by the nominal cycles per line (PAL 283.7516), so
  // the line-start phase creeps against the sample grid.  This is what an
  // ld-decode 4FSC TBC does, and it is what the demodulator predicts.
  Nominal,
  // Every line starts a whole number of samples after the last, so the
  // line-start phase is always a multiple of 90°.  A file laid out this way
  // walks away from the nominal prediction by 0.0016 cycles — about 0.58° — a
  // line, which is a full turn over a frame.
  IntegerSampleGrid,
};

// Synthesise a 4FSC frame carrying colour burst on every line and a flat
// |bar| patch across the active window.
//
// With LineGrid::Nominal the subcarrier reference matches the one the
// demodulator predicts, so a correct acquisition recovers exactly the
// modulating U and V (with V inverted on alternate lines for PAL — the
// V-switch is deliberately not undone).
std::vector<int16_t> synthesise_frame(const orc::SourceParameters& parameters,
                                      const Uv& bar, double bar_luma,
                                      bool switched_v,
                                      LineGrid grid = LineGrid::Nominal) {
  const orc::VideoSystem system = parameters.system;
  const size_t spl = static_cast<size_t>(parameters.frame_width_nominal);
  const size_t lines = static_cast<size_t>(parameters.frame_height);
  const size_t total = orc::frame_line_sample_offset(system, spl, lines);
  std::vector<int16_t> frame(total,
                             static_cast<int16_t>(parameters.blanking_level));

  const double range =
      static_cast<double>(parameters.white_level - parameters.blanking_level);
  const double cycles_per_line = orc::subcarrier_cycles_per_line(system);
  const double burst_amplitude =
      orc::nominal_burst_amplitude_ire(system) / 100.0;
  // The burst sits at 135°/225° for PAL and 180° for NTSC, referenced to +U.
  const double burst_u =
      switched_v ? (-burst_amplitude / std::sqrt(2.0)) : -burst_amplitude;
  const double burst_v = switched_v ? (burst_amplitude / std::sqrt(2.0)) : 0.0;

  const auto [burst_start, burst_end] = orc::colour_burst_range(system);
  const size_t active_start =
      static_cast<size_t>(parameters.active_video_start);
  const size_t active_end = static_cast<size_t>(parameters.active_video_end);
  const size_t first_active_line =
      static_cast<size_t>(parameters.first_active_frame_line / 2);
  const size_t last_active_line =
      static_cast<size_t>(parameters.last_active_frame_line / 2);
  const size_t f1 = orc::field1_lines(system);

  for (size_t line = 0; line < lines; ++line) {
    const size_t offset = orc::frame_line_sample_offset(system, spl, line);
    const size_t length = orc::frame_line_sample_count(system, spl, line);
    const double base = (grid == LineGrid::Nominal)
                            ? (cycles_per_line * static_cast<double>(line))
                            : (static_cast<double>(offset) * 0.25);
    const double frac = base - std::floor(base);
    const double v_sign = (!switched_v || (line % 2) == 0) ? 1.0 : -1.0;

    const size_t field_line = (line < f1) ? line : (line - f1);
    const bool in_active_lines =
        field_line >= first_active_line && field_line < last_active_line;

    for (size_t i = 0; i < length; ++i) {
      const double theta = kTwoPi * (frac + (0.25 * static_cast<double>(i)));
      double luma = 0.0;
      double u = 0.0;
      double v = 0.0;

      if (i >= static_cast<size_t>(burst_start) &&
          i < static_cast<size_t>(burst_end)) {
        u = burst_u;
        v = burst_v * v_sign;
      } else if (in_active_lines && i >= active_start && i < active_end) {
        luma = bar_luma;
        u = bar.u;
        v = bar.v * v_sign;
      }

      const double signal =
          static_cast<double>(parameters.blanking_level) + (luma * range) +
          (((u * std::sin(theta)) + (v * std::cos(theta))) * range);
      frame[offset + i] = static_cast<int16_t>(std::lround(signal));
    }
  }

  return frame;
}

// Mean U/V of every sample of |sample_class| on lines with |phase|.
Uv mean_of(const orc::VectorscopeData& data,
           orc::VectorscopeSampleClass sample_class,
           orc::VectorscopeLinePhase phase, size_t* count_out = nullptr) {
  double sum_u = 0.0;
  double sum_v = 0.0;
  size_t count = 0;
  for (const auto& sample : data.samples) {
    if (sample.sample_class != sample_class) continue;
    if (sample.line_phase != phase) continue;
    sum_u += sample.u;
    sum_v += sample.v;
    ++count;
  }
  if (count_out != nullptr) *count_out = count;
  if (count == 0) return {0.0, 0.0};
  return {sum_u / static_cast<double>(count),
          sum_v / static_cast<double>(count)};
}

}  // namespace

TEST(CompositeVectorscopeTest, SubcarrierCyclesPerLine_MatchesSystemGeometry) {
  // EBU Tech. 3280-E §1.1: PAL subcarrier is 283.7516 times line frequency.
  EXPECT_NEAR(orc::subcarrier_cycles_per_line(orc::VideoSystem::PAL), 283.7516,
              1e-4);
  // SMPTE 244M-2003 §4.1: NTSC subcarrier is 227.5 times line frequency.
  EXPECT_DOUBLE_EQ(orc::subcarrier_cycles_per_line(orc::VideoSystem::NTSC),
                   227.5);
  EXPECT_DOUBLE_EQ(orc::subcarrier_cycles_per_line(orc::VideoSystem::PAL_M),
                   227.25);
  EXPECT_DOUBLE_EQ(orc::subcarrier_cycles_per_line(orc::VideoSystem::Unknown),
                   0.0);
}

TEST(CompositeVectorscopeTest, NominalBurstAmplitude_MatchesSpecLevels) {
  // EBU Tech. 3280-E §1.2: 300 mV p-p burst on a 700 mV luminance range.
  EXPECT_NEAR(orc::nominal_burst_amplitude_ire(orc::VideoSystem::PAL),
              100.0 * 150.0 / 700.0, 1e-9);
  // SMPTE 170M-2004 §8.4: 40 IRE p-p burst.
  EXPECT_DOUBLE_EQ(orc::nominal_burst_amplitude_ire(orc::VideoSystem::NTSC),
                   20.0);
}

TEST(CompositeVectorscopeTest, Pal_RecoversBothLinePhasesWithoutVSwitchFix) {
  const orc::SourceParameters parameters = pal_parameters();
  // 75 % red bar: the classic vectorscope target.
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::ActiveLine;
  // Lines wholly inside field 1's active picture, so every sampled line
  // carries the bar rather than blanking.
  options.first_line = 100;
  options.last_line = 200;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 7, options);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(data->acquisition_mode,
            orc::VectorscopeAcquisitionMode::CompositeCarrier);
  EXPECT_EQ(data->system, orc::VideoSystem::PAL);
  EXPECT_EQ(data->field_number, 7u);

  size_t positive_count = 0;
  size_t negative_count = 0;
  const Uv positive =
      mean_of(*data, orc::VectorscopeSampleClass::Picture,
              orc::VectorscopeLinePhase::VPositive, &positive_count);
  const Uv negative =
      mean_of(*data, orc::VectorscopeSampleClass::Picture,
              orc::VectorscopeLinePhase::VNegative, &negative_count);

  ASSERT_GT(positive_count, 0u);
  ASSERT_GT(negative_count, 0u);

  const double expected_u = bar.u * kDisplayFullScale;
  const double expected_v = bar.v * kDisplayFullScale;

  // Tolerance covers the 10-bit quantisation of the synthetic signal and the
  // product detector's one-cycle filter transient at the two ends of the
  // active window, which pulls a handful of the 948 samples per line short.
  EXPECT_NEAR(positive.u, expected_u, 150.0);
  EXPECT_NEAR(positive.v, expected_v, 150.0);

  // The V-switch is deliberately not undone: the −V lines plot as the mirror
  // image of the +V lines about the U axis (ITU-R BT.470-6 Table 2 item 2.16).
  EXPECT_NEAR(negative.u, expected_u, 150.0);
  EXPECT_NEAR(negative.v, -expected_v, 150.0);
}

TEST(CompositeVectorscopeTest,
     Pal_TracksALineStartPhaseTheModelDoesNotPredict) {
  // The demodulator predicts each line's start phase from the nominal
  // subcarrier-cycles-per-line, but the prediction is not the truth: a file
  // whose lines sit on a plain integer-sample grid drifts away from it by
  // about 0.58° a line, a full turn over a frame.  The reference is tracked
  // from the burst so that drift is followed rather than plotted, otherwise
  // every line's chroma is rotated by a different angle and a frame of colour
  // bars smears into arcs at the bar radii.
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true,
                       LineGrid::IntegerSampleGrid);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::ActiveLine;
  options.first_line = 100;
  options.last_line = 200;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  ASSERT_TRUE(data->measurements.valid);

  // A synthetic signal has no phase noise of its own, so whatever the jitter
  // readout shows here is the tracker failing to follow the drift.
  EXPECT_LT(data->measurements.burst_phase_jitter_degrees, 2.0);
  EXPECT_NEAR(data->measurements.burst_phase_split_error_degrees, 0.0, 1.0);

  size_t positive_count = 0;
  size_t negative_count = 0;
  const Uv positive =
      mean_of(*data, orc::VectorscopeSampleClass::Picture,
              orc::VectorscopeLinePhase::VPositive, &positive_count);
  const Uv negative =
      mean_of(*data, orc::VectorscopeSampleClass::Picture,
              orc::VectorscopeLinePhase::VNegative, &negative_count);
  ASSERT_GT(positive_count, 0u);
  ASSERT_GT(negative_count, 0u);

  const double expected_u = bar.u * kDisplayFullScale;
  const double expected_v = bar.v * kDisplayFullScale;
  EXPECT_NEAR(positive.u, expected_u, 150.0);
  EXPECT_NEAR(positive.v, expected_v, 150.0);
  EXPECT_NEAR(negative.u, expected_u, 150.0);
  EXPECT_NEAR(negative.v, -expected_v, 150.0);

  // Every line's bar lands on the target, rather than the lines spreading into
  // an arc through it.  Sample means are taken per line because the band-pass
  // ramps in and out at the ends of the active window on every line alike;
  // what a drifting reference does instead is rotate one line against the
  // next, which a per-line mean shows directly.
  std::map<uint16_t, Uv> line_sum;
  std::map<uint16_t, size_t> line_count;
  for (const auto& sample : data->samples) {
    if (sample.sample_class != orc::VectorscopeSampleClass::Picture) continue;
    Uv& sum = line_sum[sample.line_number];
    sum.u += sample.u;
    sum.v += sample.v;
    ++line_count[sample.line_number];
  }
  ASSERT_GE(line_sum.size(), 100u);

  double worst = 0.0;
  for (const auto& [line, sum] : line_sum) {
    const double n = static_cast<double>(line_count[line]);
    // The V-switch mirrors the target about the U axis on alternate lines.
    const double reference_v = ((line % 2) == 0) ? expected_v : -expected_v;
    worst = std::max(
        worst, std::hypot((sum.u / n) - expected_u, (sum.v / n) - reference_v));
  }
  EXPECT_LT(worst, 0.05 * std::hypot(expected_u, expected_v));
}

TEST(CompositeVectorscopeTest, Pal_BurstPlotsAtPlusMinus135Degrees) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::BurstOnly;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  ASSERT_FALSE(data->samples.empty());

  // A burst-only window contains burst samples and nothing else.
  for (const auto& sample : data->samples) {
    EXPECT_EQ(sample.sample_class, orc::VectorscopeSampleClass::Burst);
  }

  size_t positive_count = 0;
  size_t negative_count = 0;
  const Uv positive =
      mean_of(*data, orc::VectorscopeSampleClass::Burst,
              orc::VectorscopeLinePhase::VPositive, &positive_count);
  const Uv negative =
      mean_of(*data, orc::VectorscopeSampleClass::Burst,
              orc::VectorscopeLinePhase::VNegative, &negative_count);
  ASSERT_GT(positive_count, 0u);
  ASSERT_GT(negative_count, 0u);

  const double positive_degrees =
      std::atan2(positive.v, positive.u) * 180.0 / M_PI;
  const double negative_degrees =
      std::atan2(negative.v, negative.u) * 180.0 / M_PI;
  EXPECT_NEAR(positive_degrees, 135.0, 2.0);
  EXPECT_NEAR(negative_degrees, -135.0, 2.0);

  const double expected_magnitude =
      (orc::nominal_burst_amplitude_ire(orc::VideoSystem::PAL) / 100.0) *
      kDisplayFullScale;

  // The plotted burst runs the chroma band-pass in and out at each end of the
  // window, so the mean over every emitted sample sits a few per cent inside
  // the burst's own amplitude — the ramp is part of the trace and belongs on
  // screen.  The instrument reading trims it and is exact.
  EXPECT_NEAR(std::hypot(positive.u, positive.v), expected_magnitude,
              expected_magnitude * 0.1);
  ASSERT_TRUE(data->measurements.valid);
  EXPECT_NEAR(data->measurements.burst_amplitude_ire,
              orc::nominal_burst_amplitude_ire(orc::VideoSystem::PAL), 0.5);
}

TEST(CompositeVectorscopeTest, Pal_LocksWithUnequalVSwitchLineCounts) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  // Wipe the burst off a run of same-V-switch lines, as a damaged tape would.
  // The two states no longer appear equally often, so a plain average of the
  // raw burst vectors would drag the reference off the −U axis.
  const size_t spl = static_cast<size_t>(parameters.frame_width_nominal);
  const auto [burst_start, burst_end] =
      orc::colour_burst_range(parameters.system);
  for (size_t line = 60; line <= 160; line += 2) {
    const size_t offset =
        orc::frame_line_sample_offset(parameters.system, spl, line);
    for (int32_t i = burst_start; i < burst_end; ++i) {
      frame[offset + static_cast<size_t>(i)] =
          static_cast<int16_t>(parameters.blanking_level);
    }
  }

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::BurstOnly;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  ASSERT_TRUE(data->measurements.valid);

  size_t positive_count = 0;
  size_t negative_count = 0;
  const Uv positive =
      mean_of(*data, orc::VectorscopeSampleClass::Burst,
              orc::VectorscopeLinePhase::VPositive, &positive_count);
  const Uv negative =
      mean_of(*data, orc::VectorscopeSampleClass::Burst,
              orc::VectorscopeLinePhase::VNegative, &negative_count);
  ASSERT_GT(positive_count, 0u);
  ASSERT_GT(negative_count, 0u);
  EXPECT_NE(positive_count, negative_count);

  EXPECT_NEAR(std::atan2(positive.v, positive.u) * 180.0 / M_PI, 135.0, 2.0);
  EXPECT_NEAR(std::atan2(negative.v, negative.u) * 180.0 / M_PI, -135.0, 2.0);
  EXPECT_NEAR(data->measurements.burst_phase_split_error_degrees, 0.0, 1.0);
}

TEST(CompositeVectorscopeTest, Pal_ReportsBurstMeasurements) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  const auto data =
      orc::extract_composite_vectorscope(frame.data(), frame.size(), parameters,
                                         0, orc::CompositeVectorscopeOptions{});
  ASSERT_TRUE(data.has_value());

  const orc::VectorscopeMeasurements& m = data->measurements;
  ASSERT_TRUE(m.valid);
  EXPECT_GT(m.burst_line_count, 100u);
  EXPECT_NEAR(m.burst_amplitude_ire,
              orc::nominal_burst_amplitude_ire(orc::VideoSystem::PAL), 0.5);
  EXPECT_NEAR(m.burst_amplitude_percent, 100.0, 3.0);
  // A synthetic burst has no jitter and an exact 90° V-switch split.
  EXPECT_LT(m.burst_phase_jitter_degrees, 1.0);
  EXPECT_NEAR(m.burst_phase_split_error_degrees, 0.0, 1.0);
  EXPECT_GT(m.chroma_to_burst_ratio, 0.0);
}

TEST(CompositeVectorscopeTest, Ntsc_SingleBurstPhaseAndNoLinePhase) {
  const orc::SourceParameters parameters = ntsc_parameters();
  const Uv bar = rgb_to_uv(0.0, 0.0, 0.75);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.114 * 0.75, /*switched_v=*/false);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::BurstOnly;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  ASSERT_FALSE(data->samples.empty());

  for (const auto& sample : data->samples) {
    EXPECT_EQ(sample.line_phase, orc::VectorscopeLinePhase::NotApplicable);
  }

  size_t count = 0;
  const Uv burst = mean_of(*data, orc::VectorscopeSampleClass::Burst,
                           orc::VectorscopeLinePhase::NotApplicable, &count);
  ASSERT_GT(count, 0u);

  // SMPTE 170M-2004 §8.4: the NTSC burst sits on the −U axis, i.e. 180°.
  EXPECT_NEAR(std::abs(std::atan2(burst.v, burst.u) * 180.0 / M_PI), 180.0,
              2.0);
}

TEST(CompositeVectorscopeTest, SharpLumaEdge_DoesNotThrowAChromaVector) {
  // A monochrome staircase: no chroma anywhere in the picture, but hard
  // luminance steps.  An instrument band-limits around the subcarrier before
  // demodulating, so those steps must leave the trace near the origin.  With
  // only enough filtering to reject the demodulation image, each step throws a
  // full-amplitude rotating vector across the display and buries the trace.
  const orc::SourceParameters parameters = pal_parameters();
  const size_t spl = static_cast<size_t>(parameters.frame_width_nominal);
  const size_t lines = static_cast<size_t>(parameters.frame_height);
  const double range =
      static_cast<double>(parameters.white_level - parameters.blanking_level);

  // Start from a normal burst-carrying frame so the reference still locks,
  // then overwrite the active picture with an achromatic staircase.
  std::vector<int16_t> frame =
      synthesise_frame(parameters, {0.0, 0.0}, 0.0, /*switched_v=*/true);

  const size_t active_start =
      static_cast<size_t>(parameters.active_video_start);
  const size_t active_end = static_cast<size_t>(parameters.active_video_end);
  const size_t step_width = (active_end - active_start) / 8;
  for (size_t line = 0; line < lines; ++line) {
    const size_t offset =
        orc::frame_line_sample_offset(parameters.system, spl, line);
    for (size_t i = active_start; i < active_end; ++i) {
      const size_t step = (i - active_start) / step_width;
      const double luma = static_cast<double>(step % 8) / 7.0;
      frame[offset + i] = static_cast<int16_t>(
          std::lround(parameters.blanking_level + (luma * range)));
    }
  }

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::ActiveLine;
  options.first_line = 100;
  options.last_line = 200;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  ASSERT_FALSE(data->samples.empty());

  // Measured against the nearest colour-bar target — blue and yellow at 75 %,
  // the innermost of the six.  A monochrome picture must never throw the trace
  // out as far as a colour it does not contain.  Band-limiting only enough to
  // reject the demodulation image lets a hard step reach past the 100 % ring.
  const Uv blue_75 = rgb_to_uv(0.0, 0.0, 0.75);
  const double nearest_target =
      std::hypot(blue_75.u, blue_75.v) * kDisplayFullScale;

  double worst = 0.0;
  for (const auto& sample : data->samples) {
    worst = std::max(worst, std::hypot(sample.u, sample.v));
  }
  EXPECT_LT(worst, nearest_target * 0.8)
      << "worst luma-edge excursion " << worst << " against nearest colour-bar "
      << "target " << nearest_target;
}

TEST(CompositeVectorscopeTest, WholeLineWindow_IncludesBurstAndBlanking) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::WholeLine;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());

  bool has_burst = false;
  bool has_picture = false;
  bool has_blanking = false;
  for (const auto& sample : data->samples) {
    switch (sample.sample_class) {
      case orc::VectorscopeSampleClass::Burst:
        has_burst = true;
        break;
      case orc::VectorscopeSampleClass::Picture:
        has_picture = true;
        break;
      case orc::VectorscopeSampleClass::Blanking:
        has_blanking = true;
        break;
    }
  }
  EXPECT_TRUE(has_burst);
  EXPECT_TRUE(has_picture);
  EXPECT_TRUE(has_blanking);
  EXPECT_EQ(data->first_line, 0u);
  EXPECT_EQ(data->last_line,
            static_cast<uint32_t>(parameters.frame_height - 1));
}

TEST(CompositeVectorscopeTest, LineRange_RestrictsSampledLines) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::ActiveLine;
  options.first_line = 100;
  options.last_line = 103;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  EXPECT_EQ(data->first_line, 100u);
  EXPECT_EQ(data->last_line, 103u);
  EXPECT_EQ(data->height, 4u);
  EXPECT_EQ(data->sample_stride, 1u);

  for (const auto& sample : data->samples) {
    EXPECT_GE(sample.line_number, 100);
    EXPECT_LE(sample.line_number, 103);
  }
}

TEST(CompositeVectorscopeTest, MaxSamples_SubsamplesAndReportsStride) {
  const orc::SourceParameters parameters = pal_parameters();
  const Uv bar = rgb_to_uv(0.75, 0.0, 0.0);
  const std::vector<int16_t> frame =
      synthesise_frame(parameters, bar, 0.299 * 0.75, /*switched_v=*/true);

  orc::CompositeVectorscopeOptions options;
  options.window = orc::VectorscopeSampleWindow::WholeLine;
  options.max_samples = 100000;

  const auto data = orc::extract_composite_vectorscope(
      frame.data(), frame.size(), parameters, 0, options);
  ASSERT_TRUE(data.has_value());
  EXPECT_GT(data->sample_stride, 1u);
  EXPECT_LE(data->samples.size(), 100000u + parameters.frame_height);
}

TEST(CompositeVectorscopeTest, RejectsUnusableParameters) {
  orc::SourceParameters parameters = pal_parameters();
  const std::vector<int16_t> frame(1000, 0);

  EXPECT_FALSE(orc::extract_composite_vectorscope(nullptr, 0, parameters, 0, {})
                   .has_value());

  parameters.system = orc::VideoSystem::Unknown;
  EXPECT_FALSE(orc::extract_composite_vectorscope(frame.data(), frame.size(),
                                                  parameters, 0, {})
                   .has_value());
}

}  // namespace orc_unit_test
