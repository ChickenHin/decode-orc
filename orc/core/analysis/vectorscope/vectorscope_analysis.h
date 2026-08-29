/*
 * File:        vectorscope_analysis.h
 * Module:      orc-core
 * Purpose:     Vectorscope analysis tool for chroma decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H
#define ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H

#include <orc/stage/frame_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/preview/orc_preview_carriers.h>

#include <memory>
#include <optional>

#include "../analysis_tool.h"
#include "vectorscope_data.h"

namespace orc {

/**
 * @brief Vectorscope visualization tool for chroma decoder output
 *
 * Extracts U/V chroma samples from the decoded output of the comb decoder for
 * real-time vectorscope display.
 */
class VectorscopeAnalysisTool : public AnalysisTool {
 public:
  std::string id() const override;
  std::string name() const override;
  std::string description() const override;
  std::string category() const override;

  std::vector<ParameterDescriptor> parameters() const override;
  bool canAnalyze(AnalysisSourceType source_type) const override;
  bool isApplicableToStage(const std::string& stage_name) const override;

  AnalysisResult analyze(const AnalysisContext& ctx,
                         AnalysisProgress* progress) override;

  bool canApplyToGraph() const override;
  bool applyToGraph(AnalysisResult& result, const Project& project,
                    NodeID node_id) override;

  int estimateDurationSeconds(const AnalysisContext& ctx) const override;

  /**
   * @brief Extract vectorscope data from a colour preview carrier.
   *
   * Uses the decoded U/V planes already present in the carrier.
   *
   * |active_area_only| limits sampling to the carrier's active picture window
   * in both axes; cleared, the whole decoded frame is plotted.
   *
   * |first_line| / |last_line| are an inclusive interlaced frame-line range
   * (0-based), the same numbering the composite acquisition selects and
   * reports lines in, so a range means the same thing in either scope.
   * |last_line| == 0 means "to the last line of the frame"; the range
   * intersects with the active-picture restriction rather than overriding it.
   */
  static VectorscopeData extractFromColourFrameCarrier(
      const ColourFrameCarrier& carrier, uint64_t field_number,
      uint32_t subsample = 1, bool active_area_only = true,
      uint32_t first_line = 0, uint32_t last_line = 0);
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H
