/*
 * File:        teletext_page_assembler.h
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and accumulating page catalogue for
 *              the teletext preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXT_PAGE_ASSEMBLER_H
#define TELETEXT_PAGE_ASSEMBLER_H

#include <orc/support/teletext_row_squasher.h>
#include <orc_teletext.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

/**
 * @brief Trailing-frame-window packet cache with an accumulating page
 *        catalogue
 *
 * The teletext preview dialog follows the frame previewer through a carousel
 * medium: random access is inherently approximate, so the dialog accumulates
 * the recovered packets of a trailing window of frames ending at the current
 * frame and decodes every page that window contains.
 *
 * Decoded pages are merged into a *catalogue* that outlives the window: a
 * page stays listed (with the frame it was last seen at) after the frames
 * carrying it have been evicted, so stepping forward through a recording
 * builds up the set of pages the service transmits instead of showing only
 * the last two seconds. Continuity is what makes that meaningful, so the
 * catalogue is discarded when the previewer jumps far enough that the new
 * window shares no frames with the old one; it is then rebuilt from the
 * frames preceding the position jumped to.
 *
 * Qt-free by design so page assembly is unit-testable without a QApplication.
 * Thread safety: none; confine an instance to the GUI thread.
 */
class TeletextPageAssembler {
 public:
  /**
   * Trailing window length in frames (2 s of 625/50 video). Page
   * transmissions span only a few fields, so the window bounds how far back
   * the previewer looks when it arrives at a new position without requesting
   * observations for a whole carousel cycle (typically tens of seconds).
   * Sequential stepping then extends coverage through the catalogue rather
   * than through a longer window.
   */
  static constexpr uint64_t kTrailingWindowFrames = 50;

  /**
   * Upper bound on catalogued pages. A full carousel is a few hundred pages;
   * the cap keeps a long scrub through a multi-service recording from growing
   * the catalogue without limit (each entry holds a 40x25 page view). When
   * full, the least recently seen page is dropped.
   */
  static constexpr std::size_t kMaxCataloguedPages = 512;

  /// One page the previewer has seen, and where it was last seen
  struct CataloguedPage {
    int magazine = 8;         ///< Displayed magazine number 1-8
    int page_number = 0;      ///< Two-digit hexadecimal page number 0x00-0xFF
    uint64_t seen_frame = 0;  ///< Frame carrying the most recent header packet
    /// Transmissions of this page counted since the last discontinuity. A
    /// carousel repeats its pages, so this is how often the page came round —
    /// a rough measure of how reliably it can be recovered here.
    uint64_t times_seen = 0;
    /// Header field index of the newest transmission already counted. The
    /// cached window is re-decoded from scratch on every frame change, so a
    /// transmission only counts when its header lies past this one.
    int64_t counted_header_field = -1;
    /// Most recent assembly, built from every row copy accumulated since the
    /// last discontinuity rather than from one transmission (see
    /// refreshCatalogue() and squasher_)
    orc::presenters::TeletextPageView page;
  };

  /**
   * @brief Advance the window so it ends at @p frame_index
   *
   * Cached frames that fall outside the new window are evicted. A move that
   * leaves no overlap with the previous window is treated as a discontinuity
   * (skip or seek) and clears the accumulated page catalogue.
   */
  void setCurrentFrame(uint64_t frame_index);

  uint64_t currentFrame() const { return current_frame_; }

  /// First frame of the current window (0 when the window reaches the start)
  uint64_t windowStartFrame() const;

  /**
   * @brief Frames inside the current window with no cached data yet
   *        (ascending order)
   */
  std::vector<uint64_t> framesNeedingData() const;

  /**
   * @brief Store the delivered per-field packet views for a frame
   *
   * Frames outside the current window are ignored (stale deliveries).
   */
  void storeFrame(uint64_t frame_index,
                  orc::presenters::TeletextFieldPacketsView field1,
                  orc::presenters::TeletextFieldPacketsView field2);

  /**
   * @brief Record that a frame could not be observed
   *
   * Cached as an empty frame so the window converges: an unobservable frame
   * stays unobservable for this view node, and re-requesting it on every
   * frame change would issue a whole window of requests per step.
   */
  void markFrameUnavailable(uint64_t frame_index);

  bool hasFrame(uint64_t frame_index) const;

  /// Drop all cached frames and the page catalogue (node/DAG change or close)
  void clear();

  /// Identity of one catalogued page, without its 40x25 content
  struct PageListing {
    int magazine = 8;
    int page_number = 0;
    uint64_t seen_frame = 0;
    uint64_t times_seen = 0;
  };

  /// Pages seen since the last discontinuity, ascending by page address
  std::vector<PageListing> cataloguedPages() const;

  /**
   * @brief Look up one catalogued page
   *
   * @param magazine    Displayed magazine number 1-8
   * @param page_number Two-digit hexadecimal page number 0x00-0xFF
   * @return Catalogue entry, or nullptr when the page has not been seen.
   *         Invalidated by any mutating call.
   */
  const CataloguedPage* findPage(int magazine, int page_number) const;

  /**
   * @brief Revision counter of the catalogue's contents
   *
   * Incremented whenever a page is added, dropped, or seen at a new frame, so
   * views can skip rebuilding their list when nothing changed. Re-decoding
   * the same window does not bump it.
   */
  uint64_t catalogueRevision() const;

 private:
  struct FrameData {
    orc::presenters::TeletextFieldPacketsView field1;
    orc::presenters::TeletextFieldPacketsView field2;
  };

  /// Decode the cached window and merge the pages it yields into the
  /// catalogue (no-op unless the cache changed since the last refresh).
  void refreshCatalogue() const;

  uint64_t current_frame_ = 0;
  // Keyed by frame index; std::map keeps temporal order for decoder feeding.
  std::map<uint64_t, FrameData> frames_;

  // Catalogue state is a cache over frames_, refreshed lazily on read.
  // Keyed by {displayed magazine, page number} so iteration is page order.
  mutable std::map<std::pair<int, int>, CataloguedPage> catalogue_;
  mutable bool catalogue_dirty_ = false;
  mutable uint64_t catalogue_revision_ = 0;

  // Copies of every row seen since the last discontinuity. Unlike frames_
  // this is not bounded by the window, which is what lets a page be assembled
  // from several partial transmissions and lets repeated copies of a row
  // correct each other (orc/support/teletext_row_squasher.h). Copies are
  // keyed by their (field, line) origin so re-decoding the window — which
  // happens on every frame change — replaces them instead of counting them
  // again.
  mutable orc::TeletextRowSquasher squasher_;
};

#endif  // TELETEXT_PAGE_ASSEMBLER_H
