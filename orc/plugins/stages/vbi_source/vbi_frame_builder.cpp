/*
 * File:        vbi_frame_builder.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Places VBI line records on an otherwise blank CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_builder.h"

#include <algorithm>
#include <utility>

#include "vbi_line_mapping.h"
#include "vbi_teletext_service.h"

namespace orc {

namespace {

const std::vector<uint32_t>& empty_frame_lines() {
  static const std::vector<uint32_t> empty;
  return empty;
}

}  // namespace

VBIFrameBuilder::VBIFrameBuilder(
    VBISourceFormat format, VBIOutputFrame output_frame,
    VBIDataPlacement placement, VBIRecordResampler resampler,
    VBILevelMapper level_mapper,
    std::array<std::vector<uint32_t>, 2> frame_lines)
    : format_(format),
      output_frame_(output_frame),
      placement_(placement),
      resampler_(std::move(resampler)),
      level_mapper_(std::move(level_mapper)),
      frame_lines_(std::move(frame_lines)) {}

const std::vector<uint32_t>& VBIFrameBuilder::frame_lines(
    uint32_t stored_field_index) const {
  if (stored_field_index >= frame_lines_.size()) {
    return empty_frame_lines();
  }
  return frame_lines_[stored_field_index];
}

void VBIFrameBuilder::build_blank_frame(
    std::vector<int16_t>& out_samples) const {
  out_samples.assign(output_frame_.samples_per_frame,
                     static_cast<int16_t>(output_frame_.levels.blanking));
}

bool VBIFrameBuilder::build_frame(const std::vector<VBILineRecord>& records,
                                  std::vector<int16_t>& out_samples,
                                  uint32_t& out_data_lines,
                                  std::string& error_message) const {
  out_data_lines = 0;

  if (output_frame_.samples_per_frame == 0 || resampler_.output_count() == 0) {
    error_message =
        "The frame builder has no output lattice, so no frame can be built.";
    return false;
  }

  build_blank_frame(out_samples);

  std::vector<VBILineLevels> levels;
  level_mapper_.map_frame(records, levels);

  std::vector<double> resampled;
  for (size_t index = 0; index < records.size(); ++index) {
    const VBILineRecord& record = records[index];
    if (record.samples.empty() || !levels[index].usable) {
      // No level reference could be established, so there is nothing to
      // normalise the record against.  The line stays as blanking rather than
      // carrying an arbitrarily scaled waveform.
      continue;
    }

    const std::vector<uint32_t>& lines = frame_lines(record.field_index);
    if (record.record_index < format_.field_range.start ||
        record.record_index - format_.field_range.start >= lines.size()) {
      error_message = "Stored field " + std::to_string(record.field_index) +
                      " record " + std::to_string(record.record_index) +
                      " is outside the configured field range, so there is no "
                      "frame line for it.";
      return false;
    }
    const uint32_t frame_line =
        lines[record.record_index - format_.field_range.start];

    resampler_.resample(record.samples, resampled);

    const VBISampleMap map =
        make_vbi_sample_map(levels[index], output_frame_.levels);
    int16_t* line = out_samples.data() + output_frame_.line_offset(frame_line) +
                    placement_.output_begin;
    for (size_t sample = 0; sample < resampled.size(); ++sample) {
      line[sample] = clamp_vbi_output_sample(map.apply(resampled[sample]));
    }

    ++out_data_lines;
  }

  return true;
}

bool make_vbi_frame_builder(const VBISourceFormat& format,
                            const VBILevelMapperConfig& level_config,
                            double capture_offset_samples,
                            VBIFrameBuilder& out_builder,
                            std::string& error_message) {
  out_builder = VBIFrameBuilder();

  VBIOutputFrame output_frame;
  if (!make_vbi_output_frame(format.tv_system, format.tt_system, output_frame,
                             error_message)) {
    return false;
  }

  VBITeletextService service;
  if (!vbi_teletext_service(format.tv_system, format.tt_system, service,
                            error_message)) {
    return false;
  }

  VBITeletextLineMap line_map;
  if (!make_vbi_teletext_line_map(format.tv_system, format.tt_system, line_map,
                                  error_message)) {
    return false;
  }

  VBIDataPlacement placement;
  if (!make_vbi_data_placement(format, service, output_frame,
                               capture_offset_samples, placement,
                               error_message)) {
    return false;
  }
  if (placement.output_count() == 0) {
    error_message =
        "No part of a line record falls inside the data region of its output "
        "line, so the capture offset or the sampling rate must be wrong.";
    return false;
  }

  // Resolve every record's frame line once, and reject a configuration that
  // puts two records of a frame on the same line: that is a wrong field range,
  // and truncating or overwriting silently would hide it (design §5.1).
  std::array<std::vector<uint32_t>, 2> frame_lines;
  std::vector<uint32_t> taken;
  for (uint32_t field = 0; field < 2u; ++field) {
    for (uint32_t record = format.field_range.start;
         record <= format.field_range.end; ++record) {
      uint32_t frame_line = 0;
      if (!map_vbi_record_to_frame_line(format, line_map, field, record,
                                        frame_line, error_message)) {
        return false;
      }
      if (std::find(taken.begin(), taken.end(), frame_line) != taken.end()) {
        error_message = "Stored field " + std::to_string(field) + " record " +
                        std::to_string(record) + " maps to frame line " +
                        std::to_string(frame_line) +
                        ", which another record of the same frame already "
                        "occupies.";
        return false;
      }
      if (static_cast<uint64_t>(frame_line) >= output_frame.lines_per_frame) {
        error_message = "Frame line " + std::to_string(frame_line) +
                        " is outside the " +
                        std::to_string(output_frame.lines_per_frame) +
                        " line output frame.";
        return false;
      }
      taken.push_back(frame_line);
      frame_lines[field].push_back(frame_line);
    }
  }

  // A time-base corrected capture arrives with its levels already fixed, so the
  // level policy is not the caller's to choose: there is nothing to estimate
  // and the estimate would be wrong if it were made (vbi_level_mapper.h).
  const VBILevelMapperConfig levels =
      (format.family == VBISourceFamily::kTBCDerived)
          ? vbi_absolute_level_config(output_frame.levels)
          : level_config;

  VBIRecordResampler resampler(placement, format.valid_samples);
  VBILevelMapper level_mapper(format, service, levels);

  out_builder =
      VBIFrameBuilder(format, output_frame, placement, std::move(resampler),
                      std::move(level_mapper), std::move(frame_lines));
  return true;
}

}  // namespace orc
