/*
 * File:        vbi_frame_geometry.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Sample lattice of a synthesised CVBS frame: line starts and
 * phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_GEOMETRY_H
#define ORC_VBI_FRAME_GEOMETRY_H

#include <cstdint>
#include <string>

#include "vbi_source_format.h"

namespace orc {

// The sample lattice of one stored CVBS frame.
//
// This is the single source of truth for where a frame line begins; no other
// code in the stage may compute a line offset for itself.  The reason is that
// PAL at 4 x fsc is not orthogonal: EBU Tech. 3280-E gives 1135.0064 samples
// per line, so a frame is 709 379 samples rather than 625 x 1135, and the
// sampling lattice repeats at frame rate rather than line rate (design §2.3).
// Indexing frame line k at k x 1135 drifts by up to four samples by the bottom
// of the frame and produces a frame that is four samples short, which then
// desynchronises every subsequent frame in the file.
//
// Both the non-orthogonal and the constant-stride cases are the same
// arithmetic: line starts follow the 0H rule
//
//     line_start(k) = ceil(k x samples_per_frame / lines_per_frame)
//
// which degenerates to a constant stride whenever the division is exact, as it
// is for the 910-samples-per-line NTSC lattice.  An orthogonal system is
// therefore a data entry, not a second code path.
//
// Evaluated in exact integer arithmetic: the nominal line length is a rational
// number (709 379 / 625 for PAL) whose decimal form is not representable in
// binary floating point.
class VBIFrameGeometry {
 public:
  // An empty lattice.  Every accessor reports zero; make_vbi_frame_geometry()
  // produces the usable instances.
  VBIFrameGeometry() = default;

  VBIFrameGeometry(uint32_t lines_per_frame, uint32_t samples_per_frame);

  uint32_t lines_per_frame() const { return lines_per_frame_; }

  // Normative sample count of one stored frame (design §2.1).
  uint32_t samples_per_frame() const { return samples_per_frame_; }

  // Nominal, fractional samples per line — 1135.0064 for PAL.
  double nominal_line_length() const;

  // True when every line holds the same whole number of samples.
  bool is_orthogonal() const;

  // First sample of frame line k, counted from the start of the frame.
  // Accepts k == lines_per_frame() as the end sentinel, where it returns
  // samples_per_frame().
  uint32_t line_start(uint32_t line) const;

  // Samples held by frame line k.  For PAL this is 1135 for 621 lines and
  // 1136 for the four lines at which the lattice takes up its accumulated
  // fraction.
  uint32_t line_length(uint32_t line) const;

  // Sub-sample offset of frame line k, in [0, 1) samples.
  //
  // Sample 0 of a stored line is the first sampling instant at or after that
  // line's 0H, so each line begins a fraction of a sample late.  Data placed
  // at a fixed time after 0H therefore lands at
  // t_offset x fs - line_phase(k) samples into the stored line (design §5.2).
  double line_phase(uint32_t line) const;

 private:
  uint32_t lines_per_frame_ = 0;
  uint32_t samples_per_frame_ = 0;
};

// Build the frame lattice for a television system.  Returns false with an
// error message for systems whose geometry the stage does not yet synthesise.
bool make_vbi_frame_geometry(VBITVSystem tv_system,
                             VBIFrameGeometry& out_geometry,
                             std::string& error_message);

// Sampling rate of the output lattice, in Hz: four times the colour
// subcarrier frequency of the television system.
//
// It is the lattice's own rate, not a free choice — the frame is a whole
// number of samples at exactly this rate — so it lives with the geometry.
// Returns false with an error message for systems the stage does not yet
// synthesise.
bool vbi_output_sample_rate_hz(VBITVSystem tv_system, double& out_rate_hz,
                               std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_FRAME_GEOMETRY_H
