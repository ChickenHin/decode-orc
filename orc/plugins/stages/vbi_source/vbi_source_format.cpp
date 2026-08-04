/*
 * File:        vbi_source_format.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Generic raw VBI container descriptor and its named presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_format.h"

namespace orc {

namespace {

// ---------------------------------------------------------------------------
// Preset data (design §3.2 / §3.3).
//
// Presets are pure data: a new format is a new entry in kPresets below and
// nothing else.  The reader, the validator and the stage all work from the
// expanded descriptor and know nothing about preset names.
// ---------------------------------------------------------------------------

// Linux bttv driver, bttv_tvnorms[]: 625-line sampling clock.
constexpr double kBt8x8PALSampleRateHz = 35468950.0;  // 8 x fsc PAL

// bttvp.h VBI_BPL: the 2048 figure is a buffer-size constant retained for
// compatibility with earlier driver versions, not a timing one.
constexpr uint32_t kBt8x8RecordStride = 2048;

// The hardware writes 1024 + vbipack * 4 samples per line; PAL uses
// vbipack = 255, leaving four bytes of padding per record.
constexpr uint32_t kBt8x8PALValidSamples = 2044;

// The final four bytes of every 65 536-byte frame are a u32 frame sequence
// number written by read().  On PAL they occupy exactly the padding of the
// last record.
constexpr uint32_t kBt8x8FrameCounterBytes = 4;

// "Experimentally, the value is measured to be about 244" — Linux
// drivers/media/pci/bt8xx/bttv-vbi.c.  Explicitly unreliable folklore, so it
// is only ever a starting hint for CRI/FRC calibration (design §5.3.3).
constexpr double kBt8x8CaptureOffsetSamples = 244.0;

// How far either side of the folkloric offset the run-in is looked for, in
// 8 x fsc samples.  About 1.4 us, which comfortably covers the spread of the
// vhs-teletext search windows the offsets in design §5.3.3 were inferred from
// while staying well inside the back porch ahead of the data.
constexpr double kBt8x8PALSearchToleranceSamples = 48.0;

// Scatter of the run-in position this card's captures are allowed before the
// fit is rejected, in 8 x fsc samples.  Eight samples is 226 ns, about one and
// a half teletext bit periods: enough for the line-to-line timing of a tape
// played into a capture card, and far too little to hide a wrong sampling
// rate, which walks the position clean out of the search window.
constexpr double kBt8x8PALMaximumSpreadSamples = 8.0;

struct PresetEntry {
  const char* name;
  VBISourceFormat format;
};

VBISourceFormat make_bt8x8_pal() {
  VBISourceFormat format;
  format.sample_rate_hz = kBt8x8PALSampleRateHz;
  format.line_length = kBt8x8RecordStride;
  format.valid_samples = kBt8x8PALValidSamples;
  format.sample_format = VBISampleFormat::kU8;
  format.field_lines = 16;
  format.field_range = VBIFieldRange{0, 15};
  format.frame_trailer_bytes = kBt8x8FrameCounterBytes;
  format.frame_trailer_is_counter = true;
  format.capture_offset_samples = kBt8x8CaptureOffsetSamples;
  format.capture_offset_is_auto = true;
  format.first_field = 1;
  format.tv_system = VBITVSystem::kPAL;
  format.tt_system = VBITeletextSystem::kWST;
  format.family = VBISourceFamily::kCardCapture;

  // A bt8x8 card is how tape and off-air material is captured, so this preset
  // has to accept what such a source really looks like.  The card's horizontal
  // phase-locked loop line-locks the sampling clock, but residual jitter,
  // velocity error and the settling after head switching remain, and the
  // teletext lines sit inside that recovery window (design §5.3.6).  The
  // reference capture measures at about four samples of scatter with a global
  // offset that is nevertheless stable to well under a sample, which is
  // exactly the asymmetry the design describes: this stage needs a global
  // offset and the downstream slicer does the per-line lock.
  //
  // The tight figure is left at the design §5.3.4 value, so a genuinely clean
  // capture is still reported as such rather than being flattered by a
  // threshold set for tape.
  format.calibration.search_tolerance_samples = kBt8x8PALSearchToleranceSamples;
  format.calibration.tight_spread_samples = 0.5;
  format.calibration.maximum_spread_samples = kBt8x8PALMaximumSpreadSamples;
  format.calibration.maximum_drift_samples = kBt8x8PALMaximumSpreadSamples;
  return format;
}

// The "custom" expansion is deliberately unconfigured: every container field
// comes from the project instead.  Leaving the geometry at zero means an
// incompletely specified custom format is rejected by validation rather than
// silently behaving like some other format.
VBISourceFormat make_custom() {
  VBISourceFormat format;
  format.sample_format = VBISampleFormat::kU8;
  format.first_field = 1;
  format.tv_system = VBITVSystem::kPAL;
  format.tt_system = VBITeletextSystem::kWST;
  format.family = VBISourceFamily::kCardCapture;
  return format;
}

const std::vector<PresetEntry>& presets() {
  static const std::vector<PresetEntry> kPresets = {
      {"bt8x8-pal", make_bt8x8_pal()},
      {"custom", make_custom()},
  };
  return kPresets;
}

}  // namespace

uint32_t standard_teletext_lines_per_field(VBITVSystem tv_system,
                                           VBITeletextSystem tt_system) {
  // ETSI EN 300 706 §4.1: WST occupies broadcast frame lines 7-22 and
  // 320-335 on 625-line systems — sixteen records per field.
  if (tv_system == VBITVSystem::kPAL && tt_system == VBITeletextSystem::kWST) {
    return 16;
  }

  // ITU-R BT.653 System C: NABTS occupies broadcast frame lines 10-21 and
  // 273-284 on 525-line systems — twelve records per field.
  if ((tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) &&
      tt_system == VBITeletextSystem::kNABTS) {
    return 12;
  }

  // Any other pairing has no defined line list; callers report it as an
  // unsupported configuration rather than guessing one.
  return 0;
}

std::vector<std::string> vbi_source_preset_names() {
  std::vector<std::string> names;
  names.reserve(presets().size());
  for (const auto& entry : presets()) {
    names.emplace_back(entry.name);
  }
  return names;
}

bool expand_vbi_source_preset(const std::string& preset_name,
                              VBISourceFormat& out_format,
                              std::string& error_message) {
  for (const auto& entry : presets()) {
    if (preset_name == entry.name) {
      out_format = entry.format;
      return true;
    }
  }

  std::string known;
  for (const auto& entry : presets()) {
    if (!known.empty()) {
      known += ", ";
    }
    known += entry.name;
  }
  error_message = "Unknown VBI source format preset '" + preset_name +
                  "'. Known presets: " + known + ".";
  return false;
}

std::string to_string(VBISampleFormat sample_format) {
  switch (sample_format) {
    case VBISampleFormat::kU8:
      return "u8";
    case VBISampleFormat::kU16LE:
      return "u16le";
    case VBISampleFormat::kS16LE:
      return "s16le";
  }
  return "u8";
}

bool parse_vbi_sample_format(const std::string& name,
                             VBISampleFormat& out_format) {
  if (name == "u8") {
    out_format = VBISampleFormat::kU8;
    return true;
  }
  if (name == "u16le") {
    out_format = VBISampleFormat::kU16LE;
    return true;
  }
  if (name == "s16le") {
    out_format = VBISampleFormat::kS16LE;
    return true;
  }
  return false;
}

}  // namespace orc
