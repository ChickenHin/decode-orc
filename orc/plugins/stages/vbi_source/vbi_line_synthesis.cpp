/*
 * File:        vbi_line_synthesis.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Synthesises the sync and blanking of one output frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_synthesis.h"

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Fraction of a raised-cosine transition that separates its 10 % and 90 %
// points: (acos(-0.8) - acos(0.8)) / pi.
const double kBuildUpFractionOfTransition =
    (std::acos(-0.8) - std::acos(0.8)) / kPi;

// Rising half of a raised cosine over [0, 1].
double raised_cosine_ramp(double fraction) {
  if (fraction <= 0.0) {
    return 0.0;
  }
  if (fraction >= 1.0) {
    return 1.0;
  }
  return 0.5 * (1.0 - std::cos(kPi * fraction));
}

}  // namespace

double vbi_raised_cosine_gate(double position, double width,
                              double transition) {
  if (!std::isfinite(position) || !(width > 0.0)) {
    return 0.0;
  }
  if (!(transition > 0.0)) {
    // A step, which no caller should ask for but which must not divide by
    // zero if one does.
    return (position >= 0.0 && position < width) ? 1.0 : 0.0;
  }

  const double half = transition / 2.0;
  if (position <= -half || position >= width + half) {
    return 0.0;
  }

  const double rise = raised_cosine_ramp((position + half) / transition);
  const double fall =
      1.0 - raised_cosine_ramp((position - (width - half)) / transition);
  return std::min(rise, fall);
}

double vbi_raised_cosine_transition(double build_up) {
  if (!(build_up > 0.0) || !std::isfinite(build_up)) {
    return 0.0;
  }
  return build_up / kBuildUpFractionOfTransition;
}

VBILineSynthesiser::VBILineSynthesiser(VBIFrameGeometry geometry,
                                       VBIVerticalInterval vertical_interval,
                                       VBIOutputLevels levels,
                                       double output_sample_rate_hz)
    : geometry_(geometry),
      vertical_interval_(vertical_interval),
      levels_(levels),
      output_sample_rate_hz_(output_sample_rate_hz) {
  const double build_up_samples =
      vertical_interval_.timing().build_up_ns * 1e-9 * output_sample_rate_hz_;
  transition_samples_ = vbi_raised_cosine_transition(build_up_samples);
}

double VBILineSynthesiser::pulse_width_samples(VBISyncPulse pulse) const {
  return vertical_interval_.timing().width_ns(pulse) * 1e-9 *
         output_sample_rate_hz_;
}

double VBILineSynthesiser::level_at(uint32_t frame_line,
                                    double position) const {
  const double blanking = static_cast<double>(levels_.blanking);
  if (frame_line >= geometry_.lines_per_frame()) {
    return blanking;
  }

  // Time from this line's 0H, in output samples.  Sample 0 of a stored line is
  // the first sampling instant at or after 0H, so the line's lattice phase is
  // how far past 0H the first sample already is.
  const double time = geometry_.line_phase(frame_line) + position;
  const double half_line = geometry_.nominal_line_length() / 2.0;

  // The pulses that can reach a position on this line: the one before it, the
  // line's own two half-line periods, and the one that opens the next line.
  // Taking the deepest keeps the waveform continuous across every boundary,
  // including a leading edge whose first half belongs to the line before.
  double depth = 0.0;
  const int64_t first_half_line = static_cast<int64_t>(frame_line) * 2;
  for (int64_t step = -1; step <= 2; ++step) {
    const VBISyncPulse pulse =
        vertical_interval_.pulse_at_half_line(first_half_line + step);
    if (pulse == VBISyncPulse::kNone) {
      continue;
    }
    const double gate =
        vbi_raised_cosine_gate(time - static_cast<double>(step) * half_line,
                               pulse_width_samples(pulse), transition_samples_);
    depth = std::max(depth, gate);
  }

  return blanking + depth * (static_cast<double>(levels_.sync_tip) - blanking);
}

void VBILineSynthesiser::synthesise_line(
    uint32_t frame_line, std::vector<double>& out_samples) const {
  const uint32_t length = geometry_.line_length(frame_line);
  out_samples.assign(length, static_cast<double>(levels_.blanking));
  for (uint32_t index = 0; index < length; ++index) {
    out_samples[index] = level_at(frame_line, static_cast<double>(index));
  }
}

}  // namespace orc
