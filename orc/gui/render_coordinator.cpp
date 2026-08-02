/*
 * File:        render_coordinator.cpp
 * Module:      orc-gui
 * Purpose:     Thread-safe coordinator for rendering operations using
 * presenters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "render_coordinator.h"

#include <orc/stage/common_types.h>  // For analysis result types

#include "logging.h"
#include "ntsc_observation_presenter.h"
#include "render_presenter.h"
#include "teletext_observation_presenter.h"
#include "vbi_presenter.h"
#include "video_parameter_observation_presenter.h"

namespace {

class RenderPresenterAdapter final : public orc::presenters::IRenderPresenter {
 public:
  explicit RenderPresenterAdapter(void* project_handle)
      : presenter_(project_handle) {}

  void setDAG(std::shared_ptr<void> dag_handle) override {
    presenter_.setDAG(std::move(dag_handle));
  }
  bool getShowDropouts() const override { return presenter_.getShowDropouts(); }
  void setShowDropouts(bool show) override { presenter_.setShowDropouts(show); }
  void setBackgroundObservationEnabled(bool enabled) override {
    presenter_.setBackgroundObservationEnabled(enabled);
  }

  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) override {
    return presenter_.subscribeInvalidation(std::move(callback));
  }
  void unsubscribeInvalidation(uint64_t subscription_id) override {
    presenter_.unsubscribeInvalidation(subscription_id);
  }

  uint64_t requestObservations(
      orc::NodeID node_id, orc::FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback) override {
    return presenter_.requestObservations(node_id, field_id,
                                          std::move(callback));
  }
  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback) override {
    return presenter_.subscribeObservationProgress(std::move(callback));
  }
  void unsubscribeObservationProgress(uint64_t subscription_id) override {
    presenter_.unsubscribeObservationProgress(subscription_id);
  }

  orc::PreviewRenderResult renderPreview(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, const std::string& option_id) override {
    return presenter_.renderPreview(node_id, output_type, output_index,
                                    option_id);
  }

  std::optional<orc::presenters::DropoutDisplaySeries> getDropoutAnalysisData(
      orc::NodeID node_id) override {
    return presenter_.getDropoutAnalysisData(node_id);
  }

  std::optional<orc::presenters::SNRDisplaySeries> getSNRAnalysisData(
      orc::NodeID node_id) override {
    return presenter_.getSNRAnalysisData(node_id);
  }

  std::optional<orc::presenters::BurstLevelDisplaySeries>
  getBurstLevelAnalysisData(orc::NodeID node_id) override {
    return presenter_.getBurstLevelAnalysisData(node_id);
  }

  std::vector<orc::PreviewOutputInfo> getAvailableOutputs(
      orc::NodeID node_id) override {
    return presenter_.getAvailableOutputs(node_id);
  }

  LineSampleData getLineSamplesWithYC(orc::NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int line_number,
                                      int sample_x,
                                      int preview_width) override {
    auto source =
        presenter_.getLineSamplesWithYC(node_id, output_type, output_index,
                                        line_number, sample_x, preview_width);
    return LineSampleData{
        std::move(source.composite_samples), std::move(source.y_samples),
        std::move(source.c_samples),         source.has_separate_channels,
        source.first_field_height,           source.second_field_height,
    };
  }

  std::optional<orc::SourceParameters> getVideoParameters(
      orc::NodeID node_id) override {
    return presenter_.getVideoParameters(node_id);
  }

  LineSampleData getFieldSamplesForTiming(orc::NodeID node_id,
                                          orc::PreviewOutputType output_type,
                                          uint64_t output_index) override {
    auto source =
        presenter_.getFieldSamplesForTiming(node_id, output_type, output_index);
    return LineSampleData{
        std::move(source.composite_samples), std::move(source.y_samples),
        std::move(source.c_samples),         source.has_separate_channels,
        source.first_field_height,           source.second_field_height,
    };
  }

  orc::FrameLineNavigationResult navigateFrameLine(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t current_field, int current_line, int direction,
      int field_height) override {
    auto result =
        presenter_.navigateFrameLine(node_id, output_type, current_field,
                                     current_line, direction, field_height);
    return orc::FrameLineNavigationResult{
        result.is_valid, result.new_field_index, result.new_line_number};
  }

  uint64_t triggerStage(orc::NodeID node_id,
                        TriggerProgressCallback callback) override {
    return presenter_.triggerStage(node_id, std::move(callback));
  }

  uint64_t triggerStage(
      orc::NodeID node_id, TriggerProgressCallback callback,
      std::map<std::string, orc::ParameterValue> parameter_overrides) override {
    return presenter_.triggerStage(node_id, std::move(callback),
                                   std::move(parameter_overrides));
  }

  void cancelTrigger() override { presenter_.cancelTrigger(); }

  bool savePNG(orc::NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id,
               double aspect_correction) override {
    return presenter_.savePNG(node_id, output_type, output_index, filename,
                              option_id, aspect_correction);
  }

  orc::ImageToFieldMappingResult mapImageToField(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, int image_y, int image_height,
      const std::string& option_id) override {
    auto result = presenter_.mapImageToField(node_id, output_type, output_index,
                                             image_y, image_height, option_id);
    return orc::ImageToFieldMappingResult{result.is_valid, result.field_index,
                                          result.field_line};
  }

  orc::FieldToImageMappingResult mapFieldToImage(
      orc::NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, uint64_t field_index, int field_line,
      int image_height, const std::string& option_id) override {
    auto result = presenter_.mapFieldToImage(node_id, output_type, output_index,
                                             field_index, field_line,
                                             image_height, option_id);
    return orc::FieldToImageMappingResult{result.is_valid, result.image_y};
  }

  orc::FrameFieldsResult getFrameFields(orc::NodeID node_id,
                                        uint64_t frame_index) override {
    auto result = presenter_.getFrameFields(node_id, frame_index);
    return orc::FrameFieldsResult{result.is_valid, result.first_field,
                                  result.second_field};
  }

  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      orc::NodeID node_id, orc::VideoDataType data_type) override {
    return presenter_.getAvailablePreviewViews(node_id, data_type);
  }

  orc::PreviewViewDataResult requestPreviewViewData(
      orc::NodeID node_id, const std::string& view_id,
      orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate) override {
    return presenter_.requestPreviewViewData(node_id, view_id, data_type,
                                             coordinate);
  }

 private:
  orc::presenters::RenderPresenter presenter_;
};

}  // namespace

std::shared_ptr<orc::presenters::IRenderPresenter> makeRenderPresenterAdapter(
    void* project_handle) {
  return std::make_shared<RenderPresenterAdapter>(project_handle);
}

// Forward declarations for core types used via opaque pointers
namespace orc {
class DAG;
class Project;
}  // namespace orc

// Phase 2.4: Analysis sink stage headers removed - now using RenderPresenter
// abstraction Removed: #include "dropout_analysis_sink_stage.h" Removed:
// #include "snr_analysis_sink_stage.h" Removed: #include
// "burst_level_analysis_sink_stage.h"

// Phase 2.7: Trigger operations migrated to RenderPresenter
// Removed: #include "ld_sink_stage.h"

namespace {

// Wrap a progress-emitting function so it only fires when the whole-percent
// value or the message changes (final updates always pass). Stage triggers
// report progress per processed frame; forwarding every call as a queued Qt
// signal floods the GUI event queue (tens of thousands of events for a long
// source) and starves painting — the cause of a blank, beach-balling progress
// dialog. The gate runs on the worker thread, before anything is queued.
template <typename EmitFn>
auto makePercentGatedProgress(EmitFn emit_fn) {
  return [emit_fn = std::move(emit_fn), last_pct = -1,
          last_message = std::string()](int current, int total,
                                        const std::string& message) mutable {
    const int pct =
        total > 0
            ? static_cast<int>(static_cast<int64_t>(current) * 100 / total)
            : 0;
    const bool final_update = total > 0 && current >= total;
    if (pct == last_pct && message == last_message && !final_update) {
      return;
    }
    last_pct = pct;
    last_message = message;
    emit_fn(current, total, message);
  };
}

}  // namespace

RenderCoordinator::RenderCoordinator(QObject* parent)
    : QObject(parent), presenter_factory_([](void* project_handle) {
        return std::make_shared<RenderPresenterAdapter>(project_handle);
      }) {}

RenderCoordinator::RenderCoordinator(RenderPresenterFactory presenter_factory,
                                     QObject* parent)
    : QObject(parent), presenter_factory_(std::move(presenter_factory)) {}

RenderCoordinator::~RenderCoordinator() { stop(); }

void RenderCoordinator::start() {
  if (worker_thread_.joinable()) {
    ORC_LOG_WARN("RenderCoordinator: Worker thread already running");
    return;
  }

  shutdown_requested_ = false;
  worker_thread_ = std::thread(&RenderCoordinator::workerLoop, this);

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread started");
}

void RenderCoordinator::stop() {
  if (!worker_thread_.joinable()) {
    return;
  }

  ORC_LOG_DEBUG("RenderCoordinator: Requesting shutdown...");

  // Send shutdown request
  shutdown_requested_ = true;

  // Wake up worker if waiting
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_one();
  }

  // Wait for worker to finish
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread stopped");
}

uint64_t RenderCoordinator::nextRequestId() {
  return next_request_id_.fetch_add(1);
}

void RenderCoordinator::enqueueRequest(std::unique_ptr<RenderRequest> request) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    request_queue_.push(std::move(request));
  }
  queue_cv_.notify_one();
}

void RenderCoordinator::updateDAG(std::shared_ptr<const void> dag) {
  auto req =
      std::make_unique<UpdateDAGRequest>(nextRequestId(), std::move(dag));
  enqueueRequest(std::move(req));
}

void RenderCoordinator::setProject(void* project) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  worker_project_ = project;
}

uint64_t RenderCoordinator::requestPreview(const orc::NodeID& node_id,
                                           orc::PreviewOutputType output_type,
                                           uint64_t output_index,
                                           const std::string& option_id) {
  uint64_t id = nextRequestId();
  latest_preview_request_id_.store(id);
  auto req = std::make_unique<RenderPreviewRequest>(id, node_id, output_type,
                                                    output_index, option_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestVBIData(const orc::NodeID& node_id,
                                           orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetVBIDataRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestTeletextData(const orc::NodeID& node_id,
                                                orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetTeletextDataRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestObservations(const orc::NodeID& node_id,
                                                orc::FieldID field_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetObservationsRequest>(id, node_id, field_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestDropoutData(const orc::NodeID& node_id,
                                               orc::DropoutAnalysisMode mode) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetDropoutDataRequest>(id, node_id, mode);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestSNRData(const orc::NodeID& node_id,
                                           orc::SNRAnalysisMode mode) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetSNRDataRequest>(id, node_id, mode);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestBurstLevelData(const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetBurstLevelDataRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestAvailableOutputs(
    const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetAvailableOutputsRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestLineSamples(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, int line_number, int sample_x,
    int preview_image_width) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetLineSamplesRequest>(
      id, node_id, output_type, output_index, line_number, sample_x,
      preview_image_width);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestFrameTimingData(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetFrameTimingRequest>(id, node_id, output_type,
                                                     output_index);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestWaveformMonitorData(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<GetWaveformMonitorRequest>(
      id, node_id, output_type, output_index);
  enqueueRequest(std::move(req));
  return id;
}

uint64_t RenderCoordinator::requestSavePNG(const orc::NodeID& node_id,
                                           orc::PreviewOutputType output_type,
                                           uint64_t output_index,
                                           const std::string& filename,
                                           const std::string& option_id,
                                           double aspect_correction) {
  uint64_t id = nextRequestId();
  auto req =
      std::make_unique<SavePNGRequest>(id, node_id, output_type, output_index,
                                       filename, option_id, aspect_correction);
  enqueueRequest(std::move(req));
  return id;
}

orc::ImageToFieldMappingResult RenderCoordinator::mapImageToField(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, int image_y, int image_height,
    const std::string& option_id) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::ImageToFieldMappingResult{false, 0, 0};
  }
  return worker_render_presenter_->mapImageToField(
      node_id, output_type, output_index, image_y, image_height, option_id);
}

orc::FieldToImageMappingResult RenderCoordinator::mapFieldToImage(
    const orc::NodeID& node_id, orc::PreviewOutputType output_type,
    uint64_t output_index, uint64_t field_index, int field_line,
    int image_height, const std::string& option_id) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::FieldToImageMappingResult{false, 0};
  }
  return worker_render_presenter_->mapFieldToImage(
      node_id, output_type, output_index, field_index, field_line, image_height,
      option_id);
}

orc::FrameFieldsResult RenderCoordinator::getFrameFields(
    const orc::NodeID& node_id, uint64_t frame_index) {
  // This is a synchronous call - safe to call render presenter directly
  // since it's just a calculation with no state changes
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return orc::FrameFieldsResult{false, 0, 0};
  }
  return worker_render_presenter_->getFrameFields(node_id, frame_index);
}

std::vector<orc::PreviewViewDescriptor>
RenderCoordinator::getAvailablePreviewViews(const orc::NodeID& node_id,
                                            orc::VideoDataType data_type) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return {};
  }
  return worker_render_presenter_->getAvailablePreviewViews(node_id, data_type);
}

orc::PreviewViewDataResult RenderCoordinator::requestPreviewViewData(
    const orc::NodeID& node_id, const std::string& view_id,
    orc::VideoDataType data_type, const orc::PreviewCoordinate& coordinate) {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (!worker_render_presenter_) {
    return {false, "Render presenter not initialized",
            orc::PreviewViewPayloadKind::None, std::nullopt, std::nullopt};
  }
  return worker_render_presenter_->requestPreviewViewData(
      node_id, view_id, data_type, coordinate);
}

uint64_t RenderCoordinator::requestTrigger(const orc::NodeID& node_id) {
  uint64_t id = nextRequestId();
  auto req = std::make_unique<TriggerStageRequest>(id, node_id);
  enqueueRequest(std::move(req));
  return id;
}

void RenderCoordinator::cancelTrigger() {
  // Call cancelTrigger on the presenter (thread-safe)
  // The presenter's implementation sets a flag that the trigger operation will
  // check
  if (worker_render_presenter_) {
    worker_render_presenter_->cancelTrigger();
  }
  ORC_LOG_DEBUG("RenderCoordinator: Trigger cancellation requested");
}

// ============================================================================
// Worker Thread Implementation
// ============================================================================

void RenderCoordinator::workerLoop() {
  ORC_LOG_DEBUG("RenderCoordinator: Worker thread loop started");

  while (!shutdown_requested_) {
    std::unique_ptr<RenderRequest> request;

    // Wait for a request
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] {
        return !request_queue_.empty() || shutdown_requested_;
      });

      if (shutdown_requested_) {
        break;
      }

      if (!request_queue_.empty()) {
        request = std::move(request_queue_.front());
        request_queue_.pop();
      }
    }

    // Process the request
    if (request) {
      try {
        processRequest(std::move(request));
      } catch (const std::exception& e) {
        ORC_LOG_ERROR("RenderCoordinator: Exception processing request: {}",
                      e.what());
        emit error(request->request_id, QString::fromStdString(e.what()));
      } catch (...) {
        ORC_LOG_ERROR(
            "RenderCoordinator: Unknown exception processing request");
        emit error(request->request_id, "Unknown error");
      }
    }
  }

  // Tear the presenter down here, on the worker thread, so its background
  // scheduler is stopped (and any awaited observation callbacks fire, emitting
  // their final signals) while this coordinator QObject is still fully alive.
  if (worker_render_presenter_ && worker_invalidation_subscription_ != 0) {
    worker_render_presenter_->unsubscribeInvalidation(
        worker_invalidation_subscription_);
    worker_invalidation_subscription_ = 0;
  }
  if (worker_render_presenter_ && worker_progress_subscription_ != 0) {
    worker_render_presenter_->unsubscribeObservationProgress(
        worker_progress_subscription_);
    worker_progress_subscription_ = 0;
  }
  worker_render_presenter_.reset();

  ORC_LOG_DEBUG("RenderCoordinator: Worker thread loop exiting");
}

void RenderCoordinator::processRequest(std::unique_ptr<RenderRequest> request) {
  switch (request->type) {
    case RenderRequestType::UpdateDAG:
      handleUpdateDAG(*static_cast<UpdateDAGRequest*>(request.get()));
      break;

    case RenderRequestType::RenderPreview:
      handleRenderPreview(*static_cast<RenderPreviewRequest*>(request.get()));
      break;

    case RenderRequestType::GetObservations:
      handleGetObservations(
          *static_cast<GetObservationsRequest*>(request.get()));
      break;

    case RenderRequestType::GetVBIData:
      handleGetVBIData(*static_cast<GetVBIDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetTeletextData:
      handleGetTeletextData(
          *static_cast<GetTeletextDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetDropoutData:
      handleGetDropoutData(*static_cast<GetDropoutDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetSNRData:
      handleGetSNRData(*static_cast<GetSNRDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetBurstLevelData:
      handleGetBurstLevelData(
          *static_cast<GetBurstLevelDataRequest*>(request.get()));
      break;

    case RenderRequestType::GetAvailableOutputs:
      handleGetAvailableOutputs(
          *static_cast<GetAvailableOutputsRequest*>(request.get()));
      break;

    case RenderRequestType::GetLineSamples:
      handleGetLineSamples(*static_cast<GetLineSamplesRequest*>(request.get()));
      break;

    case RenderRequestType::GetFrameTiming:
      handleGetFrameTiming(*static_cast<GetFrameTimingRequest*>(request.get()));
      break;

    case RenderRequestType::GetWaveformMonitor:
      handleGetWaveformMonitor(
          *static_cast<GetWaveformMonitorRequest*>(request.get()));
      break;

    case RenderRequestType::SavePNG:
      handleSavePNG(*static_cast<SavePNGRequest*>(request.get()));
      break;

    case RenderRequestType::NavigateFrameLine:
      handleNavigateFrameLine(
          *static_cast<NavigateFrameLineRequest*>(request.get()));
      break;

    case RenderRequestType::TriggerStage:
      handleTriggerStage(*static_cast<TriggerStageRequest*>(request.get()));
      break;

    case RenderRequestType::Shutdown:
      shutdown_requested_ = true;
      break;

    default:
      ORC_LOG_WARN("RenderCoordinator: Unknown request type: {}",
                   static_cast<int>(request->type));
      break;
  }
}

void RenderCoordinator::handleUpdateDAG(const UpdateDAGRequest& req) {
  ORC_LOG_DEBUG("RenderCoordinator: Updating DAG (request {})", req.request_id);

  if (!req.dag) {
    // Null DAG is valid - happens with empty projects or projects with no
    // stages
    ORC_LOG_WARN(
        "RenderCoordinator: Received null DAG (empty project with no stages)");

    // Clear all worker state
    worker_dag_.reset();
    if (worker_render_presenter_ && worker_invalidation_subscription_ != 0) {
      worker_render_presenter_->unsubscribeInvalidation(
          worker_invalidation_subscription_);
      worker_invalidation_subscription_ = 0;
    }
    if (worker_render_presenter_ && worker_progress_subscription_ != 0) {
      worker_render_presenter_->unsubscribeObservationProgress(
          worker_progress_subscription_);
      worker_progress_subscription_ = 0;
    }
    worker_render_presenter_.reset();

    ORC_LOG_DEBUG(
        "RenderCoordinator: Cleared all rendering state for empty project");
    return;
  }

  // Save current show_dropouts state before recreating presenter
  bool show_dropouts = false;
  if (worker_render_presenter_) {
    show_dropouts = worker_render_presenter_->getShowDropouts();
    ORC_LOG_DEBUG("RenderCoordinator: Preserving show_dropouts={}",
                  show_dropouts);
  }

  // Update DAG
  worker_dag_ = req.dag;

  // Create or update render presenter
  try {
    if (!worker_project_) {
      ORC_LOG_ERROR("RenderCoordinator: No project set for presenter");
      return;
    }

    if (!worker_render_presenter_) {
      worker_render_presenter_ = presenter_factory_(worker_project_);

      // Subscribe to invalidation notifications and re-emit them on the GUI
      // thread. The callback fires synchronously on the worker thread inside
      // setDAG(); the queued signal marshals to the GUI thread.
      worker_invalidation_subscription_ =
          worker_render_presenter_->subscribeInvalidation(
              [this](
                  const orc::presenters::ObservationInvalidationEvent& event) {
                QVector<int> ids;
                ids.reserve(static_cast<int>(event.changed_nodes.size()));
                for (const auto& node : event.changed_nodes) {
                  ids.push_back(node.value());
                }
                emit observationsInvalidated(ids);
              });

      // Subscribe to background-workload snapshots and re-emit them on the GUI
      // thread (Task 5.4). The callback fires on the scheduler's worker thread;
      // the queued signal marshals to the GUI thread.
      worker_progress_subscription_ =
          worker_render_presenter_->subscribeObservationProgress(
              [this](const orc::presenters::ObservationProgressEvent& event) {
                emit observationProgress(
                    event.active, event.percent_complete, event.computing,
                    static_cast<qulonglong>(event.outstanding_nodes));
              });
    }

    // Set the new DAG (cast away const since setDAG signature uses non-const
    // void*)
    worker_render_presenter_->setDAG(
        std::const_pointer_cast<void>(worker_dag_));

    // Restore show_dropouts state
    worker_render_presenter_->setShowDropouts(show_dropouts);
    ORC_LOG_DEBUG("RenderCoordinator: Restored show_dropouts={}",
                  show_dropouts);

    ORC_LOG_DEBUG("RenderCoordinator: DAG updated successfully");
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Failed to create presenter: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleRenderPreview(const RenderPreviewRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Rendering preview for node '{}', type {}, index {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.output_type),
      req.output_index, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto result = worker_render_presenter_->renderPreview(
        req.node_id, req.output_type, req.output_index, req.option_id);

    // Drop stale preview responses when a newer preview request exists.
    if (req.request_id != latest_preview_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropping stale preview response {} (latest {})",
          req.request_id, latest_preview_request_id_.load());
      return;
    }

    if (!result.success && !result.error_message.empty()) {
      ORC_LOG_ERROR("RenderCoordinator: Preview render failed: {}",
                    result.error_message);
    }
    ORC_LOG_DEBUG("RenderCoordinator: Preview render complete, success={}",
                  result.success);

    // Emit result on GUI thread
    emit previewReady(req.request_id, std::move(result));

  } catch (const std::exception& e) {
    if (req.request_id != latest_preview_request_id_.load()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Suppressing stale preview error {} (latest {})",
          req.request_id, latest_preview_request_id_.load());
      return;
    }
    ORC_LOG_ERROR("RenderCoordinator: Preview render failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetObservations(
    const GetObservationsRequest& req) {
  if (!worker_render_presenter_) {
    emit observationDataReady(req.request_id, false,
                              static_cast<qulonglong>(req.field_id.value()),
                              orc::presenters::VideoParameterObservationView{},
                              orc::presenters::NtscFieldObservationsView{});
    return;
  }

  // Capture the signal parameters the observation view models need up front, on
  // this worker thread. The delivery callback may run on the scheduler's worker
  // thread and must not touch the single-threaded presenter render state; it
  // only extracts value-type view models from the delivered (pure) context.
  // Held in a shared_ptr so the callback closure copies cheaply and without
  // throwing (a bare std::optional<SourceParameters> copy can allocate, which
  // would make the noexcept delivery lambda ill-formed).
  auto video_params = std::make_shared<std::optional<orc::SourceParameters>>(
      worker_render_presenter_->getVideoParameters(req.node_id));
  const uint64_t request_id = req.request_id;
  const orc::FieldID field_id = req.field_id;

  worker_render_presenter_->requestObservations(
      req.node_id, field_id,
      [this, request_id, field_id, video_params](
          uint64_t /*presenter_request_id*/, bool available,
          const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::VideoParameterObservationView video_view;
          orc::presenters::NtscFieldObservationsView ntsc_view;
          if (available && obs_context != nullptr) {
            video_view = orc::presenters::VideoParameterObservationPresenter::
                extractObservations(field_id, obs_context, *video_params);
            ntsc_view = orc::presenters::NtscObservationPresenter::
                extractFieldObservations(field_id, obs_context);
          }
          emit observationDataReady(
              request_id, available, static_cast<qulonglong>(field_id.value()),
              std::move(video_view), std::move(ntsc_view));
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: observation delivery failed: {}",
                        e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: observation delivery failed");
        }
      });
}

