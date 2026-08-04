/*
 * File:        audio_output.h
 * Module:      orc-gui
 * Purpose:     Device-free seam for the preview playback audio output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace orc::gui {

/**
 * @brief Sink for interleaved stereo float audio, and the playback clock.
 *
 * The preview playback design makes audio the master clock and lets video
 * chase it, so the only thing the controller needs from a device is: somewhere
 * to push samples, back-pressure telling it how much fits, and how much audio
 * the device has actually consumed so far.
 *
 * Everything is counted in *stereo pairs* (one left plus one right sample),
 * never in bytes: the implementation may fall back from float to a narrower
 * device format, which would make a byte count mean different things on
 * different machines. Pairs are also the unit the SMPTE 272M-1994 §14.3
 * cadence is expressed in, so no conversion is needed anywhere in the clock
 * arithmetic.
 *
 * Thread safety: none. Create, use and destroy on one thread — the GUI thread
 * for the Qt-backed implementation.
 */
class IAudioOutput {
 public:
  virtual ~IAudioOutput() = default;

  /**
   * @brief Open the device and begin consuming written samples.
   *
   * @param sample_rate_hz Sample rate to request (48 000 for pipeline audio)
   * @param channels       Channel count to request (2 for a channel pair)
   * @return false when no device is available or the format is unsupported
   */
  virtual bool start(uint32_t sample_rate_hz, uint32_t channels) = 0;

  /// Stop the device and discard anything still queued. Safe when not started.
  virtual void stop() = 0;

  /// Stereo pairs that write() can currently accept without blocking.
  virtual size_t stereoPairsFree() const = 0;

  /**
   * @brief Queue interleaved stereo float samples (±1.0 full scale).
   *
   * @param interleaved  2 × @p stereo_pairs floats, L R L R …
   * @param stereo_pairs Pairs offered
   * @return Pairs actually accepted; the caller must retain the remainder
   */
  virtual size_t write(const float* interleaved, size_t stereo_pairs) = 0;

  /**
   * @brief Audio consumed by the device since start(), in microseconds.
   *
   * This is the master clock: it advances only as the device actually plays,
   * so it stalls rather than drifts if the feeder ever falls behind.
   */
  virtual uint64_t playedMicroseconds() const = 0;

  /**
   * @brief Set the output amplitude.
   *
   * @param linear Linear amplitude factor, 0.0 (silent) to 1.0 (unattenuated)
   *
   * Applied at the device, not in the sample feed, so a change takes effect
   * without flushing anything already queued and never disturbs the clock.
   */
  virtual void setVolume(double linear) = 0;
};

/**
 * @brief Create the platform audio output, or nullptr when unavailable.
 *
 * Returns nullptr when the build has no audio backend
 * (ORC_GUI_AUDIO_PLAYBACK off). Constructing the returned object is what first
 * touches the audio subsystem, so a project with no audio never does.
 *
 * Must be called on the GUI thread.
 */
std::unique_ptr<IAudioOutput> createSystemAudioOutput();

}  // namespace orc::gui
