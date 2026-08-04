/*
 * File:        vbi_resampler_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for band-limited resampling of VBI records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_resampler.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace orc {
namespace {

// bt8x8 PAL samples at 8 x fsc and the output lattice is 4 x fsc, so every
// card capture in the first milestone decimates by exactly two.
constexpr double kSourceRateHz = 35468950.0;
constexpr double kOutputRateHz = 17734475.0;
constexpr double kDecimationRatio = 2.0;
constexpr double kOutputNyquistHz = kOutputRateHz / 2.0;

// ETSI EN 300 706: the WST carrier is 444 x fH = 6.9375 MHz, which sits at
// 78 % of the output Nyquist frequency.  Losing it to the anti-alias filter
// or to its own alias would destroy the data the stage exists to carry.
constexpr double kWSTBitRateHz = 6937500.0;

constexpr double kPi = 3.14159265358979323846;

std::vector<double> sine_wave(double frequency_hz, double sample_rate_hz,
                              size_t sample_count, double phase_radians) {
  std::vector<double> samples(sample_count, 0.0);
  for (size_t index = 0; index < sample_count; ++index) {
    const double time = static_cast<double>(index) / sample_rate_hz;
    samples[index] = std::sin(2.0 * kPi * frequency_hz * time + phase_radians);
  }
  return samples;
}

double root_mean_square(const std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  double sum_of_squares = 0.0;
  for (const double sample : samples) {
    sum_of_squares += sample * sample;
  }
  return std::sqrt(sum_of_squares / static_cast<double>(samples.size()));
}

// Amplitude of a single tone after resampling, relative to the unit-amplitude
// input.  The tone is the only thing in the record, so its root mean square
// carries its amplitude whether or not it has folded to a new frequency.
double relative_tone_amplitude(const VBIBandLimitedResampler& resampler,
                               double frequency_hz) {
  constexpr size_t kSourceSamples = 6000;
  constexpr double kFirstPosition = 1000.37;
  constexpr uint32_t kOutputSamples = 1800;

  const std::vector<double> source =
      sine_wave(frequency_hz, kSourceRateHz, kSourceSamples, 0.31);

  std::vector<double> output;
  resampler.resample(source, kFirstPosition, kOutputSamples, output);

  const double input_rms = 1.0 / std::sqrt(2.0);
  return root_mean_square(output) / input_rms;
}

double to_decibels(double ratio) { return 20.0 * std::log10(ratio); }

TEST(VBIResampler, TheWSTCarrierSurvivesDecimationToFourTimesSubcarrier) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const double loss_db =
      -to_decibels(relative_tone_amplitude(resampler, kWSTBitRateHz));

  EXPECT_LT(loss_db, 0.5) << "WST carrier loss " << loss_db << " dB";
  EXPECT_GT(loss_db, -0.5) << "WST carrier gain " << -loss_db << " dB";
}

// The whole passband the data occupies must come through, not just the
// carrier: half the bit rate is where the clock run-in's energy sits.
TEST(VBIResampler, ThePassbandUpToTheCarrierIsFlatWithinHalfADecibel) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  constexpr int kSteps = 16;
  for (int step = 1; step <= kSteps; ++step) {
    const double frequency = kWSTBitRateHz * static_cast<double>(step) / kSteps;
    const double amplitude = relative_tone_amplitude(resampler, frequency);
    EXPECT_LT(std::abs(to_decibels(amplitude)), 0.5)
        << "frequency " << frequency << " Hz";
  }
}

// Sample dropping would put everything above the output Nyquist straight back
// onto the data.  Nothing up there may survive at a level that matters.
TEST(VBIResampler, EnergyAboveTheOutputNyquistIsAttenuatedBySixtyDecibels) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  // Up to 45 % of the source rate; beyond that the source itself is aliased
  // and the tone is no longer the frequency it was asked for.
  constexpr double kLowest = kOutputNyquistHz * 1.015;
  constexpr double kHighest = 0.45 * kSourceRateHz;
  constexpr int kSteps = 28;
  for (int step = 0; step <= kSteps; ++step) {
    const double frequency =
        kLowest + (kHighest - kLowest) * static_cast<double>(step) / kSteps;
    const double attenuation_db =
        -to_decibels(relative_tone_amplitude(resampler, frequency));
    EXPECT_GT(attenuation_db, 60.0) << "frequency " << frequency << " Hz";
  }
}

TEST(VBIResampler, TheDecimationCutoffSitsBelowTheOutputNyquist) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const double cutoff_hz =
      resampler.cutoff_fraction_of_source_rate() * kSourceRateHz;
  EXPECT_GT(cutoff_hz, kWSTBitRateHz);
  EXPECT_LT(cutoff_hz, kOutputNyquistHz);
}

// A resampler that ran late would bias every placement by the same amount,
// which is exactly the failure the calibrated capture offset is meant to
// remove.  The kernel is symmetric about the requested position, so there is
// no delay to compensate.
TEST(VBIResampler, TheFilterHasNoResidualGroupDelay) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  EXPECT_DOUBLE_EQ(resampler.group_delay_samples(), 0.0);
}

// Linear phase, stated as the property that matters: a symmetric pulse stays
// symmetric about its own centre rather than developing a leading or
// trailing tail.
TEST(VBIResampler, ASymmetricPulseStaysSymmetricAboutItsCentre) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  constexpr size_t kSourceSamples = 1200;
  constexpr double kCentre = 600.0;
  constexpr double kHalfWidth = 24.0;

  std::vector<double> source(kSourceSamples, 0.0);
  for (size_t index = 0; index < kSourceSamples; ++index) {
    const double offset = static_cast<double>(index) - kCentre;
    if (std::abs(offset) < kHalfWidth) {
      source[index] = 0.5 * (1.0 + std::cos(kPi * offset / kHalfWidth));
    }
  }

  for (int step = 1; step <= 120; ++step) {
    const double distance = 0.5 * static_cast<double>(step);
    const double before = resampler.sample_at(source, kCentre - distance);
    const double after = resampler.sample_at(source, kCentre + distance);
    EXPECT_NEAR(before, after, 1e-9) << "distance " << distance;
  }
}

// A constant region must resample to the same constant at every fractional
// phase.  The quiet back porch that logic 0 is read from is exactly such a
// region, so any phase-dependent gain would ripple the level reference.
TEST(VBIResampler, ConstantInputIsReproducedExactlyAtEveryFractionalPhase) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const std::vector<double> source(800, 137.25);
  for (int step = 0; step < 64; ++step) {
    const double position = 200.0 + static_cast<double>(step) / 64.0;
    EXPECT_NEAR(resampler.sample_at(source, position), 137.25, 1e-9)
        << "position " << position;
  }
}

// Positions off either end of the record read the nearest stored sample, so a
// quiet head or tail keeps its own level instead of being pulled towards an
// invented zero.
TEST(VBIResampler, PositionsOutsideTheRecordReadTheNearestStoredSample) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const std::vector<double> source(600, 42.0);

  EXPECT_NEAR(resampler.sample_at(source, -50.0), 42.0, 1e-9);
  EXPECT_NEAR(resampler.sample_at(source, 0.25), 42.0, 1e-9);
  EXPECT_NEAR(resampler.sample_at(source, 599.75), 42.0, 1e-9);
  EXPECT_NEAR(resampler.sample_at(source, 900.0), 42.0, 1e-9);
}

TEST(VBIResampler, AnEmptyRecordProducesNoSignal) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const std::vector<double> empty;
  EXPECT_DOUBLE_EQ(resampler.sample_at(empty, 10.0), 0.0);

  std::vector<double> output;
  resampler.resample(empty, 0.0, 8, output);
  ASSERT_EQ(output.size(), 8u);
  for (const double sample : output) {
    EXPECT_DOUBLE_EQ(sample, 0.0);
  }
}

// The regular-stride path and repeated single-position calls are the same
// operation; the placement code relies on being able to use either.
TEST(VBIResampler, TheStridedPathMatchesRepeatedSinglePositionCalls) {
  const VBIBandLimitedResampler resampler(kDecimationRatio);

  const std::vector<double> source =
      sine_wave(4000000.0, kSourceRateHz, 2000, 0.7);

  constexpr double kFirstPosition = 300.4;
  constexpr uint32_t kCount = 400;
  std::vector<double> strided;
  resampler.resample(source, kFirstPosition, kCount, strided);

  ASSERT_EQ(strided.size(), kCount);
  for (uint32_t index = 0; index < kCount; ++index) {
    const double position =
        kFirstPosition + static_cast<double>(index) * kDecimationRatio;
    EXPECT_NEAR(strided[index], resampler.sample_at(source, position), 1e-12)
        << "output sample " << index;
  }
}

// A source at or below the output lattice rate has no bandwidth to discard,
// so the cutoff follows the source's own Nyquist instead of the output's.
TEST(VBIResampler, AUnityRatioKeepsTheFullSourceBandwidth) {
  const VBIBandLimitedResampler passthrough(1.0);
  const VBIBandLimitedResampler decimator(kDecimationRatio);

  EXPECT_GT(passthrough.cutoff_fraction_of_source_rate(),
            decimator.cutoff_fraction_of_source_rate());
  EXPECT_LT(passthrough.cutoff_fraction_of_source_rate(), 0.5);
}

// A ratio that is not a usable number must not produce a kernel that silently
// misplaces every sample.
TEST(VBIResampler, AnUnusableRatioFallsBackToUnity) {
  const VBIBandLimitedResampler zero_ratio(0.0);
  const VBIBandLimitedResampler negative_ratio(-2.0);

  EXPECT_DOUBLE_EQ(zero_ratio.ratio(), 1.0);
  EXPECT_DOUBLE_EQ(negative_ratio.ratio(), 1.0);
}

}  // namespace
}  // namespace orc
