/*
 * File:        teletext_analysis_pipeline_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the teletext analysis sink on real
 *              625-line and 525-line WST captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "sha256_hash.h"
#include "teletext_analysis_sink_stage.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// The reference captures. Neither is checked in (test-data/ is ignored), so
// every test here skips when its capture is absent.
//
// 625 lines: a FLAC-wrapped bt8x8 card dump of off-air PAL carrying World
// System Teletext — the same sample the VBI source stage is validated against.
const char* kPalCapture =
    ORC_VBI_TEST_DATA_DIR "/teletext/bt8x8 sample/0002.vbi.flac";

// 525 lines: a TBS Electra recording, the circulating material that carries
// System B on 525 lines (ITU-R BT.653 Table 1b).
const char* kNtscCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/TBS Electra WST 525-60/"
    "TBS_1988-01-03_Electra_teletext_NTSC_SP_vbi_only_u16-002.flac";

// ---------------------------------------------------------------------------
// Host services: the stage writes its packet stream through the host's file
// writer factory, so the functional harness supplies a real one.
// ---------------------------------------------------------------------------

class FileWriterUint8 final : public IFileWriterUint8 {
 public:
  ~FileWriterUint8() override { close(); }

  bool open(const std::string& filepath) override {
    stream_.open(filepath, std::ios::binary | std::ios::trunc);
    return stream_.is_open();
  }

  void write(const uint8_t* data, size_t count) override {
    stream_.write(reinterpret_cast<const char*>(data),
                  static_cast<std::streamsize>(count));
  }

  void write(const std::vector<uint8_t>& data) override {
    write(data.data(), data.size());
  }

  void flush() override { stream_.flush(); }

  void close() override {
    if (stream_.is_open()) stream_.close();
  }

 private:
  std::ofstream stream_;
};

class FunctionalStageServices final : public IStageServices {
 public:
  std::shared_ptr<IFileWriterUint8> create_buffered_file_writer_uint8(
      size_t /*buffer_size*/) override {
    return std::make_shared<FileWriterUint8>();
  }
  std::shared_ptr<IFileWriterUint16> create_buffered_file_writer_uint16(
      size_t /*buffer_size*/) override {
    return nullptr;
  }
  std::shared_ptr<IFileWriterInt16> create_buffered_file_writer_int16(
      size_t /*buffer_size*/) override {
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// A window onto a capture
// ---------------------------------------------------------------------------
//
// The sink analyses whatever frame range its input carries, and these captures
// run to tens of thousands of frames — hours of material. The tests decode a
// fixed window of a few hundred frames instead, which is several carousel
// cycles: long enough for whole pages to assemble and for repeated rows to
// correct each other, short enough to run in a test.
//
// Frame ids are passed through unchanged, so what the catalogue reports is the
// capture's own frame numbering.
class FrameWindow final : public Artifact, public VideoFrameRepresentation {
 public:
  FrameWindow(std::shared_ptr<const VideoFrameRepresentation> source,
              FrameID first, uint64_t count)
      : Artifact(ArtifactID("teletext-analysis-functional-window"),
                 Provenance{}),
        source_(std::move(source)),
        range_{first, static_cast<FrameID>(first + count - 1)} {}

  std::string type_name() const override { return "VideoFrameRepresentation"; }

  FrameIDRange frame_range() const override { return range_; }
  size_t frame_count() const override {
    return static_cast<size_t>(range_.count());
  }
  bool has_frame(FrameID id) const override {
    return range_.contains(id) && source_->has_frame(id);
  }

  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!range_.contains(id)) return std::nullopt;
    return source_->get_frame_descriptor(id);
  }

  const sample_type* get_frame(FrameID id) const override {
    return range_.contains(id) ? source_->get_frame(id) : nullptr;
  }

  const sample_type* get_line(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line(id, line) : nullptr;
  }

  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return range_.contains(id) ? source_->get_frame_copy(id)
                               : std::vector<sample_type>{};
  }

  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    return range_.contains(id) ? source_->get_line_samples(id, line)
                               : std::vector<sample_type>{};
  }

  bool has_separate_channels() const override {
    return source_->has_separate_channels();
  }
  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line_luma(id, line) : nullptr;
  }
  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return range_.contains(id) ? source_->get_line_chroma(id, line) : nullptr;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    return source_->get_video_parameters();
  }

 private:
  std::shared_ptr<const VideoFrameRepresentation> source_;
  FrameIDRange range_;
};

