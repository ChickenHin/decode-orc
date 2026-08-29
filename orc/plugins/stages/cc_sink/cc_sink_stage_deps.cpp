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

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <variant>

namespace orc {
namespace {

/// Frame rate of the video system, exactly. NTSC line 21 timing is derived
/// from 30000/1001 and not from its rounded "29.97" spelling; at an hour the
/// difference is a few frames.
double frame_rate_hz(VideoFormat format) {
  return (format == VideoFormat::PAL) ? 25.0 : (30000.0 / 1001.0);
}

/// SubRip timestamp: HH:MM:SS,mmm
std::string srt_timestamp(double seconds) {
  if (seconds < 0.0) {
    seconds = 0.0;
  }
  const int64_t total_ms = static_cast<int64_t>(std::llround(seconds * 1000.0));
  const int64_t ms = total_ms % 1000;
  const int64_t total_s = total_ms / 1000;
  const int64_t ss = total_s % 60;
  const int64_t mm = (total_s / 60) % 60;
  const int64_t hh = total_s / 3600;

  std::ostringstream oss;
  oss << std::setfill('0') << std::setw(2) << hh << ':' << std::setw(2) << mm
      << ':' << std::setw(2) << ss << ',' << std::setw(3) << ms;
  return oss.str();
}

/// Escape the five characters that cannot appear literally in HTML text.
std::string html_escape(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

}  // namespace

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
    logger_.info("CCSinkDeps: Exporting {} to SCC format: {}",
                 eia608_service_name(options.service), options.output_path);
    success =
        export_scc(representation, options, video_format, observation_context,
                   cc_observer.get(), cc_frames_exported);
  } else {
    logger_.info("CCSinkDeps: Exporting {} to {}: {}",
                 eia608_service_name(options.service),
                 options.export_format == CCExportFormat::SRT    ? "SubRip"
                 : options.export_format == CCExportFormat::HTML ? "HTML"
                                                                 : "plain text",
                 options.output_path);
    success = export_decoded(representation, options, video_format,
                             observation_context, cc_observer.get(),
                             cc_frames_exported);
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

  const double frames_per_second = frame_rate_hz(format);
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

bool CCSinkStageDeps::for_each_caption_pair(
    const VideoFrameRepresentation* representation,
    IObservationContext& observation_context, IObserverHandle* cc_observer,
    const std::function<void(const CaptionPair&)>& fn) {
  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  for (FrameID frame_id = frame_rng.first; frame_id <= frame_rng.last;
       ++frame_id) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn("CCSinkDeps: Cancelled during closed caption export");
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
    if (cc_observer && representation->has_frame(frame_id) && !frame_covered) {
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

      auto parity0_obs =
          observation_context.get(field_id, "closed_caption", "parity0_valid");
      auto parity1_obs =
          observation_context.get(field_id, "closed_caption", "parity1_valid");

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

      CaptionPair pair;
      pair.field_in_frame = field_idx;
      pair.field_index = field_id.value();
      pair.data0 = static_cast<uint8_t>(
          sanity_check_data(std::get<int32_t>(*data0_obs)));
      pair.data1 = static_cast<uint8_t>(
          sanity_check_data(std::get<int32_t>(*data1_obs)));
      fn(pair);
    }

    if (cc_observer) {
      observation_context.clear_field(FieldID(frame_id * 2));
      observation_context.clear_field(FieldID(frame_id * 2 + 1));
    }
  }

  return true;
}

bool CCSinkStageDeps::export_scc(const VideoFrameRepresentation* representation,
                                 const CCExportOptions& options,
                                 VideoFormat format,
                                 IObservationContext& observation_context,
                                 IObserverHandle* cc_observer,
                                 int32_t& cc_frames_exported) {
  try {
    std::ofstream file(options.output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}",
                    options.output_path);
      return false;
    }

    file << "Scenarist_SCC V1.0";

    // The byte pairs are written exactly as transmitted, including the channel
    // bits of the selected service and the duplicate copy of every control
    // pair: an SCC file is a record of the line 21 stream, and the tools that
    // read one de-duplicate for themselves.
    EIA608ServiceDemux demux(options.service,
                             /*suppress_repeated_controls=*/false);

    bool caption_in_progress = false;
    std::string debug_caption;