void RenderCoordinator::handleGetTeletextData(
    const GetTeletextDataRequest& req) {
  // One presenter request covers both fields of the field's parent frame
  // (requestObservations() maps field -> frame), so extract both here and
  // let the teletext dialog fill its trailing-frame-window cache at half the
  // request rate. Temporal order is ascending derived field id.
  const uint64_t frame_index = req.field_id.value() / 2;
  const orc::FieldID field1_id(frame_index * 2);
  const orc::FieldID field2_id(frame_index * 2 + 1);

  if (!worker_render_presenter_) {
    emit teletextDataReady(req.request_id, false,
                           static_cast<qulonglong>(field1_id.value()),
                           orc::presenters::TeletextFieldPacketsView{},
                           static_cast<qulonglong>(field2_id.value()),
                           orc::presenters::TeletextFieldPacketsView{});
    return;
  }

  const uint64_t request_id = req.request_id;
  worker_render_presenter_->requestObservations(
      req.node_id, req.field_id,
      [this, request_id, field1_id, field2_id](
          uint64_t /*presenter_request_id*/, bool available,
          const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::TeletextFieldPacketsView field1_view;
          orc::presenters::TeletextFieldPacketsView field2_view;
          if (available && obs_context != nullptr) {
            field1_view = orc::presenters::TeletextObservationPresenter::
                extractFieldObservations(field1_id, obs_context);
            field2_view = orc::presenters::TeletextObservationPresenter::
                extractFieldObservations(field2_id, obs_context);
          }
          emit teletextDataReady(request_id, available,
                                 static_cast<qulonglong>(field1_id.value()),
                                 std::move(field1_view),
                                 static_cast<qulonglong>(field2_id.value()),
                                 std::move(field2_view));
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: teletext delivery failed: {}",
                        e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: teletext delivery failed");
        }
      });
}

