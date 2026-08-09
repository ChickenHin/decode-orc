/*
 * File:        vbi_transport_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for raw VBI capture transport detection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_transport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace orc {
namespace {

TEST(VBITransport, FlacStreamMarkerIsDetectedAtOffsetZero) {
  const std::vector<uint8_t> flac_header = {'f',  'L',  'a',  'C',
                                            0x00, 0x00, 0x00, 0x22};

  EXPECT_TRUE(has_flac_stream_marker(flac_header.data(), flac_header.size()));
}

TEST(VBITransport, RawCaptureBytesAreNotMistakenForFlac) {
  // A bt8x8 record opens with back porch samples, not a magic number.
  const std::vector<uint8_t> raw_header = {0x80, 0x7F, 0x81, 0x80,
                                           0x7E, 0x82, 0x80, 0x80};

  EXPECT_FALSE(has_flac_stream_marker(raw_header.data(), raw_header.size()));
}

TEST(VBITransport, TruncatedOrAbsentHeaderIsNotFlac) {
  const std::vector<uint8_t> truncated = {'f', 'L', 'a'};

  EXPECT_FALSE(has_flac_stream_marker(truncated.data(), truncated.size()));
  EXPECT_FALSE(has_flac_stream_marker(nullptr, 0));
}

// The marker only counts at offset 0; the same bytes occurring inside sample
// data must not trigger unwrapping.
TEST(VBITransport, MarkerMustBeTheFirstBytesOfTheStream) {
  const std::vector<uint8_t> shifted = {0x80, 'f', 'L', 'a', 'C', 0x00};

  EXPECT_FALSE(has_flac_stream_marker(shifted.data(), shifted.size()));
}

}  // namespace
}  // namespace orc
