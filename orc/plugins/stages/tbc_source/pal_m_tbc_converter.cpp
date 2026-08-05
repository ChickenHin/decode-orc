/*
 * File:        pal_m_tbc_converter.cpp
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     PAL_M TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "pal_m_tbc_converter.h"

#include <cmath>
#include <stdexcept>

namespace orc {

// ---------------------------------------------------------------------------
// Level mapping
// ---------------------------------------------------------------------------

// tbc_to_cvbs lives in the header — see the note there on why.

// ---------------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------------

std::vector<int16_t> PalMTBCConverter::assemble_frame(
    const std::vector<uint16_t>& tbc_field1,  // 263 × 909 samples
    const std::vector<uint16_t>& tbc_field2,  // 262 × 909 samples
    int32_t tbc_blanking, int32_t tbc_white) {
  // ITU-R BT.1700-1 Annex 1 Part B §1: PAL_M frame assembly.
  // TBC field ordering (ld-decode convention, identical to NTSC):
  //   TBC field 1 (even file index) = odd-scan/first temporal, 263 real lines
  //     → VFR field 1 (top spatial).
  //   TBC field 2 (odd file index) = even-scan/second temporal, 262 real lines
  //     → VFR field 2 (bottom spatial).
  // VFR layout: [field 1 (top, 263 lines)][field 2 (bottom, 262 lines)]
  // Orthogonal: 909 samples/line.
  constexpr int32_t kTBCF1Lines = kPalMField1Lines;                    // 263
  constexpr int32_t kTBCF2Lines = kPalMFrameLines - kPalMField1Lines;  // 262
  constexpr int32_t kLineW = kPalMSamplesPerLine;                      // 909

  const size_t exp_f1 =
      static_cast<size_t>(kTBCF1Lines) * static_cast<size_t>(kLineW);
  const size_t exp_f2 =
      static_cast<size_t>(kTBCF2Lines) * static_cast<size_t>(kLineW);

  if (tbc_field1.size() != exp_f1 || tbc_field2.size() != exp_f2) {
    throw std::invalid_argument(
        "PalMTBCConverter::assemble_frame: unexpected field sample counts");
  }

  // Sized up front and written through a cursor rather than push_back: the
  // output length is fixed, so the per-sample capacity check buys nothing.
  std::vector<int16_t> frame(static_cast<size_t>(kPalMFrameSamples));
  int16_t* dst = frame.data();

  // Level map built once for the whole frame — the source levels are
  // constant across it, so the division belongs here and not in the
  // sample loops.
  const TbcLevelScale levels = level_scale(tbc_blanking, tbc_white);

  // VFR field 1 (top, 263 lines) ← TBC field 1 (odd-scan, first temporal)
  for (size_t i = 0; i < exp_f1; ++i) {
    dst[i] = levels.map(tbc_field1[i]);
  }
  dst += exp_f1;

  // VFR field 2 (bottom, 262 lines) ← TBC field 2 (even-scan, second temporal)
  for (size_t i = 0; i < exp_f2; ++i) {
    dst[i] = levels.map(tbc_field2[i]);
  }

  return frame;
}

}  // namespace orc
