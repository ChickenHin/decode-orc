/*
 * File:        vbi_timing_cross_checks.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Corroborates a fitted capture offset against other references
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_timing_cross_checks.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

#include "vbi_cri_correlator.h"
#include "vbi_frame_geometry.h"
#include "vbi_line_mapping.h"

namespace orc {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// PAL colour burst window, in nanoseconds from 0H (design §5.6).
constexpr double kPALBurstBeginNs = 5600.0;
constexpr double kPALBurstEndNs = 7850.0;

// NTSC colour burst window, in nanoseconds from 0H (design §5.6).
constexpr double kNTSCBurstBeginNs = 5300.0;
constexpr double kNTSCBurstEndNs = 7800.0;

// Multiple of the post-burst noise floor the burst remnant must reach before a
// record is counted.  A record whose capture window opens after the burst has
// nothing to measure and must not contribute a spurious edge.
constexpr double kBurstDetectionMargin = 3.0;

// Bits, counted from the run-in, over which a line's own modulation depth is
// measured.  The window starts past the framing code so that the reference is
// the packet's own data: the run-in is the densest transition sequence in the
// line and taking it as the reference would set the threshold above anything
// the data reaches.
constexpr double kActivityReferenceBeginBits = 24.0;
constexpr double kActivityReferenceEndBits = 64.0;

// How far past the expected end of modulation the search for its trailing edge
// runs, in bit periods.  Comfortably more than the tolerance the check
// applies, and inside the stored record of every format in scope.
constexpr double kActivitySearchMarginBits = 12.0;

// Room held clear of the start of the record when a reference service's
// template lead-in has to be shortened to fit, in bit periods.
constexpr double kLeadInMarginBits = 0.1;

std::string format_number(double value, int precision) {
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2u;
  if ((values.size() % 2u) == 1u) {
    return values[middle];
  }
  return 0.5 * (values[middle - 1u] + values[middle]);
}

// Amplitude of a known-frequency sinusoid over a sliding window, sampled at
// every window start position in [begin, end).
//
// The window's own mean is removed before the product sums, so a line's black
// level cannot masquerade as carrier energy however the window falls relative
// to the subcarrier period.
std::vector<double> sliding_carrier_amplitude(
    const std::vector<double>& samples, size_t begin, size_t end,
    double cycles_per_sample, size_t window) {
  std::vector<double> amplitudes;
  if (window == 0 || end <= begin || end > samples.size() ||
      (end - begin) < window) {
    return amplitudes;
  }

  amplitudes.reserve(end - begin - window + 1u);
  for (size_t start = begin; start + window <= end; ++start) {
    double total = 0.0;
    for (size_t index = 0; index < window; ++index) {
      total += samples[start + index];
    }
    const double mean = total / static_cast<double>(window);

    double in_phase = 0.0;
    double quadrature = 0.0;
    for (size_t index = 0; index < window; ++index) {
      const double angle =
          kTwoPi * cycles_per_sample * static_cast<double>(start + index);
      const double value = samples[start + index] - mean;
      in_phase += value * std::cos(angle);
      quadrature += value * std::sin(angle);
    }
    amplitudes.push_back(
        2.0 * std::sqrt(in_phase * in_phase + quadrature * quadrature) /
        static_cast<double>(window));
  }
  return amplitudes;
}

// Mean absolute sample-to-sample change over a sliding window: how much the
// waveform is moving, which is what modulation looks like without assuming
// anything about the data it carries.
std::vector<double> sliding_activity(const std::vector<double>& samples,
                                     size_t begin, size_t end, size_t window) {
  std::vector<double> activity;
  if (window == 0 || end <= begin || end > samples.size() ||
      (end - begin) < window + 1u) {
    return activity;
  }

  activity.reserve(end - begin - window);
  for (size_t start = begin; start + window < end; ++start) {
    double total = 0.0;
    for (size_t index = 0; index < window; ++index) {
      total += std::abs(samples[start + index + 1u] - samples[start + index]);
    }
    activity.push_back(total / static_cast<double>(window));
  }
  return activity;
}

// Position, in indices of the supplied series, at which it last falls through
// a threshold.  Returns false when it never does, which means whatever was
// being measured did not end inside the series.
bool trailing_threshold_crossing(const std::vector<double>& series,
                                 double threshold, size_t search_from,
                                 double& out_position) {
  if (series.size() < 2u || search_from + 1u >= series.size()) {
    return false;
  }

  for (size_t index = series.size() - 1u; index > search_from; --index) {
    if (series[index - 1u] >= threshold && series[index] < threshold) {
      const double span = series[index - 1u] - series[index];
      const double fraction =
          (span > 0.0) ? ((series[index - 1u] - threshold) / span) : 0.0;
      out_position = static_cast<double>(index - 1u) + fraction;
      return true;
    }
  }
  return false;
}

// Broadcast frame line a record sits on, 1-based, or zero when the record is
// outside the configured range.
uint32_t broadcast_line_of(const VBISourceFormat& format,
                           const VBITeletextLineMap& line_map,
                           const VBILineRecord& record) {
  uint32_t frame_line = 0;
  std::string error;
  if (!map_vbi_record_to_frame_line(format, line_map, record.field_index,
                                    record.record_index, frame_line, error)) {
    return 0;
  }
  return frame_line + 1u;
}

// The burst remnant check (design §5.3.5).
//
// A bt8x8 PAL capture window opens at about 6.879 us, inside the 5.6 to
// 7.85 us burst window, so a tail of burst appears at the start of every
// record — including records with no teletext on them at all.  Its trailing
// edge is at a known time from 0H, which makes it an offset estimate that owes
// nothing to teletext being present.
VBITimingCrossCheck check_burst_remnant(
    const VBISourceFormat& format, double capture_offset_samples,
    const std::vector<VBILineRecord>& records,
    const VBICrossCheckConfig& config) {
  VBITimingCrossCheck check;
  check.name = "Colour burst remnant";
  check.tolerance_samples = config.burst_tolerance_samples;

  double burst_begin_ns = 0.0;
  double burst_end_ns = 0.0;
  double output_rate_hz = 0.0;
  std::string error;
  if (!vbi_colour_burst_window_ns(format.tv_system, burst_begin_ns,
                                  burst_end_ns) ||
      !vbi_output_sample_rate_hz(format.tv_system, output_rate_hz, error)) {
    check.message =
        "No colour burst timing is defined for the configured television "
        "system, so the burst remnant cannot corroborate the capture offset.";
    return check;
  }

  const double subcarrier_hz = output_rate_hz / 4.0;
  const double cycles_per_sample = subcarrier_hz / format.sample_rate_hz;
  const double burst_end_in_record =
      burst_end_ns * 1e-9 * format.sample_rate_hz - capture_offset_samples;
  const double burst_begin_in_record =
      burst_begin_ns * 1e-9 * format.sample_rate_hz - capture_offset_samples;

  check.expected_samples = burst_end_in_record;

  const size_t window =
      static_cast<size_t>(std::max(4.0, std::round(1.0 / cycles_per_sample)));
  const double search_end = burst_end_in_record +
                            4.0 * config.burst_tolerance_samples +
                            static_cast<double>(window);
  if (!(burst_end_in_record > static_cast<double>(window)) ||
      burst_begin_in_record >= burst_end_in_record ||
      search_end >= static_cast<double>(format.valid_samples)) {
    check.message =
        "The capture window does not open inside the colour burst, so there is "
        "no burst remnant to measure.";
    return check;
  }

  std::vector<double> measurements;
  for (const VBILineRecord& record : records) {
    const std::vector<double> amplitude = sliding_carrier_amplitude(
        record.samples, 0u, static_cast<size_t>(search_end), cycles_per_sample,
        window);
    if (amplitude.size() < 4u) {
      continue;
    }

    // The burst is the strongest subcarrier-frequency feature at the head of
    // the record; everything after it is back porch.
    const auto peak = std::max_element(amplitude.begin(), amplitude.end());
    const size_t peak_index =
        static_cast<size_t>(std::distance(amplitude.begin(), peak));
    const size_t floor_begin =
        std::min(amplitude.size() - 1u,
                 static_cast<size_t>(burst_end_in_record +
                                     config.burst_tolerance_samples));
    const std::vector<double> floor_window(
        amplitude.begin() + static_cast<ptrdiff_t>(floor_begin),
        amplitude.end());
    const double noise_floor = median_of(floor_window);
    if (!(*peak > kBurstDetectionMargin * noise_floor)) {
      continue;
    }

    double crossing = 0.0;
    if (!trailing_threshold_crossing(amplitude,
                                     config.burst_edge_fraction * *peak,
                                     peak_index, crossing)) {
      continue;
    }

    // A boxcar detector's half-amplitude crossing sits where the window's
    // centre passes the edge, so the edge itself is half a window later than
    // the window start the crossing was found at.
    measurements.push_back(crossing + 0.5 * static_cast<double>(window));
  }

  check.records_used = measurements.size();
  if (measurements.size() < config.minimum_records) {
    check.message = "The colour burst remnant was measurable on only " +
                    std::to_string(measurements.size()) +
                    " records, too few to corroborate the capture offset.";
    return check;
  }

  check.measured_samples = median_of(std::move(measurements));
  const double disagreement =
      std::abs(check.measured_samples - check.expected_samples);
  if (disagreement <= check.tolerance_samples) {
    check.outcome = VBICrossCheckOutcome::kAgreed;
    check.message = "The colour burst remnant ends at sample " +
                    format_number(check.measured_samples, 2) +
                    " of the record, agreeing with the " +
                    format_number(check.expected_samples, 2) +
                    " the fitted capture offset predicts.";
    return check;
  }

  check.outcome = VBICrossCheckOutcome::kDisagreed;
  check.message =
      "The colour burst remnant ends at sample " +
      format_number(check.measured_samples, 2) +
      " of the record, but the fitted capture offset puts the end of the burst "
      "window at sample " +
      format_number(check.expected_samples, 2) + " — a disagreement of " +
      format_number(check.measured_samples - check.expected_samples, 2) +
      " samples. The configured sampling rate or the fitted offset is "
      "suspect.";
  return check;
}

// The other-services check (design §5.3.5).
//
// A PAL capture covering broadcast lines 7 to 22 also covers the video
// programme system on line 16 and closed captions on line 22.  Each carries
// its own run-in at its own standardised time from 0H, so each yields a
// capture offset estimate that is independent of teletext entirely.
std::vector<VBITimingCrossCheck> check_reference_services(
    const VBISourceFormat& format, double capture_offset_samples,
    const std::vector<VBILineRecord>& records,
    const VBICrossCheckConfig& config) {
  std::vector<VBITimingCrossCheck> checks;

  const std::vector<VBIReferenceService> services =
      vbi_reference_services(format.tv_system);
  if (services.empty()) {
    return checks;
  }

  VBITeletextLineMap line_map;
  std::string error;
  if (!make_vbi_teletext_line_map(format.tv_system, format.tt_system, line_map,
                                  error)) {
    return checks;
  }

  for (const VBIReferenceService& service : services) {
    VBITimingCrossCheck check;
    check.name = service.name + " (broadcast line " +
                 std::to_string(service.broadcast_line) + ")";
    check.tolerance_samples = config.service_tolerance_samples;
    check.expected_samples = capture_offset_samples;

    std::vector<const VBILineRecord*> service_records;
    for (const VBILineRecord& record : records) {
      if (broadcast_line_of(format, line_map, record) ==
          service.broadcast_line) {
        service_records.push_back(&record);
      }
    }
    if (service_records.size() < config.minimum_records) {
      check.message = "The capture does not cover broadcast line " +
                      std::to_string(service.broadcast_line) + ", so " +
                      service.name + " cannot corroborate the capture offset.";
      checks.push_back(std::move(check));
      continue;
    }

    const double predicted_anchor =
        service.t_offset_ns * 1e-9 * format.sample_rate_hz -
        capture_offset_samples;

    VBICRITemplateConfig template_config = config.template_config;
    VBICRITemplate service_template;
    if (!make_vbi_pattern_template(service.pattern, service.pattern_bits,
                                   service.bit_rate_hz, format.sample_rate_hz,
                                   template_config, service_template, error)) {
      check.message = "No " + service.name +
                      " template could be generated for this source's "
                      "sampling rate: " +
                      error;
      checks.push_back(std::move(check));
      continue;
    }

    // These services run at a fraction of teletext's bit rate, so one bit
    // period of lead-in is tens of samples and can reach back past the start
    // of a card capture's record.  Shorten it to what the record has room for
    // rather than searching a window the template cannot sit in.
    const double room = predicted_anchor -
                        config.service_search_tolerance_samples -
                        service_template.anchor_samples;
    if (room < 0.0) {
      const double reduction_bits =
          (-room) / service_template.samples_per_bit + kLeadInMarginBits;
      template_config.lead_in_bits =
          config.template_config.lead_in_bits - reduction_bits;
      if (template_config.lead_in_bits < 0.0 ||
          !make_vbi_pattern_template(service.pattern, service.pattern_bits,
                                     service.bit_rate_hz, format.sample_rate_hz,
                                     template_config, service_template,
                                     error)) {
        check.message =
            "The capture window opens too late for the " + service.name +
            " run-in to be correlated, so it cannot corroborate the capture "
            "offset.";
        checks.push_back(std::move(check));
        continue;
      }
    }

    VBICRISearchWindow window;
    window.begin_samples = std::max(
        0.0, predicted_anchor - config.service_search_tolerance_samples);
    window.end_samples =
        predicted_anchor + config.service_search_tolerance_samples;

    std::vector<double> offsets;
    for (const VBILineRecord* record : service_records) {
      const VBICRIDetection detection =
          detect_vbi_cri_position(record->samples, service_template, window,
                                  format.calibration.acceptance_correlation);
      if (!detection.accepted) {
        continue;
      }
      offsets.push_back(service.t_offset_ns * 1e-9 * format.sample_rate_hz -
                        detection.anchor_position_samples);
    }

    check.records_used = offsets.size();
    if (offsets.size() < config.minimum_records) {
      check.message = "No " + service.name + " signal was found on line " +
                      std::to_string(service.broadcast_line) +
                      ", so it cannot corroborate the capture offset.";
      checks.push_back(std::move(check));
      continue;
    }

    check.measured_samples = median_of(std::move(offsets));
    const double disagreement =
        std::abs(check.measured_samples - check.expected_samples);
    if (disagreement <= check.tolerance_samples) {
      check.outcome = VBICrossCheckOutcome::kAgreed;
      check.message = service.name + " puts the capture offset at " +
                      format_number(check.measured_samples, 2) +
                      " samples, agreeing with the fitted " +
                      format_number(check.expected_samples, 2) + " samples.";
    } else {
      check.outcome = VBICrossCheckOutcome::kDisagreed;
      check.message =
          service.name + " on line " + std::to_string(service.broadcast_line) +
          " puts the capture offset at " +
          format_number(check.measured_samples, 2) +
          " samples, but the teletext clock run-in fitted " +
          format_number(check.expected_samples, 2) +
          " samples — a "
          "disagreement of " +
          format_number(check.measured_samples - check.expected_samples, 2) +
          " samples.";
    }
    checks.push_back(std::move(check));
  }

  return checks;
}

// The data-end check (design §5.3.5).
//
// A teletext packet is exactly its standardised number of bits long, so where
// modulation stops validates the bit rate independently of where it started.
// A bit-rate error shows up here as a start and end disagreement even when the
// start alone looks right.
VBITimingCrossCheck check_data_end(const VBISourceFormat& format,
                                   const VBITeletextService& service,
                                   double capture_offset_samples,
                                   const std::vector<VBILineRecord>& records,
                                   const VBICrossCheckConfig& config) {
  VBITimingCrossCheck check;
  check.name = "End of teletext modulation";

  const double samples_per_bit = service.samples_per_bit(format.sample_rate_hz);
  check.tolerance_samples = config.data_end_tolerance_bits * samples_per_bit;

  VBICRITemplate cri_template;
  std::string error;
  if (!(samples_per_bit > 0.0) ||
      !make_vbi_cri_frc_template(service, format.sample_rate_hz,
                                 config.template_config, cri_template, error)) {
    check.message =
        "No clock run-in template could be generated for this source, so the "
        "end of modulation cannot validate the bit rate.";
    return check;
  }

  const double total_bits = static_cast<double>(service.cri_bits) +
                            static_cast<double>(service.frc_bits) +
                            static_cast<double>(service.payload_bytes) * 8.0;
  const double predicted_anchor =
      service.t_offset_ns * 1e-9 * format.sample_rate_hz -
      capture_offset_samples;
  const double predicted_end = predicted_anchor + total_bits * samples_per_bit;
  const double search_end =
      predicted_end + kActivitySearchMarginBits * samples_per_bit;
  check.expected_samples = predicted_end;

  if (search_end >= static_cast<double>(format.valid_samples)) {
    check.message =
        "The stored records end before the packet does, so the end of "
        "modulation cannot be measured.";
    return check;
  }

  VBICRISearchWindow window;
  window.begin_samples =
      std::max(0.0, predicted_anchor - config.service_search_tolerance_samples);
  window.end_samples =
      predicted_anchor + config.service_search_tolerance_samples;

  const size_t activity_window =
      static_cast<size_t>(std::max(2.0, std::round(2.0 * samples_per_bit)));

  std::vector<double> measurements;
  for (const VBILineRecord& record : records) {
    const VBICRIDetection detection =
        detect_vbi_cri_position(record.samples, cri_template, window,
                                format.calibration.acceptance_correlation);
    if (!detection.accepted) {
      continue;
    }

    const size_t begin =
        static_cast<size_t>(std::max(0.0, detection.anchor_position_samples));
    const std::vector<double> activity =
        sliding_activity(record.samples, begin, static_cast<size_t>(search_end),
                         activity_window);
    if (activity.size() < 4u) {
      continue;
    }

    const size_t reference_begin = std::min(
        activity.size(),
        static_cast<size_t>(kActivityReferenceBeginBits * samples_per_bit));
    const size_t reference_end = std::min(
        activity.size(),
        static_cast<size_t>(kActivityReferenceEndBits * samples_per_bit));
    if (reference_end <= reference_begin) {
      continue;
    }
    const std::vector<double> reference_window(
        activity.begin() + static_cast<ptrdiff_t>(reference_begin),
        activity.begin() + static_cast<ptrdiff_t>(reference_end));
    const double reference = median_of(reference_window);
    if (!(reference > 0.0)) {
      continue;
    }

    double crossing = 0.0;
    if (!trailing_threshold_crossing(
            activity, config.data_end_activity_fraction * reference,
            reference_end, crossing)) {
      continue;
    }

    // As for the burst, the crossing of a boxcar-smoothed measure sits half a
    // window ahead of the edge that caused it.
    measurements.push_back(static_cast<double>(begin) + crossing +
                           0.5 * static_cast<double>(activity_window));
  }

  check.records_used = measurements.size();
  if (measurements.size() < config.minimum_records) {
    check.message =
        "Teletext modulation was measurable on only " +
        std::to_string(measurements.size()) +
        " records, too few to validate the bit rate against packet length.";
    return check;
  }

  check.measured_samples = median_of(std::move(measurements));
  const double disagreement =
      std::abs(check.measured_samples - check.expected_samples);
  if (disagreement <= check.tolerance_samples) {
    check.outcome = VBICrossCheckOutcome::kAgreed;
    check.message = "Teletext modulation stops at sample " +
                    format_number(check.measured_samples, 1) +
                    " of the record, agreeing with the " +
                    format_number(check.expected_samples, 1) + " that " +
                    format_number(total_bits, 0) + " bits at " +
                    format_number(service.bit_rate_hz, 0) + " bit/s predict.";
    return check;
  }

  check.outcome = VBICrossCheckOutcome::kDisagreed;
  check.message =
      "Teletext modulation stops at sample " +
      format_number(check.measured_samples, 1) + " of the record, but " +
      format_number(total_bits, 0) + " bits at " +
      format_number(service.bit_rate_hz, 0) + " bit/s put the end at sample " +
      format_number(check.expected_samples, 1) + " — a disagreement of " +
      format_number(check.measured_samples - check.expected_samples, 1) +
      " samples. The configured bit rate or sampling rate is suspect.";
  return check;
}

}  // namespace

bool vbi_colour_burst_window_ns(VBITVSystem tv_system, double& out_begin_ns,
                                double& out_end_ns) {
  switch (tv_system) {
    case VBITVSystem::kPAL:
      out_begin_ns = kPALBurstBeginNs;
      out_end_ns = kPALBurstEndNs;
      return true;
    case VBITVSystem::kNTSC:
    case VBITVSystem::kPALM:
      out_begin_ns = kNTSCBurstBeginNs;
      out_end_ns = kNTSCBurstEndNs;
      return true;
  }
  out_begin_ns = 0.0;
  out_end_ns = 0.0;
  return false;
}

std::vector<VBITimingCrossCheck> run_vbi_timing_cross_checks(
    const VBISourceFormat& format, const VBITeletextService& service,
    double capture_offset_samples, const std::vector<VBILineRecord>& records,
    const VBICrossCheckConfig& config) {
  std::vector<VBITimingCrossCheck> checks;

  checks.push_back(
      check_burst_remnant(format, capture_offset_samples, records, config));

  std::vector<VBITimingCrossCheck> service_checks =
      check_reference_services(format, capture_offset_samples, records, config);
  checks.insert(checks.end(), std::make_move_iterator(service_checks.begin()),
                std::make_move_iterator(service_checks.end()));

  checks.push_back(
      check_data_end(format, service, capture_offset_samples, records, config));

  return checks;
}

}  // namespace orc
