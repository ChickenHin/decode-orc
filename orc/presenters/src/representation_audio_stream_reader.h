/*
 * File:        representation_audio_stream_reader.h
 * Module:      orc-presenters
 * Purpose:     IAudioStreamReader backed by a resolved VideoFrameRepresentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // VideoSystem
#include <orc/stage/video_frame_representation.h>
#include <orc_audio_views.h>  // AudioPairView

#include <cstddef>
#include <memory>
#include <vector>

#include "audio_stream_reader.h"

namespace orc::presenters {

/**
 * @brief Serves one channel pair of a resolved representation as float stereo.
 *
 * Internal to the presenter layer: the GUI only ever sees IAudioStreamReader.
 * Constructed directly in unit tests against a mocked representation, which is
 * why it is a named class rather than a lambda inside the presenter.
 *
 * Holding the representation shared_ptr keeps it alive for the playback
 * session — the same thing an export sink does for the duration of a run. Frame
 * *sample* pointers are never retained: get_audio_samples() returns by value.
 *
 * Thread safety: see IAudioStreamReader. Construct on the render worker, then
 * read from one other thread.
 */
class RepresentationAudioStreamReader final : public IAudioStreamReader {
 public:
  /**
   * @param representation Resolved representation carrying the audio (non-null)
   * @param pair           Channel-pair index to serve
   * @param system         Video system that fixes the SMPTE 272M cadence
   */
  RepresentationAudioStreamReader(
      std::shared_ptr<const orc::VideoFrameRepresentation> representation,
      size_t pair, orc::VideoSystem system);

  void prime(const AudioPrimeProgressCallback& progress) override;

  std::vector<float> readFrames(orc::FrameID first_frame,
                                uint64_t frame_count) override;

  uint64_t frameForPairPosition(uint64_t pair_position) const override;

  uint64_t pairPositionForFrame(orc::FrameID frame) const override;

  orc::FrameIDRange frameRange() const override;

 private:
  std::shared_ptr<const orc::VideoFrameRepresentation> representation_;
  size_t pair_ = 0;
  orc::VideoSystem system_ = orc::VideoSystem::Unknown;
  orc::FrameIDRange range_{};
};

/**
 * @brief Enumerate a representation's audio channel pairs as view types.
 *
 * Returns an empty list when the representation carries no pairs, or when its
 * video system leaves the SMPTE 272M-1994 §14.3 cadence undefined — such audio
 * cannot be addressed by frame, so it is presented as absent.
 */
std::vector<orc::AudioPairView> enumerate_audio_channel_pairs(
    const orc::VideoFrameRepresentation& representation);

/**
 * @brief Wrap one channel pair of a representation as a playback reader.
 *
 * @return Reader, or nullptr when @p representation is null, @p pair is out of
 *         range, or the video system leaves the cadence undefined.
 */
std::shared_ptr<IAudioStreamReader> make_audio_stream_reader(
    std::shared_ptr<const orc::VideoFrameRepresentation> representation,
    size_t pair);

}  // namespace orc::presenters
