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
#include <orc/support/teletext_recovery_stats.h>
#include <orc/support/teletext_row_squasher.h>
#include <orc/support/teletext_slicer.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "teletext_squash_stats.h"

namespace orc {

namespace {

constexpr size_t kWriterBufferBytes = 1UL * 1024UL * 1024UL;

// Page runs the squasher retains for the rewrite pass (see where it is used).
// A run costs its received rows: at most 24 × 16 copies × ~210 bytes, but a
// run created by an erase-on-every-transmission service holds one copy per row
// and costs around 5 kB. The bound is the point past which a recording's older
// runs start going uncorrected rather than a memory ceiling being enforced.
constexpr size_t kSquashPageRunBound = 4096;

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
  return fmt::format("{:02}:{:02}:{:02},{:03}", hours, minutes, seconds,
                     millis);
}

// One emitted packet, held so squashing can rewrite it once every copy of
// every row has been seen. |empty| marks a keep_empty_packets placeholder.
//
// The recovered stream of a long source is tens of millions of these (600k
// frames × up to 34 candidate lines), so the entry is kept lean: the per-byte
// confidence — absent entirely for threshold-detected packets, which is every
// packet of a disc or direct capture — lives in a side pool rather than as a
// 168-byte float array inside every entry.
struct StreamEntry {
  std::array<uint8_t, kTeletextPacketBytes> bytes;
  int64_t field_index;
  bool empty;
  // How sure the recovery chain was of each byte, and whether it could say.
  // Held with the packet because the rewrite pass re-feeds every copy under
  // its original identity: a re-feed that dropped the confidence would replace
  // a weighted copy with an unweighted one. |confidence_slot| indexes the
  // confidence pool when |has_confidence| is set.
  bool has_confidence;
  uint32_t confidence_slot;
};

// Pooled per-byte confidence, quantised to 8 bits (value × 255). Lossless for
// the observer path, whose stored observations already quantise to 16 levels
// (level/15 × 255 = 17 × level exactly); the direct-slicer path loses at most
// 1/255 of a vote weight per byte.
using QuantizedPacketConfidence = std::array<uint8_t, kTeletextPacketBytes>;

// Display row a packet is addressed to (X/1 to X/24, ETSI EN 300 706 §9.3.2),
// or 0 when the MRAG does not decode or the packet is not a displayable row.
// The character figures are counted over these: they are the packets whose 40
// bytes carry byte-wise odd parity and are shown on screen.
int display_row_of(const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  if (mrag_low < 0 || mrag_high < 0) {
    return 0;
  }
  const int row = ((mrag_low >> 3) & 0x01) | ((mrag_high << 1) & 0x1E);
  return (row >= 1 && row < TeletextPageSnapshot::kRows) ? row : 0;
}

// The 40 display bytes of a packet (§9.3.2: the payload after the 2 MRAG
// bytes).
TeletextRowBytes display_bytes_of(
    const std::array<uint8_t, kTeletextPacketBytes>& packet) {
  TeletextRowBytes row{};
  std::copy(packet.begin() + 2, packet.begin() + 2 + kTeletextRowBytes,
            row.begin());
  return row;
}

// Name a detector for the report, in the wording the parameter uses.
const char* detector_name(TeletextDetector detector) {
  switch (detector) {
    case TeletextDetector::kThreshold:
      return "Threshold";
    case TeletextDetector::kMlse:
      return "MLSE";
    case TeletextDetector::kAuto:
      return "Automatic";
  }
  return "Automatic";
}

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

std::string TeletextSinkStageDeps::build_report(
    const TeletextSinkOptions& options, const TeletextSinkResult& result,
    uint64_t total_frames, const TeletextRecoveryStats& stats,
    const TeletextSquashStats& squash_stats) {
  // The headline first: the one figure a reader wants before deciding whether
  // any of the detail below is worth their time.
  std::string report = "Teletext export report\n";
  const std::string loss = squash_stats.character_loss_summary();
  if (!loss.empty()) {
    report += "  " + loss + "\n\n";
  }
  report += fmt::format(
      "  Output:        {}\n"
      "  Frames:        {}\n"
      "  VBI lines:     {}-{} of each field (1-based)\n"
      "  Detector:      {}\n"
      "  Packets:       {} written, {} fields carried data",
      result.output_path, total_frames, options.first_field_line + 1,
      options.last_field_line + 1, detector_name(options.detector),
      result.packets_written, result.fields_with_data);
  if (options.keep_empty_packets) {
    report += " (empty candidate lines padded)";
  }
  if (result.bytes_repaired > 0) {
    report += fmt::format("\n  Parity repair: {} damaged display bytes mended",
                          result.bytes_repaired);
  }
  if (!result.subtitle_path.empty()) {
    report += fmt::format("\n  Subtitles:     {} cues from page {} to {}",
                          result.subtitle_cues_written, options.subtitle_page,
                          result.subtitle_path);
  }

  if (stats.lines_seen() > 0) {
    report += "\n\n" + stats.summary();
  }

  report += "\n\n";
  report += options.squash_repeated_rows
                ? squash_stats.summary()
                : "Teletext squashing: disabled; packets written as recovered";
  return report;
}

void TeletextSinkStageDeps::write_report(const TeletextSinkOptions& options,
                                         TeletextSinkResult& result) const {
  if (!options.write_report || stage_services_ == nullptr) {
    return;
  }
  // Sits beside the packet stream under its full name — mydata.t42.txt — so
  // it cannot collide with the .srt, or with a second export writing a
  // differently-named stream into the same directory.
  const std::string path = result.output_path + ".txt";
  std::shared_ptr<IFileWriterUint8> report_writer =
      stage_services_->create_buffered_file_writer_uint8(kWriterBufferBytes);
  if (!report_writer || !report_writer->open(path)) {
    ORC_LOG_WARN("TeletextSinkDeps: could not open report file: {}", path);
    return;
  }
  const std::string text = result.report + "\n";
  report_writer->write(reinterpret_cast<const uint8_t*>(text.data()),
                       text.size());
  report_writer->close();
  result.report_path = path;
}

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
      !options.tolerant_framing && options.require_valid_mrag &&
      options.parity_repair && options.detector == TeletextDetector::kAuto;

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
    slicer_options.parity_repair = options.parity_repair;
    slicer_options.detector = options.detector;
    // EBU Tech. 3280-E §1.1.1 Table 1: 4FSC PAL sample rate; bit rate fixed
    // at 444 × fH by ETSI EN 300 706 §5.3 (TeletextSlicer default).
    slicer.emplace(kPalSampleRate, kTeletextBitRate, slicer_options);
  }

  // Recovery diagnostics. Accumulated on both paths: where this stage slices,
  // from the full line results; where it reads the observer's stored
  // observations, from the packets those carry (the observer's own per-field
  // profile says how each field went, and this says how the run went).
  TeletextRecoveryStats stats;
  // What combining repeated rows changed, accumulated in the rewrite pass.
  TeletextSquashStats squash_stats;

  // Squashing needs every copy of a row before it can combine them, so the
  // recovered stream is held and rewritten in a second pass. Without it the
  // stream is written straight out and subtitles decode inline, as before.
  const bool squash = options.squash_repeated_rows;
  // The rewrite pass asks about runs of a page that were transmitted long
  // before the packet it is rewriting, so the squasher's page bound has to
  // hold every run of the recording rather than the recently-used working set
  // a live previewer needs. Erases multiply runs (see TeletextPageKey), so a
  // carousel that erases as it re-sends can reach several thousand. A run
  // evicted before the rewrite reaches it simply goes uncorrected — the vote
  // falls back to the single copy re-fed for it — so the cost of the bound is
  // lost correction, never wrong output. Proportionate to the packet stream
  // this pass already holds in memory.
  TeletextRowSquasher::Options squasher_options;
  squasher_options.max_pages = kSquashPageRunBound;
  TeletextRowSquasher squasher(squasher_options);
  std::vector<StreamEntry> stream;
  std::vector<QuantizedPacketConfidence> confidence_pool;
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

  // Observation namespace and per-line keys, built once: the loop below runs
  // per candidate line of every field of the recording, and building the key
  // strings there would be tens of millions of small allocations.
  const std::string teletext_namespace = "teletext";
  std::vector<std::string> t42_keys;
  if (!slicer) {
    t42_keys.reserve(static_cast<size_t>(options.last_field_line -
                                         options.first_field_line) +
                     1);
    for (int32_t field_line = options.first_field_line;
         field_line <= options.last_field_line; ++field_line) {
      t42_keys.push_back("t42_" + std::to_string(field_line));
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

  // A cancelled run still recovered whatever it got to, and that is exactly
  // when a reader wants to know how it was going. The report file is not
  // written for one: it describes a run that did not finish, and the option
  // asks for a record of the export beside the export.
  const auto report_partial_run = [&](TeletextSinkResult& partial) {
    partial.report =
        build_report(options, partial, total_frames, stats, squash_stats);
  };

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
        report_partial_run(result);
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
          // Per-byte confidence, where whichever path produced the packet
          // could measure it (see orc/support/teletext_slicer.h).
          bool has_confidence = false;
          TeletextPacketConfidence confidence{};

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
                stats.add_line(field_line, sliced);
                if (sliced.valid) {
                  packet = sliced.bytes;
                  has_confidence = sliced.has_byte_confidence;
                  confidence = sliced.byte_confidence;
                  result.bytes_repaired +=
                      static_cast<uint64_t>(sliced.repaired_bytes);
                }
              }
            }
          } else {
            const auto obs = observation_context.get(
                field_id, teletext_namespace,
                t42_keys[static_cast<size_t>(field_line -
                                             options.first_field_line)]);
            if (obs && std::holds_alternative<std::string>(*obs)) {
              const auto observed =
                  teletext_hex_to_observed_packet(std::get<std::string>(*obs));
              // 625-line packets only. The observer also records the 34-byte
              // packet of the 525-line service (ITU-R BT.653 Table 1b), whose
              // 32-byte rows this stage's .t42 output, page decoding and
              // squashing are all not written for; taking one here would emit
              // eight bytes per packet that were never transmitted.
              if (observed.has_value() &&
                  observed->byte_count == kTeletextPacketBytes) {
                packet = observed->bytes;
                has_confidence = observed->has_confidence;
                confidence = observed->confidence;
              }
            }
            stats.add_observed_line(field_line,
                                    packet.has_value() ? &*packet : nullptr,
                                    has_confidence ? &confidence : nullptr);
          }

          // Cue and squash timing is relative to the start of the export
          // range.
          const int64_t relative_field_index =
              static_cast<int64_t>(frame_id - frame_rng.first) * 2 + field_idx;

          if (packet.has_value()) {
            ++result.packets_written;
            field_has_data = true;
            if (squash) {
              // Quantise the confidence for the pool, then vote on the
              // dequantised values in both passes so the rewrite re-feeds
              // exactly the weights this pass used.
              uint32_t confidence_slot = 0;
              if (has_confidence) {
                QuantizedPacketConfidence quantized;
                for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
                  quantized[i] = static_cast<uint8_t>(std::lround(
                      std::clamp(confidence[i], 0.0F, 1.0F) * 255.0F));
                  confidence[i] =
                      static_cast<float>(quantized[i]) * (1.0F / 255.0F);
                }
                confidence_slot = static_cast<uint32_t>(confidence_pool.size());
                confidence_pool.push_back(quantized);
              }
              // The stream index is the copy's identity for the squasher, so
              // the rewrite pass can re-feed it without counting it twice.
              squash_pass->process_packet(
                  *packet, relative_field_index,
                  static_cast<int64_t>(stream.size()),
                  has_confidence ? &confidence : nullptr);
              stream.push_back(StreamEntry{*packet, relative_field_index, false,
                                           has_confidence, confidence_slot});
            } else {
              const TeletextPacketConfidence* weights =
                  has_confidence ? &confidence : nullptr;
              writer->write(packet->data(), packet->size());
              if (display_row_of(*packet) != 0) {
                squash_stats.add_written_row(display_bytes_of(*packet));
              }
              if (page_decoder.has_value()) {
                page_decoder->process_packet(*packet, relative_field_index,
                                             TeletextPageDecoder::kAutoSource,
                                             weights);
              }
            }
          } else if (options.keep_empty_packets) {
            ++result.packets_written;
            if (squash) {
              stream.push_back(StreamEntry{kEmptyPacket, relative_field_index,
                                           true, false, 0});
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
          squash_stats.set_page_runs(squasher.page_count());
          report_partial_run(result);
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
        TeletextPacketConfidence entry_confidence{};
        if (entry.has_confidence) {
          const QuantizedPacketConfidence& quantized =
              confidence_pool[entry.confidence_slot];
          for (size_t i = 0; i < kTeletextPacketBytes; ++i) {
            entry_confidence[i] =
                static_cast<float>(quantized[i]) * (1.0F / 255.0F);
          }
        }
        attributor->process_packet(
            entry.bytes, entry.field_index, static_cast<int64_t>(index),
            entry.has_confidence ? &entry_confidence : nullptr);
        const auto& attribution = attributor->last_row_attribution();
        if (attribution.has_value()) {
          const int row = attributor->last_row_number();
          const auto squashed = squasher.squashed_row(*attribution, row);
          if (squashed.has_value()) {
            squash_stats.add_row(display_bytes_of(entry.bytes), *squashed,
                                 squasher.copy_count(*attribution, row));
            if (!std::equal(squashed->begin(), squashed->end(),
                            out.begin() + 2)) {
              std::copy(squashed->begin(), squashed->end(), out.begin() + 2);
              ++result.packets_corrected;
            }
          }
        } else if (display_row_of(entry.bytes) != 0) {
          // A display row that belonged to no open page is written as it was
          // recovered. It carries characters the reader will see, so its
          // damage belongs in the loss figure even though no vote reached it.
          squash_stats.add_written_row(display_bytes_of(entry.bytes));
        }
        writer->write(out.data(), out.size());
      }
      squash_stats.set_page_runs(squasher.page_count());
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

    result.characters_written = squash_stats.bytes_total();
    result.characters_damaged = squash_stats.parity_failures_after();

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

    result.report =
        build_report(options, result, total_frames, stats, squash_stats);
    write_report(options, result);
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
