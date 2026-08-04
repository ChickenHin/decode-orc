/*
 * File:        vbi_frame_sequence.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Field order, dropped-frame policy and signal state of a run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_SEQUENCE_H
#define ORC_VBI_FRAME_SEQUENCE_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {

// What to do about frames the source dropped (design §6.3).
enum class VBIDropPolicy {
  // Emit only the frames present.  Output frame numbering no longer matches
  // the source's own, so the counter of every frame is recorded for the
  // mapping to stay recoverable.
  kPreserve,

  // Insert synthesised blank frames so output frame n stays aligned with
  // source frame n, preserving both the timeline and the colour sequence at
  // the cost of inventing frames.
  kPad,
};

// The signal state the output declares (design §2.4).
//
// Claiming a locked state asserts that subcarrier phase is stable and known
// and that frame boundaries are preserved with no skipped, duplicated or
// shifted frames.  It is therefore a consequence of the run, never a user
// flag: a run with no synthesised burst, or one that dropped frames from the
// timeline, cannot make either claim.
enum class VBISignalState {
  kStandardTbcLocked,
  kStandardTbcUnlocked,
};

// The CVBS metadata spelling of a signal state.
std::string to_string(VBISignalState state);

// A break in the source's frame counter.
struct VBIFrameDiscontinuity {
  // Stored frame at which the break was observed: the first frame after the
  // gap, not the last one before it.
  uint64_t source_frame_index = 0;

  // Output frame the observed frame became.
  uint64_t output_frame_index = 0;

  uint32_t previous_counter = 0;
  uint32_t counter = 0;

  // Frames the counter says are missing.  Zero for a counter that repeated or
  // ran backwards, which is a break the timeline cannot be rebuilt from.
  uint64_t missing_frames = 0;

  // True when the policy filled the gap with synthesised frames.
  bool padded = false;
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

// Policy of a synthesis run over the source's frame sequence.
struct VBIFrameSequenceConfig {
  VBIDropPolicy drops = VBIDropPolicy::kPreserve;

  // Whether the run synthesises a coherent colour burst.  A precondition of
  // the locked signal state.
  bool burst_synthesised = true;

  // Largest gap kPad will fill.  A counter that jumps by millions is a
  // corrupt trailer rather than a real drop, and inventing the frames it asks
  // for would exhaust memory before anyone saw the problem.
  uint64_t maximum_pad_frames = 4096;
};

// The output frame sequence a run produces, and what it says about the
// source's integrity.
//
// Stored frames are observed in order and turned into a plan per output frame.
// Nothing is papered over: every counter discontinuity is recorded whichever
// policy is in force, and the signal state falls out of what was observed
// (design §6.3).
//
// Not thread-safe: observation mutates the timeline.  Const queries are safe
// to call concurrently once observation is complete.
class VBIFrameTimeline {
 public:
  VBIFrameTimeline() = default;

  VBIFrameTimeline(VBISourceFormat format, VBIFrameSequenceConfig config);

  const VBIFrameSequenceConfig& config() const { return config_; }

  // True when the source format carries a frame counter, and so when drops are
  // detectable at all.  For every other format the absence of reported drops
  // means nothing, which the summary says in as many words (design §6.3).
  bool counter_available() const;

  // Record one stored frame.  Frames must be observed in stored order.
  //
  // Returns false with an error message for a frame observed out of order, or
  // for a counter gap too large for the padding policy to be a sensible
  // reading of it.
  bool observe_frame(uint64_t source_frame_index,
                     std::optional<uint32_t> frame_counter,
                     std::string& error_message);

  uint64_t output_frame_count() const {
    return static_cast<uint64_t>(frames_.size());
  }

  const std::vector<VBIOutputFramePlan>& frames() const { return frames_; }

  // What an output frame is made from.  Returns false for an index beyond the
  // frames observed so far.
  bool frame_plan(uint64_t output_frame_index,
                  VBIOutputFramePlan& out_plan) const;

  const std::vector<VBIFrameDiscontinuity>& discontinuities() const {
    return discontinuities_;
  }

  uint64_t padded_frame_count() const { return padded_frame_count_; }

  VBISignalState signal_state() const;

  // Television field the first stored field of each frame carries.  A driver
  // convention rather than recorded information, so it is configuration
  // (design §6.1).
  uint32_t first_tv_field() const;

  // The field-order assumption, in the form the capture notes record it.
  std::string field_order_note() const;

  // Human-readable account of the sequence: field order, what the counter
  // showed, what the policy did about it, and the resulting signal state.
  std::string summary() const;

 private:
  VBISourceFormat format_{};
  VBIFrameSequenceConfig config_{};

  std::vector<VBIOutputFramePlan> frames_;
  std::vector<VBIFrameDiscontinuity> discontinuities_;

  bool has_previous_ = false;
  uint64_t previous_source_frame_index_ = 0;
  std::optional<uint32_t> previous_counter_;

  uint64_t padded_frame_count_ = 0;

  // True once a break was seen that the timeline could not be rebuilt from,
  // which is what forces the unlocked state.
  bool timeline_broken_ = false;
};

}  // namespace orc

#endif  // ORC_VBI_FRAME_SEQUENCE_H
