/*
 * File:        qt_audio_output.h
 * Module:      orc-gui
 * Purpose:     QAudioSink-backed IAudioOutput for preview audio playback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QAudioFormat>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio_output.h"

class QAudioSink;
class QIODevice;

namespace orc::gui {

/**
 * @brief IAudioOutput on top of Qt Multimedia's QAudioSink (push mode).
 *
 * Only compiled when ORC_GUI_AUDIO_PLAYBACK is on; everything else in the
 * playback path is device-free so it builds either way. Nothing in Qt
 * Multimedia is touched until start() is called, so a project with no audio
 * never initialises the audio subsystem.
 *
 * Format: the requested rate with float samples, falling back to signed 16-bit
 * when the device rejects float. The buffer is sized to ~200 ms, which is deep
 * enough to ride out a late feed tick and shallow enough that the video chase
 * stays within a couple of frame periods of what is audible.
 *
 * Thread safety: none, and QAudioSink requires a running event loop to drain
 * the push device — construct and drive this from the GUI thread.
 */
class QtAudioOutput final : public IAudioOutput {
 public:
  QtAudioOutput();
  ~QtAudioOutput() override;

  QtAudioOutput(const QtAudioOutput&) = delete;
  QtAudioOutput& operator=(const QtAudioOutput&) = delete;

  bool start(uint32_t sample_rate_hz, uint32_t channels) override;
  void stop() override;
  size_t stereoPairsFree() const override;
  size_t write(const float* interleaved, size_t stereo_pairs) override;
  uint64_t playedMicroseconds() const override;
  void setVolume(double linear) override;

 private:
  // Device buffer depth. Long enough to absorb a missed feed tick, short
  // enough to keep audio-to-video skew inside a frame or two.
  static constexpr uint32_t kDeviceBufferMs = 200;

  std::unique_ptr<QAudioSink> sink_;
  QIODevice* push_device_ = nullptr;  // Owned by sink_, valid while started
  QAudioFormat format_;
  // Retained across stop()/start() so a volume set while stopped still applies
  // to the next session.
  double volume_ = 1.0;
  // Scratch for the Int16 fallback path; kept to avoid a per-write allocation.
  std::vector<int16_t> convert_buffer_;
};

}  // namespace orc::gui
