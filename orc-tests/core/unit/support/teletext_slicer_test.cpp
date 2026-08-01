/*
 * File:        teletext_slicer_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for the PAL WST TeletextSlicer (support tier)
 *
 * Lines are synthesized in memory at the PAL 4FSC sample rate from known
 * packet bytes; no filesystem, network, or clock access.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/teletext_slicer.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <vector>

#include "teletext_line_synthesizer.h"

namespace orc {
namespace tests {
namespace {

TeletextSlicer make_slicer(TeletextSlicerOptions options = {}) {
  return TeletextSlicer(kPalSampleRate, kTeletextBitRate, options);
}

TeletextLineResult slice_line(const std::vector<int16_t>& line,
                              TeletextSlicerOptions options = {}) {
  return make_slicer(options).slice(line.data(), line.size(),
                                    static_cast<int16_t>(kPalBlack),
                                    static_cast<int16_t>(kPalWhite));
}

// ---------------------------------------------------------------------------
// Hamming 8/4 and hex helpers
// ---------------------------------------------------------------------------

TEST(TeletextHamming84, EncodeDecodeRoundTrip_AllValues) {
  for (int value = 0; value < 16; ++value) {
    const uint8_t code = teletext_hamming84_encode(static_cast<uint8_t>(value));
    EXPECT_EQ(teletext_hamming84_decode(code), value);
  }
}

TEST(TeletextHamming84, SingleBitErrorsAreCorrected) {
  for (int value = 0; value < 16; ++value) {
    const uint8_t code = teletext_hamming84_encode(static_cast<uint8_t>(value));
    for (int bit = 0; bit < 8; ++bit) {
      EXPECT_EQ(teletext_hamming84_decode(code ^ (1u << bit)), value)
          << "value=" << value << " flipped bit=" << bit;
    }
  }
}

TEST(TeletextHamming84, DoubleBitErrorsAreRejected) {
  // EN 300 706 §8.2: double-bit errors are detected, not corrected.
  const uint8_t code = teletext_hamming84_encode(0x5);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b00000011), -1);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b00010100), -1);
  EXPECT_EQ(teletext_hamming84_decode(code ^ 0b10000001), -1);
}

TEST(TeletextHex, PacketRoundTrip) {
  const auto payload = make_test_payload();
  const std::string hex = teletext_packet_to_hex(payload);
  EXPECT_EQ(hex.size(), kTeletextPacketBytes * 2);
  const auto decoded = teletext_hex_to_packet(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, payload);
}

TEST(TeletextHex, RejectsBadLengthAndBadCharacters) {
  EXPECT_FALSE(teletext_hex_to_packet("").has_value());
  EXPECT_FALSE(teletext_hex_to_packet("abc").has_value());
  std::string hex = teletext_packet_to_hex(make_test_payload());
  hex[10] = 'g';
  EXPECT_FALSE(teletext_hex_to_packet(hex).has_value());
}

TEST(TeletextHex, AcceptsUppercaseHex) {
  const auto payload = make_test_payload();
  std::string hex = teletext_packet_to_hex(payload);
  for (auto& c : hex) c = static_cast<char>(std::toupper(c));
  const auto decoded = teletext_hex_to_packet(hex);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, payload);
}

// ---------------------------------------------------------------------------
// Clean recovery
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, CleanLine_RecoversBytesExactly) {
  const auto payload = make_test_payload();
  const auto result = slice_line(synthesize_teletext_line(payload));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
  EXPECT_EQ(result.framing_bit_errors, 0);
  EXPECT_GT(result.data_start_sample, 0.0);
}

TEST(TeletextSlicer, PhaseOffsetSweep_RecoversBytesExactly) {
  // Sub-sample timing sweep covering more than one full bit period
  // (≈ 2.556 samples at 444 × fH, EN 300 706 §5.3).
  const auto payload = make_test_payload();
  for (int step = 0; step <= 26; ++step) {
    const double offset = step * 0.1;
    TeletextLineSynthOptions opt;
    opt.phase_offset_samples = offset;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "offset=" << offset;
    EXPECT_EQ(result.bytes, payload) << "offset=" << offset;
  }
}

TEST(TeletextSlicer, AmplitudeSweep_RecoversAcrossSpecRange) {
  // EN 300 706 §5.2: '1' level 66 ± 6 % of black-to-white; sweep well beyond
  // the tolerance range in both directions.
  const auto payload = make_test_payload();
  for (int step = 0; step <= 9; ++step) {
    const double fraction = 0.45 + step * 0.05;
    TeletextLineSynthOptions opt;
    opt.amplitude_fraction = fraction;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "fraction=" << fraction;
    EXPECT_EQ(result.bytes, payload) << "fraction=" << fraction;
  }
}

TEST(TeletextSlicer, LowAmplitudeLine_IsRejected) {
  // Data burst far below the amplitude gate reads as an empty line.
  const auto payload = make_test_payload();
  TeletextLineSynthOptions opt;
  opt.amplitude_fraction = 0.1;
  EXPECT_FALSE(slice_line(synthesize_teletext_line(payload, opt)).valid);
}

TEST(TeletextSlicer, NoiseSweep_RecoversBytesExactly) {
  const auto payload = make_test_payload();
  for (int32_t noise = 10; noise <= 50; noise += 10) {
    TeletextLineSynthOptions opt;
    opt.noise_amplitude = noise;
    opt.noise_seed = static_cast<uint32_t>(noise) * 977u + 1u;
    opt.phase_offset_samples = 0.7;
    const auto result = slice_line(synthesize_teletext_line(payload, opt));
    ASSERT_TRUE(result.valid) << "noise=" << noise;
    EXPECT_EQ(result.bytes, payload) << "noise=" << noise;
  }
}

// ---------------------------------------------------------------------------
// Empty and noise-only rejection
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, BlankLine_IsRejected) {
  const std::vector<int16_t> line(
      static_cast<size_t>(kPalSamplesPerLineNominal),
      static_cast<int16_t>(kPalBlack));
  EXPECT_FALSE(slice_line(line).valid);
}

TEST(TeletextSlicer, NoiseOnlyLines_AreNeverValid) {
  // Deterministic noise-only lines across seeds and amplitudes must never
  // yield a packet (run-in match, framing code, and MRAG filters all gate).
  const std::array<uint8_t, kTeletextTransmissionBytes> silent{};
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    for (int32_t noise : {30, 80, 150}) {
      TeletextLineSynthOptions opt;
      opt.amplitude_fraction = 0.0;  // no data burst, baseline only
      opt.noise_amplitude = noise;
      opt.noise_seed = seed;
      const auto line = synthesize_teletext_line_raw(silent, opt);
      EXPECT_FALSE(slice_line(line).valid)
          << "seed=" << seed << " noise=" << noise;
    }
  }
}

TEST(TeletextSlicer, NullOrShortInput_IsRejected) {
  const auto slicer = make_slicer();
  EXPECT_FALSE(slicer
                   .slice(nullptr, 1135, static_cast<int16_t>(kPalBlack),
                          static_cast<int16_t>(kPalWhite))
                   .valid);
  const std::vector<int16_t> short_line(64, static_cast<int16_t>(kPalBlack));
  EXPECT_FALSE(slicer
                   .slice(short_line.data(), short_line.size(),
                          static_cast<int16_t>(kPalBlack),
                          static_cast<int16_t>(kPalWhite))
                   .valid);
}

// ---------------------------------------------------------------------------
// Framing tolerance
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, OneFramingBitError_RejectedByDefault) {
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] ^= 0x04;  // flip one framing-code bit (byte 3, EN 300 706 §6.2)
  const auto line = synthesize_teletext_line_raw(packet);
  EXPECT_FALSE(slice_line(line).valid);
}

TEST(TeletextSlicer, OneFramingBitError_AcceptedInTolerantMode) {
  const auto payload = make_test_payload();
  auto packet = make_transmission_packet(payload);
  packet[2] ^= 0x04;
  const auto line = synthesize_teletext_line_raw(packet);

  TeletextSlicerOptions options;
  options.tolerant_framing = true;
  const auto result = slice_line(line, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.framing_bit_errors, 1);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicer, TwoFramingBitErrors_RejectedInBothModes) {
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] ^= 0x14;  // two framing-code bit errors
  const auto line = synthesize_teletext_line_raw(packet);

  EXPECT_FALSE(slice_line(line).valid);
  TeletextSlicerOptions options;
  options.tolerant_framing = true;
  EXPECT_FALSE(slice_line(line, options).valid);
}

// ---------------------------------------------------------------------------
// MRAG plausibility filter
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, UncorrectableMrag_RejectedWhenFilterOn) {
  // Both MRAG bytes given double-bit errors: uncorrectable per §8.2.
  auto payload = make_test_payload();
  payload[0] ^= 0b00000011;
  payload[1] ^= 0b00011000;
  ASSERT_EQ(teletext_hamming84_decode(payload[0]), -1);
  ASSERT_EQ(teletext_hamming84_decode(payload[1]), -1);
  const auto line = synthesize_teletext_line(payload);

  EXPECT_FALSE(slice_line(line).valid);

  // With the filter off the packet passes through, bytes as transmitted.
  TeletextSlicerOptions options;
  options.require_valid_mrag = false;
  const auto result = slice_line(line, options);
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

TEST(TeletextSlicer, SingleBitDamagedMrag_PassesFilterUncorrected) {
  // A correctable (single-bit) MRAG error passes the plausibility filter and
  // the output byte stays as transmitted — no correction applied (T42
  // contract: transmission coding preserved).
  auto payload = make_test_payload();
  payload[0] ^= 0b00000010;
  ASSERT_GE(teletext_hamming84_decode(payload[0]), 0);
  const auto result = slice_line(synthesize_teletext_line(payload));
  ASSERT_TRUE(result.valid);
  EXPECT_EQ(result.bytes, payload);
}

// ---------------------------------------------------------------------------
// Run-in without framing code
// ---------------------------------------------------------------------------

TEST(TeletextSlicer, RunInWithoutFramingCode_IsRejected) {
  // Valid clock run-in followed by a corrupted framing byte: the run-in lock
  // succeeds but no framing code is found within ± 2 bit positions.
  auto packet = make_transmission_packet(make_test_payload());
  packet[2] = 0x00;
  const auto line = synthesize_teletext_line_raw(packet);
  EXPECT_FALSE(slice_line(line).valid);

  TeletextSlicerOptions options;
  options.tolerant_framing = true;
  EXPECT_FALSE(slice_line(line, options).valid);
}

}  // namespace
}  // namespace tests
}  // namespace orc