    const bool completed = for_each_caption_pair(
        representation, observation_context, cc_observer,
        [&](const CaptionPair& pair) {
          const bool mine =
              demux.accept(pair.field_in_frame, pair.data0, pair.data1);

          // A field whose pair went to one of the other three services (or to
          // none, being null padding) breaks the run: what follows is written
          // as a fresh timestamped block rather than being run on to the bytes
          // before it.
          if (!mine || (pair.data0 == 0 && pair.data1 == 0)) {
            if (caption_in_progress) {
              debug_caption += "]";
              logger_.debug("CCSinkDeps: {}", debug_caption);
            }
            caption_in_progress = false;
            return;
          }

          if (!caption_in_progress) {
            const std::string timestamp = generate_timestamp(
                static_cast<int32_t>(pair.field_index + 1), format);
            file << "\n\n" << timestamp << "\t";

            debug_caption = "Caption at " + timestamp + " : [";
            caption_in_progress = true;
          }

          const uint8_t scc0 = apply_odd_parity(pair.data0);
          const uint8_t scc1 = apply_odd_parity(pair.data1);
          file << std::hex << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc0) << std::setfill('0') << std::setw(2)
               << static_cast<int>(scc1) << " ";

          if (is_control_code(pair.data0)) {
            debug_caption += " ";
          } else {
            const char chars[3] = {static_cast<char>(pair.data0),
                                   static_cast<char>(pair.data1), 0};
            debug_caption += std::string(chars);
          }

          cc_frames_exported++;
        });

    if (!completed) {
      return false;
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

bool CCSinkStageDeps::export_decoded(
    const VideoFrameRepresentation* representation,
    const CCExportOptions& options, VideoFormat format,
    IObservationContext& observation_context, IObserverHandle* cc_observer,
    int32_t& cc_frames_exported) {
  try {
    std::ofstream file(options.output_path);
    if (!file.is_open()) {
      logger_.error("CCSinkDeps: Failed to open output file: {}",
                    options.output_path);
      return false;
    }

    eia608_decoder_ = EIA608Decoder{};

    // Decoding acts on the stream, so here the duplicate copy of each control
    // pair must be dropped: acted on twice, a roll-up scrolls two lines and a
    // caption is displayed twice.
    EIA608ServiceDemux demux(options.service,
                             /*suppress_repeated_controls=*/true);

    const double frames_per_second = frame_rate_hz(format);
    double last_timestamp = 0.0;

    const bool completed = for_each_caption_pair(
        representation, observation_context, cc_observer,
        [&](const CaptionPair& pair) {
          if (!demux.accept(pair.field_in_frame, pair.data0, pair.data1)) {
            return;
          }

          // Field index to seconds: two fields per frame.
          const double timestamp =
              static_cast<double>(pair.field_index) / (2.0 * frames_per_second);
          last_timestamp = timestamp;
          eia608_decoder_.process_bytes(timestamp, pair.data0, pair.data1);
          cc_frames_exported++;
        });

    if (!completed) {
      return false;
    }

    // Close whatever was still on screen when the run ended — without this the
    // last caption of every recording is dropped.
    const auto frame_rng = representation->frame_range();
    const double end_time =
        std::max(last_timestamp,
                 static_cast<double>(frame_rng.last + 1) / frames_per_second);
    const std::vector<CaptionCue> cues = eia608_decoder_.finalize(end_time);
    logger_.info("CCSinkDeps: Extracted {} caption cues for {}", cues.size(),
                 eia608_service_name(options.service));

    switch (options.export_format) {
      case CCExportFormat::SRT: {
        size_t index = 1;
        for (const auto& cue : cues) {
          file << index++ << "\n"
               << srt_timestamp(cue.start_time) << " --> "
               << srt_timestamp(cue.end_time) << "\n"
               << cue.text << "\n\n";
        }
        break;
      }

      case CCExportFormat::HTML: {
        // Monospaced, and every line inside a <pre>: a text service positions
        // its text by column, so a page of listings or scores only reads
        // correctly while the columns still line up.
        file << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
             << "<meta charset=\"utf-8\">\n"
             << "<title>Closed captions — "
             << eia608_service_name(options.service) << "</title>\n"
             << "<style>\n"
             << "body { font-family: sans-serif; margin: 2em; }\n"
             << "table { border-collapse: collapse; }\n"
             << "td { vertical-align: top; padding: 0.15em 1em 0.15em 0; }\n"
             << "td.time { font-family: monospace; white-space: nowrap;"
             << " color: #666; }\n"
             << "pre { font-family: monospace; margin: 0; }\n"
             << "</style>\n</head>\n<body>\n"
             << "<h1>Closed captions — " << eia608_service_name(options.service)
             << "</h1>\n"
             << "<table>\n";
        for (const auto& cue : cues) {
          file << "<tr><td class=\"time\">" << srt_timestamp(cue.start_time)
               << "</td><td><pre>" << html_escape(cue.text)
               << "</pre></td></tr>\n";
        }
        file << "</table>\n</body>\n</html>\n";
        break;
      }

      case CCExportFormat::PLAIN_TEXT:
      default: {
        for (const auto& cue : cues) {
          const int frame_number =
              static_cast<int>(cue.start_time * frames_per_second * 2.0);
          file << "\n[" << generate_timestamp(frame_number, format) << "]\n";
          for (const char ch : cue.text) {
            const uint8_t byte = static_cast<uint8_t>(ch);
            if (is_printable_char(byte) || ch == '\n' || ch == '\r' ||
                ch == '\t') {
              file << ch;
            }
          }
          file << "\n";
        }
        break;
      }
    }

    file.close();
    return true;

  } catch (const std::exception& e) {
    logger_.error("CCSinkDeps: Error exporting closed captions: {}", e.what());
    return false;
  }
}
}  // namespace orc
