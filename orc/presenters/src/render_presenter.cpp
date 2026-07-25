/*
 * File:        render_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Rendering presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "render_presenter.h"

#include <orc/stage/analysis_sink_results.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/preview/stage_preview_capability.h>
#include <orc/stage/stage.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/support/logging.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <vector>

#include "../core/include/dag_executor.h"
#include "../core/include/dag_frame_renderer.h"
#include "../core/include/frame_provenance.h"
#include "../core/include/observation_cache.h"
#include "../core/include/observation_store.h"
#include "../core/include/preview_renderer.h"
#include "../core/include/preview_view_registry.h"
#include "../core/include/project.h"
#include "../core/include/project_to_dag.h"
#include "../core/include/vbi_decoder.h"
#include "analysis_series_decimator.h"
#include "metrics_presenter.h"
#include "vbi_presenter.h"

namespace orc::presenters {

namespace {

using orc::analysis::decimate_series;
using orc::analysis::SeriesPoint;

// Representative frame number for a display bucket: its centre frame, matching
// the previous SNR/burst graph labelling.
int32_t bucket_centre(int32_t frame_start, int32_t frame_end) {
  return frame_start + (frame_end - frame_start) / 2;
}

// Fill the frame-range/provenance fields shared by every display point from a
// decimated bucket. `frame_label` is the x-axis position for the point.
orc::presenters::AnalysisDisplayBucket make_bucket(
    const orc::analysis::DecimatedBucket& b, int32_t frame_label) {
  orc::presenters::AnalysisDisplayBucket out;
  out.frame_start = b.frame_start;
  out.frame_end = b.frame_end;
  out.frame_label = frame_label;
  // Bucket width: all analysed frames spanned, including any without a value.
  out.contributing_frames = static_cast<int32_t>(b.record_count);
  out.has_data = b.value_count > 0;
  return out;
}

// True when at least one point aggregates more than one analysed frame, i.e.
// the display is bucketed rather than one-point-per-frame.
template <typename PointT>
bool series_is_decimated(const std::vector<PointT>& points) {
  for (const auto& p : points) {
    if (p.bucket.frame_end > p.bucket.frame_start) {
      return true;
    }
  }
  return false;
}

// Reduce the full-resolution per-frame dropout series to at most `max_points`
// display points. Counts and lengths are summed within each bucket (the graph
// shows dropout activity per bucket); the bucket is labelled with its last
// frame, matching the previous graph labelling.
std::vector<orc::presenters::DropoutDisplayPoint> decimate_dropout_series(
    const std::vector<orc::FrameDropoutStats>& full, std::size_t max_points) {
  std::vector<SeriesPoint> length_series;
  std::vector<SeriesPoint> count_series;
  length_series.reserve(full.size());
  count_series.reserve(full.size());
  for (const auto& fs : full) {
    length_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_length_samples),
        fs.has_data});
    count_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_count), fs.has_data});
  }

  const auto len_buckets = decimate_series(length_series, max_points);
  const auto cnt_buckets = decimate_series(count_series, max_points);

  std::vector<orc::presenters::DropoutDisplayPoint> out;
  out.reserve(len_buckets.size());
  for (std::size_t i = 0; i < len_buckets.size(); ++i) {
    orc::presenters::DropoutDisplayPoint d;
    d.bucket = make_bucket(len_buckets[i], len_buckets[i].frame_end);
    d.dropout_length_samples =
        static_cast<int64_t>(std::llround(len_buckets[i].sum));
    d.dropout_count = static_cast<int32_t>(std::llround(cnt_buckets[i].sum));
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame SNR series to at most `max_points`
// display points, averaging each metric within a bucket and labelling the
// bucket with its centre frame.
std::vector<orc::presenters::SNRDisplayPoint> decimate_snr_series(
    const std::vector<orc::FrameSNRStats>& full, std::size_t max_points) {
  std::vector<SeriesPoint> white_series;
  std::vector<SeriesPoint> black_series;
  white_series.reserve(full.size());
  black_series.reserve(full.size());
  for (const auto& fs : full) {
    white_series.push_back(
        SeriesPoint{fs.frame_number, fs.white_snr, fs.has_white_snr});
    black_series.push_back(
        SeriesPoint{fs.frame_number, fs.black_psnr, fs.has_black_psnr});
  }

  const auto white_buckets = decimate_series(white_series, max_points);
  const auto black_buckets = decimate_series(black_series, max_points);

  std::vector<orc::presenters::SNRDisplayPoint> out;
  out.reserve(white_buckets.size());
  for (std::size_t i = 0; i < white_buckets.size(); ++i) {
    orc::presenters::SNRDisplayPoint d;
    const int32_t label =
        bucket_centre(white_buckets[i].frame_start, white_buckets[i].frame_end);
    d.bucket = make_bucket(white_buckets[i], label);
    d.has_white_snr = white_buckets[i].value_count > 0;
    d.white_snr = white_buckets[i].mean;
    d.has_black_psnr = black_buckets[i].value_count > 0;
    d.black_psnr = black_buckets[i].mean;
    // A point carries data if either metric contributed a value.
    d.bucket.has_data = d.has_white_snr || d.has_black_psnr;
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame burst-level series to at most
// `max_points` display points, averaging within a bucket.
std::vector<orc::presenters::BurstLevelDisplayPoint> decimate_burst_series(
    const std::vector<orc::FrameBurstLevelStats>& full,
    std::size_t max_points) {
  std::vector<SeriesPoint> series;
  series.reserve(full.size());
  for (const auto& fs : full) {
    series.push_back(
        SeriesPoint{fs.frame_number, fs.median_burst_10bit, fs.has_data});
  }

  const auto buckets = decimate_series(series, max_points);

  std::vector<orc::presenters::BurstLevelDisplayPoint> out;
  out.reserve(buckets.size());
  for (const auto& b : buckets) {
    orc::presenters::BurstLevelDisplayPoint d;
    d.bucket = make_bucket(b, bucket_centre(b.frame_start, b.frame_end));
    d.median_burst_10bit = b.mean;
    out.push_back(d);
  }
  return out;
}

}  // namespace

class RenderPresenter::Impl {
 public:
  explicit Impl(void* project_handle)
      : project_(static_cast<orc::Project*>(project_handle)),
        trigger_cancel_requested_(false),
        trigger_active_(false),
        next_request_id_(1) {
    if (!project_) {
      throw std::invalid_argument("Project cannot be null");
    }
  }

  // Helper to get concrete DAG from opaque handle
  std::shared_ptr<const orc::DAG> getConcreteDAG() const {
    return std::static_pointer_cast<const orc::DAG>(dag_void_);
  }

  orc::Project* project_;
  std::shared_ptr<void> dag_void_;  // Opaque DAG handle
  std::unique_ptr<orc::PreviewRenderer> preview_renderer_;
  orc::PreviewViewRegistry preview_view_registry_;
  std::unique_ptr<orc::DAGFrameRenderer> field_renderer_;
  std::unique_ptr<orc::VBIDecoder> vbi_decoder_;
  std::shared_ptr<orc::ObservationCache> obs_cache_;

  // Provenance-keyed observation store, owned here so it survives DAG rebuilds
  // (editing a parameter on one branch must not discard observations for
  // unaffected branches/frames). Created lazily on first rebuild and shared
  // into the ObservationCache and field renderer. The file-identity provider
  // folds source-file identity into SOURCE-node fingerprints.
  std::shared_ptr<orc::ObservationStore> obs_store_;
  orc::FilesystemFileIdentityProvider file_identity_provider_;
  std::atomic<bool> trigger_cancel_requested_;
  std::atomic<bool> trigger_active_;
  uint64_t next_request_id_;
  std::atomic<orc::TriggerableStage*> current_trigger_stage_{nullptr};

  // Display point budget for the analysis-graph data path. The sinks expose the
  // full-resolution per-frame series; the getXAnalysisData accessors decimate
  // it to at most this many points and return a typed view series by value.
  static constexpr std::size_t kDisplayPoints = 1000;

  void rebuildRenderersFromDAG() {
    auto dag = getConcreteDAG();
    if (!dag) {
      // Clear all renderers for null DAG
      preview_renderer_.reset();
      field_renderer_.reset();
      vbi_decoder_.reset();
      obs_cache_.reset();
      preview_view_registry_ = orc::PreviewViewRegistry{};
      return;
    }

    // Save dropout state if exists
    bool show_dropouts = false;
    if (preview_renderer_) {
      show_dropouts = preview_renderer_->get_show_dropouts();
    }

    // Compute the static provenance fingerprint map for this DAG. Shared into
    // the observation cache and field renderer so observer output is keyed to
    // frame content, not the live DAG instance.
    auto fingerprints = std::make_shared<const orc::NodeFingerprintMap>(
        orc::compute_node_fingerprints(*dag, file_identity_provider_));

    // Create the shared observation store once; it persists across rebuilds.
    if (!obs_store_) {
      obs_store_ = std::make_shared<orc::ObservationStore>();
    }

    // Rebuild renderers
    obs_cache_ =
        std::make_shared<orc::ObservationCache>(dag, obs_store_, fingerprints);
    preview_renderer_ = std::make_unique<orc::PreviewRenderer>(dag);
    field_renderer_ = std::make_unique<orc::DAGFrameRenderer>(dag);
    field_renderer_->set_observation_store(obs_store_, fingerprints);
    vbi_decoder_ = std::make_unique<orc::VBIDecoder>();

    preview_view_registry_ = orc::PreviewViewRegistry{};
    orc::PreviewViewRegistry::register_default_views(
        preview_view_registry_, dag, preview_renderer_.get());

    // Restore dropout state
    if (preview_renderer_) {
      preview_renderer_->set_show_dropouts(show_dropouts);
    }
  }
};

RenderPresenter::RenderPresenter(void* project_handle)
    : impl_(std::make_unique<Impl>(project_handle)) {}

RenderPresenter::~RenderPresenter() = default;

RenderPresenter::RenderPresenter(RenderPresenter&&) noexcept = default;
RenderPresenter& RenderPresenter::operator=(RenderPresenter&&) noexcept =
    default;

bool RenderPresenter::updateDAG() {
  try {
    impl_->getConcreteDAG() = orc::project_to_dag(*impl_->project_);
    impl_->rebuildRenderersFromDAG();
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void RenderPresenter::setDAG(std::shared_ptr<void> dag_handle) {
  impl_->dag_void_ = std::move(dag_handle);
  impl_->rebuildRenderersFromDAG();
}

orc::PreviewRenderResult RenderPresenter::renderPreview(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    const std::string& option_id) {
  if (!impl_->preview_renderer_) {
    return orc::PreviewRenderResult{
        {},      false,       "Preview renderer not initialized",
        node_id, output_type, output_index};
  }

  try {
    // Call core preview renderer
    auto core_result = impl_->preview_renderer_->render_output(
        node_id, output_type, output_index, option_id);

    // Populate observation cache for the rendered field(s)
    if (impl_->obs_cache_) {
      if (output_type == orc::PreviewOutputType::Frame_Field1 ||
          output_type == orc::PreviewOutputType::Frame_Field2 ||
          output_type == orc::PreviewOutputType::Luma) {
        impl_->obs_cache_->get_field(node_id, orc::FieldID(output_index));
      } else if (output_type == orc::PreviewOutputType::Frame_Field1_First ||
                 output_type == orc::PreviewOutputType::Frame_Reversed ||
                 output_type == orc::PreviewOutputType::Split) {
        uint64_t first_field = output_index * 2;
        impl_->obs_cache_->get_field(node_id, orc::FieldID(first_field));
        impl_->obs_cache_->get_field(node_id, orc::FieldID(first_field + 1));
      }
    }

    // Convert core result to public API result
    orc::PreviewRenderResult result;
    result.image.width = core_result.image.width;
    result.image.height = core_result.image.height;
    result.image.rgb_data = std::move(core_result.image.rgb_data);
    result.image.dropout_regions = std::move(core_result.image.dropout_regions);
    result.success = core_result.success;
    result.error_message = std::move(core_result.error_message);
    result.node_id = core_result.node_id;
    result.output_type = core_result.output_type;
    result.output_index = core_result.output_index;

    return result;

  } catch (const std::exception& e) {
    return orc::PreviewRenderResult{{},      false,       e.what(),
                                    node_id, output_type, output_index};
  }
}

std::vector<orc::PreviewOutputInfo> RenderPresenter::getAvailableOutputs(
    NodeID node_id) {
  if (!impl_->preview_renderer_) {
    return {};
  }

  auto core_outputs = impl_->preview_renderer_->get_available_outputs(node_id);

  // Convert to public API types
  std::vector<orc::PreviewOutputInfo> result;
  result.reserve(core_outputs.size());

  for (const auto& core_out : core_outputs) {
    orc::PreviewOutputInfo info;
    info.type = core_out.type;
    info.display_name = core_out.display_name;
    info.count = core_out.count;
    info.is_available = core_out.is_available;
    info.dar_aspect_correction = core_out.dar_aspect_correction;
    info.option_id = core_out.option_id;
    info.dropouts_available = core_out.dropouts_available;
    info.has_separate_channels = core_out.has_separate_channels;
    info.first_field_offset = core_out.first_field_offset;
    result.push_back(std::move(info));
  }

  return result;
}

bool RenderPresenter::savePNG(NodeID node_id,
                              orc::PreviewOutputType output_type,
                              uint64_t output_index,
                              const std::string& filename,
                              const std::string& option_id,
                              double aspect_correction) {
  if (!impl_->preview_renderer_) {
    return false;
  }

  try {
    return impl_->preview_renderer_->save_png(node_id, output_type,
                                              output_index, filename, option_id,
                                              aspect_correction);
  } catch (const std::exception&) {
    return false;
  }
}

std::vector<orc::PreviewViewDescriptor>
RenderPresenter::getAvailablePreviewViews(NodeID node_id,
                                          orc::VideoDataType data_type) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return {};
  }

  return impl_->preview_view_registry_.get_applicable_views(*dag, node_id,
                                                            data_type);
}

orc::PreviewViewDataResult RenderPresenter::requestPreviewViewData(
    NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
    const orc::PreviewCoordinate& coordinate) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return {false, "DAG not initialized", orc::PreviewViewPayloadKind::None,
            std::nullopt, std::nullopt};
  }

  return impl_->preview_view_registry_.request_data(*dag, node_id, view_id,
                                                    data_type, coordinate);
}

std::optional<VBIFieldInfoView> RenderPresenter::getVBIData(NodeID node_id,
                                                            FieldID field_id) {
  if (!impl_->obs_cache_) {
    return std::nullopt;
  }

  try {
    // Render the frame to populate observations
    bool obs_ok = impl_->obs_cache_->get_field(node_id, field_id);
    if (!obs_ok) {
      return std::nullopt;
    }

    // Get observations and decode VBI
    const auto& obs_context = impl_->obs_cache_->get_observation_context();
    auto vbi_info_opt =
        VbiPresenter::decodeVbiFromObservation(&obs_context, field_id);

    if (!vbi_info_opt.has_value()) {
      return std::nullopt;
    }

    // Return the fully decoded VBI information
    return vbi_info_opt;

  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool RenderPresenter::requestDropoutData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  // This is a synchronous operation - find the node and return data immediately
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::IDropoutAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a DropoutAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  // Data is available - signal success
  if (callback) callback(request_id, true, "");
  return true;
}

bool RenderPresenter::requestSNRData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::ISNRAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a SNRAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  if (callback) callback(request_id, true, "");
  return true;
}

bool RenderPresenter::requestBurstLevelData(
    NodeID node_id, uint64_t request_id,
    std::function<void(uint64_t, bool, const std::string&)> callback) {
  if (!impl_->getConcreteDAG()) {
    if (callback) callback(request_id, false, "DAG not initialized");
    return false;
  }

  const orc::DAGNode* target_node = nullptr;
  for (const auto& node : impl_->getConcreteDAG()->nodes()) {
    if (node.node_id == node_id) {
      target_node = &node;
      break;
    }
  }

  if (!target_node) {
    if (callback) callback(request_id, false, "Node not found");
    return false;
  }

  auto* sink =
      dynamic_cast<orc::IBurstLevelAnalysisResults*>(target_node->stage.get());
  if (!sink) {
    if (callback) {
      callback(request_id, false, "Node is not a BurstLevelAnalysisSinkStage");
    }
    return false;
  }

  if (!sink->has_results()) {
    if (callback) {
      callback(request_id, false, "No data available - trigger the sink first");
    }
    return false;
  }

  if (callback) callback(request_id, true, "");
  return true;
}

uint64_t RenderPresenter::triggerStage(NodeID node_id,
                                       ProgressCallback callback) {
  if (!impl_->getConcreteDAG()) {
    throw std::runtime_error("DAG not initialized");
  }

  impl_->trigger_cancel_requested_.store(false);
  impl_->trigger_active_.store(true);
  uint64_t request_id = impl_->next_request_id_++;

  try {
    // Find the target node in the DAG
    const orc::DAGNode* target_node = nullptr;
    for (const auto& node : impl_->getConcreteDAG()->nodes()) {
      if (node.node_id == node_id) {
        target_node = &node;
        break;
      }
    }

    if (!target_node) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Node '" + node_id.to_string() +
                               "' not found in DAG");
    }

    auto trigger_stage =
        dynamic_cast<orc::TriggerableStage*>(target_node->stage.get());
    if (!trigger_stage) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Stage '" + node_id.to_string() +
                               "' is not triggerable");
    }

    // Build executor to get inputs for this node
    auto executor = std::make_shared<orc::DAGExecutor>();

    // Execute DAG up to (but not including) the target node to get its inputs
    std::vector<orc::ArtifactPtr> inputs;

    if (!target_node->input_node_ids.empty()) {
      // Execute predecessor nodes to get inputs
      auto node_outputs = executor->execute_to_node(
          *impl_->getConcreteDAG(), target_node->input_node_ids[0]);

      // Collect inputs from predecessor nodes
      for (size_t i = 0; i < target_node->input_node_ids.size(); ++i) {
        const auto& input_node_id = target_node->input_node_ids[i];
        size_t input_index = (i < target_node->input_indices.size())
                                 ? target_node->input_indices[i]
                                 : 0;

        auto it = node_outputs.find(input_node_id);
        if (it != node_outputs.end() && input_index < it->second.size()) {
          inputs.push_back(it->second[input_index]);
        }
      }
    }

    // Store pointer to current trigger stage for cancellation
    impl_->current_trigger_stage_.store(trigger_stage);

    // Set up progress callback
    trigger_stage->set_progress_callback([this, trigger_stage, callback](
                                             size_t current, size_t total,
                                             const std::string& message) {
      // Check for cancellation
      if (impl_->trigger_cancel_requested_.load()) {
        trigger_stage->cancel_trigger();
      }

      // Call user callback
      if (callback) {
        callback(static_cast<int>(current), static_cast<int>(total), message);
      }
    });

    // Execute trigger using the observation context populated during
    // execute_to_node
    orc::ObservationContext& obs_context = executor->get_observation_context();
    bool success =
        trigger_stage->trigger(inputs, target_node->parameters, obs_context);

    // Clear current trigger stage pointer
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);

    if (!success) {
      std::string status = trigger_stage->get_trigger_status();
      throw std::runtime_error("Trigger failed: " + status);
    }

  } catch (...) {
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);
    throw;
  }

  return request_id;
}

uint64_t RenderPresenter::triggerStage(
    NodeID node_id, ProgressCallback callback,
    std::map<std::string, ParameterValue> parameter_overrides) {
  if (!impl_->getConcreteDAG()) {
    throw std::runtime_error("DAG not initialized");
  }

  impl_->trigger_cancel_requested_.store(false);
  impl_->trigger_active_.store(true);
  uint64_t request_id = impl_->next_request_id_++;

  try {
    const orc::DAGNode* target_node = nullptr;
    for (const auto& node : impl_->getConcreteDAG()->nodes()) {
      if (node.node_id == node_id) {
        target_node = &node;
        break;
      }
    }

    if (!target_node) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Node '" + node_id.to_string() +
                               "' not found in DAG");
    }

    auto trigger_stage =
        dynamic_cast<orc::TriggerableStage*>(target_node->stage.get());
    if (!trigger_stage) {
      impl_->trigger_active_.store(false);
      throw std::runtime_error("Stage '" + node_id.to_string() +
                               "' is not triggerable");
    }

    auto executor = std::make_shared<orc::DAGExecutor>();
    std::vector<orc::ArtifactPtr> inputs;
    if (!target_node->input_node_ids.empty()) {
      auto node_outputs = executor->execute_to_node(
          *impl_->getConcreteDAG(), target_node->input_node_ids[0]);
      for (size_t i = 0; i < target_node->input_node_ids.size(); ++i) {
        const auto& input_node_id = target_node->input_node_ids[i];
        size_t input_index = (i < target_node->input_indices.size())
                                 ? target_node->input_indices[i]
                                 : 0;
        auto it = node_outputs.find(input_node_id);
        if (it != node_outputs.end() && input_index < it->second.size()) {
          inputs.push_back(it->second[input_index]);
        }
      }
    }

    impl_->current_trigger_stage_.store(trigger_stage);
    trigger_stage->set_progress_callback([this, trigger_stage, callback](
                                             size_t current, size_t total,
                                             const std::string& message) {
      if (impl_->trigger_cancel_requested_.load()) {
        trigger_stage->cancel_trigger();
      }
      if (callback) {
        callback(static_cast<int>(current), static_cast<int>(total), message);
      }
    });

    // Merge overrides into a copy of the node's stored parameters so the
    // project file is not modified.
    auto merged_params = target_node->parameters;
    for (const auto& [key, value] : parameter_overrides) {
      merged_params[key] = value;
    }

    orc::ObservationContext& obs_context = executor->get_observation_context();
    bool success = trigger_stage->trigger(inputs, merged_params, obs_context);

    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);

    if (!success) {
      std::string status = trigger_stage->get_trigger_status();
      throw std::runtime_error("Trigger failed: " + status);
    }

  } catch (...) {
    impl_->current_trigger_stage_.store(nullptr);
    impl_->trigger_active_.store(false);
    throw;
  }

  return request_id;
}

void RenderPresenter::cancelTrigger() {
  impl_->trigger_cancel_requested_.store(true);
  // Load the pointer atomically so the read is safe from the GUI thread
  auto* stage = impl_->current_trigger_stage_.load();
  if (stage) {
    stage->cancel_trigger();
  }
}

void RenderPresenter::setShowDropouts(bool show) {
  if (impl_->preview_renderer_) {
    impl_->preview_renderer_->set_show_dropouts(show);
  }
}

bool RenderPresenter::getShowDropouts() const {
  if (impl_->preview_renderer_) {
    return impl_->preview_renderer_->get_show_dropouts();
  }
  return false;
}

RenderPresenter::ImageToFieldMapping RenderPresenter::mapImageToField(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    int image_y, int image_height) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result = impl_->preview_renderer_->map_image_to_field(
      node_id, output_type, output_index, image_y, image_height);

  return {result.is_valid, result.field_index, result.field_line};
}

RenderPresenter::FieldToImageMapping RenderPresenter::mapFieldToImage(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    uint64_t field_index, int field_line, int image_height) {
  if (!impl_->preview_renderer_) {
    return {false, 0};
  }

  auto result = impl_->preview_renderer_->map_field_to_image(
      node_id, output_type, output_index, field_index, field_line,
      image_height);

  return {result.is_valid, result.image_y};
}

RenderPresenter::FrameFields RenderPresenter::getFrameFields(
    NodeID node_id, uint64_t frame_index) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result =
      impl_->preview_renderer_->get_frame_fields(node_id, frame_index);
  return {result.is_valid, result.first_field, result.second_field};
}

RenderPresenter::FrameLineNavigation RenderPresenter::navigateFrameLine(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t current_field,
    int current_line, int direction, int field_height) {
  if (!impl_->preview_renderer_) {
    return {false, 0, 0};
  }

  auto result = impl_->preview_renderer_->navigate_frame_line(
      node_id, output_type, current_field, current_line, direction,
      field_height);

  return {result.is_valid, result.new_field_index, result.new_line_number};
}

RenderPresenter::LineSampleData RenderPresenter::getLineSamplesWithYC(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
    int line_number, int /*sample_x*/, int /*preview_width*/) {
  LineSampleData result;
  result.has_separate_channels = false;

  if (!impl_->preview_renderer_) {
    return result;
  }

  if (output_type != orc::PreviewOutputType::Frame_Field1 &&
      output_type != orc::PreviewOutputType::Frame_Field2) {
    return result;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return result;
    }

    orc::FrameID frame_id = static_cast<orc::FrameID>(output_index / 2);
    int field_within_frame = static_cast<int>(output_index % 2);

    auto descriptor = repr->get_frame_descriptor(frame_id);
    if (!descriptor) {
      return result;
    }

    size_t f1_lines = (descriptor->system == orc::VideoSystem::PAL)
                          ? static_cast<size_t>(orc::kPalField1Lines)
                          : static_cast<size_t>(orc::kNtscField1Lines);
    size_t field_height =
        (field_within_frame == 0) ? f1_lines : (descriptor->height - f1_lines);
    size_t field_line_offset = (field_within_frame == 0) ? 0 : f1_lines;

    if (line_number < 0 || static_cast<size_t>(line_number) >= field_height) {
      return result;
    }

    size_t frame_line = field_line_offset + static_cast<size_t>(line_number);
    size_t width = descriptor->samples_per_line_nominal;

    result.has_separate_channels = repr->has_separate_channels();

    if (result.has_separate_channels) {
      const auto* y_data = repr->get_line_luma(frame_id, frame_line);
      if (y_data) {
        result.y_samples.assign(y_data, y_data + width);
      }
      const auto* c_data = repr->get_line_chroma(frame_id, frame_line);
      if (c_data) {
        result.c_samples.assign(c_data, c_data + width);
      }
      result.composite_samples = result.y_samples;
    } else {
      const auto* line_data = repr->get_line(frame_id, frame_line);
      if (!line_data) {
        return result;
      }
      result.composite_samples.assign(line_data, line_data + width);
    }

    return result;

  } catch (const std::exception&) {
    return result;
  }
}

