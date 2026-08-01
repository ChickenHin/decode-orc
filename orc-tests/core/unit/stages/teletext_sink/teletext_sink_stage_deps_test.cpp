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
