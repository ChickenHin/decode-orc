/*
 * File:        composite_vectorscope.h
 * Module:      orc-core
 * Purpose:     Chroma demodulation from the composite carrier for the
 *              technical (measurement) vectorscope
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_VECTORSCOPE_COMPOSITE_VECTORSCOPE_H
#define ORC_CORE_ANALYSIS_VECTORSCOPE_COMPOSITE_VECTORSCOPE_H

#include <orc/stage/frame_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/preview/orc_vectorscope.h>
#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "vectorscope_data.h"

namespace orc {

// ============================================================================
// Composite (measurement) vectorscope acquisition
// ============================================================================
// A technical vectorscope monitors the composite signal, not a decoded
// picture.  It product-detects chroma against a subcarrier reference recovered
// from the colour burst and plots the result with none of the processing a
// decoder applies to make a good picture:
//
//   * no delay-line / comb averaging between lines;
//   * no PAL V-switch correction, so +V and −V lines plot as two sets of
//     targets mirrored about the U axis (ITU-R BT.470-6 Table 2 item 2.16);
//   * no active-area restriction, so the burst on the back porch is in the
//     data set alongside the picture.
//
// The subcarrier reference is measured from the burst rather than assumed from
// the nominal subcarrier-cycles-per-line, which is what an instrument's
// burst-locked oscillator does.  A local straight-line fit over a few tens of
// lines acts as its flywheel: any drift in the line-start phase is tracked out
// — a source whose lines sit on a plain integer-sample grid drifts about half
// a degree per line, enough to smear a frame of colour bars into arcs — while
// per-line phase error is left visible as spread around the burst vectors.

/// Sampling options for a composite acquisition.
struct CompositeVectorscopeOptions {
  /// Portion of each line to sample.
  VectorscopeSampleWindow window = VectorscopeSampleWindow::WholeLine;

  /// Restrict the emitted lines to the active picture
  /// (SourceParameters::first_active_frame_line ..
  /// last_active_frame_line - 1), which is what the decoded acquisition plots.
  /// Applied on top of |first_line| / |last_line| — the two intersect.
  /// The burst survey and the instrument readouts are unaffected: they always
  /// run over every line of the frame that carries a burst.
  bool active_lines_only = false;

  /// Inclusive interlaced frame-line range (0-based), the numbering both
  /// acquisitions use — line 0 is the top line of the frame and consecutive
  /// numbers alternate fields.  |last_line| == 0 means "to the last line of
  /// the frame".  A range is contiguous in the picture, which a range in the
  /// flat buffer's sequential-field order would not be.
  uint32_t first_line = 0;
  uint32_t last_line = 0;

  /// Soft ceiling on emitted samples.  When the selected window would produce
  /// more, every n-th sample is taken instead; the divisor is reported back as
  /// VectorscopeData::sample_stride.  The default clears a whole PAL frame of
  /// whole lines (625 × 1135 = 709 375) so the commonest acquisition is not
  /// subsampled: dropping samples lengthens the chords the renderer draws
  /// between them, which coarsens the trace exactly where it is busiest.
  uint32_t max_samples = 800000;
};

/// Nominal colour-burst peak amplitude in IRE for |system|.
/// EBU Tech. 3280-E §1.2: PAL burst is 300 mV peak-to-peak against a 700 mV
/// luminance range → 21.43 IRE peak.
/// SMPTE 170M-2004 §8.4: NTSC burst is 40 IRE peak-to-peak → 20 IRE peak.
/// ITU-R BT.1700-1 Annex 1 Part B: PAL-M follows the 525-line levels.
double nominal_burst_amplitude_ire(VideoSystem system);

/// Subcarrier cycles per line for |system|, derived from the 4FSC frame
/// geometry: a frame holds frame_samples/4 subcarrier cycles spread over
/// frame_lines lines.  PAL gives 283.7516, NTSC 227.5, PAL-M 227.25.
/// Returns 0 for an unknown system.
double subcarrier_cycles_per_line(VideoSystem system);

/// Demodulate |frame| and produce measurement-vectorscope samples.
///
/// |frame| is a flat CVBS_U10_4FSC frame buffer (composite samples, or the
/// chroma plane of a Y/C source — the demodulator rejects the channel's DC
/// either way).  |frame_sample_count| bounds reads into it.
///
/// Returns nullopt when |parameters| does not describe a usable 4FSC frame.
///
/// Complexity: O(selected lines × line width).
std::optional<VectorscopeData> extract_composite_vectorscope(
    const int16_t* frame, size_t frame_sample_count,
    const SourceParameters& parameters, uint64_t field_number,
    const CompositeVectorscopeOptions& options);

/// Convenience wrapper: pulls the composite samples (or, for a Y/C source, the
/// chroma plane) for |frame_id| out of |representation| and demodulates them.
std::optional<VectorscopeData> extract_composite_vectorscope(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    const CompositeVectorscopeOptions& options);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_VECTORSCOPE_COMPOSITE_VECTORSCOPE_H
