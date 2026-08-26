/*
 * File:        plugin_manager_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests for the Plugin Manager's details and diagnostics
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <plugin_ux_strings.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>

#include "mocks/mock_project_presenter.h"
#include "plugin_row_presentation.h"
#include "pluginmanagerdialog.h"
#include "presenters/include/plugin_details.h"

namespace gui_unit_test {

using ::testing::NiceMock;
using ::testing::Return;

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

// Pump the event loop so the dialog's background update check completes; the
// dialog refreshes its table when it does.
void drainEvents(int milliseconds) {
  QDeadlineTimer deadline(milliseconds);
  while (!deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

orc::presenters::PluginRegistryInfo makeRegistry() {
  orc::presenters::PluginRegistryInfo registry;
  registry.registry_path = "";  // No file: the dialog must not touch disk.

  orc::presenters::PluginRegistryEntryInfo entry;
  entry.selector = "com.example.plugin";
  entry.plugin_id = "com.example.plugin";
  entry.plugin_version = "1.0.5";
  entry.path = "/plugins/example.so";
  entry.license_spdx = "MIT";
  entry.source_label = "https://example.invalid/example";
  entry.path_exists = true;
  entry.enabled = true;
  entry.trust_state = orc::presenters::kPluginTrustStateTrusted;
  entry.required_host_abi = 12;
  entry.host_abi_version = 12;
  entry.load_state = orc::presenters::PluginLoadState::WillLoad;
  entry.load_state_detail = orc::plugin_ux::kLoadStateWillLoadDetail;
  registry.entries = {entry};
  return registry;
}

QLabel* findLabel(QWidget* root, const QString& object_name) {
  return root->findChild<QLabel*>(object_name);
}

// Column index found by header text, so the test does not copy the dialog's
// private column constants.
int columnTitled(QTableWidget* table, const QString& title) {
  for (int column = 0; column < table->columnCount(); ++column) {
    if (auto* header = table->horizontalHeaderItem(column)) {
      if (header->text() == title) {
        return column;
      }
    }
  }
  return -1;
}

// One registry entry per load state the dialog can show for a registered row.
orc::presenters::PluginRegistryInfo makeRegistryWithEveryLoadState() {
  orc::presenters::PluginRegistryInfo registry;

  const auto make = [](const std::string& id,
                       orc::presenters::PluginLoadState state) {
    orc::presenters::PluginRegistryEntryInfo entry;
    entry.selector = id;
    entry.plugin_id = id;
    entry.plugin_version = "1.0.0";
    entry.path = "/plugins/" + id + ".so";
    entry.path_exists = true;
    entry.load_state = state;
    return entry;
  };

  registry.entries = {
      make("com.example.willload", orc::presenters::PluginLoadState::WillLoad),
      make("com.example.disabled", orc::presenters::PluginLoadState::Disabled),
      make("com.example.untrusted",
           orc::presenters::PluginLoadState::NotTrusted),
      make("com.example.abi", orc::presenters::PluginLoadState::AbiMismatch),
      make("com.example.missing",
           orc::presenters::PluginLoadState::FileMissing),
  };
  return registry;
}

}  // namespace

TEST(PluginManagerDialogTest, ConstructsAndRefreshesOffscreen) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(makeRegistry()));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->rowCount(), 1);
  dialog.reset();  // Destructor must join the update-check thread cleanly.
}

// Every field 'orc-cli plugins info' prints for an entry appears in the pane,
// with the same label text: both come from the one presenter-built list.
TEST(PluginManagerDialogTest, DetailsPaneShowsThePresenterFieldsForTheRow) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  const auto registry = makeRegistry();
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(registry));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->rowCount(), 1);
  table->selectRow(0);

  auto* details = findLabel(dialog.get(), "pluginDetailsLabel");
  ASSERT_NE(details, nullptr);
  const QString text = details->text();

  for (const auto& field : orc::presenters::makePluginDetails(
           &registry.entries[0], nullptr, nullptr)) {
    EXPECT_TRUE(text.contains(QString::fromStdString(field.label) + ":"))
        << "missing label " << field.label;
    EXPECT_TRUE(text.contains(QString::fromStdString(field.value)))
        << "missing value for " << field.label;
  }
  EXPECT_TRUE(text.contains("com.example.plugin"));
  EXPECT_TRUE(text.contains(orc::plugin_ux::kLoadStateWillLoadLabel));
  dialog.reset();
}

TEST(PluginManagerDialogTest, DetailsPanePromptsWhenNothingIsSelected) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(makeRegistry()));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* details = findLabel(dialog.get(), "pluginDetailsLabel");
  ASSERT_NE(details, nullptr);
  EXPECT_EQ(details->text(),
            QString::fromUtf8(orc::plugin_ux::kDetailsNoSelection));
  dialog.reset();
}

// The Diagnostics section shows what the runtime reported, in the same lines
// 'orc-cli plugins doctor' prints.
TEST(PluginManagerDialogTest, DiagnosticsSectionListsEverySeverity) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(makeRegistry()));

  std::vector<orc::presenters::PluginDiagnosticInfo> diagnostics;
  orc::presenters::PluginDiagnosticInfo info;
  info.severity = orc::presenters::PluginDiagnosticSeverity::Info;
  info.message = "Loaded plugin 'com.example.plugin'";
  diagnostics.push_back(info);
  orc::presenters::PluginDiagnosticInfo warning;
  warning.severity = orc::presenters::PluginDiagnosticSeverity::Warning;
  warning.message = "Plugin skipped";
  warning.path = "/plugins/skipped.so";
  diagnostics.push_back(warning);
  orc::presenters::PluginDiagnosticInfo error;
  error.severity = orc::presenters::PluginDiagnosticSeverity::Error;
  error.message = "Failed to load";
  error.path = "/plugins/broken.so";
  diagnostics.push_back(error);
  ON_CALL(mock, listPluginDiagnostics()).WillByDefault(Return(diagnostics));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* group = dialog->findChild<QGroupBox*>("pluginDiagnosticsGroup");
  ASSERT_NE(group, nullptr);
  EXPECT_TRUE(group->isCheckable());
  EXPECT_FALSE(group->isChecked());  // Collapsed until asked for.
  EXPECT_TRUE(group->title().contains("3"));

  auto* label = findLabel(dialog.get(), "pluginDiagnosticsLabel");
  ASSERT_NE(label, nullptr);
  for (const auto& diagnostic : diagnostics) {
    EXPECT_TRUE(label->text().contains(QString::fromStdString(
        orc::presenters::formatPluginDiagnostic(diagnostic))));
  }
  dialog.reset();
}

TEST(PluginManagerDialogTest, DiagnosticsSectionSaysSoWhenThereAreNone) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(makeRegistry()));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* label = findLabel(dialog.get(), "pluginDiagnosticsLabel");
  ASSERT_NE(label, nullptr);
  EXPECT_EQ(label->text(), QString::fromUtf8(orc::plugin_ux::kDiagnosticsNone));
  dialog.reset();
}

// The Enabled tick is the dialog's rendering of the presenter-computed load
// state: for every state, the check and its interactivity follow
// makePluginRowPresentation, never a re-derivation inside the dialog.
TEST(PluginManagerDialogTest, EnabledTickFollowsThePresenterLoadState) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  const auto registry = makeRegistryWithEveryLoadState();
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(registry));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->rowCount(), static_cast<int>(registry.entries.size()));
  const int enabled_column = columnTitled(table, "Enabled");
  ASSERT_GE(enabled_column, 0);

  for (int row = 0; row < table->rowCount(); ++row) {
    const auto& entry = registry.entries[static_cast<size_t>(row)];
    const auto expected = orc::makePluginRowPresentation(
        entry.load_state, entry.load_state_detail);
    auto* tick = table->item(row, enabled_column);
    ASSERT_NE(tick, nullptr) << "row " << row;
    EXPECT_EQ(tick->checkState() == Qt::Checked, expected.enabled_checked)
        << "tick state diverges from load_state for " << entry.plugin_id;
    EXPECT_EQ(tick->flags().testFlag(Qt::ItemIsEnabled),
              expected.enabled_interactive)
        << "tick interactivity diverges from load_state for "
        << entry.plugin_id;
  }
  dialog.reset();
}

// Remove sends the selected row's selector — the same string the row carries
// in its item data — through the presenter seam, for an id-less entry too.
TEST(PluginManagerDialogTest, RemovePassesTheSelectedRowsSelectorThrough) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;

  orc::presenters::PluginRegistryInfo registry;
  orc::presenters::PluginRegistryEntryInfo named;
  named.selector = "com.example.named";
  named.plugin_id = "com.example.named";
  named.path = "/plugins/named.so";
  named.load_state = orc::presenters::PluginLoadState::WillLoad;
  orc::presenters::PluginRegistryEntryInfo unnamed;
  unnamed.selector = "path:/plugins/unnamed.so";
  unnamed.path = "/plugins/unnamed.so";
  unnamed.load_state = orc::presenters::PluginLoadState::NotTrusted;
  registry.entries = {named, unnamed};
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(registry));

  orc::presenters::PluginRegistryMutationResult removed;
  removed.success = true;
  EXPECT_CALL(mock, removePluginEntry("path:/plugins/unnamed.so"))
      .WillOnce(Return(removed));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->rowCount(), 2);
  table->selectRow(1);

  QPushButton* remove_button = nullptr;
  for (auto* button : dialog->findChildren<QPushButton*>()) {
    if (button->text() == "Remove") {
      remove_button = button;
      break;
    }
  }
  ASSERT_NE(remove_button, nullptr);
  ASSERT_TRUE(remove_button->isEnabled());

  // The click opens a modal confirmation; answer Yes from the event loop.
  auto* accepter = new QTimer(dialog.get());
  QObject::connect(accepter, &QTimer::timeout, [] {
    if (auto* box =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      if (auto* yes = box->button(QMessageBox::Yes)) {
        yes->click();
      }
    }
  });
  accepter->start(25);
  remove_button->click();
  accepter->stop();

  dialog.reset();
}

// Closing the dialog after a registry change offers a quit, not a restart: the
// application cannot relaunch itself, and the prompt must not promise it.
TEST(PluginManagerDialogTest, AcceptAfterAChangeOffersToQuitRatherThanRestart) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;

  orc::presenters::PluginRegistryInfo registry;
  orc::presenters::PluginRegistryEntryInfo entry;
  entry.selector = "com.example.plugin";
  entry.plugin_id = "com.example.plugin";
  entry.path = "/plugins/example.so";
  entry.path_exists = true;
  entry.load_state = orc::presenters::PluginLoadState::WillLoad;
  registry.entries = {entry};
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(registry));

  orc::presenters::PluginRegistryMutationResult removed;
  removed.success = true;
  ON_CALL(mock, removePluginEntry("com.example.plugin"))
      .WillByDefault(Return(removed));

  auto dialog = std::make_unique<orc::PluginManagerDialog>(mock);
  drainEvents(500);

  auto* table = dialog->findChild<QTableWidget*>();
  ASSERT_NE(table, nullptr);
  ASSERT_EQ(table->rowCount(), 1);
  table->selectRow(0);

  QPushButton* remove_button = nullptr;
  for (auto* button : dialog->findChildren<QPushButton*>()) {
    if (button->text() == "Remove") {
      remove_button = button;
      break;
    }
  }
  ASSERT_NE(remove_button, nullptr);

  // Confirm the removal so the dialog records that the registry changed.
  auto* confirmer = new QTimer(dialog.get());
  QObject::connect(confirmer, &QTimer::timeout, [] {
    if (auto* box =
            qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
      if (auto* yes = box->button(QMessageBox::Yes)) {
        yes->click();
      }
    }
  });
  confirmer->start(25);
  remove_button->click();
  confirmer->stop();

  // Answer the close prompt with Cancel, recording what it said on the way.
  QString prompt_text;
  QStringList button_labels;
  auto* answerer = new QTimer(dialog.get());
  QObject::connect(answerer, &QTimer::timeout, [&] {
    auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
    if (box == nullptr) {
      return;
    }
    prompt_text = box->text();
    QPushButton* cancel = nullptr;
    for (auto* button : box->buttons()) {
      button_labels << button->text();
      if (button->text() == "Cancel") {
        cancel = qobject_cast<QPushButton*>(button);
      }
    }
    ASSERT_NE(cancel, nullptr);
    cancel->click();
  });
  answerer->start(25);
  auto* button_box = dialog->findChild<QDialogButtonBox*>();
  ASSERT_NE(button_box, nullptr);
  auto* ok_button = button_box->button(QDialogButtonBox::Ok);
  ASSERT_NE(ok_button, nullptr);
  ok_button->click();
  answerer->stop();

  EXPECT_EQ(prompt_text, QString::fromUtf8(orc::plugin_ux::kQuitToApplyPrompt));
  EXPECT_TRUE(button_labels.contains("Quit"))
      << "offered buttons: " << button_labels.join(", ").toStdString();
  EXPECT_FALSE(button_labels.contains("Restart"))
      << "the application cannot restart itself";
  // Cancel keeps the dialog open rather than closing it.
  EXPECT_FALSE(dialog->isHidden() && dialog->result() == QDialog::Accepted);

  dialog.reset();
}

}  // namespace gui_unit_test
