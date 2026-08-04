/*
 * File:        vbi_frame_geometry_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the synthesised CVBS frame sample lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_geometry.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

VBIFrameGeometry pal_geometry() {
  VBIFrameGeometry geometry;
  std::string error;
  EXPECT_TRUE(make_vbi_frame_geometry(VBITVSystem::kPAL, geometry, error))
      << error;
  return geometry;
}

// The normative PAL frame is 709 379 samples, which is 625 lines of
// 1135.0064 rather than 625 lines of 1135. A frame four samples short
// desynchronises every subsequent frame in the file.
TEST(VBIFrameGeometry, PALFrameHoldsTheNormativeSampleCount) {
  const VBIFrameGeometry geometry = pal_geometry();

  EXPECT_EQ(geometry.lines_per_frame(), 625u);
  EXPECT_EQ(geometry.samples_per_frame(), 709379u);
  EXPECT_NEAR(geometry.nominal_line_length(), 1135.0064, 1e-9);
  EXPECT_FALSE(geometry.is_orthogonal());

  EXPECT_EQ(geometry.line_start(0), 0u);
  EXPECT_EQ(geometry.line_start(geometry.lines_per_frame()), 709379u);
}

// 621 lines of 1135 samples and four of 1136, the long lines falling at
// frame lines 0, 156, 312 and 468.
TEST(VBIFrameGeometry,
     PALLineLengthsAreFourLongLinesAmongstSixHundredTwentyOne) {
  const VBIFrameGeometry geometry = pal_geometry();

  std::vector<uint32_t> long_lines;
  uint32_t short_line_count = 0;
  uint32_t total_samples = 0;
  for (uint32_t line = 0; line < geometry.lines_per_frame(); ++line) {
    const uint32_t length = geometry.line_length(line);
    total_samples += length;
    if (length == 1136) {
      long_lines.push_back(line);
    } else {
      EXPECT_EQ(length, 1135u) << "frame line " << line;
      ++short_line_count;
    }
  }

  EXPECT_EQ(total_samples, 709379u);
  EXPECT_EQ(short_line_count, 621u);
  EXPECT_EQ(long_lines, (std::vector<uint32_t>{0, 156, 312, 468}));
}

// Line starts follow the 0H rule rather than a constant stride; a naive
// k x 1135 index drifts by up to four samples by the bottom of the frame.
TEST(VBIFrameGeometry, PALLineStartsFollowTheZeroHRuleNotAConstantStride) {
  const VBIFrameGeometry geometry = pal_geometry();

  EXPECT_EQ(geometry.line_start(6), 6811u);
  EXPECT_EQ(geometry.line_start(21), 23836u);
  EXPECT_EQ(geometry.line_start(319), 362068u);
  EXPECT_EQ(geometry.line_start(334), 379093u);

  // The last teletext line of field 2 has drifted three samples clear of the
  // constant-stride answer.
  EXPECT_EQ(geometry.line_start(334) - 334u * 1135u, 3u);

  // Starts are strictly ascending and each is its predecessor's start plus
  // its predecessor's length.
  for (uint32_t line = 0; line < geometry.lines_per_frame(); ++line) {
    EXPECT_EQ(geometry.line_start(line) + geometry.line_length(line),
              geometry.line_start(line + 1))
        << "frame line " << line;
  }
}

// Sample 0 of a stored line is the first sampling instant at or after that
// line's 0H, so every line begins a fraction of a sample late.
TEST(VBIFrameGeometry, PALTeletextLinePhasesMatchTheDesignTable) {
  const VBIFrameGeometry geometry = pal_geometry();

  EXPECT_NEAR(geometry.line_phase(6), 0.962, 0.0005);
  EXPECT_NEAR(geometry.line_phase(21), 0.866, 0.0005);
  EXPECT_NEAR(geometry.line_phase(319), 0.958, 0.0005);
  EXPECT_NEAR(geometry.line_phase(334), 0.862, 0.0005);

  EXPECT_DOUBLE_EQ(geometry.line_phase(0), 0.0);
}

TEST(VBIFrameGeometry, PALLinePhasesStayWithinOneSample) {
  const VBIFrameGeometry geometry = pal_geometry();

  for (uint32_t line = 0; line < geometry.lines_per_frame(); ++line) {
    const double phase = geometry.line_phase(line);
    EXPECT_GE(phase, 0.0) << "frame line " << line;
    EXPECT_LT(phase, 1.0) << "frame line " << line;
  }
}

// The same arithmetic degenerates to a constant stride whenever the frame
// divides exactly, so an orthogonal system is a data entry rather than a
// second code path.
TEST(VBIFrameGeometry, AnExactlyDividingLatticeIsOrthogonalAndPhaseFree) {
  // 525 lines of 910 samples, the lattice of a 525-line system.
  const VBIFrameGeometry geometry(525, 477750);

  EXPECT_TRUE(geometry.is_orthogonal());
  EXPECT_DOUBLE_EQ(geometry.nominal_line_length(), 910.0);
  for (uint32_t line = 0; line < geometry.lines_per_frame(); ++line) {
    EXPECT_EQ(geometry.line_start(line), line * 910u);
    EXPECT_EQ(geometry.line_length(line), 910u);
    EXPECT_DOUBLE_EQ(geometry.line_phase(line), 0.0);
  }
}

// Systems whose synthesis does not exist are refused rather than given a
// geometry that implies they are supported.
TEST(VBIFrameGeometry, FiveHundredTwentyFiveLineSystemsAreNotYetAvailable) {
  VBIFrameGeometry geometry;
  std::string error;

  EXPECT_FALSE(make_vbi_frame_geometry(VBITVSystem::kNTSC, geometry, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(geometry.samples_per_frame(), 0u);

  error.clear();
  EXPECT_FALSE(make_vbi_frame_geometry(VBITVSystem::kPALM, geometry, error));
  EXPECT_FALSE(error.empty());
}

// A default-constructed lattice reports nothing rather than dividing by zero.
TEST(VBIFrameGeometry, AnEmptyLatticeIsInert) {
  const VBIFrameGeometry geometry;

  EXPECT_EQ(geometry.lines_per_frame(), 0u);
  EXPECT_EQ(geometry.samples_per_frame(), 0u);
  EXPECT_DOUBLE_EQ(geometry.nominal_line_length(), 0.0);
  EXPECT_FALSE(geometry.is_orthogonal());
  EXPECT_EQ(geometry.line_start(0), 0u);
  EXPECT_EQ(geometry.line_length(0), 0u);
  EXPECT_DOUBLE_EQ(geometry.line_phase(0), 0.0);
}

}  // namespace
}  // namespace orc
