/*
 * File:        vbi_line_placement.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Horizontal placement of a stored record on an output frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_placement.h"

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

// Clamp a fractional output index onto the frame line and return it as a
// whole sample index.
uint32_t clamp_output_index(double index, uint32_t line_length) {
  if (!std::isfinite(index) || index <= 0.0) {
    return 0;
  }
  if (index >= static_cast<double>(line_length)) {
    return line_length;
  }
  return static_cast<uint32_t>(index);
}

}  // namespace

bool make_vbi_line_placement(const VBISourceFormat& format,
                             const VBITeletextService& service,
                             const VBIFrameGeometry& geometry,
                             double capture_offset_samples, uint32_t frame_line,
                             VBILinePlacement& out_placement,
                             std::string& error_message) {
  out_placement = VBILinePlacement();

  if (frame_line >= geometry.lines_per_frame()) {
    error_message = "Frame line " + std::to_string(frame_line) +
                    " is outside the frame geometry, which holds " +
                    std::to_string(geometry.lines_per_frame()) + " lines.";
    return false;
  }

  double output_sample_rate_hz = 0.0;
  if (!vbi_output_sample_rate_hz(format.tv_system, output_sample_rate_hz,
                                 error_message)) {
    return false;
  }

  if (!(format.sample_rate_hz > 0.0) || !std::isfinite(format.sample_rate_hz)) {
    error_message =
        "The source sampling rate must be a positive number of hertz.";
    return false;
  }

  if (format.valid_samples == 0) {
    error_message =
        "The source format declares no valid samples per line record, so "
        "there is nothing to place on the output line.";
    return false;
  }

  if (!std::isfinite(capture_offset_samples)) {
    error_message = "The capture offset must be a finite number of samples.";
    return false;
  }

  const double ratio = format.sample_rate_hz / output_sample_rate_hz;
  const double line_phase = geometry.line_phase(frame_line);
  const uint32_t line_length = geometry.line_length(frame_line);

  out_placement.source_samples_per_output_sample = ratio;

  // Output sample j of this line is at (phase + j) / fs_out after 0H, and
  // record sample n is at (capture_offset + n) / fs_in after the same 0H.
  out_placement.source_position_at_output_zero =
      ratio * line_phase - capture_offset_samples;

  // The service's anchor is defined from 0H, so the line's lattice phase is
  // subtracted rather than added: a line that starts late in its own sampling
  // instant reaches a given time in fewer samples.
  out_placement.data_start_samples =
      service.t_offset_ns * 1e-9 * output_sample_rate_hz - line_phase;

  const double transmitted_bits =
      static_cast<double>(service.cri_bits) +
      static_cast<double>(service.frc_bits) +
      static_cast<double>(service.payload_bytes) * 8.0;
  out_placement.data_end_samples =
      (service.bit_rate_hz > 0.0)
          ? out_placement.data_start_samples +
                (transmitted_bits / service.bit_rate_hz) * output_sample_rate_hz
          : out_placement.data_start_samples;

  // Output samples whose source coordinate falls inside the record's valid
  // samples.  Padding is already excluded upstream by the reader, so the last
  // readable coordinate is one short of the valid count.
  const double first_covered =
      std::ceil(-out_placement.source_position_at_output_zero / ratio);
  const double last_covered =
      std::floor((static_cast<double>(format.valid_samples - 1u) -
                  out_placement.source_position_at_output_zero) /
                 ratio);

  out_placement.output_begin = clamp_output_index(first_covered, line_length);
  out_placement.output_end =
      clamp_output_index(last_covered + 1.0, line_length);
  out_placement.output_end =
      std::max(out_placement.output_end, out_placement.output_begin);

  return true;
}

void resample_vbi_line(const IVBIResampler& resampler,
                       const std::vector<double>& record_samples,
                       const VBILinePlacement& placement,
                       std::vector<double>& out_samples) {
  const uint32_t count = placement.output_count();
  out_samples.assign(count, 0.0);
  if (count == 0 || record_samples.empty()) {
    return;
  }

  const double first_position =
      placement.source_position(static_cast<double>(placement.output_begin));

  // The placement is the authority on where every output sample reads from.
  // A resampler built for a different ratio would walk a different stride, so
  // its own regular-stride path is only used when the two agree.
  const double stride = placement.source_samples_per_output_sample;
  const double stride_difference = std::abs(resampler.ratio() - stride);
  if (stride_difference <= std::abs(stride) * 1e-12) {
    resampler.resample(record_samples, first_position, count, out_samples);
    return;
  }

  for (uint32_t index = 0; index < count; ++index) {
    out_samples[index] = resampler.sample_at(
        record_samples, placement.source_position(static_cast<double>(
                            placement.output_begin + index)));
  }
}

}  // namespace orc
