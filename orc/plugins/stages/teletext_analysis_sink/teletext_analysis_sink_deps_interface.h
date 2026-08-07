/*
 * File:        teletext_analysis_sink_deps_interface.h
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Interface for TeletextAnalysisSinkStage dependencies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_H
#define ORC_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_H

#include <orc/stage/analysis_sink_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/teletext_slicer.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace orc {

/**
 * @brief Options for one teletext analysis run.
 *
 * Field lines are 0-based and identical in both fields (the stage converts
 * from the 1-based UI parameters). The default window 5-21 covers broadcast
 * lines 6-22 / 318-335 (ETSI EN 300 706 §4.1); a 525-line service uses 9-20
 * (ITU-R BT.653 §2).
 */
struct TeletextAnalysisSinkOptions {
  std::string output_path;
  int32_t first_field_line{5};
  int32_t last_field_line{21};
  // Emit a whole zero packet for every candidate line with no data so packet
  // position maps 1:1 to (frame, field, line) — the vhs-decode convention.
  bool keep_empty_packets{false};
  // Accept framing codes with one bit error (TeletextSlicerOptions).
  bool tolerant_framing{false};
  // Drop packets whose MRAG fails Hamming 8/4 correction
  // (TeletextSlicerOptions).
  bool require_valid_mrag{true};
  // Restore odd parity on damaged display bytes by flipping the bit the MLSE
  // detector was least sure of (TeletextSlicerOptions::parity_repair).
  bool parity_repair{true};
  // Bit detector (TeletextSlicerOptions).
  TeletextDetector detector{TeletextDetector::kAuto};
  // Combine repeated transmissions of each page row and write the combined
  // form ("squashing", see orc/support/teletext_row_squasher.h). Costs a
  // second pass over the recovered packets, held in memory (~50 bytes each).
  bool squash_repeated_rows{true};
  // Decode the subtitle page alongside the packet export and write SubRip cues
  // next to the packet stream. 625-line services only: the cue timing derives
  // from the 50 fields/s of ITU-R BT.1700 Annex 1 Part B Table 1 item 2.
  bool export_subtitles{false};
  // Watched subtitle page in the conventional magazine + two-hex-digit form
  // (validated by the stage via TeletextPageDecoder::parse_page_number).
  std::string subtitle_page{"888"};
  // Write the run's diagnostic report to <output_path>.txt as well as logging
  // it. The report is always built; this only decides whether it is kept
  // somewhere a reader can go back to.
  bool write_report{false};
};

struct TeletextAnalysisSinkResult {
  bool success{false};
  std::string message;
  // Path actually written, with the service's extension applied: .t42 for the
  // 42-byte 625-line packet stream, .t34 for the 34-byte 525-line one.
  std::string output_path;
  uint64_t packets_written{0};
  uint64_t fields_with_data{0};
  // Row packets whose bytes were changed by squashing (0 when disabled, or
  // when every row was only ever transmitted once).
  uint64_t packets_corrected{0};
  // Display bytes whose parity was restored by flipping the detector's
  // least-confident bit (0 unless parity_repair is set).
  uint64_t bytes_repaired{0};
  // Subtitle export results (export_subtitles only).
  std::string subtitle_path;
  uint64_t subtitle_cues_written{0};
  // The run's headline, for a caller that wants the result without the
  // report: display characters written, and how many of those are known
  // damaged because they fail the odd parity of ETSI EN 300 706 §8.1. A floor
  // rather than an exact count — a byte damaged in two bits passes parity —
  // and silent about rows that never arrived. Zero characters means no display
  // row was written.
  uint64_t characters_written{0};
  uint64_t characters_damaged{0};
  // Human-readable diagnostic report of the run: what was exported, how the
  // recovery went (orc/support/teletext_recovery_stats.h) and what combining
  // repeated rows changed (teletext_squash_stats.h). Logged by the stage at
  // debug level, and written to report_path when write_report is set.
  std::string report;
  // Path the report was written to, empty when write_report is off or the
  // write failed (which never fails the export — the packet stream is the
  // product).
  std::string report_path;
  // The viewer's half of the run: every page the range carried, plus the
  // aggregate recovery figures. Populated for a cancelled run too — what it
  // got to is exactly what a cancelled run leaves a reader asking about.
  TeletextAnalysisDataset dataset;
};

class ITeletextAnalysisSinkStageDeps {
 public:
  virtual ~ITeletextAnalysisSinkStageDeps() = default;

  virtual void init(TriggerProgressCallback progress_callback,
                    std::atomic<bool>* cancel_requested) = 0;

  // One linear pass over the whole frame range: recovers the packets, writes
  // the stream (and, optionally, the subtitle document and the report), and
  // builds the page catalogue the stage tool displays.
  virtual TeletextAnalysisSinkResult analyse(
      const VideoFrameRepresentation* representation,
      const TeletextAnalysisSinkOptions& options) = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_H
