/*
 * File:        teletext_observer.cpp
 * Module:      orc-core
 * Purpose:     PAL WST teletext observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/teletext_slicer.h>
#include <teletext_observer.h>

#include <string>
#include <vector>

namespace orc {

namespace {

// Candidate VBI window as 0-based field lines, identical in both fields.
// ETSI EN 300 706 §4.1: broadcast lines 6 to 22 (field 1) and 318 to 335
// (field 2) may carry teletext; in the frame-flat buffer these are 0-based
// field lines 5-21 of each field (field 2 offset by kPalField1Lines = 313).
constexpr size_t kFirstCandidateFieldLine = 5;
constexpr size_t kLastCandidateFieldLine = 21;

// Observation key for a candidate field line's recovered packet.
std::string t42_key(size_t field_line) {
  return "t42_" + std::to_string(field_line);
}

}  // namespace

TeletextObserver::TeletextObserver()
    // EBU Tech. 3280-E §1.1.1 Table 1: 4FSC PAL sample rate; the bit rate is
    // fixed at 444 × fH by ETSI EN 300 706 §5.3 (TeletextSlicer default).
    : slicer_(kPalSampleRate) {}

void TeletextObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("TeletextObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // ETSI EN 300 706 System B on 625-line PAL is the only supported system
  // (design scope); other systems produce no observations.
  if (vp.system != VideoSystem::PAL) {
    return;
  }

  // Levels from the source, with the spec constants as fallback.
  const int16_t black_level =
      static_cast<int16_t>(vp.black_level >= 0 ? vp.black_level : kPalBlack);
  const int16_t white_level =
      static_cast<int16_t>(vp.white_level >= 0 ? vp.white_level : kPalWhite);

  const size_t f1_lines = field1_lines(vp.system);
  const size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  const size_t frame_height = static_cast<size_t>(vp.frame_height);

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    const FieldID derived_fid(frame_id * 2 + field_idx);
    const size_t line_offset = (field_idx == 0) ? 0 : f1_lines;

    int32_t line_count = 0;
    for (size_t field_line = kFirstCandidateFieldLine;
         field_line <= kLastCandidateFieldLine; ++field_line) {
      const size_t flat_line = line_offset + field_line;
      if (flat_line >= frame_height) {
        continue;
      }

      // Luma-aware fetch: YC sources carry the data burst in the luma
      // channel; composite sources go through get_line_samples() so per-line
      // reads use the source stage's field buffering.
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

      const TeletextLineResult sliced =
          slicer_.slice(line_data, sample_count, black_level, white_level);
      if (!sliced.valid) {
        continue;
      }

      context.set(derived_fid, "teletext", t42_key(field_line),
                  teletext_packet_to_hex(sliced.bytes));
      ++line_count;
    }

    context.set(derived_fid, "teletext", "present", line_count > 0);
    context.set(derived_fid, "teletext", "line_count", line_count);

    if (line_count > 0) {
      ORC_LOG_DEBUG("TeletextObserver: Field {} recovered {} packet(s)",
                    derived_fid.value(), line_count);
    }
  }
}

std::vector<ObservationKey> TeletextObserver::get_provided_observations()
    const {
  std::vector<ObservationKey> keys;
  keys.reserve(2 + (kLastCandidateFieldLine - kFirstCandidateFieldLine + 1));
  keys.emplace_back("teletext", "present", ObservationType::BOOL,
                    "At least one valid teletext packet recovered", true);
  keys.emplace_back("teletext", "line_count", ObservationType::INT32,
                    "Number of candidate VBI lines that yielded packets", true);
  for (size_t field_line = kFirstCandidateFieldLine;
       field_line <= kLastCandidateFieldLine; ++field_line) {
    keys.emplace_back(
        "teletext", t42_key(field_line), ObservationType::STRING,
        "42 recovered T42 bytes (84 hex chars) for 0-based field line " +
            std::to_string(field_line),
        true);
  }
  return keys;
}

}  // namespace orc
