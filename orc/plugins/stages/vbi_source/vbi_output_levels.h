/*
 * File:        vbi_output_levels.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Amplitude domain of the synthesised CVBS output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_OUTPUT_LEVELS_H
#define ORC_VBI_OUTPUT_LEVELS_H

#include <cstdint>
#include <string>

#include "vbi_source_format.h"

namespace orc {

// The CVBS_U10_4FSC sample encoding reserves the extremes of the 10-bit word.
// Nothing the stage writes may land in either protected range (design §2.2),
// so every sample passes through clamp_vbi_output_sample() as the last step
// of the sample path.
constexpr uint16_t kVBIOutputSampleMin = 4;
constexpr uint16_t kVBIOutputSampleMax = 1019;

// Normative amplitudes of the output video standard preset, in
// CVBS_U10_4FSC counts (design §2.2).
struct VBIOutputLevels {
  uint16_t sync_tip = 0;
  uint16_t blanking = 0;
  uint16_t black = 0;
  uint16_t white = 0;

  // The two levels the level mapper actually targets: the data service's
  // logic 0 and logic 1.
  uint16_t logic0 = 0;
  uint16_t logic1 = 0;

  // Peak-to-peak amplitude of the data signal, in counts.
  int32_t data_amplitude() const {
    return static_cast<int32_t>(logic1) - static_cast<int32_t>(logic0);
  }
};

// Amplitude table of a television system.  Returns false with an error
// message for systems the stage does not yet synthesise.
bool vbi_output_levels(VBITVSystem tv_system, VBIOutputLevels& out_levels,
                       std::string& error_message);

// Round a synthesised sample to the stored 10-bit word and hold it inside the
// legal range.
//
// This is the final operation of the sample path, applied after resampling:
// overshoot from the anti-alias filter can push a sharp clock run-in edge
// outside the nominal band, and a protected value must never reach the file.
// The value is clamped, never rescaled or wrapped, so nothing outside the
// overshoot itself is altered.  A non-finite value is not signal and is
// treated as the low bound.
uint16_t clamp_vbi_output_sample(double value);

}  // namespace orc

#endif  // ORC_VBI_OUTPUT_LEVELS_H
