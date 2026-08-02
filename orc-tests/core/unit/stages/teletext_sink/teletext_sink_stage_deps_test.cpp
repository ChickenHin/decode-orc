/*
 * File:        teletext_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the teletext sink stage dependencies
 *
 * Covers: PAL gating, packet bytes and temporal ordering, keep-empty padding,
 * per-frame coverage skip, cancel, progress throttling, writer failure paths,
 * and the direct-slicing path used for non-default slicer options. All frame
 * data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage_deps.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/observation_service_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"
#include "../../support/teletext_line_synthesizer.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

namespace {

orc::SourceParameters make_pal_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  p.frame_height = orc::kPalFrameLines;
  p.black_level = orc::kPalBlack;
  p.white_level = orc::kPalWhite;
  return p;
}

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::NTSC;
  return p;
}

// A distinguishable MRAG-valid payload per index.
std::array<uint8_t, orc::kTeletextPacketBytes> make_payload(uint8_t variant) {
  auto payload = orc::tests::make_test_payload();
  payload[2] = variant;
  return payload;
}

// X/0 page header for transmission magazine 0 (displayed magazine 8,
// page 8<page_number:2x>), 32 space header characters.
std::array<uint8_t, orc::kTeletextPacketBytes> make_header_packet(
    int page_number, bool subtitle, bool erase) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] =
      orc::teletext_hamming84_encode(static_cast<uint8_t>(page_number & 0xF));
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));
  packet[4] = orc::teletext_hamming84_encode(0);
  packet[5] = orc::teletext_hamming84_encode(erase ? 0x8 : 0x0);  // C4
  packet[6] = orc::teletext_hamming84_encode(0);
  packet[7] = orc::teletext_hamming84_encode(subtitle ? 0x8 : 0x0);  // C6
  packet[8] = orc::teletext_hamming84_encode(0);
  packet[9] = orc::teletext_hamming84_encode(0);
  for (size_t i = 0; i < 32; ++i) {
    packet[10 + i] = orc::teletext_odd_parity_encode(' ');
  }
  return packet;
}

// Displayable row for transmission magazine 0 with boxed subtitle text
// (Start Box ×2 ... End Box, the C5/C6 convention).
std::array<uint8_t, orc::kTeletextPacketBytes> make_subtitle_row_packet(
    int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  std::string bytes;
  bytes.push_back(0x0B);
  bytes.push_back(0x0B);
  bytes += text;
  bytes.push_back(0x0A);
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < bytes.size() ? bytes[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// Displayable row for transmission magazine 0 carrying plain text.
std::array<uint8_t, orc::kTeletextPacketBytes> make_row_packet(
    int row, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  const auto mrag = orc::tests::make_mrag(0, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

}  // namespace

class TeletextSinkStageDeps : public ::testing::Test {
 public:
  void SetUp() override {
    pMockFileWriterUint8_ = std::make_shared<StrictMock<MockFileWriterUint8>>();
    cancelRequested_.store(false);
  }

 protected:
  // Wire the writer factory and capture every byte written, in order.
  void expect_writer(const std::string& expected_path) {
    EXPECT_CALL(mockStageServices_,
                create_buffered_file_writer_uint8(1UL * 1024 * 1024))
        .Times(1)
        .WillOnce(Return(pMockFileWriterUint8_));
    EXPECT_CALL(*pMockFileWriterUint8_, open(expected_path))
        .Times(1)
        .WillOnce(Return(true));
    EXPECT_CALL(*pMockFileWriterUint8_, write(_, _))
        .WillRepeatedly(Invoke([this](const uint8_t* data, size_t count) {
          written_.insert(written_.end(), data, data + count);
        }));
    EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  }

  // Hand a mock observer handle to the deps and return its raw pointer for
  // expectations.
  MockObserverHandle* expect_observer_created() {
    auto handle = std::make_unique<StrictMock<MockObserverHandle>>();
    MockObserverHandle* raw = handle.get();
    EXPECT_CALL(mockObservationService_, create_observer("teletext"))
        .Times(1)
        .WillOnce(Return(ByMove(std::move(handle))));
    return raw;
  }

  orc::TeletextSinkStageDeps make_deps(bool with_observation_service = true) {
    orc::TeletextSinkStageDeps deps(
        &mockStageServices_,
        with_observation_service ? &mockObservationService_ : nullptr);
    deps.init({}, &cancelRequested_);
    return deps;
  }

  MockStageServices mockStageServices_;
  StrictMock<MockObservationService> mockObservationService_;
  std::shared_ptr<StrictMock<MockFileWriterUint8>> pMockFileWriterUint8_;
  NiceMock<MockObservationContext> mockContext_;
  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;
  std::atomic<bool> cancelRequested_{};
  std::vector<uint8_t> written_;
};

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextSinkStageDeps, ExportT42_FailsWhenInputNotPal) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_ntsc_params()));

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Input is not PAL (teletext sink is PAL WST only)");
}

TEST_F(TeletextSinkStageDeps, ExportT42_FailsWhenInputHasNoFrames) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  // Empty range: last < first → count() == 0
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{1, 0}));

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Input has no frames");
}

TEST_F(TeletextSinkStageDeps, ExportT42_FailsWhenWriterServiceUnavailable) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  orc::TeletextSinkStageDeps deps(nullptr, &mockObservationService_);
  deps.init({}, &cancelRequested_);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Failed to create output writer service");
}

TEST_F(TeletextSinkStageDeps, ExportT42_FailsWhenOpenFails) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(1)
      .WillOnce(Return(pMockFileWriterUint8_));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42"))
      .Times(1)
      .WillOnce(Return(false));

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out";  // extension appended before open()

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Failed to open output file: out.t42");
  EXPECT_EQ(result.output_path, "out.t42");
}

TEST_F(TeletextSinkStageDeps, ExportT42_WritesPacketsInTemporalOrder) {
  const auto payload_a = make_payload(0x11);  // frame 0, field 1, line 7
  const auto payload_b = make_payload(0x22);  // frame 0, field 2, line 8
  const auto payload_c = make_payload(0x33);  // frame 1, field 1, line 8

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));
  EXPECT_CALL(mockRepresentation_, has_frame(_)).WillRepeatedly(Return(true));

  expect_writer("out.t42");

  MockObserverHandle* observer = expect_observer_created();
  EXPECT_CALL(*observer, process_frame(_, orc::FrameID(0), _)).Times(1);
  EXPECT_CALL(*observer, process_frame(_, orc::FrameID(1), _)).Times(1);

  EXPECT_CALL(mockContext_, has(_, "teletext", "present"))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(payload_a))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(1), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(payload_b))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(payload_c))));

  // Memory hygiene: both fields of each processed frame are cleared.
  for (const uint64_t field : {0u, 1u, 2u, 3u}) {
    EXPECT_CALL(mockContext_, clear_field(orc::FieldID(field))).Times(1);
  }

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out";  // .t42 appended
  options.first_field_line = 7;
  options.last_field_line = 8;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.output_path, "out.t42");
  EXPECT_EQ(result.packets_written, 3u);
  EXPECT_EQ(result.fields_with_data, 3u);

  // Strictly temporal: frame → field (1 then 2) → ascending line.
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), payload_a.begin(), payload_a.end());
  expected.insert(expected.end(), payload_b.begin(), payload_b.end());
  expected.insert(expected.end(), payload_c.begin(), payload_c.end());
  EXPECT_EQ(written_, expected);
}

TEST_F(TeletextSinkStageDeps, ExportT42_KeepEmptyPacketsPadsAllCandidateLines) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));

  expect_writer("out.t42");

  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));

  // Null observation service: data comes from the context only (none here).
  auto deps = make_deps(/*with_observation_service=*/false);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 5;
  options.last_field_line = 21;
  options.keep_empty_packets = true;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_TRUE(result.success);
  // Exactly (last − first + 1) × 2 fields × 2 frames zero packets.
  const size_t expected_packets = 17u * 2u * 2u;
  EXPECT_EQ(result.packets_written, expected_packets);
  EXPECT_EQ(result.fields_with_data, 0u);
  ASSERT_EQ(written_.size(), expected_packets * orc::kTeletextPacketBytes);
  for (const uint8_t byte : written_) {
    ASSERT_EQ(byte, 0u);
  }
}

