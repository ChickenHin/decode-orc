/*
 * File:        nabts_captions.cpp
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     The NABTS caption service as a cue list (CEA-516 §7.3.10)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/stage/analysis_sink_results.h>
#include <orc/support/nabts_page.h>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace orc {

namespace {

/// CEA-516 §5.2.2: types 0, 1 and 3 carry presentation data, which §6.1 makes
/// NAPLPS. Only those have anything a caption could be read out of.
bool type_is_presentation(uint8_t type) {
  return type == 0 || type == 1 || type == 3;
}

}  // namespace

std::vector<NabtsCaptionCue> nabts_caption_cues(
    const std::vector<NabtsCataloguedRecord>& records) {
  std::vector<NabtsCaptionCue> cues;
  for (const NabtsCataloguedRecord& record : records) {
    // The Caption Flag of §5.2.7.3, not the channel: §7.3.10 makes A00/000 the
    // entry point a receiver acquires captioning through, while the captions
    // themselves are whatever records carry the flag.
    if (!record.caption || !type_is_presentation(record.record_type)) {
      continue;
    }
    NabtsCaptionCue cue;
    cue.start_frame = record.first_seen_frame;
    cue.end_frame = record.last_seen_frame;
    cue.channel = record.channel;
    cue.address_text = record.address_text;
    cue.version = record.version;
    cue.text = nabts_page_text(record.page);
    cues.push_back(std::move(cue));
  }

  std::sort(cues.begin(), cues.end(),
            [](const NabtsCaptionCue& lhs, const NabtsCaptionCue& rhs) {
              if (lhs.start_frame != rhs.start_frame) {
                return lhs.start_frame < rhs.start_frame;
              }
              // Two captions first seen in the same frame are ordered by the
              // version §7.3.10.1 has the service increment per caption.
              return lhs.version < rhs.version;
            });

  std::vector<NabtsCaptionCue> out;
  out.reserve(cues.size());
  for (size_t i = 0; i < cues.size(); ++i) {
    const uint64_t next_start =
        (i + 1 < cues.size()) ? cues[i + 1].start_frame : cues[i].end_frame;
    if (cues[i].text.empty()) {
      continue;  // an erase, which ends the cue before it
    }
    NabtsCaptionCue cue = cues[i];
    // A cue always covers at least the frame it was first seen at, so the
    // timing stays monotonic even where two captions share a frame.
    cue.end_frame = std::max(next_start, cue.start_frame + 1);
    out.push_back(std::move(cue));
  }
  return out;
}

}  // namespace orc
