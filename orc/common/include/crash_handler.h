/*
 * File:        crash_handler.h
 * Module:      orc-common
 * Purpose:     Minimal crash handler interface for CLI/GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <functional>
#include <string>

namespace orc {

struct CrashHandlerConfig {
  std::string application_name;
  std::string version;
  std::string output_directory;
  std::string primary_log_file;
  bool enable_coredump{false};
  bool auto_upload_info{false};
  std::function<std::string()> custom_info_callback;
};

bool init_crash_handler(const CrashHandlerConfig& config);
std::string create_crash_bundle(const std::string& description);
void cleanup_crash_handler();

/// Update the log file that future crash bundles collect.
///
/// Call this when the application's log destination changes while it is
/// running (see the GUI's Tools > Logging dialogue) so a bundle carries the log
/// that was actually being written rather than the one chosen at startup.
/// Pass an empty string when logging to a file has been turned off.
void set_crash_log_file(const std::string& log_file);

}  // namespace orc
