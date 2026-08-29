/*
 * File:        cc_sink_stage_deps.h
 * Module:      orc-core
 * Purpose:     CCSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CC_SINK_STAGE_DEPS_H
#define ORC_CORE_CC_SINK_STAGE_DEPS_H

#include <orc/stage/observation/observation_service_interface.h>
#include <orc/support/eia608_decoder.h>
#include <orc/support/eia608_service_demux.h>
#include <orc/support/logging.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cc_sink_stage_deps_interface.h"

namespace orc {
class CCSinkStageDeps : public ICCSinkStageDeps {
 public:
  // observation_service may be null (e.g. an older host, or direct in-process
  // construction in tests); export_cc() then falls back to whatever closed
  // caption observations are already present in the context (none on an older
  // host), producing an empty file rather than crashing.
  explicit CCSinkStageDeps(IObservationService* observation_service = nullptr)
      : observation_service_(observation_service) {}

  void init(TriggerProgressCallback progress_callback,
            std::atomic<bool>* cancel_requested) override;

  CCExportResult export_cc(VideoFrameRepresentation* representation,
                           IObservationContext& observation_context,
                           CCExportOptions options) override;

 private:
  /// One recovered byte pair, with where in the stream it came from
  struct CaptionPair {
    /// Field within its frame: 0 for the first, 1 for the second
    int field_in_frame = 0;
    /// Field index across the whole run, as the observation is keyed
    uint64_t field_index = 0;
    uint8_t data0 = 0;
    uint8_t data1 = 0;
  };

  /**
   * @brief Drive the closed_caption observer over the run and hand back every
   *        byte pair it recovers
   *
   * Shared by all four writers: they differ in what they do with the pairs,
   * not in how the pairs are found. Returns false when the export was
   * cancelled part-way.
   */
  bool for_each_caption_pair(const VideoFrameRepresentation* representation,
                             IObservationContext& observation_context,
                             IObserverHandle* cc_observer,
                             const std::function<void(const CaptionPair&)>& fn);

  bool export_scc(const VideoFrameRepresentation* representation,
                  const CCExportOptions& options, VideoFormat format,
                  IObservationContext& observation_context,
                  IObserverHandle* cc_observer, int32_t& cc_frames_exported);

  /// Plain text, SubRip and HTML: all three are the decoded caption stream,
  /// written out differently.
  bool export_decoded(const VideoFrameRepresentation* representation,
                      const CCExportOptions& options, VideoFormat format,
                      IObservationContext& observation_context,
                      IObserverHandle* cc_observer,
                      int32_t& cc_frames_exported);

  std::string generate_timestamp(int32_t field_index, VideoFormat format) const;
  uint8_t apply_odd_parity(uint8_t byte) const;
  int32_t sanity_check_data(int32_t data_byte) const;
  bool is_control_code(uint8_t byte) const;
  bool is_printable_char(uint8_t byte) const;

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
  EIA608Decoder eia608_decoder_;
  SpdlogLoggerAdapter logger_;
};
}  // namespace orc

#endif  // ORC_CORE_CC_SINK_STAGE_DEPS_H
