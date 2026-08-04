/*
 * File:        vbi_frame_sequence.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Field order, dropped-frame policy and signal state of a run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_sequence.h"

#include "vbi_line_mapping.h"

namespace orc {

namespace {

// Frames between two counter readings, on the counter's own modular
// arithmetic.  The driver writes an unsigned 32-bit sequence number, so a
// capture long enough to wrap it advances by one across the wrap like anywhere
// else.
uint32_t counter_step(uint32_t previous, uint32_t current) {
  return current - previous;
}

// Largest step that is a gap rather than a counter running backwards.  A step
// in the upper half of the range is a decrease read modularly, which no real
// drop produces.
constexpr uint32_t kLargestForwardStep = 0x8000'0000u;

}  // namespace

std::string to_string(VBISignalState state) {
  return (state == VBISignalState::kStandardTbcLocked)
             ? "STANDARD_TBC_LOCKED"
             : "STANDARD_TBC_UNLOCKED";
}

VBIFrameTimeline::VBIFrameTimeline(VBISourceFormat format,
                                   VBIFrameSequenceConfig config)
    : format_(format), config_(config) {}

bool VBIFrameTimeline::counter_available() const {
  return format_.frame_trailer_is_counter && format_.frame_trailer_bytes > 0;
}

bool VBIFrameTimeline::observe_frame(uint64_t source_frame_index,
                                     std::optional<uint32_t> frame_counter,
                                     std::string& error_message) {
  if (has_previous_ && source_frame_index <= previous_source_frame_index_) {
    error_message =
        "Stored frame " + std::to_string(source_frame_index) +
        " was observed after frame " +
        std::to_string(previous_source_frame_index_) +
        "; frames must be observed in stored order for the output timeline to "
        "follow the capture.";
    return false;
  }

  const bool have_step = has_previous_ && previous_counter_.has_value() &&
                         frame_counter.has_value();
  const uint32_t step =
      have_step ? counter_step(*previous_counter_, *frame_counter) : 1u;

  if (have_step && step != 1u) {
    const bool forward_gap = (step > 1u) && (step < kLargestForwardStep);
    const uint64_t missing =
        forward_gap ? static_cast<uint64_t>(step) - 1u : 0u;

    if (config_.drops == VBIDropPolicy::kPad && forward_gap &&
        missing > config_.maximum_pad_frames) {
      error_message =
          "The frame counter jumps by " + std::to_string(step) +
          " at stored frame " + std::to_string(source_frame_index) +
          ", which is more than the " +
          std::to_string(config_.maximum_pad_frames) +
          " frames the padding policy will synthesise; the counter is more "
          "likely corrupt than the capture that many frames short.";
      return false;
    }

    VBIFrameDiscontinuity discontinuity;
    discontinuity.source_frame_index = source_frame_index;
    discontinuity.previous_counter = *previous_counter_;
    discontinuity.counter = *frame_counter;
    discontinuity.missing_frames = missing;

    if (config_.drops == VBIDropPolicy::kPad && forward_gap) {
      // Keep output frame n aligned with source frame n by synthesising the
      // frames the counter says are missing.  The timeline and the colour
      // sequence survive; the invented frames are flagged so nothing
      // downstream mistakes them for recovered data.
      discontinuity.padded = true;
      for (uint64_t index = 0; index < missing; ++index) {
        VBIOutputFramePlan pad;
        pad.output_frame_index = static_cast<uint64_t>(frames_.size());
        pad.source_frame_index = source_frame_index;
        pad.padding = true;
        frames_.push_back(pad);
        ++padded_frame_count_;
      }
    } else {
      // Either the policy keeps only the frames present, or the counter did
      // something no padding can undo.  Output frame numbering no longer
      // matches the source's, so frame-boundary integrity is lost.
      timeline_broken_ = true;
    }

    discontinuity.output_frame_index = static_cast<uint64_t>(frames_.size());
    discontinuities_.push_back(discontinuity);
  }

  VBIOutputFramePlan plan;
  plan.output_frame_index = static_cast<uint64_t>(frames_.size());
  plan.source_frame_index = source_frame_index;
  plan.frame_counter = frame_counter;
  frames_.push_back(plan);

  has_previous_ = true;
  previous_source_frame_index_ = source_frame_index;
  previous_counter_ = frame_counter;
  return true;
}

bool VBIFrameTimeline::frame_plan(uint64_t output_frame_index,
                                  VBIOutputFramePlan& out_plan) const {
  if (output_frame_index >= static_cast<uint64_t>(frames_.size())) {
    out_plan = VBIOutputFramePlan();
    return false;
  }
  out_plan = frames_[static_cast<size_t>(output_frame_index)];
  return true;
}

VBISignalState VBIFrameTimeline::signal_state() const {
  // Subcarrier phase is only stable and known when the burst was synthesised,
  // and frame boundaries are only preserved when nothing broke the timeline.
  if (!config_.burst_synthesised || timeline_broken_) {
    return VBISignalState::kStandardTbcUnlocked;
  }
  return VBISignalState::kStandardTbcLocked;
}

uint32_t VBIFrameTimeline::first_tv_field() const {
  return vbi_tv_field_for_stored_field(format_, 0);
}

std::string VBIFrameTimeline::field_order_note() const {
  return "First stored field of each frame taken as television field " +
         std::to_string(first_tv_field()) +
         " (assumed from the source format, not recorded by the capture)";
}

std::string VBIFrameTimeline::summary() const {
  std::string text = field_order_note() + ". ";

  text += "Output frames: " + std::to_string(frames_.size());
  if (padded_frame_count_ > 0) {
    text += " (" + std::to_string(padded_frame_count_) +
            " synthesised to fill "
            "counter gaps)";
  }
  text += ". ";

  if (!counter_available()) {
    text +=
        "The source format carries no frame counter, so dropped frames cannot "
        "be detected at all; no drops were reported because none could be. ";
  } else if (discontinuities_.empty()) {
    text += "Frame counter continuous throughout. ";
  } else {
    text += "Frame counter discontinuities: " +
            std::to_string(discontinuities_.size());
    text += (config_.drops == VBIDropPolicy::kPad)
                ? ", gaps padded to preserve the timeline. "
                : ", dropped frames not emitted. ";
  }

  text += "Signal state: " + to_string(signal_state()) + ".";
  return text;
}

}  // namespace orc
