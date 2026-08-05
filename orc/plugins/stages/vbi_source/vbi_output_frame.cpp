/*
 * File:        vbi_output_frame.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Sample lattice and amplitude domain of the emitted CVBS frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_output_frame.h"

#include <orc/stage/cvbs_signal_constants.h>

namespace orc {

namespace {

// ETSI EN 300 706 Section 5: WST logic 0 is at black and logic 1 at 66 % of
// the black-to-white excursion, which is 256 + 0.66 x 588 = 644 counts.
constexpr uint16_t kPALWSTLogic0 = static_cast<uint16_t>(kPalBlack);
constexpr uint16_t kPALWSTLogic1 = 644;

}  // namespace

bool make_vbi_output_frame(VBITVSystem tv_system, VBIOutputFrame& out_frame,
                           std::string& error_message) {
  out_frame = VBIOutputFrame{};

  switch (tv_system) {
    case VBITVSystem::kPAL:
      out_frame.system = VideoSystem::PAL;
      out_frame.sample_rate_hz = kPalSampleRate;
      out_frame.lines_per_frame = static_cast<uint32_t>(kPalFrameLines);
      out_frame.samples_per_frame = static_cast<uint32_t>(kPalFrameSamples);
      out_frame.samples_per_line_nominal =
          static_cast<uint32_t>(kPalSamplesPerLineNominal);
      out_frame.levels.blanking = static_cast<uint16_t>(kPalBlanking);
      out_frame.levels.logic0 = kPALWSTLogic0;
      out_frame.levels.logic1 = kPALWSTLogic1;
      return true;

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      // The 525-line lattice and its NABTS amplitude table (blanking 240,
      // logic 1 at 70 IRE) are a data addition that arrives with the 525-line
      // line map; publishing them now would imply support the rest of the
      // stage does not have.
      error_message =
          "Output frames for 525-line systems are not implemented yet; only "
          "PAL frames can currently be emitted.";
      return false;
  }

  error_message = "Unrecognised television system.";
  return false;
}

}  // namespace orc