// ---------------------------------------------------------------------------
// Golden data
// ---------------------------------------------------------------------------
//
// The packet stream is the stage's product, so the reference is the stream
// itself: its SHA-256 and its length. Recorded from these captures with the
// parameter set below, which is fixed here rather than taken from the
// descriptors' defaults so that changing a default cannot silently move the
// reference.
//
// The 625-line reference is the pre-refactor pipeline's own output: the
// teletext sink this stage was reworked from, driven over the same window
// through the direct-slice path the rework kept, writes these bytes exactly.
// The 525-line reference was generated with this stage, since the sink it
// replaced dropped 34-byte packets rather than exporting them.
struct StreamGolden {
  const char* sha256;
  uint64_t bytes;
  uint64_t packets;
};

// A window past the capture's off-air opening minute (the VBI source tests
// record why the head of this recording carries nothing).
constexpr FrameID kPalFirstFrame = 2000;
constexpr uint64_t kPalFrameCount = 200;

constexpr StreamGolden kPalGolden{
    "60a83a53e4f13bb345ac8e9a8787432b6ff218f3dd9f184d41049059fbf55b92", 166488,
    3964};

// Pages this window of the capture carries, in the magazine + two hex digits a
// viewer shows. A sample of the catalogue rather than all of it: the window
// holds 99 pages, and naming every one would fail on a decoder improvement that
// recovered a hundredth without saying anything about what went wrong.
const std::vector<std::string> kPalExpectedPages{"100", "101", "199", "301",
                                                 "401"};

// The 525-line window. It starts a quarter of the way into the tape, past its
// lead-in, as the other tests over this capture read it (the start is derived
// from the frame count, so it is passed in rather than named here).
constexpr uint64_t kNtscFrameCount = 300;

constexpr StreamGolden kNtscGolden{
    "5406aadba1cedac9b4720233d587f0fbc898b93056645a8d46872abb1d294936", 90168,
    2652};

// As above, of the 52 pages this window holds. Page 100 is the service's front
// page, so a decode that addresses packets correctly always finds it.
const std::vector<std::string> kNtscExpectedPages{"100", "101", "150", "199",
                                                  "400"};

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

// The decoded run: what the stage cached for the viewer, plus the stream it
// wrote.
struct AnalysisRun {
  bool triggered = false;
  std::string status;
  std::string stream_path;
  uint64_t stream_bytes = 0;
  std::string stream_sha256;
  TeletextAnalysisDataset dataset;
};

std::map<std::string, ParameterValue> analysis_parameters(
    const std::string& output_path, int32_t first_line, int32_t last_line) {
  return {
      {"output_path", output_path},
      {"first_vbi_line", first_line},
      {"last_vbi_line", last_line},
      {"keep_empty_packets", false},
      // Automatic, as a user gets it: threshold first, MLSE only where that
      // could not lock — which is the path both captures really take.
      {"detector", std::string("Automatic")},
      {"tolerant_framing", false},
      {"require_valid_mrag", true},
      {"repair_damaged_bytes", true},
      {"squash_repeated_rows", true},
      {"write_report", false},
  };
}

