/*
 * File:        plugin_row_presentation.h
 * Module:      orc-gui
 * Purpose:     Map a plugin's load state onto its Plugin Manager row
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGIN_ROW_PRESENTATION_H
#define ORC_GUI_PLUGIN_ROW_PRESENTATION_H

#include <string>

#include "presenters/include/plugin_load_state.h"

namespace orc {

/**
 * @brief How one registry row renders, derived solely from its load state.
 *
 * Qt-free so the mapping can be tested without a widget; the dialog does no
 * further derivation of its own.
 */
struct PluginRowPresentation {
  /// Ticked means "will load at the next launch".
  bool enabled_checked = false;
  /// False when toggling the tick could not change the outcome (a core plugin,
  /// or a binary that cannot run on this host at all).
  bool enabled_interactive = false;
  /// True when the version cell should carry the needs-a-rebuild marker.
  bool warn_version = false;
  /// True when ticking must ask for the trust confirmation first.
  bool tick_grants_trust = false;
  /// Tooltip explaining the state; empty for an entry that simply loads.
  std::string tooltip;
};

/**
 * @brief Derive a row's presentation from the presenter-computed load state.
 *
 * @param state Load state computed by the presenter.
 * @param detail Matching detail clause (carries ABI numbers or missing path).
 */
inline PluginRowPresentation makePluginRowPresentation(
    orc::presenters::PluginLoadState state, const std::string& detail) {
  using orc::presenters::PluginLoadState;

  PluginRowPresentation presentation;
  presentation.tooltip = orc::presenters::pluginLoadStateSummary(state, detail);

  switch (state) {
    case PluginLoadState::Core:
      presentation.enabled_checked = true;
      break;
    case PluginLoadState::WillLoad:
      presentation.enabled_checked = true;
      presentation.enabled_interactive = true;
      presentation.tooltip.clear();
      break;
    case PluginLoadState::Disabled:
      presentation.enabled_interactive = true;
      break;
    case PluginLoadState::NotTrusted:
      presentation.enabled_interactive = true;
      presentation.tick_grants_trust = true;
      break;
    case PluginLoadState::AbiMismatch:
      presentation.warn_version = true;
      break;
    case PluginLoadState::FileMissing:
      break;
  }

  return presentation;
}

}  // namespace orc

#endif  // ORC_GUI_PLUGIN_ROW_PRESENTATION_H
