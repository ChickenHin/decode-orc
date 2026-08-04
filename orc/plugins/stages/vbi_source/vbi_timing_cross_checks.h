/*
 * File:        vbi_timing_cross_checks.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Corroborates a fitted capture offset against other references
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_TIMING_CROSS_CHECKS_H
#define ORC_VBI_TIMING_CROSS_CHECKS_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_cri_template.h"
#include "vbi_line_reader.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// What a cross-check concluded.
//
// None of these is ever an error.  A cross-check that disagrees means the
// configuration deserves a second look before committing to a multi-hour
// decode, not that the run should stop: the teletext lock is the primary
// measurement and these are corroborations of it (design §5.3.5).
enum class VBICrossCheckOutcome {
  // The check does not apply to this source format, or the capture carried
  // nothing to run it against.  Distinct from agreement, because a check that
  // did not run has corroborated nothing.
  kNotApplicable,

  kAgreed,
  kDisagreed,
};

// One corroboration of the fitted timing.
struct VBITimingCrossCheck {
  std::string name;

  VBICrossCheckOutcome outcome = VBICrossCheckOutcome::kNotApplicable;

  // The two figures the check compares, in source samples, and how far apart
  // they are allowed to be.  Both are reported whatever the outcome, so a
  // marginal agreement is as visible as a disagreement.
  double measured_samples = 0.0;
  double expected_samples = 0.0;
  double tolerance_samples = 0.0;

  // Records the measurement was taken over.
  uint64_t records_used = 0;

  // Human-readable result, naming both estimates when they disagree.
  std::string message;

  double disagreement_samples() const {
    return measured_samples - expected_samples;
  }
};

// Tolerances of the cross-checks.
struct VBICrossCheckConfig {
  // How far the measured burst trailing edge may sit from the one the fitted
  // offset predicts.  Generous, because a burst envelope's edge is a shaped
  // transition rather than a hard one, and because this check exists to catch
  // a gross error rather than to refine anything.
  double burst_tolerance_samples = 8.0;

  // How far another data service's own offset estimate may sit from the
  // fitted one.
  double service_tolerance_samples = 4.0;

  // How far the end of modulation may sit from where the configured bit rate
  // puts it, in bit periods.  Expressed in bits because that is what the check
  // actually validates.
  double data_end_tolerance_bits = 8.0;

  // Half-width, in source samples, of the search for another service's run-in
  // around the position the fitted offset predicts.
  double service_search_tolerance_samples = 8.0;

  // Fraction of a line's data-region activity below which modulation is
  // considered to have stopped.
  double data_end_activity_fraction = 0.4;

  // Fraction of the burst's own envelope amplitude taken as its trailing edge.
  double burst_edge_fraction = 0.5;

  // Records a check needs before its measurement is reported at all.
  uint64_t minimum_records = 4;

  VBICRITemplateConfig template_config{};
};

// Colour burst window of a television system, in nanoseconds from 0H
// (design §5.6).  Returns false for systems the stage has no timing for.
bool vbi_colour_burst_window_ns(VBITVSystem tv_system, double& out_begin_ns,
                                double& out_end_ns);

// Run every §5.3.5 corroboration that applies to a source format over a set of
// stored records.
//
// The records are ordinary read records — the same ones calibration ran over —
// so the checks cost a second pass over data already in memory and no extra
// reading.
std::vector<VBITimingCrossCheck> run_vbi_timing_cross_checks(
    const VBISourceFormat& format, const VBITeletextService& service,
    double capture_offset_samples, const std::vector<VBILineRecord>& records,
    const VBICrossCheckConfig& config);

}  // namespace orc

#endif  // ORC_VBI_TIMING_CROSS_CHECKS_H
