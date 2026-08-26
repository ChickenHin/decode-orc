/*
 * File:        core_init.cpp
 * Module:      orc-presenters
 * Purpose:     Core initialization functions for presenters layer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/logging.h>

#include <atomic>
#include <thread>

#include "../include/project_presenter.h"

namespace orc::presenters {

void initCoreLogging(const std::string& level, const std::string& pattern,
                     const std::string& log_file, LogDestination destination) {
  orc::init_logging(level, pattern, log_file, destination);
}

bool reconfigureCoreLogging(const std::string& level,
                            const std::string& pattern,
                            const std::string& log_file,
                            LogDestination destination, bool truncate_log_file,
                            std::string* error_message) {
  return orc::reconfigure_logging(level, pattern, log_file, destination,
                                  truncate_log_file, error_message);
}

namespace {
// 0 == "auto" (half the hardware concurrency). Written once at startup before
// any project is opened, read on each scheduler build; an atomic keeps that
// cross-thread hand-off well-defined.
std::atomic<unsigned> g_background_observation_worker_count{0};
}  // namespace

void setBackgroundObservationWorkerCount(unsigned count) {
  g_background_observation_worker_count.store(count, std::memory_order_relaxed);
}

unsigned resolveBackgroundObservationWorkerCount() {
  const unsigned configured =
      g_background_observation_worker_count.load(std::memory_order_relaxed);
  if (configured > 0) {
    return configured;
  }
  // Auto: half the cores, at least 1. hardware_concurrency() may report 0 when
  // the count is indeterminate, in which case fall back to single-threaded.
  const unsigned hw = std::thread::hardware_concurrency();
  return hw > 1 ? hw / 2 : 1;
}

}  // namespace orc::presenters
