/*
 * File:        nabts_record_catalogue.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Bounded catalogue of the teletext records an analysed range
 *              carried, merged from assembled messages
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_RECORD_CATALOGUE_H
#define ORC_NABTS_RECORD_CATALOGUE_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "nabts_record.h"
#include "naplps_interpreter.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief One copy of a record's data, with the holes it arrived with
 *
 * The two run index for index: |present| says, for each byte of |data|, whether
 * that byte arrived or merely stands in for one a lost packet carried, and is
 * empty when nothing was lost. Holding the holes open rather than closing them
 * up is what keeps a damaged copy comparable with the others (see
 * NabtsDataGroup::present).
 */
struct NabtsRecordCopy {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
  /// How sure the detector was of each byte, 0-255 (see
  /// NabtsDataGroup::confidence). Empty where nothing measured this copy, which
  /// is read as full confidence — a detector that cannot express doubt has not
  /// expressed any.
  std::vector<uint8_t> confidence;
};

/**
 * @brief Combine repeated copies of one record's data into one best estimate
 *
 * A cyclic service (§7.1.2) brings every record round for the length of the
 * recording, so a capture holds many copies of each. Copies differ only where
 * they were damaged, and a vote across them recovers bytes that no single copy
 * has right — which matters for NAPLPS, where the data is a stateful opcode
 * stream and one wrong byte changes how the several after it are read.
 *
 * The vote is held per byte position. CEA-516 §3.3 gives every data byte of a
 * type-zero group odd parity in b8, and §4.3 makes a type-zero group the only
 * one carrying a teletext record — so here, unlike at the packet layer where
 * the group type is not yet known, every byte is known to be parity-coded and a
 * byte failing the check is known to be corrupt. Parity-clean candidates
 * therefore win outright wherever a position has any; a position every copy
 * damaged falls back to a vote among all of them.
 *
 * Within a class, each copy contributes how sure the detector was of its byte
 * rather than one vote apiece, so a value read cleanly outweighs the same
 * number of copies of one the detector nearly decided the other way. A copy
 * nothing measured contributes full weight, which is the reading the row
 * squasher gives an unmeasured copy and the only honest one for a detector with
 * no way of saying it is unsure. Ties go to the most recent copy, which is what
 * keeping a single copy would have shown.
 *
 * A copy abstains at the positions its own lost packets took from it, so a
 * recording that never received any one copy whole can still have every
 * position of the record decided by whichever copies did receive it.
 *
 * |copies| must be alignable position for position, oldest first — see
 * NabtsMessage::aligned, which is what admits a copy to the vote. The result is
 * as long as the length most copies agree on, and the longest of them where
 * there is no agreement at all; a copy shorter than that votes only over the
 * bytes it has.
 */
std::vector<uint8_t> nabts_vote_record_data(
    const std::vector<NabtsRecordCopy>& copies);