RenderPresenter::LineSampleData RenderPresenter::getFieldSamplesForTiming(
    NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index) {
  LineSampleData result;
  result.has_separate_channels = false;
  result.first_field_height = 0;
  result.second_field_height = 0;

  if (!impl_->preview_renderer_) {
    return result;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return result;
    }

    result.has_separate_channels = repr->has_separate_channels();

    auto collect_lines =
        [&](orc::FrameID frame_id, size_t line_offset, size_t line_count,
            size_t spl, std::vector<int16_t>& composite,
            std::vector<int16_t>& y_out, std::vector<int16_t>& c_out) {
          for (size_t l = 0; l < line_count; ++l) {
            size_t fl = line_offset + l;
            if (result.has_separate_channels) {
              const auto* y = repr->get_line_luma(frame_id, fl);
              const auto* c = repr->get_line_chroma(frame_id, fl);
              if (y) y_out.insert(y_out.end(), y, y + spl);
              if (c) c_out.insert(c_out.end(), c, c + spl);
            } else {
              const auto* d = repr->get_line(frame_id, fl);
              if (d) composite.insert(composite.end(), d, d + spl);
            }
          }
        };

    const bool is_single_field =
        (output_type == orc::PreviewOutputType::Frame_Field1 ||
         output_type == orc::PreviewOutputType::Frame_Field2 ||
         output_type == orc::PreviewOutputType::Luma);
    const bool is_frame_type =
        (output_type == orc::PreviewOutputType::Frame_Field1_First ||
         output_type == orc::PreviewOutputType::Frame_Reversed ||
         output_type == orc::PreviewOutputType::Split);

    if (is_single_field) {
      orc::FrameID frame_id = static_cast<orc::FrameID>(output_index / 2);
      int field_within_frame = static_cast<int>(output_index % 2);

      auto desc = repr->get_frame_descriptor(frame_id);
      if (!desc) {
        return result;
      }

      size_t f1_lines = (desc->system == orc::VideoSystem::PAL)
                            ? static_cast<size_t>(orc::kPalField1Lines)
                            : static_cast<size_t>(orc::kNtscField1Lines);
      size_t field_height =
          (field_within_frame == 0) ? f1_lines : (desc->height - f1_lines);
      size_t field_offset = (field_within_frame == 0) ? 0 : f1_lines;
      result.first_field_height = static_cast<int>(field_height);

      collect_lines(frame_id, field_offset, field_height,
                    desc->samples_per_line_nominal, result.composite_samples,
                    result.y_samples, result.c_samples);

    } else if (is_frame_type) {
      orc::FrameID frame_id = static_cast<orc::FrameID>(output_index);

      auto desc = repr->get_frame_descriptor(frame_id);
      if (!desc) {
        return result;
      }

      size_t f1_lines = (desc->system == orc::VideoSystem::PAL)
                            ? static_cast<size_t>(orc::kPalField1Lines)
                            : static_cast<size_t>(orc::kNtscField1Lines);
      size_t f2_lines = desc->height - f1_lines;
      size_t spl = desc->samples_per_line_nominal;

      size_t field1_offset = 0;
      size_t field1_height = f1_lines;
      size_t field2_offset = f1_lines;
      size_t field2_height = f2_lines;

      if (output_type == orc::PreviewOutputType::Frame_Reversed) {
        std::swap(field1_offset, field2_offset);
        std::swap(field1_height, field2_height);
      }

      result.first_field_height = static_cast<int>(field1_height);
      result.second_field_height = static_cast<int>(field2_height);

      collect_lines(frame_id, field1_offset, field1_height, spl,
                    result.composite_samples, result.y_samples,
                    result.c_samples);
      std::vector<int16_t> comp2, y2, c2;
      collect_lines(frame_id, field2_offset, field2_height, spl, comp2, y2, c2);

      result.composite_samples.insert(result.composite_samples.end(),
                                      comp2.begin(), comp2.end());
      result.y_samples.insert(result.y_samples.end(), y2.begin(), y2.end());
      result.c_samples.insert(result.c_samples.end(), c2.begin(), c2.end());
    }

    if (result.has_separate_channels && result.composite_samples.empty()) {
      result.composite_samples = result.y_samples;
    }

    return result;

  } catch (const std::exception&) {
    return result;
  }
}

