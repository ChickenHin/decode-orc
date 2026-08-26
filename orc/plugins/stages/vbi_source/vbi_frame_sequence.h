/*
 * File:        vbi_frame_sequence.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Dropped-frame policy and per-frame plan of a run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_SEQUENCE_H
#define ORC_VBI_FRAME_SEQUENCE_H

#include <cstdint>
#include <optional>

namespace orc {

// What to do about frames the source dropped (design §6.3).
enum class VBIDropPolicy {
  // Emit only the frames present.  Output frame numbering no longer matches
  // the source's own.
  kPreserve,

  // Insert blank frames so output frame n stays aligned with source frame n,
  // preserving the timeline at the cost of inventing frames.
  kPad,
};

// What one output frame is made from.
struct VBIOutputFramePlan {
  uint64_t output_frame_index = 0;

  // Stored frame the output frame carries.  Meaningless when padding is set:
  // a padded frame has no source, and it holds the index of the frame that
  // follows it so the gap's position stays visible.
  uint64_t source_frame_index = 0;

  // The source's own frame number, where the format provides one.
  std::optional<uint32_t> frame_counter;

  bool padding = false;
};

// Policy of a run over the source's frame sequence.
struct VBIFrameSequenceConfig {
  VBIDropPolicy drops = VBIDropPolicy::kPreserve;

  // Largest gap kPad will fill.  A counter that jumps by millions is a corrupt
  // trailer rather than a real drop, and inventing the frames it asks for would
  // exhaust memory before anyone saw the problem.
  uint64_t maximum_pad_frames = 4096;
};

}  // namespace orc

#endif  // ORC_VBI_FRAME_SEQUENCE_H
