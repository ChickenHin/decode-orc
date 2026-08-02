/*
 * File:        logging.cpp
 * Module:      orc-sdk-support
 * Purpose:     Logging system implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/support/logging.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace orc {

static std::shared_ptr<spdlog::logger> g_logger;

void init_logging(const std::string& level, const std::string& pattern,
                  const std::string& log_file, LogDestination destination) {
  std::vector<spdlog::sink_ptr> sinks;

  const LogSinkSelection selection =
      resolve_log_sinks(destination, !log_file.empty());

  // Console sink with color
  if (selection.console) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sinks.push_back(console_sink);
  }

  // File sink
  if (selection.file) {
    auto file_sink =
        std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_file, true);
    sinks.push_back(file_sink);
  }

  // Drop any previously registered "core" logger before (re)creating it. The
  // name may already be registered either by an earlier init_logging() call or
  // by another module in the process: this translation unit is compiled into
  // the orc-sdk-support static library, so the host, each dlopen'd plugin (via
  // its RTLD_LOCAL copy), and standalone test binaries each hold their own
  // g_logger, but they all share spdlog's single process-global registry.
  // Dropping first keeps register_logger() from throwing on a duplicate name.
  if (spdlog::get("core")) {
    spdlog::drop("core");
  }
  g_logger =
      std::make_shared<spdlog::logger>("core", sinks.begin(), sinks.end());
  g_logger->set_pattern(pattern);

  // Flush on every log message to ensure immediate file writing
  g_logger->flush_on(spdlog::level::trace);

  // Register with spdlog
  spdlog::register_logger(g_logger);

  // Set log level
  set_log_level(level);
}

std::shared_ptr<spdlog::logger> get_logger() {
  if (!g_logger) {
    // Reuse the shared "core" logger if another module in the process already
    // created it (spdlog's registry is process-global even though g_logger is
    // per-module). This keeps host, plugins, and test binaries logging through
    // one logger instead of racing to register a duplicate "core" name.
    g_logger = spdlog::get("core");
    if (!g_logger) {
      init_logging();
    }
  }
  return g_logger;
}

void set_log_level(const std::string& level) {
  auto logger = get_logger();

  if (level == "trace") {
    logger->set_level(spdlog::level::trace);
  } else if (level == "debug") {
    logger->set_level(spdlog::level::debug);
  } else if (level == "info") {
    logger->set_level(spdlog::level::info);
  } else if (level == "warn" || level == "warning") {
    logger->set_level(spdlog::level::warn);
  } else if (level == "error") {
    logger->set_level(spdlog::level::err);
  } else if (level == "critical") {
    logger->set_level(spdlog::level::critical);
  } else if (level == "off") {
    logger->set_level(spdlog::level::off);
  } else {
    logger->warn("Unknown log level '{}', using 'info'", level.c_str());
    logger->set_level(spdlog::level::info);
  }
}

}  // namespace orc
