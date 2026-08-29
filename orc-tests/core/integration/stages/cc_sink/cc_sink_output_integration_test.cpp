/*
 * File:        cc_sink_output_integration_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Integration test proving CCSinkStageDeps drives the host
 *              observation service and produces real closed-caption output.
 *
 * Unit tests may not touch the file system (see TESTING.md), and the CC sink
 * writes its output file directly through std::ofstream with no writer seam,
 * so end-to-end output coverage lives here in the integration tier where real
 * files are permitted.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cc_sink_stage_deps.h"
#include "mock_video_frame_representation.h"
#include "observation_service_interface_mock.h"

namespace orc_unit_test {
namespace {
using testing::_;
using testing::ByMove;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;

constexpr orc::FrameID kFrameCount = 4;

// Stand-in for the host "closed_caption" observer: writes a decodable EIA-608
// pair onto field 0 of every processed frame (data0 = 0x14 caption control
// code, data1 = a printable byte), mirroring what ClosedCaptionObserver would
// deposit for a frame that carries captions.
void write_caption_observations(const orc::VideoFrameRepresentation& /*rep*/,
                                orc::FrameID frame_id,
                                orc::IObservationContext& context) {
  const orc::FieldID field0(frame_id * 2);
  context.set(field0, "closed_caption", "present", true);
  context.set(field0, "closed_caption", "data0", static_cast<int32_t>(0x14));
  context.set(field0, "closed_caption", "data1", static_cast<int32_t>(0x2C));
  context.set(field0, "closed_caption", "parity0_valid", true);
  context.set(field0, "closed_caption", "parity1_valid", true);

  // Field 1 carries no NTSC captions.
  const orc::FieldID field1(frame_id * 2 + 1);
  context.set(field1, "closed_caption", "present", false);
}

// One byte pair of a scripted line 21 stream.
struct ScriptedPair {
  int32_t data0;
  int32_t data1;
};

// A field 1 stream carrying a CC1 pop-on caption and a TEXT2 page at the same
// time, the two alternating pair for pair — the shape of the recording issue
// #273 was reported against. One frame per pair.
//
// CC1 says "HI", TEXT2 says "42".
const std::vector<ScriptedPair>& interleaved_script() {
  static const std::vector<ScriptedPair> script = {
      {0x14, 0x20},  // CC1: Resume Caption Loading (pop-on)
      {0x14, 0x50},  // CC1: PAC, row 14 column 0
      {0x1C, 0x2A},  // TEXT2: Text Restart
      {'4', '2'},    // TEXT2 characters
      {0x14, 0x50},  // back to CC1 (its PAC again)
      {'H', 'I'},    // CC1 characters
      {0x14, 0x2F},  // CC1: End of Caption - put it up
      {0x14, 0x2C},  // CC1: Erase Displayed Memory - take it down again
  };
  return script;
}

// Observer stand-in that plays interleaved_script() out, one pair per frame.
void write_interleaved_observations(
    const orc::VideoFrameRepresentation& /*rep*/, orc::FrameID frame_id,
    orc::IObservationContext& context) {
  const orc::FieldID field0(frame_id * 2);
  const orc::FieldID field1(frame_id * 2 + 1);
  context.set(field1, "closed_caption", "present", false);

  const auto& script = interleaved_script();
  if (frame_id >= script.size()) {
    context.set(field0, "closed_caption", "present", false);
    return;
  }

  context.set(field0, "closed_caption", "present", true);
  context.set(field0, "closed_caption", "data0", script[frame_id].data0);
  context.set(field0, "closed_caption", "data1", script[frame_id].data1);
  context.set(field0, "closed_caption", "parity0_valid", true);
  context.set(field0, "closed_caption", "parity1_valid", true);
}

// RAII cleanup for a temporary output file.
struct ScopedTempFile {
  std::filesystem::path path;
  explicit ScopedTempFile(const std::string& name)
      : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove(path);
  }
  ~ScopedTempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  std::string str() const { return path.string(); }
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

