/*
 * File:        log_destination.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Log output destination selection shared by host and plugins
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <optional>
#include <string>

namespace orc {

/// Where log records are written. Selected by the `--log-out` command-line
/// switch; kBoth reproduces the historical behaviour (console always on, file
/// added when a log file is configured).
enum class LogDestination {
  kConsole,  ///< Console only; any configured log file is ignored
  kFile,     ///< Log file only; nothing is written to the console
  kBoth,     ///< Console, plus the log file when one is configured
};

/// Which sinks an initialiser should install for a destination.
struct LogSinkSelection {
  bool console = true;
  bool file = false;
};

/// Parse a `--log-out` value. Comparison is case-sensitive to keep the accepted
/// spelling identical across the GUI and CLI front ends.
/// @param value One of "console", "file", "both"
/// @return The parsed destination, or std::nullopt if the value is not one of
///         the three accepted names
inline std::optional<LogDestination> parse_log_destination(
    const std::string& value) {
  if (value == "console") {
    return LogDestination::kConsole;
  }
  if (value == "file") {
    return LogDestination::kFile;
  }
  if (value == "both") {
    return LogDestination::kBoth;
  }
  return std::nullopt;
}

/// Resolve a destination into the concrete set of sinks to install.
///
/// A file sink is only possible when a log file has been configured, so
/// kFile without one would leave the logger with no sinks at all and silently
/// swallow every message. Console output is kept in that case so the operator
/// still sees the log (front ends warn separately about the missing file).
///
/// @param destination Requested destination
/// @param have_log_file True when a non-empty log file path was supplied
inline LogSinkSelection resolve_log_sinks(LogDestination destination,
                                          bool have_log_file) {
  LogSinkSelection selection;
  selection.file = have_log_file && destination != LogDestination::kConsole;
  selection.console = destination != LogDestination::kFile || !selection.file;
  return selection;
}

}  // namespace orc
