/*
 * File:        closed_caption_observation_presenter.h
 * Module:      orc-presenters
 * Purpose:     Closed caption observation presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>
#include <orc_closed_caption.h>

// Forward declare core type to avoid exposing it in GUI layer
namespace orc {
class ObservationContext;
}

namespace orc::presenters {

/**
 * @brief Presenter for closed caption observations
 *
 * Converts the "closed_caption" observation namespace written by the closed
 * caption observer into a value-type view model, hiding the variant-based
 * observation storage from the GUI layer.
 */
class ClosedCaptionObservationPresenter {
 public:
  /**
   * @brief Extract the closed caption observation of a single field
   *
   * @param field_id         Field to extract observations for
   * @param obs_context_ptr  Opaque pointer to observation context
   * @return The field's byte pair; `observed` is false when the field carries
   *         no "closed_caption" namespace at all
   */
  static ClosedCaptionFieldDataView extractFieldObservations(
      FieldID field_id, const void* obs_context_ptr);
};

}  // namespace orc::presenters
