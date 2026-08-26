/*
 * File:        vbi_line_placement.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Horizontal placement of a stored record on an output frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LINE_PLACEMENT_H
#define ORC_VBI_LINE_PLACEMENT_H

#include <cstdint>
#include <string>

#include "vbi_output_frame.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// Where a stored line record lands on its output frame line.
//
// Placement is one affine map from the output line's sample index to the
// record's own sample coordinate.  Two offsets fold into it (design §5.2):
//
//   * the service's nominal time from 0H to the first clock run-in one bit,
//   * the capture offset, being the time from 0H to sample 0 of the record,
//     which for a card capture is measured rather than configured (§5.3.4).
//
// Folding them means the resampler is asked for the waveform at one position
// per output sample and applies one filter; nothing is delayed, shifted or
// re-interpolated afterwards, so no error accumulates.
//
// The map is the same on every data line of every frame.  Sample 0 of a stored
// line is that line's 0H — the convention every source in the host is read on
// (see VBIOutputFrame) — so a line's position in the frame does not enter the
// arithmetic, and the whole placement is computed once per run.
struct VBIDataPlacement {
  // Source samples advanced per output sample: fs_in / fs_out.
  double source_samples_per_output_sample = 0.0;

  // Source coordinate of output sample zero.  Negative by the capture offset,
  // because the record starts that far into the line.
  double source_position_at_output_zero = 0.0;

  // Output sample index of the leading edge of the first clock run-in one bit:
  // t_offset x fs_out.  Fractional, and the figure the placement is judged
  // against.
  double data_start_samples = 0.0;

  // Output sample index one clock run-in, framing code and payload later.
  double data_end_samples = 0.0;

  // Half-open output window the record is written over.  Bounded by the
  // record's valid samples, by the data region the standard gives the service
  // (with a bit period of guard at each end so the leading edge of the first
  // run-in bit and the trailing edge of the last payload bit survive), and by
  // the length of the line.
  //
  // The clip to the data region matters because a record covers more of the
  // line than the region does at both ends: a bt8x8 PAL record opens inside the
  // colour burst window and runs on past the end of the packet, and writing all
  // of it would put teletext-scaled samples where the rest of the frame is
  // blanking (design §5.6).
  uint32_t output_begin = 0;
  uint32_t output_end = 0;

  uint32_t output_count() const {
    return (output_end > output_begin) ? (output_end - output_begin) : 0;
  }

  // Source coordinate of an output sample index.
  double source_position(double output_index) const {
    return source_position_at_output_zero +
           output_index * source_samples_per_output_sample;
  }
};

// Build the placement of a source format's records on the output lattice.
//
// capture_offset_samples is passed explicitly rather than read from the format
// so that a calibrated offset overrides the descriptor's starting hint without
// the descriptor having to be rewritten (design §5.3.4).
//
// Returns false with an error message for an unusable sampling rate, a record
// with no valid samples, or a non-finite capture offset.
bool make_vbi_data_placement(const VBISourceFormat& format,
                             const VBITeletextService& service,
                             const VBIOutputFrame& output_frame,
                             double capture_offset_samples,
                             VBIDataPlacement& out_placement,
                             std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_LINE_PLACEMENT_H
