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
#include <vector>

#include "vbi_frame_geometry.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// Where a stored line record lands on its output frame line.
//
// Placement is one affine map from the output line's sample index to the
// record's own sample coordinate.  Three separate offsets fold into it
// (design §5.2, §5.5):
//
//   * the service's nominal time from 0H to the first clock run-in one bit,
//   * the capture offset, being the time from 0H to sample 0 of the record,
//     which for a card capture is measured rather than configured (§5.3.4),
//   * the frame line's sub-sample lattice phase, which is non-zero for PAL
//     because 4 x fsc PAL is not orthogonal (§2.3).
//
// Folding them means the resampler is asked for the waveform at one position
// per output sample and applies one filter; nothing is delayed, shifted or
// re-interpolated afterwards, so no error accumulates.
//
// The map is derived from the times of the two lattices relative to the same
// 0H.  Source sample n of the record is at (capture_offset + n) / fs_in, and
// output sample j of frame line k is at (phase(k) + j) / fs_out; equating
// them gives the source coordinate of every output sample.
struct VBILinePlacement {
  // Source samples advanced per output sample: fs_in / fs_out.
  double source_samples_per_output_sample = 0.0;

  // Source coordinate of output sample zero, which is generally negative
  // because the record starts well after the line does.
  double source_position_at_output_zero = 0.0;

  // Output sample index of the leading edge of the first clock run-in one
  // bit: t_offset x fs_out - phase(k).  Fractional, and the figure the
  // placement is judged against.
  double data_start_samples = 0.0;

  // Output sample index one clock run-in, framing code and payload later.
  // Cheap to carry here and it is what the §5.3.5 bit-rate cross-check
  // compares a measured data end against.
  double data_end_samples = 0.0;

  // Half-open output window over which the map has stored samples to read.
  // Bounded by both the record's valid samples and the frame line's length.
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

  // Output sample index of a source coordinate: the inverse of the above.
  double output_index(double source_position) const {
    return (source_samples_per_output_sample != 0.0)
               ? (source_position - source_position_at_output_zero) /
                     source_samples_per_output_sample
               : 0.0;
  }
};

// Build the placement of one stored record on one output frame line.
//
// capture_offset_samples is passed explicitly rather than read from the
// format so that a calibrated offset overrides the descriptor's starting hint
// without the descriptor having to be rewritten (design §5.3.4).
//
// The frame line's phase comes from the geometry and from nowhere else: it is
// the single source of truth for the sample lattice, and a placement computed
// against an assumed constant line length would drift down the frame.
//
// Returns false with an error message for a frame line outside the geometry,
// an unusable sampling rate, or a system pairing the stage cannot place.
bool make_vbi_line_placement(const VBISourceFormat& format,
                             const VBITeletextService& service,
                             const VBIFrameGeometry& geometry,
                             double capture_offset_samples, uint32_t frame_line,
                             VBILinePlacement& out_placement,
                             std::string& error_message);

// Resample one stored record onto its output window.
//
// Output sample i of out_samples is output line sample
// placement.output_begin + i.  Values are in whatever domain the record
// carries: the level mapper has already run, and clamping to the stored word
// happens after this, as the last step of the sample path (design §2.2).
void resample_vbi_line(const IVBIResampler& resampler,
                       const std::vector<double>& record_samples,
                       const VBILinePlacement& placement,
                       std::vector<double>& out_samples);

}  // namespace orc

#endif  // ORC_VBI_LINE_PLACEMENT_H
