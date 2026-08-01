/*
 * File:        teletext_observation_presenter.h
 * Module:      orc-presenters
 * Purpose:     Teletext observation presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc_teletext.h>

// Forward declare core type to avoid exposing it in GUI layer
namespace orc {
class ObservationContext;
}

namespace orc::presenters {

/**
 * @brief Presenter for teletext observations
 *
 * Converts the "teletext" observation namespace (hex-encoded `t42_<line>`
 * strings written by the teletext observer) into value-type view models,
 * and converts SDK page-decoder snapshots into renderable page views.
 *
 * This presenter hides the variant-based observation storage and the SDK
 * decoder types from the GUI layer.
 */
class TeletextObservationPresenter {
 public:
  /**
   * @brief Extract teletext observations for a single field
   *
   * @param field_id Field to extract observations for
   * @param obs_context_ptr Opaque pointer to observation context
   * @return Recovered packets in ascending field-line order; `observed` is
   *         false when the field carries no "teletext" namespace at all
   */
  static TeletextFieldPacketsView extractFieldObservations(
      FieldID field_id, const void* obs_context_ptr);

  /**
   * @brief Convert an SDK Level 1 page snapshot into a renderable page view
   *
   * Maps 7-bit G0 character codes to Unicode using the Latin G0 set
   * (ETSI EN 300 706 §15.6.1 Table 35) with the English national option
   * sub-set (§15.6.2 Table 36), and mosaic codes to sixel patterns
   * (§15.7.1 Table 47).
   */
  static TeletextPageView makePageView(const TeletextPageSnapshot& snapshot);
};

}  // namespace orc::presenters