void configure_ntsc_frames(NiceMock<MockVideoFrameRepresentation>& vfr,
                           orc::FrameID frame_count = kFrameCount) {
  ON_CALL(vfr, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0, frame_count - 1}));
  ON_CALL(vfr, has_frame(_)).WillByDefault(Return(true));
  orc::FrameDescriptor desc;
  desc.system = orc::VideoSystem::NTSC;
  ON_CALL(vfr, get_frame_descriptor(_))
      .WillByDefault(Return(std::optional<orc::FrameDescriptor>(desc)));
}
}  // namespace

TEST(CCSinkOutputIntegrationTest, Scc_ProducesCaptionBytesAndReusesOneSession) {
  NiceMock<MockObservationService> service;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  // The observer session must be created exactly once and reused for every
  // frame (the field-pairing contract Phase 2 called for).
  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  auto* handle_ptr = handle.get();
  EXPECT_CALL(*handle_ptr, process_frame(_, _, _))
      .Times(static_cast<int>(kFrameCount))
      .WillRepeatedly(Invoke(&write_caption_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service, create_observer("closed_caption"))
      .Times(1)
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_scc_it.scc");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::SCC;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.cc_frames_exported, static_cast<int32_t>(kFrameCount));

  const std::string contents = read_file(out.path);
  EXPECT_THAT(contents, testing::HasSubstr("Scenarist_SCC V1.0"));
  // 0x14 -> odd-parity 0x94, 0x2C -> odd-parity 0x2C.
  EXPECT_THAT(contents, testing::HasSubstr("942c"));
}

TEST(CCSinkOutputIntegrationTest, PlainText_RunsObserverPerFrameAndWritesFile) {
  NiceMock<MockObservationService> service;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  auto* handle_ptr = handle.get();
  EXPECT_CALL(*handle_ptr, process_frame(_, _, _))
      .Times(static_cast<int>(kFrameCount))
      .WillRepeatedly(Invoke(&write_caption_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service, create_observer("closed_caption"))
      .Times(1)
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_text_it.txt");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::PLAIN_TEXT;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  // The fixture writes the same control pair (Erase Displayed Memory) on all
  // four frames. Decoding is where an encoder's duplicate copy of a control
  // pair must be dropped, so the four pairs are two commands.
  EXPECT_EQ(result.cc_frames_exported, static_cast<int32_t>(kFrameCount) / 2);
  EXPECT_TRUE(std::filesystem::exists(out.path));
}

TEST(CCSinkOutputIntegrationTest,
     NullService_WritesValidHeaderWithoutObserver) {
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr);

  orc::ObservationContext context;
  // Older host: no observation service available.
  orc::CCSinkStageDeps deps(nullptr);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out("orc_cc_sink_nullsvc_it.scc");
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = orc::CCExportFormat::SCC;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);

  EXPECT_TRUE(result.success) << result.message;
  EXPECT_EQ(result.cc_frames_exported, 0);

  const std::string contents = read_file(out.path);
  EXPECT_THAT(contents, testing::HasSubstr("Scenarist_SCC V1.0"));
}
namespace {
// Run an export over the interleaved script and hand back the file contents.
std::string run_interleaved_export(const std::string& file_name,
                                   orc::CCExportFormat format,
                                   orc::EIA608Service service,
                                   int32_t* pairs_exported = nullptr) {
  const auto frame_count =
      static_cast<orc::FrameID>(interleaved_script().size());

  NiceMock<MockObservationService> service_mock;
  NiceMock<MockVideoFrameRepresentation> vfr;
  configure_ntsc_frames(vfr, frame_count);

  auto handle = std::make_unique<NiceMock<MockObserverHandle>>();
  ON_CALL(*handle, process_frame(_, _, _))
      .WillByDefault(Invoke(&write_interleaved_observations));
  std::unique_ptr<orc::IObserverHandle> handle_base = std::move(handle);
  EXPECT_CALL(service_mock, create_observer("closed_caption"))
      .WillOnce(Return(ByMove(std::move(handle_base))));

  orc::ObservationContext context;
  orc::CCSinkStageDeps deps(&service_mock);
  deps.init(orc::TriggerProgressCallback{}, nullptr);

  ScopedTempFile out(file_name);
  orc::CCExportOptions options;
  options.output_path = out.str();
  options.export_format = format;
  options.service = service;

  const orc::CCExportResult result = deps.export_cc(&vfr, context, options);
  EXPECT_TRUE(result.success) << result.message;
  if (pairs_exported != nullptr) {
    *pairs_exported = result.cc_frames_exported;
  }
  return read_file(out.path);
}
}  // namespace

