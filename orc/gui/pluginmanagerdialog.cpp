/*
 * File:        pluginmanagerdialog.cpp
 * Module:      orc-gui
 * Purpose:     Plugin registry management dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "pluginmanagerdialog.h"

#include <plugin_ux_strings.h>

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVBoxLayout>
#include <algorithm>
#include <unordered_set>
#include <utility>

#include "plugin_row_presentation.h"
#include "pluginbrowsedialog.h"
#include "pluginmanagermodel.h"
#include "plugintrustdialog.h"
#include "presenters/include/plugin_details.h"
#include "presenters/include/plugin_selector.h"
#include "presenters/include/project_presenter.h"
#include "presenters/include/project_presenter_types.h"

namespace orc {

// Column indices for the registry table
static constexpr int COL_ID = 0;
static constexpr int COL_PATH = 1;
static constexpr int COL_VERSION = 2;
static constexpr int COL_UPDATE = 3;
static constexpr int COL_SOURCE = 4;
static constexpr int COL_ENABLED = 5;
static constexpr int NUM_COLS = 6;
static constexpr int ROW_REGISTRY_ENTRY_ROLE = Qt::UserRole + 1;
// The one handle the row is addressed by; the CLI takes the same string.
static constexpr int ROW_SELECTOR_ROLE = Qt::UserRole + 2;
// Presenter-computed load state, stored as its underlying integer.
static constexpr int ROW_LOAD_STATE_ROLE = Qt::UserRole + 3;

namespace {

orc::presenters::PluginLoadState rowLoadState(const QTableWidgetItem* item) {
  return static_cast<orc::presenters::PluginLoadState>(
      item->data(ROW_LOAD_STATE_ROLE).toInt());
}

}  // namespace

static const QStringList COLUMN_HEADERS = {"ID",     "Path",   "Version",
                                           "Update", "Source", "Enabled"};

PluginManagerDialog::PluginManagerDialog(QWidget* parent)
    : QDialog(parent),
      owned_presenter_(std::make_unique<orc::presenters::ProjectPresenter>()),
      presenter_(owned_presenter_.get()),
      model_(std::make_unique<PluginManagerModel>(*presenter_)) {
  initialise();
}

PluginManagerDialog::PluginManagerDialog(
    orc::presenters::IProjectPresenter& presenter, QWidget* parent)
    : QDialog(parent),
      presenter_(&presenter),
      model_(std::make_unique<PluginManagerModel>(presenter)) {
  initialise();
}

void PluginManagerDialog::initialise() {
  setWindowTitle("Plugin Manager");
  resize(1280, 560);
  buildUI();
  refresh();
  refreshDiagnostics();
  captureInitialRegistrySnapshot();
  startUpdateCheck();
}

PluginManagerDialog::~PluginManagerDialog() {
  if (update_check_thread_) {
    update_check_thread_->wait();
    delete update_check_thread_;
    update_check_thread_ = nullptr;
  }
}

void PluginManagerDialog::accept() {
  if (!plugin_changes_made_) {
    QDialog::accept();
    return;
  }

  QMessageBox restart_box(this);
  restart_box.setWindowTitle("Restart Required");
  restart_box.setIcon(QMessageBox::Question);
  restart_box.setText(
      "Plugin changes require an application restart to take effect.\n\n"
      "Restart now?");
  restart_box.setStandardButtons(QMessageBox::NoButton);
  QPushButton* restart_btn =
      restart_box.addButton("Restart", QMessageBox::AcceptRole);
  QPushButton* cancel_btn =
      restart_box.addButton("Cancel", QMessageBox::RejectRole);
  restart_box.setDefaultButton(restart_btn);
  restart_box.exec();

  if (restart_box.clickedButton() == restart_btn) {
    QDialog::accept();
    QCoreApplication::quit();
    return;
  }

  if (restart_box.clickedButton() == cancel_btn) {
    // Keep dialog open so user can continue editing or choose Cancel.
    return;
  }
}

void PluginManagerDialog::reject() {
  if (!plugin_changes_made_) {
    QDialog::reject();
    return;
  }

  const auto answer = QMessageBox::question(
      this, "Discard Plugin Changes",
      "Discard plugin changes made in this dialog session?",
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

  if (answer != QMessageBox::Yes) {
    return;
  }

  QString restore_error;
  if (!restoreInitialRegistrySnapshot(&restore_error)) {
    QMessageBox::warning(
        this, "Discard Plugin Changes Failed",
        QString("Could not discard plugin changes: %1\n\n"
                "Close cancelled to avoid leaving plugin state inconsistent.")
            .arg(restore_error));
    return;
  }

  plugin_changes_made_ = false;
  removed_paths_this_session_.clear();
  refresh();
  QDialog::reject();
}

void PluginManagerDialog::captureInitialRegistrySnapshot() {
  const auto registry = model_->registry();
  initial_registry_path_ = registry.registry_path;
  initial_registry_contents_.clear();
  initial_registry_exists_ = false;

  if (initial_registry_path_.empty()) {
    return;
  }

  QFile file(QString::fromStdString(initial_registry_path_));
  if (!file.exists()) {
    initial_registry_exists_ = false;
    return;
  }

  if (!file.open(QIODevice::ReadOnly)) {
    initial_registry_exists_ = false;
    return;
  }

  initial_registry_contents_ = file.readAll().toStdString();
  initial_registry_exists_ = true;
}

bool PluginManagerDialog::restoreInitialRegistrySnapshot(
    QString* error_message) {
  if (initial_registry_path_.empty()) {
    if (error_message) {
      *error_message = "Registry path is unknown";
    }
    return false;
  }

  const QString path = QString::fromStdString(initial_registry_path_);
  QFile file(path);

  if (!initial_registry_exists_) {
    if (!file.exists()) {
      return true;
    }
    if (!file.remove()) {
      if (error_message) {
        *error_message = QString("Failed to remove '%1'").arg(path);
      }
      return false;
    }
    return true;
  }

  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    if (error_message) {
      *error_message = QString("Failed to open '%1' for writing").arg(path);
    }
    return false;
  }

  const qint64 written =
      file.write(initial_registry_contents_.c_str(),
                 static_cast<qint64>(initial_registry_contents_.size()));
  if (written != static_cast<qint64>(initial_registry_contents_.size())) {
    if (error_message) {
      *error_message =
          QString("Failed to write full snapshot to '%1'").arg(path);
    }
    return false;
  }

  return true;
}

void PluginManagerDialog::buildUI() {
  auto* root_layout = new QVBoxLayout(this);

  // Registry path row
  auto* path_row = new QHBoxLayout();
  path_row->addWidget(new QLabel("Registry:"));
  registry_path_label_ = new QLabel();
  registry_path_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  path_row->addWidget(registry_path_label_, 1);
  root_layout->addLayout(path_row);

  // Plugin table
  table_ = new QTableWidget(0, NUM_COLS, this);
  table_->setHorizontalHeaderLabels(COLUMN_HEADERS);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSelectionMode(QAbstractItemView::SingleSelection);
  table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  auto* header = table_->horizontalHeader();
  header->setSectionResizeMode(COL_ID, QHeaderView::Interactive);
  header->setSectionResizeMode(COL_PATH, QHeaderView::Interactive);
  header->setSectionResizeMode(COL_VERSION, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(COL_UPDATE, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(COL_SOURCE, QHeaderView::Stretch);
  header->setSectionResizeMode(COL_ENABLED, QHeaderView::ResizeToContents);
  QFont header_font = header->font();
  header_font.setBold(false);
  header->setFont(header_font);
  for (int col = 0; col < NUM_COLS; ++col) {
    if (auto* header_item = table_->horizontalHeaderItem(col)) {
      header_item->setFont(header_font);
    }
  }
  table_->setColumnWidth(COL_ID, 260);
  table_->setColumnWidth(COL_PATH, 220);
  table_->verticalHeader()->setVisible(false);

  // Details for the selected entry, worded and ordered by the presenter so the
  // pane and 'orc-cli plugins info' describe a plugin identically.
  auto* details_group = new QGroupBox("Details", this);
  auto* details_layout = new QVBoxLayout(details_group);
  details_label_ = new QLabel(this);
  details_label_->setObjectName("pluginDetailsLabel");
  details_label_->setWordWrap(true);
  details_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  details_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
  details_layout->addWidget(details_label_, 1);
  details_group->setMinimumWidth(320);

  auto* content_row = new QHBoxLayout();
  content_row->addWidget(table_, 2);
  content_row->addWidget(details_group, 1);
  root_layout->addLayout(content_row, 1);

  // Action buttons
  auto* button_row = new QHBoxLayout();
  add_button_ = new QPushButton("Add Plugin...");
  browse_button_ = new QPushButton("Browse Plugins...");
  remove_button_ = new QPushButton("Remove");
  update_button_ = new QPushButton("Update");
  update_button_->setToolTip(
      "Update the selected plugin to its latest published release");
  button_row->addWidget(browse_button_);
  button_row->addWidget(add_button_);
  button_row->addWidget(remove_button_);
  button_row->addWidget(update_button_);
  button_row->addStretch();
  show_core_check_ = new QCheckBox("Show core plugins");
  show_core_check_->setChecked(false);
  show_core_check_->setToolTip(
      "Show the plugins that ship with the application as well as the ones "
      "you have installed");
  button_row->addWidget(show_core_check_);
  root_layout->addLayout(button_row);

  // What the plugin runtime reported while loading, collapsed by default: it
  // matters when something did not appear, not on every visit.
  diagnostics_group_ =
      new QGroupBox(QString::fromUtf8(plugin_ux::kDiagnosticsTitle), this);
  diagnostics_group_->setObjectName("pluginDiagnosticsGroup");
  diagnostics_group_->setCheckable(true);
  diagnostics_group_->setChecked(false);
  auto* diagnostics_layout = new QVBoxLayout(diagnostics_group_);
  diagnostics_label_ = new QLabel(this);
  diagnostics_label_->setObjectName("pluginDiagnosticsLabel");
  diagnostics_label_->setWordWrap(true);
  diagnostics_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  diagnostics_label_->setVisible(false);
  diagnostics_layout->addWidget(diagnostics_label_);
  root_layout->addWidget(diagnostics_group_);

  // Commit / cancel buttons
  auto* close_box = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  root_layout->addWidget(close_box);

  // Note label
  auto* note_label =
      new QLabel(QString::fromUtf8(plugin_ux::kNotePrefix) +
                 QString::fromUtf8(plugin_ux::kRegistryChangeNote));
  note_label->setEnabled(false);
  root_layout->addWidget(note_label);

  // Connect signals
  connect(add_button_, &QPushButton::clicked, this,
          &PluginManagerDialog::onAddPlugin);
  connect(browse_button_, &QPushButton::clicked, this,
          &PluginManagerDialog::onBrowsePlugins);
  connect(remove_button_, &QPushButton::clicked, this,
          &PluginManagerDialog::onRemovePlugin);
  connect(update_button_, &QPushButton::clicked, this,
          &PluginManagerDialog::onUpdatePlugin);
  connect(show_core_check_, &QCheckBox::toggled, this,
          [this](bool) { refresh(); });
  connect(diagnostics_group_, &QGroupBox::toggled, diagnostics_label_,
          &QWidget::setVisible);
  connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, &PluginManagerDialog::onSelectionChanged);
  connect(table_, &QTableWidget::itemChanged, this,
          &PluginManagerDialog::onTableItemChanged);
  connect(close_box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(close_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  onSelectionChanged();
}

void PluginManagerDialog::refresh() {
  const auto registry = model_->registry();
  const auto loaded_plugins = model_->loadedPlugins();

  registry_path_label_->setText(
      registry.registry_path.empty()
          ? "<none>"
          : QString::fromStdString(registry.registry_path));

  // Core plugins are hidden unless the user asks for them, so the table shows
  // only self-installed plugins by default.
  const bool show_core = show_core_check_->isChecked();

  refreshing_table_ = true;
  table_->setRowCount(0);
  row_entries_.clear();
  std::unordered_set<std::string> seen_ids;
  std::unordered_set<std::string> seen_paths;

  for (const auto& e : registry.entries) {
    if (e.is_core_plugin && !show_core) {
      // Still record the identity so a duplicate runtime-loaded copy of a
      // hidden core plugin does not reappear in the fallback list below.
      if (!e.plugin_id.empty()) {
        seen_ids.insert(e.plugin_id);
      }
      if (!e.path.empty()) {
        seen_paths.insert(e.path);
      }
      continue;
    }

    // Overlay runtime-discovered data (id, version) when the registry YAML
    // doesn't have it populated yet (e.g. freshly added remote plugins).
    const auto loaded_it = std::find_if(
        loaded_plugins.begin(), loaded_plugins.end(),
        [&e](const auto& lp) { return !lp.path.empty() && lp.path == e.path; });

    const std::string display_id =
        (!e.plugin_id.empty())
            ? e.plugin_id
            : (loaded_it != loaded_plugins.end() ? loaded_it->plugin_id
                                                 : std::string());
    const std::string display_version =
        (!e.plugin_version.empty())
            ? e.plugin_version
            : (loaded_it != loaded_plugins.end() ? loaded_it->plugin_version
                                                 : std::string());

    // Ticked means "will load at the next launch", and every other cue on the
    // row comes from the same presenter-computed state.
    const auto presentation =
        makePluginRowPresentation(e.load_state, e.load_state_detail);

    // The details pane describes what the row shows, so it carries the same
    // runtime-overlaid id and version.
    auto row_entry = e;
    row_entry.plugin_id = display_id;
    row_entry.plugin_version = display_version;
    row_entries_.push_back(std::move(row_entry));

    const int row = table_->rowCount();
    table_->insertRow(row);
    auto* id_item = new QTableWidgetItem(QString::fromStdString(display_id));
    id_item->setData(ROW_REGISTRY_ENTRY_ROLE, true);
    id_item->setData(ROW_SELECTOR_ROLE, QString::fromStdString(e.selector));
    id_item->setData(ROW_LOAD_STATE_ROLE, static_cast<int>(e.load_state));
    table_->setItem(row, COL_ID, id_item);
    table_->setItem(row, COL_PATH,
                    new QTableWidgetItem(QString::fromStdString(e.path)));
    auto* version_item =
        new QTableWidgetItem(QString::fromStdString(display_version));
    if (presentation.warn_version) {
      // The binary cannot load on this host; flag it and explain the fix.
      version_item->setText(version_item->text() +
                            QString::fromUtf8(plugin_ux::kNeedsRebuildMarker));
      version_item->setToolTip(QString::fromStdString(presentation.tooltip));
    }
    table_->setItem(row, COL_VERSION, version_item);

    table_->setItem(row, COL_UPDATE, new QTableWidgetItem());
    applyUpdateStatusToRow(row, e.is_core_plugin ? std::string() : display_id);

    auto* enabled_item = new QTableWidgetItem();
    enabled_item->setData(ROW_REGISTRY_ENTRY_ROLE, true);
    enabled_item->setData(ROW_SELECTOR_ROLE,
                          QString::fromStdString(e.selector));
    enabled_item->setData(ROW_LOAD_STATE_ROLE, static_cast<int>(e.load_state));
    enabled_item->setFlags(
        presentation.enabled_interactive
            ? (Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
               Qt::ItemIsEnabled)
            : (Qt::ItemIsSelectable | Qt::ItemIsUserCheckable));
    enabled_item->setCheckState(presentation.enabled_checked ? Qt::Checked
                                                             : Qt::Unchecked);
    enabled_item->setToolTip(QString::fromStdString(presentation.tooltip));
    table_->setItem(
        row, COL_SOURCE,
        new QTableWidgetItem(QString::fromStdString(e.source_label)));
    table_->setItem(row, COL_ENABLED, enabled_item);

    if (!display_id.empty()) {
      seen_ids.insert(display_id);
    }
    if (!e.path.empty()) {
      seen_paths.insert(e.path);
    }
  }

  // Also show runtime-loaded plugins that are not represented in the registry,
  // but skip any that were removed during this dialog session (they remain
  // in-memory until the next restart but should not re-appear in the list).
  for (const auto& plugin : loaded_plugins) {
    if (plugin.is_core_plugin && !show_core) {
      continue;
    }

    const bool id_seen =
        !plugin.plugin_id.empty() && seen_ids.count(plugin.plugin_id) > 0;
    const bool path_seen =
        !plugin.path.empty() && seen_paths.count(plugin.path) > 0;
    const bool removed = removed_paths_this_session_.count(plugin.path) > 0;
    if (id_seen || path_seen || removed) {
      continue;
    }

    // Present the running plugin as the registry entry it would become — the
    // same projection the CLI's `plugins info` uses for loaded plugins, so
    // the details pane describes it in the same fields as a registered one
    // and the two front ends cannot drift about its identity or state.
    const orc::presenters::PluginRegistryEntryInfo row_entry =
        orc::presenters::makeEntryForLoadedPlugin(plugin);
    const std::string selector = row_entry.selector;
    const auto load_state = row_entry.load_state;
    const std::string source = row_entry.source_label;
    const auto presentation =
        makePluginRowPresentation(load_state, row_entry.load_state_detail);
    row_entries_.push_back(row_entry);

    const int row = table_->rowCount();
    table_->insertRow(row);

    auto* id_item =
        new QTableWidgetItem(QString::fromStdString(plugin.plugin_id));
    id_item->setData(ROW_REGISTRY_ENTRY_ROLE, false);
    id_item->setData(ROW_SELECTOR_ROLE, QString::fromStdString(selector));
    id_item->setData(ROW_LOAD_STATE_ROLE, static_cast<int>(load_state));
    table_->setItem(row, COL_ID, id_item);
    table_->setItem(row, COL_PATH,
                    new QTableWidgetItem(QString::fromStdString(plugin.path)));
    table_->setItem(
        row, COL_VERSION,
        new QTableWidgetItem(QString::fromStdString(plugin.plugin_version)));

    // Runtime-discovered plugins have no registry entry to update.
    table_->setItem(row, COL_UPDATE, new QTableWidgetItem());
    applyUpdateStatusToRow(row, std::string());

    // Unticking materialises the plugin into the registry as disabled.
    auto* enabled_item = new QTableWidgetItem();
    enabled_item->setData(ROW_REGISTRY_ENTRY_ROLE, false);
    enabled_item->setData(ROW_SELECTOR_ROLE, QString::fromStdString(selector));
    enabled_item->setData(ROW_LOAD_STATE_ROLE, static_cast<int>(load_state));
    enabled_item->setFlags(
        presentation.enabled_interactive
            ? (Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
               Qt::ItemIsEnabled)
            : (Qt::ItemIsSelectable | Qt::ItemIsUserCheckable));
    enabled_item->setCheckState(presentation.enabled_checked ? Qt::Checked
                                                             : Qt::Unchecked);
    table_->setItem(row, COL_SOURCE,
                    new QTableWidgetItem(QString::fromStdString(source)));
    table_->setItem(row, COL_ENABLED, enabled_item);
  }

  refreshing_table_ = false;

  onSelectionChanged();
}

void PluginManagerDialog::onSelectionChanged() {
  const bool has_selection = !table_->selectedItems().isEmpty();
  bool is_registry_entry = false;
  bool is_core_plugin = false;
  bool has_removal_identity = false;
  bool update_available = false;

  if (has_selection) {
    const int row = table_->currentRow();
    if (row >= 0) {
      if (auto* id_item = table_->item(row, COL_ID)) {
        is_registry_entry = id_item->data(ROW_REGISTRY_ENTRY_ROLE).toBool();
        is_core_plugin =
            rowLoadState(id_item) == orc::presenters::PluginLoadState::Core;
        const QString plugin_id = id_item->text();
        has_removal_identity =
            !id_item->data(ROW_SELECTOR_ROLE).toString().isEmpty();

        const auto status_it = update_statuses_.find(plugin_id.toStdString());
        update_available =
            status_it != update_statuses_.end() &&
            status_it->second.status ==
                orc::presenters::PluginUpdateStatus::UpdateAvailable;
      }
    }
  }

  const bool can_mutate_registry_entry = has_selection && is_registry_entry &&
                                         !is_core_plugin &&
                                         has_removal_identity;
  remove_button_->setEnabled(can_mutate_registry_entry);
  update_button_->setEnabled(can_mutate_registry_entry && update_available);

  updateDetailsPane();
}

void PluginManagerDialog::updateDetailsPane() {
  if (!details_label_) {
    return;
  }

  const int row = table_->currentRow();
  if (!table_->selectedItems().isEmpty() && row >= 0 &&
      row < static_cast<int>(row_entries_.size())) {
    const auto& entry = row_entries_[static_cast<size_t>(row)];

    // The update status is whatever the background check has established so
    // far; nothing here goes to the network.
    const orc::presenters::PluginUpdateStatusInfo* update = nullptr;
    const auto status_it = update_statuses_.find(entry.plugin_id);
    if (update_check_completed_ && status_it != update_statuses_.end()) {
      update = &status_it->second;
    }

    QString text;
    for (const auto& field :
         orc::presenters::makePluginDetails(&entry, nullptr, update)) {
      text += "<b>" + QString::fromStdString(field.label).toHtmlEscaped() +
              ":</b> " + QString::fromStdString(field.value).toHtmlEscaped() +
              "<br>";
    }
    details_label_->setText(text);
    return;
  }

  details_label_->setText(QString::fromUtf8(plugin_ux::kDetailsNoSelection));
}

void PluginManagerDialog::refreshDiagnostics() {
  if (!diagnostics_label_ || !diagnostics_group_) {
    return;
  }

  const auto diagnostics = model_->diagnostics();
  diagnostics_group_->setTitle(
      QString::fromUtf8(plugin_ux::kDiagnosticsTitle) +
      (diagnostics.empty() ? QString()
                           : QStringLiteral(" (%1)").arg(diagnostics.size())));

  if (diagnostics.empty()) {
    diagnostics_label_->setText(QString::fromUtf8(plugin_ux::kDiagnosticsNone));
    return;
  }

  QStringList lines;
  for (const auto& diagnostic : diagnostics) {
    lines.push_back(QString::fromStdString(
        orc::presenters::formatPluginDiagnostic(diagnostic)));
  }
  diagnostics_label_->setText(lines.join("\n"));
}

void PluginManagerDialog::onTableItemChanged(QTableWidgetItem* item) {
  if (!item || refreshing_table_ || item->column() != COL_ENABLED) {
    return;
  }

  const int row = item->row();
  auto* id_item = table_->item(row, COL_ID);
  if (!id_item) {
    refresh();
    return;
  }

  const bool is_registry_entry =
      id_item->data(ROW_REGISTRY_ENTRY_ROLE).toBool();
  const auto load_state = rowLoadState(id_item);
  const QString selector = id_item->data(ROW_SELECTOR_ROLE).toString();
  const QString plugin_id = id_item->text();
  const QString plugin_path = table_->item(row, COL_PATH)
                                  ? table_->item(row, COL_PATH)->text()
                                  : QString();
  const QString plugin_version = table_->item(row, COL_VERSION)
                                     ? table_->item(row, COL_VERSION)->text()
                                     : QString();
  const bool checked = (item->checkState() == Qt::Checked);

  if (load_state == orc::presenters::PluginLoadState::Core) {
    refresh();
    return;
  }

  if (selector.isEmpty()) {
    QMessageBox::warning(
        this, "Update Plugin Failed",
        "This plugin cannot be identified, so its enabled state cannot be "
        "changed.");
    refresh();
    return;
  }

  // Enabling an entry that is not trusted yet means its binary is about to be
  // downloaded and run, so that is where the trust confirmation belongs.
  const bool enabled = checked;
  const auto presentation =
      makePluginRowPresentation(load_state, std::string());

  if (enabled && presentation.tick_grants_trust) {
    if (!grantTrustWithConfirmation(selector.toStdString())) {
      refresh();  // Revert the checkbox; the entry stays untrusted.
      return;
    }
  }

  if (!is_registry_entry) {
    if (plugin_path.isEmpty()) {
      refresh();
      return;
    }

    orc::presenters::PluginRegistryEntryInfo entry_info;
    entry_info.path = plugin_path.toStdString();
    entry_info.plugin_id = plugin_id.toStdString();
    entry_info.plugin_version = plugin_version.toStdString();
    entry_info.artifact_source = "local_path";
    // This plugin was already loaded from a search path this session, so it is
    // already running and trusted; materialise it as trusted so it keeps
    // loading after restart. This is not the enable path granting trust — the
    // plugin is live now.
    entry_info.trust_state = orc::presenters::kPluginTrustStateTrusted;
    entry_info.enabled = true;

    const auto add_result = model_->addEntry(entry_info);

    if (!add_result.success &&
        add_result.error_message.find("already exists in the registry") ==
            std::string::npos &&
        add_result.error_message.find("is already registered") ==
            std::string::npos) {
      QMessageBox::warning(this, "Update Plugin Failed",
                           QString::fromStdString(add_result.error_message));
      refresh();
      return;
    }
  }

  const auto result = model_->setEnabled(selector.toStdString(), enabled);

  if (!result.success) {
    QMessageBox::warning(
        this, enabled ? "Enable Plugin Failed" : "Disable Plugin Failed",
        QString::fromStdString(result.error_message));
    refresh();
    return;
  }

  plugin_changes_made_ = true;

  refresh();
}

void PluginManagerDialog::onAddPlugin() {
  const QStringList source_modes = {"Local plugin file",
                                    "Remote GitHub releases URL"};

  bool mode_ok = false;
  const QString source_mode = QInputDialog::getItem(
      this, "Add Plugin", "Plugin source:", source_modes, 1, false, &mode_ok);

  if (!mode_ok || source_mode.isEmpty()) {
    return;
  }

  if (source_mode == "Local plugin file") {
#if defined(_WIN32)
    const QString plugin_filter = "Plugin libraries (*.dll)";
#elif defined(__APPLE__)
    const QString plugin_filter = "Plugin libraries (*.dylib)";
#else
    const QString plugin_filter = "Plugin libraries (*.so)";
#endif

    const QString path = QFileDialog::getOpenFileName(
        this, "Select Plugin Binary", QString(), plugin_filter);

    if (path.isEmpty()) {
      return;
    }

    // Adding a plugin means it will run as native code, so trust is
    // confirmed explicitly before the entry is recorded.
    if (!confirmPluginTrust(this)) {
      return;
    }

    orc::presenters::PluginRegistryEntryInfo entry_info;
    entry_info.artifact_source = "local_path";
    entry_info.path = path.toStdString();
    entry_info.enabled = true;
    entry_info.trust_state = orc::presenters::kPluginTrustStateTrusted;

    const auto result = model_->addEntry(entry_info);

    if (!result.success) {
      QMessageBox::warning(this, "Add Plugin Failed",
                           QString::fromStdString(result.error_message));
      return;
    }

    plugin_changes_made_ = true;

    refresh();
    return;
  }

  QInputDialog url_dialog(this);
  url_dialog.setWindowTitle("Add Remote Plugin");
  url_dialog.setLabelText(
      "GitHub releases URL:\n\nThe plugin binary will be downloaded and run "
      "as native code. Only add plugins from sources you trust.");
  url_dialog.setInputMode(QInputDialog::TextInput);
  url_dialog.setTextEchoMode(QLineEdit::Normal);
  url_dialog.setTextValue(
      "https://github.com/simoninns/orc-plugin_skeleton/releases");
  url_dialog.resize(900, url_dialog.sizeHint().height());
  url_dialog.setMinimumWidth(900);

  if (url_dialog.exec() != QDialog::Accepted) {
    return;
  }

  const QString releases_url = url_dialog.textValue();
  if (releases_url.trimmed().isEmpty()) {
    return;
  }

  // Adding a plugin means it will run as native code, so trust is confirmed
  // explicitly before the entry is recorded.
  if (!confirmPluginTrust(this)) {
    return;
  }

  const auto result = model_->addFromUrl(releases_url.trimmed().toStdString(),
                                         /*trusted=*/true);

  if (!result.success) {
    QMessageBox::warning(this, "Add Plugin Failed",
                         QString::fromStdString(result.error_message));
    return;
  }

  plugin_changes_made_ = true;

  refresh();
}

