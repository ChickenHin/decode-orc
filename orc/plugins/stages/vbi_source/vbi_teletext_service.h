/*
 * File:        vbi_teletext_service.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Timing constants of the VBI data services the stage places
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_TELETEXT_SERVICE_H
#define ORC_VBI_TELETEXT_SERVICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {

// Timing of one VBI data service, as the stage needs it to place and measure
// a data line (design §5.2).
//
// The figures follow libzvbi's service table, which is the most complete
// single reference for the 0H-to-clock-run-in anchor.
struct VBITeletextService {
  // Time from 0H to the leading edge of the first clock run-in one bit, at
  // half amplitude, in nanoseconds.
  double t_offset_ns = 0.0;

  double bit_rate_hz = 0.0;

  // Clock run-in length, in bits.  Sixteen alternating bits whose energy sits
  // at half the bit rate.
  uint32_t cri_bits = 0;

  // Framing code length, in bits.
  uint32_t frc_bits = 0;

  // Leading run of one bits at the start of the framing code.  This is the
  // shortest feature in a data line that reaches full amplitude through a
  // band-limited channel, which makes it the only trustworthy logic 1
  // reference on a blurred source (design §5.3.6).
  uint32_t frc_leading_ones = 0;

  // Combined clock run-in and framing code, in transmission order, most
  // significant bit first over cri_bits + frc_bits bits.
  uint32_t cri_frc_pattern = 0;

  // Payload length in bytes, framing code excluded.
  uint32_t payload_bytes = 0;

  // Samples occupied by one transmitted bit at a source's sampling rate.
  double samples_per_bit(double sample_rate_hz) const {
    return (bit_rate_hz > 0.0) ? (sample_rate_hz / bit_rate_hz) : 0.0;
  }

  // Sample offset, within a stored line record, of the leading edge of the
  // first clock run-in one bit.
  //
  // The record's sample 0 sits capture_offset samples after 0H, so the
  // service's 0H-referred anchor moves back by exactly that much.
  double cri_start_samples(double sample_rate_hz,
                           double capture_offset_samples) const {
    return (t_offset_ns * 1e-9 * sample_rate_hz) - capture_offset_samples;
  }
};

// Timing of the data service a source carries.  Returns false with an error
// message for services the stage does not yet place.
bool vbi_teletext_service(VBITVSystem tv_system, VBITeletextSystem tt_system,
                          VBITeletextService& out_service,
                          std::string& error_message);

// A data service the stage does not place, but whose own run-in and framing
// code give an offset estimate that owes nothing to teletext being present
// (design §5.3.5).
//
// A bt8x8 PAL capture covers broadcast lines 7 to 22, which carries the video
// programme system on line 16 and closed captions on line 22, so a capture
// with sparse teletext can still be corroborated.
struct VBIReferenceService {
  std::string name;

  // Broadcast frame line the service occupies, 1-based, counting line 1 as the
  // first line of field 1.
  uint32_t broadcast_line = 0;

  // Time from 0H to the leading edge of the pattern's first one bit, in
  // nanoseconds, on libzvbi's convention.
  double t_offset_ns = 0.0;

  double bit_rate_hz = 0.0;

  // Run-in and framing code in transmission order, most significant bit first
  // over pattern_bits bits.  For the biphase services the pattern is already
  // in channel-bit form, so rendering it as non-return-to-zero at the channel
  // rate reproduces the transmitted waveform.
  uint32_t pattern = 0;
  uint32_t pattern_bits = 0;
};

// Services usable as independent timing references on a television system, in
// broadcast line order.  Empty for systems the stage has no table for.
std::vector<VBIReferenceService> vbi_reference_services(VBITVSystem tv_system);

}  // namespace orc

#endif  // ORC_VBI_TELETEXT_SERVICE_H
