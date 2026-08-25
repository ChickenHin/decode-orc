/*
 * File:        logging_settings_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 smoke and round-trip coverage of the logging dialogue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_settings_dialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFont>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPushButton>

namespace gui_unit_test {
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

using orc::LoggingSettings;
using orc::LoggingSettingsDialog;

constexpr auto kDefaultLogFile = "/default/decode-orc-logs/orc-gui.log";

// Applies an interface font for the duration of a test and restores the one
// the rest of the suite runs with.
class ScopedApplicationFont {
 public:
  explicit ScopedApplicationFont(int point_size)
      : previous_(QApplication::font()) {
    QFont font = previous_;
    font.setPointSize(point_size);
    QApplication::setFont(font);
  }
  ~ScopedApplicationFont() { QApplication::setFont(previous_); }

 private:
  QFont previous_;
};

LoggingSettings makeSettings(bool enabled, const QString& level,
                             const QString& path = QString{}) {
  LoggingSettings settings;
  settings.file_logging_enabled = enabled;
  settings.level = level;
  settings.file_path = path;
  return settings;
}

// =============================================================================
// Smoke
// =============================================================================

TEST(LoggingSettingsDialogTest, Construct_BuildsControls) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);

  EXPECT_EQ(dialog.windowTitle().toStdString(), "Logging");
  EXPECT_NE(dialog.findChild<QCheckBox*>("fileLoggingCheck"), nullptr);
  EXPECT_NE(dialog.findChild<QComboBox*>("levelCombo"), nullptr);
  EXPECT_NE(dialog.findChild<QLineEdit*>("filePathEdit"), nullptr);
  EXPECT_NE(dialog.findChild<QPushButton*>("browseButton"), nullptr);
}

TEST(LoggingSettingsDialogTest, LevelCombo_OffersEveryLevel) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  auto* combo = dialog.findChild<QComboBox*>("levelCombo");
  ASSERT_NE(combo, nullptr);

  EXPECT_EQ(combo->count(), orc::LoggingSettingsModel::levelNames().size());
}

// =============================================================================
// Round trip
// =============================================================================

TEST(LoggingSettingsDialogTest, Settings_RoundTripThroughTheControls) {
  (void)ensureApplication();

  const LoggingSettings initial =
      makeSettings(true, "debug", "/logs/session.log");
  LoggingSettingsDialog dialog(initial, kDefaultLogFile);

  EXPECT_EQ(dialog.settings(), initial);
}

TEST(LoggingSettingsDialogTest, SetSettings_ReplacesTheControlContents) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  const LoggingSettings replacement =
      makeSettings(true, "trace", "/logs/other.log");

  dialog.setSettings(replacement);

  EXPECT_EQ(dialog.settings(), replacement);
}

// The dialogue offers only canonical level names, so a stored long spelling
// still has to select a real entry rather than leaving the combo blank.
TEST(LoggingSettingsDialogTest, SetSettings_NormalisesTheLevel) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(true, "warning", "/logs/a.log"),
                               kDefaultLogFile);

  EXPECT_EQ(dialog.settings().level.toStdString(), "warn");
}

TEST(LoggingSettingsDialogTest, Settings_TrimsThePathBeforeReporting) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(true, "info"), kDefaultLogFile);
  auto* path_edit = dialog.findChild<QLineEdit*>("filePathEdit");
  ASSERT_NE(path_edit, nullptr);

  path_edit->setText("  /logs/padded.log  ");

  EXPECT_EQ(dialog.settings().file_path.toStdString(), "/logs/padded.log");
}

// =============================================================================
// Control state
// =============================================================================

// The path only means something while file logging is on, so its controls
// follow the checkbox.
TEST(LoggingSettingsDialogTest, PathControls_FollowTheEnableCheckbox) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  auto* check = dialog.findChild<QCheckBox*>("fileLoggingCheck");
  auto* path_edit = dialog.findChild<QLineEdit*>("filePathEdit");
  auto* browse = dialog.findChild<QPushButton*>("browseButton");
  ASSERT_NE(check, nullptr);
  ASSERT_NE(path_edit, nullptr);
  ASSERT_NE(browse, nullptr);

  EXPECT_FALSE(path_edit->isEnabled());
  EXPECT_FALSE(browse->isEnabled());

  check->setChecked(true);

  EXPECT_TRUE(path_edit->isEnabled());
  EXPECT_TRUE(browse->isEnabled());
}

TEST(LoggingSettingsDialogTest, Summary_TracksTheSelectedConfiguration) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  auto* summary = dialog.findChild<QLabel*>("summaryLabel");
  ASSERT_NE(summary, nullptr);
  EXPECT_TRUE(summary->text().contains("off"));

  dialog.setSettings(makeSettings(true, "debug", "/logs/session.log"));

  EXPECT_TRUE(summary->text().contains("debug"));
  EXPECT_TRUE(summary->text().contains("/logs/session.log"));
}

// An empty path is legal: the placeholder tells the user where the log will go
// and the summary has to name the same place.
TEST(LoggingSettingsDialogTest, Summary_NamesTheDefaultPathWhenNoneIsChosen) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(true, "info"), kDefaultLogFile);
  auto* summary = dialog.findChild<QLabel*>("summaryLabel");
  auto* path_edit = dialog.findChild<QLineEdit*>("filePathEdit");
  ASSERT_NE(summary, nullptr);
  ASSERT_NE(path_edit, nullptr);

  EXPECT_EQ(path_edit->placeholderText().toStdString(), kDefaultLogFile);
  EXPECT_TRUE(summary->text().contains(kDefaultLogFile));
}

TEST(LoggingSettingsDialogTest, LevelDescription_TracksTheSelectedLevel) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(true, "info"), kDefaultLogFile);
  auto* description = dialog.findChild<QLabel*>("levelDescription");
  auto* combo = dialog.findChild<QComboBox*>("levelCombo");
  ASSERT_NE(description, nullptr);
  ASSERT_NE(combo, nullptr);

  const QString before = description->text();
  combo->setCurrentText("critical");

  EXPECT_NE(description->text(), before);
  EXPECT_EQ(
      description->text().toStdString(),
      orc::LoggingSettingsModel::levelDescription("critical").toStdString());
}

// =============================================================================
// Sizing
// =============================================================================

// The dialogue is mostly wrapped explanatory text, and a word-wrapped label is
// only as tall as the width it is finally given. Unless the layout accounts for
// that, the window can be dragged smaller than the text it holds - or open that
// way - and the wrapped remainder is simply cut off. Shrinking to the smallest
// size the window will accept must still show every line.
TEST(LoggingSettingsDialogTest, Layout_ShowsEveryWrappedLineWhenShrunk) {
  (void)ensureApplication();

  for (int point_size : {8, 10, 13, 18, 24}) {
    ScopedApplicationFont font(point_size);

    LoggingSettingsDialog dialog(
        makeSettings(true, "trace"),
        "/home/user/Documents/decode-orc-logs/orc-gui.log");
    dialog.show();
    QCoreApplication::processEvents();

    // Ask for a size far below anything usable; the window may only shrink to
    // what its contents need.
    dialog.resize(1, 1);
    QCoreApplication::processEvents();

    for (QLabel* label : dialog.findChildren<QLabel*>()) {
      if (!label->wordWrap()) {
        continue;
      }
      EXPECT_GE(label->height(), label->heightForWidth(label->width()))
          << "clipped at " << point_size
          << "pt: " << label->text().toStdString();
    }

    dialog.close();
    QCoreApplication::processEvents();
  }
}

// The size the window refuses to go below has to be the size that shows
// everything, not a single line's worth of each explanation.
TEST(LoggingSettingsDialogTest, Layout_MinimumSizeCoversTheFullContents) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  dialog.show();
  QCoreApplication::processEvents();

  EXPECT_GE(dialog.minimumSizeHint().height(), dialog.sizeHint().height());
  EXPECT_GE(dialog.height(), dialog.sizeHint().height());
}

// The summary sentence changes length as the controls change; the dialogue has
// to grow to keep it fully visible once it is already on screen.
TEST(LoggingSettingsDialogTest, Layout_AbsorbsALongerSummaryWhileOpen) {
  (void)ensureApplication();

  LoggingSettingsDialog dialog(makeSettings(false, "info"), kDefaultLogFile);
  dialog.show();
  QCoreApplication::processEvents();

  dialog.setSettings(
      makeSettings(true, "trace",
                   "/home/user/Documents/decode-orc-logs/"
                   "a-rather-long-name-for-this-particular-capture.log"));
  QCoreApplication::processEvents();

  auto* summary = dialog.findChild<QLabel*>("summaryLabel");
  ASSERT_NE(summary, nullptr);
  EXPECT_GE(summary->height(), summary->heightForWidth(summary->width()));
  EXPECT_GE(dialog.minimumSizeHint().height(), dialog.sizeHint().height());
}

}  // namespace
}  // namespace gui_unit_test
