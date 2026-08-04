/*
 * File:        vbi_frame_geometry.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Sample lattice of a synthesised CVBS frame: line starts and
 * phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_geometry.h"

namespace orc {

namespace {

// EBU Tech. 3280-E Section 1.2: 625 lines per frame, 1135.0064 samples per
// line at 4 x fsc, which is exactly 709 379 samples per frame.
constexpr uint32_t kPALLinesPerFrame = 625;
constexpr uint32_t kPALSamplesPerFrame = 709379;

}  // namespace

VBIFrameGeometry::VBIFrameGeometry(uint32_t lines_per_frame,
                                   uint32_t samples_per_frame)
    : lines_per_frame_(lines_per_frame),
      samples_per_frame_(samples_per_frame) {}

double VBIFrameGeometry::nominal_line_length() const {
  if (lines_per_frame_ == 0) {
    return 0.0;
  }
  return static_cast<double>(samples_per_frame_) /
         static_cast<double>(lines_per_frame_);
}

bool VBIFrameGeometry::is_orthogonal() const {
  return lines_per_frame_ != 0 && (samples_per_frame_ % lines_per_frame_) == 0;
}

uint32_t VBIFrameGeometry::line_start(uint32_t line) const {
  if (lines_per_frame_ == 0) {
    return 0;
  }
  if (line >= lines_per_frame_) {
    // The end sentinel, and the only sensible answer for an out-of-range
    // line: the frame ends where the lattice says it ends.
    return samples_per_frame_;
  }

  // Ceiling division in exact integer arithmetic.
  const uint64_t numerator = static_cast<uint64_t>(line) * samples_per_frame_;
  const uint64_t denominator = lines_per_frame_;
  return static_cast<uint32_t>((numerator + denominator - 1) / denominator);
}

uint32_t VBIFrameGeometry::line_length(uint32_t line) const {
  if (line >= lines_per_frame_) {
    return 0;
  }
  return line_start(line + 1) - line_start(line);
}

double VBIFrameGeometry::line_phase(uint32_t line) const {
  if (lines_per_frame_ == 0 || line >= lines_per_frame_) {
    return 0.0;
  }

  // line_start(k) - k x samples_per_frame / lines_per_frame, evaluated as a
  // single exact rational so no fraction is lost before the division.
  const int64_t scaled_start =
      static_cast<int64_t>(line_start(line)) * lines_per_frame_;
  const int64_t scaled_nominal =
      static_cast<int64_t>(line) * static_cast<int64_t>(samples_per_frame_);
  return static_cast<double>(scaled_start - scaled_nominal) /
         static_cast<double>(lines_per_frame_);
}

bool make_vbi_frame_geometry(VBITVSystem tv_system,
                             VBIFrameGeometry& out_geometry,
                             std::string& error_message) {
  switch (tv_system) {
    case VBITVSystem::kPAL:
      out_geometry = VBIFrameGeometry(kPALLinesPerFrame, kPALSamplesPerFrame);
      return true;

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      // The orthogonal 910-samples-per-line lattice needs no new arithmetic,
      // only its two constants; frame synthesis for 525-line systems is not
      // implemented yet, so declaring the geometry would imply support that
      // does not exist.
      error_message =
          "Frame geometry for 525-line systems is not implemented yet; only "
          "PAL frames can currently be synthesised.";
      out_geometry = VBIFrameGeometry();
      return false;
  }

  error_message = "Unrecognised television system.";
  out_geometry = VBIFrameGeometry();
  return false;
}

}  // namespace orc
