/*
 * File:        disc_mapper_analysis.h
 * Module:      analysis
 * Purpose:     Disc mapper analysis tool: detects skipped, repeated, and
 * missing frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H
#define ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H

#include "../analysis_tool.h"

namespace orc {

/**
 * @brief Disc mapper analysis tool
 *
 * Analyzes source captures to detect skipped, repeated, and missing frames
 * that indicate laserdisc player tracking problems.
 */
class DiscMapperAnalysisTool : public AnalysisTool {
 public:
  std::string id() const override;
  std::string name() const override;
  std::string description() const override;
  std::string category() const override;

  std::vector<ParameterDescriptor> parameters() const override;
  bool canAnalyze(AnalysisSourceType source_type) const override;
  bool isApplicableToStage(const std::string& stage_name) const override;
  int priority() const override { return 1; }  // Stage-specific tool

  AnalysisResult analyze(const AnalysisContext& ctx,
                         AnalysisProgress* progress) override;

  bool canApplyToGraph() const override;
  bool applyToGraph(AnalysisResult& result, const Project& project,
                    NodeID node_id) override;

  int estimateDurationSeconds(const AnalysisContext& ctx) const override;
};

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_DISC_MAPPER_ANALYSIS_H
