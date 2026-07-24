/*
 * File:        tbc_level_derivation.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     Derive ld-decode 16-bit domain levels from TBC metadata
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "tbc_metadata_types.h"

namespace orc {

// SMPTE 170M-2004 §4.4: standard NTSC picture black sits on a 7.5 IRE setup
// pedestal above the 0 IRE blanking level.  NTSC-J (Japanese NTSC) has no
// setup — picture black IS the blanking level.  A measured setup below half
// the standard pedestal is classified as NTSC-J.
constexpr double kNtscSetupIre = 7.5;
constexpr double kNtscJSetupThresholdIre = kNtscSetupIre / 2.0;

// Derive the ld-decode 16-bit domain levels from the black16bIre /
// white16bIre pair when the metadata carries no explicit blanking level
// (the ld-decode JSON always; the pre-blanking-column SQLite schema).
//
// PAL has no setup pedestal, so black16bIre IS the 0 IRE blanking level.
// For NTSC and PAL_M, black16bIre is normally the 7.5 IRE setup pedestal
// (kTbcNtscBlack), so blanking is derived as
//   blanking = black − 7.5 × (white − black) / 92.5
// (92.5 = IRE span from 7.5 IRE black to 100 IRE white; SMPTE 170M-2004
// Table 1 / SMPTE 244M-2003 §4.2.1).
//
// NTSC-J captures instead store picture black at the 0 IRE blanking level
// (kTbcNtscBlanking) with no setup.  Since black/white alone cannot say
// which convention applies, both interpretations are evaluated and the one
// whose implied blanking lands closer to the nominal ld-decode 16-bit
// blanking (kTbcNtscBlanking; the .tbc output domain is fixed at
// CVBS_U10_4FSC × 64) is chosen.  black_16b is always populated so callers
// can detect the NTSC-J case via is_ntsc_j_black_level().
inline TbcDomainLevels derive_tbc_domain_levels(VideoSystem system,
                                                int32_t black_16b,
                                                int32_t white_16b) {
  TbcDomainLevels levels;
  levels.white_16b = white_16b;
  levels.black_16b = black_16b;

  if (system == VideoSystem::NTSC || system == VideoSystem::PAL_M) {
    const double units_per_ire =
        static_cast<double>(white_16b - black_16b) / (100.0 - kNtscSetupIre);
    const int32_t blanking_with_setup = static_cast<int32_t>(
        std::round(black_16b - kNtscSetupIre * units_per_ire));
    if (system == VideoSystem::NTSC &&
        std::abs(black_16b - kTbcNtscBlanking) <
            std::abs(blanking_with_setup - kTbcNtscBlanking)) {
      // NTSC-J: picture black at 0 IRE — black IS blanking.
      levels.blanking_16b = black_16b;
    } else {
      levels.blanking_16b = blanking_with_setup;
    }
  } else {
    // PAL: no setup pedestal; black == blanking (0 IRE).
    levels.blanking_16b = black_16b;
  }
  return levels;
}

// True when the levels describe an NTSC-J (0 IRE) picture black: the setup
// measured between black and blanking is below half the SMPTE 170M 7.5 IRE
// pedestal.  Requires black_16b; returns false when it is absent or the
// levels are invalid.
inline bool is_ntsc_j_black_level(const TbcDomainLevels& levels) {
  if (!levels.black_16b.has_value() || !levels.is_valid()) return false;
  const double setup_ire =
      static_cast<double>(*levels.black_16b - levels.blanking_16b) * 100.0 /
      static_cast<double>(levels.white_16b - levels.blanking_16b);
  return setup_ire < kNtscJSetupThresholdIre;
}

}  // namespace orc
