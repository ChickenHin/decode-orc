/*
 * File:        vbi_frame_index.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Resolves output frames to stored frames from the frame counter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_index.h"

#include <algorithm>

#include "vbi_line_mapping.h"

namespace orc {

bool VBIFrameIndex::build(const VBISourceFormat& format,
                          const VBIFrameSequenceConfig& config,
                          uint64_t stored_frame_count,
                          const IVBIFrameCounterSource& counters,
                          VBIFrameIndex& out_index,
                          std::string& error_message) {
  out_index = VBIFrameIndex();
  out_index.format_ = format;
  out_index.config_ = config;
  out_index.counters_ = &counters;
  out_index.stored_frame_count_ = stored_frame_count;
  out_index.output_frame_count_ = stored_frame_count;

  if (stored_frame_count == 0 || !out_index.counter_available()) {
    return true;
  }

  std::optional<uint32_t> first;
  std::optional<uint32_t> last;
  if (!counters.frame_counter(0, first, error_message) ||
      !counters.frame_counter(stored_frame_count - 1u, last, error_message)) {
    return false;
  }
  if (!first.has_value() || !last.has_value()) {
    // The format declares a counter but the capture did not produce one; the
    // source is treated as one that cannot report drops.
    return true;
  }

  out_index.first_counter_ = *first;
  out_index.have_counters_ = true;
  out_index.counter_cache_[0] = *first;
  out_index.counter_cache_[stored_frame_count - 1u] = *last;

  const uint64_t span = out_index.counter_offset(*last);
  const uint64_t stored_span = stored_frame_count - 1u;

  if (span < stored_span) {
    // The counter advanced by fewer frames than the capture stores, so it
    // repeated or ran backwards somewhere.  Nothing can rebuild the timeline
    // from that, and padding certainly cannot: the frames are emitted as
    // stored and the run is reported as unlocked.
    out_index.timeline_broken_ = true;
    return true;
  }

  out_index.dropped_frame_count_ = span - stored_span;
  if (out_index.dropped_frame_count_ == 0) {
    return true;
  }

  if (config.drops == VBIDropPolicy::kPad) {
    if (out_index.dropped_frame_count_ > config.maximum_pad_frames) {
      error_message =
          "The frame counter accounts for " +
          std::to_string(out_index.dropped_frame_count_) +
          " frames missing from the capture, which is more than the " +
          std::to_string(config.maximum_pad_frames) +
          " frames the padding policy will synthesise; the counter is more "
          "likely corrupt than the capture that many frames short.";
      return false;
    }
    // Keep output frame n aligned with source frame n by synthesising the
    // frames the counter says are missing.  The timeline and the colour
    // sequence survive; the invented frames are flagged so nothing downstream
    // mistakes them for recovered data (design §6.3).
    out_index.output_frame_count_ =
        stored_frame_count + out_index.dropped_frame_count_;
    out_index.identity_mapping_ = false;
    return true;
  }

  // Only the frames present are emitted, so output frame numbering no longer
  // matches the source's and frame-boundary integrity is lost.
  out_index.timeline_broken_ = true;
  return true;
}

bool VBIFrameIndex::counter_available() const {
  return format_.frame_trailer_is_counter && format_.frame_trailer_bytes > 0;
}

uint64_t VBIFrameIndex::counter_offset(uint32_t counter) const {
  // Modular difference: a capture long enough to wrap the driver's unsigned
  // 32-bit sequence number advances by one across the wrap like anywhere else.
  return static_cast<uint64_t>(static_cast<uint32_t>(counter - first_counter_));
}

VBISignalState VBIFrameIndex::signal_state() const {
  // Subcarrier phase is only stable and known when the burst was synthesised,
  // and frame boundaries are only preserved when nothing broke the timeline
  // (design §2.4).
  if (!config_.burst_synthesised || timeline_broken_) {
    return VBISignalState::kStandardTbcUnlocked;
  }
  return VBISignalState::kStandardTbcLocked;
}

uint32_t VBIFrameIndex::first_tv_field() const {
  return vbi_tv_field_for_stored_field(format_, 0);
}

std::string VBIFrameIndex::field_order_note() const {
  return "First stored field of each frame taken as television field " +
         std::to_string(first_tv_field()) +
         " (assumed from the source format, not recorded by the capture)";
}

std::string VBIFrameIndex::summary() const {
  std::string text = field_order_note() + ". ";

  text += "Output frames: " + std::to_string(output_frame_count_);
  if (output_frame_count_ > stored_frame_count_) {
    text += " (" + std::to_string(output_frame_count_ - stored_frame_count_) +
            " synthesised to fill counter gaps)";
  }
  text += ". ";

  if (!counter_available() || !have_counters_) {
    text +=
        "The source format carries no frame counter, so dropped frames cannot "
        "be detected at all; no drops were reported because none could be. ";
  } else if (dropped_frame_count_ == 0 && !timeline_broken_) {
    text += "Frame counter continuous throughout. ";
  } else if (timeline_broken_ && dropped_frame_count_ == 0) {
    text +=
        "The frame counter repeated or ran backwards, so the capture's own "
        "frame numbering cannot be rebuilt. ";
  } else {
    text += "Frame counter accounts for " +
            std::to_string(dropped_frame_count_) +
            " frame(s) missing from the capture";
    text += (config_.drops == VBIDropPolicy::kPad)
                ? ", gaps padded to preserve the timeline. "
                : ", dropped frames not emitted. ";
  }

  text += "Signal state: " + to_string(signal_state()) + ".";
  return text;
}

bool VBIFrameIndex::counter_at(uint64_t stored_frame_index,
                               uint32_t& out_counter,
                               std::string& error_message) const {
  const auto cached = counter_cache_.find(stored_frame_index);
  if (cached != counter_cache_.end()) {
    out_counter = cached->second;
    return true;
  }

  if (counters_ == nullptr) {
    error_message = "The frame index has no counter source to read.";
    return false;
  }

  std::optional<uint32_t> counter;
  if (!counters_->frame_counter(stored_frame_index, counter, error_message)) {
    return false;
  }
  if (!counter.has_value()) {
    error_message = "Stored frame " + std::to_string(stored_frame_index) +
                    " carries no frame counter, so the frames the capture "
                    "dropped cannot be located.";
    return false;
  }

  counter_cache_[stored_frame_index] = *counter;
  out_counter = *counter;
  return true;
}

bool VBIFrameIndex::frame_plan(uint64_t output_frame_index,
                               VBIOutputFramePlan& out_plan,
                               std::string& error_message) const {
  out_plan = VBIOutputFramePlan();

  if (output_frame_index >= output_frame_count_) {
    error_message = "Output frame " + std::to_string(output_frame_index) +
                    " is beyond the " + std::to_string(output_frame_count_) +
                    " frames this capture produces.";
    return false;
  }

  out_plan.output_frame_index = output_frame_index;

  if (identity_mapping_) {
    out_plan.source_frame_index = output_frame_index;
    return true;
  }

  // Padding is in force and the counter has gaps.  The counter advances by at
  // least one per stored frame, so the offset of stored frame s is at least s
  // and at most s + dropped: the frame carrying output frame m lies in
  // [m - dropped, m], and bisection over that range finds it.
  uint64_t low = (output_frame_index > dropped_frame_count_)
                     ? output_frame_index - dropped_frame_count_
                     : 0u;
  uint64_t high = std::min(output_frame_index, stored_frame_count_ - 1u);

  // Largest stored frame whose offset does not exceed the output frame.
  uint64_t candidate = low;
  while (low <= high) {
    const uint64_t middle = low + (high - low) / 2u;
    uint32_t counter = 0;
    if (!counter_at(middle, counter, error_message)) {
      return false;
    }
    if (counter_offset(counter) <= output_frame_index) {
      candidate = middle;
      low = middle + 1u;
    } else {
      if (middle == 0u) {
        break;
      }
      high = middle - 1u;
    }
  }

  uint32_t candidate_counter = 0;
  if (!counter_at(candidate, candidate_counter, error_message)) {
    return false;
  }

  if (counter_offset(candidate_counter) == output_frame_index) {
    out_plan.source_frame_index = candidate;
    out_plan.frame_counter = candidate_counter;
    return true;
  }

  // The output frame falls in a gap the capture never recorded.  A padded
  // frame has no source, so it holds the index of the frame that follows it
  // and the gap's position stays visible.
  out_plan.padding = true;
  out_plan.source_frame_index =
      std::min(candidate + 1u, stored_frame_count_ - 1u);
  return true;
}

}  // namespace orc
