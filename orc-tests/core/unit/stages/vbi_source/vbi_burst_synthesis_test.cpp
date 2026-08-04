/*
 * File:        vbi_burst_synthesis_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised colour burst
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_burst_synthesis.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace orc {
namespace {

constexpr double kPi = 3.14159265358979323846;

// EBU Tech. 3280-E Section 1.1.1 Table 1: the 4 x fsc PAL output lattice.
constexpr double kPALOutputRateHz = 17734475.0;
constexpr double kSamplesPerMicrosecond = kPALOutputRateHz * 1e-6;

VBIFrameGeometry pal_geometry() {
  VBIFrameGeometry geometry;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_geometry(VBITVSystem::kPAL, geometry, error))
      << error;
  return geometry;
}

VBIOutputLevels pal_levels() {
  VBIOutputLevels levels;
  std::string error;
  EXPECT_TRUE(vbi_output_levels(VBITVSystem::kPAL, levels, error)) << error;
  return levels;
}

VBIBurstTiming pal_burst_timing() {
  VBIBurstTiming timing;
  std::string error;
  EXPECT_TRUE(
      make_vbi_burst_timing(VBITVSystem::kPAL, pal_levels(), timing, error))
      << error;
  return timing;
}

VBIBurstSynthesiser pal_synthesiser() {
  return VBIBurstSynthesiser(pal_geometry(), pal_levels(), pal_burst_timing(),
                             kPALOutputRateHz);
}

// A blanking-filled line of the right length for a frame line.
std::vector<double> blank_line(const VBIFrameGeometry& geometry,
                               const VBIOutputLevels& levels,
                               uint32_t frame_line) {
  return std::vector<double>(geometry.line_length(frame_line),
                             static_cast<double>(levels.blanking));
}

// Phase of the burst actually present on a line, in degrees relative to the
// +U axis, recovered by demodulating against the output lattice's own
// subcarrier reference.
//
// This is what makes the coherence check meaningful: the reference is the
// absolute sample index in the output sequence, so a phase that drifted from
// frame to frame would show up here even though every frame looks correct on
// its own.
double recovered_phase_degrees(const VBIFrameGeometry& geometry,
                               const VBIOutputLevels& levels,
                               uint64_t frame_index, uint32_t frame_line,
                               const std::vector<double>& line) {
  const double base = static_cast<double>(frame_index) *
                          static_cast<double>(geometry.samples_per_frame()) +
                      static_cast<double>(geometry.line_start(frame_line));

  double in_phase = 0.0;
  double quadrature = 0.0;
  for (size_t index = 0; index < line.size(); ++index) {
    const double value = line[index] - static_cast<double>(levels.blanking);
    const double angle = 2.0 * kPi * 0.25 * (base + static_cast<double>(index));
    in_phase += value * std::sin(angle);
    quadrature += value * std::cos(angle);
  }
  return std::atan2(quadrature, in_phase) * 180.0 / kPi;
}

// ITU-R BT.470-6 Table 2 items 2.14 and 2.15.
TEST(VBIBurstSynthesis, PALBurstParametersMatchTheStandard) {
  const VBIBurstTiming timing = pal_burst_timing();
  const VBIOutputLevels levels = pal_levels();

  EXPECT_DOUBLE_EQ(timing.start_ns, 5600.0);
  EXPECT_DOUBLE_EQ(timing.duration_ns, 2250.0);
  EXPECT_DOUBLE_EQ(timing.end_ns(), 7850.0);
  EXPECT_DOUBLE_EQ(timing.subcarrier_hz, 4433618.75);
  EXPECT_DOUBLE_EQ(timing.phase_degrees, 135.0);
  EXPECT_TRUE(timing.swinging);

  // Peak-to-peak burst is three sevenths of the blanking-to-white excursion:
  // 3 x 588 / 7 = 252 counts, the same amplitude as sync.
  EXPECT_DOUBLE_EQ(timing.amplitude_counts, 252.0);
  EXPECT_DOUBLE_EQ(timing.amplitude_counts,
                   static_cast<double>(levels.blanking) -
                       static_cast<double>(levels.sync_tip));

  // The standard's own two statements of the duration — 2.25 us and ten
  // subcarrier cycles — agree to a fortieth of a cycle.
  EXPECT_NEAR(timing.duration_ns * 1e-9 * timing.subcarrier_hz, 10.0, 0.03);
}

TEST(VBIBurstSynthesis, FiveHundredTwentyFiveLineSystemsAreNotYetAvailable) {
  VBIBurstTiming timing;
  std::string error;

  EXPECT_FALSE(
      make_vbi_burst_timing(VBITVSystem::kNTSC, pal_levels(), timing, error));
  EXPECT_FALSE(error.empty());
}

// The burst occupies its window and nothing else: the data region in
// particular must be left exactly as the caller blanked it.
TEST(VBIBurstSynthesis, BurstOccupiesOnlyItsOwnWindow) {
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIOutputLevels levels = pal_levels();
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();
  const double blanking = static_cast<double>(levels.blanking);

  const uint32_t frame_line = 6;
  std::vector<double> line = blank_line(geometry, levels, frame_line);
  synthesiser.synthesise_burst(0, frame_line, line);

  const double phase = geometry.line_phase(frame_line);
  const double window_start = 5.6 * kSamplesPerMicrosecond - phase;
  const double window_end = 7.85 * kSamplesPerMicrosecond - phase;

  double peak = 0.0;
  for (size_t index = 0; index < line.size(); ++index) {
    const double deviation = line[index] - blanking;
    const double position = static_cast<double>(index);
    if (position >= window_start && position <= window_end) {
      peak = std::max(peak, std::abs(deviation));
    } else if (position < window_start - 5.0 || position > window_end + 5.0) {
      // Clear of the envelope's taper the line is untouched.
      EXPECT_DOUBLE_EQ(line[index], blanking) << "sample " << index;
    }
  }

  // Peak deviation is half the peak-to-peak amplitude, and the 4 x fsc lattice
  // samples a 135 degree burst at its 0.707 points.
  EXPECT_NEAR(peak, 126.0 * std::sqrt(0.5), 1.0);
}

// ITU-R BT.470-6 Table 2 item 2.16: the V component reverses from line to
// line, and the reversal itself reverses every two fields.  A 625-line frame
// is an odd number of lines, so strict alternation produces that on its own.
TEST(VBIBurstSynthesis, SwingReversesEveryLineAndEveryFrame) {
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();

  EXPECT_EQ(synthesiser.swing_sign(0, 0), 1);
  EXPECT_EQ(synthesiser.swing_sign(0, 1), -1);
  EXPECT_EQ(synthesiser.swing_sign(0, 2), 1);

  // Frame line 0 of successive frames alternates, so the pattern repeats over
  // two frames and, with the subcarrier progression, over four.
  EXPECT_EQ(synthesiser.swing_sign(1, 0), -1);
  EXPECT_EQ(synthesiser.swing_sign(2, 0), 1);
  EXPECT_EQ(synthesiser.swing_sign(3, 0), -1);

  EXPECT_DOUBLE_EQ(synthesiser.phase_degrees(0, 0), 135.0);
  EXPECT_DOUBLE_EQ(synthesiser.phase_degrees(0, 1), -135.0);
  EXPECT_DOUBLE_EQ(synthesiser.phase_degrees(1, 0), -135.0);
}

// The synthesised burst must carry the phase the standard asks for, on every
// line of an eight-frame sequence, measured against an absolute subcarrier
// reference rather than against itself.
TEST(VBIBurstSynthesis, PhaseProgressionIsCoherentAcrossEightFrames) {
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIOutputLevels levels = pal_levels();
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();

  for (uint64_t frame = 0; frame < 8; ++frame) {
    for (const uint32_t frame_line : {6u, 7u, 21u, 319u, 334u, 500u}) {
      std::vector<double> line = blank_line(geometry, levels, frame_line);
      synthesiser.synthesise_burst(frame, frame_line, line);

      const double recovered =
          recovered_phase_degrees(geometry, levels, frame, frame_line, line);
      const double expected = synthesiser.phase_degrees(frame, frame_line);

      double difference = recovered - expected;
      while (difference > 180.0) difference -= 360.0;
      while (difference < -180.0) difference += 360.0;

      EXPECT_NEAR(difference, 0.0, 3.0)
          << "frame " << frame << " line " << frame_line;
    }
  }
}

// ITU-R BT.470-6 Table 2 item 2.17 and Fig. 5a: nine lines of each
// field-blanking interval carry no burst, and the window meanders by one line
// with a four-field period.
TEST(VBIBurstSynthesis, BurstBlankingCoversNineLinesOfEachFieldInterval) {
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();

  for (uint64_t frame = 0; frame < 4; ++frame) {
    uint32_t blanked = 0;
    for (uint32_t frame_line = 0; frame_line < 625; ++frame_line) {
      if (synthesiser.is_blanked(frame, frame_line)) {
        ++blanked;
      }
    }
    // Two intervals touch each frame: the one in its middle, and the parts of
    // the two that straddle its boundaries.  The straddling window splits 3/6
    // one way and 4/5 the other, so a frame carries 17 or 19 blanked lines and
    // consecutive pairs carry 36 — four nine-line windows.
    EXPECT_EQ(blanked, (frame % 2u == 0u) ? 17u : 19u) << "frame " << frame;
  }

  // Even frames use the windows starting at broadcast lines 311 and 623, odd
  // frames the ones a line earlier.
  EXPECT_TRUE(synthesiser.is_blanked(0, 310));   // broadcast line 311
  EXPECT_TRUE(synthesiser.is_blanked(0, 318));   // broadcast line 319
  EXPECT_FALSE(synthesiser.is_blanked(0, 309));  // broadcast line 310
  EXPECT_FALSE(synthesiser.is_blanked(0, 319));  // broadcast line 320

  EXPECT_TRUE(synthesiser.is_blanked(1, 309));   // broadcast line 310
  EXPECT_TRUE(synthesiser.is_blanked(1, 317));   // broadcast line 318
  EXPECT_FALSE(synthesiser.is_blanked(1, 318));  // broadcast line 319

  // The window that opens at the end of an even frame runs 623 to 6, so it
  // continues over the first six lines of the frame after it.
  EXPECT_TRUE(synthesiser.is_blanked(0, 622));  // broadcast line 623
  EXPECT_TRUE(synthesiser.is_blanked(1, 5));    // broadcast line 6
  EXPECT_FALSE(synthesiser.is_blanked(1, 6));   // broadcast line 7
  // And the one at the end of an odd frame runs 622 to 5.
  EXPECT_TRUE(synthesiser.is_blanked(1, 621));  // broadcast line 622
  EXPECT_TRUE(synthesiser.is_blanked(2, 4));    // broadcast line 5
  EXPECT_FALSE(synthesiser.is_blanked(2, 5));   // broadcast line 6
}

// The teletext lines must never fall inside the burst-blanking sequence: a
// data line without burst is not what a broadcast teletext line looks like.
TEST(VBIBurstSynthesis, TeletextLinesAreNeverBurstBlanked) {
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();

  for (uint64_t frame = 0; frame < 8; ++frame) {
    for (uint32_t frame_line = 6; frame_line <= 21; ++frame_line) {
      EXPECT_FALSE(synthesiser.is_blanked(frame, frame_line))
          << "frame " << frame << " line " << frame_line;
    }
    for (uint32_t frame_line = 319; frame_line <= 334; ++frame_line) {
      EXPECT_FALSE(synthesiser.is_blanked(frame, frame_line))
          << "frame " << frame << " line " << frame_line;
    }
  }
}

// A blanked line keeps whatever the caller put there, which is blanking.
TEST(VBIBurstSynthesis, ABlankedLineIsLeftAlone) {
  const VBIFrameGeometry geometry = pal_geometry();
  const VBIOutputLevels levels = pal_levels();
  const VBIBurstSynthesiser synthesiser = pal_synthesiser();

  const uint32_t frame_line = 310;  // broadcast line 311, inside window I
  ASSERT_TRUE(synthesiser.is_blanked(0, frame_line));

  std::vector<double> line = blank_line(geometry, levels, frame_line);
  synthesiser.synthesise_burst(0, frame_line, line);

  for (const double value : line) {
    EXPECT_DOUBLE_EQ(value, static_cast<double>(levels.blanking));
  }
}

}  // namespace
}  // namespace orc
