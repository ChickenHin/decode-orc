/*
 * File:        pluginmanagermodel.h
 * Module:      orc-gui
 * Purpose:     Presenter-boundary model for plugin registry management
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGINMANAGERMODEL_H
#define ORC_GUI_PLUGINMANAGERMODEL_H

#include <string>
#include <vector>

#include "presenters/include/i_project_presenter.h"

namespace orc {

/**
 * @brief Registry-management operations for the Plugin Manager dialog.
 *
 * Wraps IProjectPresenter so the dialog's mutations run through a mockable
 * seam. Crucially, enabling and trusting are separate operations: setEnabled()
 * never grants trust, so a user must confirm trust explicitly.
 */
class PluginManagerModel {
 public:
  explicit PluginManagerModel(orc::presenters::IProjectPresenter& presenter)
      : presenter_(presenter) {}

  orc::presenters::PluginRegistryInfo registry() const {
    return presenter_.getPluginRegistry();
  }
  std::vector<orc::presenters::LoadedPluginInfo> loadedPlugins() const {
    return presenter_.listLoadedPlugins();
  }

  // Enable/disable only. Does NOT touch trust state.
  orc::presenters::PluginRegistryMutationResult setEnabled(
      const std::string& plugin_id, bool enabled) {
    return presenter_.setPluginEnabled(plugin_id, enabled);
  }

  // Trust/untrust. Called only after explicit user confirmation.
  orc::presenters::PluginRegistryMutationResult setTrusted(
      const std::string& plugin_id, bool trusted) {
    return presenter_.setPluginTrusted(plugin_id, trusted);
  }

  orc::presenters::PluginRegistryMutationResult addEntry(
      const orc::presenters::PluginRegistryEntryInfo& entry) {
    return presenter_.addPluginEntry(entry);
  }

  orc::presenters::PluginRegistryMutationResult addFromUrl(
      const std::string& releases_url, bool trusted) {
    return presenter_.addPluginFromUrl(releases_url, trusted);
  }

  // Network-bound: one GitHub API request per distinct source repository.
  // Callers must run this off the UI thread.
  std::vector<orc::presenters::PluginUpdateStatusInfo> checkUpdates() const {
    return presenter_.checkPluginUpdates();
  }

  // Rewrites the entry to the latest upstream release. The entry reverts to
  // untrusted; the dialog offers the explicit trust confirmation afterwards.
  orc::presenters::PluginRegistryMutationResult updateToLatest(
      const std::string& plugin_id) {
    return presenter_.updatePluginToLatestRelease(plugin_id);
  }

 private:
  orc::presenters::IProjectPresenter& presenter_;
};

}  // namespace orc

#endif  // ORC_GUI_PLUGINMANAGERMODEL_H
