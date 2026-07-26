/*
 * File:        cc_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     CCSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cc_sink_stage_deps.h"

#include <orc/stage/common_types.h>

#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace orc {
void CCSinkStageDeps::init(TriggerProgressCallback progress_callback,
                           std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

CCExportResult CCSinkStageDeps::export_cc(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context, CCExportOptions options) {
  if (!representation) {
    return {false, "Input representation is null", 0};
  }

  if (options.output_path.empty()) {
    return {false, "output_path parameter is required", 0};
  }

  (void)options.write_csv;

  const auto frame_rng = representation->frame_range();
  if (frame_rng.count() == 0) {
    return {false, "Input has no frames", 0};
  }

  auto descriptor = representation->get_frame_descriptor(frame_rng.first);
  if (!descriptor.has_value()) {
    return {false, "Cannot determine video format", 0};
  }
  const VideoFormat video_format = video_format_from_system(descriptor->system);

  // Obtain a host-owned "closed_caption" observer session. The handle is reused
  // across every frame so the export runs the same standard observer the host
  // uses. A null service (older host) leaves the handle null; the export then
  // falls back to whatever observations already exist in the context.
  std::unique_ptr<IObserverHandle> cc_observer;
  if (observation_service_) {
    cc_observer = observation_service_->create_observer("closed_caption");
  }
  if (!cc_observer) {
    logger_.warn(
        "CCSinkDeps: observation service unavailable; closed caption data will "
        "be read from the context only");
  }

  int32_t cc_frames_exported = 0;
  bool success = false;

  if (options.export_format == CCExportFormat::SCC) {
    logger_.info("CCSinkDeps: Exporting closed captions to SCC format: {}",
                 options.output_path);
    success =
        export_scc(representation, options.output_path, video_format,
                   observation_context, cc_observer.get(), cc_frames_exported);
  } else {
    logger_.info(
        "CCSinkDeps: Exporting closed captions to plain text format: {}",
        options.output_path);
    success = export_plain_text(representation, options.output_path,
                                video_format, observation_context,
                                cc_observer.get(), cc_frames_exported);
  }

  if (!success) {
    return {false, "Failed to export closed captions", cc_frames_exported};
  }

  return {true, "Exported " + std::to_string(cc_frames_exported) + " CC frames",
          cc_frames_exported};
}

std::string CCSinkStageDeps::generate_timestamp(int32_t field_index,
                                                VideoFormat format) const {
  double frame_index = static_cast<double>(
      (field_index - 1) / 2);  // NOLINT(bugprone-integer-division)

  const double frames_per_second = (format == VideoFormat::PAL) ? 25.0 : 29.97;
  const double frames_per_minute = frames_per_second * 60.0;
  const double frames_per_hour = frames_per_minute * 60.0;

  const int32_t hh = static_cast<int32_t>(frame_index / frames_per_hour);
  frame_index -= static_cast<double>(hh) * frames_per_hour;
  const int32_t mm = static_cast<int32_t>(frame_index / frames_per_minute);
  frame_index -= static_cast<double>(mm) * frames_per_minute;
  const int32_t ss = static_cast<int32_t>(frame_index / frames_per_second);
  frame_index -= static_cast<double>(ss) * frames_per_second;
  const int32_t ff = static_cast<int32_t>(frame_index);

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << hh << ":" << std::setfill('0')
      << std::setw(2) << mm << ":" << std::setfill('0') << std::setw(2) << ss
      << ":" << std::setfill('0') << std::setw(2) << ff;

  return oss.str();
}

uint8_t CCSinkStageDeps::apply_odd_parity(uint8_t byte) const {
  uint8_t val = byte & 0x7F;
  int count = 0;
  uint8_t tmp = val;
  while (tmp) {
    count += static_cast<int>(tmp & 1U);
    tmp >>= 1;
  }
  if (count % 2 == 0) {
    val |= 0x80;
  }
  return val;
}

int32_t CCSinkStageDeps::sanity_check_data(int32_t data_byte) const {
  if (data_byte == -1) {
    return -1;
  }

  if (data_byte >= 0x10 && data_byte <= 0x1F) {
    return data_byte;
  }

  if (data_byte >= 0x20 && data_byte <= 0x7E) {
    return data_byte;
  }

  return 0;
}

bool CCSinkStageDeps::is_control_code(uint8_t byte) const {
  return byte >= 0x10 && byte <= 0x1F;
}

bool CCSinkStageDeps::is_printable_char(uint8_t byte) const {
  return byte >= 0x20 && byte <= 0x7E;
}

bool CCSinkStageDeps::export_scc(const VideoFrameRepresentation* representation,
                                 const std::string& output_path,
                                 VideoFormat format,
                                 IObservationContext& observation_context,
                                 IObserverHandle* cc_observer,
                                 int32_t& cc_frames_exported) {
  try {
    std::ofstream file(output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}", output_path);
      return false;
    }

    file << "Scenarist_SCC V1.0";

    bool caption_in_progress = false;
    std::string debug_caption;
    const auto frame_rng = representation->frame_range();
    const uint64_t total_frames = frame_rng.count();

    for (FrameID frame_id = frame_rng.first; frame_id <= frame_rng.last;
         ++frame_id) {
      if (cancel_requested_ && cancel_requested_->load()) {
        logger_.warn("CCSinkDeps: Cancelled during SCC export");
        return false;
      }
      if (progress_callback_) {
        const uint64_t done = frame_id - frame_rng.first + 1;
        progress_callback_(done, total_frames, "Processing closed captions...");
      }

      // Run the standard closed_caption observer for this frame so its results
      // land in the context (fields frame_id*2 and frame_id*2 + 1). When the
      // service is unavailable, read whatever is already present.
      //
      // Phase 5.3: when the host has pre-loaded this frame's observation from
      // the provenance-keyed store, skip re-running the observer.
      // closed_caption is stateful, so the host only pre-loads when the whole
      // range is covered (all-or-nothing); a covered frame here therefore means
      // the entire run is covered and the stream's continuity is preserved.
      const bool frame_covered =
          observation_context.has(FieldID(frame_id * 2), "closed_caption",
                                  "present") ||
          observation_context.has(FieldID(frame_id * 2 + 1), "closed_caption",
                                  "present");
      if (cc_observer && representation->has_frame(frame_id) &&
          !frame_covered) {
        cc_observer->process_frame(*representation, frame_id,
                                   observation_context);
      }

      for (int field_idx = 0; field_idx < 2; ++field_idx) {
        const FieldID field_id(frame_id * 2 + static_cast<uint64_t>(field_idx));

        int32_t data0 = -1;
        int32_t data1 = -1;

        auto present_obs =
            observation_context.get(field_id, "closed_caption", "present");
        if (present_obs && std::holds_alternative<bool>(*present_obs) &&
            std::get<bool>(*present_obs)) {
          auto data0_obs =
              observation_context.get(field_id, "closed_caption", "data0");
          auto data1_obs =
              observation_context.get(field_id, "closed_caption", "data1");

          if (data0_obs && data1_obs &&
              std::holds_alternative<int32_t>(*data0_obs) &&
              std::holds_alternative<int32_t>(*data1_obs)) {
            auto parity0_obs = observation_context.get(
                field_id, "closed_caption", "parity0_valid");
            auto parity1_obs = observation_context.get(
                field_id, "closed_caption", "parity1_valid");

            const bool parity0_valid =
                parity0_obs && std::holds_alternative<bool>(*parity0_obs)
                    ? std::get<bool>(*parity0_obs)
                    : false;
            const bool parity1_valid =
                parity1_obs && std::holds_alternative<bool>(*parity1_obs)
                    ? std::get<bool>(*parity1_obs)
                    : false;

            if (parity0_valid || parity1_valid) {
              data0 = sanity_check_data(std::get<int32_t>(*data0_obs));
              data1 = sanity_check_data(std::get<int32_t>(*data1_obs));
            }
          }
        }

        // Suppress spurious mid-caption starts: a caption must begin with the
        // 0x14 control code.
        if (!caption_in_progress && data0 > 0 && data0 != 0x14) {
          data0 = 0;
          data1 = 0;
        }

        if (data0 == -1 || data1 == -1) {
          continue;
        }

        if (data0 > 0 || data1 > 0) {
          if (!caption_in_progress) {
            const std::string timestamp = generate_timestamp(
                static_cast<int32_t>(field_id.value() + 1), format);
            file << "\n\n" << timestamp << "\t";

            debug_caption = "Caption at " + timestamp + " : [";
            caption_in_progress = true;
          }

          const uint8_t scc0 = apply_odd_parity(static_cast<uint8_t>(data0));
          const uint8_t scc1 = apply_odd_parity(static_cast<uint8_t>(data1));
          file << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc0) << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc1) << " ";

          if (is_control_code(static_cast<uint8_t>(data0))) {
            debug_caption += " ";
          } else {
            char chars[3] = {static_cast<char>(data0), static_cast<char>(data1),
                             0};
            debug_caption += std::string(chars);
          }

          cc_frames_exported++;
        } else {
          if (caption_in_progress) {
            debug_caption += "]";
            logger_.debug("CCSinkDeps: {}", debug_caption);
          }
          caption_in_progress = false;
        }
      }

      if (cc_observer) {
        observation_context.clear_field(FieldID(frame_id * 2));
        observation_context.clear_field(FieldID(frame_id * 2 + 1));
      }
    }

    file << "\n\n";
    file.close();

    logger_.info("CCSinkDeps: Exported {} SCC caption fields",
                 cc_frames_exported);
    return true;

  } catch (const std::exception& e) {
    logger_.error("CCSinkDeps: Error exporting SCC: {}", e.what());
    return false;
  }
}

bool CCSinkStageDeps::export_plain_text(
    const VideoFrameRepresentation* representation,
    const std::string& output_path, VideoFormat format,
    IObservationContext& observation_context, IObserverHandle* cc_observer,
    int32_t& cc_frames_exported) {
  try {
    std::ofstream file(output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}", output_path);
      return false;
    }

    eia608_decoder_ = EIA608Decoder{};

    const double frames_per_second =
        (format == VideoFormat::PAL) ? 25.0 : 29.97;
    const auto frame_rng = representation->frame_range();
    const uint64_t total_frames = frame_rng.count();

    for (FrameID frame_id = frame_rng.first; frame_id <= frame_rng.last;
         ++frame_id) {
      if (cancel_requested_ && cancel_requested_->load()) {
        logger_.warn("CCSinkDeps: Cancelled during plain text export");
        return false;
      }
      if (progress_callback_) {
        const uint64_t done = frame_id - frame_rng.first + 1;
        progress_callback_(done, total_frames, "Processing closed captions...");
      }

      // Phase 5.3: skip the observer for frames the host has pre-loaded from
      // the store (all-or-nothing for the stateful closed_caption stream, see
      // export_scc()).
      const bool frame_covered =
          observation_context.has(FieldID(frame_id * 2), "closed_caption",
                                  "present") ||
          observation_context.has(FieldID(frame_id * 2 + 1), "closed_caption",
                                  "present");
      if (cc_observer && representation->has_frame(frame_id) &&
          !frame_covered) {
        cc_observer->process_frame(*representation, frame_id,
                                   observation_context);
      }

      for (int field_idx = 0; field_idx < 2; ++field_idx) {
        const FieldID field_id(frame_id * 2 + static_cast<uint64_t>(field_idx));

        auto present_obs =
            observation_context.get(field_id, "closed_caption", "present");
        if (!present_obs || !std::holds_alternative<bool>(*present_obs) ||
            !std::get<bool>(*present_obs)) {
          continue;
        }

        auto data0_obs =
            observation_context.get(field_id, "closed_caption", "data0");
        auto data1_obs =
            observation_context.get(field_id, "closed_caption", "data1");

        if (!data0_obs || !data1_obs ||
            !std::holds_alternative<int32_t>(*data0_obs) ||
            !std::holds_alternative<int32_t>(*data1_obs)) {
          continue;
        }

        const int32_t data0 = std::get<int32_t>(*data0_obs);
        const int32_t data1 = std::get<int32_t>(*data1_obs);

        auto parity0_obs = observation_context.get(field_id, "closed_caption",
                                                   "parity0_valid");
        auto parity1_obs = observation_context.get(field_id, "closed_caption",
                                                   "parity1_valid");

        const bool parity0_valid =
            parity0_obs && std::holds_alternative<bool>(*parity0_obs)
                ? std::get<bool>(*parity0_obs)
                : false;
        const bool parity1_valid =
            parity1_obs && std::holds_alternative<bool>(*parity1_obs)
                ? std::get<bool>(*parity1_obs)
                : false;

        if (!parity0_valid && !parity1_valid) {
          continue;
        }

        const uint8_t byte1 = static_cast<uint8_t>(sanity_check_data(data0));
        const uint8_t byte2 = static_cast<uint8_t>(sanity_check_data(data1));

        const double timestamp =
            (static_cast<double>(field_id.value() + 1) / 2.0) /
            frames_per_second;
        eia608_decoder_.process_bytes(timestamp, byte1, byte2);
        cc_frames_exported++;
      }

      if (cc_observer) {
        observation_context.clear_field(FieldID(frame_id * 2));
        observation_context.clear_field(FieldID(frame_id * 2 + 1));
      }
    }

    const auto& cues = eia608_decoder_.get_cues();
    logger_.info("CCSinkDeps: Extracted {} caption cues", cues.size());

    for (const auto& cue : cues) {
      int frame_number =
          static_cast<int>(cue.start_time * frames_per_second * 2.0);
      std::string timestamp = generate_timestamp(frame_number, format);

      file << "\n[" << timestamp << "]\n";
      for (char ch : cue.text) {
        const uint8_t byte = static_cast<uint8_t>(ch);
        if (is_printable_char(byte) || ch == '\n' || ch == '\r' || ch == '\t') {
          file << ch;
        }
      }
      file << "\n";
    }

    file.close();
    return true;

  } catch (const std::exception& e) {
    logger_.error("CCSinkDeps: Error exporting plain text: {}", e.what());
    return false;
  }
}
}  // namespace orc
