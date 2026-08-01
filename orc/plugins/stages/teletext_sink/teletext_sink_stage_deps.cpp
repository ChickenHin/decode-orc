/*
 * File:        teletext_sink_stage_deps.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     TeletextSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_sink_stage_deps.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>
#include <orc/support/teletext_slicer.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace orc {

namespace {

constexpr size_t kWriterBufferBytes = 1UL * 1024UL * 1024UL;

// Progress callbacks are throttled to once every N frames (plus the final
// frame) so tight loops do not flood the UI.
constexpr uint64_t kProgressThrottleFrames = 10;

// 42 zero bytes emitted for a candidate line with no data when
// keep_empty_packets is enabled — the vhs-decode convention giving a 1:1
// packet-to-(frame, field, line) mapping (design §2.2).
constexpr std::array<uint8_t, kTeletextPacketBytes> kEmptyPacket{};

}  // namespace

void TeletextSinkStageDeps::init(TriggerProgressCallback progress_callback,
                                 std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

TeletextSinkResult TeletextSinkStageDeps::export_t42(
    const VideoFrameRepresentation* representation,
    IObservationContext& observation_context,
    const TeletextSinkOptions& options) {
  TeletextSinkResult result;

  if (!representation) {
    result.message = "Input representation is null";
    return result;
  }

  std::string output_path = options.output_path;
  const std::string t42_ext = ".t42";
  if (output_path.length() < t42_ext.length() ||
      output_path.compare(output_path.length() - t42_ext.length(),
                          t42_ext.length(), t42_ext) != 0) {
    output_path += t42_ext;
    ORC_LOG_DEBUG("TeletextSinkDeps: Added .t42 extension: {}", output_path);
  }
  result.output_path = output_path;

  // ETSI EN 300 706 System B on 625-line PAL is the only supported system
  // (design §1.3).
  const auto vp_opt = representation->get_video_parameters();
  if (!vp_opt.has_value() || vp_opt->system != VideoSystem::PAL) {
    result.message = "Input is not PAL (teletext sink is PAL WST only)";
    return result;
  }
  const auto& vp = vp_opt.value();

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();
  if (total_frames == 0) {
    result.message = "Input has no frames";
    return result;
  }

  std::shared_ptr<IFileWriterUint8> writer;
  if (stage_services_) {
    writer =
        stage_services_->create_buffered_file_writer_uint8(kWriterBufferBytes);
  }
  if (!writer) {
    result.message = "Failed to create output writer service";
    return result;
  }
  if (!writer->open(output_path)) {
    result.message = "Failed to open output file: " + output_path;
    return result;
  }

  // The default slicer options match the host observer's fixed configuration,
  // so its (cacheable) observations can be consumed directly. Non-default
  // options require slicing here with a locally configured TeletextSlicer.
  const bool use_observer_path =
      !options.tolerant_framing && options.require_valid_mrag;

  std::unique_ptr<IObserverHandle> observer;
  if (use_observer_path && observation_service_) {
    observer = observation_service_->create_observer("teletext");
  }
  if (use_observer_path && !observer) {
    ORC_LOG_WARN(
        "TeletextSinkDeps: observation service unavailable; teletext data "
        "will be read from the context only");
  }

  std::optional<TeletextSlicer> slicer;
  if (!use_observer_path) {
    TeletextSlicerOptions slicer_options;
    slicer_options.tolerant_framing = options.tolerant_framing;
    slicer_options.require_valid_mrag = options.require_valid_mrag;
    // EBU Tech. 3280-E §1.1.1 Table 1: 4FSC PAL sample rate; bit rate fixed
    // at 444 × fH by ETSI EN 300 706 §5.3 (TeletextSlicer default).
    slicer.emplace(kPalSampleRate, kTeletextBitRate, slicer_options);
  }

  // Data levels from the source, with the spec constants as fallback (the
  // observer applies the same rule).
  const int16_t black_level =
      static_cast<int16_t>(vp.black_level >= 0 ? vp.black_level : kPalBlack);
  const int16_t white_level =
      static_cast<int16_t>(vp.white_level >= 0 ? vp.white_level : kPalWhite);
  const size_t f1_lines = field1_lines(vp.system);
  const size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  const size_t frame_height = static_cast<size_t>(vp.frame_height);

  try {
    uint64_t frames_processed = 0;
    for (FrameID frame_id = frame_rng.first; frame_id <= frame_rng.last;
         ++frame_id) {
      if (cancel_requested_ && cancel_requested_->load()) {
        writer->close();
        result.message = "Cancelled after " + std::to_string(frames_processed) +
                         " of " + std::to_string(total_frames) +
                         " frames; partial output left at " + output_path;
        ORC_LOG_WARN("TeletextSinkDeps: {}", result.message);
        return result;
      }

      ++frames_processed;
      if (progress_callback_ &&
          (frames_processed % kProgressThrottleFrames == 0 ||
           frames_processed == total_frames)) {
        progress_callback_(frames_processed, total_frames,
                           "Exporting teletext frame " +
                               std::to_string(frames_processed) + "/" +
                               std::to_string(total_frames));
      }

      const FieldID field0(frame_id * 2);
      const FieldID field1(frame_id * 2 + 1);

      // Per-frame coverage skip: the observer is stateless, so a frame whose
      // observations were pre-loaded from the host's provenance store need
      // not be sliced again.
      if (observer) {
        const bool frame_covered =
            observation_context.has(field0, "teletext", "present") &&
            observation_context.has(field1, "teletext", "present");
        if (!frame_covered && representation->has_frame(frame_id)) {
          observer->process_frame(*representation, frame_id,
                                  observation_context);
        }
      }

      // Packet emission is strictly temporal: frame → field (1 then 2) →
      // ascending line — the order carousel-reassembling consumers expect.
      for (int field_idx = 0; field_idx < 2; ++field_idx) {
        const FieldID field_id(frame_id * 2 + static_cast<uint64_t>(field_idx));
        bool field_has_data = false;

        for (int32_t field_line = options.first_field_line;
             field_line <= options.last_field_line; ++field_line) {
          std::optional<std::array<uint8_t, kTeletextPacketBytes>> packet;

          if (slicer) {
            // Direct slicing (non-default slicer options). Same line fetch
            // idiom as the observer: luma channel for YC sources, buffered
            // per-line reads otherwise.
            const size_t flat_line = (field_idx == 0 ? 0 : f1_lines) +
                                     static_cast<size_t>(field_line);
            if (flat_line < frame_height) {
              const int16_t* line_data = nullptr;
              size_t sample_count = 0;
              std::vector<int16_t> line_copy;
              if (representation->has_separate_channels()) {
                line_data = representation->get_line_luma(frame_id, flat_line);
                sample_count = line_width;
              } else {
                line_copy =
                    representation->get_line_samples(frame_id, flat_line);
                line_data = line_copy.data();
                sample_count = line_copy.size();
              }
              if (line_data != nullptr && sample_count > 0) {
                const TeletextLineResult sliced = slicer->slice(
                    line_data, sample_count, black_level, white_level);
                if (sliced.valid) {
                  packet = sliced.bytes;
                }
              }
            }
          } else {
            const auto obs = observation_context.get(
                field_id, "teletext", "t42_" + std::to_string(field_line));
            if (obs && std::holds_alternative<std::string>(*obs)) {
              packet = teletext_hex_to_packet(std::get<std::string>(*obs));
            }
          }

          if (packet.has_value()) {
            writer->write(packet->data(), packet->size());
            ++result.packets_written;
            field_has_data = true;
          } else if (options.keep_empty_packets) {
            writer->write(kEmptyPacket.data(), kEmptyPacket.size());
            ++result.packets_written;
          }
        }

        if (field_has_data) {
          ++result.fields_with_data;
        }
      }

      // Memory hygiene: drop this frame's observations once consumed.
      if (observer) {
        observation_context.clear_field(field0);
        observation_context.clear_field(field1);
      }
    }

    writer->close();

    result.success = true;
    result.message = "Exported " + std::to_string(result.packets_written) +
                     " teletext packets (" +
                     std::to_string(result.fields_with_data) +
                     " fields with data) to " + output_path;
    ORC_LOG_INFO("TeletextSinkDeps: {}", result.message);
    return result;

  } catch (const std::exception& e) {
    writer->close();
    result.success = false;
    result.message = std::string("Error during teletext export: ") + e.what();
    ORC_LOG_ERROR("TeletextSinkDeps: {}", result.message);
    return result;
  }
}

}  // namespace orc