// Teletext loops, so a recording holds several copies of every row damaged in
// different places. The export combines them and writes the combined form
// (orc/support/teletext_row_squasher.h, after ali1234/vhs-teletext).
TEST_F(TeletextSinkStageDeps, ExportT42_SquashingRepairsADamagedRowByte) {
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  const auto clean = make_row_packet(1, "HELLO");
  auto damaged = clean;
  damaged[2] ^= 0x01;  // break odd parity on the leading display byte
  ASSERT_FALSE(orc::teletext_odd_parity_valid(damaged[2]));

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));

  expect_writer("out.t42");

  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  // Frame 0 carries the damaged copy, frame 1 the clean retransmission.
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(header))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(damaged))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(header))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(clean))));

  auto deps = make_deps(/*with_observation_service=*/false);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 7;
  options.last_field_line = 8;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.packets_written, 4u);
  EXPECT_EQ(result.packets_corrected, 1u);

  // Packet order, count and timing are unchanged; only the damaged display
  // byte moved, and the headers passed through untouched.
  std::vector<uint8_t> expected;
  expected.insert(expected.end(), header.begin(), header.end());
  expected.insert(expected.end(), clean.begin(), clean.end());
  expected.insert(expected.end(), header.begin(), header.end());
  expected.insert(expected.end(), clean.begin(), clean.end());
  EXPECT_EQ(written_, expected);
}

