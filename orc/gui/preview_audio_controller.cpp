/*
 * File:        preview_audio_controller.cpp
 * Module:      orc-gui
 * Purpose:     Audio-mastered playback session for the preview dialogue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_audio_controller.h"

#include <orc/stage/audio/audio_channel_pair.h>  // kAudioSampleRateHz

#include <QTimer>
#include <algorithm>
#include <utility>

#include "logging.h"
#include "preview_audio_chase.h"

namespace orc::gui {

using preview_audio::kAudioChannels;
using preview_audio::kFeedChunkFrames;
using preview_audio::kFeedIntervalMs;
using preview_audio::kFeedLeadMs;

PreviewAudioController::PreviewAudioController(QObject* parent)
    : QObject(parent),
      feed_timer_(new QTimer(this)),
      lead_target_pairs_(preview_audio::stereoPairsForMilliseconds(
          kFeedLeadMs, orc::kAudioSampleRateHz)) {
  feed_timer_->setInterval(static_cast<int>(kFeedIntervalMs));
  connect(feed_timer_, &QTimer::timeout, this, &PreviewAudioController::feed);
}

PreviewAudioController::~PreviewAudioController() { stop(); }

void PreviewAudioController::setAudioOutput(
    std::unique_ptr<IAudioOutput> output) {
  stop();
  output_ = std::move(output);
  applyVolume();
}

void PreviewAudioController::setReader(
    std::shared_ptr<orc::presenters::IAudioStreamReader> reader) {
  // A new reader means a new node, pair or DAG version: whatever was playing
  // came from a representation the dialogue has moved on from.
  stop();
  reader_ = std::move(reader);
  range_ = reader_ ? reader_->frameRange() : orc::FrameIDRange{1, 0};
  if (!range_.contains(start_frame_)) {
    start_frame_ = range_.empty() ? 0 : range_.first;
  }
}

void PreviewAudioController::setItemsPerFrame(uint32_t items_per_frame) {
  items_per_frame_ = items_per_frame == 0 ? 1 : items_per_frame;
}

bool PreviewAudioController::start(orc::FrameID first_frame) {
  stop();

  if (!output_ || !reader_) {
    return false;
  }
  if (range_.empty() || !range_.contains(first_frame)) {
    ORC_LOG_DEBUG(
        "PreviewAudioController: Frame {} is outside the audio range [{}, {}]",
        first_frame, range_.first, range_.last);
    return false;
  }

  if (!output_->start(orc::kAudioSampleRateHz, kAudioChannels)) {
    ORC_LOG_WARN("PreviewAudioController: Audio device refused to start");
    return false;
  }
  applyVolume();

  start_frame_ = first_frame;
  feed_cursor_ = first_frame;
  fed_pairs_ = 0;
  underrun_count_ = 0;
  pending_.clear();
  pending_offset_ = 0;
  state_ = State::kPlaying;

  // Fill the device before the first tick so playback starts immediately
  // rather than one feed interval later.
  feed();
  if (state_ == State::kPlaying) {
    feed_timer_->start();
  }
  return state_ == State::kPlaying;
}

void PreviewAudioController::stop() {
  feed_timer_->stop();
  if (output_) {
    output_->stop();
  }
  state_ = State::kStopped;
  fed_pairs_ = 0;
  pending_.clear();
  pending_offset_ = 0;
  feed_cursor_ = start_frame_;
}

void PreviewAudioController::seek(orc::FrameID frame) {
  const bool was_playing = isPlaying();
  stop();
  start_frame_ = frame;
  feed_cursor_ = frame;
  if (was_playing) {
    start(frame);
  }
}

orc::FrameID PreviewAudioController::targetFrame() const {
  if (state_ != State::kPlaying || !reader_) {
    return start_frame_;
  }
  const uint64_t position =
      reader_->pairPositionForFrame(start_frame_) + playedPairs();
  const orc::FrameID frame = reader_->frameForPairPosition(position);
  if (range_.empty()) {
    return start_frame_;
  }
  return std::clamp(frame, range_.first, range_.last);
}

uint64_t PreviewAudioController::targetPreviewIndex() const {
  return preview_audio::previewIndexForFrame(targetFrame(), items_per_frame_);
}

void PreviewAudioController::setVolume(double volume) {
  volume_ = std::clamp(volume, 0.0, 1.0);
  applyVolume();
}

void PreviewAudioController::setMuted(bool muted) {
  muted_ = muted;
  applyVolume();
}

void PreviewAudioController::feed() {
  if (state_ != State::kPlaying || !output_ || !reader_) {
    return;
  }

  // Sample starvation before topping up: once the device has been refilled
  // there is nothing left to tell that it had run dry since the last tick.
  const bool starved = fed_pairs_ > 0 && playedPairs() >= fed_pairs_;

  // Anything the device refused last time goes in first: the stream must stay
  // contiguous for the played-pairs → frame mapping to hold.
  writePending();

  while (pending_offset_ >= pending_.size() && !feedExhausted() &&
         bufferedPairs() < lead_target_pairs_) {
    pending_ = reader_->readFrames(feed_cursor_, kFeedChunkFrames);
    pending_offset_ = 0;
    feed_cursor_ = std::min(feed_cursor_ + kFeedChunkFrames, range_.last + 1);
    if (pending_.empty()) {
      // Nothing readable at the cursor; the loop still terminates because the
      // cursor advanced, and the drain check below closes out the session.
      continue;
    }
    writePending();
  }

  if (feedExhausted() && pendingPairs() == 0 && playedPairs() >= fed_pairs_) {
    ORC_LOG_DEBUG(
        "PreviewAudioController: Reached the end of the audio range after {} "
        "stereo pairs",
        fed_pairs_);
    finish();
    return;
  }

  if (starved) {
    // The device consumed everything queued while frames remain. The clock
    // stalls rather than drifts, so the video chase stalls with it, and the
    // refill above resumes the stream exactly where it left off — nothing to
    // correct, only something to report.
    ++underrun_count_;
    ORC_LOG_DEBUG(
        "PreviewAudioController: Audio underrun at stereo pair {} (frame {})",
        fed_pairs_, feed_cursor_);
    emit underrunDetected();
  }
}

uint64_t PreviewAudioController::playedPairs() const {
  if (!output_) {
    return 0;
  }
  const uint64_t played = preview_audio::stereoPairsPlayed(
      output_->playedMicroseconds(), orc::kAudioSampleRateHz);
  return std::min(played, fed_pairs_);
}

uint64_t PreviewAudioController::pendingPairs() const {
  if (pending_offset_ >= pending_.size()) {
    return 0;
  }
  return (pending_.size() - pending_offset_) / kAudioChannels;
}

uint64_t PreviewAudioController::bufferedPairs() const {
  return (fed_pairs_ - playedPairs()) + pendingPairs();
}

bool PreviewAudioController::feedExhausted() const {
  return range_.empty() || feed_cursor_ > range_.last;
}

void PreviewAudioController::writePending() {
  while (pending_offset_ < pending_.size()) {
    const size_t offered = static_cast<size_t>(pendingPairs());
    const size_t accepted =
        output_->write(pending_.data() + pending_offset_, offered);
    if (accepted == 0) {
      return;  // Device full; the tail waits for the next tick.
    }
    pending_offset_ += accepted * kAudioChannels;
    fed_pairs_ += accepted;
  }
  pending_.clear();
  pending_offset_ = 0;
}

void PreviewAudioController::applyVolume() {
  if (!output_) {
    return;
  }
  // Squared taper: a linear slider then tracks perceived loudness reasonably
  // well. Muting is amplitude only — the device keeps consuming, so the clock
  // and the video chase are unaffected.
  output_->setVolume(muted_ ? 0.0 : volume_ * volume_);
}

void PreviewAudioController::finish() {
  stop();
  emit finished();
}

}  // namespace orc::gui
