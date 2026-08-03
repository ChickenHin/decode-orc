/*
 * File:        teletext_line_synthesizer.h
 * Module:      orc-tests/core/unit/support
 * Purpose:     In-memory NRZ teletext line synthesizer for slicer/observer
 *              unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace orc {
namespace tests {

// Full transmission packet: run-in (2) + framing (1) + MRAG-and-data (42).
// ETSI EN 300 706 §7.1: 360 bits = 45 bytes.
constexpr size_t kTeletextTransmissionBytes = 45;

// Synthesis options. Defaults produce a clean, nominally timed PAL line.
struct TeletextLineSynthOptions {
  double sample_rate = kPalSampleRate;
  double bit_rate = kTeletextBitRate;  // EN 300 706 §5.3: 444 × fH
  size_t sample_count = static_cast<size_t>(kPalSamplesPerLineNominal);
  int16_t black_level = static_cast<int16_t>(kPalBlack);
  int16_t white_level = static_cast<int16_t>(kPalWhite);
  // EN 300 706 §5.2: '1' level nominally 66 % of black-to-white.
  double amplitude_fraction = 0.66;
  // Centre of the first run-in bit. EN 300 706 §6.3: timing reference (mid
  // penultimate '1', run-in bit 15 of 16) nominally 12,0 µs after the sync
  // leading edge → first bit centre 14 bit periods earlier ≈ 9,98 µs.
  double first_bit_centre_us = 9.98;
  // Additional sub-sample/bit-phase offset applied to the whole burst.
  double phase_offset_samples = 0.0;
  // Uniform noise in ± this amplitude (10-bit domain counts); 0 = clean.
  int32_t noise_amplitude = 0;
  uint32_t noise_seed = 1;
  // Optional band limiting, applied after the NRZ pulses and before the
  // noise. Models a channel that cannot pass the whole data band: consumer
  // VHS luma rolls off around 3 MHz, below the 3,47 MHz clock run-in
  // fundamental (EN 300 706 §6.1), so the run-in is attenuated far more than
  // the payload and each bit smears into its neighbours. 0 = unlimited.
  double low_pass_cutoff_hz = 0.0;
};

// Band-limit |line| in place with a Hamming-windowed sinc low-pass. Edge
// samples clamp to the first/last value, which is harmless here: the burst
// sits well inside the line.
inline void band_limit_line(std::vector<int16_t>& line, double sample_rate,
                            double cutoff_hz) {
  constexpr double kPi = 3.14159265358979323846;
  constexpr int kHalfTaps = 20;
  const double fc = cutoff_hz / sample_rate;  // cycles per sample

  std::vector<double> taps(2 * kHalfTaps + 1);
  double gain = 0.0;
  for (int i = -kHalfTaps; i <= kHalfTaps; ++i) {
    const double sinc = (i == 0) ? 2.0 * fc
                                 : std::sin(2.0 * kPi * fc * i) /
                                       (kPi * static_cast<double>(i));
    const double window =
        0.54 + 0.46 * std::cos(kPi * static_cast<double>(i) / kHalfTaps);
    taps[static_cast<size_t>(i + kHalfTaps)] = sinc * window;
    gain += sinc * window;
  }
  for (auto& tap : taps) {
    tap /= gain;
  }

  const std::vector<int16_t> input = line;
  const auto at = [&input](int index) {
    const int clamped =
        std::max(0, std::min(index, static_cast<int>(input.size()) - 1));
    return static_cast<double>(input[static_cast<size_t>(clamped)]);
  };
  for (size_t s = 0; s < line.size(); ++s) {
    double sum = 0.0;
    for (int i = -kHalfTaps; i <= kHalfTaps; ++i) {
      sum += taps[static_cast<size_t>(i + kHalfTaps)] *
             at(static_cast<int>(s) + i);
    }
    line[s] = static_cast<int16_t>(std::lround(sum));
  }
}

// Build the 45-byte transmission packet for a 42-byte T42 payload.
// Bytes are LSB-first on air (EN 300 706 §7.1): run-in 1010… → 0x55 0x55
// (§6.1); framing 11100100 in transmission order → LSB-first byte 0x27
// (conventionally written 0xE4 MSB-first, §6.2).
inline std::array<uint8_t, kTeletextTransmissionBytes> make_transmission_packet(
    const std::array<uint8_t, kTeletextPacketBytes>& payload) {
  std::array<uint8_t, kTeletextTransmissionBytes> packet{};
  packet[0] = 0x55;
  packet[1] = 0x55;
  packet[2] = 0x27;
  for (size_t i = 0; i < payload.size(); ++i) {
    packet[3 + i] = payload[i];
  }
  return packet;
}

// Build a valid MRAG (bytes 4-5 of the transmission packet) for a magazine
// and packet number. EN 300 706 §7.1.2: byte 4 carries magazine bits 2^0-2^2
// plus packet-number 2^0; byte 5 carries packet-number 2^1-2^4; both bytes
// Hamming 8/4 coded (§8.2).
inline std::array<uint8_t, 2> make_mrag(int magazine, int packet_number) {
  const uint8_t nibble1 =
      static_cast<uint8_t>((magazine & 0x7) | ((packet_number & 0x1) << 3));
  const uint8_t nibble2 = static_cast<uint8_t>((packet_number >> 1) & 0xF);
  return {teletext_hamming84_encode(nibble1),
          teletext_hamming84_encode(nibble2)};
}

// Synthesize an NRZ teletext line from a raw 45-byte transmission packet.
// Rectangular NRZ pulses at the §5.2 levels, LSB-first per byte, on a black
// baseline; optional deterministic uniform noise (xorshift PRNG).
inline std::vector<int16_t> synthesize_teletext_line_raw(
    const std::array<uint8_t, kTeletextTransmissionBytes>& packet,
    const TeletextLineSynthOptions& opt = {}) {
  std::vector<int16_t> line(opt.sample_count, opt.black_level);
  const double spb = opt.sample_rate / opt.bit_rate;
  const double t0 = opt.first_bit_centre_us * opt.sample_rate / 1e6 +
                    opt.phase_offset_samples;
  const double one_level =
      static_cast<double>(opt.black_level) +
      opt.amplitude_fraction * (static_cast<double>(opt.white_level) -
                                static_cast<double>(opt.black_level));

  constexpr int kTotalBits =
      static_cast<int>(kTeletextTransmissionBytes) * 8;  // §7.1: 360 bits
  for (size_t s = 0; s < line.size(); ++s) {
    const int bit_index =
        static_cast<int>(std::lround((static_cast<double>(s) - t0) / spb));
    if (bit_index < 0 || bit_index >= kTotalBits) {
      continue;
    }
    const int bit =
        (packet[static_cast<size_t>(bit_index) >> 3] >> (bit_index & 7)) & 1;
    line[s] =
        static_cast<int16_t>(bit ? std::lround(one_level) : opt.black_level);
  }

  if (opt.low_pass_cutoff_hz > 0.0) {
    band_limit_line(line, opt.sample_rate, opt.low_pass_cutoff_hz);
  }

  if (opt.noise_amplitude > 0) {
    uint32_t state = opt.noise_seed != 0 ? opt.noise_seed : 1;
    for (auto& sample : line) {
      // xorshift32: deterministic, seedable, no <random> heavy machinery.
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      const int32_t span = 2 * opt.noise_amplitude + 1;
      const int32_t noise =
          static_cast<int32_t>(state % static_cast<uint32_t>(span)) -
          opt.noise_amplitude;
      sample = static_cast<int16_t>(sample + noise);
    }
  }
  return line;
}

// Synthesize an NRZ teletext line carrying the given 42-byte T42 payload.
inline std::vector<int16_t> synthesize_teletext_line(
    const std::array<uint8_t, kTeletextPacketBytes>& payload,
    const TeletextLineSynthOptions& opt = {}) {
  return synthesize_teletext_line_raw(make_transmission_packet(payload), opt);
}

// A deterministic, MRAG-valid 42-byte test payload (magazine 1, packet 0,
// then a byte pattern exercising all bit positions).
inline std::array<uint8_t, kTeletextPacketBytes> make_test_payload() {
  std::array<uint8_t, kTeletextPacketBytes> payload{};
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/0);
  payload[0] = mrag[0];
  payload[1] = mrag[1];
  for (size_t i = 2; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(0x5A ^ (i * 37));
  }
  return payload;
}

// As make_test_payload(), but with the 40 data bytes odd-parity coded the way
// a real display row is (EN 300 706 §9.3.1). Detectors that check the
// transmission coding — the MLSE detector applies a parity plausibility gate
// to rows 0-25 — need a payload that is coded as transmitted, not arbitrary
// bytes.
inline std::array<uint8_t, kTeletextPacketBytes> make_parity_coded_payload() {
  auto payload = make_test_payload();
  for (size_t i = 2; i < payload.size(); ++i) {
    payload[i] = teletext_odd_parity_encode(payload[i] & 0x7F);
  }
  return payload;
}

}  // namespace tests
}  // namespace orc