// Load a capture, window it, and run the sink over the window.
AnalysisRun analyse(const std::string& capture_path,
                    const std::string& format_preset,
                    const std::filesystem::path& output_path,
                    FrameID (*first_frame)(uint64_t), uint64_t frame_count,
                    int32_t first_line, int32_t last_line) {
  AnalysisRun run;

  VBISourceStage source;
  ObservationContext observations;
  const std::vector<ArtifactPtr> outputs = source.execute(
      {}, {{"input_path", capture_path}, {"format", format_preset}},
      observations);
  if (outputs.empty()) return run;
  const auto representation =
      std::dynamic_pointer_cast<VideoFrameRepresentation>(outputs.front());
  if (!representation) return run;

  const auto window = std::make_shared<FrameWindow>(
      representation, first_frame(representation->frame_count()), frame_count);

  FunctionalStageServices services;
  TeletextAnalysisSinkStage stage(&services);
  const auto parameters =
      analysis_parameters(output_path.string(), first_line, last_line);
  stage.set_parameters(parameters);
  stage.execute({window}, parameters, observations);
  run.triggered = stage.trigger({window}, parameters, observations);
  run.status = stage.get_trigger_status();
  run.dataset = stage.dataset();

  // The stage appends the service's extension, so the stream is found from the
  // path it reports rather than from the one it was given.
  for (const char* extension : {".t42", ".t34"}) {
    const std::filesystem::path candidate = output_path.string() + extension;
    if (!std::filesystem::exists(candidate)) continue;
    run.stream_path = candidate.string();
    run.stream_bytes = std::filesystem::file_size(candidate);
    run.stream_sha256 = sha256_hex_of_file(candidate.string());
  }
  return run;
}

// The pages a run catalogued, as the magazine + two hex digits a viewer shows.
std::set<std::string> catalogued_pages(const TeletextAnalysisDataset& dataset) {
  std::set<std::string> pages;
  for (const auto& page : dataset.pages) {
    char text[8];
    std::snprintf(text, sizeof(text), "%d%02X", page.magazine,
                  page.page_number);
    pages.insert(text);
  }
  return pages;
}

void report(const char* what, const AnalysisRun& run) {
  std::cout << "[ INFO     ] " << what << ": " << run.status << "\n"
            << "[ INFO     ] " << what << ": " << run.stream_bytes
            << " bytes, sha256 " << run.stream_sha256 << "\n"
            << "[ INFO     ] " << what << ": pages";
  for (const auto& page : catalogued_pages(run.dataset)) {
    std::cout << " " << page;
  }
  std::cout << std::endl;
}

// Unique output directory per test; removed on teardown.
class TeletextAnalysisPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-teletext-") + info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
};

// ---------------------------------------------------------------------------
// 625 lines
// ---------------------------------------------------------------------------

TEST_F(TeletextAnalysisPipelineTest,
       PalCaptureExportsTheReferencePacketStream) {
  if (!std::filesystem::exists(kPalCapture)) {
    GTEST_SKIP() << "625-line capture not present: " << kPalCapture;
  }

  const AnalysisRun run = analyse(
      kPalCapture, "bt8x8 card dump, 8-bit (WST)", dir_ / "pal",
      [](uint64_t) { return kPalFirstFrame; }, kPalFrameCount, 6, 22);
  report("PAL WST", run);

  ASSERT_TRUE(run.triggered) << run.status;

  // A 625-line service is exported as the 42-byte packet stream, whatever the
  // output path was called.
  EXPECT_EQ(std::filesystem::path(run.stream_path).extension(), ".t42");

  // Every byte of the stream, against the reference: the packets, their
  // contents after repair and squashing, and the strict frame → field →
  // ascending-line order they are written in.
  EXPECT_EQ(run.stream_bytes, kPalGolden.bytes);
  EXPECT_EQ(run.stream_sha256, kPalGolden.sha256)
      << "the exported packet stream differs from the reference decode of this "
         "capture";
  EXPECT_EQ(run.dataset.summary.packets_recovered, kPalGolden.packets);
}

