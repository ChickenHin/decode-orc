/*
 * File:        teletext_slicer.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     PAL WST (System B) teletext data-line slicer producing T42
 *              packets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

// ETSI EN 300 706 §6.1: 16-bit clock run-in, transmission order 1010…1010
// (first transmitted bit is '1'; even bit indices are ones).
constexpr int kRunInBits = 16;

// ETSI EN 300 706 §6.2: 8-bit framing code, transmission order 11100100
// (conventionally written 0xE4 MSB-first).
constexpr int kFramingBits = 8;
constexpr int kFramingCodeBits[kFramingBits] = {1, 1, 1, 0, 0, 1, 0, 0};

// ETSI EN 300 706 §7.1: 360-bit packet minus run-in (16) and framing (8)
// leaves 336 payload bits = 42 bytes, transmitted LSB first per byte.
constexpr int kPayloadBits = 336;

// ETSI EN 300 706 §5.2: data '1' level is 66 % of the black-to-white
// difference (the '0' level is black).
constexpr double kDataOneLevelFraction = 0.66;

// Minimum recovered data amplitude, as a fraction of the nominal §5.2 '1'
// level, below which a line is treated as empty. Implementation choice: half
// the nominal amplitude rejects blank and noise-only lines cheaply while
// accepting the §5.2 tolerance range (66 ± 6 %) with wide margin.
constexpr double kAmplitudeGateFraction = 0.5;

// Search window for the centre of the first clock run-in bit, in µs from the
// start of the line. ETSI EN 300 706 §6.3: the timing reference (mid-point of
// the penultimate '1' of the run-in, i.e. run-in bit 15 of 16) is nominally
// 12,0 µs after the half-amplitude point of the sync leading edge, placing
// the first bit centre 14 bit periods (≈ 2,02 µs) earlier, at ≈ 9,98 µs. The
// §6.3 note allows departures for network re-timing, so the window spans
// ± 2 µs around nominal; the lower bound also clears the PAL colour burst
// (which ends ≈ 7,8 µs into the line).
constexpr double kRunInSearchStartUs = 8.0;
constexpr double kRunInSearchEndUs = 12.0;

// Correlation phase-search step in samples. At ≈ 2.556 samples/bit a quarter
// sample bounds the bit-centre placement error at ≈ 5 % of a bit period.
constexpr double kPhaseSearchStep = 0.25;

// Minimum run-in bits that must match the alternating pattern after
// thresholding. ETSI EN 300 706 §6.1 note: the two leading data ones may be
// absent or reduced in amplitude, so up to two mismatches are allowed.
constexpr int kMinRunInMatches = 14;

// Framing-code search range around the nominal position, in bit periods.
// The run-in correlation is ambiguous to even-bit shifts (the alternating
// kernel re-aligns every 2 bits), and on real recordings the correlation peak
// lands up to four bits from the true run-in start — the §6.3 note allows the
// insertion point to move for network re-timing, and the kernel also
// correlates against the framing code and the leading payload. A ± 2 window
// therefore misses the framing code outright on a third of otherwise perfect
// lines, so the search spans ± 4 bit positions.
constexpr int kFramingSearchBits = 4;

// Linear interpolation between adjacent samples at fractional position |t|.
// Caller guarantees t >= 0 and t + 1 < sample_count.
inline double sample_at(const int16_t* line, double t) {
  const auto i = static_cast<size_t>(t);
  const double frac = t - static_cast<double>(i);
  return static_cast<double>(line[i]) +
         (static_cast<double>(line[i + 1]) - static_cast<double>(line[i])) *
             frac;
}

}  // namespace

uint8_t teletext_hamming84_encode(uint8_t value) {
  // ETSI EN 300 706 §8.2 encoding equations. Bit numbering: spec bit 1 (first
  // transmitted) is the byte LSB, so P1..P4 occupy bits 0/2/4/6 and D1..D4
  // bits 1/3/5/7.
  const int d1 = (value >> 0) & 1;
  const int d2 = (value >> 1) & 1;
  const int d3 = (value >> 2) & 1;
  const int d4 = (value >> 3) & 1;
  const int p1 = 1 ^ d1 ^ d3 ^ d4;
  const int p2 = 1 ^ d1 ^ d2 ^ d4;
  const int p3 = 1 ^ d1 ^ d2 ^ d3;
  const int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
  return static_cast<uint8_t>((p1 << 0) | (d1 << 1) | (p2 << 2) | (d2 << 3) |
                              (p3 << 4) | (d3 << 5) | (p4 << 6) | (d4 << 7));
}

int teletext_hamming84_decode(uint8_t byte) {
  // ETSI EN 300 706 §8.2: Hamming 8/4 has minimum distance 4, so every byte
  // within Hamming distance 1 of a codeword decodes to that codeword (single
  // errors corrected, including protection-bit errors) and every byte at
  // distance 2 is uncorrectable (double error detected). A 256-entry table
  // realises exactly that decision rule.
  static const auto kTable = [] {
    std::array<int8_t, 256> table{};
    table.fill(-1);
    for (int value = 0; value < 16; ++value) {
      const uint8_t code =
          teletext_hamming84_encode(static_cast<uint8_t>(value));
      table[code] = static_cast<int8_t>(value);
      for (int bit = 0; bit < 8; ++bit) {
        table[code ^ (1u << bit)] = static_cast<int8_t>(value);
      }
    }
    return table;
  }();
  return kTable[byte];
}

std::string teletext_packet_to_hex(
    const std::array<uint8_t, kTeletextPacketBytes>& bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(kTeletextPacketBytes * 2);
  for (const uint8_t byte : bytes) {
    hex.push_back(kHexDigits[byte >> 4]);
    hex.push_back(kHexDigits[byte & 0x0F]);
  }
  return hex;
}

std::optional<std::array<uint8_t, kTeletextPacketBytes>> teletext_hex_to_packet(
    std::string_view hex) {
  if (hex.size() != kTeletextPacketBytes * 2) {
    return std::nullopt;
  }
  const auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::array<uint8_t, kTeletextPacketBytes> bytes{};
  for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
    const int high = nibble(hex[i * 2]);
    const int low = nibble(hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    bytes[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return bytes;
}

TeletextSlicer::TeletextSlicer(double sample_rate, double bit_rate,
                               TeletextSlicerOptions options)
    : sample_rate_(sample_rate),
      samples_per_bit_(sample_rate / bit_rate),
      options_(options) {}

TeletextLineResult TeletextSlicer::slice(const int16_t* line,
                                         size_t sample_count,
                                         int16_t black_level,
                                         int16_t white_level) const {
  TeletextLineResult result;

  const double spb = samples_per_bit_;
  // Whole packet (360 bits, §7.1) must fit after the earliest search start.
  const double search_start = kRunInSearchStartUs * sample_rate_ / 1e6;
  const double min_samples =
      search_start + (kRunInBits + kFramingBits + kPayloadBits) * spb + 2.0;
  if (line == nullptr || static_cast<double>(sample_count) < min_samples) {
    return result;
  }

  // ETSI EN 300 706 §5.2: nominal '1' amplitude above black level.
  const double nominal_amplitude =
      kDataOneLevelFraction *
      (static_cast<double>(white_level) - static_cast<double>(black_level));
  if (nominal_amplitude <= 0.0) {
    return result;
  }
  const double amplitude_gate = kAmplitudeGateFraction * nominal_amplitude;

  // Step 1 — coarse gate: most VBI lines are empty; reject immediately when
  // the line never rises meaningfully above black level.
  int16_t peak = black_level;
  const auto gate_begin = static_cast<size_t>(search_start);
  for (size_t i = gate_begin; i < sample_count; ++i) {
    peak = std::max(peak, line[i]);
  }
  if (static_cast<double>(peak) - static_cast<double>(black_level) <
      amplitude_gate) {
    return result;
  }

  // Step 2 — clock run-in acquisition (§6.1): correlate against a ±
  // alternating kernel at the known bit period across the §6.3 timing window.
  // The correlation peak yields the bit phase; the recovered 0/1 levels set
  // an adaptive slicing threshold local to the data burst.
  const double search_end = kRunInSearchEndUs * sample_rate_ / 1e6;
  double best_corr = 0.0;
  double best_t0 = -1.0;
  for (double t0 = search_start; t0 <= search_end; t0 += kPhaseSearchStep) {
    double corr = 0.0;
    for (int k = 0; k < kRunInBits; ++k) {
      const double s = sample_at(line, t0 + k * spb);
      corr += (k % 2 == 0) ? s : -s;  // §6.1: even bit indices are ones
    }
    if (corr > best_corr) {
      best_corr = corr;
      best_t0 = t0;
    }
  }
  if (best_t0 < 0.0) {
    return result;
  }

  double ones_level = 0.0;
  double zeros_level = 0.0;
  for (int k = 0; k < kRunInBits; ++k) {
    const double s = sample_at(line, best_t0 + k * spb);
    ((k % 2 == 0) ? ones_level : zeros_level) += s;
  }
  ones_level /= kRunInBits / 2.0;
  zeros_level /= kRunInBits / 2.0;
  const double amplitude = ones_level - zeros_level;
  if (amplitude < amplitude_gate) {
    return result;
  }
  const double threshold = 0.5 * (ones_level + zeros_level);

  // Validate the run-in bit pattern at the locked phase (§6.1, allowing for
  // the note's reduced leading ones).
  int run_in_matches = 0;
  for (int k = 0; k < kRunInBits; ++k) {
    const int bit = sample_at(line, best_t0 + k * spb) > threshold ? 1 : 0;
    run_in_matches += (bit == (k % 2 == 0 ? 1 : 0)) ? 1 : 0;
  }
  if (run_in_matches < kMinRunInMatches) {
    return result;
  }

  // Step 3 — framing-code lock (§6.2): resolve the run-in's even-bit-shift
  // ambiguity by searching for the framing code around the correlation lock.
  // Candidates are ranked by framing-code bit errors first, then by whether
  // the MRAG that follows is Hamming 8/4 decodable (§7.1.2, §8.2), then by
  // proximity to the lock. The MRAG tie-break matters because widening the
  // search also widens the chance of the payload happening to spell the
  // framing code; an alignment whose address bytes decode is the real one.
  const int max_framing_errors = options_.tolerant_framing ? 1 : 0;
  const auto bit_at = [&](double t) {
    return sample_at(line, t) > threshold ? 1 : 0;
  };
  const auto payload_start = [&](int shift) {
    return best_t0 + (kRunInBits + shift + kFramingBits) * spb;
  };

  int best_shift = 0;
  int best_errors = kFramingBits + 1;
  bool best_mrag_ok = false;
  bool found = false;
  for (int shift = -kFramingSearchBits; shift <= kFramingSearchBits; ++shift) {
    int errors = 0;
    for (int k = 0; k < kFramingBits; ++k) {
      errors += (bit_at(best_t0 + (kRunInBits + shift + k) * spb) !=
                 kFramingCodeBits[k])
                    ? 1
                    : 0;
    }
    if (errors > max_framing_errors) {
      continue;
    }
    // The whole packet must fit at this alignment (§7.1).
    const double start = payload_start(shift);
    if (start < 0.0 || start + (kPayloadBits - 1) * spb + 1.0 >=
                           static_cast<double>(sample_count)) {
      continue;
    }

    std::array<uint8_t, 2> mrag{};
    for (int n = 0; n < 16; ++n) {
      if (bit_at(start + n * spb) != 0) {
        mrag[static_cast<size_t>(n) >> 3] |=
            static_cast<uint8_t>(1u << (n & 7));
      }
    }
    const bool mrag_ok = teletext_hamming84_decode(mrag[0]) >= 0 &&
                         teletext_hamming84_decode(mrag[1]) >= 0;
    if (options_.require_valid_mrag && !mrag_ok) {
      continue;
    }

    const bool better = !found || errors < best_errors ||
                        (errors == best_errors && mrag_ok && !best_mrag_ok) ||
                        (errors == best_errors && mrag_ok == best_mrag_ok &&
                         std::abs(shift) < std::abs(best_shift));
    if (better) {
      found = true;
      best_errors = errors;
      best_shift = shift;
      best_mrag_ok = mrag_ok;
    }
  }
  if (!found) {
    return result;
  }

  // Step 4 — payload extraction (§7.1): 336 bits at bit-centre positions,
  // LSB first per byte. No Hamming/parity correction is applied: the T42
  // contract preserves transmission coding.
  const double data_start = payload_start(best_shift);
  for (int n = 0; n < kPayloadBits; ++n) {
    if (sample_at(line, data_start + n * spb) > threshold) {
      result.bytes[static_cast<size_t>(n) >> 3] |=
          static_cast<uint8_t>(1u << (n & 7));
    }
  }

  // The MRAG plausibility filter (§7.1.2, §8.2) was applied per candidate
  // alignment in step 3; the stored bytes stay as transmitted.

  result.framing_bit_errors = best_errors;
  result.data_start_sample = data_start;
  result.valid = true;
  return result;
}

}  // namespace orc
