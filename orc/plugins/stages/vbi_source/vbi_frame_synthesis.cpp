/*
 * File:        vbi_frame_synthesis.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Assembles synthesised CVBS frames from mapped VBI line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_synthesis.h"

#include <algorithm>
#include <cmath>

#include "vbi_line_placement.h"

namespace orc {

namespace {

// EBU Tech. 3280-E Section 1.2: a 625-line frame sampled at 4 x fsc holds
// 709 379 samples, being 625 lines of 1135.0064 rather than 625 lines of 1135.
constexpr uint32_t kPALNormativeFrameSamples = 709379;

}  // namespace

bool vbi_normative_frame_samples(VBITVSystem tv_system,
                                 uint32_t& out_samples_per_frame,
                                 std::string& error_message) {
  out_samples_per_frame = 0;
  switch (tv_system) {
    case VBITVSystem::kPAL:
      out_samples_per_frame = kPALNormativeFrameSamples;
      return true;

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      error_message =
          "Frame synthesis for 525-line systems is not implemented yet; only "
          "PAL frames can currently be synthesised.";
      return false;
  }

  error_message = "Unrecognised television system.";
  return false;
}

VBIDataWindow vbi_data_region_window(const VBILinePlacement& placement,
                                     double guard_samples,
                                     uint32_t line_length) {
  VBIDataWindow window;
  if (line_length == 0) {
    return window;
  }

  const double guard = (guard_samples > 0.0) ? guard_samples : 0.0;
  const double first = std::floor(placement.data_start_samples - guard);
  const double last = std::ceil(placement.data_end_samples + guard);

  const double begin = std::max(static_cast<double>(placement.output_begin),
                                std::max(0.0, first));
  const double end =
      std::min(static_cast<double>(placement.output_end),
               std::min(static_cast<double>(line_length), std::max(0.0, last)));

  window.begin = static_cast<uint32_t>(begin);
  window.end = static_cast<uint32_t>(std::max(begin, end));
  return window;
}

VBIFrameSynthesiser::VBIFrameSynthesiser(
    VBISourceFormat format, VBITeletextService service,
    VBIFrameGeometry geometry, VBIVerticalInterval vertical_interval,
    VBITeletextLineMap line_map, VBIOutputLevels levels,
    VBIBurstTiming burst_timing, VBIFrameSynthesisConfig config,
    double output_sample_rate_hz)
    : format_(format),
      service_(service),
      geometry_(geometry),
      output_sample_rate_hz_(output_sample_rate_hz),
      line_map_(std::move(line_map)),
      levels_(levels),
      config_(config),
      line_synthesiser_(geometry, vertical_interval, levels,
                        output_sample_rate_hz),
      burst_synthesiser_(geometry, levels, burst_timing,
                         output_sample_rate_hz) {}

double VBIFrameSynthesiser::data_guard_samples() const {
  return service_.samples_per_bit(output_sample_rate_hz_);
}

bool VBIFrameSynthesiser::assemble_manufactured_frame(
    uint64_t output_frame_index, VBISynthesisedFrame& out_frame,
    std::vector<double>& line_buffer, std::string& error_message) const {
  const uint32_t lines = geometry_.lines_per_frame();
  const uint32_t frame_samples = geometry_.samples_per_frame();
  if (lines == 0 || frame_samples == 0) {
    error_message =
        "The frame synthesiser has no output geometry, so no frame can be "
        "assembled.";
    return false;
  }

  uint32_t normative_samples = 0;
  if (!vbi_normative_frame_samples(format_.tv_system, normative_samples,
                                   error_message)) {
    return false;
  }
  if (frame_samples != normative_samples) {
    error_message = "The output geometry describes a frame of " +
                    std::to_string(frame_samples) +
                    " samples but the standard requires exactly " +
                    std::to_string(normative_samples) + ".";
    return false;
  }

  out_frame.output_frame_index = output_frame_index;
  out_frame.burst_synthesised = config_.synthesise_burst;
  out_frame.data_frame_lines.clear();
  out_frame.samples.assign(frame_samples, levels_.blanking);

  // Written sample count, accumulated line by line rather than assumed.  The
  // normative frame size is the check that catches a geometry mistake at once:
  // a constant-stride PAL frame is four samples short and desynchronises every
  // frame after it (design §2.1, §8).
  uint64_t written = 0;
  for (uint32_t line = 0; line < lines; ++line) {
    const uint32_t line_start = geometry_.line_start(line);
    const uint32_t line_length = geometry_.line_length(line);
    if (static_cast<uint64_t>(line_start) + line_length > frame_samples) {
      error_message = "Frame line " + std::to_string(line) +
                      " runs past the end of the " +
                      std::to_string(frame_samples) +
                      " sample frame, so the output geometry is inconsistent.";
      return false;
    }

    line_synthesiser_.synthesise_line(line, line_buffer);
    if (config_.synthesise_burst) {
      burst_synthesiser_.synthesise_burst(output_frame_index, line,
                                          line_buffer);
    }

    for (uint32_t index = 0; index < line_length; ++index) {
      out_frame.samples[line_start + index] =
          clamp_vbi_output_sample(line_buffer[index]);
    }
    written += line_length;
  }

  if (written != frame_samples) {
    error_message = "The assembled frame holds " + std::to_string(written) +
                    " samples but the standard requires exactly " +
                    std::to_string(frame_samples) +
                    "; the frame geometry has been violated.";
    return false;
  }

  return true;
}

bool VBIFrameSynthesiser::synthesise_blank_frame(
    uint64_t output_frame_index, VBISynthesisedFrame& out_frame,
    std::string& error_message) const {
  out_frame = VBISynthesisedFrame();
  std::vector<double> line_buffer;
  if (!assemble_manufactured_frame(output_frame_index, out_frame, line_buffer,
                                   error_message)) {
    return false;
  }
  out_frame.padding = true;
  return true;
}

bool VBIFrameSynthesiser::synthesise_frame(
    uint64_t output_frame_index, const std::vector<VBIMappedLine>& mapped_lines,
    const IVBIResampler& resampler, double capture_offset_samples,
    VBISynthesisedFrame& out_frame, std::string& error_message) const {
  out_frame = VBISynthesisedFrame();
  std::vector<double> line_buffer;
  if (!assemble_manufactured_frame(output_frame_index, out_frame, line_buffer,
                                   error_message)) {
    return false;
  }

  std::vector<double> resampled;
  for (const VBIMappedLine& mapped : mapped_lines) {
    if (!mapped.levels_established || mapped.samples.empty()) {
      // No level reference could be established, so there is nothing to
      // normalise the record against.  The line stays as manufactured
      // blanking rather than carrying an arbitrarily scaled waveform.
      continue;
    }

    uint32_t frame_line = 0;
    if (!map_vbi_record_to_frame_line(format_, line_map_, mapped.field_index,
                                      mapped.record_index, frame_line,
                                      error_message)) {
      return false;
    }

    if (std::find(out_frame.data_frame_lines.begin(),
                  out_frame.data_frame_lines.end(),
                  frame_line) != out_frame.data_frame_lines.end()) {
      error_message =
          "Stored field " + std::to_string(mapped.field_index) + " record " +
          std::to_string(mapped.record_index) + " maps to frame line " +
          std::to_string(frame_line) +
          ", which another record of the same frame already occupies.";
      return false;
    }

    VBILinePlacement placement;
    if (!make_vbi_line_placement(format_, service_, geometry_,
                                 capture_offset_samples, frame_line, placement,
                                 error_message)) {
      return false;
    }

    line_synthesiser_.synthesise_line(frame_line, line_buffer);
    if (config_.synthesise_burst) {
      burst_synthesiser_.synthesise_burst(output_frame_index, frame_line,
                                          line_buffer);
    }

    resample_vbi_line(resampler, mapped.samples, placement, resampled);
    const uint32_t line_start = geometry_.line_start(frame_line);
    const uint32_t line_length = geometry_.line_length(frame_line);

    // Only the data region takes source samples; the sync, the burst and the
    // porches either side of it stay as manufactured (design §5.6).
    const VBIDataWindow window =
        vbi_data_region_window(placement, data_guard_samples(), line_length);
    for (uint32_t line_index = window.begin; line_index < window.end;
         ++line_index) {
      const uint32_t offset = line_index - placement.output_begin;
      if (offset >= resampled.size()) {
        break;
      }
      line_buffer[line_index] = resampled[offset];
    }

    for (uint32_t index = 0; index < line_length; ++index) {
      out_frame.samples[line_start + index] =
          clamp_vbi_output_sample(line_buffer[index]);
    }

    out_frame.data_frame_lines.push_back(frame_line);
  }

  std::sort(out_frame.data_frame_lines.begin(),
            out_frame.data_frame_lines.end());
  out_frame.padding = out_frame.data_frame_lines.empty();
  return true;
}

bool make_vbi_frame_synthesiser(const VBISourceFormat& format,
                                const VBIFrameSynthesisConfig& config,
                                VBIFrameSynthesiser& out_synthesiser,
                                std::string& error_message) {
  out_synthesiser = VBIFrameSynthesiser();

  VBIFrameGeometry geometry;
  if (!make_vbi_frame_geometry(format.tv_system, geometry, error_message)) {
    return false;
  }

  double output_sample_rate_hz = 0.0;
  if (!vbi_output_sample_rate_hz(format.tv_system, output_sample_rate_hz,
                                 error_message)) {
    return false;
  }

  VBIVerticalInterval vertical_interval;
  if (!make_vbi_vertical_interval(format.tv_system, vertical_interval,
                                  error_message)) {
    return false;
  }

  VBITeletextLineMap line_map;
  if (!make_vbi_teletext_line_map(format.tv_system, format.tt_system, line_map,
                                  error_message)) {
    return false;
  }

  VBIOutputLevels levels;
  if (!vbi_output_levels(format.tv_system, levels, error_message)) {
    return false;
  }

  VBITeletextService service;
  if (!vbi_teletext_service(format.tv_system, format.tt_system, service,
                            error_message)) {
    return false;
  }

  VBIBurstTiming burst_timing;
  if (config.synthesise_burst &&
      !make_vbi_burst_timing(format.tv_system, levels, burst_timing,
                             error_message)) {
    return false;
  }

  out_synthesiser = VBIFrameSynthesiser(
      format, service, geometry, vertical_interval, line_map, levels,
      burst_timing, config, output_sample_rate_hz);
  return true;
}

}  // namespace orc
