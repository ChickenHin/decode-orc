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

#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_row_squasher.h>
#include <orc_teletext.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
 * the last few seconds. Continuity is what makes that meaningful, so the
 * catalogue is discarded when the previewer *seeks* — lands somewhere no part
 * of the run describes — and is then rebuilt from the frames preceding the
 * position jumped to. Moving back within a window's travel of the run is not
 * a seek: the run is laid out again from further back, but everything it has
 * learned is kept (see setCurrentFrame()).
 *
 * Frames are fed to a *persistent* decoder in ascending order and never
 * decoded twice. That matters at this window length: re-decoding the whole
 * window on every frame change (as an earlier, much shorter window could
 * afford to) would cost the same work thousands of times over, and the cost
 * of a frame step would grow with the window rather than staying flat. The
 * consequence is that a frame arriving after the decoder has already passed
 * it cannot contribute — see storeFrame().
 *
 * Qt-free by design so page assembly is unit-testable without a QApplication.
 * Thread safety: none; confine an instance to the GUI thread.
 */
class TeletextPageAssembler {
 public:
  /**
   * Trailing window length in frames (30 s of 625/50 video).
   *
   * How far back the previewer reaches when it arrives at a new position.
   * This has to be long compared with a *carousel cycle*, not with a page:
   * one page occupies only a handful of frames, but the service does not
   * bring it round again until every other page has been sent, which is
   * several hundred frames on a typical magazine. A window shorter than a
   * cycle holds only whichever page transmissions happen to fall inside it,
   * so the page list stays nearly empty however long the user waits.
   *
   * It is not, however, the limit on how much of a recording the assembler
   * remembers: everything decoded since the last discontinuity stays in the
   * catalogue and the row squasher, so stepping or playing forward extends
   * coverage without bound and at no extra cost. Only *reaching backwards*
   * is bounded, and only because each frame of it costs one observation
   * read before anything can be shown.
   */
  static constexpr uint64_t kTrailingWindowFrames = 750;

