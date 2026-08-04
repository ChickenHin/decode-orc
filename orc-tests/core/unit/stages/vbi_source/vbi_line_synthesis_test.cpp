/*
 * File:        vbi_line_synthesis_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised sync and blanking of a frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_synthesis.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "vbi_output_levels.h"

namespace orc {
namespace {

// EBU Tech. 3280-E Section 1.1.1 Table 1: the 4 x fsc PAL output lattice.
constexpr double kPALOutputRateHz = 17734475.0;

// Samples per microsecond on that lattice.
constexpr double kSamplesPerMicrosecond = kPALOutputRateHz * 1e-6;

// A teletext-carrying frame line of field 1 and one of field 2.
constexpr uint32_t kField1TeletextLine = 6;
constexpr uint32_t kField2TeletextLine = 319;

struct PALSynthesis {
  VBIFrameGeometry geometry;
  VBIVerticalInterval vertical_interval;
  VBIOutputLevels levels;
};

PALSynthesis pal_parts() {
  PALSynthesis parts;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_geometry(VBITVSystem::kPAL, parts.geometry, error))
      << error;
  EXPECT_TRUE(make_vbi_vertical_interval(VBITVSystem::kPAL,
                                         parts.vertical_interval, error))
      << error;
  EXPECT_TRUE(vbi_output_levels(VBITVSystem::kPAL, parts.levels, error))
      << error;
  return parts;
}

VBILineSynthesiser pal_synthesiser() {
  const PALSynthesis parts = pal_parts();
  return VBILineSynthesiser(parts.geometry, parts.vertical_interval,
                            parts.levels, kPALOutputRateHz);
}

// Level at a time measured from a line's 0H rather than from its first stored
// sample: the standard's pulse positions are defined against 0H, and PAL lines
// do not begin there.
double level_at_time(const VBILineSynthesiser& synthesiser, uint32_t frame_line,
                     double time_samples) {
  const double phase = synthesiser.geometry().line_phase(frame_line);
  return synthesiser.level_at(frame_line, time_samples - phase);
}

TEST(VBIRaisedCosineGate, IsHalfAmplitudeAtBothOfThePulseEdges) {
  const double width = 100.0;
  const double transition = 8.0;

  EXPECT_NEAR(vbi_raised_cosine_gate(0.0, width, transition), 0.5, 1e-12);
  EXPECT_NEAR(vbi_raised_cosine_gate(width, width, transition), 0.5, 1e-12);
  EXPECT_NEAR(vbi_raised_cosine_gate(width / 2.0, width, transition), 1.0,
              1e-12);
  EXPECT_DOUBLE_EQ(vbi_raised_cosine_gate(-transition / 2.0, width, transition),
                   0.0);
  EXPECT_DOUBLE_EQ(
      vbi_raised_cosine_gate(width + transition / 2.0, width, transition), 0.0);
  EXPECT_DOUBLE_EQ(vbi_raised_cosine_gate(-10.0, width, transition), 0.0);
}

// Standards quote the 10 % to 90 % build-up time, so the conversion to the
// full transition must reproduce it.
TEST(VBIRaisedCosineGate, TransitionReproducesTheQuotedBuildUpTime) {
  const double build_up = 4.0;
  const double transition = vbi_raised_cosine_transition(build_up);
  ASSERT_GT(transition, build_up);

  const double width = 200.0;
  const int64_t steps = 2000000;
  const double step = 2.0 * transition / static_cast<double>(steps);
  double at_ten = 0.0;
  double at_ninety = 0.0;
  bool found_ten = false;
  for (int64_t index = 0; index <= steps; ++index) {
    const double position = -transition + static_cast<double>(index) * step;
    const double value = vbi_raised_cosine_gate(position, width, transition);
    if (!found_ten && value >= 0.1) {
      at_ten = position;
      found_ten = true;
    }
    if (value >= 0.9) {
      at_ninety = position;
      break;
    }
  }

  ASSERT_TRUE(found_ten);
  EXPECT_NEAR(at_ninety - at_ten, build_up, 1e-3);
}

TEST(VBIRaisedCosineGate, DegenerateArgumentsProduceNoPulse) {
  EXPECT_DOUBLE_EQ(vbi_raised_cosine_gate(0.0, 0.0, 8.0), 0.0);
  EXPECT_DOUBLE_EQ(vbi_raised_cosine_transition(0.0), 0.0);
  EXPECT_DOUBLE_EQ(vbi_raised_cosine_transition(-1.0), 0.0);
}

// ITU-R BT.470-6 Table 1-2: pulse durations are measured between the
// half-amplitude points of their edges, and 0H is the half-amplitude point of
// the line-synchronising pulse's leading edge.
TEST(VBILineSynthesis, LineSyncSpansTheStandardsFourPointSevenMicroseconds) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double half_amplitude =
      0.5 * (static_cast<double>(synthesiser.levels().blanking) +
             static_cast<double>(synthesiser.levels().sync_tip));

  for (const uint32_t line : {kField1TeletextLine, kField2TeletextLine}) {
    EXPECT_NEAR(level_at_time(synthesiser, line, 0.0), half_amplitude, 0.01)
        << "frame line " << line;
    EXPECT_NEAR(level_at_time(synthesiser, line, 4.7 * kSamplesPerMicrosecond),
                half_amplitude, 0.01)
        << "frame line " << line;

    // Sync tip through the middle of the pulse, blanking either side of it.
    EXPECT_DOUBLE_EQ(level_at_time(synthesiser, line, 40.0),
                     static_cast<double>(synthesiser.levels().sync_tip));
    EXPECT_DOUBLE_EQ(level_at_time(synthesiser, line, -6.0),
                     static_cast<double>(synthesiser.levels().blanking));
    EXPECT_DOUBLE_EQ(level_at_time(synthesiser, line, 90.0),
                     static_cast<double>(synthesiser.levels().blanking));
  }
}

// Design §5.6: the back porch, the burst window, the data region and the front
// porch are all at blanking before the burst and the data are written over
// them.
TEST(VBILineSynthesis, EverythingAfterSyncIsBlankingUntilTheNextLine) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double blanking = static_cast<double>(synthesiser.levels().blanking);

  // ITU-R BT.470-6 Table 2 item 2.14: the burst window, 5.6 to 7.85 us.
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, kField1TeletextLine,
                                 5.6 * kSamplesPerMicrosecond),
                   blanking);
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, kField1TeletextLine,
                                 7.85 * kSamplesPerMicrosecond),
                   blanking);

  // The WST data region opens at the service's 10.3 us anchor and closes well
  // inside the line.
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, kField1TeletextLine,
                                 10.3 * kSamplesPerMicrosecond),
                   blanking);
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, kField1TeletextLine,
                                 62.09 * kSamplesPerMicrosecond),
                   blanking);
}

// Design §5.6: a step edge rings through any downstream filter and can look
// like signal, so every edge is a raised cosine of bounded slope.
TEST(VBILineSynthesis, SyncEdgesShowNoStepTransition) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double amplitude = static_cast<double>(synthesiser.levels().blanking) -
                           static_cast<double>(synthesiser.levels().sync_tip);

  // Peak slope of a raised-cosine transition is pi / 2 of its mean slope.
  const double expected_peak_delta = amplitude * 3.14159265358979323846 /
                                     (2.0 * synthesiser.transition_samples());

  std::vector<double> line;
  double largest_delta = 0.0;
  for (uint32_t frame_line = 0; frame_line < 40; ++frame_line) {
    synthesiser.synthesise_line(frame_line, line);
    ASSERT_FALSE(line.empty());
    for (size_t index = 1; index < line.size(); ++index) {
      largest_delta =
          std::max(largest_delta, std::abs(line[index] - line[index - 1]));
    }
  }

  EXPECT_GT(largest_delta, 0.0);
  EXPECT_LE(largest_delta, expected_peak_delta * 1.01);
  // A step would be the full sync amplitude in a single sample.
  EXPECT_LT(largest_delta, amplitude / 4.0);
}

// Nothing manufactured may reach the protected ends of the sample encoding.
TEST(VBILineSynthesis, EverySynthesisedSampleIsInsideTheLegalRange) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();

  std::vector<double> line;
  for (uint32_t frame_line = 0;
       frame_line < synthesiser.geometry().lines_per_frame(); ++frame_line) {
    synthesiser.synthesise_line(frame_line, line);
    ASSERT_EQ(line.size(), synthesiser.geometry().line_length(frame_line));
    for (const double value : line) {
      EXPECT_GE(value, static_cast<double>(kVBIOutputSampleMin));
      EXPECT_LE(value, static_cast<double>(kVBIOutputSampleMax));
      EXPECT_EQ(clamp_vbi_output_sample(value),
                static_cast<uint16_t>(std::lround(value)));
    }
  }
}

// The vertical interval is synthesised from the standard, because the source
// contains none of it.
TEST(VBILineSynthesis, BroadAndEqualisingPulsesAppearTwicePerLine) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double sync_tip = static_cast<double>(synthesiser.levels().sync_tip);
  const double blanking = static_cast<double>(synthesiser.levels().blanking);
  const double half_line = synthesiser.geometry().nominal_line_length() / 2.0;

  // Frame line 0 is wholly field-synchronising pulses: 27.3 us of sync tip in
  // each half-line period, with blanking between them.
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, 0, 13.0 * kSamplesPerMicrosecond),
                   sync_tip);
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, 0, 29.0 * kSamplesPerMicrosecond),
                   blanking);
  EXPECT_DOUBLE_EQ(
      level_at_time(synthesiser, 0, half_line + 13.0 * kSamplesPerMicrosecond),
      sync_tip);

  // Frame line 4 is wholly equalising pulses: 2.35 us each.
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, 4, 1.2 * kSamplesPerMicrosecond),
                   sync_tip);
  EXPECT_DOUBLE_EQ(level_at_time(synthesiser, 4, 3.5 * kSamplesPerMicrosecond),
                   blanking);
  EXPECT_DOUBLE_EQ(
      level_at_time(synthesiser, 4, half_line + 1.2 * kSamplesPerMicrosecond),
      sync_tip);

  EXPECT_NEAR(synthesiser.pulse_width_samples(VBISyncPulse::kBroad),
              27.3 * kSamplesPerMicrosecond, 1e-9);
  EXPECT_NEAR(synthesiser.pulse_width_samples(VBISyncPulse::kEqualising),
              2.35 * kSamplesPerMicrosecond, 1e-9);
}

// PAL lines do not all hold the same number of samples, and each begins at its
// own sub-sample offset from 0H, so a line's waveform has to be evaluated
// against the geometry rather than against an assumed stride.
TEST(VBILineSynthesis, LinesFollowTheirOwnLatticePhaseAndLength) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double half_amplitude =
      0.5 * (static_cast<double>(synthesiser.levels().blanking) +
             static_cast<double>(synthesiser.levels().sync_tip));

  std::vector<double> line;
  for (uint32_t frame_line = 0;
       frame_line < synthesiser.geometry().lines_per_frame(); ++frame_line) {
    synthesiser.synthesise_line(frame_line, line);
    EXPECT_EQ(line.size(), synthesiser.geometry().line_length(frame_line))
        << "frame line " << frame_line;
    // Whatever the line's phase, its leading edge crosses half amplitude at
    // its own 0H.
    EXPECT_NEAR(level_at_time(synthesiser, frame_line, 0.0), half_amplitude,
                0.01)
        << "frame line " << frame_line;
  }

  // The long lines are where the lattice takes up its accumulated fraction.
  EXPECT_EQ(synthesiser.geometry().line_length(156), 1136u);
  EXPECT_EQ(synthesiser.geometry().line_length(157), 1135u);
}

// A line's trailing samples already belong to the leading edge of the line
// after it, which is what keeps the waveform continuous across the boundary.
TEST(VBILineSynthesis, TheWaveformIsContinuousAcrossALineBoundary) {
  const VBILineSynthesiser synthesiser = pal_synthesiser();
  const double blanking = static_cast<double>(synthesiser.levels().blanking);

  std::vector<double> line;
  synthesiser.synthesise_line(5, line);
  ASSERT_FALSE(line.empty());

  const double last = line.back();
  EXPECT_LT(last, blanking);

  std::vector<double> next_line;
  synthesiser.synthesise_line(6, next_line);
  ASSERT_FALSE(next_line.empty());

  // The descent continues into the next line rather than restarting.
  EXPECT_LT(next_line.front(), last);
  EXPECT_LT(
      std::abs(next_line.front() - last),
      (blanking - static_cast<double>(synthesiser.levels().sync_tip)) / 4.0);
}

TEST(VBILineSynthesis, AnEmptySynthesiserProducesNoLine) {
  const VBILineSynthesiser synthesiser(
      VBIFrameGeometry(), VBIVerticalInterval(), VBIOutputLevels(), 0.0);

  std::vector<double> line;
  synthesiser.synthesise_line(0, line);
  EXPECT_TRUE(line.empty());
  EXPECT_DOUBLE_EQ(synthesiser.level_at(0, 0.0), 0.0);
}

}  // namespace
}  // namespace orc
