/*
 * File:        qt_audio_output.cpp
 * Module:      orc-gui
 * Purpose:     QAudioSink-backed IAudioOutput for preview audio playback
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "qt_audio_output.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <algorithm>
#include <cmath>

#include "logging.h"

namespace orc::gui {

namespace {

// Float ±1.0 to signed 16-bit, matching the carrier-domain conversions in
// orc/stage/audio/audio_sample_feed.h: a float sample is carrier / 8388608 and
// the S16 feed is carrier >> 8, so full scale is ±32768 with the positive end
// saturating at the int16 maximum.
int16_t float_to_s16(float sample) {
  const int64_t scaled = std::llround(sample * 32768.0f);
  return static_cast<int16_t>(std::clamp<int64_t>(scaled, -32768, 32767));
}

}  // namespace

QtAudioOutput::QtAudioOutput() = default;

QtAudioOutput::~QtAudioOutput() { stop(); }

bool QtAudioOutput::start(uint32_t sample_rate_hz, uint32_t channels) {
  stop();

  const QAudioDevice device = QMediaDevices::defaultAudioOutput();
  if (device.isNull()) {
    ORC_LOG_WARN("QtAudioOutput: No default audio output device available");
    return false;
  }

  QAudioFormat format;
  format.setSampleRate(static_cast<int>(sample_rate_hz));
  format.setChannelCount(static_cast<int>(channels));
  format.setSampleFormat(QAudioFormat::Float);

  if (!device.isFormatSupported(format)) {
    // Fall back to the format every device supports rather than resampling.
    format.setSampleFormat(QAudioFormat::Int16);
    if (!device.isFormatSupported(format)) {
      ORC_LOG_WARN(
          "QtAudioOutput: Device '{}' supports neither float nor 16-bit "
          "{} Hz {}-channel output",
          device.description().toStdString(), sample_rate_hz, channels);
      return false;
    }
    ORC_LOG_DEBUG("QtAudioOutput: Device rejected float, using 16-bit output");
  }

  format_ = format;
  sink_ = std::make_unique<QAudioSink>(device, format_);
  sink_->setBufferSize(
      format_.bytesForDuration(static_cast<qint64>(kDeviceBufferMs) * 1000));

  push_device_ = sink_->start();
  if (push_device_ == nullptr) {
    ORC_LOG_WARN("QtAudioOutput: Failed to start audio sink (state {})",
                 static_cast<int>(sink_->state()));
    sink_.reset();
    return false;
  }

  sink_->setVolume(volume_);

  ORC_LOG_DEBUG(
      "QtAudioOutput: Started {} Hz {}-channel {} output on '{}' ({} byte "
      "buffer)",
      sample_rate_hz, channels,
      format_.sampleFormat() == QAudioFormat::Float ? "float" : "16-bit",
      device.description().toStdString(), sink_->bufferSize());
  return true;
}

void QtAudioOutput::stop() {
  if (!sink_) {
    return;
  }
  sink_->stop();
  // The push device belongs to the sink and does not outlive it.
  push_device_ = nullptr;
  sink_.reset();
}

size_t QtAudioOutput::stereoPairsFree() const {
  if (!sink_ || push_device_ == nullptr) {
    return 0;
  }
  const int bytes_per_pair = format_.bytesPerFrame();
  if (bytes_per_pair <= 0) {
    return 0;
  }
  return static_cast<size_t>(sink_->bytesFree() / bytes_per_pair);
}

size_t QtAudioOutput::write(const float* interleaved, size_t stereo_pairs) {
  if (!sink_ || push_device_ == nullptr || interleaved == nullptr ||
      stereo_pairs == 0) {
    return 0;
  }

  const int bytes_per_pair = format_.bytesPerFrame();
  if (bytes_per_pair <= 0) {
    return 0;
  }

  // Never offer more than the device can take: a partially accepted write
  // would leave the caller re-queuing a tail it has to track anyway, and this
  // keeps that case to what the device genuinely refused.
  const size_t offered = std::min(stereo_pairs, stereoPairsFree());
  if (offered == 0) {
    return 0;
  }

  const size_t channels = static_cast<size_t>(format_.channelCount());
  qint64 written_bytes = 0;

  if (format_.sampleFormat() == QAudioFormat::Float) {
    written_bytes = push_device_->write(
        reinterpret_cast<const char*>(interleaved),
        static_cast<qint64>(offered) * static_cast<qint64>(bytes_per_pair));
  } else {
    const size_t sample_count = offered * channels;
    convert_buffer_.resize(sample_count);
    for (size_t s = 0; s < sample_count; ++s) {
      convert_buffer_[s] = float_to_s16(interleaved[s]);
    }
    written_bytes = push_device_->write(
        reinterpret_cast<const char*>(convert_buffer_.data()),
        static_cast<qint64>(offered) * static_cast<qint64>(bytes_per_pair));
  }

  if (written_bytes <= 0) {
    return 0;
  }

  // A device that stops mid-pair would desynchronise the channels for the rest
  // of the session; report only whole pairs so the caller re-sends the tail.
  if (written_bytes % bytes_per_pair != 0) {
    ORC_LOG_WARN(
        "QtAudioOutput: Device accepted a partial stereo pair ({} of "
        "{} bytes per pair)",
        written_bytes % bytes_per_pair, bytes_per_pair);
  }
  return static_cast<size_t>(written_bytes / bytes_per_pair);
}

uint64_t QtAudioOutput::playedMicroseconds() const {
  if (!sink_) {
    return 0;
  }
  // processedUSecs() counts audio the device has consumed since start(), so it
  // stalls on an underrun instead of running away from what is audible.
  return static_cast<uint64_t>(sink_->processedUSecs());
}

void QtAudioOutput::setVolume(double linear) {
  volume_ = std::clamp(linear, 0.0, 1.0);
  if (sink_) {
    // Applied at the device: already-queued samples are untouched, so a
    // volume or mute change never flushes the buffer or disturbs the clock.
    sink_->setVolume(static_cast<qreal>(volume_));
  }
}

}  // namespace orc::gui
