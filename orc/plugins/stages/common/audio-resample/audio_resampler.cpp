/*
 * File:        audio_resampler.cpp
 * Module:      orc-audio-resample (shared stage-plugin library)
 * Purpose:     SoXR-based stereo conversion of any-rate audio to the
 *              synchronous 48 kHz 24-bit channel-pair pipeline form
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio_resampler.h"

#include <orc/support/logging.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace orc {

// ---------------------------------------------------------------------------
// widen_16_to_24
// ---------------------------------------------------------------------------

std::vector<int32_t> AudioResampler::widen_16_to_24(
    const std::vector<int16_t>& input) {
  std::vector<int32_t> output(input.size());
  std::transform(input.begin(), input.end(), output.begin(),
                 [](int16_t s) { return static_cast<int32_t>(s) << 8; });
  return output;
}

// ---------------------------------------------------------------------------
// resample
// ---------------------------------------------------------------------------

std::vector<int32_t> AudioResampler::resample(
    const std::vector<int32_t>& input_stereo, double in_rate, double out_rate) {
  if (input_stereo.empty()) return {};
  if (in_rate == out_rate) return input_stereo;

  constexpr unsigned kChannels = 2;
  const size_t in_frames = input_stereo.size() / kChannels;
  if (in_frames == 0) return {};

  // SoXR HQ quality, int32 interleaved I/O.
  // SOXR_INT32_I = signed 32-bit interleaved (all channels in one array).
  // The 24-bit-in-int32 carrier passes through unchanged in scale: SoXR is
  // linear, so 24-bit-ranged input yields 24-bit-ranged output.
  const soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
  const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, 0);

  soxr_error_t err = nullptr;
  soxr_t soxr = soxr_create(in_rate, out_rate, kChannels, &err, &io_spec,
                            &quality, nullptr);
  if (!soxr || err) {
    ORC_LOG_ERROR("AudioResampler: soxr_create failed: {}",
                  err ? err : "null handle");
    if (soxr) soxr_delete(soxr);
    return {};
  }

  std::vector<int32_t> output;
  // Estimate the output frame count with a small safety margin so the whole
  // result lands in one allocation.
  output.reserve((static_cast<size_t>(std::lround(
                      static_cast<double>(in_frames) * out_rate / in_rate)) +
                  64) *
                 kChannels);

  // The stream is fed to SoXR in fixed-size chunks rather than in a single
  // whole-stream call. SoXR buffers everything handed to it in an internal
  // FIFO that grows by exactly one block per realloc (soxr fifo.h:
  // `allocation += n`, never doubling), so a one-shot call makes the resample
  // quadratic on any allocator that cannot extend a large block in place. On
  // glibc that is hidden by mremap, but the Windows heap copies every time,
  // and an hour-long capture (~150 M frames) spent nearly an hour here
  // shifting tens of terabytes (issue #230). Chunking bounds the FIFO to a
  // few hundred KB and makes the cost linear on every platform; the output is
  // byte-identical to the one-shot call.
  constexpr size_t kChunkFrames = 65536;
  // Per-chunk output capacity. A short capacity is not lossy — SoXR simply
  // consumes less input and the loop comes back for the rest — but sizing it
  // to the rate ratio plus slack keeps the common case to one pass per chunk.
  const size_t chunk_capacity =
      static_cast<size_t>(
          std::ceil(static_cast<double>(kChunkFrames) * out_rate / in_rate)) +
      256;

  std::vector<int32_t> chunk(chunk_capacity * kChannels);
  const auto drain = [&output, &chunk](size_t frames) {
    output.insert(
        output.end(), chunk.begin(),
        chunk.begin() + static_cast<std::ptrdiff_t>(frames * kChannels));
  };

  size_t consumed = 0;
  while (consumed < in_frames) {
    const size_t want = std::min(kChunkFrames, in_frames - consumed);
    size_t idone = 0;
    size_t odone = 0;
    err = soxr_process(soxr, input_stereo.data() + consumed * kChannels, want,
                       &idone, chunk.data(), chunk_capacity, &odone);
    if (err) {
      ORC_LOG_WARN("AudioResampler: soxr_process error: {}", err);
      soxr_delete(soxr);
      drain(odone);
      return output;
    }
    drain(odone);
    // No input accepted and no output produced means SoXR cannot make
    // progress; stop rather than spin. Output alone still counts as progress
    // (the FIFO drained, so the next call can take input).
    if (idone == 0 && odone == 0) {
      ORC_LOG_WARN(
          "AudioResampler: SoXR stalled after {} of {} input frames; the "
          "remainder is dropped",
          consumed, in_frames);
      break;
    }
    consumed += idone;
  }

  // Flush residual samples (nullptr input signals end-of-stream to SoXR),
  // draining until it stops producing.
  for (;;) {
    size_t odone = 0;
    err = soxr_process(soxr, nullptr, 0, nullptr, chunk.data(), chunk_capacity,
                       &odone);
    if (err) {
      ORC_LOG_WARN("AudioResampler: soxr_process flush error: {}", err);
      break;
    }
    if (odone == 0) break;
    drain(odone);
  }

  soxr_delete(soxr);
  return output;
}

// ---------------------------------------------------------------------------
// resample_to_synchronous
// ---------------------------------------------------------------------------

std::vector<std::vector<int32_t>> AudioResampler::resample_to_synchronous(
    const std::vector<int32_t>& raw_stereo, double in_rate_hz,
    VideoSystem system, size_t frame_count) {
  std::vector<std::vector<int32_t>> frames(frame_count);
  if (system == VideoSystem::Unknown) return frames;

  if (raw_stereo.empty() || frame_count == 0) {
    for (size_t i = 0; i < frame_count; ++i) {
      frames[i].assign(static_cast<size_t>(audio_pairs_in_frame(i, system)) * 2,
                       0);
    }
    return frames;
  }

  // Convert to the synchronous 48 kHz rate (no-op for 48 kHz input).
  std::vector<int32_t> resampled;
  const std::vector<int32_t>* stream = &raw_stereo;
  if (in_rate_hz != static_cast<double>(kAudioSampleRateHz)) {
    resampled = resample(raw_stereo, in_rate_hz,
                         static_cast<double>(kAudioSampleRateHz));
    stream = &resampled;
  }

  // Segment into cadence-sized blocks; zero-pad short material and truncate
  // excess so the blocks total exactly audio_pair_offset(frame_count) pairs.
  for (size_t i = 0; i < frame_count; ++i) {
    const size_t block_pairs =
        static_cast<size_t>(audio_pairs_in_frame(i, system));
    const size_t src_start =
        static_cast<size_t>(audio_pair_offset(i, system)) * 2;
    const size_t src_end = src_start + block_pairs * 2;

    frames[i].assign(block_pairs * 2, 0);

    if (src_start < stream->size()) {
      const size_t available = std::min(src_end, stream->size()) - src_start;
      std::memcpy(frames[i].data(), stream->data() + src_start,
                  available * sizeof(int32_t));
      // Remaining values are already zero from assign().
    }
    // When src_start >= stream->size() the frame is silent (all zeros).
  }

  return frames;
}

}  // namespace orc
