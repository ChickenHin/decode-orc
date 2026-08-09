/*
 * File:        vbi_source_format.cpp
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Generic raw VBI container descriptor and its named presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_source_format.h"

#include <orc/stage/cvbs_signal_constants.h>

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

// Fraction of the sampled records a 625-line card capture must lock before its
// fit is trusted.  A PAL broadcaster has the whole of the WST line list to use
// and the material in circulation uses most of it, so a quarter is a low bar
// that only a wrong configuration falls under.
constexpr double kBt8x8PALMinimumAcceptanceFraction = 0.25;

// The same figure for a SECAM source, where it cannot be anything like as high
// (see make_bt8x8_secam below for why).  It is deliberately not zero: the fit
// still has to lock on something repeatable, and the absolute floor of eight
// accepted records, the spread limit and the drift check are what actually
// catch a wrong container, rate or system.  This one figure only asks that at
// least one record of every other frame carried the data service.
constexpr double kBt8x8SECAMMinimumAcceptanceFraction = 0.03;

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
  format.calibration.minimum_acceptance_fraction =
      kBt8x8PALMinimumAcceptanceFraction;
  return format;
}

// A bt8x8 dump of a SECAM source.  The container is the PAL one byte for byte:
// the driver's SECAM entry (Linux bttv_tvnorms[]) carries the same 35 468 950
// Hz sampling clock, the same vbipack of 255 and the same vbistart of {7, 320}
// as its PAL entry, so the record stride, the field stride, the frame trailer
// and the lines the sixteen records cover are all unchanged.  Post-decode SECAM
// is a 625-line signal carrying the same World System Teletext, so the output
// geometry and the data service are the PAL ones too.
//
// What differs is how much of the line list can carry teletext.  A SECAM
// transmission with vertical colour identification puts the identification
// signal — a continuous 4,4 MHz burst, the "green bottles" — on frame lines
// 8-15 and 321-328, which is records 1 to 8 of each stored field.  Half of the
// sixteen records are therefore spoken for before any teletext is inserted, and
// broadcasters using those idents typically left the remainder to test signals
// and a very few teletext lines: the Russian tape this preset was measured on
// carries teletext on records 12-14 only (frame lines 19-21 and 332-334), with
// a VITS and a white bar on records 9-11.
//
// That makes the acceptance fraction — the share of sampled records the clock
// run-in is found on — the one calibration threshold that cannot be shared with
// PAL.  Three lines of sixteen is 19% before a single line is lost to tape, and
// the PAL preset's quarter rejects the fit outright even though it is a good
// one: on the reference tape it lands on 57 records with a spread of under two
// samples and no drift, which is a healthy fit by every other measure the
// stage has.
VBISourceFormat make_bt8x8_secam() {
  VBISourceFormat format = make_bt8x8_pal();
  format.calibration.minimum_acceptance_fraction =
      kBt8x8SECAMMinimumAcceptanceFraction;
  return format;
}

// Cropped VBI-only .tbc captures: the first sixteen line records of each field
// of a decoded luma .tbc, at the decoder's own 4 x fsc rate.  The rate is taken
// from the CVBS constants rather than written out, so that it is bit-identical
// to the output lattice's and the resampling ratio is exactly one.
constexpr double kTBCVBINTSCSampleRateHz = kNtscSampleRate;

// A .tbc line is the whole line: 910 samples with no padding.
constexpr uint32_t kTBCVBINTSCRecordStride = 910;

// Record 0 of the sixteen is broadcast field line 9 — the last post-equalising
// line, which carries no data service — so the standard's twelve teletext lines
// (field lines 10-21 of each field) are records 1 to 12, and records 13-15 are
// active picture.
//
// Measured on the circulating captures rather than assumed: record 0 carries a
// 2.3 us equalising pulse where every other record carries a 4.7 us line sync,
// and record 12 carries the line 21 closed caption run-in at its standard
// 10.5 us from 0H.
constexpr uint32_t kTBCVBINTSCFirstDataRecord = 1;
constexpr uint32_t kTBCVBINTSCLastDataRecord = 12;

// The first stored field of every circulating capture is television field 1,
// measured two ways on all five of them: record 0 carries two equalising pulses
// on field 1 against one on field 2, and on a captioned recording record 12 is
// line 21, so the field carrying the caption run-in is field 1.  A crop whose
// producer started on the other field would need its own preset.
constexpr uint32_t kFirstStoredField = 1;

VBISourceFormat make_tbc_vbi_ntsc(VBITeletextSystem tt_system) {
  VBISourceFormat format;
  format.sample_rate_hz = kTBCVBINTSCSampleRateHz;
  format.line_length = kTBCVBINTSCRecordStride;
  format.valid_samples = kTBCVBINTSCRecordStride;
  format.sample_format = VBISampleFormat::kU16LE;
  format.field_lines = 16;
  format.field_range =
      VBIFieldRange{kTBCVBINTSCFirstDataRecord, kTBCVBINTSCLastDataRecord};
  format.frame_trailer_bytes = 0;
  format.frame_trailer_is_counter = false;

  // Family B: the upstream time-base correction already put sample 0 of every
  // record at that line's 0H, so the offset is known to be zero and must never
  // be fitted (design §5.3.3).  Validation enforces that.
  format.capture_offset_samples = 0.0;
  format.capture_offset_is_auto = false;

  format.first_field = kFirstStoredField;
  format.tv_system = VBITVSystem::kNTSC;
  format.tt_system = tt_system;
  format.family = VBISourceFamily::kTBCDerived;
  return format;
}

const std::vector<PresetEntry>& presets() {
  // Every preset is a complete configuration: the container, the data service
  // it carries and the policies that follow from both.  There is nothing left
  // for the user to fill in, which is why there is no "custom" entry — a format
  // nobody has measured cannot be described correctly by guessing at fields,
  // and adding one that has been measured is a single entry here.
  //
  // The names are what the user reads and what the project file stores, so they
  // say where the capture came from, what its samples are, and which service it
  // carries; the geometry behind each is in the parameter description.
  static const std::vector<PresetEntry> kPresets = {
      {"bt8x8 card dump, 8-bit (WST)", make_bt8x8_pal()},
      {"bt8x8 card dump, 8-bit (WST, SECAM source)", make_bt8x8_secam()},
      {".tbc VBI crop, 16-bit (WST)",
       make_tbc_vbi_ntsc(VBITeletextSystem::kWST)},
      {".tbc VBI crop, 16-bit (NABTS)",
       make_tbc_vbi_ntsc(VBITeletextSystem::kNABTS)},
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

  // ITU-R BT.653 §2: both 525-line teletext systems in scope — System B, the
  // 525-line variant of WST, and System C (NABTS) — occupy broadcast frame
  // lines 10-21 and 273-284, twelve records per field.  The systems differ in
  // framing code and packet length, not in which lines they sit on.
  if (tv_system == VBITVSystem::kNTSC || tv_system == VBITVSystem::kPALM) {
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

std::vector<std::string> vbi_source_preset_names(VBITVSystem tv_system) {
  std::vector<std::string> names;
  names.reserve(presets().size());
  for (const auto& entry : presets()) {
    if (entry.format.tv_system == tv_system) {
      names.emplace_back(entry.name);
    }
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

}  // namespace orc
