/*
 * File:        vbi_synthetic_line.h
 * Module:      orc-tests
 * Purpose:     Renders synthetic VBI line records for timing-recovery tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TESTS_VBI_SYNTHETIC_LINE_H
#define ORC_TESTS_VBI_SYNTHETIC_LINE_H

#include <cmath>
#include <cstdint>
#include <vector>

namespace orc {
namespace testing {

// A line record to render, in one source's own sample coordinates.
//
// Deliberately implemented by a different route from the production template
// generator — an oversampled ideal waveform convolved with a discrete Gaussian
// rather than a closed-form sum of error functions — so that a test asserting
// where the correlator finds the run-in is testing the correlator rather than
// two halves of one implementation agreeing with each other.
struct SyntheticVBILine {
  double sample_rate_hz = 35468950.0;  // 8 x fsc PAL, the bt8x8 card rate
  uint32_t valid_samples = 2044;

  double bit_rate_hz = 6937500.0;  // WST

  // Run-in and framing code in transmission order, most significant bit first.
  uint32_t pattern = 0xAAAAE4u;
  uint32_t pattern_bits = 24;

  // Bits of the run-in at the head of the pattern, whose amplitude the channel
  // attenuates first because they are its highest-frequency content.
  uint32_t run_in_bits = 16;

  // Pseudo-random data bits following the framing code.
  uint32_t payload_bits = 336;

  // Explicit data bits to transmit after the framing code, in transmission
  // order.  Empty renders the pseudo-random payload above instead; a caller
  // that means to recover the payload downstream supplies it here.
  std::vector<bool> payload;

  // Where the leading edge of the pattern's first one bit is to land.
  double anchor_position_samples = 121.3;

  double logic0 = 40.0;
  double logic1 = 200.0;

  // Channel blur, as the standard deviation of a Gaussian impulse response in
  // bit periods.  0.8 reproduces the VHS waveform measured in design §5.3.6.
  double blur_bit_periods = 0.25;

  // Run-in amplitude as a fraction of the data amplitude, applied before the
  // blur.  0.055 is the measured VHS figure.
  double run_in_amplitude_fraction = 1.0;

  // Uniform noise amplitude in source counts, from a deterministic sequence.
  double noise_amplitude = 0.0;
  uint32_t seed = 1;

  // A colour burst tail at the head of the record, as a bt8x8 PAL capture
  // carries because its window opens inside the burst (design §5.3.5).
  bool include_burst = false;
  double burst_end_samples = 34.0;
  double burst_frequency_hz = 4433618.75;
  double burst_amplitude = 60.0;
  double burst_phase_radians = 0.0;

  // True renders the data; false leaves the record at logic 0 plus noise,
  // which is what a line carrying no data service looks like.
  bool carries_data = true;
};

namespace detail {

// Oversampling of the ideal waveform before the channel filter is applied.
constexpr uint32_t kOversample = 8;

inline double next_uniform(uint32_t& state) {
  // Numerical Recipes linear congruential generator: deterministic, and its
  // quality is irrelevant for additive test noise.
  state = state * 1664525u + 1013904223u;
  return (static_cast<double>(state) / 4294967296.0) - 0.5;
}

}  // namespace detail

// Render one record.
inline std::vector<double> render_synthetic_vbi_line(
    const SyntheticVBILine& line) {
  const size_t sample_count = line.valid_samples;
  std::vector<double> record(sample_count, line.logic0);

  const double samples_per_bit = line.sample_rate_hz / line.bit_rate_hz;
  const double midpoint = 0.5 * (line.logic0 + line.logic1);

  if (line.carries_data && line.pattern_bits > 0) {
    // First one bit of the pattern: the anchor every service's timing is
    // measured to.
    uint32_t first_one = 0;
    for (uint32_t index = 0; index < line.pattern_bits; ++index) {
      if (((line.pattern >> (line.pattern_bits - 1u - index)) & 1u) != 0u) {
        first_one = index;
        break;
      }
    }
    const double bit_zero_samples =
        line.anchor_position_samples -
        static_cast<double>(first_one) * samples_per_bit;

    const uint32_t payload_bits =
        line.payload.empty() ? line.payload_bits
                             : static_cast<uint32_t>(line.payload.size());
    std::vector<bool> bits;
    bits.reserve(line.pattern_bits + payload_bits);
    for (uint32_t index = 0; index < line.pattern_bits; ++index) {
      bits.push_back(
          ((line.pattern >> (line.pattern_bits - 1u - index)) & 1u) != 0u);
    }
    if (line.payload.empty()) {
      uint32_t payload_state = line.seed * 2654435761u + 1u;
      for (uint32_t index = 0; index < payload_bits; ++index) {
        bits.push_back(detail::next_uniform(payload_state) >= 0.0);
      }
    } else {
      bits.insert(bits.end(), line.payload.begin(), line.payload.end());
    }

    // Ideal waveform on an oversampled grid.
    const size_t oversampled_count = sample_count * detail::kOversample;
    std::vector<double> ideal(oversampled_count, line.logic0);
    for (size_t index = 0; index < oversampled_count; ++index) {
      const double position =
          static_cast<double>(index) / detail::kOversample - bit_zero_samples;
      if (position < 0.0) {
        continue;
      }
      const size_t bit_index = static_cast<size_t>(position / samples_per_bit);
      if (bit_index >= bits.size()) {
        continue;
      }
      const bool high = bits[bit_index];
      double level = high ? line.logic1 : line.logic0;
      if (bit_index < line.run_in_bits) {
        // The run-in collapses towards the data midpoint on a band-limited
        // channel long before the data does.
        level = midpoint + line.run_in_amplitude_fraction * (level - midpoint);
      }
      ideal[index] = level;
    }

    // Gaussian channel.
    const double blur_samples =
        line.blur_bit_periods * samples_per_bit * detail::kOversample;
    if (blur_samples > 1e-6) {
      const int64_t radius =
          static_cast<int64_t>(std::ceil(4.0 * blur_samples));
      std::vector<double> kernel(static_cast<size_t>(2 * radius + 1), 0.0);
      double kernel_total = 0.0;
      for (int64_t tap = -radius; tap <= radius; ++tap) {
        const double value =
            std::exp(-0.5 * static_cast<double>(tap) *
                     static_cast<double>(tap) / (blur_samples * blur_samples));
        kernel[static_cast<size_t>(tap + radius)] = value;
        kernel_total += value;
      }

      std::vector<double> filtered(oversampled_count, 0.0);
      const int64_t last = static_cast<int64_t>(oversampled_count) - 1;
      for (int64_t index = 0; index <= last; ++index) {
        double accumulator = 0.0;
        for (int64_t tap = -radius; tap <= radius; ++tap) {
          int64_t source = index + tap;
          source = (source < 0) ? 0 : ((source > last) ? last : source);
          accumulator += kernel[static_cast<size_t>(tap + radius)] *
                         ideal[static_cast<size_t>(source)];
        }
        filtered[static_cast<size_t>(index)] = accumulator / kernel_total;
      }
      ideal.swap(filtered);
    }

    for (size_t index = 0; index < sample_count; ++index) {
      record[index] = ideal[index * detail::kOversample];
    }
  }

  if (line.include_burst) {
    for (size_t index = 0; index < sample_count; ++index) {
      if (static_cast<double>(index) >= line.burst_end_samples) {
        break;
      }
      const double angle =
          2.0 * 3.14159265358979323846 * line.burst_frequency_hz *
              static_cast<double>(index) / line.sample_rate_hz +
          line.burst_phase_radians;
      record[index] += line.burst_amplitude * std::sin(angle);
    }
  }

  if (line.noise_amplitude > 0.0) {
    uint32_t state = line.seed * 22695477u + 3u;
    for (double& value : record) {
      value += 2.0 * line.noise_amplitude * detail::next_uniform(state);
    }
  }

  return record;
}

}  // namespace testing
}  // namespace orc

#endif  // ORC_TESTS_VBI_SYNTHETIC_LINE_H
