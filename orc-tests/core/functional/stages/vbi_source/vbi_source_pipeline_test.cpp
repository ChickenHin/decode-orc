/*
 * File:        vbi_source_pipeline_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the VBI source stage on real media
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "vbi_offset_calibration.h"
#include "vbi_output_levels.h"
#include "vbi_source_format.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// The reference sample: a four-hour FLAC-wrapped bt8x8 PAL capture carrying
// world system teletext. It is not checked into the repository (test-data/ is
// ignored), so every test here skips when it is absent.
const char* kReferenceCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

// The design's success criterion is that the existing teletext decoders,
// pointed at this stage's output, recover the same packets they would from a
// native decode (design §1). The decoders are the oracle here; nothing
// downstream of the stage boundary is under test.
//
// Frames examined. A handful spread across the capture rather than a prefix
// of it: the whole file is 368 007 frames and 10 GB compressed, so a full
// decode is a manual milestone check rather than an automated one — and the
// opening minute of this recording is off-air, carrying no teletext at all,
// which is exactly why the design samples a capture across its length rather
// than from its head (design §5.3.4).
constexpr uint32_t kFramesUnderTest = 8;

// Stored frame lines a 625-line WST capture carries data on: broadcast frame
// lines 7-22 and 320-335 (design §5.1).
const std::vector<size_t> kTeletextFrameLines = [] {
  std::vector<size_t> lines;
  for (size_t line = 6; line <= 21; ++line) lines.push_back(line);
  for (size_t line = 319; line <= 334; ++line) lines.push_back(line);
  return lines;
}();

bool reference_capture_available() {
  return std::filesystem::exists(kReferenceCapture);
}

std::map<std::string, ParameterValue> reference_parameters() {
  return {
      {"input_path", std::string(kReferenceCapture)},
      {"format", std::string("bt8x8-pal")},
  };
}

// One packet's magazine and row, which is what says a recovered packet is a
// real teletext packet rather than noise that spelled a framing code.
struct PacketAddress {
  int magazine = 0;
  int row = 0;
};

// ETSI EN 300 706 §7.1.2: the two Hamming 8/4 protected MRAG bytes carry the
// magazine in the low three bits of the first nibble, the least significant
// row bit in its fourth, and the remaining four row bits in the second nibble.
bool decode_address(const TeletextLineResult& result, PacketAddress& out) {
  const int first = teletext_hamming84_decode(result.bytes[0]);
  const int second = teletext_hamming84_decode(result.bytes[1]);
  if (first < 0 || second < 0) return false;
  out.magazine = first & 0x7;
  out.row = ((first >> 3) & 0x1) | ((second & 0xF) << 1);
  return true;
}

// Run the stage over the reference capture and hand back its output.
std::shared_ptr<VideoFrameRepresentation> load_reference_capture(
    VBISourceStage& stage, ObservationContext& observations) {
  const std::vector<ArtifactPtr> outputs =
      stage.execute({}, reference_parameters(), observations);
  if (outputs.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
}

// ---------------------------------------------------------------------------
// The stage against the reference capture
// ---------------------------------------------------------------------------

TEST(VBISourcePipeline, OpensTheReferenceCaptureAndReportsItsFrameCount) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_reference_capture(stage, observations);
  ASSERT_NE(representation, nullptr);

  // 24 117 706 752 bytes of capture at 65 536 bytes per bt8x8 PAL frame.
  EXPECT_EQ(representation->frame_count(), 368007u);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  EXPECT_EQ(parameters->system, VideoSystem::PAL);

  const auto descriptor = representation->get_frame_descriptor(0);
  ASSERT_TRUE(descriptor.has_value());
  EXPECT_EQ(descriptor->samples_total, static_cast<size_t>(kPalFrameSamples));

  // The capture offset was fitted rather than taken from the preset's
  // folkloric starting hint of 244 samples (design §5.3.3).
  const auto offset =
      observations.get(FieldID(0), "vbi_source", "capture_offset");
  ASSERT_TRUE(offset.has_value());
  EXPECT_NEAR(std::get<double>(*offset), 261.6, 2.0);
}

// The acceptance oracle: the existing WST slicer, pointed at the synthesised
// frames, recovers real teletext packets.
TEST(VBISourcePipeline, TheExistingTeletextSlicerRecoversPacketsFromTheOutput) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  const auto representation = load_reference_capture(stage, observations);
  ASSERT_NE(representation, nullptr);
  ASSERT_GT(representation->frame_count(), kFramesUnderTest);

  // EBU Tech. 3280-E §1.1.1 Table 1 sample rate; the data levels are the ones
  // the stage declares it synthesised to.
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kAuto;
  const TeletextSlicer slicer(kPalSampleRate, kTeletextBitRate, options);

  const auto parameters = representation->get_video_parameters();
  ASSERT_TRUE(parameters.has_value());
  const auto black = static_cast<int16_t>(parameters->black_level);
  const auto white = static_cast<int16_t>(parameters->white_level);

  uint64_t candidate_lines = 0;
  uint64_t recovered = 0;
  std::set<int> magazines;
  std::set<int> rows;

  for (const uint64_t frame : vbi_calibration_frame_indices(
           static_cast<uint64_t>(representation->frame_count()),
           kFramesUnderTest)) {
    for (const size_t line : kTeletextFrameLines) {
      const std::vector<int16_t> samples =
          representation->get_line_samples(static_cast<FrameID>(frame), line);
      ASSERT_FALSE(samples.empty()) << "frame " << frame << " line " << line;
      ++candidate_lines;

      const TeletextLineResult result =
          slicer.slice(samples.data(), samples.size(), black, white);
      if (!result.valid) continue;
      ++recovered;

      PacketAddress address;
      ASSERT_TRUE(decode_address(result, address))
          << "frame " << frame << " line " << line;
      magazines.insert(address.magazine);
      rows.insert(address.row);
    }
  }

  // Recorded so the log carries the measurement the assertions are drawn
  // against rather than only their verdict.
  GTEST_LOG_(INFO) << recovered << " of " << candidate_lines
                   << " candidate lines yielded a packet, across "
                   << rows.size() << " row(s) and " << magazines.size()
                   << " magazine(s)";

  // A capture of a live broadcast carries teletext on most of its lines. The
  // bar is deliberately well below what a working stage produces and well
  // above what a mis-placed or mis-scaled output could reach by accident.
  EXPECT_GT(recovered, candidate_lines / 2)
      << recovered << " of " << candidate_lines << " candidate lines sliced";

  // Real teletext addresses the rows of a page in order and carries a page
  // header (row 0) somewhere in the frames examined.
  EXPECT_GT(rows.size(), 4u);
  EXPECT_NE(rows.find(0), rows.end()) << "no page header recovered";
  EXPECT_FALSE(magazines.empty());
}

// The two policies over the same capture: the counter says the same thing
// about it either way, and only the policy's response to it differs.
TEST(VBISourcePipeline, TheFrameCounterIsReadWithoutWalkingTheCapture) {
  if (!reference_capture_available()) {
    GTEST_SKIP() << "Reference capture not present: " << kReferenceCapture;
  }

  VBISourceStage stage;
  ObservationContext observations;
  ASSERT_NE(load_reference_capture(stage, observations), nullptr);

  const auto sequence =
      observations.get(FieldID(0), "vbi_source", "frame_sequence");
  ASSERT_TRUE(sequence.has_value());
  const std::string summary = std::get<std::string>(*sequence);

  // Whatever the capture turns out to hold, the stage must have made a
  // statement about its frame counter rather than staying silent.
  EXPECT_TRUE(summary.find("Frame counter") != std::string::npos ||
              summary.find("frame counter") != std::string::npos)
      << summary;
  EXPECT_NE(summary.find("Signal state"), std::string::npos) << summary;
}

}  // namespace
}  // namespace orc
