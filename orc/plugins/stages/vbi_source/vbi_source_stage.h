/*
 * File:        vbi_source_stage.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Raw VBI capture source stage: places VBI records into CVBS
 * frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_SOURCE_STAGE_H
#define ORC_VBI_SOURCE_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "vbi_byte_source.h"
#include "vbi_source_format.h"

namespace orc {

// Dependency injection interface for the VBI source stage.
//
// A raw VBI capture has no sidecars: the whole of the stage's external I/O is
// opening one file and reading bytes out of it, so this interface is the byte
// transport and nothing else.  Unit tests substitute an in-memory byte source
// and never touch the filesystem.
class IVBISourceStageDeps {
 public:
  virtual ~IVBISourceStageDeps() = default;

  // Validate that input_path exists and is a readable regular file.
  virtual bool validate_input_file(const std::string& input_path,
                                   std::string& error_message) const = 0;

  // Open the capture for reading, transparently unwrapping FLAC.  Returns
  // nullptr with an error message when it cannot be opened.
  virtual std::unique_ptr<IVBIByteSource> open_byte_source(
      const std::string& input_path, std::string& error_message) const = 0;
};

// Raw VBI capture source stage.
//
// Third-party VBI captures — bt8x8 and family card dumps, cropped VBI-only
// .tbc files — hold nothing but the teletext lines.  This stage reads those
// line records and lays them onto CVBS frames at the timing point and the
// amplitude the standard puts them at, so that the existing teletext decoders
// see them exactly as they see them on a native decode (design §1).
//
// The rest of each frame is blanking.  A capture carries no sync, no vertical
// interval and no burst, and nothing that reads this stage's output looks for
// them: the teletext slicer locks to the clock run-in within the line it is
// handed.  Manufacturing a whole television signal around the data would cost
// far more than placing the data does and would be spent entirely on samples
// nobody reads.
//
// Nothing is written to disk: the stage's only product is the in-memory
// CVBS_U10_4FSC representation.  Connect the CVBS sink to export it.
//
// Frames are built lazily, one at a time, on the frame a consumer asks for.
// A PAL frame is 1,4 MB against the 64 KiB of VBI records it comes from, so a
// four-hour capture is 24 GB of records and 522 GB of frames; materialising it
// is not an option (design §5.7).
//
// What the stage is currently able to place is one path end to end: the bt8x8
// PAL preset, world system teletext, FLAC-wrapped or raw.  Every other format
// in the design's table fails at configuration with a message saying so rather
// than producing plausible but wrong output.
class VBISourceStage : public DAGStage,
                       public ParameterizedStage,
                       public IStagePreviewCapability {
 public:
  explicit VBISourceStage(std::shared_ptr<IVBISourceStageDeps> deps = nullptr);
  ~VBISourceStage() override = default;

  void set_deps_override(std::shared_ptr<IVBISourceStageDeps> deps) {
    deps_ = std::move(deps);
  }

  // DAGStage interface
  std::string version() const override { return "1.0.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 0; }
  size_t output_count() const override { return 1; }

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;

  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  // Every configured value the stage holds, in the spelling the project file
  // and the parameter surface use.
  struct Configuration {
    std::string input_path;
    std::string format_preset = "bt8x8-pal";

    // Container fields, used only when the preset is "custom".
    double container_sample_rate_hz = 0.0;
    uint32_t container_line_length = 0;
    uint32_t container_valid_samples = 0;
    std::string container_sample_format = "u8";
    uint32_t container_field_lines = 0;
    uint32_t container_first_record = 0;
    uint32_t container_last_record = 0;
    uint32_t container_frame_trailer_bytes = 0;
    std::string container_tv_system = "PAL";

    std::string teletext_system = "WST";
    std::string capture_offset_mode = "auto";
    double capture_offset_samples = 0.0;
    std::string levels = "per-line";
    double fixed_logic0 = 0.0;
    double fixed_logic1 = 255.0;
    uint32_t first_field = 1;
    std::string drops = "preserve";

    // Identity of the configuration, for the frame cache and the artifact ID.
    std::string cache_key() const;
  };

  // Expand the configuration into a container descriptor.  Returns false with
  // an error message for a preset that does not exist, a data service the
  // stage cannot place, or a container field that cannot be parsed.
  static bool make_source_format(const Configuration& configuration,
                                 VBISourceFormat& out_format,
                                 std::string& error_message);

  // Read the stage's configuration out of an execute() parameter map, falling
  // back to the value held by the stage for anything the map omits.
  Configuration configuration_from(
      const std::map<std::string, ParameterValue>& parameters) const;

  Configuration configuration_;

  mutable std::mutex execute_mutex_;
  std::string cached_key_;
  ArtifactPtr cached_representation_;

  std::shared_ptr<IVBISourceStageDeps> deps_;
};

}  // namespace orc

#endif  // ORC_VBI_SOURCE_STAGE_H
