/*
 * File:        vbi_frame_synthesis.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Assembles synthesised CVBS frames from mapped VBI line records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_SYNTHESIS_H
#define ORC_VBI_FRAME_SYNTHESIS_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_burst_synthesis.h"
#include "vbi_frame_geometry.h"
#include "vbi_level_mapper.h"
#include "vbi_line_mapping.h"
#include "vbi_line_placement.h"
#include "vbi_line_synthesis.h"
#include "vbi_output_levels.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"
#include "vbi_vertical_interval.h"

namespace orc {

// What the frame assembler manufactures around the recovered data.
struct VBIFrameSynthesisConfig {
  // Whether a coherent colour burst is written.  Default on: it is the more
  // faithful reconstruction, since real broadcast teletext lines carry burst,
  // and it is a precondition of claiming a locked signal state (design §5.6).
  bool synthesise_burst = true;
};

// One assembled output frame.
struct VBISynthesisedFrame {
  uint64_t output_frame_index = 0;

  // The frame's samples in stored order, already clamped to the legal range of
  // the sample encoding.  Exactly samples_per_frame() entries.
  std::vector<uint16_t> samples;

  // Stored frame lines that received resampled source data, in ascending
  // order.  Everything else in the frame is manufactured.
  std::vector<uint32_t> data_frame_lines;

  bool burst_synthesised = false;

  // True when the frame holds no source data at all: either a frame
  // synthesised to fill a gap in the source's frame counter, or one whose
  // records carried no measurable data service.
  bool padding = false;
};

// Assembles complete CVBS frames.
//
// The assembler owns the whole output frame: it walks every frame line through
// the geometry module, synthesises the line's sync structure and burst, writes
// any resampled source data over the result, and clamps into the stored word.
// Line offsets come from the geometry and from nowhere else — a frame built on
// an assumed constant line length would be four samples short for PAL and
// would desynchronise every frame after it (design §2.3, §8).
//
// Thread-compatible: instances hold configuration only, and every synthesis
// member is const and writes only to its output.
class VBIFrameSynthesiser {
 public:
  // An unusable assembler; make_vbi_frame_synthesiser() produces the usable
  // instances.  Synthesising through this one is a reported error rather than
  // a crash.
  VBIFrameSynthesiser() = default;

  VBIFrameSynthesiser(VBISourceFormat format, VBITeletextService service,
                      VBIFrameGeometry geometry,
                      VBIVerticalInterval vertical_interval,
                      VBITeletextLineMap line_map, VBIOutputLevels levels,
                      VBIBurstTiming burst_timing,
                      VBIFrameSynthesisConfig config,
                      double output_sample_rate_hz);

  const VBIFrameGeometry& geometry() const { return geometry_; }

  const VBIOutputLevels& levels() const { return levels_; }

  const VBIFrameSynthesisConfig& config() const { return config_; }

  const VBILineSynthesiser& line_synthesiser() const {
    return line_synthesiser_;
  }

  const VBIBurstSynthesiser& burst_synthesiser() const {
    return burst_synthesiser_;
  }

  // Guard held either side of the data region when a record is clipped to it:
  // one transmitted bit period, in output samples.
  double data_guard_samples() const;

  // Assemble a frame that carries no source data: the sync structure, the
  // vertical interval and the burst only.  This is what a padded frame is
  // made of (design §6.3).
  bool synthesise_blank_frame(uint64_t output_frame_index,
                              VBISynthesisedFrame& out_frame,
                              std::string& error_message) const;

  // Assemble a frame carrying the mapped records of one stored source frame.
  //
  // capture_offset_samples is passed per call rather than read from the format
  // so the calibrated value overrides the descriptor's starting hint without
  // rewriting it (design §5.3.4).  Lines whose levels could not be established
  // are emitted as ordinary blanking rather than as noise.
  bool synthesise_frame(uint64_t output_frame_index,
                        const std::vector<VBIMappedLine>& mapped_lines,
                        const IVBIResampler& resampler,
                        double capture_offset_samples,
                        VBISynthesisedFrame& out_frame,
                        std::string& error_message) const;

 private:
  // Fill an output frame with manufactured lines, and note where each line
  // starts.  Returns false when the assembled size does not match the
  // normative frame size.
  bool assemble_manufactured_frame(uint64_t output_frame_index,
                                   VBISynthesisedFrame& out_frame,
                                   std::vector<double>& line_buffer,
                                   std::string& error_message) const;

  VBISourceFormat format_{};
  VBITeletextService service_{};
  VBIFrameGeometry geometry_{};
  double output_sample_rate_hz_ = 0.0;
  VBITeletextLineMap line_map_{};
  VBIOutputLevels levels_{};
  VBIFrameSynthesisConfig config_{};
  VBILineSynthesiser line_synthesiser_{
      VBIFrameGeometry(), VBIVerticalInterval(), VBIOutputLevels(), 0.0};
  VBIBurstSynthesiser burst_synthesiser_{VBIFrameGeometry(), VBIOutputLevels(),
                                         VBIBurstTiming(), 0.0};
};

// Half-open output window over which a frame line accepts resampled source
// samples.
struct VBIDataWindow {
  uint32_t begin = 0;
  uint32_t end = 0;

  uint32_t count() const { return (end > begin) ? (end - begin) : 0; }
};

// Clip a line placement to the line's data region (design §5.6).
//
// A record covers more of the line than the data region does: a bt8x8 PAL
// record opens at about 7.4 us, which is inside the colour burst window, and
// it runs on past the end of the packet.  Writing all of it would overwrite
// the synthesised burst with a level-mapped fragment of the source's own and
// would put teletext-scaled samples into the front porch, so the record is
// clipped to the region the standard gives the data.
//
// guard_samples is kept at each end, so the leading edge of the first clock
// run-in bit and the trailing edge of the last payload bit survive the clip.
VBIDataWindow vbi_data_region_window(const VBILinePlacement& placement,
                                     double guard_samples,
                                     uint32_t line_length);

// Normative sample count of one stored frame of a television system.
//
// Stated here independently of the frame geometry so that the assembler can
// hold what it builds against the standard rather than against the lattice it
// was handed.  That is what catches the constant-1135 mistake: a PAL frame
// assembled at 625 x 1135 is four samples short, and every frame after it in
// the file is displaced (design §2.1, §8).
bool vbi_normative_frame_samples(VBITVSystem tv_system,
                                 uint32_t& out_samples_per_frame,
                                 std::string& error_message);

// Build the assembler a source format calls for: its frame geometry, vertical
// interval, teletext line map, amplitude domain and burst parameters.  Returns
// false with an error message for any part the stage does not yet synthesise.
bool make_vbi_frame_synthesiser(const VBISourceFormat& format,
                                const VBIFrameSynthesisConfig& config,
                                VBIFrameSynthesiser& out_synthesiser,
                                std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_FRAME_SYNTHESIS_H
