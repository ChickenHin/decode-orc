/*
 * File:        teletext_subtitle_feed.cpp
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Collect teletext subtitle cues from "teletext" observations
 *              for mov_text embedding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_subtitle_feed.h"

#include <orc/support/logging.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>

#include <string>
#include <variant>

namespace orc {

namespace {

// The host teletext observer's fixed candidate window: 0-based field lines
// 5-21 in both fields (broadcast lines 6-22 / 318-335, EN 300 706 §4.1).
constexpr int32_t kFirstCandidateFieldLine = 5;
constexpr int32_t kLastCandidateFieldLine = 21;

// ITU-R BT.1700 Annex 1 Part B Table 1 item 2: 625-line PAL scans 50 fields
// per second.
constexpr double kPalFieldsPerSecond = 50.0;

}  // namespace

std::vector<CaptionCue> collect_teletext_subtitle_cues(
    const IObservationContext& observation_context, uint64_t field_start,
    uint64_t field_count, const std::string& subtitle_page) {
  TeletextPageDecoder decoder;
  if (!decoder.set_subtitle_page(subtitle_page)) {
    ORC_LOG_WARN(
        "TeletextSubtitleFeed: invalid subtitle page '{}' (expected magazine "
        "digit 1-8 plus two hex digits, e.g. 888)",
        subtitle_page);
    return {};
  }

  for (uint64_t offset = 0; offset < field_count; ++offset) {
    const FieldID field_id(field_start + offset);
    for (int32_t line = kFirstCandidateFieldLine;
         line <= kLastCandidateFieldLine; ++line) {
      const auto observation = observation_context.get(
          field_id, "teletext", "t42_" + std::to_string(line));
      if (!observation || !std::holds_alternative<std::string>(*observation)) {
        continue;
      }
      const auto packet =
          teletext_hex_to_packet(std::get<std::string>(*observation));
      if (packet.has_value()) {
        decoder.process_packet(*packet, static_cast<int64_t>(offset));
      }
    }
  }
  decoder.finalize(static_cast<int64_t>(field_count));

  std::vector<CaptionCue> cues;
  cues.reserve(decoder.subtitle_cues().size());
  for (const auto& cue : decoder.subtitle_cues()) {
    cues.emplace_back(
        static_cast<double>(cue.start_field_index) / kPalFieldsPerSecond,
        static_cast<double>(cue.end_field_index) / kPalFieldsPerSecond,
        cue.text);
  }
  return cues;
}

}  // namespace orc
