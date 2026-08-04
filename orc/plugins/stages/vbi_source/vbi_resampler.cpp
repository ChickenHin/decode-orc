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

namespace orc {

namespace {

// Modified Bessel function of the first kind, order zero, by its power
// series.  The series converges quickly for the arguments a Kaiser window
// uses, and the loop stops as soon as a term stops contributing.
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

VBIBandLimitedResampler::VBIBandLimitedResampler(
    double source_samples_per_output_sample, VBIResamplerConfig config)
    : ratio_(source_samples_per_output_sample), config_(config) {
  if (!(ratio_ > 0.0) || !std::isfinite(ratio_)) {
    ratio_ = 1.0;
  }
  config_.half_width_samples = std::max(1u, config_.half_width_samples);
  config_.phase_steps = std::max(1u, config_.phase_steps);
  config_.cutoff_fraction = std::clamp(config_.cutoff_fraction, 0.01, 1.0);
  config_.kaiser_beta = std::max(0.0, config_.kaiser_beta);

  // The passband that must survive is the narrower of the two Nyquist limits.
  // Decimation is bounded by the output rate; anything at or below 1:1 is
  // bounded by the source rate, since there is no more bandwidth to keep.
  const double limiting_ratio = std::max(1.0, ratio_);
  cutoff_normalised_ = config_.cutoff_fraction * 0.5 / limiting_ratio;

  const uint32_t half_width = config_.half_width_samples;
  const uint32_t steps = config_.phase_steps;
  const size_t table_size = static_cast<size_t>(half_width) * 2u * steps + 1u;

  kernel_table_.resize(table_size);
  const double window_denominator = bessel_i0(config_.kaiser_beta);
  for (size_t index = 0; index < table_size; ++index) {
    const double offset =
        static_cast<double>(index) / steps - static_cast<double>(half_width);
    const double window_position = offset / static_cast<double>(half_width);
    const double window_argument = 1.0 - window_position * window_position;
    const double window =
        (window_argument <= 0.0)
            ? 0.0
            : bessel_i0(config_.kaiser_beta * std::sqrt(window_argument)) /
                  window_denominator;
    kernel_table_[index] = 2.0 * cutoff_normalised_ *
                           normalised_sinc(2.0 * cutoff_normalised_ * offset) *
                           window;
  }
}

double VBIBandLimitedResampler::kernel_weight(double offset) const {
  const double half_width = static_cast<double>(config_.half_width_samples);
  const double table_position =
      (offset + half_width) * static_cast<double>(config_.phase_steps);
  if (!(table_position > 0.0)) {
    return kernel_table_.front();
  }

  const double last_interval = static_cast<double>(kernel_table_.size() - 2u);
  if (table_position >= last_interval + 1.0) {
    return kernel_table_.back();
  }

  const size_t lower = static_cast<size_t>(table_position);
  const double fraction = table_position - static_cast<double>(lower);
  return kernel_table_[lower] +
         fraction * (kernel_table_[lower + 1u] - kernel_table_[lower]);
}

double VBIBandLimitedResampler::sample_at(const std::vector<double>& source,
                                          double position) const {
  if (source.empty() || !std::isfinite(position)) {
    return 0.0;
  }

  const int64_t last_index = static_cast<int64_t>(source.size()) - 1;
  const int64_t half_width = static_cast<int64_t>(config_.half_width_samples);
  const double floor_position = std::floor(position);
  const int64_t base = static_cast<int64_t>(floor_position);
  const double fraction = position - floor_position;

  double accumulator = 0.0;
  double weight_sum = 0.0;
  for (int64_t tap = -half_width + 1; tap <= half_width; ++tap) {
    // Distance from the kernel centre to the stored sample this tap reads.
    const double weight = kernel_weight(fraction - static_cast<double>(tap));

    // Positions near either end of the record read the nearest stored sample.
    // Constant extension keeps a quiet head or tail at its own level rather
    // than pulling it towards an invented zero, which would be a step edge in
    // the middle of the sample path.
    const int64_t index = std::clamp(base + tap, int64_t{0}, last_index);
    accumulator += weight * source[static_cast<size_t>(index)];
    weight_sum += weight;
  }

  return (weight_sum != 0.0) ? (accumulator / weight_sum) : 0.0;
}

void VBIBandLimitedResampler::resample(const std::vector<double>& source,
                                       double first_position, uint32_t count,
                                       std::vector<double>& out_samples) const {
  out_samples.assign(count, 0.0);
  for (uint32_t index = 0; index < count; ++index) {
    out_samples[index] =
        sample_at(source, first_position + static_cast<double>(index) * ratio_);
  }
}

}  // namespace orc
