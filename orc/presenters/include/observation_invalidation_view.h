/*
 * File:        observation_invalidation_view.h
 * Module:      orc-presenters
 * Purpose:     View-facing payload for observation invalidation notifications
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/node_id.h>

#include <functional>
#include <vector>

namespace orc::presenters {

/**
 * @brief Notification payload delivered when a project edit invalidates stored
 *        observations.
 *
 * Contains only view-facing types (node IDs) — no Qt and no host-internal
 * provenance types — so it can cross the presenter/view boundary. The GUI
 * marshals this to the main thread in its coordinator.
 */
struct ObservationInvalidationEvent {
  /// Nodes whose stored observations became stale, sorted by NodeID. Downstream
  /// descendants of an edited node are already included.
  std::vector<NodeID> changed_nodes;
};

/// Callback invoked with an invalidation event. Fired synchronously on the
/// thread that rebuilds the DAG; subscribers must marshal to their own thread.
using ObservationInvalidationCallback =
    std::function<void(const ObservationInvalidationEvent&)>;

}  // namespace orc::presenters
