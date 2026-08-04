/*
 * File:        preview_audio_controller.h
 * Module:      orc-gui
 * Purpose:     Audio-mastered playback session for the preview dialogue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <audio_stream_reader.h>  // IAudioStreamReader
#include <orc/stage/frame_id.h>   // FrameID, FrameIDRange

#include <QObject>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio_output.h"

class QTimer;

namespace orc::gui {

/**
 * @brief Plays one audio channel pair and tells the preview what frame to show.
 *
 * Preview rendering cannot be relied on to reach real time — a heavy DAG can
 * take seconds per frame — so the audio device is the clock and the video
 * chases it. The controller feeds the device continuously at 1× and, on every
 * playback tick, reports the preview index whose audio the device has actually
 * reached. Frames the renderer could not deliver in time are simply skipped by
 * the target jumping more than one index; no frame-dropping machinery is
 * needed beyond that.
 *
 * Because the clock is derived from audio the device has consumed, an underrun
 * stalls the chase instead of desynchronising it: the target stops advancing
 * until the device is fed again, and the frame ↔ stream-position mapping stays
 * exact because samples are always fed contiguously from the start frame.
 *
 * The reader must already be primed — prime() can take minutes on an EFM or
 * TBC source and belongs behind a progress dialogue on the caller's side.
 *
 * Thread affinity: GUI thread. Reads from the reader are cheap once primed
 * (an O(1) vector copy per frame), so the feeder runs on the GUI thread's
 * timer; moving it to a worker later needs no interface change because the
 * reader's read methods are already safe from one other thread.
 */
class PreviewAudioController : public QObject {
  Q_OBJECT

 public:
  explicit PreviewAudioController(QObject* parent = nullptr);
  ~PreviewAudioController() override;

  /**
   * @brief Install the audio device.
   *
   * Takes ownership. Passing nullptr (what createSystemAudioOutput() returns
   * when there is no backend) leaves the controller inert: start() fails and
   * the dialogue keeps its video-only playback. Stops any playback in
   * progress.
   */
  void setAudioOutput(std::unique_ptr<IAudioOutput> output);

  /// Whether an audio device is installed and playback is possible at all.
  bool hasAudioOutput() const { return output_ != nullptr; }

  /**
   * @brief Install the channel-pair reader for the session.
   *
   * Takes a share of the reader, which keeps the underlying representation
   * alive for as long as playback needs it. Stops any playback in progress —
   * a new reader means a new node, pair or DAG version.
   */
  void setReader(std::shared_ptr<orc::presenters::IAudioStreamReader> reader);

  /// Whether a reader is installed.
  bool hasReader() const { return reader_ != nullptr; }

  /**
   * @brief Preview items per video frame at the viewed output.
   *
   * 1 for a frame-indexed output, 2 for a field-indexed one. Audio maps to
   * whole frames either way; this only scales the reported target index.
   */
  void setItemsPerFrame(uint32_t items_per_frame);
  uint32_t itemsPerFrame() const { return items_per_frame_; }

  /**
   * @brief Open the device and begin playing from @p first_frame.
   *
   * @return false when there is no output or reader, when the frame is outside
   *         the reader's range, or when the device refused to start.
   */
  bool start(orc::FrameID first_frame);

  /// Stop the device, discard queued audio and keep the reader for a restart.
  void stop();

  /**
   * @brief Move the playback position to @p frame.
   *
   * Restarts the device from the new position when playing, so the clock is
   * always rebased by construction rather than corrected; when stopped it just
   * records where the next start() begins.
   */
  void seek(orc::FrameID frame);

  bool isPlaying() const { return state_ == State::kPlaying; }

  /// Frame the playback started from (also where a stopped controller sits).
  orc::FrameID startFrame() const { return start_frame_; }

  /**
   * @brief Video frame the device's audio clock has reached.
   *
   * Clamped to the reader's range. Equals startFrame() when not playing.
   */
  orc::FrameID targetFrame() const;

  /// targetFrame() expressed as a preview item index (see setItemsPerFrame).
  uint64_t targetPreviewIndex() const;

  /**
   * @brief Set the playback volume from a linear 0.0–1.0 control.
   *
   * Forwarded to the device as volume², which approximates perceptual loudness
   * for a linear slider. Applies live and is retained across sessions.
   */
  void setVolume(double volume);
  double volume() const { return volume_; }

  /// Mute without pausing: the clock runs on and the video keeps chasing.
  void setMuted(bool muted);
  bool isMuted() const { return muted_; }

  /// Underruns observed since the last start(); exposed for tests and logging.
  uint64_t underrunCount() const { return underrun_count_; }

 public slots:
  /**
   * @brief Top the device up and check for drain or underrun.
   *
   * Driven by the internal timer while playing; called directly by tests so
   * they need no event loop.
   */
  void feed();

 signals:
  /// The last frame's audio has finished playing. The controller is stopped.
  void finished();

  /// The device ran dry mid-stream; the chase stalls until it is fed again.
  void underrunDetected();

 private:
  enum class State { kStopped, kPlaying };

  // Stereo pairs the device has consumed, never more than has been fed: a
  // device reporting further would push the chase past audio nobody has heard.
  uint64_t playedPairs() const;

  // Pairs read from the reader but not yet accepted by the device.
  uint64_t pendingPairs() const;

  // Pairs read from the reader and not yet played, queued or pending.
  uint64_t bufferedPairs() const;

  // Whether every frame in range has been read into the feed path.
  bool feedExhausted() const;

  // Push as much of the pending buffer into the device as it will take.
  void writePending();

  // Send the current volume/mute state to the device.
  void applyVolume();

  // Stop and report the end of the stream.
  void finish();

  std::unique_ptr<IAudioOutput> output_;
  std::shared_ptr<orc::presenters::IAudioStreamReader> reader_;
  QTimer* feed_timer_ = nullptr;

  State state_ = State::kStopped;
  uint32_t items_per_frame_ = 1;

  // Frames are fed contiguously from here, so played pairs map back to an
  // absolute stream position without any per-underrun bookkeeping.
  orc::FrameID start_frame_ = 0;
  orc::FrameID feed_cursor_ = 0;
  orc::FrameIDRange range_{1, 0};  // Empty until a reader is installed

  uint64_t fed_pairs_ = 0;
  uint64_t lead_target_pairs_ = 0;
  uint64_t underrun_count_ = 0;

  // Samples read from the reader that the device would not take yet.
  std::vector<float> pending_;
  size_t pending_offset_ = 0;  // Floats already accepted from pending_

  double volume_ = 1.0;
  bool muted_ = false;
};

}  // namespace orc::gui
