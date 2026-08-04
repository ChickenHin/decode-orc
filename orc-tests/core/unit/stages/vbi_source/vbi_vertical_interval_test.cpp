/*
 * File:        vbi_vertical_interval_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised field-blanking pulse structure
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_vertical_interval.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

VBIVerticalInterval pal_vertical_interval() {
  VBIVerticalInterval interval;
  std::string error;
  EXPECT_TRUE(make_vbi_vertical_interval(VBITVSystem::kPAL, interval, error))
      << error;
  return interval;
}

// ITU-R BT.470-6 Table 1-1 symbol c and Table 1-2 symbols p, q and s.
TEST(VBIVerticalInterval, PALPulseWidthsMatchTheStandard) {
  const VBIVerticalInterval interval = pal_vertical_interval();
  const VBISyncTiming& timing = interval.timing();

  EXPECT_DOUBLE_EQ(timing.line_sync_width_ns, 4700.0);
  EXPECT_DOUBLE_EQ(timing.equalising_pulse_width_ns, 2350.0);
  EXPECT_DOUBLE_EQ(timing.broad_pulse_width_ns, 27300.0);
  EXPECT_DOUBLE_EQ(timing.build_up_ns, 250.0);

  EXPECT_DOUBLE_EQ(timing.width_ns(VBISyncPulse::kLineSync), 4700.0);
  EXPECT_DOUBLE_EQ(timing.width_ns(VBISyncPulse::kEqualising), 2350.0);
  EXPECT_DOUBLE_EQ(timing.width_ns(VBISyncPulse::kBroad), 27300.0);
  EXPECT_DOUBLE_EQ(timing.width_ns(VBISyncPulse::kNone), 0.0);
}

// ITU-R BT.470-6 Table 1-2 symbols l, m and n: three groups of 2.5 H, which is
// five half-line periods each, per field.
TEST(VBIVerticalInterval, PALFrameCarriesTenBroadAndTwentyEqualisingPulses) {
  const VBIVerticalInterval interval = pal_vertical_interval();

  EXPECT_EQ(interval.lines_per_frame(), 625u);
  EXPECT_EQ(interval.half_lines_per_frame(), 1250u);
  EXPECT_EQ(interval.pulses_per_group(), 5u);

  uint32_t broad = 0;
  uint32_t equalising = 0;
  uint32_t line_sync = 0;
  uint32_t none = 0;
  for (uint32_t half_line = 0; half_line < interval.half_lines_per_frame();
       ++half_line) {
    switch (interval.pulse_at_half_line(half_line)) {
      case VBISyncPulse::kBroad:
        ++broad;
        break;
      case VBISyncPulse::kEqualising:
        ++equalising;
        break;
      case VBISyncPulse::kLineSync:
        ++line_sync;
        break;
      case VBISyncPulse::kNone:
        ++none;
        break;
    }
  }

  EXPECT_EQ(broad, 10u);
  EXPECT_EQ(equalising, 20u);
  // The remaining 1220 half-line periods are 610 ordinary lines.
  EXPECT_EQ(line_sync, 610u);
  EXPECT_EQ(none, 610u);
}

// ITU-R BT.470-6 Fig. 2-1a and 2-1b: the groups sit either side of each
// field's synchronising datum, and the first field's leading group runs off
// the end of the frame into its own start.
TEST(VBIVerticalInterval, PALGroupsSitWhereTheStandardPutsThem) {
  const VBIVerticalInterval interval = pal_vertical_interval();

  for (uint32_t half_line = 0; half_line < 5; ++half_line) {
    EXPECT_EQ(interval.pulse_at_half_line(half_line), VBISyncPulse::kBroad)
        << "half-line " << half_line;
    EXPECT_EQ(interval.pulse_at_half_line(625 + half_line),
              VBISyncPulse::kBroad)
        << "half-line " << (625 + half_line);
  }

  for (uint32_t half_line = 5; half_line < 10; ++half_line) {
    EXPECT_EQ(interval.pulse_at_half_line(half_line), VBISyncPulse::kEqualising)
        << "half-line " << half_line;
  }
  for (uint32_t half_line = 1245; half_line < 1250; ++half_line) {
    EXPECT_EQ(interval.pulse_at_half_line(half_line), VBISyncPulse::kEqualising)
        << "half-line " << half_line;
  }
  for (uint32_t half_line = 620; half_line < 625; ++half_line) {
    EXPECT_EQ(interval.pulse_at_half_line(half_line), VBISyncPulse::kEqualising)
        << "half-line " << half_line;
  }
  for (uint32_t half_line = 630; half_line < 635; ++half_line) {
    EXPECT_EQ(interval.pulse_at_half_line(half_line), VBISyncPulse::kEqualising)
        << "half-line " << half_line;
  }

  // The first line clear of each sequence carries an ordinary line sync, and
  // teletext begins on the line after it.
  EXPECT_EQ(interval.pulses_for_line(5).first_half, VBISyncPulse::kLineSync);
  EXPECT_EQ(interval.pulses_for_line(5).second_half, VBISyncPulse::kNone);
  EXPECT_EQ(interval.pulses_for_line(318).first_half, VBISyncPulse::kLineSync);
  EXPECT_EQ(interval.pulses_for_line(318).second_half, VBISyncPulse::kNone);
}

// Every teletext line is an ordinary line: the data must never land on a line
// whose synchronising structure differs from the one the decoder expects.
TEST(VBIVerticalInterval, PALTeletextLinesCarryOrdinaryLineSync) {
  const VBIVerticalInterval interval = pal_vertical_interval();

  for (uint32_t frame_line = 6; frame_line <= 21; ++frame_line) {
    const VBIHalfLinePulses pulses = interval.pulses_for_line(frame_line);
    EXPECT_EQ(pulses.first_half, VBISyncPulse::kLineSync)
        << "frame line " << frame_line;
    EXPECT_EQ(pulses.second_half, VBISyncPulse::kNone)
        << "frame line " << frame_line;
  }
  for (uint32_t frame_line = 319; frame_line <= 334; ++frame_line) {
    const VBIHalfLinePulses pulses = interval.pulses_for_line(frame_line);
    EXPECT_EQ(pulses.first_half, VBISyncPulse::kLineSync)
        << "frame line " << frame_line;
    EXPECT_EQ(pulses.second_half, VBISyncPulse::kNone)
        << "frame line " << frame_line;
  }
}

// The two fields differ by half a line, and that offset is what identifies the
// field parity: field 1's sequence opens on a line boundary, field 2's half a
// line into line 313 (design §5.6).
TEST(VBIVerticalInterval, PALFieldsDifferByTheHalfLinePattern) {
  const VBIVerticalInterval interval = pal_vertical_interval();

  EXPECT_EQ(interval.broad_group_half_line(1), 0u);
  EXPECT_EQ(interval.broad_group_half_line(2), 625u);
  EXPECT_DOUBLE_EQ(interval.field_sync_start_line(1), 1.0);
  EXPECT_DOUBLE_EQ(interval.field_sync_start_line(2), 313.5);

  // Field 1's first line is wholly broad pulses; field 2's opens with the last
  // equalising pulse of its leading group and turns broad half-way through.
  const VBIHalfLinePulses field1_first = interval.pulses_for_line(0);
  EXPECT_EQ(field1_first.first_half, VBISyncPulse::kBroad);
  EXPECT_EQ(field1_first.second_half, VBISyncPulse::kBroad);

  const VBIHalfLinePulses field2_first = interval.pulses_for_line(312);
  EXPECT_EQ(field2_first.first_half, VBISyncPulse::kEqualising);
  EXPECT_EQ(field2_first.second_half, VBISyncPulse::kBroad);

  // The same asymmetry at the end of each sequence: field 1's trailing group
  // ends on a line boundary, field 2's mid-line.
  EXPECT_EQ(interval.pulses_for_line(4).second_half, VBISyncPulse::kEqualising);
  EXPECT_EQ(interval.pulses_for_line(317).second_half, VBISyncPulse::kNone);
}

// Indices outside the frame wrap, so a caller asking about the half-line
// before a frame gets the one that precedes it in the sequence.
TEST(VBIVerticalInterval, HalfLineIndicesWrapWithinTheFrame) {
  const VBIVerticalInterval interval = pal_vertical_interval();

  EXPECT_EQ(interval.pulse_at_half_line(-1), interval.pulse_at_half_line(1249));
  EXPECT_EQ(interval.pulse_at_half_line(-5), interval.pulse_at_half_line(1245));
  EXPECT_EQ(interval.pulse_at_half_line(1250), interval.pulse_at_half_line(0));
  EXPECT_EQ(interval.pulse_at_half_line(1252), interval.pulse_at_half_line(2));
}

TEST(VBIVerticalInterval, FiveHundredTwentyFiveLineSystemsAreNotYetAvailable) {
  VBIVerticalInterval interval;
  std::string error;

  EXPECT_FALSE(make_vbi_vertical_interval(VBITVSystem::kNTSC, interval, error));
  EXPECT_FALSE(error.empty());

  error.clear();
  EXPECT_FALSE(make_vbi_vertical_interval(VBITVSystem::kPALM, interval, error));
  EXPECT_FALSE(error.empty());
}

TEST(VBIVerticalInterval, AnEmptySequenceIsInert) {
  const VBIVerticalInterval interval;

  EXPECT_EQ(interval.lines_per_frame(), 0u);
  EXPECT_EQ(interval.half_lines_per_frame(), 0u);
  EXPECT_EQ(interval.pulse_at_half_line(0), VBISyncPulse::kNone);
  EXPECT_EQ(interval.pulses_for_line(0).first_half, VBISyncPulse::kNone);
}

}  // namespace
}  // namespace orc
