/*
 * File:        orc_audio_views.h
 * Module:      orc-view-types
 * Purpose:     Audio channel-pair view types shared by presenters, GUI and CLI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <string>

namespace orc {

/**
 * @brief One audio channel pair available at a node's output.
 *
 * Every pipeline audio channel pair is stereo, 48 kHz frame-locked and
 * 24-bit-in-int32 (SMPTE 272M-1994 §1.2/§1.3), so a view needs only the pair's
 * identity and provenance to present it. @c index is the pair index at the
 * resolved representation and is what audio sample access is keyed on; it is
 * node-specific and must be re-enumerated when the viewed node changes.
 */
struct AudioPairView {
  size_t index = 0;  ///< Stable pair index at the node (0-based)
  std::string name;  ///< Descriptor name, e.g. "Analogue", "EFM digital audio"
  /// Lower-case AudioOrigin spelling: "analogue", "hifi", "efm", "imported",
  /// "derived" or "unknown".
  std::string origin;
};

}  // namespace orc
