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
#include <QGroupBox>
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
class IProjectPresenter;
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
 * asks for the explicit trust confirmation.  Registry edits are reconciled at
 * startup (see plugin_ux::kRegistryChangeNote), never mid-session.
 */
class PluginManagerDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PluginManagerDialog(QWidget* parent = nullptr);
  /**
   * @brief Construct against a caller-owned presenter.
   *
   * @param presenter Presenter to read and mutate the registry through; must
   *                  outlive the dialog.
   */
  explicit PluginManagerDialog(orc::presenters::IProjectPresenter& presenter,
                               QWidget* parent = nullptr);
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
  // Shared by both constructors once the presenter and model are in place.
  void initialise();
  void buildUI();
  void refresh();
  // Describe the selected row in the same fields, in the same order, as
  // 'orc-cli plugins info' prints for the same entry.
  void updateDetailsPane();
  // Fill the Diagnostics section with what the runtime reported at startup;
  // 'orc-cli plugins doctor' prints the same lines.
  void refreshDiagnostics();
  void captureInitialRegistrySnapshot();
  bool restoreInitialRegistrySnapshot(QString* error_message);
  // Show the explicit trust warning and, if the user accepts, record the entry
  // as trusted. Returns false when trust was declined or could not be written.
  bool grantTrustWithConfirmation(const std::string& selector);
  // Query each remote plugin's source repository for its latest release on a
  // worker thread; the Update column shows "Checking..." until it finishes.
  void startUpdateCheck();
  // Text/tooltip for the Update column of one registry row.
  void applyUpdateStatusToRow(int row, const std::string& plugin_id);

  // Owned only when the dialog was constructed without one.
  std::unique_ptr<orc::presenters::ProjectPresenter> owned_presenter_;
  orc::presenters::IProjectPresenter* presenter_ = nullptr;
  std::unique_ptr<PluginManagerModel> model_;

  QLabel* registry_path_label_;
  QTableWidget* table_;
  QLabel* details_label_ = nullptr;
  QGroupBox* diagnostics_group_ = nullptr;
  QLabel* diagnostics_label_ = nullptr;
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
  // One registry entry per table row, so the details pane describes exactly
  // the row the user selected (including runtime-only plugins, which are
  // presented as the registry entry they would become).
  std::vector<orc::presenters::PluginRegistryEntryInfo> row_entries_;
  std::unordered_set<std::string> removed_paths_this_session_;
  std::string initial_registry_path_;
  std::string initial_registry_contents_;
  bool initial_registry_exists_ = false;
};

}  // namespace orc

#endif  // ORC_GUI_PLUGINMANAGERDIALOG_H
