/*
 * File:        pluginbrowsedialog.h
 * Module:      orc-gui
 * Purpose:     Browse and install plugins from the curated index
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGINBROWSEDIALOG_H
#define ORC_GUI_PLUGINBROWSEDIALOG_H

#include <QDialog>

#include "pluginbrowsemodel.h"
#include "presenters/include/i_project_presenter.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QThread;

namespace orc {

/**
 * @brief Dialog listing curated plugins with search, details and install.
 *
 * The index is fetched asynchronously on open so the UI never blocks on the
 * network; an offline/stale banner is shown when the list came from cache.
 * Installing records an untrusted registry entry and offers explicit trust
 * confirmation. Registry edits are reconciled at startup (see
 * plugin_ux::kRegistryChangeNote), never mid-session.
 */
class PluginBrowseDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PluginBrowseDialog(orc::presenters::IProjectPresenter& presenter,
                              QWidget* parent = nullptr);
  ~PluginBrowseDialog() override;

  // True when a plugin was installed during this session (so the caller can
  // refresh the registry view).
  bool changesMade() const { return changes_made_; }

 private slots:
  void onSearchChanged();
  void onSelectionChanged();
  void onInstall();
  void onRefreshFinished();

 private:
  void buildUI();
  void startRefresh();
  void populateList();
  void updateDetails();
  const orc::presenters::PluginIndexEntryInfo* selectedEntry() const;

  PluginBrowseModel model_;
  QLineEdit* search_edit_ = nullptr;
  QListWidget* list_ = nullptr;
  QLabel* details_label_ = nullptr;
  QLabel* status_label_ = nullptr;
  QPushButton* install_button_ = nullptr;
  QThread* refresh_thread_ = nullptr;
  bool changes_made_ = false;
};

}  // namespace orc

#endif  // ORC_GUI_PLUGINBROWSEDIALOG_H
