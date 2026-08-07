/*
 * File:        teletext_analysis_presenter.h
 * Module:      orc-presenters
 * Purpose:     Teletext analysis presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/analysis_sink_results.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc_teletext.h>

namespace orc::presenters {

/**
 * @brief Presenter for the teletext analysis sink's results
 *
 * Converts the SDK dataset the stage caches from its last trigger run — the
 * page catalogue and the aggregate recovery figures — into value-type view
 * models, hiding the SDK decoder types from the GUI layer.
 */
class TeletextAnalysisPresenter {
 public:
  /**
   * @brief Convert a triggered stage's dataset into the viewer's model
   *
   * Page order is carried through unchanged (ascending by {magazine, page
   * number}); the viewer decides how to present it.
   */
  static TeletextAnalysisView makeAnalysisView(
      const TeletextAnalysisDataset& dataset);

  /**
   * @brief Convert an SDK Level 1 page snapshot into a renderable page view
   *
   * Maps 7-bit G0 character codes to Unicode using the Latin G0 set
   * (ETSI EN 300 706 §15.6.1 Table 35) with the national option sub-set the
   * page's header selected (§15.6.2 Table 36), and mosaic codes to sixel
   * patterns (§15.7.1 Table 47).
   */
  static TeletextPageView makePageView(const TeletextPageSnapshot& snapshot);
};

}  // namespace orc::presenters