void RenderCoordinator::handleGetVBIData(const GetVBIDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting VBI data for node '{}', field {} (request "
      "{})",
      req.node_id.to_string(), req.field_id.value(), req.request_id);

  if (!worker_render_presenter_) {
    emit vbiDataReady(req.request_id, orc::presenters::VBIFieldInfoView{});
    return;
  }

  const uint64_t request_id = req.request_id;
  const orc::FieldID field_id = req.field_id;

  worker_render_presenter_->requestObservations(
      req.node_id, field_id,
      [this, request_id, field_id](uint64_t /*presenter_request_id*/,
                                   bool available,
                                   const void* obs_context) noexcept {
        // This callback may run on the scheduler's worker thread inside a
        // completion callback that must never throw; contain everything.
        try {
          orc::presenters::VBIFieldInfoView vbi_view;
          if (available && obs_context != nullptr) {
            auto decoded =
                orc::presenters::VbiPresenter::decodeVbiFromObservation(
                    obs_context, field_id);
            if (decoded.has_value()) {
              vbi_view = std::move(*decoded);
            }
          }
          emit vbiDataReady(request_id, std::move(vbi_view));
        } catch (const std::exception& e) {
          ORC_LOG_ERROR("RenderCoordinator: VBI delivery failed: {}", e.what());
        } catch (...) {
          ORC_LOG_ERROR("RenderCoordinator: VBI delivery failed");
        }
      });
}

