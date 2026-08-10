/*
 * File:        vbi_frame_builder.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Places VBI line records on an otherwise blank CVBS frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_FRAME_BUILDER_H
#define ORC_VBI_FRAME_BUILDER_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vbi_level_mapper.h"
#include "vbi_line_placement.h"
#include "vbi_line_reader.h"
#include "vbi_output_frame.h"
#include "vbi_resampler.h"
#include "vbi_source_format.h"

namespace orc {

// Builds the CVBS frames the stage emits.
//
// Everything a capture holds is the teletext lines, and everything downstream
// wants of it is those lines at the right place on the right frame line at the
// right amplitude.  So that is all the builder writes: the frame is blanking
// throughout, and each record is band-limited onto the output lattice, mapped
// into the amplitude domain and laid over the data region of its frame line.
//
// Nothing else is manufactured.  There is no sync, no vertical interval and no
// colour burst, because there is nothing in a VBI capture to reconstruct them
// from and no consumer of this stage that reads them: the teletext slicer locks
// to the clock run-in inside the line it is handed, and every other observer of
// a VBI capture works the same way.  Synthesising the rest of a television
// signal would cost roughly forty times what placing the data costs, all of it
// spent on samples nothing reads.
//
// Which frame line a record lands on is resolved once, at construction, into a
// table per stored field; building a frame is then a fill, a lookup and a
// filter per data line.
//
// Thread-compatible: instances hold configuration only, and build_frame() is
// const and writes only to its output.
class VBIFrameBuilder {
 public:
  // An unusable builder; make_vbi_frame_builder() produces the usable
  // instances.  Building through this one is a reported error rather than a
  // crash.
  VBIFrameBuilder() = default;

  VBIFrameBuilder(VBISourceFormat format, VBIOutputFrame output_frame,
                  VBIDataPlacement placement, VBIRecordResampler resampler,
                  VBILevelMapper level_mapper,
                  std::array<std::vector<uint32_t>, 2> frame_lines);

  const VBIOutputFrame& output_frame() const { return output_frame_; }

  const VBIDataPlacement& placement() const { return placement_; }

  const VBILevelMapper& level_mapper() const { return level_mapper_; }

  // Frame lines the records of a stored field are written to, in record order.
  const std::vector<uint32_t>& frame_lines(uint32_t stored_field_index) const;

  // A frame of blanking and nothing else.  This is what a frame synthesised to
  // fill a gap in the source's frame counter is made of (design §6.3), and the
  // starting point of every other frame.
  void build_blank_frame(std::vector<int16_t>& out_samples) const;

  // Build the frame carrying one stored frame's records.
  //
  // Records whose levels could not be established are left as blanking rather
  // than written as arbitrarily scaled noise.  out_data_lines counts the frame
  // lines that did receive source data.
  bool build_frame(const std::vector<VBILineRecord>& records,
                   std::vector<int16_t>& out_samples, uint32_t& out_data_lines,
                   std::string& error_message) const;

 private:
  VBISourceFormat format_{};
  VBIOutputFrame output_frame_{};
  VBIDataPlacement placement_{};
  VBIRecordResampler resampler_{};
  VBILevelMapper level_mapper_{};

  // Frame line per record, indexed by stored field then by record index
  // relative to the format's field range.
  std::array<std::vector<uint32_t>, 2> frame_lines_{};
};

// Build the assembler a source format calls for: its output lattice, the
// placement of its records on that lattice, the resampler that lands them
// there, and the frame line each record belongs to.
//
// The two timing figures settled when a capture is opened.
//
// They are passed explicitly rather than read from the format and the service
// table so that what was measured overrides what was tabulated, without either
// of those having to be rewritten (design §5.3.4).  Which of the two is
// measured depends on the source family, and it is never both: a capture has
// one unknown here, and fitting two would be fitting it twice.
struct VBIResolvedTiming {
  // Time from 0H to sample 0 of every record, in source samples.  Fitted from
  // the run-in for a card capture, whose hardware does not say; exactly zero
  // for a TBC-derived one, whose records start at 0H by construction.
  double capture_offset_samples = 0.0;

  // The service's 0H-to-run-in anchor in nanoseconds, when it was measured
  // from the capture rather than taken from the service table.  Empty leaves
  // the tabulated figure standing.
  //
  // Only a TBC-derived capture ever sets it.  A card capture's fitted offset
  // has already put its run-in at the tabulated anchor, so there is nothing
  // left for a measurement here to say.
  std::optional<double> service_anchor_ns;
};

// Returns false with an error message for any part the stage does not yet
// support, and for a configuration that maps two records of a frame onto the
// same frame line — which is a wrong field range rather than something to
// resolve silently.
bool make_vbi_frame_builder(const VBISourceFormat& format,
                            const VBILevelMapperConfig& level_config,
                            const VBIResolvedTiming& timing,
                            VBIFrameBuilder& out_builder,
                            std::string& error_message);

}  // namespace orc

#endif  // ORC_VBI_FRAME_BUILDER_H
