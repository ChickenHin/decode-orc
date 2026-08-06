/*
 * File:        teletext_observer.cpp
 * Module:      orc-core
 * Purpose:     PAL WST teletext observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/teletext_recovery_stats.h>
#include <orc/support/teletext_slicer.h>
#include <teletext_observer.h>

#include <array>
#include <string>
#include <vector>

namespace orc {

namespace {

// Candidate VBI window as 0-based field lines, identical in both fields.
// ETSI EN 300 706 §4.1: broadcast lines 6 to 22 (field 1) and 318 to 335
// (field 2) may carry teletext; in the frame-flat buffer these are 0-based
// field lines 5-21 of each field (field 2 offset by kPalField1Lines = 313).
constexpr size_t kFirstCandidateFieldLine = 5;
constexpr size_t kLastCandidateFieldLine = 21;

// Observation key for a candidate field line's recovered packet. Built once:
// process_frame() asks per recovered line of every field it observes.
const std::string& t42_key(size_t field_line) {
  static const auto keys = [] {
    std::array<std::string,
               kLastCandidateFieldLine - kFirstCandidateFieldLine + 1>
        built;
    for (size_t line = kFirstCandidateFieldLine;
         line <= kLastCandidateFieldLine; ++line) {
      built[line - kFirstCandidateFieldLine] = "t42_" + std::to_string(line);
    }
    return built;
  }();
  return keys[field_line - kFirstCandidateFieldLine];
}

// Observers take no configuration, so the one slicer setup here has to serve
// every source the host may be pointed at. kAuto does: the threshold detector
// runs first and a band-limited source (consumer VHS, where the clock run-in
// is attenuated below the detection threshold) falls back to MLSE. A line the
// threshold detector recovers never reaches the fallback, so disc and direct
// CVBS captures are unchanged in both results and cost.
//
// Parity repair is on for the same reason. It acts only on MLSE-detected
// packets in parity-coded rows — a byte that failed the odd parity of ETSI EN
// 300 706 §8.1 has its least-confident bit flipped — so a source the threshold
// detector handles is again untouched, while a tape gets back characters it
// would otherwise lose (on the reference captures, packets whose 40 data bytes
// all satisfy parity rise from 70 % to 88 %). What it costs is that a repair
// which guessed wrong can no longer be told from a byte that arrived intact;
// the repaired byte carries the low confidence of the bit that was flipped, so
// consumers weighting repeated copies of a row still prefer one that did.
TeletextSlicerOptions observer_slicer_options() {
  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kAuto;
  options.parity_repair = true;
  return options;
}

}  // namespace

TeletextObserver::TeletextObserver()
    // EBU Tech. 3280-E §1.1.1 Table 1: 4FSC PAL sample rate; the bit rate is
    // fixed at 444 × fH by ETSI EN 300 706 §5.3 (TeletextSlicer default).
    : slicer_(kPalSampleRate, kTeletextBitRate, observer_slicer_options()) {}

void TeletextObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("TeletextObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // Non-PAL systems produce no observations (see applies_to()); callers
  // normally skip the observer entirely, this early-return is defence in
  // depth.
  if (!applies_to(vp)) {
    return;
  }

  // Levels from the source, with the spec constants as fallback.
  const int16_t black_level =
      static_cast<int16_t>(vp.black_level >= 0 ? vp.black_level : kPalBlack);
  const int16_t white_level =
      static_cast<int16_t>(vp.white_level >= 0 ? vp.white_level : kPalWhite);

  const size_t f1_lines = field1_lines(vp.system);
  const size_t line_width = static_cast<size_t>(vp.frame_width_nominal);
  const size_t frame_height = static_cast<size_t>(vp.frame_height);

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    const FieldID derived_fid(frame_id * 2 + field_idx);
    const size_t line_offset = (field_idx == 0) ? 0 : f1_lines;

    // Recovery diagnostics for this field. The accumulator is local, so the
    // observer stays stateless (the sink's per-frame coverage skip relies on
    // that) and one field's profile never bleeds into the next.
    TeletextRecoveryStats stats;

    int32_t line_count = 0;
    for (size_t field_line = kFirstCandidateFieldLine;
         field_line <= kLastCandidateFieldLine; ++field_line) {
      const size_t flat_line = line_offset + field_line;
      if (flat_line >= frame_height) {
        continue;
      }

      // Luma-aware fetch: YC sources carry the data burst in the luma
      // channel; composite sources go through get_line_samples() so per-line
      // reads use the source stage's field buffering.
      const int16_t* line_data = nullptr;
      size_t sample_count = 0;
      std::vector<int16_t> line_copy;
      if (representation.has_separate_channels()) {
        line_data = representation.get_line_luma(frame_id, flat_line);
        sample_count = line_width;
      } else {
        line_copy = representation.get_line_samples(frame_id, flat_line);
        line_data = line_copy.data();
        sample_count = line_copy.size();
      }
      if (line_data == nullptr || sample_count == 0) {
        continue;
      }

      const TeletextLineResult sliced =
          slicer_.slice(line_data, sample_count, black_level, white_level);
      stats.add_line(static_cast<int>(field_line), sliced);
      if (!sliced.valid) {
        continue;
      }

      // The recovered bytes, and — where the detector could measure it — how
      // sure it was of each of them, so a consumer combining repeated copies of
      // a row can weight this one (orc/support/teletext_row_squasher.h). The
      // suffix is optional at both ends: observations stored by earlier builds
      // carry none and stay perfectly usable.
      context.set(
          derived_fid, "teletext", t42_key(field_line),
          sliced.has_byte_confidence
              ? teletext_packet_to_hex(sliced.bytes, sliced.byte_confidence)
              : teletext_packet_to_hex(sliced.bytes));
      ++line_count;
    }

    context.set(derived_fid, "teletext", "present", line_count > 0);
    context.set(derived_fid, "teletext", "line_count", line_count);

    // Recovery diagnostics for the field. Only fields that carried a data
    // burst have anything to say, and both texts are built only when the
    // logger will actually take them — every field of a full decode passes
    // here, so at debug level that is one line per field and the tables of the
    // full profile are left to trace.
    if (stats.lines_with_burst() > 0) {
      if (get_logger()->should_log(spdlog::level::trace)) {
        ORC_LOG_TRACE("TeletextObserver: Field {} recovery profile\n{}",
                      derived_fid.value(), stats.summary());
      } else if (get_logger()->should_log(spdlog::level::debug)) {
        ORC_LOG_DEBUG("TeletextObserver: Field {}: {}", derived_fid.value(),
                      stats.brief());
      }
    }
  }
}

std::vector<ObservationKey> TeletextObserver::get_provided_observations()
    const {
  std::vector<ObservationKey> keys;
  keys.reserve(2 + (kLastCandidateFieldLine - kFirstCandidateFieldLine + 1));
  keys.emplace_back("teletext", "present", ObservationType::BOOL,
                    "At least one valid teletext packet recovered", true);
  keys.emplace_back("teletext", "line_count", ObservationType::INT32,
                    "Number of candidate VBI lines that yielded packets", true);
  for (size_t field_line = kFirstCandidateFieldLine;
       field_line <= kLastCandidateFieldLine; ++field_line) {
    keys.emplace_back(
        "teletext", t42_key(field_line), ObservationType::STRING,
        "42 recovered T42 bytes (84 hex chars), optionally followed by 42 "
        "per-byte confidence digits, for 0-based field line " +
            std::to_string(field_line),
        true);
  }
  return keys;
}

}  // namespace orc
