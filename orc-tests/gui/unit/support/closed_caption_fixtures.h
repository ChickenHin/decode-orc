/*
 * File:        closed_caption_fixtures.h
 * Module:      orc-tests/gui/unit
 * Purpose:     Hand-built EIA-608 byte-pair fixtures for closed caption
 *              preview tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc_closed_caption.h>

#include <cstdint>
#include <string>

namespace gui_unit_test {

// EIA-608 control codes as the observer delivers them: the 7-bit values, with
// the transmitted parity bit already stripped (CTA-608-E Table 52, data
// channel 1).
constexpr int32_t kCcControlByte = 0x14;
constexpr int32_t kCcResumeCaptionLoading = 0x20;  // RCL - pop-on mode
constexpr int32_t kCcRollUp2 = 0x25;               // RU2 - roll-up mode
constexpr int32_t kCcEraseDisplayedMemory = 0x2C;  // EDM
constexpr int32_t kCcCarriageReturn = 0x2D;        // CR
constexpr int32_t kCcEndOfCaption = 0x2F;          // EOC - display the caption

// Preamble address code putting the cursor at the bottom row, column 0
// (CTA-608-E Table 53: 0x14 with bit 5 of the second byte set selects row 15,
// and bit 4 set with a zero indent code selects column 0).
constexpr int32_t kCcPacRow15Col0 = 0x70;

// A field carrying one byte pair, both bytes passing their parity check.
inline orc::presenters::ClosedCaptionFieldDataView makeCaptionField(
    int32_t data0, int32_t data1) {
  orc::presenters::ClosedCaptionFieldDataView field;
  field.observed = true;
  field.present = true;
  field.data0 = data0;
  field.data1 = data1;
  field.parity0_valid = true;
  field.parity1_valid = true;
  return field;
}

// A field carrying a byte pair neither byte of which passed its parity check.
inline orc::presenters::ClosedCaptionFieldDataView makeDamagedField(
    int32_t data0, int32_t data1) {
  auto field = makeCaptionField(data0, data1);
  field.parity0_valid = false;
  field.parity1_valid = false;
  return field;
}

// A field the observer looked at and found no caption data on — what every
// field of an uncaptioned recording, and the second field of every NTSC frame,
// looks like.
inline orc::presenters::ClosedCaptionFieldDataView makeEmptyField() {
  orc::presenters::ClosedCaptionFieldDataView field;
  field.observed = true;
  return field;
}

// A two-character caption payload field.
inline orc::presenters::ClosedCaptionFieldDataView makeTextField(char first,
                                                                 char second) {
  return makeCaptionField(static_cast<int32_t>(first),
                          static_cast<int32_t>(second));
}

}  // namespace gui_unit_test
