/*
 * File:        vbi_cri_template.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Clock run-in and framing code templates for timing recovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_cri_template.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace orc {

namespace {

// Logic levels the ideal pattern is generated at.  Any symmetric pair does:
// the template is zero-meaned and normalised before it is used, so only the
// shape survives.
constexpr double kLogic0 = -0.5;
constexpr double kLogic1 = 0.5;

// A run-in is a tone at half the bit rate, so two samples per bit is the point
// at which it stops being represented at all.
constexpr double kMinimumSamplesPerBit = 2.0;

// Blur below this is a step edge for all practical purposes, and the error
// function form degenerates.
constexpr double kMinimumBlurSamples = 1e-6;

constexpr double kSqrt2 = 1.4142135623730951;

// Bit at an index of a pattern held most significant bit first.
bool pattern_bit(uint32_t pattern, uint32_t bit_count, uint32_t index) {
  return ((pattern >> (bit_count - 1u - index)) & 1u) != 0u;
}

// One level transition of the ideal pattern.
struct PatternEdge {
  double position_samples = 0.0;
  double level_change = 0.0;
};

// Hold a template at zero mean and unit Euclidean norm.  Returns false when
// there is no shape to normalise, which is a template of a constant.
bool normalise_template(std::vector<double>& samples) {
  if (samples.empty()) {
    return false;
  }

  double total = 0.0;
  for (const double value : samples) {
    total += value;
  }
  const double mean = total / static_cast<double>(samples.size());

  double energy = 0.0;
  for (double& value : samples) {
    value -= mean;
    energy += value * value;
  }
  if (!(energy > 0.0)) {
    return false;
  }

  const double norm = std::sqrt(energy);
  for (double& value : samples) {
    value /= norm;
  }
  return true;
}

// Value of a Gaussian-filtered pattern at a continuous sample coordinate.
//
// The filtered waveform is the sum of the pattern's edges, each smeared into
// an error function.  Evaluating it in closed form rather than convolving an
// oversampled grid keeps the template free of the grid's own artefacts, which
// matters because a bias of a fraction of a sample here becomes a bias in
// every fitted position.
double filtered_pattern_value(const std::vector<PatternEdge>& edges,
                              double position, double blur_samples) {
  double value = kLogic0;
  for (const PatternEdge& edge : edges) {
    const double distance = position - edge.position_samples;
    const double step =
        (blur_samples <= kMinimumBlurSamples)
            ? ((distance >= 0.0) ? 1.0 : 0.0)
            : (0.5 * (1.0 + std::erf(distance / (blur_samples * kSqrt2))));
    value += edge.level_change * step;
  }
  return value;
}

}  // namespace

bool make_vbi_pattern_template(uint32_t pattern, uint32_t bit_count,
                               double bit_rate_hz, double sample_rate_hz,
                               const VBICRITemplateConfig& config,
                               VBICRITemplate& out_template,
                               std::string& error_message) {
  out_template = VBICRITemplate();

  if (bit_count == 0 || bit_count > 32) {
    error_message =
        "A run-in and framing code pattern must hold between 1 and 32 bits; " +
        std::to_string(bit_count) + " were given.";
    return false;
  }
  if (!(bit_rate_hz > 0.0) || !std::isfinite(bit_rate_hz)) {
    error_message = "The data service's bit rate must be positive.";
    return false;
  }
  if (!(sample_rate_hz > 0.0) || !std::isfinite(sample_rate_hz)) {
    error_message = "The sampling rate must be positive.";
    return false;
  }

  const double samples_per_bit = sample_rate_hz / bit_rate_hz;
  if (samples_per_bit < kMinimumSamplesPerBit) {
    error_message =
        "A sampling rate of " + std::to_string(sample_rate_hz) +
        " Hz gives only " + std::to_string(samples_per_bit) +
        " samples per bit at " + std::to_string(bit_rate_hz) +
        " bit/s, which cannot represent the clock run-in's alternation.";
    return false;
  }

  const double lead_in_samples =
      std::max(0.0, config.lead_in_bits) * samples_per_bit;

  // Transitions of the ideal pattern, the first of them being the rise out of
  // the lead-in's logic 0.
  std::vector<PatternEdge> edges;
  edges.reserve(bit_count);
  bool previous = false;
  bool anchored = false;
  double anchor_samples = lead_in_samples;
  for (uint32_t index = 0; index < bit_count; ++index) {
    const bool bit = pattern_bit(pattern, bit_count, index);
    const double bit_start =
        lead_in_samples + static_cast<double>(index) * samples_per_bit;
    if (bit != previous) {
      edges.push_back(PatternEdge{
          bit_start, bit ? (kLogic1 - kLogic0) : (kLogic0 - kLogic1)});
    }
    if (bit && !anchored) {
      anchor_samples = bit_start;
      anchored = true;
    }
    previous = bit;
  }
  if (edges.empty()) {
    error_message =
        "The pattern holds no level transitions, so it cannot locate anything.";
    return false;
  }

  // The template ends with the pattern.  Whatever follows a framing code is
  // payload, which is unknown by definition, so extending the template into it
  // would correlate against an assumption rather than against the signal.
  const double length_samples =
      lead_in_samples + static_cast<double>(bit_count) * samples_per_bit;
  const size_t sample_count = static_cast<size_t>(std::ceil(length_samples));
  if (sample_count < 2) {
    error_message = "The generated template is too short to correlate against.";
    return false;
  }

  const double blur_samples =
      std::max(0.0, config.blur_bit_periods) * samples_per_bit;

  std::vector<double> samples(sample_count, 0.0);
  for (size_t index = 0; index < sample_count; ++index) {
    samples[index] =
        filtered_pattern_value(edges, static_cast<double>(index), blur_samples);
  }
  if (!normalise_template(samples)) {
    error_message =
        "The generated template is a constant, so it carries no position.";
    return false;
  }

  out_template.samples = std::move(samples);
  out_template.sample_rate_hz = sample_rate_hz;
  out_template.bit_rate_hz = bit_rate_hz;
  out_template.samples_per_bit = samples_per_bit;
  out_template.anchor_samples = anchor_samples;
  out_template.bit_count = bit_count;
  return true;
}

bool make_vbi_cri_frc_template(const VBITeletextService& service,
                               double sample_rate_hz,
                               const VBICRITemplateConfig& config,
                               VBICRITemplate& out_template,
                               std::string& error_message) {
  const uint32_t bit_count = service.cri_bits + service.frc_bits;
  return make_vbi_pattern_template(service.cri_frc_pattern, bit_count,
                                   service.bit_rate_hz, sample_rate_hz, config,
                                   out_template, error_message);
}

double vbi_template_autocorrelation(const VBICRITemplate& tmpl,
                                    double lag_samples) {
  if (tmpl.samples.size() < 2 || !std::isfinite(lag_samples)) {
    return 0.0;
  }

  // The shifted copy is read by linear interpolation and everything outside
  // the template reads as its own mean, which is zero.  A lag therefore loses
  // correlation both by mismatching and by overlapping less, which is the
  // honest accounting: a pattern that only matches over part of itself has not
  // located anything.
  const std::vector<double>& samples = tmpl.samples;
  const double last = static_cast<double>(samples.size() - 1u);

  double correlation = 0.0;
  for (size_t index = 0; index < samples.size(); ++index) {
    const double position = static_cast<double>(index) + lag_samples;
    if (position < 0.0 || position > last) {
      continue;
    }
    const double floor_position = std::floor(position);
    const size_t lower = static_cast<size_t>(floor_position);
    const size_t upper = std::min(lower + 1u, samples.size() - 1u);
    const double fraction = position - floor_position;
    const double shifted =
        samples[lower] + fraction * (samples[upper] - samples[lower]);
    correlation += samples[index] * shifted;
  }

  // The template is already unit-norm, so no denominator is needed.
  return correlation;
}

}  // namespace orc