TEST_F(TeletextAnalysisPipelineTest, PalCaptureCataloguesTheServicesPages) {
  if (!std::filesystem::exists(kPalCapture)) {
    GTEST_SKIP() << "625-line capture not present: " << kPalCapture;
  }

  const AnalysisRun run = analyse(
      kPalCapture, "bt8x8 card dump, 8-bit (WST)", dir_ / "pal",
      [](uint64_t) { return kPalFirstFrame; }, kPalFrameCount, 6, 22);
  report("PAL WST", run);
  ASSERT_TRUE(run.triggered) << run.status;

  EXPECT_EQ(run.dataset.summary.frames_analysed, kPalFrameCount);
  ASSERT_FALSE(run.dataset.pages.empty()) << "no page completed";

  // The pages this window of the capture carries. A catalogue that lost the
  // page numbers — a mis-decoded MRAG or page-number nibble — reports different
  // ones, and one that lost the carousel reports fewer.
  const std::set<std::string> pages = catalogued_pages(run.dataset);
  for (const std::string& expected : kPalExpectedPages) {
    EXPECT_NE(pages.find(expected), pages.end())
        << "page " << expected << " missing from the catalogue";
  }

  // The carousel of a broadcast service, not a handful of pages that happened
  // to complete: this window catalogues 99.
  EXPECT_GT(pages.size(), 80u);

  // Every catalogued page was seen at least once and within the window.
  for (const auto& page : run.dataset.pages) {
    EXPECT_GT(page.times_seen, 0u);
    EXPECT_GE(page.first_seen_frame, kPalFirstFrame);
    EXPECT_LE(page.last_seen_frame, kPalFirstFrame + kPalFrameCount);
    EXPECT_LE(page.first_seen_frame, page.last_seen_frame);
  }

  // A 625-line page is 40 columns from one 42-byte packet per row.
  for (const auto& page : run.dataset.pages) {
    EXPECT_EQ(page.page.columns, TeletextPageSnapshot::kColumns);
  }
}

// ---------------------------------------------------------------------------
// 525 lines
// ---------------------------------------------------------------------------

TEST_F(TeletextAnalysisPipelineTest,
       NtscCaptureExportsTheReferencePacketStream) {
  if (!std::filesystem::exists(kNtscCapture)) {
    GTEST_SKIP() << "525-line capture not present: " << kNtscCapture;
  }

  const AnalysisRun run = analyse(
      kNtscCapture, ".tbc VBI crop, 16-bit (WST)", dir_ / "ntsc",
      [](uint64_t frames) { return static_cast<FrameID>(frames / 4); },
      kNtscFrameCount, 10, 21);
  report("NTSC WST", run);

  ASSERT_TRUE(run.triggered) << run.status;

  // A 525-line service is exported as the 34-byte packet stream — the packets
  // the pre-refactor sink dropped.
  EXPECT_EQ(std::filesystem::path(run.stream_path).extension(), ".t34");
  ASSERT_GT(run.dataset.summary.packets_recovered, 0u);
  EXPECT_EQ(run.stream_bytes, run.dataset.summary.packets_recovered * 34)
      << "the stream is not a flat run of whole 34-byte packets";

  EXPECT_EQ(run.stream_bytes, kNtscGolden.bytes);
  EXPECT_EQ(run.stream_sha256, kNtscGolden.sha256)
      << "the exported packet stream differs from the reference decode of this "
         "capture";
  EXPECT_EQ(run.dataset.summary.packets_recovered, kNtscGolden.packets);
}

TEST_F(TeletextAnalysisPipelineTest, NtscCaptureCataloguesTheServicesPages) {
  if (!std::filesystem::exists(kNtscCapture)) {
    GTEST_SKIP() << "525-line capture not present: " << kNtscCapture;
  }

  const AnalysisRun run = analyse(
      kNtscCapture, ".tbc VBI crop, 16-bit (WST)", dir_ / "ntsc",
      [](uint64_t frames) { return static_cast<FrameID>(frames / 4); },
      kNtscFrameCount, 10, 21);
  report("NTSC WST", run);
  ASSERT_TRUE(run.triggered) << run.status;

  ASSERT_FALSE(run.dataset.pages.empty()) << "no page completed";

  const std::set<std::string> pages = catalogued_pages(run.dataset);
  for (const std::string& expected : kNtscExpectedPages) {
    EXPECT_NE(pages.find(expected), pages.end())
        << "page " << expected << " missing from the catalogue";
  }

  // This window catalogues 52 pages of the service's carousel.
  EXPECT_GT(pages.size(), 40u);

  // 40 columns from 34-byte packets: this service sends the last eight columns
  // of each row in row-extension packets, which the catalogue's page assembly
  // reads. A page still 32 wide would mean they had stopped being recognised.
  size_t wide_pages = 0;
  for (const auto& page : run.dataset.pages) {
    if (page.page.columns == TeletextPageSnapshot::kColumns) ++wide_pages;
  }
  EXPECT_EQ(wide_pages, run.dataset.pages.size());
}

}  // namespace
}  // namespace orc
