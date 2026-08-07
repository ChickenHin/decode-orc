/*
 * File:        teletext_page_catalogue.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Bounded catalogue of the teletext pages an analysed range
 *              carried, merged from decoded page snapshots
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_PAGE_CATALOGUE_H
#define ORC_TELETEXT_PAGE_CATALOGUE_H

#include <orc/stage/analysis_sink_results.h>
#include <orc/support/teletext_page_decoder.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace orc {

/**
 * @brief Accumulates decoded page snapshots into a bounded page catalogue
 *
 * A trigger run decodes the whole frame range in one pass, which on a long
 * recording is hundreds of thousands of page transmissions. Catalogueing them
 * — one entry per {magazine, page number, sub-code}, replaced as better
 * assemblies arrive — bounds what the run holds by the number of pages the
 * service carries rather than by the length of the recording.
 *
 * The sub-code is part of the identity because a page number can carry a
 * sequence of sub-pages the service cycles through (ETSI EN 300 706 Annex A.1:
 * sub-pages of a page intended for display are coded Mxx-0001 to Mxx-0079, a
 * page with none is coded Mxx-0000). Keying on the page number alone would
 * leave whichever sub-page the carousel happened to send last, with the rest of
 * the sequence discarded as if it had been a retransmission of the same page.
 *
 * This is the same identity the row squasher keys its copies on — its
 * TeletextPageKey carries the sub-code too — so a catalogue entry holds exactly
 * the copies that were combined into it, and rows of one sub-page are never
 * shown as another's.
 *
 * Snapshots must be merged in ascending temporal order, which is the order the
 * decoder emits them in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextPageCatalogue {
 public:
  /**
   * Upper bound on catalogued sub-pages. A full carousel is a few hundred
   * pages; the cap keeps a multi-service recording from growing the catalogue
   * without limit (each entry holds a 40x25 page grid). When full, the least
   * recently seen sub-page is dropped — with its page, if it was the last one —
   * and the dataset is flagged truncated.
   */
  static constexpr std::size_t kMaxCataloguedSubPages = 512;

  /**
   * Upper bound on the sub-pages held for any one page number, so that a page
   * whose sub-codes churn cannot crowd the rest of the service out of the
   * catalogue. ETSI EN 300 706 Annex A.1 codes the sub-pages of a display page
   * Mxx-0001 to Mxx-0079, which with the no-sub-pages code Mxx-0000 is what
   * this allows.
   */
  static constexpr std::size_t kMaxSubPagesPerPage = 80;

  explicit TeletextPageCatalogue(
      std::size_t max_subpages = kMaxCataloguedSubPages,
      std::size_t max_subpages_per_page = kMaxSubPagesPerPage);

  /**
   * @brief Merge one decoded page snapshot
   *
   * @param snapshot Page as rendered by TeletextPageDecoder
   * @param frame_id Frame carrying the snapshot's header packet
   *
   * A snapshot whose transmission is still in progress updates the sub-page's
   * content but does not count as another appearance; nor does a header
   * re-sent part-way through the page's own transmission, which the decoder
   * reports under the appearance's original header field index.
   */
  void merge(const TeletextPageSnapshot& snapshot, uint64_t frame_id);

  /// Page numbers catalogued so far
  std::size_t size() const { return pages_.size(); }

  /// Sub-pages catalogued so far, over all page numbers — what the cap bounds
  std::size_t subpage_count() const { return subpage_count_; }

  /// True once a cap has dropped at least one sub-page
  bool truncated() const { return truncated_; }

  /// Catalogue contents, ascending by {magazine, page number}, each page's
  /// sub-pages ascending by sub-code
  std::vector<TeletextCataloguedPage> pages() const;

 private:
  struct SubEntry {
    TeletextCataloguedSubPage subpage;
    /// Header field index of the newest appearance already counted, so a
    /// transmission re-emitted as a fragment is not counted twice
    int64_t counted_header_field = -1;
    /// Monotonic counter of the last update, for the sub-page bounds
    uint64_t last_touched = 0;
  };

  struct Entry {
    bool subtitle = false;
    // Keyed by sub-code so iteration is sub-page order: Annex A.1 numbers the
    // sub-pages of a display page sequentially from Mxx-0001, so ascending
    // sub-code is the sequence the service cycles through.
    std::map<int, SubEntry> subpages;
  };

  // Least recently touched sub-page of |entry|; never called on an empty one.
  static std::map<int, SubEntry>::iterator oldest_subpage(Entry& entry);

  void enforce_subpage_bounds(Entry& page_entry);

  std::size_t max_subpages_;
  std::size_t max_subpages_per_page_;
  // Keyed by {displayed magazine, page number} so iteration is page order.
  std::map<std::pair<int, int>, Entry> pages_;
  std::size_t subpage_count_ = 0;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_CATALOGUE_H
