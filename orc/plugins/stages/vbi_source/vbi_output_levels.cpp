/*
 * File:        vbi_output_levels.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Amplitude domain of the synthesised CVBS output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_output_levels.h"

#include <cmath>

namespace orc {

namespace {

// CVBS File Format Specification, PAL video standard preset, CVBS_U10_4FSC
// sample encoding: sync tip 4, blanking and black 256 (no pedestal), white
// 844.
constexpr uint16_t kPALSyncTip = 4;
constexpr uint16_t kPALBlanking = 256;
constexpr uint16_t kPALBlack = 256;
constexpr uint16_t kPALWhite = 844;

// ETSI EN 300 706 Section 5: WST logic 0 is at black and logic 1 at 66 % of
// the black-to-white excursion, which is 256 + 0.66 x 588 = 644 counts.
constexpr uint16_t kPALWSTLogic0 = kPALBlack;
constexpr uint16_t kPALWSTLogic1 = 644;

}  // namespace

bool vbi_output_levels(VBITVSystem tv_system, VBIOutputLevels& out_levels,
                       std::string& error_message) {
  out_levels = VBIOutputLevels{};

  switch (tv_system) {
    case VBITVSystem::kPAL:
      out_levels.sync_tip = kPALSyncTip;
      out_levels.blanking = kPALBlanking;
      out_levels.black = kPALBlack;
      out_levels.white = kPALWhite;
      out_levels.logic0 = kPALWSTLogic0;
      out_levels.logic1 = kPALWSTLogic1;
      return true;

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      // The 525-line table (sync tip 16, blanking 240, black 282, white 800,
      // NABTS logic 1 at 70 IRE) is a data addition that arrives with 525-line
      // frame synthesis; publishing it now would imply support that the rest
      // of the stage does not have.
      error_message =
          "Output levels for 525-line systems are not implemented yet; only "
          "PAL output can currently be synthesised.";
      return false;
  }

  error_message = "Unrecognised television system.";
  return false;
}

uint16_t clamp_vbi_output_sample(double value) {
  // The low comparison is written so that a NaN, which compares false against
  // everything, takes the same path as an under-range value.
  if (!(value > static_cast<double>(kVBIOutputSampleMin))) {
    return kVBIOutputSampleMin;
  }
  if (value > static_cast<double>(kVBIOutputSampleMax)) {
    return kVBIOutputSampleMax;
  }
  return static_cast<uint16_t>(std::lround(value));
}

}  // namespace orc
