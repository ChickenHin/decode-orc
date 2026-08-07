/*
 * File:        analysis_sink_results.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Interfaces for accessing analysis sink stage results across
 *              shared library boundaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_SINK_RESULTS_H
#define ORC_CORE_ANALYSIS_SINK_RESULTS_H

#include <orc/stage/common_types.h>
#include <orc/support/teletext_page_decoder.h>

#include <cstdint>
#include <vector>

namespace orc {

// On macOS, dynamic_cast to a concrete plugin class fails when that class is
// defined in a dylib that is also included in the host binary (duplicate
// type_info pointers across DSOs). Casting to these interfaces — defined in
// orc-core, which is always a single shared symbol source — works reliably on
// all platforms.

class IDropoutAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameDropoutStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~IDropoutAnalysisResults() = default;
};

class ISNRAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameSNRStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~ISNRAnalysisResults() = default;
};

class IBurstLevelAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const std::vector<FrameBurstLevelStats>& frame_stats() const = 0;
  virtual int32_t total_frames() const = 0;
  virtual ~IBurstLevelAnalysisResults() = default;
};

/**
 * @brief One teletext page the analysed range carried
 *
 * The page is catalogued rather than kept per transmission: a carousel brings
 * the same page round hundreds of times in a recording, and what a reader wants
 * is one best assembly of it plus how often and where it was seen.
 */
struct TeletextCataloguedPage {
  int magazine = 8;     ///< Displayed magazine number 1-8
  int page_number = 0;  ///< Two-digit hexadecimal page number 0x00-0xFF

  /// Frames carrying the first and the most recent header packet of the page
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances counted over the analysed range — how often the carousel
  /// brought the page round, and so a rough measure of how reliably it can be
  /// recovered. A header re-sent part-way through the page's own transmission
  /// is the same appearance, not another one.
  uint64_t times_seen = 0;

  /// The page has been transmitted with C6 (subtitle, ETSI EN 300 706 §9.3.1.3
  /// Table 2) set at least once. Sticky: a service may drop C6 between
  /// captions, and the page is still the subtitle page in between.
  bool subtitle = false;

  /// Best assembly of the page, built from every row copy recovered over the
  /// analysed range rather than from one transmission
  TeletextPageSnapshot page;
};

/**
 * @brief How the recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-page detail lives in the stage's
 * own report.
 */
struct TeletextRecoverySummary {
  uint64_t frames_analysed = 0;
  uint64_t fields_with_data = 0;
  uint64_t packets_recovered = 0;
  /// Row packets whose bytes were changed by combining repeated copies
  uint64_t packets_corrected = 0;
  /// Display bytes whose odd parity was restored by the detector's repair
  uint64_t bytes_repaired = 0;
  /// Display characters written, and how many of those are known damaged
  /// because they fail the odd parity of ETSI EN 300 706 §8.1. A floor rather
  /// than an exact count — a byte damaged in two bits passes parity.
  uint64_t characters_written = 0;
  uint64_t characters_damaged = 0;
  /// Packet slots that came back empty during a page transmission: a service
  /// part-way through sending a page fills every line it is using in every
  /// field, so an empty slot is a packet the recording lost. An estimate, and
  /// silent about pages that never started arriving at all.
  uint64_t lost_packets_estimate = 0;
  /// True when the page cap was reached and the least recently seen pages were
  /// dropped, so the catalogue is not the whole set the range carried.
  bool pages_truncated = false;
};

/// Everything the teletext sink caches from one trigger run.
struct TeletextAnalysisDataset {
  /// Ascending by {magazine, page number}
  std::vector<TeletextCataloguedPage> pages;
  TeletextRecoverySummary summary;
};

class ITeletextAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const TeletextAnalysisDataset& dataset() const = 0;
  virtual ~ITeletextAnalysisResults() = default;
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_SINK_RESULTS_H
