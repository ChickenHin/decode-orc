/*
 * File:        teletext_sink_stage_deps_interface.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Interface for TeletextSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_STAGE_DEPS_INTERFACE_H
#define ORC_TELETEXT_SINK_STAGE_DEPS_INTERFACE_H

#include <orc/stage/observation/observation_context_interface.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace orc {

/**
 * @brief Options for a T42 export run.
 *
 * Field lines are 0-based and identical in both fields (the stage converts
 * from the 1-based UI parameters). The default window 5-21 covers broadcast
 * lines 6-22 / 318-335 (ETSI EN 300 706 §4.1).
 */
struct TeletextSinkOptions {
  std::string output_path;
  int32_t first_field_line{5};
  int32_t last_field_line{21};
  // Emit 42 zero bytes for every candidate line with no data so packet
  // position maps 1:1 to (frame, field, line) — the vhs-decode convention.
  bool keep_empty_packets{false};
  // Accept framing codes with one bit error (TeletextSlicerOptions).
  bool tolerant_framing{false};
  // Drop packets whose MRAG fails Hamming 8/4 correction
  // (TeletextSlicerOptions).
  bool require_valid_mrag{true};
  // Combine repeated transmissions of each page row and write the combined
  // form ("squashing", see orc/support/teletext_row_squasher.h). Costs a
  // second pass over the recovered packets, held in memory (~50 bytes each).
  bool squash_repeated_rows{true};
  // Decode the subtitle page alongside the T42 export and write SubRip cues
  // next to the packet stream (design §6.1).
  bool export_subtitles{false};
  // Watched subtitle page in the conventional magazine + two-hex-digit form
  // (validated by the stage via TeletextPageDecoder::parse_page_number).
  std::string subtitle_page{"888"};
};

struct TeletextSinkResult {
  bool success{false};
  std::string message;
  // Path actually written (with the .t42 extension applied).
  std::string output_path;
  uint64_t packets_written{0};
  uint64_t fields_with_data{0};
  // Row packets whose bytes were changed by squashing (0 when disabled, or
  // when every row was only ever transmitted once).
  uint64_t packets_corrected{0};
  // Subtitle export results (export_subtitles only).
  std::string subtitle_path;
  uint64_t subtitle_cues_written{0};
};

class ITeletextSinkStageDeps {
 public:
  virtual ~ITeletextSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  virtual TeletextSinkResult export_t42(
      const VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      const TeletextSinkOptions& options) = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_STAGE_DEPS_INTERFACE_H
