/*
 * File:        fake_audio_stream_reader.h
 * Module:      orc-tests/gui/unit
 * Purpose:     Cadence-accurate IAudioStreamReader stand-in for playback tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <audio_stream_reader.h>
#include <orc/stage/audio/audio_channel_pair.h>  // audio_pair_offset
#include <orc/stage/common_types.h>              // VideoSystem

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace orc::gui::test {

/**
 * @brief In-memory reader following the real SMPTE 272M-1994 §14.3 cadence.
 *
 * Uses the SDK's own audio_pair_offset()/audio_pairs_in_frame(), so the
 * NTSC 1602/1601 sequence a controller test exercises is the pipeline's, not a
 * test-local approximation. Every sample carries its absolute stereo-pair
 * stream position, which lets a test assert that what reached the device is
 * contiguous from the frame playback started at.
 */
class FakeAudioStreamReader final : public orc::presenters::IAudioStreamReader {
 public:
  FakeAudioStreamReader(orc::FrameIDRange range, orc::VideoSystem system)
      : range_(range), system_(system) {}

  void prime(
      const orc::presenters::AudioPrimeProgressCallback& progress) override {
    ++prime_calls_;
    if (progress) {
      progress(1, 1, "primed");
    }
  }

  std::vector<float> readFrames(orc::FrameID first_frame,
                                uint64_t frame_count) override {
    read_requests_.emplace_back(first_frame, frame_count);

    std::vector<float> out;
    if (frame_count == 0 || range_.empty()) {
      return out;
    }
    const orc::FrameID first = std::max(first_frame, range_.first);
    if (first > range_.last || first_frame + frame_count <= first) {
      return out;
    }
    const orc::FrameID last =
        std::min(first_frame + frame_count - 1, range_.last);

    const uint64_t begin = orc::audio_pair_offset(first, system_);
    const uint64_t end = orc::audio_pair_offset(last + 1, system_);
    out.reserve((end - begin) * 2);
    for (uint64_t position = begin; position < end; ++position) {
      out.push_back(static_cast<float>(position));  // Left
      out.push_back(static_cast<float>(position));  // Right
    }
    return out;
  }

  uint64_t frameForPairPosition(uint64_t pair_position) const override {
    if (range_.empty()) {
      return 0;
    }
    // Estimate then correct, exactly as the presenter's reader does, so the
    // NTSC cadence stays exact at every position.
    uint64_t frame = 0;
    while (orc::audio_pair_offset(frame + 1, system_) <= pair_position) {
      ++frame;
    }
    return frame;
  }

  uint64_t pairPositionForFrame(orc::FrameID frame) const override {
    return orc::audio_pair_offset(frame, system_);
  }

  orc::FrameIDRange frameRange() const override { return range_; }

  // ---- Observations -------------------------------------------------------

  int primeCalls() const { return prime_calls_; }
  const std::vector<std::pair<orc::FrameID, uint64_t>>& readRequests() const {
    return read_requests_;
  }
  void clearReadRequests() { read_requests_.clear(); }

 private:
  orc::FrameIDRange range_;
  orc::VideoSystem system_;
  int prime_calls_ = 0;
  std::vector<std::pair<orc::FrameID, uint64_t>> read_requests_;
};

}  // namespace orc::gui::test
