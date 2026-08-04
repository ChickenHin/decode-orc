/*
 * File:        vbi_cri_template.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Clock run-in and framing code templates for timing recovery
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_CRI_TEMPLATE_H
#define ORC_VBI_CRI_TEMPLATE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi_teletext_service.h"

namespace orc {

// Shape of a generated template.
//
// The defaults describe a clean source: broadcast, off-air or LaserDisc
// material, where an ideal band-limited pattern matches the recorded waveform
// closely.  A tape-sourced capture wants a measured template instead
// (design §5.3.6), which is what make_vbi_measured_template() is for.
struct VBICRITemplateConfig {
  // Channel blur applied to the ideal pattern, as the standard deviation of a
  // Gaussian impulse response in bit periods.  A little blur is not optional:
  // a square template correlates poorly against any real channel, and its
  // edges carry energy the sampled waveform cannot contain.
  //
  // The default is what a 5 MHz channel does to a 6.9375 Mbit/s signal — a
  // rise time of roughly 0.35 / 5 MHz, which is half a bit period — and it is
  // also what correlates best against the reference capture, where it lifts
  // the median peak from 0.60 to 0.73.
  double blur_bit_periods = 0.4;

  // Logic 0 held ahead of the pattern's first bit, in bit periods.  It gives
  // the leading edge a defined shape rather than starting the template
  // part-way through a transition, and it matches the back porch the run-in
  // actually rises out of.
  double lead_in_bits = 1.0;
};

// A correlation template in one source's sample coordinates.
//
// Samples are zero-mean with unit Euclidean norm, which is what makes a
// correlation against a record a normalised one: neither the record's own
// black level nor its gain can influence where the peak lands, so no separate
// level normalisation pass is needed ahead of the search (design §5.3.4).
struct VBICRITemplate {
  std::vector<double> samples;

  double sample_rate_hz = 0.0;
  double bit_rate_hz = 0.0;
  double samples_per_bit = 0.0;

  // Template coordinate of the leading edge of the pattern's first one bit,
  // which is the point every service's 0H offset is measured to.  Patterns
  // that open with a zero bit therefore anchor at their first one, not at
  // their first bit.
  double anchor_samples = 0.0;

  uint32_t bit_count = 0;

  // True when the template was measured from a real waveform rather than
  // generated from the ideal pattern.
  bool measured = false;

  size_t size() const { return samples.size(); }

  bool empty() const { return samples.empty(); }
};

// Build a template from a bit pattern given in transmission order, most
// significant bit first over bit_count bits.
//
// Returns false with an error message for an empty pattern, a non-positive
// rate, or a sampling rate that cannot represent the pattern: an alternating
// run-in is a tone at half the bit rate, so fewer than two samples per bit
// leaves nothing to correlate against.
bool make_vbi_pattern_template(uint32_t pattern, uint32_t bit_count,
                               double bit_rate_hz, double sample_rate_hz,
                               const VBICRITemplateConfig& config,
                               VBICRITemplate& out_template,
                               std::string& error_message);

// Build the combined clock run-in and framing code template of a data service
// (design §5.3.2).
//
// The two parts are generated as one pattern rather than separately, because
// the framing code is what makes the position unambiguous: an alternating
// run-in correlates just as well against itself shifted by two bit periods,
// which at 4 x fsc PAL is five samples of positional uncertainty and quite
// enough to place the data in the wrong byte phase (design §5.3.1).
bool make_vbi_cri_frc_template(const VBITeletextService& service,
                               double sample_rate_hz,
                               const VBICRITemplateConfig& config,
                               VBICRITemplate& out_template,
                               std::string& error_message);

// Build a template from a measured run-in and framing code waveform.
//
// This is the seam for sources whose channel has departed too far from an
// ideal one for a generated pattern to match — a VHS capture blurred to
// sigma of about 0.8 bit periods has a run-in at five per cent of the data
// amplitude, and its correlation peak against a square template is both weak
// and biased (design §5.3.6).  The waveform is resampled from its own
// sampling rate onto this source's, so a template measured once at any
// convenient rate serves every format.
//
// waveform_anchor_samples locates the leading edge of the first one bit in the
// measured waveform's own coordinates.
bool make_vbi_measured_template(const std::vector<double>& waveform,
                                double waveform_samples_per_bit,
                                double waveform_anchor_samples,
                                uint32_t bit_count, double bit_rate_hz,
                                double sample_rate_hz,
                                VBICRITemplate& out_template,
                                std::string& error_message);

// Normalised autocorrelation of a template at a lag in samples.
//
// One at zero lag by construction.  The figure at a lag of two bit periods is
// the one that matters: it is how much of the run-in's self-similarity the
// framing code has broken, and therefore whether a correlation peak means an
// absolute position or one of several equally good ones.
double vbi_template_autocorrelation(const VBICRITemplate& tmpl,
                                    double lag_samples);

}  // namespace orc

#endif  // ORC_VBI_CRI_TEMPLATE_H
