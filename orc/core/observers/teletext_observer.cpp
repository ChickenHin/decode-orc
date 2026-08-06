/*
 * File:        teletext_observer.cpp
 * Module:      orc-core
 * Purpose:     WST teletext observer implementation
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

// Candidate VBI window as 0-based field lines, identical in both fields of a
// frame.
//
// 625 lines — ETSI EN 300 706 §4.1: broadcast lines 6 to 22 (field 1) and 318
// to 335 (field 2) may carry teletext; in the frame-flat buffer those are
// 0-based field lines 5-21 of each field (field 2 offset by kPalField1Lines).
constexpr size_t kFirstCandidateFieldLine625 = 5;
constexpr size_t kLastCandidateFieldLine625 = 21;

// 525 lines — ITU-R BT.653 §2: broadcast lines 10 to 21 (field 1) and 273 to
// 284 (field 2), which are 0-based field lines 9-20 of each field. The same
// list the VBI source stage places to (vbi_line_mapping.cpp), so a capture
// that stage wrote and a TBC of the same broadcast are read on one window.
//
// Field line 20 is broadcast line 21, which on a captioned NTSC recording
// carries EIA-608 rather than teletext. The two services are alternatives, not
// neighbours, and a caption line has no framing code, so it is examined and
// rejected rather than excluded here.
constexpr size_t kFirstCandidateFieldLine525 = 9;
constexpr size_t kLastCandidateFieldLine525 = 20;

// The widest window, which is what the observation-key table has to span.
constexpr size_t kFirstCandidateFieldLine = kFirstCandidateFieldLine625;
constexpr size_t kLastCandidateFieldLine = kLastCandidateFieldLine625;
static_assert(kFirstCandidateFieldLine525 >= kFirstCandidateFieldLine &&
                  kLastCandidateFieldLine525 <= kLastCandidateFieldLine,
              "525-line candidates must be a subset of the declared keys");

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

// Which teletext system a video system carries, and where in the field to look
// for it.
struct SystemProfile {
  TeletextSystem teletext_system = TeletextSystem::kWst625;
  size_t first_field_line = kFirstCandidateFieldLine625;
  size_t last_field_line = kLastCandidateFieldLine625;
};

SystemProfile profile_for(VideoSystem system) {
  if (system == VideoSystem::PAL) {
    return SystemProfile{TeletextSystem::kWst625, kFirstCandidateFieldLine625,
                         kLastCandidateFieldLine625};
  }
  // NTSC and PAL_M share the 525-line structure and therefore the service
  // (ITU-R BT.653 Table 1b); only the 4FSC sample rate differs between them,
  // which is a property of the slicer rather than of this table.
  return SystemProfile{TeletextSystem::kWst525, kFirstCandidateFieldLine525,
                       kLastCandidateFieldLine525};
}

}  // namespace

TeletextObserver::TeletextObserver()
    // EBU Tech. 3280-E §1.1.1 Table 1 / SMPTE 244M-2003 §4.1 / ITU-R BT.1700-1
    // Annex 1 Part B: the 4FSC sample rate of each system. The bit rate comes
    // from the teletext system (ITU-R BT.653 Tables 1a and 1b).
    : slicer_pal_(kPalSampleRate, TeletextSystem::kWst625,
                  observer_slicer_options()),
      slicer_ntsc_(kNtscSampleRate, TeletextSystem::kWst525,
                   observer_slicer_options()),
      slicer_palm_(kPalMSampleRate, TeletextSystem::kWst525,
                   observer_slicer_options()) {}

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

  // Systems with no defined WST service produce no observations (see
  // applies_to()); callers normally skip the observer entirely, this
  // early-return is defence in depth.
  if (!applies_to(vp)) {
    return;
  }

  const SystemProfile profile = profile_for(vp.system);
  const TeletextSlicer& slicer = (vp.system == VideoSystem::PAL) ? slicer_pal_
                                 : (vp.system == VideoSystem::PAL_M)
                                     ? slicer_palm_
                                     : slicer_ntsc_;

  // Levels from the source, with the spec constants as fallback. The data '0'
  // reference is black in both systems (ETSI EN 300 706 §5.2, ITU-R BT.653
  // Table 1b); on a 525-line system the transmitted '0' actually sits at
  // blanking, below the 7,5 IRE setup black, which makes the amplitude gate
  // derived from these levels stricter than the standard requires rather than
  // looser — measured against a real burst it still clears by a factor of two.
  const int16_t default_black = static_cast<int16_t>(
      vp.system == VideoSystem::PAL ? kPalBlack : kNtscBlack);
  const int16_t default_white = static_cast<int16_t>(
      vp.system == VideoSystem::PAL ? kPalWhite : kNtscWhite);
  const int16_t black_level = static_cast<int16_t>(
      vp.black_level >= 0 ? vp.black_level : default_black);
  const int16_t white_level = static_cast<int16_t>(
      vp.white_level >= 0 ? vp.white_level : default_white);

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
    for (size_t field_line = profile.first_field_line;
         field_line <= profile.last_field_line; ++field_line) {
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
          slicer.slice(line_data, sample_count, black_level, white_level);
      stats.add_line(static_cast<int>(field_line), sliced);
      if (!sliced.valid) {
        continue;
      }

      // The recovered bytes, and — where the detector could measure it — how
      // sure it was of each of them, so a consumer combining repeated copies of
      // a row can weight this one (orc/support/teletext_row_squasher.h). The
      // length of the string names the packet length as well as whether the
      // confidence suffix is there; the suffix is optional at both ends, so
      // observations stored by earlier builds carry none and stay perfectly
      // usable.
      context.set(
          derived_fid, "teletext", t42_key(field_line),
          sliced.has_byte_confidence
              ? teletext_packet_to_hex(sliced.bytes, sliced.byte_confidence,
                                       sliced.packet_bytes)
              : teletext_packet_to_hex(sliced.bytes, sliced.packet_bytes));
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
        "Recovered T42 packet for 0-based field line " +
            std::to_string(field_line) +
            ": two hex chars per byte (42 bytes on 625-line systems, 34 on "
            "525-line ones), optionally followed by one confidence digit per "
            "byte",
        true);
  }
  return keys;
}

}  // namespace orc
