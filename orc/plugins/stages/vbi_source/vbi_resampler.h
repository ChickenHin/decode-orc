/*
 * File:        vbi_resampler.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Band-limited resampling of VBI records onto the output lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_RESAMPLER_H
#define ORC_VBI_RESAMPLER_H

#include <cstdint>
#include <vector>

namespace orc {

// The seam through which every stored sample reaches the output lattice.
//
// A resampler is asked for the waveform at arbitrary, generally fractional,
// source-sample coordinates.  That is what lets the whole of the horizontal
// placement problem — the data service's 0H offset, the calibrated capture
// offset and the line's sub-sample lattice phase — collapse into the position
// arithmetic of a single filtering pass, with no separate interpolation stage
// and so no cumulative error (design §5.5).
//
// Implementations must be linear-phase with their group delay already
// compensated: a value asked for at position p is the waveform at p, not at p
// plus a filter delay.  Placement accuracy depends on it, so the property is
// part of the contract rather than a caller's responsibility.
//
// Implementations perform low-pass filtering and linear interpolation and
// nothing else.  No slicing, sharpening, level restoration or re-quantisation
// may happen here: a deconvolving slicer downstream recovers data by matching
// the blurred waveform it is given, and conditioning would destroy exactly the
// information it depends on (design §5.3.6).
class IVBIResampler {
 public:
  virtual ~IVBIResampler() = default;

  // Source samples advanced per output sample.  Two for the 8 x fsc card
  // captures, one for a TBC-derived source already on the output lattice.
  virtual double ratio() const = 0;

  // Residual group delay, in source samples.  Zero for any implementation
  // that satisfies the contract above; exposed so a violation is visible
  // rather than silently biasing every placement.
  virtual double group_delay_samples() const = 0;

  // Waveform value at one source-sample coordinate.  Positions outside the
  // sample array read the nearest stored sample.
  virtual double sample_at(const std::vector<double>& source,
                           double position) const = 0;

  // Waveform at count positions starting at first_position and advancing by
  // ratio().  Equivalent to repeated sample_at() calls; separate so an
  // implementation can exploit the regular stride.
  virtual void resample(const std::vector<double>& source,
                        double first_position, uint32_t count,
                        std::vector<double>& out_samples) const = 0;
};

// Shape of the band-limited interpolation kernel.
//
// The defaults are the 8 x fsc to 4 x fsc case the card captures need.  With a
// cutoff at 90 % of the output Nyquist the transition band closes about
// 8.7 MHz, which leaves the 6.9375 MHz WST carrier well inside the passband
// while putting everything that would fold onto it into the stopband
// (design §5.5).
struct VBIResamplerConfig {
  // Kernel half-width in source samples; the filter uses twice this many
  // taps.  This sets how narrow the transition band can be, and 128 taps
  // gives about 80 dB of stopband for the 2:1 case.
  uint32_t half_width_samples = 64;

  // Kaiser window shape parameter.  Trades passband ripple and stopband
  // attenuation against transition width for a given tap count.
  double kaiser_beta = 8.0;

  // Cutoff as a fraction of the output Nyquist frequency.  Below one so the
  // transition band lands inside the output band instead of straddling
  // Nyquist and folding onto itself.
  double cutoff_fraction = 0.90;

  // Kernel table steps per source sample.  The table is read with linear
  // interpolation, so this bounds the phase quantisation of a fractional
  // delay well below the accuracy placement needs.
  uint32_t phase_steps = 512;
};

// Low-pass-and-resample in one pass, using a Kaiser-windowed sinc kernel
// evaluated at fractional positions.
//
// Decimating an 8 x fsc capture by dropping every other sample would alias the
// 6.9375 MHz WST carrier straight back onto itself, because it sits at 78 % of
// the 8.87 MHz output Nyquist; the anti-alias filter is not optional
// (design §5.5).  Filtering and fractional delay are the same operation here:
// the kernel is centred on the requested position, which both band-limits and
// interpolates, and its symmetry is what makes the group delay zero.
//
// Each output sample is divided by the sum of the kernel weights actually
// applied, so the response at DC is exactly one at every fractional phase.
// Without that, a constant region such as the quiet back porch would ripple by
// the kernel's phase-to-phase gain variation.
//
// The kernel table is built once at construction.  Instances are immutable
// afterwards and const member functions are safe to call concurrently.
class VBIBandLimitedResampler : public IVBIResampler {
 public:
  explicit VBIBandLimitedResampler(double source_samples_per_output_sample,
                                   VBIResamplerConfig config = {});

  double ratio() const override { return ratio_; }

  double group_delay_samples() const override { return 0.0; }

  const VBIResamplerConfig& config() const { return config_; }

  // Filter cutoff as a fraction of the source sampling rate.
  double cutoff_fraction_of_source_rate() const { return cutoff_normalised_; }

  double sample_at(const std::vector<double>& source,
                   double position) const override;

  void resample(const std::vector<double>& source, double first_position,
                uint32_t count,
                std::vector<double>& out_samples) const override;

 private:
  // Kernel weight at a distance of offset source samples from the centre.
  double kernel_weight(double offset) const;

  double ratio_ = 1.0;
  VBIResamplerConfig config_;
  double cutoff_normalised_ = 0.0;

  // Kernel sampled at 1 / phase_steps source-sample increments over
  // [-half_width, +half_width], with one extra entry so the last interval can
  // be interpolated.
  std::vector<double> kernel_table_;
};

}  // namespace orc

#endif  // ORC_VBI_RESAMPLER_H
