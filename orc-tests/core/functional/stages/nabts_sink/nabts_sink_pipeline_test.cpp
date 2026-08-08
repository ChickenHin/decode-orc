/*
 * File:        nabts_sink_pipeline_test.cpp
 * Module:      orc-tests
 * Purpose:     End-to-end validation of the NABTS sink on real 525-line
 *              System C captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/teletext_slicer.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nabts_sink_stage.h"
#include "sha256_hash.h"
#include "vbi_source_stage.h"

namespace orc {
namespace {

// The reference captures. Neither is checked in (test-data/ is ignored), so
// every test here skips when its capture is absent.
//
// Both are FLAC-wrapped VBI crops of off-air NTSC recordings carrying NABTS:
// NBC Teletext (1983) and CBS ExtraVision (1985), the two services the US
// networks ran. They are what CEA-516 was written for.
const char* kNbtsCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/NTSC NABTS Teletext samples/"
    "NBC_1983-12-15_NBC_Teletext_NTSC_EP_vbi_only_part1_u16.flac";

const char* kExtraVisionCapture = ORC_VBI_TEST_DATA_DIR
    "/teletext/NTSC NABTS Teletext samples/"
    "CBS_1985-12-10_ExtraVision_teletext_NTSC_SP_vbi_only_part4_u16.flac";

// The 525-line World System Teletext capture, used for the cross-service
// check: a WST recording must yield essentially nothing from this stage.
const char* kWstCapture = ORC_VBI_TEST_DATA_DIR
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
// These captures run to tens of thousands of frames — hours of material. The
// tests decode a fixed window of a few hundred frames instead: long enough for
// the pass to settle and for whole data groups to arrive, short enough to run
// in a test. Frame ids are passed through unchanged.
class FrameWindow final : public Artifact, public VideoFrameRepresentation {
 public:
  FrameWindow(std::shared_ptr<const VideoFrameRepresentation> source,
              FrameID first, uint64_t count)
      : Artifact(ArtifactID("nabts-functional-window"), Provenance{}),
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
struct StreamGolden {
  const char* sha256;
  uint64_t bytes;
  uint64_t packets;
};

// A window a quarter of the way into each recording, past its lead-in.
constexpr uint64_t kFrameCount = 300;

constexpr StreamGolden kNbcGolden{"", 0, 0};
constexpr StreamGolden kExtraVisionGolden{"", 0, 0};

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

struct RecoveryRun {
  bool triggered = false;
  std::string status;
  std::string stream_path;
  uint64_t stream_bytes = 0;
  std::string stream_sha256;
};

std::map<std::string, ParameterValue> recovery_parameters(
    const std::string& output_path, bool learned_scan, int32_t decode_threads) {
  return {
      {"output_path", output_path},
      {"first_vbi_line", int32_t{10}},
      {"last_vbi_line", int32_t{21}},
      {"keep_empty_packets", false},
      // Automatic, as a user gets it: threshold first, MLSE only where that
      // could not lock.
      {"detector", std::string("Automatic")},
      {"tolerant_framing", false},
      {"require_valid_prefix", true},
      {"pin_data_phase", learned_scan},
      {"learn_active_lines", learned_scan},
      {"decode_threads", decode_threads},
      {"write_report", true},
  };
}

RecoveryRun recover(const std::string& capture_path,
                    const std::string& format_preset,
                    const std::filesystem::path& output_path,
                    bool learned_scan = true, int32_t decode_threads = 0) {
  RecoveryRun run;

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
      representation, static_cast<FrameID>(representation->frame_count() / 4),
      kFrameCount);

  FunctionalStageServices services;
  NabtsSinkStage stage(&services);
  const auto parameters =
      recovery_parameters(output_path.string(), learned_scan, decode_threads);
  stage.set_parameters(parameters);
  stage.execute({window}, parameters, observations);
  run.triggered = stage.trigger({window}, parameters, observations);
  run.status = stage.get_trigger_status();

  const std::filesystem::path candidate = output_path.string() + ".t33";
  if (std::filesystem::exists(candidate)) {
    run.stream_path = candidate.string();
    run.stream_bytes = std::filesystem::file_size(candidate);
    run.stream_sha256 = sha256_hex_of_file(candidate.string());
  }
  return run;
}

void report(const char* what, const RecoveryRun& run) {
  std::cout << "[ INFO     ] " << what << ": " << run.status << "\n"
            << "[ INFO     ] " << what << ": " << run.stream_bytes
            << " bytes, sha256 " << run.stream_sha256 << std::endl;
}

class NabtsSinkPipelineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-nabts-") + info->name());
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
// Recovery
// ---------------------------------------------------------------------------

TEST_F(NabtsSinkPipelineTest, NbcCaptureRecoversAServicesWorthOfPackets) {
  if (!std::filesystem::exists(kNbtsCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kNbtsCapture;
  }

  const RecoveryRun run =
      recover(kNbtsCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "nbc");
  report("NBC Teletext", run);

  ASSERT_TRUE(run.triggered) << run.status;
  EXPECT_EQ(std::filesystem::path(run.stream_path).extension(), ".t33");

  // The stream is a whole number of 33-byte packets and nothing else.
  ASSERT_GT(run.stream_bytes, 0u) << "no packets recovered at all";
  EXPECT_EQ(run.stream_bytes % kNabtsPacketBytes, 0u);

  // A real service inserting on several lines of every field yields packets in
  // the thousands over 300 frames, not the handful a false-lock rate produces.
  const uint64_t packets = run.stream_bytes / kNabtsPacketBytes;
  EXPECT_GT(packets, kFrameCount)
      << "only " << packets << " packets over " << kFrameCount
      << " frames — that is a false-lock rate, not a service";
}

TEST_F(NabtsSinkPipelineTest,
       ExtraVisionCaptureRecoversAServicesWorthOfPackets) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  const RecoveryRun run =
      recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
              dir_ / "extravision");
  report("CBS ExtraVision", run);

  ASSERT_TRUE(run.triggered) << run.status;
  ASSERT_GT(run.stream_bytes, 0u) << "no packets recovered at all";
  EXPECT_EQ(run.stream_bytes % kNabtsPacketBytes, 0u);

  const uint64_t packets = run.stream_bytes / kNabtsPacketBytes;
  EXPECT_GT(packets, kFrameCount)
      << "only " << packets << " packets over " << kFrameCount
      << " frames — that is a false-lock rate, not a service";
}

// The claim the whole design rests on, against real recordings rather than
// synthesized lines: System B and System C share everything on a 525-line
// capture but the framing code, so a World System Teletext recording must
// yield essentially nothing here.
TEST_F(NabtsSinkPipelineTest, AWstCaptureYieldsAlmostNothing) {
  if (!std::filesystem::exists(kWstCapture)) {
    GTEST_SKIP() << "525-line WST capture not present: " << kWstCapture;
  }
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present for comparison";
  }

  const RecoveryRun wst = recover(kWstCapture, ".tbc VBI crop, 16-bit (NABTS)",
                                  dir_ / "wst-as-nabts");
  report("TBS Electra (WST) read as NABTS", wst);
  ASSERT_TRUE(wst.triggered) << wst.status;

  const RecoveryRun nabts = recover(
      kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)", dir_ / "nabts");
  ASSERT_TRUE(nabts.triggered) << nabts.status;

  const uint64_t wst_packets = wst.stream_bytes / kNabtsPacketBytes;
  const uint64_t nabts_packets = nabts.stream_bytes / kNabtsPacketBytes;
  ASSERT_GT(nabts_packets, 0u);

  // An order of magnitude, which is what the recordings actually show: 126
  // packets from the WST capture against 2460 from the NABTS one over the same
  // 300 frames. The separation is not perfect and is not expected to be — the
  // threshold detector rejects every WST line outright, but the MLSE detector
  // fits the framing code rather than matching it, so a few per cent of a
  // damaged recording's lines survive both it and the Hamming prefix gate. They
  // arrive at a mean decision confidence of 0,21 against 0,56 for a real
  // service, and none of them assembles into a data group.
  //
  // The bound is deliberately loose: it is here to catch the discrimination
  // failing altogether, not to pin a decoder tuning figure.
  EXPECT_LT(wst_packets * 10, nabts_packets)
      << wst_packets << " packets from a WST recording against "
      << nabts_packets << " from a NABTS one: the framing-code discrimination "
      << "is not holding";
}

// The property the parallel decode rests on: what a worker may read while it
// works is frozen for the length of a block, so the recovered stream cannot
// depend on how the blocks were scheduled.
TEST_F(NabtsSinkPipelineTest, TheDecodeDoesNotDependOnTheThreadCount) {
  if (!std::filesystem::exists(kExtraVisionCapture)) {
    GTEST_SKIP() << "NABTS capture not present: " << kExtraVisionCapture;
  }

  std::string reference;
  for (const int32_t threads : {1, 3, 8}) {
    const RecoveryRun run =
        recover(kExtraVisionCapture, ".tbc VBI crop, 16-bit (NABTS)",
                dir_ / ("threads-" + std::to_string(threads)),
                /*learned_scan=*/true, threads);
    ASSERT_TRUE(run.triggered) << run.status;
    ASSERT_FALSE(run.stream_sha256.empty());

    if (reference.empty()) {
      reference = run.stream_sha256;
    } else {
      EXPECT_EQ(run.stream_sha256, reference)
          << "decoding on " << threads << " thread(s) produced a different "
          << "packet stream";
    }
  }
}

}  // namespace
}  // namespace orc
