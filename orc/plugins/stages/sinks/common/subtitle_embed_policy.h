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
enum class SubtitleEmbedSource { kNone, kClosedCaptions, kTeletext };

// Everything the selection depends on.
//
// Both subtitle sources read their data from the observation context, never
// from the pipeline's VideoFrameRepresentation, so no representation appears
// here: subtitle embedding must stay independent of audio embedding, which is
// the only consumer of the representation.
struct SubtitleEmbedRequest {
  bool embed_closed_captions = false;
  bool embed_teletext_subtitles = false;
  std::string container_format;  ///< "mp4", "mov", "mkv", "mxf", ...
  bool video_system_is_pal = false;
  bool has_observation_context = false;
};

struct SubtitleEmbedDecision {
  SubtitleEmbedSource source = SubtitleEmbedSource::kNone;
  /// Why closed captions were requested but not selected; empty otherwise.
  std::string closed_caption_reason;
  /// Why teletext subtitles were requested but not selected; empty otherwise.
  std::string teletext_reason;
};

// Resolve the subtitle source for an export. Closed captions take precedence
// over teletext subtitles when both are requested and both are viable, because
// the mov_text stream can only carry one of them.
//
// A requested source is dropped when the container cannot carry mov_text
// (only MP4/MOV can), when no observation context is available to read the
// data from, or — for teletext — when the video system is not PAL, since
// teletext is PAL World System Teletext only.
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

  bool teletext_viable = request.embed_teletext_subtitles;
  if (request.embed_teletext_subtitles) {
    if (!mov_text_container) {
      teletext_viable = false;
      decision.teletext_reason =
          "Teletext subtitles are only supported in MP4/MOV containers";
    } else if (closed_captions_viable) {
      teletext_viable = false;
      decision.teletext_reason =
          "Closed captions already occupy the subtitle stream";
    } else if (!request.video_system_is_pal) {
      teletext_viable = false;
      decision.teletext_reason = "Teletext subtitles are PAL WST only";
    } else if (!request.has_observation_context) {
      teletext_viable = false;
      decision.teletext_reason =
          "No observation context provided, teletext data unavailable";
    }
  }

  if (closed_captions_viable) {
    decision.source = SubtitleEmbedSource::kClosedCaptions;
  } else if (teletext_viable) {
    decision.source = SubtitleEmbedSource::kTeletext;
  }
  return decision;
}

}  // namespace orc

#endif  // ORC_CORE_SUBTITLE_EMBED_POLICY_H
