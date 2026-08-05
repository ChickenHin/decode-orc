/*
 * File:        vbi_resampler.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Band-limited resampling of VBI records onto the output lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_resampler.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace orc {

namespace {

// Modified Bessel function of the first kind, order zero, by its power series.
// The series converges quickly for the arguments a Kaiser window uses, and the
// loop stops as soon as a term stops contributing.
double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  for (int order = 1; order < 64; ++order) {
    const double factor = x / (2.0 * static_cast<double>(order));
    term *= factor * factor;
    sum += term;
    if (term < sum * 1e-18) {
      break;
    }
  }
  return sum;
}

// Normalised sinc, sin(pi x) / (pi x), with its removable singularity filled
// in.
double normalised_sinc(double x) {
  if (std::abs(x) < 1e-12) {
    return 1.0;
  }
  constexpr double kPi = 3.14159265358979323846;
  const double argument = kPi * x;
  return std::sin(argument) / argument;
}

}  // namespace

VBIRecordResampler::VBIRecordResampler(const VBIDataPlacement& placement,
                                       uint32_t record_samples,
                                       VBIResamplerConfig config)
    : config_(config) {
  config_.half_width_samples = std::max(1u, config_.half_width_samples);
  config_.phase_steps = std::max(1u, config_.phase_steps);
  config_.cutoff_fraction = std::clamp(config_.cutoff_fraction, 0.01, 1.0);
  config_.kaiser_beta = std::max(0.0, config_.kaiser_beta);

  const double ratio = placement.source_samples_per_output_sample;
  if (!(ratio > 0.0) || !std::isfinite(ratio) || record_samples == 0 ||
      placement.output_count() == 0) {
    return;
  }

  // The passband that must survive is the narrower of the two Nyquist limits.
  // Decimation is bounded by the output rate; anything at or below 1:1 is
  // bounded by the source rate, since there is no more bandwidth to keep.
  const double limiting_ratio = std::max(1.0, ratio);
  cutoff_normalised_ = config_.cutoff_fraction * 0.5 / limiting_ratio;

  output_count_ = placement.output_count();
  taps_ = config_.half_width_samples * 2u;

  first_tap_.resize(output_count_);
  row_.resize(output_count_);

  const double window_denominator = bessel_i0(config_.kaiser_beta);
  const auto half_width = static_cast<int64_t>(config_.half_width_samples);
  const double steps = static_cast<double>(config_.phase_steps);

  // Bank row per distinct quantised fractional delay.  Every output sample of
  // a whole-number ratio quantises to the same delay, so the bank is one row.
  std::map<uint32_t, uint32_t> rows;

  for (uint32_t sample = 0; sample < output_count_; ++sample) {
    const double position = placement.source_position(
        static_cast<double>(placement.output_begin + sample));
    double floor_position = std::floor(position);
    auto phase = static_cast<uint32_t>(
        std::llround((position - floor_position) * steps));
    if (phase >= config_.phase_steps) {
      // The delay rounded up to a whole sample, which is the next centre.
      phase = 0;
      floor_position += 1.0;
    }
    const auto base = static_cast<int64_t>(floor_position);

    const auto existing = rows.find(phase);
    uint32_t row = 0;
    if (existing != rows.end()) {
      row = existing->second;
    } else {
      row = static_cast<uint32_t>(bank_.size() / taps_);
      rows.emplace(phase, row);

      const double fraction = static_cast<double>(phase) / steps;
      bank_.resize(bank_.size() + taps_);
      double* weights = bank_.data() + static_cast<size_t>(row) * taps_;

      double weight_sum = 0.0;
      for (int64_t tap = -half_width + 1; tap <= half_width; ++tap) {
        // Distance from the kernel centre to the stored sample this tap reads.
        const double offset = fraction - static_cast<double>(tap);
        const double window_position = offset / static_cast<double>(half_width);
        const double window_argument = 1.0 - window_position * window_position;
        const double window =
            (window_argument <= 0.0)
                ? 0.0
                : bessel_i0(config_.kaiser_beta * std::sqrt(window_argument)) /
                      window_denominator;
        const double weight =
            normalised_sinc(2.0 * cutoff_normalised_ * offset) * window;

        weights[tap + half_width - 1] = weight;
        weight_sum += weight;
      }

      // A row that sums to one has a response of exactly one at DC, at every
      // fractional delay.
      if (weight_sum != 0.0) {
        const double scale = 1.0 / weight_sum;
        for (uint32_t tap = 0; tap < taps_; ++tap) {
          weights[tap] *= scale;
        }
      }
    }

    row_[sample] = row;
    first_tap_[sample] = static_cast<int32_t>(base - half_width + 1);
  }
}

void VBIRecordResampler::resample(const std::vector<double>& record,
                                  std::vector<double>& out_samples) const {
  out_samples.assign(output_count_, 0.0);
  if (output_count_ == 0 || record.empty()) {
    return;
  }

  const double* samples = record.data();
  const auto last = static_cast<int32_t>(record.size()) - 1;
  const auto taps = static_cast<int32_t>(taps_);

  for (uint32_t sample = 0; sample < output_count_; ++sample) {
    const double* weights =
        bank_.data() + static_cast<size_t>(row_[sample]) * taps_;
    const int32_t first = first_tap_[sample];

    double accumulator = 0.0;
    if (first >= 0 && first + taps - 1 <= last) {
      // The whole kernel falls inside the record, which is every sample of the
      // data region on a real capture.
      const double* window = samples + first;
      for (int32_t tap = 0; tap < taps; ++tap) {
        accumulator += weights[tap] * window[tap];
      }
    } else {
      // Constant extension at whichever end the kernel ran off.
      for (int32_t tap = 0; tap < taps; ++tap) {
        const int32_t index = std::clamp(first + tap, 0, last);
        accumulator += weights[tap] * samples[index];
      }
    }
    out_samples[sample] = accumulator;
  }
}

}  // namespace orc
