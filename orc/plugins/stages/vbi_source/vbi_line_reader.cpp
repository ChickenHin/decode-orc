/*
 * File:        vbi_line_reader.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Turns a raw VBI byte stream into indexed line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_reader.h"

#include <utility>

namespace orc {

namespace {

// Fields stored per frame.  Every format in scope stores two sequential
// fields per frame; the mapping onto TV field parity is a separate concern
// (VBISourceFormat::first_field).
constexpr uint32_t kStoredFieldsPerFrame = 2;

// Width of the bt8x8 frame sequence number at the tail of each frame.
constexpr uint32_t kFrameCounterBytes = 4;

}  // namespace

bool decode_vbi_samples(VBISampleFormat sample_format,
                        const uint8_t* record_bytes, uint32_t sample_count,
                        std::vector<double>& out_samples,
                        std::string& error_message) {
  out_samples.clear();

  switch (sample_format) {
    case VBISampleFormat::kU8:
      out_samples.reserve(sample_count);
      for (uint32_t index = 0; index < sample_count; ++index) {
        out_samples.push_back(static_cast<double>(record_bytes[index]));
      }
      return true;

    case VBISampleFormat::kU16LE:
    case VBISampleFormat::kS16LE:
      // The 16-bit decode path belongs with the TBC-derived source family,
      // which is not yet wired through the rest of the stage.  Failing here
      // is deliberate: a half-supported format would produce plausible but
      // wrong output.
      error_message = "Sample format '" + to_string(sample_format) +
                      "' is not implemented yet; only 'u8' captures can "
                      "currently be read.";
      return false;
  }

  error_message = "Unrecognised sample format.";
  return false;
}

VBILineReader::VBILineReader(VBISourceFormat format,
                             IVBIByteSource& byte_source)
    : format_(std::move(format)), byte_source_(&byte_source) {}

std::optional<uint64_t> VBILineReader::frame_count() const {
  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    return std::nullopt;
  }

  const std::optional<uint64_t> stream_bytes = byte_source_->size_bytes();
  if (!stream_bytes.has_value()) {
    return std::nullopt;
  }
  return *stream_bytes / frame_bytes;
}

bool VBILineReader::has_partial_trailing_frame() const {
  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    return false;
  }

  const std::optional<uint64_t> stream_bytes = byte_source_->size_bytes();
  if (!stream_bytes.has_value()) {
    return false;
  }
  return (*stream_bytes % frame_bytes) != 0;
}

bool VBILineReader::read_frame(uint64_t frame_index,
                               VBIFrameRecords& out_records,
                               std::string& error_message) const {
  out_records = VBIFrameRecords{};
  out_records.frame_index = frame_index;

  const uint64_t frame_bytes = format_.bytes_per_frame();
  if (frame_bytes == 0) {
    error_message =
        "VBI container geometry is unset (line_length and field_lines must "
        "both be non-zero).";
    return false;
  }

  const uint64_t frame_offset = frame_index * frame_bytes;

  std::vector<uint8_t> frame_buffer(static_cast<size_t>(frame_bytes));
  const size_t bytes_read = byte_source_->read_at(
      frame_offset, frame_buffer.size(), frame_buffer.data(), error_message);
  if (!error_message.empty()) {
    return false;
  }
  if (bytes_read != frame_buffer.size()) {
    // A short final frame is a reported error, never a silent truncation: it
    // means the configured frame size does not divide the capture, so the
    // whole configuration is suspect (design §1.1, §8).
    error_message = "Capture ends part-way through frame " +
                    std::to_string(frame_index) + ": read " +
                    std::to_string(bytes_read) + " of " +
                    std::to_string(frame_bytes) +
                    " bytes. The stream length is not an exact multiple of the "
                    "configured frame size.";
    return false;
  }

  // The frame trailer overlaps the padding of the final record rather than
  // extending the frame, so it is read from the frame's last bytes.
  if (format_.frame_trailer_is_counter &&
      format_.frame_trailer_bytes >= kFrameCounterBytes &&
      frame_bytes >= kFrameCounterBytes) {
    const uint8_t* counter_bytes =
        frame_buffer.data() + frame_buffer.size() - kFrameCounterBytes;
    const uint32_t counter = static_cast<uint32_t>(counter_bytes[0]) |
                             (static_cast<uint32_t>(counter_bytes[1]) << 8) |
                             (static_cast<uint32_t>(counter_bytes[2]) << 16) |
                             (static_cast<uint32_t>(counter_bytes[3]) << 24);
    out_records.frame_counter = counter;
  }

  const uint64_t record_bytes = format_.bytes_per_record();
  const uint64_t field_bytes = format_.bytes_per_field();
  const uint32_t records_per_field = format_.field_range.count();

  out_records.lines.reserve(static_cast<size_t>(records_per_field) *
                            kStoredFieldsPerFrame);

  for (uint32_t field_index = 0; field_index < kStoredFieldsPerFrame;
       ++field_index) {
    for (uint32_t record_index = format_.field_range.start;
         record_index <= format_.field_range.end &&
         record_index < format_.field_lines;
         ++record_index) {
      const uint64_t record_offset =
          field_index * field_bytes + record_index * record_bytes;

      VBILineRecord line;
      line.frame_index = frame_index;
      line.field_index = field_index;
      line.record_index = record_index;
      line.source_byte_offset = frame_offset + record_offset;
      line.source_byte_length = static_cast<uint64_t>(format_.valid_samples) *
                                format_.bytes_per_sample();

      if (!decode_vbi_samples(
              format_.sample_format, frame_buffer.data() + record_offset,
              format_.valid_samples, line.samples, error_message)) {
        return false;
      }

      out_records.lines.push_back(std::move(line));
    }
  }

  return true;
}

}  // namespace orc
