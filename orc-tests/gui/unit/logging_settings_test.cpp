/*
 * File:        logging_settings_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 coverage of the diagnostic logging configuration rules
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_settings.h"

#include <gtest/gtest.h>

namespace {

using orc::LoggingSettings;
using orc::LoggingSettingsModel;

LoggingSettings enabled(const QString& level, const QString& path = QString{}) {
  LoggingSettings settings;
  settings.file_logging_enabled = true;
  settings.level = level;
  settings.file_path = path;
  return settings;
}

// =============================================================================
// Level names
// =============================================================================

TEST(LoggingSettingsModelTest, LevelNames_AreOrderedBySeverity) {
  const QStringList names = LoggingSettingsModel::levelNames();
  ASSERT_EQ(names.size(), 7);
  EXPECT_EQ(names.front().toStdString(), "trace");
  EXPECT_EQ(names.at(2).toStdString(), "info");
  EXPECT_EQ(names.back().toStdString(), "off");
}

TEST(LoggingSettingsModelTest, NormaliseLevel_AcceptsCanonicalNames) {
  for (const QString& name : LoggingSettingsModel::levelNames()) {
    EXPECT_EQ(LoggingSettingsModel::normaliseLevel(name).toStdString(),
              name.toStdString());
  }
}

TEST(LoggingSettingsModelTest, NormaliseLevel_IsCaseAndPaddingInsensitive) {
  EXPECT_EQ(LoggingSettingsModel::normaliseLevel("  DEBUG ").toStdString(),
            "debug");
}

// The logger accepts "warning" as a spelling of "warn"; the dialogue offers
// only "warn", so a settings file carrying the long spelling must map onto it
// rather than falling back to info.
TEST(LoggingSettingsModelTest, NormaliseLevel_MapsWarningOntoWarn) {
  EXPECT_EQ(LoggingSettingsModel::normaliseLevel("warning").toStdString(),
            "warn");
  EXPECT_TRUE(LoggingSettingsModel::isValidLevel("warning"));
}

TEST(LoggingSettingsModelTest, NormaliseLevel_UnknownNameFallsBackToInfo) {
  EXPECT_EQ(LoggingSettingsModel::normaliseLevel("verbose").toStdString(),
            "info");
  EXPECT_EQ(LoggingSettingsModel::normaliseLevel("").toStdString(), "info");
  EXPECT_FALSE(LoggingSettingsModel::isValidLevel("verbose"));
}

// =============================================================================
// Destination
// =============================================================================

// The console is never dropped: a session launched from a terminal keeps its
// output whether or not a log file is also being written.
TEST(LoggingSettingsModelTest, DestinationFor_EnabledKeepsConsoleAndAddsFile) {
  EXPECT_EQ(LoggingSettingsModel::destinationFor(enabled("debug")),
            orc::LogDestination::kBoth);
}

TEST(LoggingSettingsModelTest, DestinationFor_DisabledIsConsoleOnly) {
  EXPECT_EQ(LoggingSettingsModel::destinationFor(LoggingSettings{}),
            orc::LogDestination::kConsole);
}

// =============================================================================
// Log file resolution
// =============================================================================

TEST(LoggingSettingsModelTest, ResolveLogFile_DisabledYieldsNoFile) {
  LoggingSettings settings;
  settings.file_path = "/tmp/chosen.log";
  EXPECT_TRUE(
      LoggingSettingsModel::resolveLogFile(settings, "/default/orc-gui.log")
          .isEmpty());
}

TEST(LoggingSettingsModelTest, ResolveLogFile_UsesChosenPath) {
  EXPECT_EQ(LoggingSettingsModel::resolveLogFile(
                enabled("debug", "/logs/session.log"), "/default/orc-gui.log")
                .toStdString(),
            "/logs/session.log");
}

TEST(LoggingSettingsModelTest, ResolveLogFile_BlankPathFallsBackToDefault) {
  EXPECT_EQ(LoggingSettingsModel::resolveLogFile(enabled("debug", "   "),
                                                 "/default/orc-gui.log")
                .toStdString(),
            "/default/orc-gui.log");
}

// =============================================================================
// Summary text
// =============================================================================

TEST(LoggingSettingsModelTest, SummaryText_DisabledExplainsConsoleOnly) {
  const QString text =
      LoggingSettingsModel::summaryText(LoggingSettings{}, "/default.log");
  EXPECT_TRUE(text.contains("off"));
  EXPECT_FALSE(text.contains("/default.log"));
}

TEST(LoggingSettingsModelTest, SummaryText_EnabledNamesLevelAndResolvedPath) {
  const QString text =
      LoggingSettingsModel::summaryText(enabled("debug"), "/default.log");
  EXPECT_TRUE(text.contains("debug"));
  EXPECT_TRUE(text.contains("/default.log"));
}

// A level whose records this build compiled out cannot produce anything, so
// the summary has to say so rather than promising output that never arrives.
TEST(LoggingSettingsModelTest, SummaryText_WarnsWhenLevelIsCompiledOut) {
  const QString text =
      LoggingSettingsModel::summaryText(enabled("trace"), "/default.log");
  if (LoggingSettingsModel::isLevelCompiledIn("trace")) {
    EXPECT_FALSE(text.contains("compiled without"));
  } else {
    EXPECT_TRUE(text.contains("compiled without"));
  }
}

// Levels at or above info are never compiled out, whatever the build type.
TEST(LoggingSettingsModelTest, IsLevelCompiledIn_InfoAndAboveAlwaysAvailable) {
  EXPECT_TRUE(LoggingSettingsModel::isLevelCompiledIn("info"));
  EXPECT_TRUE(LoggingSettingsModel::isLevelCompiledIn("warn"));
  EXPECT_TRUE(LoggingSettingsModel::isLevelCompiledIn("error"));
  EXPECT_TRUE(LoggingSettingsModel::isLevelCompiledIn("critical"));
}

// =============================================================================
// Level descriptions
// =============================================================================

TEST(LoggingSettingsModelTest, LevelDescription_IsProvidedForEveryLevel) {
  for (const QString& name : LoggingSettingsModel::levelNames()) {
    EXPECT_FALSE(LoggingSettingsModel::levelDescription(name).isEmpty())
        << name.toStdString();
  }
}

}  // namespace
