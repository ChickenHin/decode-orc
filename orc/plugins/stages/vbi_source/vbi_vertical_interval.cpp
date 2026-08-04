/*
 * File:        vbi_vertical_interval.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Field-blanking pulse structure of a synthesised CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_vertical_interval.h"

namespace orc {

namespace {

// ITU-R BT.470-6 Table 1-1 symbol c and Table 1-2 symbols p, q, r and s, for
// the 625-line systems B, B1, D, D1, G, H, I, K and K1.
constexpr double kPALLineSyncWidthNs = 4700.0;
constexpr double kPALEqualisingPulseWidthNs = 2350.0;
constexpr double kPALBroadPulseWidthNs = 27300.0;
constexpr double kPALSyncBuildUpNs = 250.0;

// ITU-R BT.470-6 Table 1-2 symbols l, m and n: each of the three groups of the
// field-synchronising sequence lasts 2.5 H, which is five half-line periods.
constexpr uint32_t kPALPulsesPerGroup = 5;

// ITU-R BT.470-6 Fig. 2-1a and 2-1b: the field-synchronising (broad) pulses of
// field 1 begin at the leading edge of the line-synchronising pulse of line 1,
// and those of field 2 half a line period into line 313.  Expressed on the
// frame's half-line grid, with half-line 0 at the start of stored frame line 0.
constexpr uint32_t kPALLinesPerFrame = 625;
constexpr uint32_t kPALField1BroadHalfLine = 0;
constexpr uint32_t kPALField2BroadHalfLine = 625;

}  // namespace

double VBISyncTiming::width_ns(VBISyncPulse pulse) const {
  switch (pulse) {
    case VBISyncPulse::kLineSync:
      return line_sync_width_ns;
    case VBISyncPulse::kEqualising:
      return equalising_pulse_width_ns;
    case VBISyncPulse::kBroad:
      return broad_pulse_width_ns;
    case VBISyncPulse::kNone:
      break;
  }
  return 0.0;
}

VBIVerticalInterval::VBIVerticalInterval(uint32_t lines_per_frame,
                                         uint32_t pulses_per_group,
                                         uint32_t field1_broad_half_line,
                                         uint32_t field2_broad_half_line,
                                         VBISyncTiming timing)
    : lines_per_frame_(lines_per_frame),
      pulses_per_group_(pulses_per_group),
      field1_broad_half_line_(field1_broad_half_line),
      field2_broad_half_line_(field2_broad_half_line),
      timing_(timing) {}

uint32_t VBIVerticalInterval::broad_group_half_line(uint32_t tv_field) const {
  return (tv_field == 2) ? field2_broad_half_line_ : field1_broad_half_line_;
}

double VBIVerticalInterval::field_sync_start_line(uint32_t tv_field) const {
  // Broadcast line 1 is stored frame line 0, which begins at half-line 0.
  return 1.0 + static_cast<double>(broad_group_half_line(tv_field)) / 2.0;
}

bool VBIVerticalInterval::in_group(uint32_t half_line,
                                   int64_t group_start) const {
  const int64_t period = static_cast<int64_t>(half_lines_per_frame());
  if (period <= 0 || pulses_per_group_ == 0) {
    return false;
  }

  // Distance from the start of the group, wrapped into one frame.  A group
  // that runs off the end of the frame continues at its start, which is what
  // makes the sequence straddling the frame boundary need no special case.
  int64_t distance = (static_cast<int64_t>(half_line) - group_start) % period;
  if (distance < 0) {
    distance += period;
  }
  return distance < static_cast<int64_t>(pulses_per_group_);
}

VBISyncPulse VBIVerticalInterval::pulse_at_half_line(int64_t half_line) const {
  const int64_t period = static_cast<int64_t>(half_lines_per_frame());
  if (period <= 0) {
    return VBISyncPulse::kNone;
  }

  int64_t wrapped = half_line % period;
  if (wrapped < 0) {
    wrapped += period;
  }
  const uint32_t index = static_cast<uint32_t>(wrapped);
  const int64_t group = static_cast<int64_t>(pulses_per_group_);

  for (uint32_t tv_field = 1; tv_field <= 2; ++tv_field) {
    const int64_t broad_start =
        static_cast<int64_t>(broad_group_half_line(tv_field));
    if (in_group(index, broad_start)) {
      return VBISyncPulse::kBroad;
    }
    // ITU-R BT.470-6 Table 1-2 symbols l and n: one group of equalising pulses
    // each side of the broad pulses.
    if (in_group(index, broad_start - group) ||
        in_group(index, broad_start + group)) {
      return VBISyncPulse::kEqualising;
    }
  }

  // Outside the field-synchronising sequence every line carries its own
  // synchronising pulse and the second half of the line carries nothing.
  return ((index % 2u) == 0u) ? VBISyncPulse::kLineSync : VBISyncPulse::kNone;
}

VBIHalfLinePulses VBIVerticalInterval::pulses_for_line(
    uint32_t frame_line) const {
  VBIHalfLinePulses pulses;
  const int64_t first = static_cast<int64_t>(frame_line) * 2;
  pulses.first_half = pulse_at_half_line(first);
  pulses.second_half = pulse_at_half_line(first + 1);
  return pulses;
}

bool make_vbi_vertical_interval(VBITVSystem tv_system,
                                VBIVerticalInterval& out_interval,
                                std::string& error_message) {
  switch (tv_system) {
    case VBITVSystem::kPAL: {
      VBISyncTiming timing;
      timing.line_sync_width_ns = kPALLineSyncWidthNs;
      timing.equalising_pulse_width_ns = kPALEqualisingPulseWidthNs;
      timing.broad_pulse_width_ns = kPALBroadPulseWidthNs;
      timing.build_up_ns = kPALSyncBuildUpNs;
      out_interval = VBIVerticalInterval(kPALLinesPerFrame, kPALPulsesPerGroup,
                                         kPALField1BroadHalfLine,
                                         kPALField2BroadHalfLine, timing);
      return true;
    }

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      // The 525-line sequence is the same structure with six pulses to a group
      // and the fields the other way round in the half-line grid, but nothing
      // else of the 525-line synthesis path exists yet.
      error_message =
          "Vertical interval synthesis for 525-line systems is not implemented "
          "yet; only PAL frames can currently be synthesised.";
      out_interval = VBIVerticalInterval();
      return false;
  }

  error_message = "Unrecognised television system.";
  out_interval = VBIVerticalInterval();
  return false;
}

}  // namespace orc
