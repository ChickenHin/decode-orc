/*
 * File:        burst_level_analysis_sink_deps.h
 * Module:      orc-core
 * Purpose:     BurstLevelAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_H
#define ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_H

#include <orc/stage/observation/observation_service_interface.h>
#include <orc/support/logging.h>

#include <atomic>
#include <iosfwd>
#include <memory>
#include <utility>

#include "burst_level_analysis_sink_deps_interface.h"

namespace orc {
class BurstLevelAnalysisSinkStageDeps
    : public IBurstLevelAnalysisSinkStageDeps {
 public:
  // observation_service may be null (e.g. an older host, or direct in-process
  // construction in tests); compute_and_analyze() then skips observation and
  // reports empty burst-level statistics.
  explicit BurstLevelAnalysisSinkStageDeps(
      IObservationService* observation_service)
      : observation_service_(observation_service) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  BurstAnalysisComputeResult compute_and_analyze(
      VideoFrameRepresentation* representation,
      IObservationContext& observation_context,
      BurstAnalysisComputeOptions options) override;

  bool write_csv(const std::string& path,
                 const std::vector<FrameBurstLevelStats>& frame_stats) override;

  // Filesystem-free formatter. Writes the canonical per-frame CSV to `os`:
  // frame_number,median_burst_10bit — one row per analysed frame, with an
  // absent value as an empty field. Not part of the deps interface so it can be
  // unit-tested directly against an std::ostringstream.
  void write_csv(std::ostream& os,
                 const std::vector<FrameBurstLevelStats>& frame_stats);

 private:
  class SpdlogLoggerAdapter {
   public:
    template <typename... Args>
    void trace(const char* fmt, Args&&... args) const {
      ORC_LOG_TRACE(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(const char* fmt, Args&&... args) const {
      ORC_LOG_DEBUG(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const char* fmt, Args&&... args) const {
      ORC_LOG_INFO(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(const char* fmt, Args&&... args) const {
      ORC_LOG_WARN(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(const char* fmt, Args&&... args) const {
      ORC_LOG_ERROR(fmt, std::forward<Args>(args)...);
    }
  };

  IObservationService* observation_service_{nullptr};
  TriggerProgressCallback progress_callback_;
  std::atomic<bool>* cancel_requested_{nullptr};
  SpdlogLoggerAdapter logger_;
};
}  // namespace orc

#endif  // ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_DEPS_H
