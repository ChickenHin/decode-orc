/*
 * File:        log_destination_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for --log-out destination parsing and sink selection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/support/log_destination.h>

namespace orc {
namespace tests {
namespace {

TEST(LogDestination, ParsesEveryAcceptedName) {
  EXPECT_EQ(parse_log_destination("console"), LogDestination::kConsole);
  EXPECT_EQ(parse_log_destination("file"), LogDestination::kFile);
  EXPECT_EQ(parse_log_destination("both"), LogDestination::kBoth);
}

TEST(LogDestination, ReturnsNulloptForUnrecognisedName) {
  EXPECT_FALSE(parse_log_destination("").has_value());
  EXPECT_FALSE(parse_log_destination("stdout").has_value());
  EXPECT_FALSE(parse_log_destination("Console").has_value());
  EXPECT_FALSE(parse_log_destination("console,file").has_value());
  EXPECT_FALSE(parse_log_destination(" file").has_value());
}

TEST(LogDestination, ConsoleDestinationIgnoresConfiguredLogFile) {
  auto without_file = resolve_log_sinks(LogDestination::kConsole, false);
  EXPECT_TRUE(without_file.console);
  EXPECT_FALSE(without_file.file);

  // A configured log file is ignored for a console-only destination.
  auto with_file = resolve_log_sinks(LogDestination::kConsole, true);
  EXPECT_TRUE(with_file.console);
  EXPECT_FALSE(with_file.file);
}

TEST(LogDestination, FileDestinationDropsConsoleWhenLogFileConfigured) {
  auto selection = resolve_log_sinks(LogDestination::kFile, true);
  EXPECT_FALSE(selection.console);
  EXPECT_TRUE(selection.file);
}

TEST(LogDestination, FileDestinationFallsBackToConsoleWithoutLogFile) {
  auto selection = resolve_log_sinks(LogDestination::kFile, false);
  EXPECT_TRUE(selection.console);
  EXPECT_FALSE(selection.file);
}

TEST(LogDestination, BothDestinationInstallsConsoleAndFile) {
  auto selection = resolve_log_sinks(LogDestination::kBoth, true);
  EXPECT_TRUE(selection.console);
  EXPECT_TRUE(selection.file);
}

TEST(LogDestination, BothDestinationIsConsoleOnlyWithoutLogFile) {
  auto selection = resolve_log_sinks(LogDestination::kBoth, false);
  EXPECT_TRUE(selection.console);
  EXPECT_FALSE(selection.file);
}

}  // namespace
}  // namespace tests
}  // namespace orc
