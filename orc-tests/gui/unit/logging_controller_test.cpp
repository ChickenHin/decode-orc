/*
 * File:        logging_controller_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 2 coverage of runtime logging application and persistence
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "logging_controller.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <memory>
#include <vector>

namespace {

using orc::LoggingController;
using orc::LoggingSettings;

// One recorded call to the logger-applying seam.
struct AppliedConfiguration {
  QString level;
  QString log_file;
  orc::LogDestination destination = orc::LogDestination::kConsole;
  bool truncate_log_file = false;
};

// Stands in for the real loggers so no sink is ever installed and no log file
// is opened. `failure` makes the next apply report an unopenable file.
class FakeLogApplier {
 public:
  LoggingController::LogApplyFunction function() {
    return [this](const std::string& level, const std::string& log_file,
                  orc::LogDestination destination, bool truncate_log_file,
                  std::string* error_message) {
      calls_.push_back({QString::fromStdString(level),
                        QString::fromStdString(log_file), destination,
                        truncate_log_file});
      if (!failure_.empty()) {
        if (error_message != nullptr) {
          *error_message = failure_;
        }
        return false;
      }
      return true;
    };
  }

  void failWith(const std::string& message) { failure_ = message; }
  const std::vector<AppliedConfiguration>& calls() const { return calls_; }

 private:
  std::vector<AppliedConfiguration> calls_;
  std::string failure_;
};

// Redirects QSettings writes to an isolated temporary location so persistence
// does not touch the developer's real configuration.
class LoggingControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName("decode-orc-tests");
    QCoreApplication::setApplicationName("logging-controller-test");
    // Start every test from the stored defaults rather than whatever the
    // previous test persisted.
    LoggingController::persistSettings(LoggingSettings{});
  }

  void TearDown() override { QStandardPaths::setTestModeEnabled(false); }

  std::unique_ptr<LoggingController> makeController(
      const LoggingSettings& initial) {
    return std::make_unique<LoggingController>(initial, applier_.function());
  }

  FakeLogApplier applier_;
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
// Construction
// =============================================================================

TEST_F(LoggingControllerTest, Construct_ReportsInitialSettings) {
  auto controller = makeController(makeSettings(true, "debug", "/logs/a.log"));

  EXPECT_TRUE(controller->settings().file_logging_enabled);
  EXPECT_EQ(controller->settings().level.toStdString(), "debug");
  EXPECT_EQ(controller->activeLogFile().toStdString(), "/logs/a.log");
  EXPECT_TRUE(applier_.calls().empty());
}

// The command line accepts level spellings the dialogue does not offer, so the
// initial value is canonicalised rather than shown back verbatim.
TEST_F(LoggingControllerTest, Construct_NormalisesInitialLevel) {
  auto controller = makeController(makeSettings(false, "WARNING"));
  EXPECT_EQ(controller->settings().level.toStdString(), "warn");
}

TEST_F(LoggingControllerTest, Construct_DisabledHasNoActiveLogFile) {
  auto controller = makeController(makeSettings(false, "info", "/logs/a.log"));
  EXPECT_TRUE(controller->activeLogFile().isEmpty());
}

TEST_F(LoggingControllerTest, Instance_TracksActiveController) {
  EXPECT_EQ(LoggingController::instance(), nullptr);
  {
    auto controller = makeController(LoggingSettings{});
    EXPECT_EQ(LoggingController::instance(), controller.get());
  }
  EXPECT_EQ(LoggingController::instance(), nullptr);
}

// =============================================================================
// Applying
// =============================================================================

TEST_F(LoggingControllerTest, Apply_EnablingInstallsFileSinkAtChosenLevel) {
  auto controller = makeController(LoggingSettings{});

  const auto result =
      controller->apply(makeSettings(true, "debug", "/logs/session.log"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_EQ(applier_.calls().front().level.toStdString(), "debug");
  EXPECT_EQ(applier_.calls().front().log_file.toStdString(),
            "/logs/session.log");
  EXPECT_EQ(applier_.calls().front().destination, orc::LogDestination::kBoth);
  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.log_file.toStdString(), "/logs/session.log");
}

// Turning file logging off must not leave the file sink installed; the console
// destination is what removes it.
TEST_F(LoggingControllerTest, Apply_DisablingDropsFileSink) {
  auto controller = makeController(makeSettings(true, "debug", "/logs/a.log"));

  const auto result = controller->apply(makeSettings(false, "info"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_EQ(applier_.calls().front().destination,
            orc::LogDestination::kConsole);
  EXPECT_TRUE(applier_.calls().front().log_file.isEmpty());
  EXPECT_TRUE(result.log_file.isEmpty());
  EXPECT_TRUE(controller->activeLogFile().isEmpty());
}

TEST_F(LoggingControllerTest, Apply_BlankPathUsesTheDefaultLocation) {
  auto controller = makeController(LoggingSettings{});

  const auto result = controller->apply(makeSettings(true, "info", "   "));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_EQ(applier_.calls().front().log_file.toStdString(),
            LoggingController::defaultLogFilePath().toStdString());
  EXPECT_EQ(result.log_file.toStdString(),
            LoggingController::defaultLogFilePath().toStdString());
}

TEST_F(LoggingControllerTest, Apply_NormalisesLevelBeforeInstalling) {
  auto controller = makeController(LoggingSettings{});

  controller->apply(makeSettings(true, "TRACE", "/logs/a.log"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_EQ(applier_.calls().front().level.toStdString(), "trace");
  EXPECT_EQ(controller->settings().level.toStdString(), "trace");
}

TEST_F(LoggingControllerTest, Apply_EmitsSettingsChanged) {
  auto controller = makeController(LoggingSettings{});
  QSignalSpy spy(controller.get(), &LoggingController::settingsChanged);

  controller->apply(makeSettings(true, "debug", "/logs/a.log"));

  EXPECT_EQ(spy.count(), 1);
}

// A file that cannot be opened is reported, but the request is still what the
// controller holds so the dialogue reopens on the path that needs correcting.
TEST_F(LoggingControllerTest, Apply_FailedFileIsReportedAndStillRecorded) {
  auto controller = makeController(LoggingSettings{});
  applier_.failWith("Permission denied");

  const auto result =
      controller->apply(makeSettings(true, "debug", "/nowhere/a.log"));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error.toStdString(), "Permission denied");
  EXPECT_EQ(result.log_file.toStdString(), "/nowhere/a.log");
  EXPECT_TRUE(controller->settings().file_logging_enabled);
  EXPECT_EQ(controller->settings().file_path.toStdString(), "/nowhere/a.log");
}

// =============================================================================
// Log file replacement
// =============================================================================

// A capture must never mix runs, so opening a file that is not already being
// written replaces whatever it held.
TEST_F(LoggingControllerTest, Apply_TurningLoggingOnReplacesTheFile) {
  auto controller = makeController(LoggingSettings{});

  controller->apply(makeSettings(true, "debug", "/logs/a.log"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_TRUE(applier_.calls().front().truncate_log_file);
}

// Changing the detail level part-way through a capture must not throw away
// what has already been recorded.
TEST_F(LoggingControllerTest, Apply_SameFileKeepsWhatIsAlreadyRecorded) {
  auto controller = makeController(makeSettings(true, "info", "/logs/a.log"));

  controller->apply(makeSettings(true, "debug", "/logs/a.log"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_FALSE(applier_.calls().front().truncate_log_file);
}

TEST_F(LoggingControllerTest, Apply_SwitchingFileReplacesTheNewOne) {
  auto controller = makeController(makeSettings(true, "info", "/logs/a.log"));

  controller->apply(makeSettings(true, "info", "/logs/b.log"));

  ASSERT_EQ(applier_.calls().size(), 1U);
  EXPECT_TRUE(applier_.calls().front().truncate_log_file);
}

// Turning logging off and on again starts a new capture rather than resuming
// the previous one.
TEST_F(LoggingControllerTest, Apply_ReenablingTheSamePathReplacesTheFile) {
  auto controller = makeController(makeSettings(true, "info", "/logs/a.log"));

  controller->apply(makeSettings(false, "info", "/logs/a.log"));
  controller->apply(makeSettings(true, "info", "/logs/a.log"));

  ASSERT_EQ(applier_.calls().size(), 2U);
  EXPECT_TRUE(applier_.calls().back().truncate_log_file);
}

// =============================================================================
// Persistence
// =============================================================================

TEST_F(LoggingControllerTest, Apply_PersistsForTheNextSession) {
  {
    auto controller = makeController(LoggingSettings{});
    controller->apply(makeSettings(true, "debug", "/logs/kept.log"));
  }

  const LoggingSettings restored = LoggingController::loadPersistedSettings();
  EXPECT_TRUE(restored.file_logging_enabled);
  EXPECT_EQ(restored.level.toStdString(), "debug");
  EXPECT_EQ(restored.file_path.toStdString(), "/logs/kept.log");
}

TEST_F(LoggingControllerTest, LoadPersistedSettings_DefaultsToFileLoggingOff) {
  const LoggingSettings defaults = LoggingController::loadPersistedSettings();
  EXPECT_FALSE(defaults.file_logging_enabled);
  EXPECT_EQ(defaults.level.toStdString(), "info");
  EXPECT_TRUE(defaults.file_path.isEmpty());
}

TEST_F(LoggingControllerTest, DefaultLogFilePath_IsAnAbsoluteLogFile) {
  const QString path = LoggingController::defaultLogFilePath();
  EXPECT_FALSE(path.isEmpty());
  EXPECT_TRUE(path.endsWith(".log"));
  EXPECT_TRUE(path.contains("decode-orc-logs"));
}

}  // namespace
