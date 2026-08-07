/*
 * File:        teletext_frame_slicer.h
 * Module:      orc-stage-plugin-teletext_analysis_sink
 * Purpose:     Per-frame WST teletext line recovery from a video frame
 *              representation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_FRAME_SLICER_H
#define ORC_TELETEXT_FRAME_SLICER_H

#include <orc/stage/common_types.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/teletext_slicer.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

// Candidate VBI window as 0-based field lines, identical in both fields of a
// frame.
//
// 625 lines — ETSI EN 300 706 §4.1: broadcast lines 6 to 22 (field 1) and 318
// to 335 (field 2) may carry teletext; in the frame-flat buffer those are
// 0-based field lines 5-21 of each field (field 2 offset by the field-1 line
// count).
constexpr int32_t kTeletextFirstFieldLine625 = 5;
constexpr int32_t kTeletextLastFieldLine625 = 21;

// 525 lines — ITU-R BT.653 §2: broadcast lines 10 to 21 (field 1) and 273 to
// 284 (field 2), which are 0-based field lines 9-20 of each field. The same
// list the VBI source stage places to (vbi_line_mapping.cpp), so a capture
// that stage wrote and a TBC of the same broadcast are read on one window.
//
// Field line 20 is broadcast line 21, which on a captioned NTSC recording
// carries EIA-608 rather than teletext. The two services are alternatives, not
// neighbours, and a caption line has no framing code, so it is examined and
// rejected rather than excluded here.
constexpr int32_t kTeletextFirstFieldLine525 = 9;
constexpr int32_t kTeletextLastFieldLine525 = 20;

/// Slicer tuning the stage's parameters map onto.
struct TeletextFrameSlicerOptions {
  // Bit detector (TeletextSlicerOptions::detector).
  TeletextDetector detector = TeletextDetector::kAuto;
  // Restore odd parity on damaged display bytes by flipping the bit the MLSE
  // detector was least sure of (TeletextSlicerOptions::parity_repair).
  bool parity_repair = true;
  // Accept framing codes with one bit error (TeletextSlicerOptions).
  bool tolerant_framing = false;
  // Drop packets whose MRAG fails Hamming 8/4 correction
  // (TeletextSlicerOptions).
  bool require_valid_mrag = true;
  // Candidate window override as 0-based field lines, identical in both
  // fields. Unset means the television system's own window (above), which is
  // what a caller with no opinion wants.
  std::optional<int32_t> first_field_line;
  std::optional<int32_t> last_field_line;
};

/// One candidate line of one field, sliced.
struct TeletextFrameLineResult {
  /// 0-based field line the result came from
  int32_t field_line = 0;
  /// 0-based line in the frame-flat buffer the samples were read from
  size_t flat_line = 0;
  TeletextLineResult sliced;
};

/**
 * @brief Recovers the teletext packets of one field of a video frame
 *
 * Owns one TeletextSlicer per television system — each carries its own sample
 * rate, bit rate, packet length and data-timing window, and all are cheap
 * enough to build once here rather than per frame — and selects between them
 * from the representation's video parameters.
 *
 * Only the ITU-R BT.653 System B services are recovered: 625 lines (ETSI EN
 * 300 706, PAL) and 525 lines (Table 1b, NTSC and PAL_M). NABTS (System C)
 * shares the 525 lines and the bit rate but not the framing code, so its lines
 * are seen and rejected rather than decoded.
 *
 * Thread safety: slice_field() is const and the class holds no mutable state;
 * a single instance may be used concurrently from multiple threads.
 */
class TeletextFrameSlicer {
 public:
  explicit TeletextFrameSlicer(TeletextFrameSlicerOptions options = {});

  /// Whether |system| carries a WST service this can recover.
  static bool applies_to(VideoSystem system);

  /// Which teletext system a video system carries, and where in the field to
  /// look for it.
  struct SystemProfile {
    TeletextSystem teletext_system = TeletextSystem::kWst625;
    int32_t first_field_line = kTeletextFirstFieldLine625;
    int32_t last_field_line = kTeletextLastFieldLine625;
  };

  /// Profile of |system| before any option override is applied.
  static SystemProfile profile_for(VideoSystem system);

  /// Profile actually probed, i.e. profile_for() with this instance's window
  /// override applied.
  SystemProfile effective_profile(VideoSystem system) const;

  /**
   * @brief Slice every candidate line of one field
   *
   * @param representation Source of the video parameters and the line samples
   * @param frame_id       Frame to read
   * @param field_idx      0 for field 1, 1 for field 2
   * @param results        Receives one entry per candidate line that yielded
   *                       samples, in ascending line order — rejected lines
   *                       included, so a caller accumulating recovery
   *                       diagnostics sees every line that was looked at.
   *                       Cleared first; reused across calls so a full-range
   *                       pass allocates once.
   *
   * Does nothing when the representation has no video parameters, carries a
   * system with no WST service, or holds no samples for the lines in question.
   */
  void slice_field(const VideoFrameRepresentation& representation,
                   FrameID frame_id, size_t field_idx,
                   std::vector<TeletextFrameLineResult>& results) const;

 private:
  // Chooses the slicer built for |system|.
  const TeletextSlicer& slicer_for(VideoSystem system) const;

  TeletextFrameSlicerOptions options_;

  // One slicer per television system; see the class comment.
  TeletextSlicer slicer_pal_;
  TeletextSlicer slicer_ntsc_;
  TeletextSlicer slicer_palm_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_FRAME_SLICER_H
