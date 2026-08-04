/*
 * File:        vbi_level_mapper.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Maps card-capture sample levels into the CVBS amplitude domain
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_LEVEL_MAPPER_H
#define ORC_VBI_LEVEL_MAPPER_H

#include <cstdint>
#include <string>
#include <vector>

#include "vbi_line_reader.h"
#include "vbi_output_levels.h"
#include "vbi_source_format.h"
#include "vbi_teletext_service.h"

namespace orc {

// How the mapper decides which levels to apply to a line.
enum class VBILevelMode {
  // Each line is normalised by its own estimate.  Follows fast gain changes
  // but carries per-line estimation noise into the output.
  kPerLine,

  // Levels are the median over the lines of a stored frame, with a line's own
  // estimate used only where it deviates significantly from that median.
  kRolling,

  // Configured levels applied to every line; nothing is estimated.
  kFixed,
};

// Level estimation and mapping policy.
struct VBILevelMapperConfig {
  VBILevelMode mode = VBILevelMode::kPerLine;

  // Margin held clear of the nominal clock run-in when reading the quiet
  // region, in source samples.  Three samples is about 85 ns at 8 x fsc and
  // gives the bt8x8 PAL back-porch window of samples 0-117 in design §5.4.
  double quiet_guard_samples = 3.0;

  // Smallest data amplitude, in source counts, that can be a measurement
  // rather than noise.  A line below it carries no data service, so its own
  // estimate is discarded.
  double minimum_amplitude_counts = 8.0;

  // A line whose amplitude falls below this fraction of the frame's median
  // amplitude is likewise treated as carrying no data service.
  double minimum_amplitude_fraction = 0.25;

  // Relative departure from the frame's median amplitude at which kRolling
  // stops holding a line at the median and applies the line's own estimate.
  double rolling_deviation_fraction = 0.25;

  // Source-domain levels used by kFixed.
  double fixed_logic0 = 0.0;
  double fixed_logic1 = 0.0;
};

// Sample windows within a stored line record from which levels are read.
// Ranges are half-open, in samples from the start of the record.
struct VBIRecordWindows {
  // Quiet region between the start of the record and the clock run-in: the
  // logic 0 reference.
  uint32_t quiet_begin = 0;
  uint32_t quiet_end = 0;

  // The clock run-in.
  uint32_t cri_begin = 0;
  uint32_t cri_end = 0;

  // Centre bit of the framing code's leading run of ones: the logic 1
  // reference that survives a band-limited channel.
  uint32_t frc_reference_begin = 0;
  uint32_t frc_reference_end = 0;
};

// Levels read from one line record, all in source-domain counts.
struct VBILineLevels {
  double logic0 = 0.0;
  double logic1 = 0.0;

  // The two logic 1 candidates, kept for diagnostics: the larger becomes
  // logic1 above (design §5.4).
  double cri_logic1 = 0.0;
  double frc_logic1 = 0.0;

  // Peak-to-peak swing of the clock run-in.
  double cri_peak_to_peak = 0.0;

  // Clock run-in swing as a fraction of the full data amplitude — a direct
  // per-line estimate of how band-limited the source is.  About 1 on a clean
  // line; the VHS waveform measured in design §5.3.6 gives 0.055.
  double cri_frc_ratio = 0.0;

  // False when the line carries no measurable data service.
  bool usable = false;

  double amplitude() const { return logic1 - logic0; }
};

// One mapped line record.
struct VBIMappedLine {
  uint64_t frame_index = 0;
  uint32_t field_index = 0;
  uint32_t record_index = 0;

  // Levels actually applied, which are not the line's own when a frame level
  // was substituted for it.
  VBILineLevels applied_levels;

  // The line's own measurement, whether or not it was applied.
  VBILineLevels measured_levels;

  // False when the line was held at its own estimate's replacement.
  bool used_own_estimate = false;

  // False when no level reference could be established at all, in which case
  // the line is emitted as blanking.
  bool levels_established = false;

  // Sample values in the output amplitude domain, in record order, padding
  // already excluded by the reader.  Not clamped: clamping is the last step
  // of the sample path and happens after resampling (design §2.2).
  std::vector<double> samples;
};

// Derive the level-reading windows of a source format.
VBIRecordWindows vbi_record_windows(const VBISourceFormat& format,
                                    const VBITeletextService& service,
                                    double quiet_guard_samples);

// Read the levels of one line record.
VBILineLevels estimate_vbi_line_levels(const std::vector<double>& samples,
                                       const VBIRecordWindows& windows,
                                       double minimum_amplitude_counts);

// Map a source-domain sample through a line's levels into the output domain.
double map_vbi_sample(double sample, const VBILineLevels& levels,
                      const VBIOutputLevels& output_levels);

// Maps u8 card captures — the sources whose levels are relative and affected
// by automatic gain control — into the output amplitude domain.
//
// Levels are estimated per line from the record's own structure: logic 0 from
// the quiet region ahead of the clock run-in, and logic 1 from the larger of
// the run-in's peaks and the framing code's leading run of ones.  Taking the
// larger is what stops a band-limited source under-scaling: on the VHS
// waveform measured in design §5.3.6 the run-in has collapsed to a few per
// cent of the data amplitude, and normalising to it would produce output at a
// fraction of the correct amplitude.
//
// The mapping is linear and nothing else is done to the samples.  A
// deconvolving slicer downstream recovers data by matching the blurred
// waveform it is given, so any sharpening, slicing or re-quantisation here
// would destroy the information it depends on (design §5.3.6).
//
// The mapper works on a whole stored frame at a time.  That is what makes
// kRolling's median deterministic: frames are synthesised lazily and in
// whatever order they are asked for, so a window carried across frames would
// make a frame's output depend on which frames were read before it.
//
// Thread-compatible: instances hold configuration only.
class VBILevelMapper {
 public:
  VBILevelMapper(VBISourceFormat format, VBITeletextService service,
                 VBIOutputLevels output_levels, VBILevelMapperConfig config);

  const VBIRecordWindows& windows() const { return windows_; }

  const VBILevelMapperConfig& config() const { return config_; }

  // Map every record of one stored frame.
  void map_frame(const std::vector<VBILineRecord>& records,
                 std::vector<VBIMappedLine>& out_lines) const;

 private:
  // Levels held in common by a frame, from which lines are mapped when their
  // own measurement is not used.
  VBILineLevels frame_levels(const std::vector<VBILineLevels>& measured) const;

  VBISourceFormat format_;
  VBITeletextService service_;
  VBIOutputLevels output_levels_;
  VBILevelMapperConfig config_;
  VBIRecordWindows windows_;
};

}  // namespace orc

#endif  // ORC_VBI_LEVEL_MAPPER_H
