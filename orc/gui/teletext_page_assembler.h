/*
 * File:        teletext_page_assembler.h
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and Level 1 page assembly for the
 *              teletext preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXT_PAGE_ASSEMBLER_H
#define TELETEXT_PAGE_ASSEMBLER_H

#include <orc_teletext.h>

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

/**
 * @brief Trailing-frame-window packet cache with on-demand page assembly
 *
 * The teletext preview dialog follows the frame previewer through a carousel
 * medium: random access is inherently approximate, so the dialog accumulates
 * the recovered packets of a trailing window of frames ending at the current
 * frame and assembles the requested page from whatever that window contains
 * ("page seen at frame N" rather than pretended continuous reception).
 * Sequential playback degrades gracefully to live reception because the
 * window advances one frame at a time and cached frames are retained.
 *
 * Qt-free by design so page assembly is unit-testable without a QApplication.
 * Thread safety: none; confine an instance to the GUI thread.
 */
class TeletextPageAssembler {
 public:
  /**
   * Trailing window length in frames (2 s of 625/50 video). Page
   * transmissions span only a few fields, so the window bounds how far back
   * the previewer looks for the most recent transmission of the requested
   * page without requesting observations for a whole carousel cycle
   * (typically tens of seconds) on every frame change.
   */
  static constexpr uint64_t kTrailingWindowFrames = 50;

  /**
   * @brief Advance the window so it ends at @p frame_index
   *
   * Cached frames that fall outside the new window are evicted.
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

  bool hasFrame(uint64_t frame_index) const;

  /// Drop all cached frames (node/DAG change or project close)
  void clear();

  /**
   * @brief Assemble the most recent transmission of a page from the window
   *
   * Feeds all cached packets to a fresh page decoder in temporal order
   * (ascending frame, field 1 then field 2, ascending line) and returns the
   * last completed snapshot of the requested page, or std::nullopt when the
   * page was not seen in the window.
   *
   * @param magazine    Displayed magazine number 1-8
   * @param page_number Two-digit hexadecimal page number 0x00-0xFF
   */
  std::optional<orc::presenters::TeletextPageView> assemblePage(
      int magazine, int page_number) const;

 private:
  struct FrameData {
    orc::presenters::TeletextFieldPacketsView field1;
    orc::presenters::TeletextFieldPacketsView field2;
  };

  uint64_t current_frame_ = 0;
  // Keyed by frame index; std::map keeps temporal order for decoder feeding.
  std::map<uint64_t, FrameData> frames_;
};

#endif  // TELETEXT_PAGE_ASSEMBLER_H
