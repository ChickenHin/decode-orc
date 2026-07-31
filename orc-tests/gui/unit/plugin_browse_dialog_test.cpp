/*
 * File:        plugin_browse_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen smoke test for the curated plugin browse dialog
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
#include <QLabel>
#include <QListWidget>

#include "mocks/mock_project_presenter.h"
#include "pluginbrowsedialog.h"
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

orc::presenters::PluginIndexInfo makeIndex(bool offline) {
  orc::presenters::PluginIndexInfo info;
  info.available = true;
  info.offline = offline;
  info.from_cache = offline;
  orc::presenters::PluginIndexEntryInfo entry;
  entry.id = "acme.deint";
  entry.display_name = "ACME Deinterlacer";
  entry.description = "Motion-adaptive deinterlacing";
  entry.version = "1.0.5";
  entry.license_spdx = "GPL-3.0-or-later";
  entry.source_repo_url = "https://example.invalid/acme";
  entry.tags = {"video"};
  entry.has_compatible_build = true;
  info.entries = {entry};
  return info;
}

// Pump the event loop briefly so the asynchronous refresh completes.
void drainEvents(int milliseconds) {
  QDeadlineTimer deadline(milliseconds);
  while (!deadline.hasExpired()) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
}

// The details pane's text, wherever it ends up in the layout.
QString detailsText(QWidget* dialog) {
  QString combined;
  for (auto* label : dialog->findChildren<QLabel*>()) {
    combined += label->text() + "\n";
  }
  return combined;
}

}  // namespace

TEST(PluginBrowseDialogTest, ConstructsAndRefreshesOffscreen) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, fetchPluginIndex())
      .WillByDefault(Return(makeIndex(/*offline=*/false)));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);
  EXPECT_FALSE(dialog->changesMade());
  dialog.reset();  // Destructor must join the refresh thread cleanly.
}

TEST(PluginBrowseDialogTest, OfflineIndexDoesNotCrash) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, fetchPluginIndex())
      .WillByDefault(Return(makeIndex(/*offline=*/true)));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);
  dialog.reset();
}

TEST(PluginBrowseDialogTest, CompatibleEntryShowsManifestDeclared) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  ON_CALL(mock, fetchPluginIndex())
      .WillByDefault(Return(makeIndex(/*offline=*/false)));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);

  auto* list = dialog->findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);
  EXPECT_FALSE(list->item(0)->text().contains("("));

  bool declared_shown = false;
  for (auto* label : dialog->findChildren<QLabel*>()) {
    if (label->text().contains("declared by the release manifest")) {
      declared_shown = true;
    }
  }
  EXPECT_TRUE(declared_shown);
  dialog.reset();
}

TEST(PluginBrowseDialogTest, MissingManifestEntryShowsIncompatible) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  auto index = makeIndex(/*offline=*/false);
  index.entries[0].has_compatible_build = false;
  index.entries[0].compatibility_message =
      "The release does not include orc-plugin-manifest.yaml; a release "
      "manifest is required for a plugin to be installable";
  ON_CALL(mock, fetchPluginIndex()).WillByDefault(Return(index));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);

  auto* list = dialog->findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);
  EXPECT_TRUE(list->item(0)->text().contains("(incompatible)"));

  bool message_shown = false;
  for (auto* label : dialog->findChildren<QLabel*>()) {
    if (label->text().contains("release manifest is required")) {
      message_shown = true;
    }
  }
  EXPECT_TRUE(message_shown);
  dialog.reset();
}

TEST(PluginBrowseDialogTest, AbiMismatchEntryShowsIncompatible) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  auto index = makeIndex(/*offline=*/false);
  index.host_abi_version = 12;
  index.entries[0].has_compatible_build = false;
  index.entries[0].compatibility_message =
      "Latest release is built for Orc ABI 11; this host requires ABI 12";
  ON_CALL(mock, fetchPluginIndex()).WillByDefault(Return(index));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);

  auto* list = dialog->findChild<QListWidget*>();
  ASSERT_NE(list, nullptr);
  ASSERT_EQ(list->count(), 1);
  EXPECT_TRUE(list->item(0)->text().contains("(incompatible)"));

  bool message_shown = false;
  for (auto* label : dialog->findChildren<QLabel*>()) {
    if (label->text().contains("built for Orc ABI 11")) {
      message_shown = true;
    }
  }
  EXPECT_TRUE(message_shown);
  dialog.reset();
}

// The pane's field labels are the shared ones, so the dialog and
// 'orc-cli plugins info' name the same things.
TEST(PluginBrowseDialogTest, DetailsLabelsComeFromTheSharedStrings) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  const auto index = makeIndex(/*offline=*/false);
  ON_CALL(mock, fetchPluginIndex()).WillByDefault(Return(index));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);

  const QString text = detailsText(dialog.get());
  for (const auto& field : orc::presenters::makePluginDetails(
           nullptr, &index.entries[0], nullptr)) {
    EXPECT_TRUE(text.contains(QString::fromStdString(field.label) + ":"))
        << "missing label " << field.label;
    EXPECT_TRUE(text.contains(QString::fromStdString(field.value)))
        << "missing value for " << field.label;
  }
  dialog.reset();
}

// An installed copy behind the latest release reads the same here as it does
// from the CLI: which version is installed, and what is available.
TEST(PluginBrowseDialogTest, InstalledOutdatedEntryShowsTheVersionRelation) {
  ensureApplication();
  NiceMock<orc::presenters::test::MockProjectPresenter> mock;
  auto index = makeIndex(/*offline=*/false);
  index.entries[0].version = "1.0.6";
  index.entries[0].already_installed = true;
  ON_CALL(mock, fetchPluginIndex()).WillByDefault(Return(index));

  orc::presenters::PluginRegistryInfo registry;
  orc::presenters::PluginRegistryEntryInfo installed;
  installed.selector = "acme.deint";
  installed.plugin_id = "acme.deint";
  installed.plugin_version = "1.0.5";
  registry.entries = {installed};
  ON_CALL(mock, getPluginRegistry()).WillByDefault(Return(registry));

  auto dialog = std::make_unique<orc::PluginBrowseDialog>(mock);
  drainEvents(500);

  const QString text = detailsText(dialog.get());
  EXPECT_TRUE(text.contains(QString::fromStdString(
      orc::plugin_ux::installedValue(true, "1.0.5", "1.0.6"))));
  EXPECT_TRUE(text.contains("1.0.6"));
  dialog.reset();
}

}  // namespace gui_unit_test
