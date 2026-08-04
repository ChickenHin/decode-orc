/*
 * File:        vbi_teletext_service.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Timing constants of the VBI data services the stage places
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_teletext_service.h"

#include <utility>

namespace orc {

namespace {

// ETSI EN 300 706 Section 6: Teletext System B on 625 lines.  The 0H anchor
// is libzvbi's, which measures to the leading edge of the first clock run-in
// one bit; EN 300 706 states the same timing from a different reference and
// derives 10.198 us, a difference of well under one bit period.  The nominal
// is configured at libzvbi's value for compatibility with the rest of the
// ecosystem and settled from the data by calibration.
constexpr double kWSTOffsetNs = 10300.0;

// 444 x fH, the line frequency being 15 625 Hz.
constexpr double kWSTBitRateHz = 6937500.0;

constexpr uint32_t kWSTCRIBits = 16;
constexpr uint32_t kWSTFRCBits = 8;

// Framing code 0xE4, transmitted 11100100: three ones, which is what breaks
// the clock run-in's alternation and gives an unambiguous absolute position.
constexpr uint32_t kWSTFRCLeadingOnes = 3;

// libzvbi cri_frc for System B: 0xAAAAE4 over 24 bits.
constexpr uint32_t kWSTCRIFRCPattern = 0xAAAAE4u;

constexpr uint32_t kWSTPayloadBytes = 42;

// ---------------------------------------------------------------------------
// Independent timing references (design §5.3.5).
//
// The figures are libzvbi's service table entries for the services that share
// the 625-line teletext range.  They are used only to corroborate a fitted
// capture offset, never to place anything, which is why a coarse channel-bit
// rendering of each run-in is sufficient.
// ---------------------------------------------------------------------------

// ETSI EN 300 231: video programme system, broadcast line 16 of field 1.
constexpr uint32_t kVPSLine = 16;
constexpr double kVPSOffsetNs = 12500.0;
constexpr double kVPSBitRateHz = 2500000.0;
constexpr uint32_t kVPSPattern = 0xAAAA8A99u;
constexpr uint32_t kVPSPatternBits = 32;

// ITU-R BT.1119 / EIA-608 style captioning on 625 lines, broadcast lines 22
// and 335.
constexpr uint32_t kCaptionField1Line = 22;
constexpr uint32_t kCaptionField2Line = 335;
constexpr double kCaptionOffsetNs = 10500.0;
constexpr double kCaptionBitRateHz = 500000.0;
constexpr uint32_t kCaptionPattern = 0x5551u;
constexpr uint32_t kCaptionPatternBits = 16;

}  // namespace

bool vbi_teletext_service(VBITVSystem tv_system, VBITeletextSystem tt_system,
                          VBITeletextService& out_service,
                          std::string& error_message) {
  out_service = VBITeletextService{};

  if (tv_system == VBITVSystem::kPAL && tt_system == VBITeletextSystem::kWST) {
    out_service.t_offset_ns = kWSTOffsetNs;
    out_service.bit_rate_hz = kWSTBitRateHz;
    out_service.cri_bits = kWSTCRIBits;
    out_service.frc_bits = kWSTFRCBits;
    out_service.frc_leading_ones = kWSTFRCLeadingOnes;
    out_service.cri_frc_pattern = kWSTCRIFRCPattern;
    out_service.payload_bytes = kWSTPayloadBytes;
    return true;
  }

  if ((tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) &&
      tt_system == VBITeletextSystem::kNABTS) {
    // ITU-R BT.653 System C: 10 480 ns, 5 727 272 bit/s, framing code 0xE7.
    // Withheld until 525-line placement and synthesis exist, so that an NABTS
    // configuration fails with a clear error rather than producing plausible
    // but wrong output.
    error_message =
        "Teletext system NABTS is not implemented yet; only WST captures can "
        "currently be placed.";
    return false;
  }

  error_message =
      "The configured teletext system is not defined on the configured "
      "television system; WST is defined on PAL and NABTS on NTSC/PAL_M.";
  return false;
}

std::vector<VBIReferenceService> vbi_reference_services(VBITVSystem tv_system) {
  if (tv_system != VBITVSystem::kPAL) {
    // The 525-line references arrive with the 525-line placement they would
    // be checked against; claiming none is honest, and the cross-checks are
    // reported as inapplicable rather than as agreement.
    return {};
  }

  std::vector<VBIReferenceService> services;

  VBIReferenceService vps;
  vps.name = "VPS";
  vps.broadcast_line = kVPSLine;
  vps.t_offset_ns = kVPSOffsetNs;
  vps.bit_rate_hz = kVPSBitRateHz;
  vps.pattern = kVPSPattern;
  vps.pattern_bits = kVPSPatternBits;
  services.push_back(std::move(vps));

  for (const uint32_t line : {kCaptionField1Line, kCaptionField2Line}) {
    VBIReferenceService caption;
    caption.name = "Closed Caption 625";
    caption.broadcast_line = line;
    caption.t_offset_ns = kCaptionOffsetNs;
    caption.bit_rate_hz = kCaptionBitRateHz;
    caption.pattern = kCaptionPattern;
    caption.pattern_bits = kCaptionPatternBits;
    services.push_back(std::move(caption));
  }

  return services;
}

}  // namespace orc
