/*
 * File:        teletext_slicer.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     PAL WST (System B) teletext data-line slicer producing T42
 *              packets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SLICER_H
#define ORC_TELETEXT_SLICER_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace orc {

// ETSI EN 300 706 §5.3: bit rate = 444 × nominal fH = 6,9375 Mbit/s ± 25 ppm.
constexpr double kTeletextBitRate = 6'937'500.0;

// ETSI EN 300 706 §7.1: a teletext packet comprises 360 bits organized as 45
// bytes; removing the clock run-in (2 bytes, §6.1) and framing code (1 byte,
// §6.2) leaves the 42-byte MRAG + data payload — the T42 packet.
constexpr size_t kTeletextPacketBytes = 42;

// Encode a 4-bit value as a Hamming 8/4 protected byte.
// ETSI EN 300 706 §8.2: bits 1, 3, 5, 7 (LSB numbering, transmission order)
// carry the protection bits P1-P4 and bits 2, 4, 6, 8 the data bits D1-D4.
// Only the low nibble of |value| is used.
uint8_t teletext_hamming84_encode(uint8_t value);

// Decode a Hamming 8/4 protected byte.
// ETSI EN 300 706 §8.2: single-bit errors are identified and corrected;
// double-bit errors are detected. Returns the decoded 4-bit value (0-15), or
// -1 when the byte is uncorrectable (double-bit error).
int teletext_hamming84_decode(uint8_t byte);

// Encode a 42-byte T42 packet as 84 lowercase hex characters. Shared between
// the teletext observer (producer) and the teletext sink (consumer) so the
// observation-string representation has a single definition.
std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes);

// Decode 84 hex characters (either case) back to the 42 packet bytes.
// Returns std::nullopt when the length or any character is invalid.
std::optional<std::array<uint8_t, kTeletextPacketBytes>> teletext_hex_to_packet(
    std::string_view hex);

// Result of slicing one candidate VBI line.
struct TeletextLineResult {
  // True when the clock run-in and framing code were found and the payload
  // was extracted (subject to the optional MRAG plausibility filter).
  bool valid = false;

  // MRAG + 40 data bytes in transmission coding (Hamming 8/4 on addressing
  // bytes, odd parity on display bytes). No error correction is applied to
  // the payload: the T42 contract preserves transmission coding.
  std::array<uint8_t, kTeletextPacketBytes> bytes{};

  // Number of bit errors accepted in the framing code: 0, or 1 when the
  // slicer runs in tolerant-framing mode.
  int framing_bit_errors = 0;

  // Sample position (fractional) where the framing code ended and the payload
  // began. Diagnostics only.
  double data_start_sample = 0.0;
};

// Slicer tuning options. Defaults match the strictest behaviour: exact
// framing-code match and MRAG plausibility filtering enabled.
struct TeletextSlicerOptions {
  // Accept a framing code with one bit error (ETSI EN 300 706 §6.2 defines
  // the exact 8-bit pattern; some receivers tolerate a single error, at the
  // cost of a higher false-positive rate on noisy sources).
  bool tolerant_framing = false;

  // Require both MRAG bytes (ETSI EN 300 706 §7.1.2) to survive Hamming 8/4
  // correction (§8.2) at the chosen byte alignment. Single-bit-damaged
  // packets still pass — Hamming 8/4 corrects those — but noise that happens
  // to spell the framing code does not, which is what keeps the framing-code
  // search window safe. A packet whose addressing is unrecoverable carries no
  // usable payload anyway: TeletextPageDecoder drops it. The MRAG bytes in
  // the output remain uncorrected (transmission coding).
  bool require_valid_mrag = true;
};

/**
 * @brief PAL WST teletext data-line slicer.
 *
 * Recovers 42-byte T42 packets (MRAG + data, transmission coding) from
 * single VBI lines of 4FSC-sampled PAL video. At the decode-orc PAL sample
 * rate one teletext bit spans ≈ 2.556 samples, so recovery uses clock run-in
 * correlation and interpolated bit-centre sampling rather than a
 * transition-map approach.
 *
 * Thread safety: slice() is const and the class holds no mutable state; a
 * single instance may be used concurrently from multiple threads.
 */
class TeletextSlicer {
 public:
  // |sample_rate| in Hz (e.g. kPalSampleRate = 17,734,475 Hz).
  // |bit_rate| fixed at 444 × fH by ETSI EN 300 706 §5.3; overridable for
  // tests only.
  explicit TeletextSlicer(double sample_rate,
                          double bit_rate = kTeletextBitRate,
                          TeletextSlicerOptions options = {});

  // Slice one candidate VBI line of |sample_count| samples in the
  // CVBS_U10_4FSC 10-bit level domain. |black_level| and |white_level| locate
  // the data levels of ETSI EN 300 706 §5.2 (0 = black, 1 = 66 % of
  // black-to-white). Returns a result with valid == false when the line
  // carries no recoverable teletext packet.
  TeletextLineResult slice(const int16_t* line, size_t sample_count,
                           int16_t black_level, int16_t white_level) const;

 private:
  double sample_rate_;
  double samples_per_bit_;
  TeletextSlicerOptions options_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SLICER_H