TEST_F(TeletextSinkStageDeps,
       ExportT42_SquashingOffLeavesTheStreamAsRecovered) {
  const auto header = make_header_packet(0x00, /*subtitle=*/false,
                                         /*erase=*/false);
  const auto clean = make_row_packet(1, "HELLO");
  auto damaged = clean;
  damaged[2] ^= 0x01;

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 1}));

  expect_writer("out.t42");

  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(header))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(damaged))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(header))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_8"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(clean))));

  auto deps = make_deps(/*with_observation_service=*/false);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 7;
  options.last_field_line = 8;
  options.squash_repeated_rows = false;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.packets_corrected, 0u);

  std::vector<uint8_t> expected;
  expected.insert(expected.end(), header.begin(), header.end());
  expected.insert(expected.end(), damaged.begin(), damaged.end());
  expected.insert(expected.end(), header.begin(), header.end());
  expected.insert(expected.end(), clean.begin(), clean.end());
  EXPECT_EQ(written_, expected)
      << "the recovered stream was not passed through";
}

TEST_F(TeletextSinkStageDeps, ExportT42_SkipsObserverForCoveredFrames) {
  const auto payload = make_payload(0x44);

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  expect_writer("out.t42");

  // The frame is covered by pre-loaded observations: the observer session is
  // created but must never process the frame.
  MockObserverHandle* observer = expect_observer_created();
  EXPECT_CALL(*observer, process_frame(_, _, _)).Times(0);

  EXPECT_CALL(mockContext_, has(_, "teletext", "present"))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(payload))));
  EXPECT_CALL(mockContext_, clear_field(_)).Times(2);

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 7;
  options.last_field_line = 7;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.packets_written, 1u);
  EXPECT_EQ(written_, std::vector<uint8_t>(payload.begin(), payload.end()));
}

TEST_F(TeletextSinkStageDeps, ExportT42_CancelAbortsWithTruthfulStatus) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 4}));

  expect_writer("out.t42");

  cancelRequested_.store(true);

  auto deps = make_deps(/*with_observation_service=*/false);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "Cancelled after 0 of 5 frames; partial output left at out.t42");
  EXPECT_EQ(result.packets_written, 0u);
}

TEST_F(TeletextSinkStageDeps, ExportT42_ThrottlesProgressReporting) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 19}));

  expect_writer("out.t42");

  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));

  std::vector<std::pair<uint64_t, uint64_t>> progress;
  orc::TeletextSinkStageDeps deps(&mockStageServices_, nullptr);
  deps.init(
      [&progress](uint64_t current, uint64_t total, const std::string&) {
        progress.emplace_back(current, total);
      },
      &cancelRequested_);

  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 5;
  options.last_field_line = 5;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_TRUE(result.success);
  // Throttled to every 10th frame plus the final frame.
  const std::vector<std::pair<uint64_t, uint64_t>> expected = {{10, 20},
                                                               {20, 20}};
  EXPECT_EQ(progress, expected);
}

