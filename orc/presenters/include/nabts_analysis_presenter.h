/*
 * File:        nabts_analysis_presenter.h
 * Module:      orc-presenters
 * Purpose:     NABTS analysis presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc_nabts.h>

#include <vector>

#include "vbi-services/nabts_page.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc::presenters {

/**
 * @brief Presenter for the NABTS sink's results
 *
 * Converts the SDK dataset the stage caches from its last trigger run — the
 * record catalogue and the aggregate recovery figures — into value-type view
 * models, so the GUI holds no NAPLPS knowledge.
 *
 * Three things are resolved here rather than left to a renderer, because each
 * needs a standard the GUI has no business reading:
 *
 * - **Colour.** X3.110 §5.3.1 expresses a colour as three bits per gun; a
 *   renderer wants 8-bit channels. Where the record was in a colour-mapped mode
 *   the map has already been applied by the interpreter, but the incremental
 *   colour runs of §5.3.3.6.3 are still map addresses and are resolved here.
 * - **Characters.** A kCharacter primitive names a code position and a G-set;
 *   §7's repertoires say what that is. Runs of them are coalesced into text
 *   runs, and the non-spacing marks of Tables 26 and 27 are composed onto the
 *   letters that follow them.
 * - **Captions.** CEA-516 §7.3.10's caption records become a cue list with
 *   frame extents, which is a reading of the service rather than of any one
 *   record.
 */
class NabtsAnalysisPresenter {
 public:
  /**
   * @brief Convert a triggered stage's dataset into the viewer's model
   *
   * Record order is carried through unchanged — ascending by {channel, record
   * address, version}, which is the order §7.3's Next-record rule steps a
   * service in — and the caption cues are built from it.
   */
  static NabtsAnalysisView makeAnalysisView(
      const NabtsAnalysisDataset& dataset);

  /**
   * @brief Convert one decoded presentation record into a drawable display list
   *
   * Public because it is the interesting half of the conversion and is tested
   * directly against synthetic display lists.
   */
  static NabtsPageView makePageView(const NabtsPageSnapshot& snapshot);

  /**
   * @brief The caption cues a catalogue carries (CEA-516 §7.3.10)
   *
   * A thin conversion of nabts_caption_cues(), which is where the reading of
   * the caption service lives: the sink stage's SubRip export reads it the same
   * way, and the two must not disagree.
   */
  static std::vector<NabtsCaptionCueView> makeCaptionCues(
      const std::vector<NabtsCataloguedRecord>& records);
};

}  // namespace orc::presenters
