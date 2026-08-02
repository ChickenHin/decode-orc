/*
 * File:        vbi_presenter.h
 * Module:      orc-presenters
 * Purpose:     VBI observation presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/field_id.h>

#include <optional>

#include "vbi_view_models.h"

namespace orc {
enum class VbiSoundMode;
}  // namespace orc

namespace orc::presenters {

/**
 * @brief Stateless VBI view-model extraction from a delivered observation
 *        context
 *
 * Mirrors the other observation presenters (NTSC, video parameters,
 * teletext): observations are produced by the async requestObservations()
 * path and this presenter only turns the delivered context into value-type
 * view models. It never renders and never holds a DAG.
 */
class VbiPresenter {
 public:
  VbiPresenter() = delete;

  // Public helper for sound mode conversion (for use in callbacks)
  static VbiSoundModeView mapSoundMode(orc::VbiSoundMode mode);

  // Decode a field's VBI from an observation context delivered by
  // requestObservations(). The context is passed opaquely so callers (e.g.
  // render_coordinator) need no core headers. Never forward the pointer past
  // the delivering callback.
  static std::optional<VBIFieldInfoView> decodeVbiFromObservation(
      const void*
          observation_context_ptr,  ///< Opaque handle to observation context
      FieldID field_id);

  // Merge two field VBI views into a single frame-level interpretation
  static VBIFieldInfoView mergeFrameVbiViews(const VBIFieldInfoView& field1,
                                             const VBIFieldInfoView& field2);
};

}  // namespace orc::presenters
