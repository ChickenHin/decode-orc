/*
 * File:        vbi_burst_synthesis.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Synthesises the colour burst of a CVBS frame line
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_burst_synthesis.h"

#include <algorithm>
#include <cmath>

#include "vbi_line_synthesis.h"

namespace orc {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ITU-R BT.1700 Annex 1 Part B Table 1 (625-line PAL): colour subcarrier
// frequency.
constexpr double kPALSubcarrierHz = 4433618.75;

// ITU-R BT.470-6 Table 2 item 2.14 g and h: the 625-line burst starts
// 5.6 us after epoch 0H and lasts 2.25 us, which is ten subcarrier cycles.
constexpr double kPALBurstStartNs = 5600.0;
constexpr double kPALBurstDurationNs = 2250.0;

// ITU-R BT.470-6 Table 2 item 2.16: the 625-line burst sits at 135 degrees to
// the +U axis, its V component reversing from line to line.
constexpr double kPALBurstPhaseDegrees = 135.0;

// ITU-R BT.470-6 Table 2 item 2.15: peak-to-peak burst amplitude is three
// sevenths of the difference between blanking and peak white for the 625-line
// PAL systems.
constexpr double kPALBurstAmplitudeNumerator = 3.0;
constexpr double kPALBurstAmplitudeDenominator = 7.0;

// ITU-R BT.470-6 Table 2 item 2.17 and Fig. 5a: the four burst-blanking
// windows of the 625-line PAL systems, as inclusive broadcast frame lines.
// Windows II and IV run past the end of the frame and continue into the next.
constexpr uint32_t kPALBlankingMidFrameEvenStart = 311;  // window I
constexpr uint32_t kPALBlankingMidFrameOddStart = 310;   // window III
constexpr uint32_t kPALBlankingBoundaryEvenStart = 623;  // window II
constexpr uint32_t kPALBlankingBoundaryOddStart = 622;   // window IV
constexpr uint32_t kPALBlankingWindowLines = 9;

}  // namespace

bool make_vbi_burst_timing(VBITVSystem tv_system,
                           const VBIOutputLevels& output_levels,
                           VBIBurstTiming& out_timing,
                           std::string& error_message) {
  out_timing = VBIBurstTiming();

  switch (tv_system) {
    case VBITVSystem::kPAL: {
      out_timing.start_ns = kPALBurstStartNs;
      out_timing.duration_ns = kPALBurstDurationNs;
      out_timing.subcarrier_hz = kPALSubcarrierHz;

      // One subcarrier period: the shortest taper that gates the burst without
      // spreading its energy across the band.
      out_timing.envelope_build_up_ns = 1e9 / kPALSubcarrierHz;

      out_timing.amplitude_counts =
          kPALBurstAmplitudeNumerator *
          (static_cast<double>(output_levels.white) -
           static_cast<double>(output_levels.blanking)) /
          kPALBurstAmplitudeDenominator;
      out_timing.phase_degrees = kPALBurstPhaseDegrees;
      out_timing.swinging = true;
      return true;
    }

    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      // The 525-line burst is a fixed 180 degree phase with a two-frame
      // sequence rather than a swinging one, and none of the rest of the
      // 525-line synthesis path exists yet.
      error_message =
          "Colour burst synthesis for 525-line systems is not implemented yet; "
          "only PAL frames can currently be synthesised.";
      return false;
  }

  error_message = "Unrecognised television system.";
  return false;
}

VBIBurstSynthesiser::VBIBurstSynthesiser(VBIFrameGeometry geometry,
                                         VBIOutputLevels levels,
                                         VBIBurstTiming timing,
                                         double output_sample_rate_hz)
    : geometry_(geometry),
      levels_(levels),
      timing_(timing),
      output_sample_rate_hz_(output_sample_rate_hz) {
  cycles_per_sample_ = (output_sample_rate_hz_ > 0.0)
                           ? timing_.subcarrier_hz / output_sample_rate_hz_
                           : 0.0;
  envelope_transition_samples_ = vbi_raised_cosine_transition(
      timing_.envelope_build_up_ns * 1e-9 * output_sample_rate_hz_);
}

int VBIBurstSynthesiser::swing_sign(uint64_t frame_index,
                                    uint32_t frame_line) const {
  if (!timing_.swinging) {
    return 1;
  }

  // Ordinal of the line in the whole output sequence.  A 625-line frame is an
  // odd number of lines, so strict alternation across the sequence arrives at
  // each frame with the opposite parity to the last, which is the two-frame
  // reversal of the standard's table.
  const uint64_t line_ordinal =
      frame_index * geometry_.lines_per_frame() + frame_line;
  return ((line_ordinal % 2u) == 0u) ? 1 : -1;
}

double VBIBurstSynthesiser::phase_degrees(uint64_t frame_index,
                                          uint32_t frame_line) const {
  return timing_.phase_degrees * swing_sign(frame_index, frame_line);
}

bool VBIBurstSynthesiser::is_blanked(uint64_t frame_index,
                                     uint32_t frame_line) const {
  if (frame_line >= geometry_.lines_per_frame()) {
    return false;
  }
  if (!timing_.swinging) {
    return false;
  }

  // Broadcast frame line: stored frame line 0 is broadcast line 1.
  const uint32_t line = frame_line + 1u;
  const uint32_t lines = geometry_.lines_per_frame();
  const bool even_frame = (frame_index % 2u) == 0u;

  // The window pairing is anchored at output frame 0.  Which of the four
  // windows a given output frame uses cannot be recovered from a synthesised
  // signal — there is no external colour-field reference — so what matters is
  // that the pairing alternates with the standard's four-field period, which
  // this does.
  const uint32_t mid_frame_start =
      even_frame ? kPALBlankingMidFrameEvenStart : kPALBlankingMidFrameOddStart;
  if (line >= mid_frame_start &&
      line < mid_frame_start + kPALBlankingWindowLines) {
    return true;
  }

  // The window that opens near the end of this frame.
  const uint32_t boundary_start =
      even_frame ? kPALBlankingBoundaryEvenStart : kPALBlankingBoundaryOddStart;
  if (line >= boundary_start) {
    return true;
  }

  // The tail of the window that opened in the frame before, which was of the
  // other parity.
  const uint32_t previous_boundary_start =
      even_frame ? kPALBlankingBoundaryOddStart : kPALBlankingBoundaryEvenStart;
  const uint32_t tail_lines =
      kPALBlankingWindowLines - (lines - previous_boundary_start + 1u);
  return line <= tail_lines;
}

void VBIBurstSynthesiser::synthesise_burst(
    uint64_t frame_index, uint32_t frame_line,
    std::vector<double>& line_samples) const {
  if (frame_line >= geometry_.lines_per_frame() || line_samples.empty()) {
    return;
  }
  if (is_blanked(frame_index, frame_line) || !(timing_.duration_ns > 0.0)) {
    return;
  }

  const double line_phase = geometry_.line_phase(frame_line);
  const double start_samples =
      timing_.start_ns * 1e-9 * output_sample_rate_hz_ - line_phase;
  const double width_samples =
      timing_.duration_ns * 1e-9 * output_sample_rate_hz_;
  const double half_transition = envelope_transition_samples_ / 2.0;

  const double first = std::floor(start_samples - half_transition);
  const double last =
      std::ceil(start_samples + width_samples + half_transition);
  const uint32_t begin = static_cast<uint32_t>(std::max(0.0, first));
  const uint32_t end = static_cast<uint32_t>(
      std::min(static_cast<double>(line_samples.size()), std::max(0.0, last)));

  const double phase_radians =
      phase_degrees(frame_index, frame_line) * kPi / 180.0;
  const double peak = timing_.amplitude_counts / 2.0;

  // Absolute sample index of the line's first sample in the output sequence.
  // Subcarrier phase is a function of it and of nothing else, which is what
  // makes the progression coherent across lines, fields and frames.
  const double base_sample =
      static_cast<double>(frame_index) *
          static_cast<double>(geometry_.samples_per_frame()) +
      static_cast<double>(geometry_.line_start(frame_line));

  for (uint32_t index = begin; index < end; ++index) {
    const double envelope =
        vbi_raised_cosine_gate(static_cast<double>(index) - start_samples,
                               width_samples, envelope_transition_samples_);
    if (envelope <= 0.0) {
      continue;
    }
    const double cycles =
        cycles_per_sample_ * (base_sample + static_cast<double>(index));
    const double angle = 2.0 * kPi * (cycles - std::floor(cycles));
    line_samples[index] += peak * envelope * std::sin(angle + phase_radians);
  }
}

}  // namespace orc