void PluginManagerDialog::onBrowsePlugins() {
  PluginBrowseDialog dialog(*presenter_, this);
  dialog.exec();
  if (dialog.changesMade()) {
    plugin_changes_made_ = true;
    refresh();
  }
}

bool PluginManagerDialog::grantTrustWithConfirmation(
    const std::string& selector) {
  // Granting trust allows the binary to be downloaded and executed, so it
  // always goes through the explicit warning.
  if (!confirmPluginTrust(this)) {
    return false;
  }

  const auto result = model_->setTrusted(selector, true);
  if (!result.success) {
    QMessageBox::warning(this, "Update Trust Failed",
                         QString::fromStdString(result.error_message));
    return false;
  }

  plugin_changes_made_ = true;
  return true;
}

void PluginManagerDialog::onRemovePlugin() {
  const int row = table_->currentRow();
  if (row < 0) {
    return;
  }

  const auto* id_item = table_->item(row, COL_ID);
  if (!id_item) {
    return;
  }

  if (rowLoadState(id_item) == orc::presenters::PluginLoadState::Core) {
    QMessageBox::information(this, "Remove Plugin",
                             "Core plugins cannot be removed.");
    return;
  }

  const QString selector = id_item->data(ROW_SELECTOR_ROLE).toString();
  const QString plugin_path = table_->item(row, COL_PATH)
                                  ? table_->item(row, COL_PATH)->text()
                                  : QString();

  const auto answer = QMessageBox::question(
      this, "Remove Plugin",
      QString("Remove selected plugin from the registry?") +
          (selector.isEmpty() ? QString() : QString("\n\n%1").arg(selector)),
      QMessageBox::Yes | QMessageBox::No);

  if (answer != QMessageBox::Yes) {
    return;
  }

  const auto result = model_->removeEntry(selector.toStdString());

  if (!result.success) {
    QMessageBox::warning(this, "Remove Plugin Failed",
                         QString::fromStdString(result.error_message));
    return;
  }

  // Track the removed path so it is suppressed from the loaded-plugins
  // fallback display during this session (the binary stays in-memory until
  // the next restart, but the user should see it gone immediately).
  if (!plugin_path.isEmpty()) {
    removed_paths_this_session_.insert(plugin_path.toStdString());
  }
  plugin_changes_made_ = true;

  refresh();
}

