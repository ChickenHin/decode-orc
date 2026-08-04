/*
 * File:        vbi_line_synthesis.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Synthesises the sync and blanking of one output frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LINE_SYNTHESIS_H
#define ORC_VBI_LINE_SYNTHESIS_H

#include <cstdint>
#include <vector>

#include "vbi_frame_geometry.h"
#include "vbi_output_levels.h"
#include "vbi_vertical_interval.h"

namespace orc {

// A raised-cosine gated pulse, evaluated at a position in samples.
//
// The pulse is one between its half-amplitude points at 0 and width, zero
// outside, and moves between the two over a raised-cosine transition centred
// on each of those points.  Centring the transition is what makes the width a
// measurement between half-amplitude points, which is how every pulse duration
// in ITU-R BT.470-6 is defined, and what makes a line's leading edge continue
// smoothly from the end of the line before it.
double vbi_raised_cosine_gate(double position, double width, double transition);

// Full duration of a raised-cosine transition whose 10 % to 90 % build-up
// time is build_up.
//
// A raised-cosine ramp of total duration T reaches 10 % at
// T x acos(0.8) / pi and 90 % at T x acos(-0.8) / pi, so the build-up time
// that standards quote is 0.5903 T.  Standards specify the build-up rather
// than the total, so the conversion belongs here rather than in a comment
// beside a hand-tuned number.
double vbi_raised_cosine_transition(double build_up);

// Synthesises the manufactured part of an output frame line: the
// synchronising pulses and the blanking that surrounds the data region.
//
// Everything outside the teletext data region is manufactured, because a raw
// VBI capture contains none of it (design §5.6).  A line that carries no data
// becomes an ordinary blank line by the same path, which is what makes the
// output a legal black raster rather than a frame full of structurally odd
// lines.
//
// Positions are evaluated against the frame geometry and nothing else: PAL is
// not orthogonal, so a line's samples sit at their own sub-sample offset from
// that line's 0H, and a synthesiser that assumed a constant stride would place
// every pulse in the lower part of the frame a fraction of a sample early
// (design §2.3).
//
// Thread-compatible: instances hold configuration only and every member is
// const.
class VBILineSynthesiser {
 public:
  VBILineSynthesiser(VBIFrameGeometry geometry,
                     VBIVerticalInterval vertical_interval,
                     VBIOutputLevels levels, double output_sample_rate_hz);

  const VBIFrameGeometry& geometry() const { return geometry_; }

  const VBIVerticalInterval& vertical_interval() const {
    return vertical_interval_;
  }

  const VBIOutputLevels& levels() const { return levels_; }

  double output_sample_rate_hz() const { return output_sample_rate_hz_; }

  // Width of a synchronising pulse in output samples.
  double pulse_width_samples(VBISyncPulse pulse) const;

  // Full duration of a synchronising edge in output samples.
  double transition_samples() const { return transition_samples_; }

  // Level of the synthesised waveform at a position on a frame line, measured
  // in output samples from that line's first stored sample.
  //
  // Fractional positions are meaningful: the edges are continuous functions of
  // time, so this is the waveform rather than an interpolation of it.
  double level_at(uint32_t frame_line, double position) const;

  // Fill a frame line with its synchronising pulses and blanking.  The buffer
  // is resized to the line's own length, which is not constant for PAL.
  void synthesise_line(uint32_t frame_line,
                       std::vector<double>& out_samples) const;

 private:
  VBIFrameGeometry geometry_;
  VBIVerticalInterval vertical_interval_;
  VBIOutputLevels levels_;
  double output_sample_rate_hz_ = 0.0;
  double transition_samples_ = 0.0;
};

}  // namespace orc

#endif  // ORC_VBI_LINE_SYNTHESIS_H
