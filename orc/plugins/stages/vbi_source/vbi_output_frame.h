/*
 * File:        vbi_output_frame.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Sample lattice and amplitude domain of the emitted CVBS frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_OUTPUT_FRAME_H
#define ORC_VBI_OUTPUT_FRAME_H

#include <orc/stage/common_types.h>
#include <orc/support/frame_line_util.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "vbi_source_format.h"

namespace orc {

// The CVBS_U10_4FSC sample encoding reserves the extremes of the 10-bit word.
// Nothing the stage writes may land in either protected range (design §2.2),
// so every sample passes through clamp_vbi_output_sample() as the last step of
// the sample path.
constexpr int16_t kVBIOutputSampleMin = 4;
constexpr int16_t kVBIOutputSampleMax = 1019;

// Amplitudes the stage writes, in CVBS_U10_4FSC counts (design §2.2).
//
// The stage places recovered data on an otherwise blank frame, so it needs
// three levels and no more: what the rest of the frame sits at, and the two the
// data service's logic levels map onto.
struct VBIOutputLevels {
  uint16_t blanking = 0;
  uint16_t logic0 = 0;
  uint16_t logic1 = 0;

  // Peak-to-peak amplitude of the data signal, in counts.
  int32_t data_amplitude() const {
    return static_cast<int32_t>(logic1) - static_cast<int32_t>(logic0);
  }
};

// The frames the stage emits: their lattice and their amplitude domain.
//
// Line offsets follow the host's flat-frame convention (frame_line_util.h):
// every line holds samples_per_line_nominal samples except the last line of
// each PAL field, which holds two more.  That convention is what the teletext
// observer, the preview and the CVBS sink all read a line by, so following it
// here is what makes a placed data line land where they look for it.
//
// It is not quite the 0H lattice of a real 4FSC PAL frame, which repeats at
// frame rate rather than line rate (EBU Tech. 3280-E: 1135.0064 samples per
// line).  Over the teletext lines the two differ by at most 0.15 samples —
// 8 ns, a sixteenth of a bit period — because the flat convention takes up its
// accumulated fraction at the foot of each field, below every line the stage
// writes.  A real time-base corrected capture is stored on exactly the same
// convention, so matching it is both simpler and more faithful than modelling
// the true lattice would be.
struct VBIOutputFrame {
  VideoSystem system = VideoSystem::PAL;
  double sample_rate_hz = 0.0;
  uint32_t lines_per_frame = 0;
  uint32_t samples_per_frame = 0;
  uint32_t samples_per_line_nominal = 0;
  VBIOutputLevels levels{};

  // First sample of frame line k, counted from the start of the frame.
  size_t line_offset(uint32_t line) const {
    return frame_line_sample_offset(system, samples_per_line_nominal, line);
  }

  // Samples held by frame line k.
  size_t line_length(uint32_t line) const {
    return frame_line_sample_count(system, samples_per_line_nominal, line);
  }
};

// Describe the output frames a system pairing is placed on.  Returns false with
// an error message for systems the stage does not yet emit.
//
// The data service enters because the two services do not define their logic
// levels against the same reference: WST measures from black on a black-to-
// white scale, NABTS from blanking on an IRE one.  The two coincide on PAL,
// where black is blanking, and differ by the 7,5 IRE setup on NTSC.
bool make_vbi_output_frame(VBITVSystem tv_system, VBITeletextSystem tt_system,
                           VBIOutputFrame& out_frame,
                           std::string& error_message);

// Round a placed sample to the stored 10-bit word and hold it inside the legal
// range.
//
// This is the final operation of the sample path, applied after resampling:
// overshoot from the anti-alias filter can push a sharp clock run-in edge
// outside the nominal band, and a protected value must never reach the file.
// The value is clamped, never rescaled or wrapped, so nothing outside the
// overshoot itself is altered.  A non-finite value is not signal and is
// treated as the low bound.
//
// Defined here rather than out of line because it runs once per placed sample.
inline int16_t clamp_vbi_output_sample(double value) {
  // The low comparison is written so that a NaN, which compares false against
  // everything, takes the same path as an under-range value.
  if (!(value > static_cast<double>(kVBIOutputSampleMin))) {
    return kVBIOutputSampleMin;
  }
  if (value > static_cast<double>(kVBIOutputSampleMax)) {
    return kVBIOutputSampleMax;
  }
  return static_cast<int16_t>(value + 0.5);
}

}  // namespace orc

#endif  // ORC_VBI_OUTPUT_FRAME_H