std::optional<orc::SourceParameters> RenderPresenter::getVideoParameters(
    NodeID node_id) {
  if (!impl_->preview_renderer_) {
    return std::nullopt;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return std::nullopt;
    }

    return repr->get_video_parameters();

  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::vector<std::string> RenderPresenter::getAudioChannelPairNames(
    NodeID node_id) {
  std::vector<std::string> names;
  if (!impl_->preview_renderer_) {
    return names;
  }

  try {
    auto repr = impl_->preview_renderer_->get_representation_at_node(node_id);
    if (!repr) {
      return names;
    }
    const size_t count = repr->audio_channel_pair_count();
    names.reserve(count);
    for (size_t p = 0; p < count; ++p) {
      auto desc = repr->get_audio_channel_pair_descriptor(p);
      names.push_back(desc ? desc->name : std::string());
    }
  } catch (const std::exception&) {
    names.clear();
  }
  return names;
}

// === Analysis Data Access ===

namespace {

// Locate a DAG node by id, returning nullptr if absent.
const orc::DAGNode* find_node(const orc::DAG& dag, NodeID node_id) {
  for (const auto& node : dag.nodes()) {
    if (node.node_id == node_id) {
      return &node;
    }
  }
  return nullptr;
}

}  // namespace

std::optional<DropoutDisplaySeries> RenderPresenter::getDropoutAnalysisData(
    NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::IDropoutAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  // The sink exposes the full-resolution per-frame series; decimate it to the
  // display point budget before handing it to the graph as a typed view series.
  DropoutDisplaySeries series;
  series.points =
      decimate_dropout_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::optional<SNRDisplaySeries> RenderPresenter::getSNRAnalysisData(
    NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::ISNRAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  SNRDisplaySeries series;
  series.points =
      decimate_snr_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::optional<BurstLevelDisplaySeries>
RenderPresenter::getBurstLevelAnalysisData(NodeID node_id) {
  auto dag = impl_->getConcreteDAG();
  if (!dag) {
    return std::nullopt;
  }

  const orc::DAGNode* target_node = find_node(*dag, node_id);
  if (!target_node) {
    return std::nullopt;
  }

  auto* sink =
      dynamic_cast<orc::IBurstLevelAnalysisResults*>(target_node->stage.get());
  if (!sink || !sink->has_results()) {
    return std::nullopt;
  }

  BurstLevelDisplaySeries series;
  series.points =
      decimate_burst_series(sink->frame_stats(), Impl::kDisplayPoints);
  series.total_frames = sink->total_frames();
  series.decimated = series_is_decimated(series.points);
  return series;
}

std::shared_ptr<const void> RenderPresenter::executeToNode(NodeID node_id) {
  if (!impl_->getConcreteDAG()) {
    return nullptr;
  }

  try {
    orc::DAGExecutor executor;
    auto node_outputs =
        executor.execute_to_node(*impl_->getConcreteDAG(), node_id);

    auto it = node_outputs.find(node_id);
    if (it != node_outputs.end() && !it->second.empty()) {
      // Return the first output (typically VideoFrameRepresentation)
      return std::static_pointer_cast<const void>(it->second[0]);
    }
  } catch (const std::exception&) {
    return nullptr;
  }

  return nullptr;
}

const void* RenderPresenter::getObservationContext(NodeID node_id,
                                                   FieldID field_id) {
  if (!impl_->field_renderer_) {
    return nullptr;
  }

  orc::FrameID frame_id = static_cast<orc::FrameID>(field_id.value() / 2);
  impl_->field_renderer_->render_frame_at_node(node_id, frame_id);

  return &impl_->field_renderer_->get_observation_context();
}

}  // namespace orc::presenters
