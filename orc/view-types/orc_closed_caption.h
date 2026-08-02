/*
 * File:        orc_closed_caption.h
 * Module:      orc-view-types
 * Purpose:     Closed caption observation view models for MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

namespace orc::presenters {

/**
 * @brief The closed caption observation of a single field
 *
 * One EIA-608 byte pair per field, exactly as the closed caption observer
 * recovered it from the caption data line (line 21 of NTSC, line 22 of PAL).
 * The bytes are the 7-bit values with the transmitted parity bit already
 * stripped; whether that bit checked out is reported separately, because a
 * byte that failed its check is still worth showing and still tells a viewer
 * something about the recording.
 */
struct ClosedCaptionFieldDataView {
  /// The "closed_caption" namespace exists for this field — the field has been
  /// observed, whatever the observer found
  bool observed = false;
  /// A byte pair was recovered. False on a field the caption stream does not
  /// use (NTSC carries captions on one field of each frame only) as well as on
  /// one whose data line held nothing decodable.
  bool present = false;
  int32_t data0 = 0;           ///< First EIA-608 byte, 7-bit
  int32_t data1 = 0;           ///< Second EIA-608 byte, 7-bit
  bool parity0_valid = false;  ///< data0 passed its odd-parity check
  bool parity1_valid = false;  ///< data1 passed its odd-parity check
};

}  // namespace orc::presenters
