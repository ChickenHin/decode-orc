/*
 * File:        preview_audio_chase.h
 * Module:      orc-gui
 * Purpose:     Pure clock arithmetic for audio-mastered preview playback
 *              (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // PreviewOutputType

#include <cstdint>

namespace orc::gui::preview_audio {

// Pipeline audio channel pairs are stereo (SMPTE 272M-1994 §1.2).
inline constexpr uint32_t kAudioChannels = 2;

// Cap on how far ahead of the device the feeder reads. The device buffer is
// the effective lead: the feeder shares the GUI thread, so a stall that delays
// a feed tick delays the reads with it and only audio already handed to the
// device keeps playing. The cap simply stops the feeder reading unboundedly
// ahead of a device that reports a very deep buffer.
inline constexpr uint32_t kFeedLeadMs = 500;

// Feed timer period. Short relative to the device buffer, so several ticks
// pass before a queue that stops being topped up could run down.
inline constexpr uint32_t kFeedIntervalMs = 50;

// Video frames pulled from the reader per top-up. At 1920 pairs/frame (PAL)
// this is 40 ms of audio per read — small enough that a read never blocks the
// caller noticeably, large enough to keep per-frame overhead irrelevant.
inline constexpr uint64_t kFeedChunkFrames = 4;

/**
 * @brief Stereo pairs a device has consumed after playing for @p played_us.
 *
 * Truncating division: the result is the last pair the device has definitely
 * finished, which is what the video chase should not run ahead of.
 */
constexpr uint64_t stereoPairsPlayed(uint64_t played_us,
                                     uint32_t sample_rate_hz) {
  return played_us * sample_rate_hz / 1000000ULL;
}

/// Playing time occupied by @p stereo_pairs at @p sample_rate_hz.
constexpr uint64_t microsecondsForStereoPairs(uint64_t stereo_pairs,
                                              uint32_t sample_rate_hz) {
  return sample_rate_hz == 0 ? 0 : stereo_pairs * 1000000ULL / sample_rate_hz;
}

/// Stereo pairs making up @p milliseconds of audio at @p sample_rate_hz.
constexpr uint64_t stereoPairsForMilliseconds(uint64_t milliseconds,
                                              uint32_t sample_rate_hz) {
  return milliseconds * sample_rate_hz / 1000ULL;
}

/**
 * @brief Preview item index showing video frame @p frame.
 *
 * @param items_per_frame 1 for a frame-indexed output, 2 for a field-indexed
 *                        one (audio always maps to whole frames, so a field
 *                        output advances two indices per frame period).
 *
 * Field-indexed outputs land on the first field of the frame: the audio for a
 * frame starts with it, and stepping to the second field would put the chase
 * half a frame ahead of the samples that have actually played.
 */
constexpr uint64_t previewIndexForFrame(uint64_t frame,
                                        uint32_t items_per_frame) {
  return frame * (items_per_frame == 0 ? 1 : items_per_frame);
}

/// Video frame shown at preview item index @p index. Inverse of the above.
constexpr uint64_t frameForPreviewIndex(uint64_t index,
                                        uint32_t items_per_frame) {
  return index / (items_per_frame == 0 ? 1 : items_per_frame);
}

/**
 * @brief Preview items making up one video frame at output @p type.
 *
 * Field-indexed outputs step a field per index, so a frame spans two of them;
 * every other output steps a whole frame. The classification matches the one
 * the preview navigation itself uses when converting positions between output
 * types (MainWindow::onPreviewModeChanged).
 */
constexpr uint32_t itemsPerFrameForOutputType(orc::PreviewOutputType type) {
  switch (type) {
    case orc::PreviewOutputType::Frame_Field1:
    case orc::PreviewOutputType::Frame_Field2:
    case orc::PreviewOutputType::Luma:
      return 2;
    default:
      return 1;
  }
}

}  // namespace orc::gui::preview_audio
