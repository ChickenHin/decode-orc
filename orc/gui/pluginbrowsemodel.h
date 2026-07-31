/*
 * File:        pluginbrowsemodel.h
 * Module:      orc-gui
 * Purpose:     Presenter-boundary model for the curated plugin browse dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGINBROWSEMODEL_H
#define ORC_GUI_PLUGINBROWSEMODEL_H

#include <string>
#include <vector>

#include "presenters/include/i_project_presenter.h"

namespace orc {

/**
 * @brief Backing model for the "Browse plugins" dialog.
 *
 * Wraps IProjectPresenter so index fetch/search/install run through a mockable
 * seam. refresh() performs the (potentially slow) network fetch and caches the
 * resulting list in-memory; search() then filters that list without further
 * I/O. Not thread-safe: callers must serialise refresh() against reads.
 */
class PluginBrowseModel {
 public:
  explicit PluginBrowseModel(orc::presenters::IProjectPresenter& presenter)
      : presenter_(presenter) {}

  // Fetch the index (network or cached fallback) and store it, together with
  // the local registry so an entry's installed copy can be described alongside
  // the release the index offers.
  void refresh() {
    index_ = presenter_.fetchPluginIndex();
    registry_ = presenter_.getPluginRegistry();
  }

  const orc::presenters::PluginIndexInfo& index() const { return index_; }

  /// The registry entry for an index id, or nullptr when it is not installed.
  const orc::presenters::PluginRegistryEntryInfo* installedEntry(
      const std::string& plugin_id) const;

  bool available() const { return index_.available; }
  bool offline() const { return index_.offline; }
  bool fromCache() const { return index_.from_cache; }

  // Case-insensitive filter over id, name, description and tags. An empty term
  // returns every entry.
  std::vector<orc::presenters::PluginIndexEntryInfo> search(
      const std::string& term) const;

  // Human-readable one-line status for the dialog banner.
  std::string statusMessage() const;

  orc::presenters::PluginRegistryMutationResult install(
      const std::string& plugin_id) {
    return presenter_.installPluginFromIndex(plugin_id);
  }

  // Grant trust to an installed entry after explicit confirmation.
  orc::presenters::PluginRegistryMutationResult trust(
      const std::string& plugin_id) {
    return presenter_.setPluginTrusted(plugin_id, true);
  }

 private:
  orc::presenters::IProjectPresenter& presenter_;
  orc::presenters::PluginIndexInfo index_;
  orc::presenters::PluginRegistryInfo registry_;
};

}  // namespace orc

#endif  // ORC_GUI_PLUGINBROWSEMODEL_H
