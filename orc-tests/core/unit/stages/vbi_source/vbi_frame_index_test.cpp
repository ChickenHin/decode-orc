/*
 * File:        vbi_frame_index_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the lazy output-to-stored frame mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_frame_index.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_source_format.h"

namespace orc {
namespace {

// A capture's counters, without a capture.  Reads are counted so the tests can
// assert that resolving a frame does not walk the file.
class FakeCounterSource : public IVBIFrameCounterSource {
 public:
  explicit FakeCounterSource(std::vector<uint32_t> counters)
      : counters_(std::move(counters)) {}

  // A source whose format declares a counter but whose frames carry none.
  FakeCounterSource() = default;

  bool frame_counter(uint64_t stored_frame_index,
                     std::optional<uint32_t>& out_counter,
                     std::string& error_message) const override {
    ++reads;
    if (counters_.empty()) {
      out_counter.reset();
      return true;
    }
    if (stored_frame_index >= counters_.size()) {
      error_message = "Frame " + std::to_string(stored_frame_index) +
                      " is beyond the capture.";
      return false;
    }
    out_counter = counters_[static_cast<size_t>(stored_frame_index)];
    return true;
  }

  mutable uint64_t reads = 0;

 private:
  std::vector<uint32_t> counters_;
};

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

// Counters for a capture that dropped nothing.
std::vector<uint32_t> continuous_counters(uint32_t first, uint32_t count) {
  std::vector<uint32_t> counters;
  counters.reserve(count);
  for (uint32_t index = 0; index < count; ++index) {
    counters.push_back(first + index);
  }
  return counters;
}

VBIFrameIndex build_index(const VBISourceFormat& format,
                          const VBIFrameSequenceConfig& config,
                          uint64_t stored_frames,
                          const IVBIFrameCounterSource& counters) {
  VBIFrameIndex index;
  std::string error;
  EXPECT_TRUE(VBIFrameIndex::build(format, config, stored_frames, counters,
                                   index, error))
      << error;
  return index;
}

// ---------------------------------------------------------------------------
// A capture that dropped nothing
// ---------------------------------------------------------------------------

TEST(VBIFrameIndex, ContinuousCounterMapsOutputFramesToStoredFramesDirectly) {
  const FakeCounterSource counters(continuous_counters(399598u, 100u));
  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), VBIFrameSequenceConfig{}, 100u, counters);

  EXPECT_EQ(index.output_frame_count(), 100u);
  EXPECT_EQ(index.dropped_frame_count(), 0u);
  EXPECT_FALSE(index.timeline_broken());

  VBIOutputFramePlan plan;
  std::string error;
  ASSERT_TRUE(index.frame_plan(42, plan, error)) << error;
  EXPECT_EQ(plan.source_frame_index, 42u);
  EXPECT_FALSE(plan.padding);
}

// The whole point of the index: establishing the timeline costs two counter
// reads whatever the capture's length, and resolving a frame of a capture that
// dropped nothing costs none at all.
TEST(VBIFrameIndex, EstablishesTheTimelineFromTheCapturesEndpointsAlone) {
  const FakeCounterSource counters(continuous_counters(0u, 4096u));
  const VBIFrameIndex index = build_index(
      bt8x8_pal_format(), VBIFrameSequenceConfig{}, 4096u, counters);

  EXPECT_EQ(counters.reads, 2u);

  VBIOutputFramePlan plan;
  std::string error;
  for (const uint64_t frame : {uint64_t{0}, uint64_t{2000}, uint64_t{4095}}) {
    ASSERT_TRUE(index.frame_plan(frame, plan, error)) << error;
    EXPECT_EQ(plan.source_frame_index, frame);
  }
  EXPECT_EQ(counters.reads, 2u);
}

TEST(VBIFrameIndex, RejectsAnOutputFrameBeyondTheCapture) {
  const FakeCounterSource counters(continuous_counters(0u, 8u));
  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), VBIFrameSequenceConfig{}, 8u, counters);

  VBIOutputFramePlan plan;
  std::string error;
  EXPECT_FALSE(index.frame_plan(8, plan, error));
  EXPECT_NE(error.find("beyond"), std::string::npos) << error;
}

// A counter that wraps its unsigned 32-bit range advances by one across the
// wrap like anywhere else.
TEST(VBIFrameIndex, CounterWrapIsNotReadAsADrop) {
  std::vector<uint32_t> counters;
  for (uint32_t index = 0; index < 8u; ++index) {
    counters.push_back(0xFFFFFFFCu + index);
  }

  const FakeCounterSource source(counters);
  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), VBIFrameSequenceConfig{}, 8u, source);

  EXPECT_EQ(index.dropped_frame_count(), 0u);
  EXPECT_EQ(index.output_frame_count(), 8u);
  EXPECT_FALSE(index.timeline_broken());
}

// ---------------------------------------------------------------------------
// A capture that dropped frames
// ---------------------------------------------------------------------------

TEST(VBIFrameIndex, PreserveEmitsOnlyTheFramesPresentAndReportsTheBreak) {
  // Three frames never reached the file between stored frames 3 and 4.
  std::vector<uint32_t> counters = {10u, 11u, 12u, 13u, 17u, 18u};

  const FakeCounterSource source(counters);
  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPreserve;

  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), config, counters.size(), source);

  EXPECT_EQ(index.dropped_frame_count(), 3u);
  EXPECT_EQ(index.output_frame_count(), counters.size());
  EXPECT_TRUE(index.timeline_broken());

  VBIOutputFramePlan plan;
  std::string error;
  ASSERT_TRUE(index.frame_plan(4, plan, error)) << error;
  EXPECT_EQ(plan.source_frame_index, 4u);
  EXPECT_FALSE(plan.padding);
}

TEST(VBIFrameIndex, PadKeepsOutputFramesAlignedWithTheSourcesOwnNumbering) {
  std::vector<uint32_t> counters = {10u, 11u, 12u, 13u, 17u, 18u};

  const FakeCounterSource source(counters);
  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPad;

  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), config, counters.size(), source);

  EXPECT_EQ(index.dropped_frame_count(), 3u);
  EXPECT_EQ(index.output_frame_count(), counters.size() + 3u);
  EXPECT_FALSE(index.timeline_broken());

  // Output frame n carries the frame whose counter is n frames after the
  // first, and the gap becomes padding rather than a shift.
  const std::vector<std::pair<uint64_t, uint64_t>> stored_at = {
      {0, 0}, {1, 1}, {2, 2}, {3, 3}, {7, 4}, {8, 5}};
  VBIOutputFramePlan plan;
  std::string error;
  for (const auto& [output_frame, stored_frame] : stored_at) {
    ASSERT_TRUE(index.frame_plan(output_frame, plan, error)) << error;
    EXPECT_FALSE(plan.padding) << "output frame " << output_frame;
    EXPECT_EQ(plan.source_frame_index, stored_frame);
  }

  for (const uint64_t padded : {uint64_t{4}, uint64_t{5}, uint64_t{6}}) {
    ASSERT_TRUE(index.frame_plan(padded, plan, error)) << error;
    EXPECT_TRUE(plan.padding) << "output frame " << padded;
    // A padded frame holds the index of the frame that follows the gap, so
    // the gap's position stays visible.
    EXPECT_EQ(plan.source_frame_index, 4u);
  }
}

// Locating a gap is a bisection over the range the drop total bounds, not a
// walk of the capture.
TEST(VBIFrameIndex, LocatingAGapReadsFarFewerCountersThanThereAreFrames) {
  std::vector<uint32_t> counters;
  for (uint32_t index = 0; index < 1000u; ++index) {
    // One frame lost at stored frame 500.
    counters.push_back(index + ((index >= 500u) ? 1u : 0u));
  }

  const FakeCounterSource source(counters);
  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPad;

  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), config, counters.size(), source);
  EXPECT_EQ(index.dropped_frame_count(), 1u);
  EXPECT_EQ(index.output_frame_count(), 1001u);

  const uint64_t reads_after_build = source.reads;

  VBIOutputFramePlan plan;
  std::string error;
  ASSERT_TRUE(index.frame_plan(500, plan, error)) << error;
  EXPECT_TRUE(plan.padding);

  ASSERT_TRUE(index.frame_plan(501, plan, error)) << error;
  EXPECT_FALSE(plan.padding);
  EXPECT_EQ(plan.source_frame_index, 500u);

  EXPECT_LT(source.reads - reads_after_build, 16u);
}

TEST(VBIFrameIndex, RefusesToPadAGapLargerThanThePolicyAllows) {
  const FakeCounterSource source({0u, 1u, 100000u});

  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPad;
  config.maximum_pad_frames = 4096;

  VBIFrameIndex index;
  std::string error;
  EXPECT_FALSE(VBIFrameIndex::build(bt8x8_pal_format(), config, 3u, source,
                                    index, error));
  EXPECT_NE(error.find("corrupt"), std::string::npos) << error;
}

// A counter that repeats or runs backwards is a break no padding can undo.
TEST(VBIFrameIndex, ACounterThatDoesNotAdvanceBreaksTheTimeline) {
  const FakeCounterSource source({50u, 51u, 52u, 50u});

  VBIFrameSequenceConfig config;
  config.drops = VBIDropPolicy::kPad;

  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), config, 4u, source);

  EXPECT_TRUE(index.timeline_broken());
  EXPECT_EQ(index.output_frame_count(), 4u);
}

// ---------------------------------------------------------------------------
// What the summary says
// ---------------------------------------------------------------------------

// A format with no counter cannot report drops, and the summary must say that
// rather than implying continuity (design §6.3).
TEST(VBIFrameIndex, ASourceWithoutACounterSaysDropsCannotBeDetected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.frame_trailer_is_counter = false;
  format.frame_trailer_bytes = 0;

  const FakeCounterSource source;
  const VBIFrameIndex index =
      build_index(format, VBIFrameSequenceConfig{}, 32u, source);

  EXPECT_FALSE(index.counter_available());
  EXPECT_EQ(index.output_frame_count(), 32u);
  EXPECT_EQ(index.dropped_frame_count(), 0u);
  EXPECT_NE(index.summary().find("no frame counter"), std::string::npos)
      << index.summary();
  EXPECT_EQ(source.reads, 0u);
}

TEST(VBIFrameIndex, SummaryRecordsTheFieldOrderAssumptionAndTheCounter) {
  const FakeCounterSource source(continuous_counters(0u, 4u));
  const VBIFrameIndex index =
      build_index(bt8x8_pal_format(), VBIFrameSequenceConfig{}, 4u, source);

  const std::string summary = index.summary();
  EXPECT_NE(summary.find("television field 1"), std::string::npos) << summary;
  EXPECT_NE(summary.find("follows the capture's own"), std::string::npos)
      << summary;
  EXPECT_NE(summary.find("continuous"), std::string::npos) << summary;
}

}  // namespace
}  // namespace orc