void RenderCoordinator::handleGetDropoutData(const GetDropoutDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting dropout analysis data for node '{}', mode {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.mode), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series = worker_render_presenter_->getDropoutAnalysisData(req.node_id);
    if (!series) {
      // Stage has not been triggered yet — trigger it now so the data is
      // available.
      ORC_LOG_DEBUG(
          "RenderCoordinator: Dropout stage has no results, triggering now "
          "(request {})",
          req.request_id);
      worker_render_presenter_->triggerStage(
          req.node_id,
          makePercentGatedProgress(
              [this](int current, int total, const std::string& message) {
                emit dropoutProgress(static_cast<size_t>(current),
                                     static_cast<size_t>(total),
                                     QString::fromStdString(message));
              }));
      series = worker_render_presenter_->getDropoutAnalysisData(req.node_id);
      if (!series) {
        emit error(req.request_id,
                   "Failed to get dropout data - node may not be a "
                   "DropoutAnalysisSinkStage or trigger failed");
        return;
      }
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Served dropout dataset from sink ({} points, {} "
        "frames total)",
        series->points.size(), series->total_frames);
    emit dropoutDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Dropout analysis failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetSNRData(const GetSNRDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting SNR analysis data for node '{}', mode {} "
      "(request {})",
      req.node_id.to_string(), static_cast<int>(req.mode), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series = worker_render_presenter_->getSNRAnalysisData(req.node_id);
    if (!series) {
      // Stage has not been triggered yet — trigger it now so the data is
      // available.
      ORC_LOG_DEBUG(
          "RenderCoordinator: SNR stage has no results, triggering now "
          "(request {})",
          req.request_id);
      auto progress_cb = makePercentGatedProgress(
          [this](int current, int total, const std::string& message) {
            emit snrProgress(static_cast<size_t>(current),
                             static_cast<size_t>(total),
                             QString::fromStdString(message));
          });
      worker_render_presenter_->triggerStage(req.node_id, progress_cb);
      series = worker_render_presenter_->getSNRAnalysisData(req.node_id);
      if (!series) {
        emit error(req.request_id,
                   "Failed to get SNR data - node may not be a "
                   "SNRAnalysisSinkStage or trigger failed");
        return;
      }
    }

    ORC_LOG_DEBUG("RenderCoordinator: Served SNR dataset from sink ({} points)",
                  series->points.size());
    emit snrDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: SNR analysis failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetBurstLevelData(
    const GetBurstLevelDataRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting burst level analysis data for node '{}' "
      "(request {})",
      req.node_id.to_string(), req.request_id);

  try {
    if (!worker_render_presenter_) {
      emit error(req.request_id, "Render presenter not initialized");
      return;
    }

    // Use the RenderPresenter abstraction instead of direct DAG access.
    auto series =
        worker_render_presenter_->getBurstLevelAnalysisData(req.node_id);
    if (!series) {
      // Stage has not been triggered yet — trigger it now so the data is
      // available.
      ORC_LOG_DEBUG(
          "RenderCoordinator: Burst level stage has no results, triggering now "
          "(request {})",
          req.request_id);
      auto progress_cb = makePercentGatedProgress(
          [this](int current, int total, const std::string& message) {
            emit burstLevelProgress(static_cast<size_t>(current),
                                    static_cast<size_t>(total),
                                    QString::fromStdString(message));
          });
      worker_render_presenter_->triggerStage(req.node_id, progress_cb);
      series = worker_render_presenter_->getBurstLevelAnalysisData(req.node_id);
      if (!series) {
        emit error(req.request_id,
                   "Failed to get burst data - node may not be a "
                   "BurstLevelAnalysisSinkStage or trigger failed");
        return;
      }
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Served burst dataset from sink ({} points)",
        series->points.size());
    emit burstLevelDataReady(req.request_id, std::move(*series));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Burst level analysis failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetAvailableOutputs(
    const GetAvailableOutputsRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting available outputs for node '{}' (request {})",
      req.node_id.to_string(), req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto outputs = worker_render_presenter_->getAvailableOutputs(req.node_id);

    ORC_LOG_DEBUG("RenderCoordinator: Found {} available outputs",
                  outputs.size());

    // Emit result on GUI thread (using public_api types directly)
    emit availableOutputsReady(req.request_id, std::move(outputs));

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Get available outputs failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetLineSamples(const GetLineSamplesRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting line samples for node '{}', line {} (request "
      "{})",
      req.node_id.to_string(), req.line_number, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Get line samples with Y/C separation if available
    auto sample_data = worker_render_presenter_->getLineSamplesWithYC(
        req.node_id, req.output_type, req.output_index, req.line_number,
        req.sample_x, req.preview_image_width);

    if (sample_data.composite_samples.empty()) {
      // Line data not available (expected for sink stages that don't produce
      // field representations)
      ORC_LOG_DEBUG(
          "RenderCoordinator: Line data not available for node '{}' (expected "
          "for sink stages)",
          req.node_id.to_string());
      emit error(req.request_id, "Line data not available");
      return;
    }

    // Get video parameters from the representation
    auto video_params =
        worker_render_presenter_->getVideoParameters(req.node_id);

    // Emit samples with Y/C separation when available
    if (sample_data.has_separate_channels) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Emitting line samples with Y/C separation (Y: {} "
          "samples, C: {} samples)",
          sample_data.y_samples.size(), sample_data.c_samples.size());
    }

    emit lineSamplesReady(
        req.request_id, req.output_index, req.line_number, req.sample_x,
        std::move(sample_data.composite_samples), video_params,
        std::move(sample_data.y_samples), std::move(sample_data.c_samples));

  } catch (const std::exception& e) {
    ORC_LOG_DEBUG(
        "RenderCoordinator: Get line samples failed: {} (expected for sink "
        "stages)",
        e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetFrameTiming(const GetFrameTimingRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting field timing data for node '{}', index {} "
      "(request {})",
      req.node_id.to_string(), req.output_index, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Get field samples for timing view
    auto sample_data = worker_render_presenter_->getFieldSamplesForTiming(
        req.node_id, req.output_type, req.output_index);

    if (sample_data.composite_samples.empty() &&
        sample_data.y_samples.empty()) {
      // Field data not available
      ORC_LOG_DEBUG("RenderCoordinator: Field data not available for node '{}'",
                    req.node_id.to_string());
      emit error(req.request_id, "Field data not available");
      return;
    }

    // Determine field indices based on output type
    uint64_t field_index = req.output_index;
    std::optional<uint64_t> field_index_2;
    std::vector<int16_t> samples_2;
    std::vector<int16_t> y_samples_2;
    std::vector<int16_t> c_samples_2;

    if (req.output_type == orc::PreviewOutputType::Frame_Field1_First ||
        req.output_type == orc::PreviewOutputType::Frame_Reversed ||
        req.output_type == orc::PreviewOutputType::Split) {
      // For frame modes, output_index is a frame number, so convert to field
      // indices Frame N consists of fields (N*2) and (N*2 + 1)
      field_index = req.output_index * 2;
      field_index_2 = field_index + 1;

      // Note: For frame modes, the data is already concatenated in sample_data
      // We don't separate it back out here - the widget will handle the
      // combined data
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Emitting field timing data (field {}{}, {} "
        "composite samples, {} Y samples, {} C samples)",
        field_index,
        field_index_2.has_value()
            ? std::string(" + ") + std::to_string(field_index_2.value())
            : "",
        sample_data.composite_samples.size(), sample_data.y_samples.size(),
        sample_data.c_samples.size());

    emit frameTimingDataReady(
        req.request_id, field_index, field_index_2,
        std::move(sample_data.composite_samples), std::move(samples_2),
        std::move(sample_data.y_samples), std::move(sample_data.c_samples),
        std::move(y_samples_2), std::move(c_samples_2),
        sample_data.first_field_height, sample_data.second_field_height);

  } catch (const std::exception& e) {
    ORC_LOG_DEBUG("RenderCoordinator: Get field timing failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleGetWaveformMonitor(
    const GetWaveformMonitorRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Getting waveform monitor data for node '{}', index "
      "{} (request {})",
      req.node_id.to_string(), req.output_index, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    auto sample_data = worker_render_presenter_->getFieldSamplesForTiming(
        req.node_id, req.output_type, req.output_index);

    if (sample_data.composite_samples.empty() &&
        sample_data.y_samples.empty()) {
      ORC_LOG_DEBUG(
          "RenderCoordinator: Field data not available for node '{}' "
          "(waveform monitor)",
          req.node_id.to_string());
      emit error(req.request_id, "Field data not available");
      return;
    }

    ORC_LOG_DEBUG(
        "RenderCoordinator: Emitting waveform monitor data ({} composite, {} "
        "Y, {} C samples)",
        sample_data.composite_samples.size(), sample_data.y_samples.size(),
        sample_data.c_samples.size());

    emit waveformMonitorDataReady(
        req.request_id, std::move(sample_data.composite_samples),
        std::move(sample_data.y_samples), std::move(sample_data.c_samples),
        sample_data.first_field_height, sample_data.second_field_height);

  } catch (const std::exception& e) {
    ORC_LOG_DEBUG("RenderCoordinator: Get waveform monitor failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleNavigateFrameLine(
    const NavigateFrameLineRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Navigating frame line for node '{}', field {}, line "
      "{}, direction {} (request {})",
      req.node_id.to_string(), req.current_field, req.current_line,
      req.direction, req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Use the render presenter's method to navigate
    auto result = worker_render_presenter_->navigateFrameLine(
        req.node_id, req.output_type, req.current_field, req.current_line,
        req.direction, req.field_height);

    // Emit result on GUI thread (using public_api types)
    emit frameLineNavigationReady(req.request_id, result);

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Frame line navigation failed: {}",
                  e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::handleTriggerStage(const TriggerStageRequest& req) {
  ORC_LOG_DEBUG("RenderCoordinator: Triggering stage '{}' (request {})",
                req.node_id.to_string(), req.request_id);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    emit triggerComplete(req.request_id, false,
                         "Render presenter not initialized");
    return;
  }

  try {
    // Use RenderPresenter to handle triggering
    // The presenter abstracts all DAG access and stage interaction
    worker_render_presenter_->triggerStage(
        req.node_id,
        makePercentGatedProgress([this](int current, int total,
                                        const std::string& message) {
          // Emit progress updates (Qt will queue to GUI thread)
          emit triggerProgress(current, total, QString::fromStdString(message));
        }));

    ORC_LOG_DEBUG("RenderCoordinator: Trigger complete successfully");
    emit triggerComplete(req.request_id, true,
                         "Trigger completed successfully");

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: Trigger failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
    emit triggerComplete(req.request_id, false,
                         QString::fromStdString(e.what()));
  }
}

void RenderCoordinator::setShowDropouts(bool show) {
  if (worker_render_presenter_) {
    worker_render_presenter_->setShowDropouts(show);
    ORC_LOG_DEBUG("RenderCoordinator: Show dropouts set to {}", show);
  }
}

void RenderCoordinator::handleSavePNG(const SavePNGRequest& req) {
  ORC_LOG_DEBUG(
      "RenderCoordinator: Saving PNG for node '{}', type {}, index {} to '{}'",
      req.node_id.to_string(), static_cast<int>(req.output_type),
      req.output_index, req.filename);

  if (!worker_render_presenter_) {
    ORC_LOG_ERROR("RenderCoordinator: Render presenter not initialized");
    emit error(req.request_id, "Render presenter not initialized");
    return;
  }

  try {
    // Use presenter's PNG save functionality
    bool success = worker_render_presenter_->savePNG(
        req.node_id, req.output_type, req.output_index, req.filename,
        req.option_id, req.aspect_correction);

    if (success) {
      ORC_LOG_DEBUG("RenderCoordinator: PNG saved successfully to '{}'",
                    req.filename);
    } else {
      ORC_LOG_ERROR("RenderCoordinator: Failed to save PNG to '{}'",
                    req.filename);
      emit error(
          req.request_id,
          QString::fromStdString("Failed to save PNG file: " + req.filename));
    }

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("RenderCoordinator: PNG export failed: {}", e.what());
    emit error(req.request_id, QString::fromStdString(e.what()));
  }
}
