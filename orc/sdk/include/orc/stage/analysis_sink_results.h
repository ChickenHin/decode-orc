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
#include <orc/support/nabts_page.h>
#include <orc/support/teletext_page_decoder.h>

#include <cstdint>
#include <string>
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
 * @brief One sub-page of a catalogued teletext page
 *
 * A page number can carry a sequence of sub-pages that the service cycles
 * through — the multi-page set of ETSI EN 300 706 Annex A.1, what a receiver
 * presents as a rotating page. Each is a page in its own right, told from its
 * siblings by the page sub-code (§9.3.1.2), and each is catalogued separately
 * so a reader can step through the sequence instead of seeing only whichever
 * one the carousel happened to leave behind.
 */
struct TeletextCataloguedSubPage {
  /// 13-bit page sub-code S1-S4 as transmitted (ETSI EN 300 706 §9.3.1.2),
  /// packed S1 in bits 0-3, S2 in 4-6, S3 in 7-10, S4 in 11-12
  int subcode = 0;

  /// Frames carrying the first and the most recent header packet of this
  /// sub-page
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances of this sub-page counted over the analysed range. A header
  /// re-sent part-way through the sub-page's own transmission is the same
  /// appearance, not another one.
  uint64_t times_seen = 0;

  /// Best assembly of the sub-page, built from every row copy recovered over
  /// the analysed range rather than from one transmission
  TeletextPageSnapshot page;
};

/**
 * @brief One teletext page the analysed range carried
 *
 * The page is catalogued rather than kept per transmission: a carousel brings
 * the same page round hundreds of times in a recording, and what a reader wants
 * is one best assembly of it plus how often and where it was seen.
 *
 * Where the page is a multi-page set, that assembly is per sub-page: see
 * @ref subpages, which always holds at least one entry.
 */
struct TeletextCataloguedPage {
  int magazine = 8;     ///< Displayed magazine number 1-8
  int page_number = 0;  ///< Two-digit hexadecimal page number 0x00-0xFF

  /// Frames carrying the first and the most recent header packet of the page,
  /// over all of its sub-pages
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;

  /// Appearances counted over the analysed range — how often the carousel
  /// brought the page round, and so a rough measure of how reliably it can be
  /// recovered. A header re-sent part-way through the page's own transmission
  /// is the same appearance, not another one. Summed over the sub-pages, so on
  /// a multi-page set this counts the transmissions rather than the cycles.
  uint64_t times_seen = 0;

  /// The page has been transmitted with C6 (subtitle, ETSI EN 300 706 §9.3.1.3
  /// Table 2) set at least once. Sticky: a service may drop C6 between
  /// captions, and the page is still the subtitle page in between.
  bool subtitle = false;

  /// The page's sub-pages, ascending by sub-page number, never empty. A page
  /// that is not a multi-page set has exactly one entry — sub-code 0000 in the
  /// coding of Annex A.1 — so a consumer need not special-case either.
  std::vector<TeletextCataloguedSubPage> subpages;
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
  /// True when the sub-page cap was reached and the least recently seen ones
  /// were dropped, so the catalogue is not the whole set the range carried.
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

/**
 * @brief One application function descriptor, ready to show (CEA-516 §7.2.2)
 *
 * A NABTS application record is a sequence of function descriptors rather than
 * anything displayable, and what a reader wants of one is a listing. Both
 * fields are text because both are read rather than acted on: the host has no
 * business executing a broadcaster's control functions, and rendering them is
 * all this is for.
 */
struct NabtsRecordFunction {
  /// Function code in the "2/0" column/row notation the standard uses.
  std::string code;
  /// Whether the code is control data (column 2) rather than information
  /// (column 3).
  bool control = false;
  /// Arguments as printable text, with anything unprintable shown as its
  /// hexadecimal value. Empty for a descriptor with no arguments, which
  /// §7.2.3.1 makes a request to restore that function's initial state.
  std::string arguments;
};

/**
 * @brief One teletext record the analysed range carried
 *
 * Catalogued rather than kept per transmission, for the reason the teletext
 * page catalogue is: a cyclic service (CEA-516 §7.1.2) brings the same record
 * round throughout a recording, and what a reader wants is one best copy of it
 * plus how often and where it was seen.
 *
 * A record here is a *message* in the standard's terms (§5.2.6) — one unlinked
 * record, or a linked series joined — because that is the unit a receiver
 * presents.
 */
struct NabtsCataloguedRecord {
  /// Data channel, i.e. the packet address of §3.2.3.
  uint16_t channel = 0;
  /// Record address in the nine-digit long form §5.2.5 makes equivalent to the
  /// short one, so records that name the same address either way compare equal.
  uint64_t address = 0;
  /// The address as transmitted: three hexadecimal digits, or nine when the
  /// record carried an address extension.
  std::string address_text;
  /// Channel and address together, as a reader would cite them.
  std::string channel_text;

  /// RT (§5.2.2): 0 cyclic presentation, 1 non-cyclic presentation,
  /// 2 application, 3 priority presentation; 4-15 reserved.
  uint8_t record_type = 0;
  /// Version number from classification flag byte Y16 (§5.2.7.2), 0 when the
  /// record carried no classification sequence.
  uint8_t version = 0;

