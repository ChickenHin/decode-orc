/*
 * File:        project_load_status_formatter.h
 * Module:      orc-gui
 * Purpose:     Pure helper that turns on-demand DAG execution progress into the
 *              project-load progress dialog's label (Tier 1 / gui-logic
 *              testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>

namespace orc::gui {

// Label for the modal shown while a freshly opened project's source is being
// prepared. Two things make the wait bearable: naming the stage the worker is
// actually executing, and an elapsed-seconds counter that keeps ticking even
// though a single stage (a large source opening) can hold position "1 of 2" for
// a long time.
//
// @param stage_label     Node label or stage name being executed; empty before
//                        the first progress event arrives (the worker is still
//                        rebuilding renderers), which yields the generic
//                        "Preparing preview…" text.
// @param current         1-based position in the execution order.
// @param total           Number of nodes in the execution order.
// @param elapsed_seconds Seconds since the load started; omitted when < 1 so
//                        the text does not flicker on fast projects.
inline std::string formatProjectLoadStatus(const std::string& stage_label,
                                           std::uint64_t current,
                                           std::uint64_t total,
                                           int elapsed_seconds) {
  std::string text;
  if (stage_label.empty() || total == 0) {
    text = "Preparing preview\xE2\x80\xA6";
  } else {
    text = "Executing " + stage_label;
    if (total > 1) {
      text +=
          " (" + std::to_string(current) + " of " + std::to_string(total) + ")";
    }
    text += "\xE2\x80\xA6";
  }
  if (elapsed_seconds >= 1) {
    text += " " + std::to_string(elapsed_seconds) + "s";
  }
  return text;
}

}  // namespace orc::gui
