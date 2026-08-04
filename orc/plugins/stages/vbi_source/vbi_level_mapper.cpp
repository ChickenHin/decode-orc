/*
 * File:        vbi_level_mapper.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Maps card-capture sample levels into the CVBS amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_level_mapper.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace orc {

namespace {

// Quantile of the clock run-in window taken as its peak level.  The run-in is
// an even alternation of ones and zeros, so its upper decile sits inside the
// one bits while staying clear of any single noisy peak.
constexpr double kCRIPeakQuantile = 0.9;
constexpr double kCRITroughQuantile = 0.1;

// Linear-interpolated quantile of an already sorted, non-empty sample set.
double sorted_quantile(const std::vector<double>& sorted, double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }
  if (sorted.size() == 1) {
    return sorted.front();
  }

  const double position = quantile * static_cast<double>(sorted.size() - 1);
  const double floor_position = std::floor(position);
  const size_t lower = static_cast<size_t>(floor_position);
  const size_t upper = std::min(lower + 1, sorted.size() - 1);
  const double fraction = position - floor_position;
  return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  return sorted_quantile(values, 0.5);
}

// Copy a half-open window of a record, clamped to the samples present.
std::vector<double> window_samples(const std::vector<double>& samples,
                                   uint32_t begin, uint32_t end) {
  const size_t count = samples.size();
  const size_t first = std::min(static_cast<size_t>(begin), count);
  const size_t last = std::min(static_cast<size_t>(end), count);
  if (last <= first) {
    return {};
  }
  return std::vector<double>(samples.begin() + static_cast<ptrdiff_t>(first),
                             samples.begin() + static_cast<ptrdiff_t>(last));
}

double mean_of(const std::vector<double>& values) {
  if (values.empty()) {
    return 0.0;
  }
  double total = 0.0;
  for (const double value : values) {
    total += value;
  }
  return total / static_cast<double>(values.size());
}

}  // namespace

VBIRecordWindows vbi_record_windows(const VBISourceFormat& format,
                                    const VBITeletextService& service,
                                    double quiet_guard_samples) {
  VBIRecordWindows windows;

  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);
  const double cri_start = service.cri_start_samples(
      format.sample_rate_hz, format.capture_offset_samples);
  if (!(samples_per_bit > 0.0) || !(cri_start > 0.0)) {
    return windows;
  }

  const double valid_samples = static_cast<double>(format.valid_samples);
  const auto clamp_to_record = [valid_samples](double position) {
    return static_cast<uint32_t>(
        std::max(0.0, std::min(position, valid_samples)));
  };

  // Everything between the start of the record and the run-in is back porch,
  // so it is the line's own logic 0 reference.  A bt8x8 PAL record opens
  // inside the burst window, but the burst remnant is symmetric about
  // blanking and a median is unmoved by it.
  windows.quiet_begin = 0;
  windows.quiet_end =
      clamp_to_record(std::floor(cri_start - quiet_guard_samples));

  // Run-in: whole samples strictly inside the sixteen alternating bits.
  windows.cri_begin = clamp_to_record(std::ceil(cri_start));
  windows.cri_end = clamp_to_record(
      std::floor(cri_start + samples_per_bit * service.cri_bits));

  // Logic 1 reference: the centre bit of the framing code's leading run of
  // ones, which is settled even on a channel blurred to a full bit period.
  if (service.frc_leading_ones >= 3) {
    const double run_start_bit = static_cast<double>(service.cri_bits);
    const double centre_bit =
        run_start_bit + static_cast<double>(service.frc_leading_ones) / 2.0;
    const double reference_begin =
        cri_start + samples_per_bit * (centre_bit - 0.5);
    const double reference_end =
        cri_start + samples_per_bit * (centre_bit + 0.5);
    windows.frc_reference_begin = clamp_to_record(std::ceil(reference_begin));
    windows.frc_reference_end = clamp_to_record(std::floor(reference_end));
    if (windows.frc_reference_end <= windows.frc_reference_begin) {
      // A source sampled at barely more than one sample per bit still needs a
      // reference sample; take the one nearest the centre of the run.
      windows.frc_reference_begin =
          clamp_to_record(std::floor(cri_start + samples_per_bit * centre_bit));
      windows.frc_reference_end = clamp_to_record(
          static_cast<double>(windows.frc_reference_begin) + 1.0);
    }
  }

  return windows;
}

VBILineLevels estimate_vbi_line_levels(const std::vector<double>& samples,
                                       const VBIRecordWindows& windows,
                                       double minimum_amplitude_counts) {
  VBILineLevels levels;

  const std::vector<double> quiet =
      window_samples(samples, windows.quiet_begin, windows.quiet_end);
  if (quiet.empty()) {
    return levels;
  }
  levels.logic0 = median_of(quiet);

  std::vector<double> cri =
      window_samples(samples, windows.cri_begin, windows.cri_end);
  if (!cri.empty()) {
    std::sort(cri.begin(), cri.end());
    levels.cri_logic1 = sorted_quantile(cri, kCRIPeakQuantile);
    levels.cri_peak_to_peak =
        levels.cri_logic1 - sorted_quantile(cri, kCRITroughQuantile);
  }

  const std::vector<double> frc = window_samples(
      samples, windows.frc_reference_begin, windows.frc_reference_end);
  if (!frc.empty()) {
    levels.frc_logic1 = mean_of(frc);
  }

  // The larger of the two candidates wins.  On a clean line they agree; on a
  // band-limited one the run-in has collapsed towards the data midpoint and
  // only the framing code still reaches logic 1 (design §5.4).
  levels.logic1 = std::max(levels.cri_logic1, levels.frc_logic1);

  const double frc_amplitude = levels.frc_logic1 - levels.logic0;
  if (frc_amplitude > 0.0) {
    levels.cri_frc_ratio = levels.cri_peak_to_peak / frc_amplitude;
  }

  levels.usable = levels.amplitude() >= minimum_amplitude_counts;
  return levels;
}

double map_vbi_sample(double sample, const VBILineLevels& levels,
                      const VBIOutputLevels& output_levels) {
  const double source_amplitude = levels.amplitude();
  if (!(source_amplitude > 0.0)) {
    return static_cast<double>(output_levels.blanking);
  }

  const double scale =
      static_cast<double>(output_levels.data_amplitude()) / source_amplitude;
  return static_cast<double>(output_levels.logic0) +
         (sample - levels.logic0) * scale;
}

VBILevelMapper::VBILevelMapper(VBISourceFormat format,
                               VBITeletextService service,
                               VBIOutputLevels output_levels,
                               VBILevelMapperConfig config)
    : format_(std::move(format)),
      service_(std::move(service)),
      output_levels_(output_levels),
      config_(config),
      windows_(
          vbi_record_windows(format_, service_, config_.quiet_guard_samples)) {}

VBILineLevels VBILevelMapper::frame_levels(
    const std::vector<VBILineLevels>& measured) const {
  VBILineLevels levels;

  std::vector<double> logic0_values;
  std::vector<double> amplitude_values;
  logic0_values.reserve(measured.size());
  amplitude_values.reserve(measured.size());
  for (const VBILineLevels& line : measured) {
    if (!line.usable) {
      continue;
    }
    logic0_values.push_back(line.logic0);
    amplitude_values.push_back(line.amplitude());
  }
  if (logic0_values.empty()) {
    return levels;
  }

  levels.logic0 = median_of(std::move(logic0_values));
  levels.logic1 = levels.logic0 + median_of(std::move(amplitude_values));
  levels.usable = levels.amplitude() >= config_.minimum_amplitude_counts;
  return levels;
}

void VBILevelMapper::map_frame(const std::vector<VBILineRecord>& records,
                               std::vector<VBIMappedLine>& out_lines) const {
  out_lines.clear();
  out_lines.reserve(records.size());

  const bool fixed = config_.mode == VBILevelMode::kFixed;

  std::vector<VBILineLevels> measured;
  measured.reserve(records.size());
  for (const VBILineRecord& record : records) {
    if (fixed) {
      measured.push_back(VBILineLevels{});
      continue;
    }
    measured.push_back(estimate_vbi_line_levels(
        record.samples, windows_, config_.minimum_amplitude_counts));
  }

  VBILineLevels common;
  if (fixed) {
    common.logic0 = config_.fixed_logic0;
    common.logic1 = config_.fixed_logic1;
    common.usable = common.amplitude() >= config_.minimum_amplitude_counts;
  } else {
    common = frame_levels(measured);
  }

  for (size_t index = 0; index < records.size(); ++index) {
    const VBILineRecord& record = records[index];
    const VBILineLevels& own = measured[index];

    VBIMappedLine line;
    line.frame_index = record.frame_index;
    line.field_index = record.field_index;
    line.record_index = record.record_index;
    line.measured_levels = own;

    if (!common.usable) {
      // Nothing in the frame gave a level reference, so there is no honest
      // scale to apply.  The record becomes ordinary blanking rather than
      // noise amplified by a fabricated gain (design §5.3.4).
      line.levels_established = false;
      line.samples.assign(record.samples.size(),
                          static_cast<double>(output_levels_.blanking));
      out_lines.push_back(std::move(line));
      continue;
    }

    line.levels_established = true;
    line.applied_levels = common;

    if (!fixed && own.usable) {
      // A line far below the frame's amplitude carries no data service, so
      // its own estimate is noise and the frame's levels are used instead.
      const double amplitude_floor =
          common.amplitude() * config_.minimum_amplitude_fraction;
      const bool carries_data = own.amplitude() >= amplitude_floor;

      bool apply_own = carries_data;
      if (carries_data && config_.mode == VBILevelMode::kRolling) {
        // Hold the line at the frame's median unless it departs from it by
        // more than estimation noise, so that per-line gain noise is not
        // propagated into the output while a real gain change still is.
        const double deviation =
            std::fabs(own.amplitude() - common.amplitude());
        apply_own =
            deviation > common.amplitude() * config_.rolling_deviation_fraction;
      }

      if (apply_own) {
        line.applied_levels = own;
        line.used_own_estimate = true;
      }
    }

    line.samples.reserve(record.samples.size());
    for (const double sample : record.samples) {
      line.samples.push_back(
          map_vbi_sample(sample, line.applied_levels, output_levels_));
    }

    out_lines.push_back(std::move(line));
  }
}

}  // namespace orc
