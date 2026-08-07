/*
 * File:        teletext_frame_slicer.cpp
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Per-frame WST teletext line recovery implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_frame_slicer.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <vector>

namespace orc {

namespace {

// The slicer options the stage's tuning parameters map onto. The system is set
// per slicer by the constructor below.
TeletextSlicerOptions to_slicer_options(
    const TeletextFrameSlicerOptions& options) {
  TeletextSlicerOptions slicer_options;
  slicer_options.detector = options.detector;
  slicer_options.parity_repair = options.parity_repair;
  slicer_options.tolerant_framing = options.tolerant_framing;
  slicer_options.require_valid_mrag = options.require_valid_mrag;
  return slicer_options;
}

}  // namespace

TeletextFrameSlicer::TeletextFrameSlicer(TeletextFrameSlicerOptions options)
    : options_(options),
      // EBU Tech. 3280-E §1.1.1 Table 1 / SMPTE 244M-2003 §4.1 / ITU-R
      // BT.1700-1 Annex 1 Part B: the 4FSC sample rate of each system. The bit
      // rate comes from the teletext system (ITU-R BT.653 Tables 1a and 1b).
      slicer_pal_(kPalSampleRate, TeletextSystem::kWst625,
                  to_slicer_options(options)),
      slicer_ntsc_(kNtscSampleRate, TeletextSystem::kWst525,
                   to_slicer_options(options)),
      slicer_palm_(kPalMSampleRate, TeletextSystem::kWst525,
                   to_slicer_options(options)) {}

bool TeletextFrameSlicer::applies_to(VideoSystem system) {
  // ITU-R BT.653 System B, on both the television systems it is defined for:
  // 625 lines (ETSI EN 300 706, PAL) and 525 lines (Table 1b, NTSC and PAL_M).
  return system == VideoSystem::PAL || system == VideoSystem::NTSC ||
         system == VideoSystem::PAL_M;
}

TeletextFrameSlicer::SystemProfile TeletextFrameSlicer::profile_for(
    VideoSystem system) {
  if (system == VideoSystem::PAL) {
    return SystemProfile{TeletextSystem::kWst625, kTeletextFirstFieldLine625,
                         kTeletextLastFieldLine625};
  }
  // NTSC and PAL_M share the 525-line structure and therefore the service
  // (ITU-R BT.653 Table 1b); only the 4FSC sample rate differs between them,
  // which is a property of the slicer rather than of this table.
  return SystemProfile{TeletextSystem::kWst525, kTeletextFirstFieldLine525,
                       kTeletextLastFieldLine525};
}

TeletextFrameSlicer::SystemProfile TeletextFrameSlicer::effective_profile(
    VideoSystem system) const {
  SystemProfile profile = profile_for(system);
  if (options_.first_field_line.has_value()) {
    profile.first_field_line = *options_.first_field_line;
  }
  if (options_.last_field_line.has_value()) {
    profile.last_field_line = *options_.last_field_line;
  }
  return profile;
}

const TeletextSlicer& TeletextFrameSlicer::slicer_for(
    VideoSystem system) const {
  if (system == VideoSystem::PAL) {
    return slicer_pal_;
  }
  return (system == VideoSystem::PAL_M) ? slicer_palm_ : slicer_ntsc_;
}

void TeletextFrameSlicer::slice_field(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    size_t field_idx, std::vector<TeletextFrameLineResult>& results) const {
  results.clear();

  const auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    return;
  }
  const auto& vp = vp_opt.value();
  if (!applies_to(vp.system)) {
    return;
  }

  const SystemProfile profile = effective_profile(vp.system);
  if (profile.last_field_line < profile.first_field_line) {
    return;
  }
  const TeletextSlicer& slicer = slicer_for(vp.system);

  // Levels from the source, with the spec constants as fallback. The data '0'
  // reference is black in both systems (ETSI EN 300 706 §5.2, ITU-R BT.653
  // Table 1b); on a 525-line system the transmitted '0' actually sits at
  // blanking, below the 7,5 IRE setup black, which makes the amplitude gate
  // derived from these levels stricter than the standard requires rather than
  // looser — measured against a real burst it still clears by a factor of two.
  const int16_t default_black = static_cast<int16_t>(
      vp.system == VideoSystem::PAL ? kPalBlack : kNtscBlack);
  const int16_t default_white = static_cast<int16_t>(
      vp.system == VideoSystem::PAL ? kPalWhite : kNtscWhite);
  const int16_t black_level = static_cast<int16_t>(
      vp.black_level >= 0 ? vp.black_level : default_black);
  const int16_t white_level = static_cast<int16_t>(
      vp.white_level >= 0 ? vp.white_level : default_white);

  const size_t f1_lines = field1_lines(vp.system);
  const size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  const size_t frame_height = static_cast<size_t>(vp.frame_height);
  const size_t line_offset = (field_idx == 0) ? 0 : f1_lines;

  results.reserve(
      static_cast<size_t>(profile.last_field_line - profile.first_field_line) +
      1);

  for (int32_t field_line = profile.first_field_line;
       field_line <= profile.last_field_line; ++field_line) {
    if (field_line < 0) {
      continue;
    }
    const size_t flat_line = line_offset + static_cast<size_t>(field_line);
    if (flat_line >= frame_height) {
      continue;
    }

    // Luma-aware fetch: YC sources carry the data burst in the luma channel;
    // composite sources go through get_line_samples() so per-line reads use the
    // source stage's field buffering.
    const int16_t* line_data = nullptr;
    size_t sample_count = 0;
    std::vector<int16_t> line_copy;
    if (representation.has_separate_channels()) {
      line_data = representation.get_line_luma(frame_id, flat_line);
      sample_count = line_width;
    } else {
      line_copy = representation.get_line_samples(frame_id, flat_line);
      line_data = line_copy.data();
      sample_count = line_copy.size();
    }
    if (line_data == nullptr || sample_count == 0) {
      continue;
    }

    TeletextFrameLineResult entry;
    entry.field_line = field_line;
    entry.flat_line = flat_line;
    entry.sliced =
        slicer.slice(line_data, sample_count, black_level, white_level);
    results.push_back(entry);
  }
}

}  // namespace orc