TEST_F(TeletextSinkStageDeps, ExportT42_WritesSubtitleSrtWhenEnabled) {
  // A minimal subtitle transmission on 0-based field line 7: header (C6 +
  // C4) in field 0, one boxed row in field 1, terminating time-filling
  // header in field 2.
  const auto header = make_header_packet(0x88, /*subtitle=*/true,
                                         /*erase=*/true);
  const auto row = make_subtitle_row_packet(20, "HELLO");
  const auto terminator =
      make_header_packet(0xFF, /*subtitle=*/false, /*erase=*/false);

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 2}));
  EXPECT_CALL(mockRepresentation_, has_frame(_)).WillRepeatedly(Return(true));

  // Two writers: the T42 packet stream, then the SubRip document.
  auto srt_writer = std::make_shared<StrictMock<MockFileWriterUint8>>();
  std::vector<uint8_t> srt_written;
  EXPECT_CALL(mockStageServices_,
              create_buffered_file_writer_uint8(1UL * 1024 * 1024))
      .Times(2)
      .WillOnce(Return(pMockFileWriterUint8_))
      .WillOnce(Return(srt_writer));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42")).WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, write(_, _))
      .WillRepeatedly(Invoke([this](const uint8_t* data, size_t count) {
        written_.insert(written_.end(), data, data + count);
      }));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  EXPECT_CALL(*srt_writer, open("out.srt")).WillOnce(Return(true));
  EXPECT_CALL(*srt_writer, write(_, _))
      .WillRepeatedly(Invoke([&srt_written](const uint8_t* data, size_t count) {
        srt_written.insert(srt_written.end(), data, data + count);
      }));
  EXPECT_CALL(*srt_writer, close()).Times(1);

  MockObserverHandle* observer = expect_observer_created();
  EXPECT_CALL(*observer, process_frame(_, _, _)).Times(3);

  EXPECT_CALL(mockContext_, has(_, "teletext", "present"))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));
  EXPECT_CALL(mockContext_, get(orc::FieldID(0), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(header))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(1), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(row))));
  EXPECT_CALL(mockContext_, get(orc::FieldID(2), "teletext", "t42_7"))
      .WillRepeatedly(Return(std::optional<orc::ObservationValue>(
          orc::teletext_packet_to_hex(terminator))));
  EXPECT_CALL(mockContext_, clear_field(_)).Times(6);

  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 7;
  options.last_field_line = 7;
  options.export_subtitles = true;
  options.subtitle_page = "888";

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.packets_written, 3u);
  EXPECT_EQ(result.subtitle_path, "out.srt");
  EXPECT_EQ(result.subtitle_cues_written, 1u);
  EXPECT_EQ(result.message,
            "Exported 3 teletext packets (3 fields with data) to out.t42; "
            "1 subtitle cues to out.srt");

  // Cue displayed from the row's field (1 → 20 ms at 50 fields/s) and
  // closed by finalize at the end of the 3-frame range (field 6 → 120 ms).
  const std::string expected_srt =
      "1\n00:00:00,020 --> 00:00:00,120\nHELLO\n\n";
  EXPECT_EQ(std::string(srt_written.begin(), srt_written.end()), expected_srt);
}

TEST_F(TeletextSinkStageDeps,
       ExportT42_ReportsFailureWhenSubtitleWriterCannotOpen) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));

  auto srt_writer = std::make_shared<StrictMock<MockFileWriterUint8>>();
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(2)
      .WillOnce(Return(pMockFileWriterUint8_))
      .WillOnce(Return(srt_writer));
  EXPECT_CALL(*pMockFileWriterUint8_, open("out.t42")).WillOnce(Return(true));
  EXPECT_CALL(*pMockFileWriterUint8_, close()).Times(1);
  EXPECT_CALL(*srt_writer, open("out.srt")).WillOnce(Return(false));

  EXPECT_CALL(mockContext_, get(_, _, _)).WillRepeatedly(Return(std::nullopt));

  auto deps = make_deps(/*with_observation_service=*/false);
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.export_subtitles = true;

  const auto result =
      deps.export_t42(&mockRepresentation_, mockContext_, options);

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message,
            "Exported 0 teletext packets to out.t42 but failed to open "
            "subtitle output: out.srt");
}

TEST_F(TeletextSinkStageDeps, ExportT42_NonDefaultOptionsSliceDirectly) {
  const auto payload = orc::tests::make_test_payload();
  const auto line = orc::tests::synthesize_teletext_line(payload);

  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_pal_params()));
  EXPECT_CALL(mockRepresentation_, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  // Field 1, 0-based field line 7 → frame-flat line 7 carries teletext.
  EXPECT_CALL(mockRepresentation_, get_line(orc::FrameID(0), 7))
      .WillRepeatedly(Return(line.data()));
  // Field 2, frame-flat line 313 + 7 = 320 is empty.
  EXPECT_CALL(mockRepresentation_, get_line(orc::FrameID(0), 320))
      .WillRepeatedly(Return(nullptr));

  expect_writer("out.t42");

  // Non-default slicer options bypass the host observer entirely: the strict
  // observation service and context mocks fail on any interaction.
  auto deps = make_deps();
  orc::TeletextSinkOptions options;
  options.output_path = "out.t42";
  options.first_field_line = 7;
  options.last_field_line = 7;
  options.tolerant_framing = true;

  StrictMock<MockObservationContext> strictContext;
  const auto result =
      deps.export_t42(&mockRepresentation_, strictContext, options);

  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.packets_written, 1u);
  EXPECT_EQ(result.fields_with_data, 1u);
  EXPECT_EQ(written_, std::vector<uint8_t>(payload.begin(), payload.end()));
}

}  // namespace orc_unit_test
