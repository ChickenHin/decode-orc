/*
 * File:        subtitle_embed_policy.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Selects which subtitle source the video sink embeds into the
 *              single mov_text stream, and why a requested source was dropped
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_SUBTITLE_EMBED_POLICY_H
#define ORC_CORE_SUBTITLE_EMBED_POLICY_H

#include <string>

namespace orc {

// The output carries at most one mov_text subtitle stream, so at most one
// subtitle source can be embedded per export.
enum class SubtitleEmbedSource { kNone, kClosedCaptions };

// Everything the selection depends on.
//
// The subtitle source reads its data from the observation context, never from
// the pipeline's VideoFrameRepresentation, so no representation appears here:
// subtitle embedding must stay independent of audio embedding, which is the
// only consumer of the representation.
struct SubtitleEmbedRequest {
  bool embed_closed_captions = false;
  std::string container_format;  ///< "mp4", "mov", "mkv", "mxf", ...
  bool has_observation_context = false;
};

struct SubtitleEmbedDecision {
  SubtitleEmbedSource source = SubtitleEmbedSource::kNone;
  /// Why closed captions were requested but not selected; empty otherwise.
  std::string closed_caption_reason;
};

// Resolve the subtitle source for an export.
//
// A requested source is dropped when the container cannot carry mov_text
// (only MP4/MOV can), or when no observation context is available to read the
// data from.
inline SubtitleEmbedDecision select_subtitle_embed_source(
    const SubtitleEmbedRequest& request) {
  const bool mov_text_container =
      (request.container_format == "mp4" || request.container_format == "mov");

  SubtitleEmbedDecision decision;

  bool closed_captions_viable = request.embed_closed_captions;
  if (request.embed_closed_captions) {
    if (!mov_text_container) {
      closed_captions_viable = false;
      decision.closed_caption_reason =
          "Closed captions are only supported in MP4/MOV containers";
    } else if (!request.has_observation_context) {
      closed_captions_viable = false;
      decision.closed_caption_reason =
          "No observation context provided, closed caption data unavailable";
    }
  }

  if (closed_captions_viable) {
    decision.source = SubtitleEmbedSource::kClosedCaptions;
  }
  return decision;
}

}  // namespace orc

#endif  // ORC_CORE_SUBTITLE_EMBED_POLICY_H
