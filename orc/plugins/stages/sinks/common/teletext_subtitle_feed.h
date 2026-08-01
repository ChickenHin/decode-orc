/*
 * File:        teletext_subtitle_feed.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Collect teletext subtitle cues from "teletext" observations
 *              for mov_text embedding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SUBTITLE_FEED_H
#define ORC_TELETEXT_SUBTITLE_FEED_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/support/eia608_decoder.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Decode teletext subtitles from stored "teletext" observations.
 *
 * Reads the per-line t42_<field_line> hex observations of the host teletext
 * observer over [field_start, field_start + field_count), feeds the packets
 * in temporal order into a TeletextPageDecoder watching |subtitle_page|
 * ("888"-style magazine + two hex digits), and converts the resulting cues
 * to seconds relative to |field_start| (625-line PAL: 50 fields/s).
 *
 * Returns an empty vector when |subtitle_page| is malformed or no subtitle
 * transmissions are present. The cue text carries plain rows separated by
 * newlines (Level 1 attributes dropped) — the same contract as the EIA-608
 * cues consumed by the mov_text muxing path.
 */
std::vector<CaptionCue> collect_teletext_subtitle_cues(
    const IObservationContext& observation_context, uint64_t field_start,
    uint64_t field_count, const std::string& subtitle_page);

}  // namespace orc

#endif  // ORC_TELETEXT_SUBTITLE_FEED_H