void PluginManagerDialog::startUpdateCheck() {
  if (update_check_running_) {
    return;
  }
  if (update_check_thread_) {
    update_check_thread_->wait();
    delete update_check_thread_;
    update_check_thread_ = nullptr;
  }

  update_check_running_ = true;
  pending_update_results_.clear();

  // Show "Checking..." on rows that will receive a status.
  for (int row = 0; row < table_->rowCount(); ++row) {
    if (auto* id_item = table_->item(row, COL_ID)) {
      const bool is_registry_entry =
          id_item->data(ROW_REGISTRY_ENTRY_ROLE).toBool();
      const bool is_core =
          rowLoadState(id_item) == orc::presenters::PluginLoadState::Core;
      applyUpdateStatusToRow(row, (is_registry_entry && !is_core)
                                      ? id_item->text().toStdString()
                                      : std::string());
    }
  }

  // The check performs one GitHub API request per distinct repository; run it
  // off the UI thread like the browse dialog's index refresh.
  update_check_thread_ = QThread::create(
      [this]() { pending_update_results_ = model_->checkUpdates(); });
  connect(update_check_thread_, &QThread::finished, this,
          &PluginManagerDialog::onUpdateCheckFinished);
  update_check_thread_->start();
}

void PluginManagerDialog::onUpdateCheckFinished() {
  if (update_check_thread_) {
    update_check_thread_->deleteLater();
    update_check_thread_ = nullptr;
  }
  update_check_running_ = false;
  update_check_completed_ = true;

  update_statuses_.clear();
  for (auto& status : pending_update_results_) {
    update_statuses_.emplace(status.plugin_id, std::move(status));
  }
  pending_update_results_.clear();

  refresh();
}