TEST(CCSinkOutputIntegrationTest, PlainText_ExportsOnlyTheSelectedService) {
  // Issue #273: exported together, the two services' characters interleave and
  // the caption reads "42HI" (or worse, in a real recording).
  const std::string cc1 = run_interleaved_export(
      "orc_cc_sink_service_cc1_it.txt", orc::CCExportFormat::PLAIN_TEXT,
      orc::EIA608Service::CC1);
  EXPECT_THAT(cc1, testing::HasSubstr("HI"));
  EXPECT_THAT(cc1, testing::Not(testing::HasSubstr("42")));

  const std::string text2 = run_interleaved_export(
      "orc_cc_sink_service_t2_it.txt", orc::CCExportFormat::PLAIN_TEXT,
      orc::EIA608Service::T2);
  EXPECT_THAT(text2, testing::HasSubstr("42"));
  EXPECT_THAT(text2, testing::Not(testing::HasSubstr("HI")));
}

TEST(CCSinkOutputIntegrationTest, Scc_WritesOnlyTheSelectedServicesBytes) {
  int32_t pairs = 0;
  const std::string scc = run_interleaved_export(
      "orc_cc_sink_service_cc1_it.scc", orc::CCExportFormat::SCC,
      orc::EIA608Service::CC1, &pairs);

  EXPECT_THAT(scc, testing::HasSubstr("Scenarist_SCC V1.0"));
  // CC1's six pairs, and not TEXT2's two.
  EXPECT_EQ(pairs, 6);
  // 0x1C (TEXT2's control byte) with odd parity is 0x9c; "42" is 0x34 0x32.
  EXPECT_THAT(scc, testing::Not(testing::HasSubstr("9c")));
  EXPECT_THAT(scc, testing::Not(testing::HasSubstr("3432")));
  // 'H' 'I' with odd parity: 0xc8 0x49.
  EXPECT_THAT(scc, testing::HasSubstr("c849"));
}

TEST(CCSinkOutputIntegrationTest, Srt_WritesNumberedCuesWithMillisecondTimes) {
  const std::string srt = run_interleaved_export(
      "orc_cc_sink_it.srt", orc::CCExportFormat::SRT, orc::EIA608Service::CC1);

  EXPECT_THAT(srt, testing::StartsWith("1\n00:00:00,"));
  EXPECT_THAT(srt, testing::HasSubstr(" --> 00:00:00,"));
  EXPECT_THAT(srt, testing::HasSubstr("HI"));
  EXPECT_THAT(srt, testing::Not(testing::HasSubstr("42")));
}

TEST(CCSinkOutputIntegrationTest, Html_WritesAMonospacedTranscript) {
  const std::string html = run_interleaved_export(
      "orc_cc_sink_it.html", orc::CCExportFormat::HTML, orc::EIA608Service::T2);

  EXPECT_THAT(html, testing::StartsWith("<!DOCTYPE html>"));
  EXPECT_THAT(html, testing::HasSubstr("T2"));
  // The column layout only survives inside a preformatted, monospaced block.
  EXPECT_THAT(html, testing::HasSubstr("<pre>"));
  EXPECT_THAT(html, testing::HasSubstr("42"));
  EXPECT_THAT(html, testing::EndsWith("</html>\n"));
}

}  // namespace orc_unit_test
