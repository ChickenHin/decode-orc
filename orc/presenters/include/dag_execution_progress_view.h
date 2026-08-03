/*
 * File:        dag_execution_progress_view.h
 * Module:      orc-presenters
 * Purpose:     View-facing payload for on-demand DAG execution progress driven
 *              by preview queries (project open, stage selection)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <functional>

namespace orc::presenters {

/**
 * @brief One node of an on-demand preview execution is about to run.
 *
 * Preview queries execute the DAG up to the queried node. Opening a large
 * source can take many seconds inside a single node, so the view uses these
 * events to tell the user what the worker is doing rather than leaving the
 * window silent. Contains only view-facing scalars — no core types — so it can
 * cross the presenter/view boundary. The node is identified by
 * @c NodeID::value(); resolving it to a stage label is the view's job.
 */
struct DagExecutionProgressEvent {
  int node_id = 0;            ///< NodeID::value() of the node about to run.
  std::uint64_t current = 0;  ///< 1-based position in the execution order.
  std::uint64_t total = 0;    ///< Number of nodes in the execution order.
};

/// Callback invoked immediately before each node of an on-demand execution
/// runs. Fired synchronously on whichever thread drives the preview query (the
/// render coordinator's worker thread in the GUI); subscribers must marshal to
/// their own thread.
using DagExecutionProgressCallback =
    std::function<void(const DagExecutionProgressEvent&)>;

}  // namespace orc::presenters
