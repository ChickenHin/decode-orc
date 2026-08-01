/*
 * File:        teletext_row_squasher.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Combine repeated copies of a teletext page row into one
 *              best-estimate row ("squashing")
 *
 * The squashing technique implemented here — grouping every copy of a
 * sub-page recovered from a recording and combining them per byte position —
 * is taken from vhs-teletext by Alistair Buxton (ali1234),
 * https://github.com/ali1234/vhs-teletext, with thanks. No code is shared:
 * this is an independent implementation of the idea, extended to weight the
 * vote by the odd-parity check the standard already provides (see the class
 * comment below).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_ROW_SQUASHER_H
#define ORC_TELETEXT_ROW_SQUASHER_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "teletext_slicer.h"

namespace orc {

// ETSI EN 300 706 §9.3.2: a displayable row packet carries 40 bytes.
constexpr size_t kTeletextRowBytes = 40;

using TeletextRowBytes = std::array<uint8_t, kTeletextRowBytes>;

// Identity of the page a row belongs to. Sub-code is part of the key because
// a different sub-code is a different page (ETSI EN 300 706 §9.3.1.2) —
// merging rotating sub-pages would blend unrelated content.
struct TeletextPageKey {
  int magazine = 8;
  int page_number = 0;
  int subcode = 0;

  bool operator<(const TeletextPageKey& other) const {
    if (magazine != other.magazine) return magazine < other.magazine;
    if (page_number != other.page_number)
      return page_number < other.page_number;
    return subcode < other.subcode;
  }
  bool operator==(const TeletextPageKey& other) const {
    return magazine == other.magazine && page_number == other.page_number &&
           subcode == other.subcode;
  }
};

/**
 * @brief Combines repeated transmissions of a page row into one row
 *
 * Teletext is a carousel: any recording longer than one carousel cycle holds
 * several copies of every row, damaged in different places. Comparing the
 * copies recovers a row cleaner than any single copy of it. The technique,
 * and the name, come from vhs-teletext by Alistair Buxton (ali1234) —
 * https://github.com/ali1234/vhs-teletext — where "squashing" compares every
 * instance of a sub-page and takes the most frequent value at each position.
 *
 * The combination here is likewise per byte position. Where that project
 * takes the plain mode (most frequent value), this one first restricts the
 * vote to candidates that pass odd parity (ETSI EN 300 706 §8.1): parity
 * detects every single-bit error, so a byte that fails it is known to be
 * corrupt and should never win over one that passes, however often it was
 * seen. The plain mode is the fallback when no copy of a byte is
 * parity-clean.
 *
 * Copies are keyed by an opaque source id so a consumer that re-reads the
 * same recovered line — as a sliding-window previewer does every time its
 * window is rebuilt — replaces its earlier copy instead of stuffing the
 * ballot.
 *
 * Header packets (X/0) are deliberately not squashed by this class: their
 * display bytes carry a real-time clock (§9.3.1.4) that legitimately differs
 * between transmissions of the same page.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextRowSquasher {
 public:
  struct Options {
    // Copies retained per row. Beyond a handful the vote rarely changes, and
    // each copy costs 40 bytes plus its source id.
    size_t max_copies_per_row = 16;
    // Upper bound on tracked pages; the least recently updated is dropped
    // when the bound is reached.
    size_t max_pages = 1024;
  };

  TeletextRowSquasher() = default;
  explicit TeletextRowSquasher(Options options) : options_(options) {}

  /**
   * @brief Record one copy of a display row
   *
   * @param key    Page the row belongs to
   * @param row    Display row 1-24 (ETSI EN 300 706 §9.3.2)
   * @param bytes  The 40 display bytes in transmission coding
   * @param source Opaque identity of where this copy came from. Adding the
   *               same source twice replaces the earlier copy rather than
   *               counting it again.
   */
  void add_row(const TeletextPageKey& key, int row,
               const TeletextRowBytes& bytes, int64_t source);

  /// Best estimate of @p row, or nullopt when no copy has been recorded
  std::optional<TeletextRowBytes> squashed_row(const TeletextPageKey& key,
                                               int row) const;

  /**
   * @brief Drop every copy held for a page
   *
   * Call this when the transmission says the page's content is being
   * replaced — a header with C4 (erase page) set, ETSI EN 300 706 §9.3.1.3
   * Table 2. Combining copies assumes they are copies of the *same* content;
   * across an erase they are not, and voting would blend the old page into
   * the new one.
   */
  void erase_page(const TeletextPageKey& key);

  /// Copies recorded for @p row (0 when none)
  size_t copy_count(const TeletextPageKey& key, int row) const;

  /// Pages currently tracked
  size_t page_count() const { return pages_.size(); }

  void clear();

 private:
  struct RowCopies {
    std::vector<TeletextRowBytes> copies;
    std::vector<int64_t> sources;
  };
  struct PageRows {
    // Index 0 is unused: display rows are numbered from 1 (§9.3.2).
    std::array<RowCopies, 25> rows;
    // Monotonic counter of the last update, for the max_pages bound.
    uint64_t last_touched = 0;
  };

  void enforce_page_bound();

  Options options_;
  std::map<TeletextPageKey, PageRows> pages_;
  uint64_t touch_counter_ = 0;
};

}  // namespace orc

#endif  // ORC_TELETEXT_ROW_SQUASHER_H
