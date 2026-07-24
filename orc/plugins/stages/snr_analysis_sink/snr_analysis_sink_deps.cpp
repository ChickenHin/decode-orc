/*
 * File:        snr_analysis_sink_deps.cpp
 * Module:      orc-core
 * Purpose:     SNRAnalysisSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "snr_analysis_sink_deps.h"

#include <orc/stage/field_id.h>

#include <fstream>
#include <memory>
#include <ostream>
#include <utility>
#include <variant>

namespace orc {
void SNRAnalysisSinkStageDeps::init(TriggerProgressCallback progress_callback,
                                    std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  cancel_requested_ = cancel_requested;
}

SNRAnalysisComputeResult SNRAnalysisSinkStageDeps::compute_and_analyze(
    VideoFrameRepresentation* representation,
    IObservationContext& observation_context,
    SNRAnalysisComputeOptions options) {
  if (!representation) {
    return {false, "Input representation is null", {}, 0};
  }

  (void)options.output_path;
  (void)options.write_csv;

  SNRAnalysisComputeResult result;
  result.success = true;
  result.message = "SNR analysis complete";

  const auto frame_rng = representation->frame_range();
  const uint64_t total_frames = frame_rng.count();

  if (total_frames == 0) {
    logger_.warn("SNRAnalysisSinkDeps: No frames available");
    result.total_frames = 0;
    return result;
  }

  // Observer sessions are created once and reused across every sampled frame
  // (these observers hold no cross-frame state; the observation field is read
  // and cleared each iteration). A null service — e.g. an older host — leaves
  // the handles null and the per-frame observation is skipped.
  std::unique_ptr<IObserverHandle> white_snr_handle;
  std::unique_ptr<IObserverHandle> black_psnr_handle;
  if (observation_service_) {
    white_snr_handle = observation_service_->create_observer("white_snr");
    black_psnr_handle = observation_service_->create_observer("black_psnr");
  } else {
    logger_.warn(
        "SNRAnalysisSinkDeps: observation service unavailable; SNR "
        "observations skipped");
  }

  // Canonical per-frame capture: analyse frames at first, first + N, … where
  // N = frame_interval, and record each analysed frame's true frame number.
  // Display bucketing is applied downstream by the shared decimation utility
  // (orc/core/analysis/analysis_series_decimator), not here.
  const uint64_t interval = options.frame_interval > 0
                                ? static_cast<uint64_t>(options.frame_interval)
                                : 1U;
  const uint64_t analysed_count = (total_frames + interval - 1U) / interval;

  logger_.debug(
      "SNRAnalysisSinkDeps: {} frames, interval {} → {} analysed frames",
      total_frames, interval, analysed_count);

  result.frame_stats.reserve(static_cast<size_t>(analysed_count));

  uint64_t analysed = 0;
  for (uint64_t offset = 0; offset < total_frames; offset += interval) {
    if (cancel_requested_ && cancel_requested_->load()) {
      logger_.warn("SNRAnalysisSinkDeps: Cancel requested at frame offset {}",
                   offset);
      result.success = false;
      result.message = "Cancelled by user";
      result.frame_stats.clear();
      result.total_frames = 0;
      return result;
    }

    const FrameID fid = frame_rng.first + offset;

    if (white_snr_handle && (options.snr_mode == SNRAnalysisMode::WHITE ||
                             options.snr_mode == SNRAnalysisMode::BOTH)) {
      white_snr_handle->process_frame(*representation, fid,
                                      observation_context);
    }
    if (black_psnr_handle && (options.snr_mode == SNRAnalysisMode::BLACK ||
                              options.snr_mode == SNRAnalysisMode::BOTH)) {
      black_psnr_handle->process_frame(*representation, fid,
                                       observation_context);
    }

    const FieldID frame_fid(fid * 2U);

    FrameSNRStats frame_stat;
    frame_stat.frame_number = static_cast<int32_t>(fid) + 1;

    auto white_val = observation_context.get(frame_fid, "white_snr", "snr_db");
    if (white_val && std::holds_alternative<double>(*white_val)) {
      frame_stat.white_snr = std::get<double>(*white_val);
      frame_stat.has_white_snr = true;
    }

    auto black_val =
        observation_context.get(frame_fid, "black_psnr", "psnr_db");
    if (black_val && std::holds_alternative<double>(*black_val)) {
      frame_stat.black_psnr = std::get<double>(*black_val);
      frame_stat.has_black_psnr = true;
    }

    observation_context.clear_field(frame_fid);

    frame_stat.has_data = frame_stat.has_white_snr || frame_stat.has_black_psnr;
    result.frame_stats.push_back(frame_stat);

    ++analysed;
    if (progress_callback_ &&
        (analysed % 50 == 0 || analysed == analysed_count)) {
      progress_callback_(analysed, analysed_count,
                         "Analysing frame " + std::to_string(analysed) + "/" +
                             std::to_string(analysed_count));
    }
  }

  result.total_frames = static_cast<int32_t>(total_frames);
  logger_.debug("SNRAnalysisSinkDeps: Complete — {} analysed frames from {}",
                result.frame_stats.size(), total_frames);

  return result;
}

void SNRAnalysisSinkStageDeps::write_csv(
    std::ostream& os, const std::vector<FrameSNRStats>& frame_stats) {
  // Canonical per-frame schema. One row per analysed frame; an absent metric is
  // written as an empty field (never the string "nan"). Units live in the
  // header names (dB); values are plain numbers.
  os << "frame_number,white_snr_db,black_psnr_db\n";
  for (const auto& fs : frame_stats) {
    os << fs.frame_number << ',';
    if (fs.has_white_snr) os << fs.white_snr;
    os << ',';
    if (fs.has_black_psnr) os << fs.black_psnr;
    os << '\n';
  }
}

bool SNRAnalysisSinkStageDeps::write_csv(
    const std::string& path, const std::vector<FrameSNRStats>& frame_stats) {
  if (frame_stats.empty()) {
    logger_.warn("SNRAnalysisSinkDeps: No data to write");
    return false;
  }

  logger_.debug("SNRAnalysisSinkDeps: Writing CSV to: {}", path);

  std::ofstream csv(path, std::ios::out | std::ios::trunc);
  if (!csv.is_open()) {
    logger_.error("SNRAnalysisSinkDeps: Failed to open file for writing: {}",
                  path);
    return false;
  }

  write_csv(csv, frame_stats);

  logger_.debug("SNRAnalysisSinkDeps: Successfully wrote {} data rows to: {}",
                frame_stats.size(), path);
  return true;
}
}  // namespace orc
