/*
 * File:        vbi_burst_synthesis.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Synthesises the colour burst of a CVBS frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_BURST_SYNTHESIS_H
#define ORC_VBI_BURST_SYNTHESIS_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_frame_geometry.h"
#include "vbi_output_levels.h"
#include "vbi_source_format.h"

namespace orc {

// Placement, amplitude and phase of a television system's colour burst.
struct VBIBurstTiming {
  // ITU-R BT.470-6 Table 2 item 2.14 g: start of the burst after epoch 0H.
  double start_ns = 0.0;

  // ITU-R BT.470-6 Table 2 item 2.14 h: duration of the burst.
  double duration_ns = 0.0;

  // Build-up of the burst envelope, 10 % to 90 %.  A gated sine with square
  // ends would spread energy across the whole band; a real burst is gated
  // gently and a synthesised one should be too.
  double envelope_build_up_ns = 0.0;

  // Peak-to-peak amplitude in output counts.
  double amplitude_counts = 0.0;

  // Colour subcarrier frequency in hertz.
  double subcarrier_hz = 0.0;

  // ITU-R BT.470-6 Table 2 item 2.16: nominal burst phase relative to the
  // +U axis.  PAL's burst swings between +135 and -135 degrees.
  double phase_degrees = 0.0;

  // True when the sign of the phase above alternates from line to line, which
  // is the PAL swinging burst.
  bool swinging = false;

  double end_ns() const { return start_ns + duration_ns; }
};

// Burst parameters of a television system, in the output amplitude domain.
// Returns false with an error message for systems the stage does not yet
// synthesise.
bool make_vbi_burst_timing(VBITVSystem tv_system,
                           const VBIOutputLevels& output_levels,
                           VBIBurstTiming& out_timing,
                           std::string& error_message);

// Synthesises the colour burst onto frame lines.
//
// The source carries at most a fragment of the original burst — a bt8x8 PAL
// record opens at 6.879 us, inside the burst window, so samples 0 to 34 hold
// its tail — which is far too little to preserve.  So the burst is either
// omitted or manufactured, and manufacturing it is both the more faithful
// reconstruction and what permits the output to claim a locked signal state
// (design §5.6, §2.4).
//
// Phase is a function of the absolute sample index in the output sequence.
// That is what makes the progression coherent for nothing: the output lattice
// is exactly four times the subcarrier frequency, so subcarrier phase advances
// by exactly 90 degrees per sample, and a PAL frame of 709 379 samples is not
// a whole number of subcarrier cycles, so the four-frame phase progression
// falls out of the arithmetic rather than being imposed on it.  The absolute
// phase of the first output frame has no external reference in a synthesised
// signal, so it is defined here as zero at its first sample.
//
// Thread-compatible: instances hold configuration only.
class VBIBurstSynthesiser {
 public:
  VBIBurstSynthesiser(VBIFrameGeometry geometry, VBIOutputLevels levels,
                      VBIBurstTiming timing, double output_sample_rate_hz);

  const VBIBurstTiming& timing() const { return timing_; }

  // Sign of the burst's V component on a frame line: +1 for a +135 degree
  // burst and -1 for -135 degrees.
  //
  // ITU-R BT.470-6 Table 2 item 2.16: the sign alternates line to line and
  // reverses every two fields, which the 625-line frame produces on its own —
  // an odd number of lines to a frame means strict line-to-line alternation
  // arrives at the next frame with the opposite parity.
  int swing_sign(uint64_t frame_index, uint32_t frame_line) const;

  // Burst phase on a frame line, in degrees relative to the +U axis.
  double phase_degrees(uint64_t frame_index, uint32_t frame_line) const;

  // True when the burst-blanking sequence suppresses the burst on a frame
  // line.
  //
  // ITU-R BT.470-6 Table 2 item 2.17 and Fig. 5a: the burst is blanked over
  // nine lines of each field-blanking interval, and the window meanders by one
  // line with a four-field period so that the line-to-line phase alternation
  // resumes in the same sense after every interval.
  bool is_blanked(uint64_t frame_index, uint32_t frame_line) const;

  // Write the burst over a frame line's samples.  Does nothing when the line
  // is inside the burst-blanking sequence, so the caller's blanking stands.
  void synthesise_burst(uint64_t frame_index, uint32_t frame_line,
                        std::vector<double>& line_samples) const;

 private:
  VBIFrameGeometry geometry_;
  VBIOutputLevels levels_;
  VBIBurstTiming timing_;
  double output_sample_rate_hz_ = 0.0;

  // Subcarrier cycles per output sample: exactly a quarter for a 4 x fsc
  // lattice.
  double cycles_per_sample_ = 0.0;

  double envelope_transition_samples_ = 0.0;
};

}  // namespace orc

#endif  // ORC_VBI_BURST_SYNTHESIS_H
