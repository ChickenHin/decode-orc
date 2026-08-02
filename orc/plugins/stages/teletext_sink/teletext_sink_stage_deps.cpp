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
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_row_squasher.h>
#include <orc/support/teletext_slicer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

// ITU-R BT.1700 Annex 1 Part B Table 1 item 2: 625-line PAL scans 50 fields
// per second; subtitle cue timing derives from the field index (the same
// field-number/field-rate derivation as the closed-caption sink).
constexpr double kPalFieldsPerSecond = 50.0;

// Format a field index as an SRT timestamp (HH:MM:SS,mmm).
std::string srt_timestamp(int64_t field_index) {
  const double seconds_total =
      static_cast<double>(field_index) / kPalFieldsPerSecond;
  const int64_t millis_total = std::llround(seconds_total * 1000.0);
  const int64_t hours = millis_total / 3'600'000;
  const int64_t minutes = (millis_total / 60'000) % 60;
  const int64_t seconds = (millis_total / 1'000) % 60;
  const int64_t millis = millis_total % 1'000;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(hours), static_cast<long long>(minutes),
                static_cast<long long>(seconds),
                static_cast<long long>(millis));
  return buffer;
}

// One emitted packet, held so squashing can rewrite it once every copy of
// every row has been seen. |empty| marks a keep_empty_packets placeholder.
struct StreamEntry {
  std::array<uint8_t, kTeletextPacketBytes> bytes;
  int64_t field_index;
  bool empty;
};

// Render the decoder's cues as a SubRip document.
std::string format_srt(const std::vector<TeletextSubtitleCue>& cues) {
  std::string srt;
  size_t index = 1;
  for (const auto& cue : cues) {
    srt += std::to_string(index++);
    srt += '\n';
    srt += srt_timestamp(cue.start_field_index);
    srt += " --> ";
    srt += srt_timestamp(cue.end_field_index);
    srt += '\n';
    srt += cue.text;
    srt += "\n\n";
  }
  return srt;
}

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

  // Squashing needs every copy of a row before it can combine them, so the
  // recovered stream is held and rewritten in a second pass. Without it the
  // stream is written straight out and subtitles decode inline, as before.
  const bool squash = options.squash_repeated_rows;
  TeletextRowSquasher squasher;
  std::vector<StreamEntry> stream;
  std::optional<TeletextPageDecoder> squash_pass;
  if (squash) {
    squash_pass.emplace();
    squash_pass->set_row_squasher(&squasher);
  }

  // Subtitle export: every recovered packet is additionally fed, in the
  // same temporal order, into a page decoder watching the subtitle page
  // (design §6.1). The page string was validated by the stage. When squashing
  // is on this runs in the rewrite pass instead, so cues come from the
  // corrected rows.
  std::optional<TeletextPageDecoder> page_decoder;
  if (options.export_subtitles) {
    page_decoder.emplace();
    if (!page_decoder->set_subtitle_page(options.subtitle_page)) {
      result.message = "Invalid subtitle page: " + options.subtitle_page;
      writer->close();
      return result;
    }
    if (squash) {
      page_decoder->set_row_squasher(&squasher);
    }
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

          // Cue and squash timing is relative to the start of the export
          // range.
          const int64_t relative_field_index =
              static_cast<int64_t>(frame_id - frame_rng.first) * 2 + field_idx;

          if (packet.has_value()) {
            ++result.packets_written;
            field_has_data = true;
            if (squash) {
              // The stream index is the copy's identity for the squasher, so
              // the rewrite pass can re-feed it without counting it twice.
              squash_pass->process_packet(*packet, relative_field_index,
                                          static_cast<int64_t>(stream.size()));
              stream.push_back(
                  StreamEntry{*packet, relative_field_index, false});
            } else {
              writer->write(packet->data(), packet->size());
              if (page_decoder.has_value()) {
                page_decoder->process_packet(*packet, relative_field_index);
              }
            }
          } else if (options.keep_empty_packets) {
            ++result.packets_written;
            if (squash) {
              stream.push_back(
                  StreamEntry{kEmptyPacket, relative_field_index, true});
            } else {
              writer->write(kEmptyPacket.data(), kEmptyPacket.size());
            }
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

    // Rewrite pass: now that every copy of every row has been seen, emit the
    // stream with each row replaced by the combination of its copies. Packet
    // order, count and timing are unchanged — only damaged display bytes
    // move. Headers and enhancement packets pass through untouched (their
    // display bytes carry a clock that differs between transmissions).
    if (squash) {
      // A fresh decoder re-derives which page each row belongs to. It shares
      // the squasher, and re-feeds each copy under its original stream index,
      // so the table it consults is the one built above, unchanged.
      TeletextPageDecoder rewrite;
      rewrite.set_row_squasher(&squasher);
      TeletextPageDecoder* attributor =
          page_decoder.has_value() ? &*page_decoder : &rewrite;

      for (size_t index = 0; index < stream.size(); ++index) {
        if (cancel_requested_ && cancel_requested_->load()) {
          writer->close();
          result.message =
              "Cancelled while writing squashed output; partial "
              "output left at " +
              output_path;
          ORC_LOG_WARN("TeletextSinkDeps: {}", result.message);
          return result;
        }
        if (progress_callback_ &&
            (index % (kProgressThrottleFrames * 100) == 0 ||
             index + 1 == stream.size())) {
          progress_callback_(index + 1, stream.size(),
                             "Combining repeated teletext rows " +
                                 std::to_string(index + 1) + "/" +
                                 std::to_string(stream.size()));
        }

        const StreamEntry& entry = stream[index];
        if (entry.empty) {
          writer->write(kEmptyPacket.data(), kEmptyPacket.size());
          continue;
        }

        auto out = entry.bytes;
        attributor->process_packet(entry.bytes, entry.field_index,
                                   static_cast<int64_t>(index));
        const auto& attribution = attributor->last_row_attribution();
        if (attribution.has_value()) {
          const auto squashed = squasher.squashed_row(
              *attribution, attributor->last_row_number());
          if (squashed.has_value() &&
              !std::equal(squashed->begin(), squashed->end(),
                          out.begin() + 2)) {
            std::copy(squashed->begin(), squashed->end(), out.begin() + 2);
            ++result.packets_corrected;
          }
        }
        writer->write(out.data(), out.size());
      }
      ORC_LOG_INFO(
          "TeletextSinkDeps: squashing corrected {} of {} row packets across "
          "{} sub-pages",
          result.packets_corrected, stream.size(), squasher.page_count());
    }

    writer->close();

    if (page_decoder.has_value()) {
      // Close any cue still on screen at the end of the export range.
      page_decoder->finalize(static_cast<int64_t>(total_frames) * 2);
      const auto& cues = page_decoder->subtitle_cues();

      // The .t42 extension was applied above; the SubRip document sits next
      // to the packet stream.
      std::string subtitle_path =
          output_path.substr(0, output_path.length() - t42_ext.length()) +
          ".srt";
      std::shared_ptr<IFileWriterUint8> subtitle_writer =
          stage_services_->create_buffered_file_writer_uint8(
              kWriterBufferBytes);
      if (!subtitle_writer || !subtitle_writer->open(subtitle_path)) {
        result.message =
            "Exported " + std::to_string(result.packets_written) +
            " teletext packets to " + output_path +
            " but failed to open subtitle output: " + subtitle_path;
        ORC_LOG_ERROR("TeletextSinkDeps: {}", result.message);
        return result;
      }
      const std::string srt = format_srt(cues);
      subtitle_writer->write(reinterpret_cast<const uint8_t*>(srt.data()),
                             srt.size());
      subtitle_writer->close();
      result.subtitle_path = subtitle_path;
      result.subtitle_cues_written = cues.size();
    }

    result.success = true;
    result.message = "Exported " + std::to_string(result.packets_written) +
                     " teletext packets (" +
                     std::to_string(result.fields_with_data) +
                     " fields with data) to " + output_path;
    if (!result.subtitle_path.empty()) {
      result.message += "; " + std::to_string(result.subtitle_cues_written) +
                        " subtitle cues to " + result.subtitle_path;
    }
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
