/*
 * File:        closed_caption_observation_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Closed caption observation presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/closed_caption_observation_presenter.h"

#include <orc/stage/observation/observation_context.h>

#include <variant>

namespace orc::presenters {

ClosedCaptionFieldDataView
ClosedCaptionObservationPresenter::extractFieldObservations(
    FieldID field_id, const void* obs_context_ptr) {
  ClosedCaptionFieldDataView result;
  if (obs_context_ptr == nullptr) {
    return result;
  }
  const auto* obs_context =
      static_cast<const orc::ObservationContext*>(obs_context_ptr);

  auto present_obs = obs_context->get(field_id, "closed_caption", "present");
  if (!present_obs) {
    // Field never observed (or a source the observer produces nothing for).
    return result;
  }
  result.observed = true;
  if (std::holds_alternative<bool>(*present_obs)) {
    result.present = std::get<bool>(*present_obs);
  }
  if (!result.present) {
    // The observer writes the data keys only alongside a true "present".
    return result;
  }

  auto data0_obs = obs_context->get(field_id, "closed_caption", "data0");
  auto data1_obs = obs_context->get(field_id, "closed_caption", "data1");
  if (data0_obs && std::holds_alternative<int32_t>(*data0_obs)) {
    result.data0 = std::get<int32_t>(*data0_obs);
  }
  if (data1_obs && std::holds_alternative<int32_t>(*data1_obs)) {
    result.data1 = std::get<int32_t>(*data1_obs);
  }

  auto parity0_obs =
      obs_context->get(field_id, "closed_caption", "parity0_valid");
  auto parity1_obs =
      obs_context->get(field_id, "closed_caption", "parity1_valid");
  if (parity0_obs && std::holds_alternative<bool>(*parity0_obs)) {
    result.parity0_valid = std::get<bool>(*parity0_obs);
  }
  if (parity1_obs && std::holds_alternative<bool>(*parity1_obs)) {
    result.parity1_valid = std::get<bool>(*parity1_obs);
  }

  return result;
}

}  // namespace orc::presenters
