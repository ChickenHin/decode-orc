/*
 * File:        orc_analysis_series.h
 * Module:      orc-view-types
 * Purpose:     Decimated analysis display-series view types for graph dialogs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <vector>

namespace orc::presenters {

/**
 * @brief Frame range and provenance shared by every decimated display point.
 *
 * When the analysed series is longer than the graph's display budget each point
 * aggregates a contiguous run of analysed frames [frame_start, frame_end]; when
 * it is shorter every point maps to exactly one analysed frame
 * (frame_start == frame_end, contributing_frames == 1). `frame_label` is the
 * representative frame plotted on the x-axis.
 */
struct AnalysisDisplayBucket {
  int32_t frame_start = 0;  ///< First analysed frame in the bucket (1-based)
  int32_t frame_end = 0;    ///< Last analysed frame in the bucket (1-based)
  int32_t frame_label = 0;  ///< Representative frame plotted on the x-axis
  int32_t contributing_frames =
      0;                  ///< Analysed frames aggregated into the bucket
  bool has_data = false;  ///< True if any contributing frame carried data
};

/**
 * @brief One decimated dropout display point. Counts and lengths are summed
 * across the bucket's contributing frames.
 */
struct DropoutDisplayPoint {
  AnalysisDisplayBucket bucket;
  int64_t dropout_length_samples = 0;  ///< Summed dropout length (samples)
  int32_t dropout_count = 0;           ///< Summed dropout run count
};

/**
 * @brief One decimated SNR display point. Level metrics are averaged across the
 * bucket's contributing frames.
 */
struct SNRDisplayPoint {
  AnalysisDisplayBucket bucket;
  bool has_white_snr = false;   ///< True if white_snr is meaningful
  double white_snr = 0.0;       ///< Mean white SNR across the bucket (dB)
  bool has_black_psnr = false;  ///< True if black_psnr is meaningful
  double black_psnr = 0.0;      ///< Mean black PSNR across the bucket (dB)
};

/**
 * @brief One decimated burst-level display point. The value is averaged across
 * the bucket's contributing frames.
 */
struct BurstLevelDisplayPoint {
  AnalysisDisplayBucket bucket;
  double median_burst_10bit =
      0.0;  ///< Mean median burst amplitude (10-bit units)
};

/**
 * @brief A decimated analysis series ready for a graph dialog.
 *
 * `total_frames` is the total frame count of the analysed source (used for the
 * x-axis extent and the frame marker); `decimated` is true when at least one
 * point aggregates more than one analysed frame, so the dialog can label the
 * view as bucketed rather than per-frame.
 */
template <typename PointT>
struct AnalysisDisplaySeries {
  std::vector<PointT> points;  ///< Decimated display points (≤ display budget)
  int32_t total_frames = 0;    ///< Total frames in the analysed source
  bool decimated = false;      ///< True when any point covers >1 analysed frame
};

using DropoutDisplaySeries = AnalysisDisplaySeries<DropoutDisplayPoint>;
using SNRDisplaySeries = AnalysisDisplaySeries<SNRDisplayPoint>;
using BurstLevelDisplaySeries = AnalysisDisplaySeries<BurstLevelDisplayPoint>;

}  // namespace orc::presenters
