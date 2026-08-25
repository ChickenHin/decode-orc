/*
 * File:        logging_controller.h
 * Module:      orc-gui
 * Purpose:     Runtime application and persistence of diagnostic logging
 * settings
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_LOGGING_CONTROLLER_H
#define ORC_GUI_LOGGING_CONTROLLER_H

#include <QObject>
#include <QString>
#include <functional>
#include <string>

#include "logging_settings.h"

namespace orc {

/**
 * @brief Owns the process-wide diagnostic logging state.
 *
 * LoggingSettingsModel decides what a set of settings means; LoggingController
 * drives the impure side: it reconfigures the GUI and core loggers at runtime,
 * creates the log file's directory, remembers the user's choice between runs,
 * and tells the rest of the GUI when the configuration changed. One instance is
 * expected to exist for the lifetime of the GUI; instance() exposes it so the
 * Tools menu can open the dialogue.
 *
 * Reconfiguring preserves both logger objects, so stage plugins - which log
 * through the shared core logger - start writing to a newly enabled log file
 * without being reloaded.
 *
 * Thread-safety: must only be used on the GUI (main) thread. The loggers it
 * reconfigures remain safe to use from any thread throughout.
 */
class LoggingController : public QObject {
  Q_OBJECT

 public:
  /// Outcome of applying a configuration.
  struct ApplyResult {
    /// False when the log file could not be opened; logging continues to the
    /// console and `error` explains why.
    bool ok = true;
    /// Reason the log file could not be opened; empty when ok.
    QString error;
    /// The log file actually in use; empty when file logging is off.
    QString log_file;
  };

  /// Applies one configuration to a logger. Mirrors reconfigure_gui_logging():
  /// returns false and fills `error_message` when a requested file sink could
  /// not be opened.
  using LogApplyFunction =
      std::function<bool(const std::string& level, const std::string& log_file,
                         LogDestination destination, bool truncate_log_file,
                         std::string* error_message)>;

  /// Constructs the controller from the configuration already installed for
  /// this run (by the command line or by the persisted settings). Nothing is
  /// reconfigured or persisted here; `initial` only describes what is live.
  explicit LoggingController(const LoggingSettings& initial,
                             QObject* parent = nullptr);

  /// Test constructor: routes reconfiguration through `apply_function` instead
  /// of the real loggers, and skips creating the log file's directory.
  LoggingController(const LoggingSettings& initial,
                    LogApplyFunction apply_function, QObject* parent = nullptr);

  ~LoggingController() override;

  /// Returns the active controller, or nullptr if none has been constructed.
  static LoggingController* instance();

  /// The configuration currently installed.
  const LoggingSettings& settings() const;

  /// The log file being written, or an empty string when file logging is off.
  QString activeLogFile() const;

  /// Installs `settings` on the GUI and core loggers, persists them, and emits
  /// settingsChanged(). A log file that cannot be opened is reported in the
  /// result; logging continues to the console and the settings are still
  /// recorded as requested so the user can correct the path.
  ///
  /// Opening a log file replaces its contents, so a capture never mixes runs
  /// (or two separate captures) in one file. Re-applying while the same file
  /// stays open - changing only the detail level, say - appends instead, so
  /// what has been captured so far is not thrown away.
  ApplyResult apply(const LoggingSettings& settings);

  /// Settings saved by the last session, or the defaults on first run.
  static LoggingSettings loadPersistedSettings();

  /// Records `settings` for the next session.
  static void persistSettings(const LoggingSettings& settings);

  /// Where a log file goes when the user has not chosen a path: a
  /// `decode-orc-logs` folder beside the crash bundles, in the user's
  /// documents directory.
  static QString defaultLogFilePath();

  /// Pattern used for every record. Matches the command-line front ends so a
  /// log captured from the GUI reads the same as one captured from a terminal.
  static QString logPattern();

 signals:
  void settingsChanged(const LoggingSettings& settings);

 private:
  LoggingController(const LoggingSettings& initial,
                    LogApplyFunction apply_function, bool manage_log_directory,
                    QObject* parent);

  LoggingSettings settings_;
  LogApplyFunction apply_function_;
  /// True for the test constructor: no directory is created for the log file.
  bool manage_log_directory_ = true;

  static LoggingController* s_instance;
};

}  // namespace orc

#endif  // ORC_GUI_LOGGING_CONTROLLER_H
