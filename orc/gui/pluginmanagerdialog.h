/*
 * File:        pluginmanagerdialog.h
 * Module:      orc-gui
 * Purpose:     Plugin registry management dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGINMANAGERDIALOG_H
#define ORC_GUI_PLUGINMANAGERDIALOG_H

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "presenters/include/project_presenter_types.h"

class QThread;

namespace orc {
namespace presenters {
class ProjectPresenter;
}  // namespace presenters

class PluginManagerModel;

/**
 * @brief Dialog for managing the persistent plugin registry.
 *
 * Shows registered plugins and their load/filesystem status, and allows the
 * user to add, remove, enable and disable plugins through a mockable
 * presenter-boundary model.  The Enabled checkbox means "will load at the next
 * launch", so ticking an entry whose binary has not been trusted yet (a CLI
 * install, a hand-edited registry file, or a freshly updated release) first
 * asks for the explicit trust confirmation.  Registry changes take effect on
 * the next application launch.
 */
class PluginManagerDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PluginManagerDialog(QWidget* parent = nullptr);
  ~PluginManagerDialog() override;

 protected:
  void accept() override;
  void reject() override;

 private slots:
  void onAddPlugin();
  void onBrowsePlugins();
  void onRemovePlugin();
  void onUpdatePlugin();
  void onSelectionChanged();
  void onTableItemChanged(QTableWidgetItem* item);
  void onUpdateCheckFinished();

 private:
  void buildUI();
  void refresh();
  void captureInitialRegistrySnapshot();
  bool restoreInitialRegistrySnapshot(QString* error_message);
  // Show the explicit trust warning and, if the user accepts, record the entry
  // as trusted. Returns false when trust was declined or could not be written.
  bool grantTrustWithConfirmation(const std::string& plugin_id);
  // Query each remote plugin's source repository for its latest release on a
  // worker thread; the Update column shows "Checking..." until it finishes.
  void startUpdateCheck();
  // Text/tooltip for the Update column of one registry row.
  void applyUpdateStatusToRow(int row, const std::string& plugin_id);

  std::unique_ptr<orc::presenters::ProjectPresenter> presenter_;
  std::unique_ptr<PluginManagerModel> model_;

  QLabel* registry_path_label_;
  QTableWidget* table_;
  QPushButton* add_button_;
  QPushButton* browse_button_;
  QPushButton* remove_button_;
  QPushButton* update_button_;
  // Core plugins ship with the application; hiding them by default leaves the
  // table showing only what the user installed themselves.
  QCheckBox* show_core_check_;
  QThread* update_check_thread_ = nullptr;
  bool update_check_running_ = false;
  bool update_check_completed_ = false;
  std::vector<orc::presenters::PluginUpdateStatusInfo> pending_update_results_;
  std::map<std::string, orc::presenters::PluginUpdateStatusInfo>
      update_statuses_;
  bool refreshing_table_ = false;
  bool plugin_changes_made_ = false;
  std::unordered_set<std::string> removed_paths_this_session_;
  std::string initial_registry_path_;
  std::string initial_registry_contents_;
  bool initial_registry_exists_ = false;
};

}  // namespace orc

#endif  // ORC_GUI_PLUGINMANAGERDIALOG_H
