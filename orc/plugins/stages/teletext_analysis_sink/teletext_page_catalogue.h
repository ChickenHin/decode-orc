/*
 * File:        teletext_page_catalogue.h
 * Module:      orc-stage-plugin-teletext_analysis_sink
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
 * — one entry per {magazine, page number}, replaced as better assemblies
 * arrive — bounds what the run holds by the number of pages the service
 * carries rather than by the length of the recording.
 *
 * Snapshots must be merged in ascending temporal order, which is the order the
 * decoder emits them in.
 *
 * Thread safety: none; confine an instance to one thread.
 */
class TeletextPageCatalogue {
 public:
  /**
   * Upper bound on catalogued pages. A full carousel is a few hundred pages;
   * the cap keeps a multi-service recording from growing the catalogue without
   * limit (each entry holds a 40x25 page grid). When full, the least recently
   * seen page is dropped and the dataset is flagged truncated.
   */
  static constexpr std::size_t kMaxCataloguedPages = 512;

  explicit TeletextPageCatalogue(std::size_t max_pages = kMaxCataloguedPages);

  /**
   * @brief Merge one decoded page snapshot
   *
   * @param snapshot Page as rendered by TeletextPageDecoder
   * @param frame_id Frame carrying the snapshot's header packet
   *
   * A snapshot whose transmission is still in progress updates the entry's
   * content but does not count as another appearance; nor does a header
   * re-sent part-way through the page's own transmission, which the decoder
   * reports under the appearance's original header field index.
   */
  void merge(const TeletextPageSnapshot& snapshot, uint64_t frame_id);

  /// Pages catalogued so far
  std::size_t size() const { return pages_.size(); }

  /// True once the cap has dropped at least one page
  bool truncated() const { return truncated_; }

  /// Catalogue contents, ascending by {magazine, page number}
  std::vector<TeletextCataloguedPage> pages() const;

 private:
  struct Entry {
    TeletextCataloguedPage page;
    /// Header field index of the newest appearance already counted, so a
    /// transmission re-emitted as a fragment is not counted twice
    int64_t counted_header_field = -1;
    /// Monotonic counter of the last update, for the page bound
    uint64_t last_touched = 0;
  };

  void enforce_page_bound();

  std::size_t max_pages_;
  // Keyed by {displayed magazine, page number} so iteration is page order.
  std::map<std::pair<int, int>, Entry> pages_;
  uint64_t touch_counter_ = 0;
  bool truncated_ = false;
};

}  // namespace orc

#endif  // ORC_TELETEXT_PAGE_CATALOGUE_H