  /**
   * Frames framesNeedingData() will name in one call.
   *
   * Arriving at a new position leaves a whole window unread, and every frame
   * costs one observation read. Handing the caller all of them at once would
   * queue thousands of reads ahead of the previewer's own rendering; the cap
   * paces them instead, and the caller comes back for more as deliveries
   * land. Frames are always named in ascending order, so the decoder's
   * contiguous frontier keeps advancing.
   */
  static constexpr std::size_t kMaxFramesPerRequest = 120;

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
    /// Appearances of this page counted since the last discontinuity — how
    /// often the carousel has brought it round, and so a rough measure of how
    /// reliably it can be recovered here. A header re-sent part-way through
    /// the page's own transmission is the same appearance, not another one.
    uint64_t times_seen = 0;
    /// Header field index of the newest appearance already counted. Guards
    /// against counting a transmission twice when it is re-emitted.
    int64_t counted_header_field = -1;
    /// The page has been transmitted with C6 (subtitle) set at least once
    /// since the last discontinuity — this is the service telling the receiver
    /// which page carries the subtitles, and 888 is only a convention (the
    /// LaserDisc samples this was built against use 190). Sticky, because a
    /// service may drop C6 between captions (the decoder treats that as the
    /// clear event); the page is still the subtitle page in between, and a
    /// marker that blinked out whenever nothing was on screen would be worse
    /// than useless for finding it.
    bool subtitle = false;
    /// Most recent assembly, built from every row copy accumulated since the
    /// last discontinuity rather than from one transmission (see
    /// refreshCatalogue() and squasher_)
    orc::presenters::TeletextPageView page;
  };

  TeletextPageAssembler();
  ~TeletextPageAssembler();

  /**
   * @brief Advance the window so it ends at @p frame_index
   *
   * A move backwards past the start of the decode run lays the run out again
   * from a window before the new position — the decoder only goes forwards —
   * but keeps the page catalogue and the accumulated row copies: the frames in
   * between are the same recording, so what has been decoded is still true of
   * it, and the page on screen stays up while the re-read happens.
   *
   * Only a *seek* discards: a move more than a whole window forward of the
   * previewer, or so far back that a fresh window here would not reach where
   * the run began. There nothing decoded so far describes anywhere near the
   * new position, and a service can change entirely across a seek.
   */
  void setCurrentFrame(uint64_t frame_index);

  uint64_t currentFrame() const { return current_frame_; }

  /// First frame of the current window (0 when the window reaches the start)
  uint64_t windowStartFrame() const;

  /**
   * @brief Highest frame whose packets are still worth holding or delivering
   *
   * Frames ahead of the previewer are kept after a backward step, as far as a
   * continuous move could come back to them; each one held is an observation
   * read saved when it does. Beyond this only a seek could reach them, and a
   * seek discards the cache anyway. Callers tracking in-flight reads should
   * use this rather than currentFrame() as the upper bound on what is still
   * worth waiting for.
   */
  uint64_t retainedFrameLimit() const {
    return current_frame_ + kTrailingWindowFrames;
  }

  /**
   * @brief Frames inside the current window with no cached data yet
   *
   * Ascending, and at most kMaxFramesPerRequest of them; call again once the
   * named frames have been delivered to get the next batch.
   */
  std::vector<uint64_t> framesNeedingData() const;

  /// Total frames in the window still without data (not capped by the batch
  /// size), for reporting progress
  std::size_t framesNeedingDataCount() const;

  /**
   * @brief Store the delivered per-field packet views for a frame
   *
   * Frames outside the run's reach are ignored (stale deliveries; see
   * retainedFrameLimit()), as are frames the decoder has already passed: it is
   * fed strictly forwards, so a late delivery for an earlier frame can no
   * longer contribute. Requests are issued in ascending order and the decoder
   * only advances over frames that have arrived, so this is the rare case of a
   * response overtaken by a seek.
   */
  void storeFrame(uint64_t frame_index,
                  orc::presenters::TeletextFieldPacketsView field1,
                  orc::presenters::TeletextFieldPacketsView field2);

  /**
   * @brief Record that a frame could not be observed
   *
   * Cached as an empty frame so the window converges: an unobservable frame
   * stays unobservable for this view node, and re-requesting it on every
   * frame change would issue a whole window of requests per step. It also
   * unblocks the decoder, which advances only over frames it has.
   */
  void markFrameUnavailable(uint64_t frame_index);

  /// True once @p frame_index has been fetched, whether or not its packets
  /// are still held (they are released as soon as the decoder consumes them)
  bool hasFrame(uint64_t frame_index) const;

  /// Drop all cached frames and the page catalogue (node/DAG change or close)
  void clear();

  /// Identity of one catalogued page, without its 40x25 content
  struct PageListing {
    int magazine = 8;
    int page_number = 0;
    uint64_t seen_frame = 0;
    uint64_t times_seen = 0;
    /// False while the page is still arriving (see
    /// orc::presenters::TeletextPageView::transmission_complete)
    bool transmission_complete = true;
    /// Seen with the C6 subtitle control bit set (see
    /// CataloguedPage::subtitle)
    bool subtitle = false;
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

  /// Feed newly available frames to the decoder and merge what it yields into
  /// the catalogue (no-op unless the cache advanced since the last refresh).
  void refreshCatalogue() const;

  /// Merge one decoded page into the catalogue.
  void mergeSnapshot(const orc::TeletextPageSnapshot& snapshot) const;

  /**
   * @brief Packet slots that came back empty during one transmission
   *
   * A missing row is not evidence of anything: services habitually omit the
   * blank rows that space a page out. An empty *slot* is evidence, because a
   * service part-way through sending a page fills every line it is using in
   * every field. The lines in use are taken from the transmission itself, so
   * this needs no knowledge of the recording's insertion pattern.
   *
   * @param first_field Field index of the transmission's header packet
   * @param last_field  Field index of its last packet
   */
  int lostPacketsBetween(int64_t first_field, int64_t last_field) const;

  /// Lay the decode run out again from @p anchor_frame, discarding only the
  /// decoder's own assembly state. The catalogue and the row copies survive;
  /// discardAccumulated() is what throws those away.
  void restartDecodeAt(uint64_t anchor_frame);

  /// Throw away everything learned by this decode run — cached frames, row
  /// copies, per-field packet counts and the catalogue — for a seek to a
  /// position none of it describes.
  void discardAccumulated();

  /// Frame a decode run arriving at @p frame_index should start from — one
  /// whole window back, clamped to the start of the recording.
  static uint64_t anchorFor(uint64_t frame_index);

  uint64_t current_frame_ = 0;
  // Keyed by frame index; std::map keeps temporal order for decoder feeding.
  // A frame's packets are released once the decoder has consumed them, so an
  // entry is usually just the record that the frame was fetched — which is
  // why releasing them happens during the (lazy, const) catalogue refresh.
  mutable std::map<uint64_t, FrameData> frames_;

  // First frame this decode run covers, and the first frame not yet fed to
  // the decoder. Everything in [decode_anchor_, decode_frontier_) has been
  // decoded exactly once, in ascending order.
  uint64_t decode_anchor_ = 0;
  mutable uint64_t decode_frontier_ = 0;

  // Catalogue state is a cache over what has been decoded, refreshed lazily
  // on read. Keyed by {displayed magazine, page number} so iteration is page
  // order.
  mutable std::map<std::pair<int, int>, CataloguedPage> catalogue_;
  mutable bool catalogue_dirty_ = false;
  mutable uint64_t catalogue_revision_ = 0;

  // Fed strictly forwards and never rewound; recreated on a discontinuity.
  // Held by pointer so a restart is a plain reset rather than a bespoke
  // clear() on the decoder.
  mutable std::unique_ptr<orc::TeletextPageDecoder> decoder_;

  // Copies of every row seen since the last discontinuity. Unlike frames_
  // this is not bounded by the window, which is what lets a page be assembled
  // from several partial transmissions and lets repeated copies of a row
  // correct each other (orc/support/teletext_row_squasher.h). Copies are
  // keyed by their (field, line) origin, so a frame that somehow reaches the
  // decoder twice re-seats its copies instead of stuffing the ballot.
  mutable orc::TeletextRowSquasher squasher_;

  // Packets recovered per field index, kept after the field's packets are
  // released so a completed transmission can be asked what its slots yielded
  // (lostPacketsBetween()). Bounded: the oldest entries are dropped once the
  // history is far longer than any single transmission.
  mutable std::map<int64_t, int> field_packet_counts_;
  static constexpr std::size_t kMaxFieldCountHistory = 8192;
};

#endif  // TELETEXT_PAGE_ASSEMBLER_H
