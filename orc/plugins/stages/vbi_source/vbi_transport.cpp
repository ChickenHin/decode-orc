/*
 * File:        vbi_transport.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw-file and FLAC-wrapped byte transports for VBI captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_transport.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

namespace orc {

const char kFlacStreamMarker[5] = {'f', 'L', 'a', 'C', '\0'};

bool has_flac_stream_marker(const uint8_t* buffer, size_t length) {
  if (buffer == nullptr || length < 4) {
    return false;
  }
  return std::memcmp(buffer, kFlacStreamMarker, 4) == 0;
}

namespace {

// How far ahead of the current decode position a request may sit before it is
// cheaper to seek than to decode forward.  One FLAC block at the community
// encoder's --blocksize=65535 is ~64 KiB of samples, so this is a handful of
// blocks: far enough that ordinary sequential frame reads never seek, close
// enough that a scrub across the capture does.
constexpr uint64_t kForwardDecodeLimitBytes = 4ull * 1024 * 1024;

// How far back a failed seek is retried.  The community encoder writes no
// SEEKTABLE, so libavformat locates a target by bisecting the stream and
// parsing a frame header at each probe position to read its timestamp.  A
// probe that lands where the parser cannot complete a frame abandons the
// whole seek, even though positions on either side of it are perfectly
// seekable -- the reference capture has one such target in its last few
// frames, which is exactly where reading the final frames lands.  Seeking
// earlier than asked is always sound here, because the caller only needs to
// be positioned at or before the target and decodes forward from there; the
// only cost is decoding the bytes stepped over, which is why the ladder
// starts small.
constexpr uint64_t kSeekRetryBackoffBytes[] = {
    1ull * 1024 * 1024,
    8ull * 1024 * 1024,
    64ull * 1024 * 1024,
};

// Bits per sample this transport can unwrap: 8-bit for the card capture family
// and 16-bit for the TBC-derived one.  Both are re-biased to unsigned on the
// way out, because the FLAC header carries no signedness and every capture in
// circulation was written by the community encoder's `--sign=unsigned`
// invocation.
constexpr uint32_t kSupportedBitsPerSample[] = {8, 16};

bool is_supported_bits_per_sample(int bits) {
  for (const uint32_t supported : kSupportedBitsPerSample) {
    if (bits == static_cast<int>(supported)) {
      return true;
    }
  }
  return false;
}

// -------------------------------------------------------------------------
// Plain file transport
// -------------------------------------------------------------------------

class VBIRawFileByteSource final : public IVBIByteSource {
 public:
  static std::unique_ptr<VBIRawFileByteSource> open(
      const std::string& path, std::string& error_message) {
    auto source =
        std::unique_ptr<VBIRawFileByteSource>(new VBIRawFileByteSource(path));
    if (!source->stream_.is_open()) {
      error_message = "Could not open VBI capture '" + path + "' for reading.";
      return nullptr;
    }
    return source;
  }

  std::optional<uint64_t> size_bytes() const override { return size_bytes_; }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    if (count == 0) {
      return 0;
    }
    if (byte_offset >= size_bytes_) {
      return 0;
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(byte_offset), std::ios::beg);
    if (!stream_) {
      error_message = "Seek to byte " + std::to_string(byte_offset) +
                      " failed in VBI capture '" + path_ + "'.";
      return 0;
    }

    const uint64_t available = size_bytes_ - byte_offset;
    const size_t wanted = static_cast<size_t>(
        std::min<uint64_t>(available, static_cast<uint64_t>(count)));
    stream_.read(reinterpret_cast<char*>(out_buffer),
                 static_cast<std::streamsize>(wanted));
    const size_t produced = static_cast<size_t>(stream_.gcount());
    bytes_read_ += produced;
    return produced;
  }

  uint64_t bytes_decoded() const override { return bytes_read_; }

 private:
  explicit VBIRawFileByteSource(const std::string& path)
      : path_(path), stream_(path, std::ios::binary) {
    if (stream_.is_open()) {
      stream_.seekg(0, std::ios::end);
      size_bytes_ = static_cast<uint64_t>(stream_.tellg());
      stream_.seekg(0, std::ios::beg);
    }
  }

  std::string path_;
  mutable std::ifstream stream_;
  uint64_t size_bytes_ = 0;
  uint64_t bytes_read_ = 0;
};

// -------------------------------------------------------------------------
// FLAC transport
//
// FLAC is a lossless byte compressor here, not audio.  The declared sample
// rate is a conventional placeholder and is never consulted; only the
// declared bits per sample is meaningful, and it seeds sample-format
// detection rather than any timing decision (design §3.3).
//
// The stream is decoded lazily, one block at a time, and never materialised:
// a four-hour bt8x8 capture is 24 GB raw.  Random access asks the demuxer to
// seek so that scrubbing does not re-decode from the head of the file; where
// the capture carries a SEEKTABLE that is an index lookup, and where it does
// not -- which is what the community encoder produces -- libavformat bisects
// the stream instead, which is fallible in the way seek_at_or_before covers.
// -------------------------------------------------------------------------

class VBIFlacByteSource final : public IVBIByteSource {
 public:
  ~VBIFlacByteSource() override {
    av_frame_free(&frame_);
    av_packet_free(&packet_);
    avcodec_free_context(&codec_ctx_);
    if (format_ctx_ != nullptr) {
      avformat_close_input(&format_ctx_);
    }
  }

  static std::unique_ptr<VBIFlacByteSource> open(const std::string& path,
                                                 std::string& error_message);

  std::optional<uint64_t> size_bytes() const override {
    if (total_bytes_ == 0) {
      return std::nullopt;
    }
    return total_bytes_;
  }

  std::optional<uint32_t> declared_bits_per_sample() const override {
    return bits_per_sample_;
  }

  uint64_t bytes_decoded() const override { return bytes_decoded_; }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override;

 private:
  VBIFlacByteSource() = default;

  // Reposition the decoder so that the next decoded block covers or precedes
  // target_byte, seeking only when decoding forward would be wasteful.
  bool ensure_position(uint64_t target_byte, std::string& error_message);

  // Seek to the last stream position at or before target_byte, retrying
  // earlier positions when the demuxer refuses one.  On success
  // out_landed_byte is the position actually asked for, which is at or before
  // target_byte and is where the decoder will resume.
  bool seek_at_or_before(uint64_t target_byte, uint64_t& out_landed_byte);

  // Decode the next block into block_bytes_, setting block_start_byte_ from
  // its presentation timestamp.  Returns false at end of stream (with an
  // empty error message) or on a decode failure.
  bool decode_next_block(std::string& error_message);

  // Convert one decoded AVFrame into the capture's raw byte sequence.
  bool convert_frame_to_bytes(std::string& error_message);

  AVFormatContext* format_ctx_ = nullptr;
  AVCodecContext* codec_ctx_ = nullptr;
  AVPacket* packet_ = nullptr;
  AVFrame* frame_ = nullptr;
  int stream_index_ = -1;

  uint32_t bits_per_sample_ = 0;
  uint32_t bytes_per_sample_ = 0;
  uint64_t total_bytes_ = 0;

  // Used only to convert a block's timestamp into a sample index, never to
  // derive VBI timing: the declared rate is a placeholder (design §3.3).
  AVRational stream_time_base_{0, 1};
  int declared_sample_rate_ = 0;

  std::vector<uint8_t> block_bytes_;
  uint64_t block_start_byte_ = 0;
  bool have_block_ = false;
  bool draining_ = false;
  bool at_end_ = false;
  uint64_t bytes_decoded_ = 0;

  // Where the decoder will resume when no block has been decoded since the
  // last reposition.  Without a seek it resumes at the head of the stream;
  // after one it resumes at or before the byte that was seeked to.
  bool seek_pending_ = false;
  uint64_t seek_target_byte_ = 0;
};

std::unique_ptr<VBIFlacByteSource> VBIFlacByteSource::open(
    const std::string& path, std::string& error_message) {
  auto source = std::unique_ptr<VBIFlacByteSource>(new VBIFlacByteSource());

  if (avformat_open_input(&source->format_ctx_, path.c_str(), nullptr,
                          nullptr) < 0) {
    error_message = "Could not open FLAC-wrapped VBI capture '" + path + "'.";
    return nullptr;
  }
  if (avformat_find_stream_info(source->format_ctx_, nullptr) < 0) {
    error_message = "Could not read the stream information of '" + path + "'.";
    return nullptr;
  }

  for (unsigned int index = 0; index < source->format_ctx_->nb_streams;
       ++index) {
    const AVCodecParameters* params =
        source->format_ctx_->streams[index]->codecpar;
    if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
      source->stream_index_ = static_cast<int>(index);
      break;
    }
  }
  if (source->stream_index_ < 0) {
    error_message = "'" + path + "' contains no FLAC stream to unwrap.";
    return nullptr;
  }

  AVStream* stream = source->format_ctx_->streams[source->stream_index_];
  AVCodecParameters* params = stream->codecpar;

  if (params->codec_id != AV_CODEC_ID_FLAC) {
    error_message = "'" + path +
                    "' is compressed with an unsupported codec; VBI captures "
                    "are transported either raw or FLAC-wrapped.";
    return nullptr;
  }

  // The community encoder invocation uses --channels=1; a multi-channel
  // stream would interleave the byte sequence and is not a VBI capture.
  const int channels = params->ch_layout.nb_channels;
  if (channels != 1) {
    error_message = "'" + path + "' has " + std::to_string(channels) +
                    " channels; a FLAC-wrapped VBI capture is mono.";
    return nullptr;
  }

  int declared_bits = params->bits_per_raw_sample;
  if (declared_bits <= 0) {
    declared_bits = params->bits_per_coded_sample;
  }
  if (!is_supported_bits_per_sample(declared_bits)) {
    error_message = "'" + path + "' declares " + std::to_string(declared_bits) +
                    " bits per sample; a FLAC-wrapped VBI capture holds 8-bit "
                    "or 16-bit samples.";
    return nullptr;
  }
  source->bits_per_sample_ = static_cast<uint32_t>(declared_bits);
  source->bytes_per_sample_ = (source->bits_per_sample_ + 7u) / 8u;

  const AVCodec* codec = avcodec_find_decoder(params->codec_id);
  if (codec == nullptr) {
    error_message = "No FLAC decoder is available in this build.";
    return nullptr;
  }
  source->codec_ctx_ = avcodec_alloc_context3(codec);
  if (source->codec_ctx_ == nullptr) {
    error_message = "Could not allocate a FLAC decoder context.";
    return nullptr;
  }
  if (avcodec_parameters_to_context(source->codec_ctx_, params) < 0) {
    error_message = "Could not configure the FLAC decoder for '" + path + "'.";
    return nullptr;
  }
  // Required for the decoder to propagate packet timestamps onto frames,
  // which is how a decoded block's byte position is established after a seek.
  source->codec_ctx_->pkt_timebase = stream->time_base;
  source->stream_time_base_ = stream->time_base;
  source->declared_sample_rate_ = params->sample_rate;
  if (avcodec_open2(source->codec_ctx_, codec, nullptr) < 0) {
    error_message = "Could not open the FLAC decoder for '" + path + "'.";
    return nullptr;
  }

  source->packet_ = av_packet_alloc();
  source->frame_ = av_frame_alloc();
  if (source->packet_ == nullptr || source->frame_ == nullptr) {
    error_message = "Could not allocate FLAC decoding buffers.";
    return nullptr;
  }

  // The FLAC STREAMINFO block carries the total sample count, which the
  // demuxer exposes as the stream duration in sample units.  Note that the
  // declared sample rate is deliberately not used for anything.
  if (stream->duration > 0) {
    source->total_bytes_ =
        static_cast<uint64_t>(stream->duration) * source->bytes_per_sample_;
  }

  ORC_LOG_DEBUG(
      "VBI transport: unwrapping FLAC capture '{}' ({} bits/sample, {} raw "
      "bytes); the declared sample rate of {} Hz is ignored",
      path, source->bits_per_sample_, source->total_bytes_,
      params->sample_rate);

  return source;
}

bool VBIFlacByteSource::convert_frame_to_bytes(std::string& error_message) {
  const AVSampleFormat sample_fmt = static_cast<AVSampleFormat>(frame_->format);
  const int container_bytes = av_get_bytes_per_sample(sample_fmt);
  if (container_bytes != 2 && container_bytes != 4) {
    error_message =
        "The FLAC decoder produced an unexpected sample format for a VBI "
        "capture.";
    return false;
  }

  // The decoder left-aligns the coded sample within its container word, so
  // the raw value is recovered by shifting back down.  The community encoder
  // uses --sign=unsigned, so the signed decoded value is re-biased to the
  // unsigned byte the capture card originally wrote.
  const int shift = container_bytes * 8 - static_cast<int>(bits_per_sample_);
  const int32_t unsigned_bias = 1 << (bits_per_sample_ - 1);

  const uint8_t* plane = frame_->extended_data[0];
  const int sample_count = frame_->nb_samples;

  // Captured before the resize below so the timestamp-free fallback can
  // advance the position by the block that is being replaced.
  const size_t previous_block_bytes = block_bytes_.size();

  block_bytes_.resize(static_cast<size_t>(sample_count) * bytes_per_sample_);
  for (int index = 0; index < sample_count; ++index) {
    int32_t coded = 0;
    if (container_bytes == 2) {
      coded = reinterpret_cast<const int16_t*>(plane)[index];
    } else {
      coded = reinterpret_cast<const int32_t*>(plane)[index];
    }

    const uint32_t raw =
        static_cast<uint32_t>((coded >> shift) + unsigned_bias);
    for (uint32_t byte_index = 0; byte_index < bytes_per_sample_;
         ++byte_index) {
      block_bytes_[static_cast<size_t>(index) * bytes_per_sample_ +
                   byte_index] =
          static_cast<uint8_t>((raw >> (8u * byte_index)) & 0xFFu);
    }
  }

  // The block's position comes from its timestamp rather than from an
  // accumulated count, so a seek lands at a known byte offset.
  if (frame_->pts != AV_NOPTS_VALUE && declared_sample_rate_ > 0) {
    const AVRational sample_time_base{1, declared_sample_rate_};
    const int64_t sample_index =
        av_rescale_q(frame_->pts, stream_time_base_, sample_time_base);
    block_start_byte_ = static_cast<uint64_t>(sample_index) * bytes_per_sample_;
  } else if (have_block_) {
    block_start_byte_ += previous_block_bytes;
  } else {
    error_message =
        "The FLAC stream has no timestamps, so a byte position cannot be "
        "established.";
    return false;
  }

  have_block_ = true;
  bytes_decoded_ += block_bytes_.size();
  return true;
}

bool VBIFlacByteSource::decode_next_block(std::string& error_message) {
  while (true) {
    const int receive_result = avcodec_receive_frame(codec_ctx_, frame_);
    if (receive_result == 0) {
      return convert_frame_to_bytes(error_message);
    }
    if (receive_result == AVERROR_EOF) {
      at_end_ = true;
      return false;
    }
    if (receive_result != AVERROR(EAGAIN)) {
      error_message = "FLAC decoding failed while unwrapping the capture.";
      return false;
    }

    if (draining_) {
      at_end_ = true;
      return false;
    }

    const int read_result = av_read_frame(format_ctx_, packet_);
    if (read_result == AVERROR_EOF) {
      avcodec_send_packet(codec_ctx_, nullptr);
      draining_ = true;
      continue;
    }
    if (read_result < 0) {
      error_message =
          "Reading the FLAC stream failed while unwrapping the "
          "capture.";
      return false;
    }
    if (packet_->stream_index != stream_index_) {
      av_packet_unref(packet_);
      continue;
    }
    const int send_result = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
      error_message = "The FLAC decoder rejected a packet.";
      return false;
    }
  }
}

bool VBIFlacByteSource::seek_at_or_before(uint64_t target_byte,
                                          uint64_t& out_landed_byte) {
  const auto attempt = [&](uint64_t candidate_byte) {
    const int64_t candidate_sample =
        static_cast<int64_t>(candidate_byte / bytes_per_sample_);
    if (av_seek_frame(format_ctx_, stream_index_, candidate_sample,
                      AVSEEK_FLAG_BACKWARD) < 0) {
      return false;
    }
    out_landed_byte = candidate_byte;
    return true;
  };

  if (attempt(target_byte)) {
    return true;
  }

  for (const uint64_t backoff : kSeekRetryBackoffBytes) {
    if (backoff >= target_byte) {
      break;
    }
    if (attempt(target_byte - backoff)) {
      ORC_LOG_WARN(
          "VBI transport: the FLAC demuxer refused a seek to byte {}; "
          "resumed from byte {} and decoded forward",
          target_byte, target_byte - backoff);
      return true;
    }
  }

  // The head of the stream needs no bisection, so it is the one position that
  // is always reachable; taking it turns a hard failure into a slow read
  // rather than a wrong one.
  if (target_byte > 0 && attempt(0)) {
    ORC_LOG_WARN(
        "VBI transport: no seek point near byte {} was accepted; decoding "
        "forward from the head of the stream",
        target_byte);
    return true;
  }
  return false;
}

bool VBIFlacByteSource::ensure_position(uint64_t target_byte,
                                        std::string& error_message) {
  if (have_block_) {
    const uint64_t block_end = block_start_byte_ + block_bytes_.size();
    const bool behind = target_byte < block_start_byte_;
    const bool far_ahead = target_byte > block_end + kForwardDecodeLimitBytes;
    if (!behind && !far_ahead) {
      return true;
    }
  } else if (seek_pending_) {
    // The decoder is positioned to resume at or before the last seek target,
    // so anything from there up to the forward window is reachable without
    // seeking again.
    if (target_byte >= seek_target_byte_ &&
        target_byte <= seek_target_byte_ + kForwardDecodeLimitBytes) {
      return true;
    }
  } else if (target_byte <= kForwardDecodeLimitBytes) {
    // Untouched stream: the decoder resumes at the head.
    return true;
  }

  uint64_t landed_byte = 0;
  if (!seek_at_or_before(target_byte, landed_byte)) {
    error_message = "Seeking the FLAC stream to byte " +
                    std::to_string(target_byte) + " failed.";
    return false;
  }

  avcodec_flush_buffers(codec_ctx_);
  block_bytes_.clear();
  have_block_ = false;
  draining_ = false;
  at_end_ = false;
  seek_pending_ = true;
  // Where the decoder resumes, which a retry may have put earlier than asked.
  seek_target_byte_ = landed_byte;
  return true;
}

size_t VBIFlacByteSource::read_at(uint64_t byte_offset, size_t count,
                                  uint8_t* out_buffer,
                                  std::string& error_message) {
  if (count == 0) {
    return 0;
  }
  if (!ensure_position(byte_offset, error_message)) {
    return 0;
  }

  size_t produced = 0;
  while (produced < count) {
    const uint64_t wanted_byte = byte_offset + produced;

    if (have_block_ && wanted_byte >= block_start_byte_ &&
        wanted_byte < block_start_byte_ + block_bytes_.size()) {
      const size_t within =
          static_cast<size_t>(wanted_byte - block_start_byte_);
      const size_t available = block_bytes_.size() - within;
      const size_t chunk = std::min(count - produced, available);
      std::memcpy(out_buffer + produced, block_bytes_.data() + within, chunk);
      produced += chunk;
      continue;
    }

    if (have_block_ && wanted_byte < block_start_byte_) {
      // A seek landed past the request; decoding forward can never reach it.
      error_message = "The FLAC stream seeked past byte " +
                      std::to_string(wanted_byte) +
                      "; the capture's seek points are inconsistent.";
      return 0;
    }

    if (!decode_next_block(error_message)) {
      if (!error_message.empty()) {
        return 0;
      }
      break;  // End of stream: a short read, which the caller interprets.
    }
  }

  return produced;
}

}  // namespace

std::unique_ptr<IVBIByteSource> open_vbi_byte_source(
    const std::string& path, std::string& error_message) {
  uint8_t magic[4] = {0, 0, 0, 0};
  size_t magic_length = 0;
  {
    std::ifstream probe(path, std::ios::binary);
    if (!probe.is_open()) {
      error_message = "Could not open VBI capture '" + path + "' for reading.";
      return nullptr;
    }
    probe.read(reinterpret_cast<char*>(magic), sizeof(magic));
    magic_length = static_cast<size_t>(probe.gcount());
  }

  if (has_flac_stream_marker(magic, magic_length)) {
    return VBIFlacByteSource::open(path, error_message);
  }
  return VBIRawFileByteSource::open(path, error_message);
}

}  // namespace orc
