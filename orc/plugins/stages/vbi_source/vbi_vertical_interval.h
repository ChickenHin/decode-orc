/*
 * File:        vbi_vertical_interval.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Field-blanking pulse structure of a synthesised CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_VERTICAL_INTERVAL_H
#define ORC_VBI_VERTICAL_INTERVAL_H

#include <cstdint>
#include <string>

#include "vbi_source_format.h"

namespace orc {

// The pulse a half-line period carries.
//
// The whole synchronising structure of a frame — line sync, the equalising
// pulse groups and the field-synchronising broad pulses — is a sequence of
// pulses on a half-line grid, differing only in width.  Describing it that way
// is what makes the two fields fall out of one table: a field whose sequence
// begins on a half-line boundary produces the offset pattern by construction
// rather than by a second set of rules (design §5.6).
enum class VBISyncPulse {
  kNone,        // the second half of an ordinary line: no pulse
  kLineSync,    // ITU-R BT.470-6 Table 1-1 symbol c: line-synchronising pulse
  kEqualising,  // ITU-R BT.470-6 Table 1-2 symbol p
  kBroad,       // ITU-R BT.470-6 Table 1-2 symbol q: field-synchronising pulse
};

// Widths of the synchronising pulses of a television system.
//
// A raw VBI capture contains none of this: the vertical interval is
// synthesised entirely from the standard (design §5.6), so these numbers are
// the whole of what makes the output a structurally valid frame.
struct VBISyncTiming {
  // ITU-R BT.470-6 Table 1-2 symbol r: 4.7 us for the 625-line systems.
  double line_sync_width_ns = 0.0;

  // ITU-R BT.470-6 Table 1-2 symbol p.
  double equalising_pulse_width_ns = 0.0;

  // ITU-R BT.470-6 Table 1-2 symbol q.
  double broad_pulse_width_ns = 0.0;

  // ITU-R BT.470-6 Table 1-2 symbol s: 10 % to 90 % build-up time of the
  // synchronising and equalising pulse edges.  A synthesised edge must have
  // one: a step rings through any downstream filter and can look like signal
  // (design §5.6).
  double build_up_ns = 0.0;

  // Width of one pulse type, in nanoseconds.  kNone has no width.
  double width_ns(VBISyncPulse pulse) const;
};

// The two half-line periods of one frame line.
struct VBIHalfLinePulses {
  VBISyncPulse first_half = VBISyncPulse::kNone;
  VBISyncPulse second_half = VBISyncPulse::kNone;
};

// The synchronising pulse sequence of a whole frame, on a half-line grid.
//
// Each television field contributes three groups of pulses: equalising, broad,
// equalising, each of pulses_per_group half-line periods (ITU-R BT.470-6
// Table 1-2 symbols l, m and n, each 2.5 H for the 625-line systems).  A field
// is placed by the half-line at which its broad-pulse group begins, and that
// single number carries the field parity: PAL's field 1 begins on a whole line
// and field 2 half a line later, the (1, 0.5) pattern of design §5.6.
//
// Half-line indices wrap within the frame, so the group that straddles the
// frame boundary needs no special case.
class VBIVerticalInterval {
 public:
  // An empty sequence, in which every half-line reports kNone.
  // make_vbi_vertical_interval() produces the usable instances.
  VBIVerticalInterval() = default;

  VBIVerticalInterval(uint32_t lines_per_frame, uint32_t pulses_per_group,
                      uint32_t field1_broad_half_line,
                      uint32_t field2_broad_half_line, VBISyncTiming timing);

  const VBISyncTiming& timing() const { return timing_; }

  uint32_t lines_per_frame() const { return lines_per_frame_; }

  uint32_t half_lines_per_frame() const { return lines_per_frame_ * 2u; }

  uint32_t pulses_per_group() const { return pulses_per_group_; }

  // Half-line at which a television field's broad-pulse group begins.  An
  // out-of-range field number yields the first field's value.
  uint32_t broad_group_half_line(uint32_t tv_field) const;

  // Frame line, 1-based and counting broadcast line 1 as the first line of
  // field 1, at which a television field's synchronising sequence begins.
  //
  // Fractional by half a line for a field that begins mid-line: the (1, 313.5)
  // pair for PAL is the half-line pattern that distinguishes the two fields of
  // a frame (design §5.6).
  double field_sync_start_line(uint32_t tv_field) const;

  // Pulse carried by a half-line period.  Indices outside the frame wrap, so
  // the caller may ask about the half-line before the frame or after it.
  VBISyncPulse pulse_at_half_line(int64_t half_line) const;

  // Pulses carried by the two half-line periods of a frame line.
  VBIHalfLinePulses pulses_for_line(uint32_t frame_line) const;

 private:
  // True when a half-line falls inside a group of pulses_per_group periods
  // starting at group_start, evaluated on the wrapped half-line grid.
  bool in_group(uint32_t half_line, int64_t group_start) const;

  uint32_t lines_per_frame_ = 0;
  uint32_t pulses_per_group_ = 0;
  uint32_t field1_broad_half_line_ = 0;
  uint32_t field2_broad_half_line_ = 0;
  VBISyncTiming timing_{};
};

// Build the frame's synchronising sequence for a television system.  Returns
// false with an error message for systems the stage does not yet synthesise.
bool make_vbi_vertical_interval(VBITVSystem tv_system,
                                VBIVerticalInterval& out_interval,
                                std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_VERTICAL_INTERVAL_H