/**
 * @brief Accumulates assembled messages into a bounded record catalogue
 *
 * A trigger run reads the whole frame range in one pass, and a cyclic service
 * (CEA-516 §7.1.2) sends every record of its magazine round and round for the
 * length of the recording. Catalogueing them — one entry per {channel, record
 * address, version}, the identity §5.2.1 gives a record — bounds what the run
 * holds by the size of the service rather than by the length of the recording.
 *
 * The version is part of the identity because §5.2.1 says it is: a service that
 * revises a page increments Y16 (§5.2.7.2), and the old and new versions are
 * different records that happen to share an address. Keying on the address
 * alone would show whichever the recording happened to end on.
 *
 * Which copy is kept is decided by how good it is rather than by how recent:
 * a complete, undamaged copy is never replaced by a damaged one, so a service
 * recorded off air keeps the copy that arrived cleanly rather than the last one
 * before the tape ran out. Among copies of equal quality the longer wins, and
 * among equals the first — replacing like with like would only churn.
 *
 * Where no copy ever arrives whole — which is the ordinary case for a recording
 * off tape — the damaged copies are kept and combined instead of one of them
 * being chosen, so the record data is a vote across the carousel rather than
 * whichever copy happened to be longest. See nabts_vote_record_data() for the
 * vote, and NabtsMessage::aligned for which copies are eligible to take part.
 * The copies are discarded the moment an undamaged one arrives: there is then
 * nothing left for a vote to improve on.
 *
 * Messages must be merged in ascending temporal order, which is the order the
 * record assembler emits them in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class NabtsRecordCatalogue {
 public:
  /**
   * Upper bound on catalogued records.
   *
   * §7.1.1 organises a service into magazines of pages, and §3.2.3 allows 4096
   * channels of them; a real broadcast service ran to a few hundred pages. This
   * sits above that with room for several channels to share the recording,
   * which is what a capture of a whole VBI actually contains. When full, the
   * least recently seen record is dropped and the dataset flagged truncated.
   */
  static constexpr std::size_t kMaxCataloguedRecords = 1024;

  /**
   * Copies of a record's data retained for the vote.
   *
   * Beyond a handful of copies the vote rarely changes, and a recording of any
   * length would otherwise hold every copy of every record it carried. Record
   * data is capped at 1904 bytes by §8.4.2.5, so the two bounds together put
   * the worst case a little over 30 MB — reached only by a capture that fills
   * the catalogue with full-length records none of which ever arrives
   * undamaged. A record that does arrive undamaged holds no copies at all.
   */
  static constexpr std::size_t kMaxCopiesPerRecord = 16;

  explicit NabtsRecordCatalogue(std::size_t max_records = kMaxCataloguedRecords,
                                std::size_t max_copies = kMaxCopiesPerRecord);

  /**
   * @brief Merge one assembled message
   *
   * @param message  Message as joined by NabtsRecordAssembler
   * @param frame_id Frame carrying the last packet that completed it
   *
   * Every copy counts as an appearance, including a partial one: how often a
   * record came round is a property of the service, and a reader comparing
   * times_seen against times_intact is exactly how the recording's condition
   * shows up.
   */
  void merge(const NabtsMessage& message, uint64_t frame_id);

  /// Records catalogued so far — what the cap bounds.
  std::size_t size() const { return records_.size(); }

  /// True once the cap has dropped at least one record.
  bool truncated() const { return truncated_; }

  /**
   * @brief Catalogue contents, ascending by {channel, address, version}
   *
   * Where a record's copies were combined rather than chosen among, the vote is
   * held here, and the presentation code of §6.1 is run into a display list
   * here too — once per record rather than once per copy, since only the
   * combined result is ever drawn.
   */
  std::vector<NabtsCataloguedRecord> records() const;

 private:
  struct Entry {
    NabtsCataloguedRecord record;
    /// Whether the kept copy was complete and undamaged, which is what a later
    /// copy has to beat to replace it.
    bool kept_copy_intact = false;
    /// Monotonic counter of the last update, for the cap.
    uint64_t last_touched = 0;
    /// Damaged copies of the record data eligible to vote, oldest first. Empty
    /// once an undamaged copy has arrived, and for a record that only ever
    /// arrived misaligned.
    std::vector<NabtsRecordCopy> copies;
  };

  /// Identity of a record (§5.2.1). Ordered so iteration is channel order, then
  /// address, then version — which is how a reader would list a service.
  using Key = std::tuple<uint16_t, uint64_t, uint8_t>;

  /// Whether |message| is a better copy than the one |entry| is holding.
  static bool copy_is_better(const Entry& entry, const NabtsMessage& message);

  /// Overwrite |entry|'s kept copy from |message|.
  static void take_copy(Entry& entry, const NabtsMessage& message);

  /// Retain |message|'s data for the vote, if it can take part in one.
  void add_copy(Entry& entry, const NabtsMessage& message) const;

  void enforce_bounds();

  std::size_t max_records_;
  std::size_t max_copies_;
  /// Reused across records rather than built per record: an interpreter is a
  /// few kilobytes of state and run() resets all of it, so one instance decodes
  /// the whole service. Mutable because records() is where the presentation
  /// code is run, and reading the catalogue does not change it.
  mutable NaplpsInterpreter interpreter_;
  std::map<Key, Entry> records_;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_NABTS_RECORD_CATALOGUE_H
