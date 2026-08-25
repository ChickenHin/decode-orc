/*
 * File:        logging.h
 * Module:      orc-gui
 * Purpose:     GUI logging convenience header
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// Destination vocabulary only; the ORC_LOG_* macros below deliberately shadow
// the SDK's own (see orc/support/logging.h), so that header is not pulled in.
#include <orc/support/log_destination.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace orc {

/// Get the GUI-specific logger
std::shared_ptr<spdlog::logger> get_gui_logger();

/// Reset the GUI logger (it will be recreated on next use)
void reset_gui_logger();

/// Initialize GUI logging independently of core
/// @param destination Which sinks to install (console, file, or both)
void init_gui_logging(
    const std::string& level = "info",
    const std::string& pattern = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
    const std::string& log_file = "",
    LogDestination destination = LogDestination::kBoth);

/// Reconfigure the GUI logger's sinks and level while the application runs.
///
/// The logger object is preserved and only its sinks are swapped, so the
/// change is picked up by everything already logging through it. Sinks are
/// swapped under the mutex that serialises writes, making this safe to call
/// while worker threads log.
///
/// @param level Log level (trace, debug, info, warn, error, critical, off)
/// @param pattern Log pattern applied to every installed sink
/// @param log_file File to write to; ignored when empty
/// @param destination Which sinks to install (console, file, or both)
/// @param truncate_log_file True to replace the log file's contents, false to
///        append to a file this run already opened. init_gui_logging() always
///        replaces, so each run starts a fresh log
/// @param error_message Optional; receives the reason a requested log file
///        could not be opened (the console sink is kept in that case)
/// @return True when the requested destination was installed in full
bool reconfigure_gui_logging(const std::string& level,
                             const std::string& pattern,
                             const std::string& log_file,
                             LogDestination destination, bool truncate_log_file,
                             std::string* error_message = nullptr);

}  // namespace orc

// GUI-specific logging macros that use the GUI logger
#define ORC_LOG_TRACE(...) \
  SPDLOG_LOGGER_TRACE(orc::get_gui_logger(), __VA_ARGS__)
#define ORC_LOG_DEBUG(...) \
  SPDLOG_LOGGER_DEBUG(orc::get_gui_logger(), __VA_ARGS__)
#define ORC_LOG_INFO(...) SPDLOG_LOGGER_INFO(orc::get_gui_logger(), __VA_ARGS__)
#define ORC_LOG_WARN(...) SPDLOG_LOGGER_WARN(orc::get_gui_logger(), __VA_ARGS__)
#define ORC_LOG_ERROR(...) \
  SPDLOG_LOGGER_ERROR(orc::get_gui_logger(), __VA_ARGS__)
#define ORC_LOG_CRITICAL(...) \
  SPDLOG_LOGGER_CRITICAL(orc::get_gui_logger(), __VA_ARGS__)