void PluginManagerDialog::applyUpdateStatusToRow(int row,
                                                 const std::string& plugin_id) {
  auto* item = table_->item(row, COL_UPDATE);
  if (!item) {
    return;
  }

  QString text = QString::fromUtf8(plugin_ux::kUpdateStatusNone);
  QString tooltip;

  if (!plugin_id.empty()) {
    if (update_check_running_) {
      text = QString::fromUtf8(plugin_ux::kUpdateStatusChecking);
    } else if (update_check_completed_) {
      const auto it = update_statuses_.find(plugin_id);
      if (it != update_statuses_.end()) {
        const auto& status = it->second;
        if (status.status !=
            orc::presenters::PluginUpdateStatus::NotApplicable) {
          // The column and the CLI's `update:` field share one wording.
          text = QString::fromStdString(
              orc::presenters::pluginUpdateStatusLabel(status));
        }
        switch (status.status) {
          case orc::presenters::PluginUpdateStatus::UpToDate:
            tooltip = QStringLiteral("Latest release: %1")
                          .arg(QString::fromStdString(status.latest_tag));
            break;
          case orc::presenters::PluginUpdateStatus::UpdateAvailable:
            tooltip = QStringLiteral(
                "A newer release is published; select the plugin and press "
                "Update.");
            break;
          case orc::presenters::PluginUpdateStatus::Unreachable:
            // The label already carries the failure detail; the tooltip
            // repeats it for a truncated cell.
            text = QString::fromUtf8(plugin_ux::kUpdateStatusUnreachable);
            tooltip = QString::fromStdString(status.message);
            break;
          case orc::presenters::PluginUpdateStatus::Unknown:
            tooltip = QStringLiteral(
                "The installed version is unknown, so it cannot be compared "
                "with the latest release.");
            break;
          case orc::presenters::PluginUpdateStatus::NotApplicable:
            break;
        }
      }
    }
  }

  item->setText(text);
  item->setToolTip(tooltip);
}

