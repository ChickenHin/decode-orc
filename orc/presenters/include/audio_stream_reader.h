/*
 * File:        audio_stream_reader.h
 * Module:      orc-presenters
 * Purpose:     Frame-addressed audio sample reader contract for preview
 *              playback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/audio/audio_channel_pair.h>  // kAudioSampleRateHz
#include <orc/stage/frame_id.h>                  // FrameID, FrameIDRange

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @brief Progress sink for a deferred whole-stream audio decode.
 *
 * (done, total, message). @c total may be 0 while the work size is unknown, in
 * which case only @c message is meaningful. Signature-compatible with the SDK's
 * AudioDecodeProgressFn so it forwards straight through to the producer.
 */
using AudioPrimeProgressCallback =
    std::function<void(uint64_t done, uint64_t total, const std::string&)>;

/**
 * @brief Reads one audio channel pair as frame-addressed interleaved stereo.
 *
 * Wraps a resolved representation's audio surface for a single channel pair and
 * hands out float samples ready for an audio device. All frame ↔
 * stream-position arithmetic follows the SMPTE 272M-1994 §14.3 cadence (PAL
 * 1920 pairs/frame; NTSC/PAL-M 1602/1601/1602/1601/1602), so callers never
 * assume a constant per-frame stride.
 *
 * Thread safety: a reader is *created* on the render worker (creation executes
 * the DAG, which is single-threaded by contract). Once created, prime() and the
 * read/mapping methods may be called from any one other thread — they only use
 * representation const accessors, which are documented safe for concurrent use.
 * The reader itself holds the representation alive for its lifetime.
 */
class IAudioStreamReader {
 public:
  virtual ~IAudioStreamReader() = default;

  /**
   * @brief Force any deferred whole-stream decode to run now.
   *
   * Blocking: an EFM or TBC audio decode plus resample can take seconds to
   * minutes, so never call this on the GUI thread or on the render worker.
   * Idempotent, and safe to call with an empty @p progress.
   */
  virtual void prime(const AudioPrimeProgressCallback& progress) = 0;

  /**
   * @brief Interleaved stereo float samples (±1.0) for a run of frames.
   *
   * Covers the half-open frame range [@p first_frame, @p first_frame +
   * @p frame_count), clamped to frameRange(). The returned vector holds
   * 2 × (sum of audio_pairs_in_frame() over the covered frames) floats;
   * cadence-exact silence is substituted for any frame the representation has
   * no samples for, so the clock arithmetic stays intact across gaps.
   */
  virtual std::vector<float> readFrames(orc::FrameID first_frame,
                                        uint64_t frame_count) = 0;

  /// Frame containing absolute stereo-pair stream position @p pair_position.
  virtual uint64_t frameForPairPosition(uint64_t pair_position) const = 0;

  /// Absolute stereo-pair stream position of the start of frame @p frame.
  virtual uint64_t pairPositionForFrame(orc::FrameID frame) const = 0;

  /// Frames this reader can serve (inclusive on both ends).
  virtual orc::FrameIDRange frameRange() const = 0;
};

}  // namespace orc::presenters