  // Classification flags worth listing (§5.2.7.2). All false when the record
  // carried no classification sequence, which §5.2.7.2 makes the correct
  // reading of an absent flag byte.
  bool caption = false;
  bool cyclic_marker = false;
  bool priority = false;
  bool alarm = false;
  bool update = false;
  bool support_record = false;
  bool index = false;
  bool more = false;

  /// What §7.1.5 reserves this channel and address for, or empty.
  std::string reserved_purpose;

  /// Frames carrying the first and the most recent copy of this record.
  uint64_t first_seen_frame = 0;
  uint64_t last_seen_frame = 0;
  /// Copies counted over the analysed range — how often the service brought the
  /// record round, and so a rough measure of how reliably it can be recovered.
  uint64_t times_seen = 0;
  /// Copies that arrived whole and undamaged, of @ref times_seen.
  uint64_t times_intact = 0;

  /// Records in the linked series (§5.2.6); 1 for an unlinked record.
  uint32_t records_in_message = 0;
  /// The best copy has every link of its series and every packet of every
  /// group. A false here is why a presentation record may render short.
  bool complete = false;

  /// Best copy of the record data (§5.3): NAPLPS presentation code for record
  /// types 0, 1 and 3, application data for type 2. "Best" is the first
  /// complete and undamaged copy seen, or the longest copy if none was.
  std::vector<uint8_t> data;

  /// Function descriptors, for an application record. Empty otherwise.
  std::vector<NabtsRecordFunction> functions;

  /// The record's presentation code (§6.1 NAPLPS) run into a display list, for
  /// a presentation record — types 0, 1 and 3. Empty for an application record,
  /// whose data is function descriptors rather than drawing.
  NabtsPageSnapshot page;
};

/**
 * @brief How the NABTS recovery went over the analysed range
 *
 * Aggregate counts only; the per-line and per-group detail lives in the stage's
 * own report.
 */
struct NabtsRecoverySummary {
  uint64_t frames_analysed = 0;
  uint64_t fields_with_data = 0;
  uint64_t packets_recovered = 0;
  /// Packets whose Hamming 8/4 prefix did not decode (§3.2.2), and so could not
  /// even be filed under a channel.
  uint64_t packets_prefix_rejected = 0;
  /// Packet slots that came back empty on a line the recording has been seen to
  /// carry data on — an estimate of what was lost.
  uint64_t lost_packets_estimate = 0;
  /// Data blocks the suffix product code repaired, and data blocks whose suffix
  /// check failed and could not be repaired (§3.4).
  uint64_t blocks_corrected = 0;
  uint64_t blocks_damaged = 0;
  /// Data groups that arrived whole, and groups that ended without every packet
  /// S1,S2 promised (§4.2.5).
  uint64_t groups_completed = 0;
  uint64_t groups_incomplete = 0;
  /// Messages assembled whole, and messages missing at least one link.
  uint64_t messages_complete = 0;
  uint64_t messages_partial = 0;
  /// True when the record cap was reached and the least recently seen ones were
  /// dropped, so the catalogue is not everything the range carried.
  bool records_truncated = false;
};

/// Everything the NABTS sink caches from one trigger run.
struct NabtsAnalysisDataset {
  /// Ascending by {channel, address, version}
  std::vector<NabtsCataloguedRecord> records;
  NabtsRecoverySummary summary;
};

/**
 * @brief One caption, with the frames it was on screen for
 *
 * CEA-516 §7.3.10 carries captioning as non-cyclic presentation records marked
 * with the Caption Flag of §5.2.7.3, each new caption a new version of the same
 * record address (§7.3.10.1: "Each time the caption content is changed, the
 * Version Number shall be changed").
 */
struct NabtsCaptionCue {
  /// Frames the cue covers (0-based, as the catalogue counts them). The extent
  /// runs to the next caption: §7.3.10.1 has a receiver replace the caption on
  /// screen rather than being told when to take it down.
  uint64_t start_frame = 0;
  uint64_t end_frame = 0;
  uint16_t channel = 0;
  std::string address_text;
  uint8_t version = 0;
  /// The record's text, as nabts_page_text() reads it.
  std::string text;
};

/**
 * @brief The caption service a catalogue carries (CEA-516 §7.3.10)
 *
 * Ascending by the frame each caption was first seen at. A caption record that
 * drew nothing is an erase — §7.3.10.1: "Captions may be erased by the use of
 * PLPS code that erases either the entire display, or the area covered by the
 * caption" — so it ends the caption before it and yields no cue of its own.
 *
 * Shared between the sink stage's SubRip export and the host's caption track,
 * so the two cannot disagree about what the service said.
 */
std::vector<NabtsCaptionCue> nabts_caption_cues(
    const std::vector<NabtsCataloguedRecord>& records);

class INabtsAnalysisResults {
 public:
  virtual bool has_results() const = 0;
  virtual const NabtsAnalysisDataset& dataset() const = 0;
  virtual ~INabtsAnalysisResults() = default;
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_SINK_RESULTS_H
