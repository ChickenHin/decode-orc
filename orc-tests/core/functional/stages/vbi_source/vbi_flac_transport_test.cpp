/*
 * File:        vbi_flac_transport_test.cpp
 * Module:      orc-tests
 * Purpose:     Functional tests for FLAC unwrapping of a real VBI capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_line_reader.h"
#include "vbi_source_format.h"
#include "vbi_source_validation.h"
#include "vbi_transport.h"

namespace orc {
namespace {

// The reference sample: a four-hour FLAC-wrapped bt8x8 PAL capture. It is not
// checked into the repository (test-data/ is ignored), so every test here
// skips when it is absent. Note the space in the directory name.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

// The decoded length stated in the design: exactly 368 007 bt8x8 PAL frames
// of 65 536 bytes.
constexpr uint64_t kReferenceRawBytes = 24117706752ull;
constexpr uint64_t kReferenceFrameCount = 368007ull;

// Frames read sequentially from the head. Deliberately a bounded prefix, not
// the whole capture.
constexpr uint64_t kPrefixFrames = 8;

// Frames read from the end of the capture, far enough back to cover the
// stream's last few decoded blocks.
constexpr uint64_t kTailFrames = 8;

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

std::unique_ptr<IVBIByteSource> open_reference(std::string& error) {
  return open_vbi_byte_source(kReferenceCapture, error);
}

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

TEST(VBIFlacTransport, ReferenceCaptureDecodesToTheDocumentedLength) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  ASSERT_TRUE(source->size_bytes().has_value());
  EXPECT_EQ(*source->size_bytes(), kReferenceRawBytes);

  ASSERT_TRUE(source->declared_bits_per_sample().has_value());
  EXPECT_EQ(*source->declared_bits_per_sample(), 8u);
}

// The container descriptor is a property of the capture, never of the
// wrapper: the FLAC header's 48000 Hz placeholder must not reach it.
TEST(VBIFlacTransport, ContainerDescriptorIsUnaffectedByTheWrapper) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  EXPECT_DOUBLE_EQ(format.sample_rate_hz, 35468950.0);

  VBITransportHints hints;
  hints.bits_per_sample = source->declared_bits_per_sample();

  const std::vector<std::string> errors =
      validate_vbi_source_config(format, source->size_bytes(), hints);
  for (const std::string& message : errors) {
    ADD_FAILURE() << message;
  }
}

TEST(VBIFlacTransport, ReferenceCaptureFactorisesIntoWholeFrames) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  VBILineReader reader(bt8x8_pal_format(), *source);
  ASSERT_TRUE(reader.frame_count().has_value());
  EXPECT_EQ(*reader.frame_count(), kReferenceFrameCount);
  EXPECT_FALSE(reader.has_partial_trailing_frame());
}

// The bt8x8 frame counter sits in the last four bytes of every 65 536-byte
// frame. A monotonic sequence across the decoded prefix is strong evidence
// that the unwrapped byte stream is byte-identical to the raw capture: any
// offset or sample-width error in the transport turns those bytes into noise.
// The step is not asserted to be exactly one, because a genuine source frame
// drop is exactly what this counter exists to reveal (design §6.3).
TEST(VBIFlacTransport, DecodedPrefixYieldsWellFormedFramesAndCounters) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  VBILineReader reader(format, *source);

  std::optional<uint32_t> first_counter;
  std::optional<uint32_t> previous_counter;
  for (uint64_t frame = 0; frame < kPrefixFrames; ++frame) {
    VBIFrameRecords records;
    ASSERT_TRUE(reader.read_frame(frame, records, error))
        << "frame " << frame << ": " << error;

    ASSERT_EQ(records.lines.size(), 32u) << "frame " << frame;
    for (const VBILineRecord& line : records.lines) {
      ASSERT_EQ(line.samples.size(), 2044u);
      ASSERT_EQ(line.source_byte_length, 2044u);
    }

    ASSERT_TRUE(records.frame_counter.has_value()) << "frame " << frame;
    if (previous_counter.has_value()) {
      EXPECT_GT(*records.frame_counter, *previous_counter)
          << "frame counter did not advance at frame " << frame;
    } else {
      first_counter = records.frame_counter;
    }
    previous_counter = records.frame_counter;
  }

  // Over a short prefix the counter should track the frame index closely; a
  // wildly larger advance would mean the trailer is not a counter at all.
  ASSERT_TRUE(first_counter.has_value());
  ASSERT_TRUE(previous_counter.has_value());
  const uint64_t advance = static_cast<uint64_t>(*previous_counter) -
                           static_cast<uint64_t>(*first_counter);
  EXPECT_GE(advance, kPrefixFrames - 1);
  EXPECT_LT(advance, kPrefixFrames * 4);
}

// Random access must use the stream's seek points rather than re-decoding
// from the head: the capture is 10 GB compressed, so a from-head decode of a
// late frame is not a viable access path.
TEST(VBIFlacTransport, RandomAccessSeeksRatherThanDecodingFromTheHead) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  VBILineReader reader(format, *source);

  // A frame well into the capture, then one well before it: the backward
  // access is the one that would betray a rewind-and-replay implementation.
  constexpr uint64_t kLateFrame = 300000;
  constexpr uint64_t kEarlierFrame = 100000;

  VBIFrameRecords records;
  ASSERT_TRUE(reader.read_frame(kLateFrame, records, error)) << error;
  const uint64_t after_late = source->bytes_decoded();
  EXPECT_LT(after_late, kLateFrame * format.bytes_per_frame());

  ASSERT_TRUE(reader.read_frame(kEarlierFrame, records, error)) << error;
  const uint64_t backward_cost = source->bytes_decoded() - after_late;

  // A rewind would decode the whole 6.5 GB run-up to the earlier frame; a
  // seek costs a few blocks.
  EXPECT_LT(backward_cost, 64ull * 1024 * 1024)
      << "backward access decoded " << backward_cost << " bytes";
}

// The demuxer bisects the stream to find a seek point, and a probe that lands
// where a frame header cannot be parsed makes it refuse the seek outright.
// The reference capture has such a target among its last frames, so the tail
// is exactly where a transport that treats a refused seek as fatal breaks --
// and the tail is read by anything that walks the capture to its end.
TEST(VBIFlacTransport, FramesAtTheTailOfTheCaptureAreReachable) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  const VBISourceFormat format = bt8x8_pal_format();
  VBILineReader reader(format, *source);
  ASSERT_TRUE(reader.frame_count().has_value());
  const uint64_t frame_count = *reader.frame_count();
  ASSERT_GT(frame_count, kTailFrames);

  // Descending, so every frame is reached by a seek rather than by decoding
  // on from the previous one.
  for (uint64_t offset = 1; offset <= kTailFrames; ++offset) {
    const uint64_t frame = frame_count - offset;

    VBIFrameRecords records;
    ASSERT_TRUE(reader.read_frame(frame, records, error))
        << "frame " << frame << ": " << error;
    EXPECT_EQ(records.lines.size(), 32u) << "frame " << frame;
    EXPECT_TRUE(records.frame_counter.has_value()) << "frame " << frame;
  }
}

// Seeking must be exact rather than approximate: the same frame reached from
// two different stream positions has to yield the same bytes. This is
// content-independent, so it holds regardless of what the capture contains.
TEST(VBIFlacTransport, SeekedFramesAreReproducibleFromAnyStreamPosition) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  std::string error;
  std::unique_ptr<IVBIByteSource> source = open_reference(error);
  ASSERT_NE(source, nullptr) << error;

  VBILineReader reader(bt8x8_pal_format(), *source);

  for (const uint64_t frame : {1000ull, 150000ull, 367000ull}) {
    VBIFrameRecords forward;
    ASSERT_TRUE(reader.read_frame(frame, forward, error))
        << "frame " << frame << ": " << error;

    // Reposition somewhere unrelated, then come back.
    VBIFrameRecords elsewhere;
    ASSERT_TRUE(reader.read_frame(200000, elsewhere, error)) << error;

    VBIFrameRecords again;
    ASSERT_TRUE(reader.read_frame(frame, again, error))
        << "frame " << frame << " (revisited): " << error;

    EXPECT_EQ(again.frame_counter, forward.frame_counter) << "frame " << frame;
    ASSERT_EQ(again.lines.size(), forward.lines.size());
    for (size_t index = 0; index < again.lines.size(); ++index) {
      EXPECT_EQ(again.lines[index].samples, forward.lines[index].samples)
          << "frame " << frame << ", record " << index;
    }
  }
}

}  // namespace
}  // namespace orc