void PluginManagerDialog::onUpdatePlugin() {
  const int row = table_->currentRow();
  if (row < 0) {
    return;
  }
  const auto* id_item = table_->item(row, COL_ID);
  if (!id_item || row >= static_cast<int>(row_entries_.size())) {
    return;
  }

  // Update statuses are reported per plugin id, read from the row's entry
  // data rather than the rendered cell text; the mutation itself goes
  // through the row's selector like every other registry change.
  const std::string plugin_id =
      row_entries_[static_cast<size_t>(row)].plugin_id;
  const std::string selector =
      id_item->data(ROW_SELECTOR_ROLE).toString().toStdString();
  if (plugin_id.empty()) {
    return;
  }
  const auto status_it = update_statuses_.find(plugin_id);
  if (status_it == update_statuses_.end() ||
      status_it->second.status !=
          orc::presenters::PluginUpdateStatus::UpdateAvailable) {
    return;
  }
  const auto& status = status_it->second;

  const auto answer = QMessageBox::question(
      this, "Update Plugin",
      QStringLiteral(
          "Update '%1' from %2 to %3?\n\n"
          "The new release will be downloaded as a fresh binary, so you will "
          "be asked to confirm that it may run.")
          .arg(QString::fromStdString(plugin_id),
               QString::fromStdString(status.installed_version.empty()
                                          ? std::string("the installed version")
                                          : status.installed_version),
               QString::fromStdString(status.latest_version)),
      QMessageBox::Yes | QMessageBox::No);
  if (answer != QMessageBox::Yes) {
    return;
  }

  const auto result = model_->updateToLatest(selector);
  if (!result.success) {
    QMessageBox::warning(this, "Update Plugin Failed",
                         QString::fromStdString(result.error_message));
    return;
  }

  plugin_changes_made_ = true;

  // The rewritten entry is untrusted; confirm the new binary right away.
  // Declining leaves it untrusted, which shows as an unticked Enabled box the
  // user can tick later to confirm.
  grantTrustWithConfirmation(selector);
  refresh();

  // Re-evaluate statuses against the rewritten registry entry.
  update_check_completed_ = false;
  startUpdateCheck();
}

}  // namespace orc
