/*
 * File:        render_coordinator.h
 * Module:      orc-gui
 * Purpose:     Thread-safe coordinator for rendering operations using
 * presenters
 *
 * This class implements an Actor Model pattern where rendering state
 * is owned by a single worker thread. The GUI thread sends requests via
 * a thread-safe queue and receives responses via Qt signals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef RENDER_COORDINATOR_H
#define RENDER_COORDINATOR_H

#include <audio_stream_reader.h>  // IAudioStreamReader (preview audio playback)
#include <orc/stage/common_types.h>
#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>   // Public API VideoParameters
#include <orc/stage/params/parameter_types.h>  // ParameterValue
#include <orc/stage/preview/orc_rendering.h>  // Public API rendering types (includes mapping result types)
#include <orc/stage/preview/preview_stage_types.h>  // PreviewNavigationHint
#include <orc_analysis_series.h>  // Analysis display-series view types
#include <orc_audio_views.h>      // AudioPairView
#include <orc_closed_caption.h>   // Closed caption observation view types
#include <orc_preview_views.h>
#include <orc_teletext.h>  // Teletext observation view types

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "dag_execution_progress_view.h"
#include "ntsc_observation_view_models.h"
#include "observation_invalidation_view.h"
#include "observation_progress_view.h"
#include "vbi_view_models.h"
#include "video_parameter_observation_view_models.h"

namespace orc::presenters {
class IRenderPresenter;
class RenderPresenter;
}  // namespace orc::presenters

// Forward declarations
class GUIProject;

/**
 * @brief Request types for the render coordinator
 */
enum class RenderRequestType {
  UpdateDAG,             // Update the DAG being rendered
  RenderPreview,         // Render a preview image
  GetObservations,       // Fetch a frame's observations (async, non-blocking)
  GetVBIData,            // Decode VBI data for a field
  GetClosedCaptionData,  // Fetch closed caption bytes for a frame (async)
  GetDropoutData,        // Get dropout analysis data
  GetSNRData,            // Get SNR analysis data
  GetBurstLevelData,     // Get burst level analysis data
  GetTeletextAnalysisData,  // Get the teletext page catalogue of a sink stage
  TriggerStage,             // Trigger a stage (batch processing)
  CancelTrigger,            // Cancel ongoing trigger
  GetAvailableOutputs,      // Query available preview outputs
  GetAudioChannelPairs,     // Query the node's audio channel pairs
  CreateAudioStreamReader,  // Create a playback reader for one channel pair
  GetLineSamples,           // Get 16-bit samples for a line
  GetFrameTiming,           // Get all frame samples for timing view
  GetWaveformMonitor,       // Get all frame samples for waveform monitor
  SavePNG,                  // Save preview as PNG file
  NavigateFrameLine,        // Navigate to next/previous line in frame mode
  Shutdown                  // Shutdown the worker thread
};

/**
 * @brief Base class for all requests
 */
struct RenderRequest {
  RenderRequestType type;
  uint64_t request_id;  // Unique ID to match responses

  virtual ~RenderRequest() = default;

 protected:
  explicit RenderRequest(RenderRequestType t, uint64_t id)
      : type(t), request_id(id) {}
};

/**
 * @brief Request to update the DAG
 */
struct UpdateDAGRequest : public RenderRequest {
  std::shared_ptr<const void> dag;

  UpdateDAGRequest(uint64_t id, std::shared_ptr<const void> d)
      : RenderRequest(RenderRequestType::UpdateDAG, id), dag(std::move(d)) {}
};

/**
 * @brief Request to render a preview
 */
struct RenderPreviewRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  std::string option_id;
  orc::PreviewNavigationHint hint;

  RenderPreviewRequest(
      uint64_t id, orc::NodeID node, orc::PreviewOutputType type,
      uint64_t index, std::string opt_id = "",
      orc::PreviewNavigationHint nav_hint = orc::PreviewNavigationHint::Random)
      : RenderRequest(RenderRequestType::RenderPreview, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        option_id(std::move(opt_id)),
        hint(nav_hint) {}
};

/**
 * @brief Request to fetch a frame's observations without blocking
 */
struct GetObservationsRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetObservationsRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetObservations, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to get VBI data
 */
struct GetVBIDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetVBIDataRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetVBIData, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to fetch closed caption bytes for the frame containing a field
 */
struct GetClosedCaptionDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::FieldID field_id;

  GetClosedCaptionDataRequest(uint64_t id, orc::NodeID node, orc::FieldID fid)
      : RenderRequest(RenderRequestType::GetClosedCaptionData, id),
        node_id(std::move(node)),
        field_id(fid) {}
};

/**
 * @brief Request to get dropout analysis data for all fields
 */
struct GetDropoutDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::DropoutAnalysisMode mode;

  GetDropoutDataRequest(uint64_t id, orc::NodeID node,
                        orc::DropoutAnalysisMode m)
      : RenderRequest(RenderRequestType::GetDropoutData, id),
        node_id(std::move(node)),
        mode(m) {}
};

/**
 * @brief Request to get SNR analysis data for all fields
 */
struct GetSNRDataRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::SNRAnalysisMode mode;

  GetSNRDataRequest(uint64_t id, orc::NodeID node, orc::SNRAnalysisMode m)
      : RenderRequest(RenderRequestType::GetSNRData, id),
        node_id(std::move(node)),
        mode(m) {}
};

/**
 * @brief Request to get burst level analysis data for all fields
 */
struct GetBurstLevelDataRequest : public RenderRequest {
  orc::NodeID node_id;

  GetBurstLevelDataRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetBurstLevelData, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to get the teletext page catalogue of an analysis sink stage
 */
struct GetTeletextAnalysisDataRequest : public RenderRequest {
  orc::NodeID node_id;

  GetTeletextAnalysisDataRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetTeletextAnalysisData, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to trigger a stage
 */
struct TriggerStageRequest : public RenderRequest {
  orc::NodeID node_id;

  explicit TriggerStageRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::TriggerStage, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to save preview as PNG
 */
struct SavePNGRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  std::string filename;
  std::string option_id;
  double aspect_correction;

  SavePNGRequest(uint64_t id, orc::NodeID node, orc::PreviewOutputType type,
                 uint64_t index, std::string file, std::string opt_id = "",
                 double correction = 1.0)
      : RenderRequest(RenderRequestType::SavePNG, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        filename(std::move(file)),
        option_id(std::move(opt_id)),
        aspect_correction(correction) {}
};

/**
 * @brief Request to get available outputs
 */
struct GetAvailableOutputsRequest : public RenderRequest {
  orc::NodeID node_id;

  GetAvailableOutputsRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetAvailableOutputs, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to enumerate a node's audio channel pairs
 */
struct GetAudioChannelPairsRequest : public RenderRequest {
  orc::NodeID node_id;

  GetAudioChannelPairsRequest(uint64_t id, orc::NodeID node)
      : RenderRequest(RenderRequestType::GetAudioChannelPairs, id),
        node_id(std::move(node)) {}
};

/**
 * @brief Request to create a playback reader for one audio channel pair
 */
struct CreateAudioStreamReaderRequest : public RenderRequest {
  orc::NodeID node_id;
  size_t pair;

  CreateAudioStreamReaderRequest(uint64_t id, orc::NodeID node, size_t p)
      : RenderRequest(RenderRequestType::CreateAudioStreamReader, id),
        node_id(std::move(node)),
        pair(p) {}
};

/**
 * @brief Request to get line samples
 */
struct GetLineSamplesRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;
  int line_number;
  int sample_x;
  int preview_image_width;  // Width of the preview image for coordinate mapping

  GetLineSamplesRequest(uint64_t id, orc::NodeID node,
                        orc::PreviewOutputType type, uint64_t index, int line,
                        int x, int img_width)
      : RenderRequest(RenderRequestType::GetLineSamples, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index),
        line_number(line),
        sample_x(x),
        preview_image_width(img_width) {}
};

/**
 * @brief Request to get field timing data
 */
struct GetFrameTimingRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;

  GetFrameTimingRequest(uint64_t id, orc::NodeID node,
                        orc::PreviewOutputType type, uint64_t index)
      : RenderRequest(RenderRequestType::GetFrameTiming, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index) {}
};

/**
 * @brief Request to get waveform monitor data
 */
struct GetWaveformMonitorRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t output_index;

  GetWaveformMonitorRequest(uint64_t id, orc::NodeID node,
                            orc::PreviewOutputType type, uint64_t index)
      : RenderRequest(RenderRequestType::GetWaveformMonitor, id),
        node_id(std::move(node)),
        output_type(type),
        output_index(index) {}
};

/**
 * @brief Request to navigate to next/previous line in frame mode
 */
struct NavigateFrameLineRequest : public RenderRequest {
  orc::NodeID node_id;
  orc::PreviewOutputType output_type;
  uint64_t current_field;
  int current_line;
  int direction;     // +1 for down, -1 for up
  int field_height;  // Height of a single field in lines

  NavigateFrameLineRequest(uint64_t id, orc::NodeID node,
                           orc::PreviewOutputType type, uint64_t field,
                           int line, int dir, int height)
      : RenderRequest(RenderRequestType::NavigateFrameLine, id),
        node_id(std::move(node)),
        output_type(type),
        current_field(field),
        current_line(line),
        direction(dir),
        field_height(height) {}
};

/**
 * @brief Base class for responses
 */
struct RenderResponse {
  uint64_t request_id;
  bool success;
  std::string error_message;

  virtual ~RenderResponse() = default;

 protected:
  RenderResponse(uint64_t id, bool s, std::string err = "")
      : request_id(id), success(s), error_message(std::move(err)) {}
};

/**
 * @brief Response with preview render result
 */
struct PreviewRenderResponse : public RenderResponse {
  orc::PreviewRenderResult result;

  PreviewRenderResponse(uint64_t id, bool s, orc::PreviewRenderResult r,
                        std::string err = "")
      : RenderResponse(id, s, std::move(err)), result(std::move(r)) {}
};

/**
 * @brief Response with VBI data
 */
struct VBIDataResponse : public RenderResponse {
  orc::presenters::VBIFieldInfoView vbi_info;

  VBIDataResponse(uint64_t id, bool s, orc::presenters::VBIFieldInfoView info,
                  std::string err = "")
      : RenderResponse(id, s, std::move(err)), vbi_info(std::move(info)) {}
};

/**
 * @brief Response with dropout analysis data
 */
struct DropoutDataResponse : public RenderResponse {
  orc::presenters::DropoutDisplaySeries series;

  DropoutDataResponse(uint64_t id, bool s,
                      orc::presenters::DropoutDisplaySeries data,
                      std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with SNR analysis data
 */
struct SNRDataResponse : public RenderResponse {
  orc::presenters::SNRDisplaySeries series;

  SNRDataResponse(uint64_t id, bool s, orc::presenters::SNRDisplaySeries data,
                  std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with burst level analysis data
 */
struct BurstLevelDataResponse : public RenderResponse {
  orc::presenters::BurstLevelDisplaySeries series;

  BurstLevelDataResponse(uint64_t id, bool s,
                         orc::presenters::BurstLevelDisplaySeries data,
                         std::string err = "")
      : RenderResponse(id, s, std::move(err)), series(std::move(data)) {}
};

/**
 * @brief Response with available outputs
 */
struct AvailableOutputsResponse : public RenderResponse {
  std::vector<orc::PreviewOutputInfo> outputs;

  AvailableOutputsResponse(uint64_t id, bool s,
                           std::vector<orc::PreviewOutputInfo> out,
                           std::string err = "")
      : RenderResponse(id, s, std::move(err)), outputs(std::move(out)) {}
};

/**
 * @brief Response for trigger completion
 */
struct TriggerCompleteResponse : public RenderResponse {
  std::string status_message;

  TriggerCompleteResponse(uint64_t id, bool s, std::string status,
                          std::string err = "")
      : RenderResponse(id, s, std::move(err)),
        status_message(std::move(status)) {}
};

/**
 * @brief Response for frame line navigation
 */
struct FrameLineNavigationResponse : public RenderResponse {
  orc::FrameLineNavigationResult result;

  FrameLineNavigationResponse(uint64_t id, bool s,
                              orc::FrameLineNavigationResult nav_result,
                              std::string err = "")
      : RenderResponse(id, s, std::move(err)), result(nav_result) {}
};

namespace orc::presenters {

class IRenderPresenter {
 public:
  using TriggerProgressCallback =
      std::function<void(int, int, const std::string&)>;

  struct LineSampleData {
    std::vector<int16_t> composite_samples;
    std::vector<int16_t> y_samples;
    std::vector<int16_t> c_samples;
    bool has_separate_channels;
    int first_field_height = 0;
    int second_field_height = 0;
  };

  virtual ~IRenderPresenter() = default;

  virtual void setDAG(std::shared_ptr<void> dag_handle) = 0;
  virtual bool getShowDropouts() const = 0;
  virtual void setShowDropouts(bool show) = 0;

  // Disable the presenter's background observation pipeline (scheduler,
  // sweeps, prefetch). Call before the first setDAG() on auxiliary presenters
  // that only render frames — construction then stays cheap enough for the
  // GUI thread and no duplicate background pipeline is spawned.
  virtual void setBackgroundObservationEnabled(bool enabled) = 0;

  // Observe the on-demand DAG execution that getAvailableOutputs()/
  // renderPreview() drive. Fires once per node, immediately before it runs, on
  // the calling (worker) thread; an empty callback stops delivery. Lets the
  // view report progress while a large source is being opened.
  virtual void setExecutionProgressCallback(
      orc::presenters::DagExecutionProgressCallback callback) = 0;

  // Phase 3: observation-invalidation notifications. subscribeInvalidation()
  // returns an id passed to unsubscribeInvalidation() to cancel delivery.
  virtual uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) = 0;
  virtual void unsubscribeInvalidation(uint64_t subscription_id) = 0;

  // Phase 5: async observation delivery + background-workload progress.
  virtual uint64_t requestObservations(
      NodeID node_id, FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback) = 0;
  virtual uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback) = 0;
  virtual void unsubscribeObservationProgress(uint64_t subscription_id) = 0;

  // @p hint tells stages whether the frame is part of a run of adjacent frames
  // (playback) or a one-off position (scrubbing, a single navigation, an
  // export). Only the playback path sends Sequential.
  virtual orc::PreviewRenderResult renderPreview(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      const std::string& option_id, orc::PreviewNavigationHint hint) = 0;

  virtual std::optional<orc::presenters::DropoutDisplaySeries>
  getDropoutAnalysisData(NodeID node_id) = 0;
  virtual std::optional<orc::presenters::SNRDisplaySeries> getSNRAnalysisData(
      NodeID node_id) = 0;
  virtual std::optional<orc::presenters::BurstLevelDisplaySeries>
  getBurstLevelAnalysisData(NodeID node_id) = 0;
  virtual std::optional<orc::presenters::TeletextAnalysisView>
  getTeletextAnalysisData(NodeID node_id) = 0;
  virtual std::vector<orc::PreviewOutputInfo> getAvailableOutputs(
      NodeID node_id) = 0;

  // Audio channel pairs available at the node, for the preview audio selector.
  // An empty list is the normal "no audio here" answer, not an error.
  virtual std::vector<orc::AudioPairView> getAudioChannelPairs(
      NodeID node_id) = 0;

  // Create a frame-addressed reader for one channel pair. Executes the DAG, so
  // it runs on the coordinator's worker thread; the reader is then primed and
  // read from the playback thread. Returns nullptr when the pair is unusable.
  virtual std::shared_ptr<orc::presenters::IAudioStreamReader>
  createAudioStreamReader(NodeID node_id, size_t pair) = 0;

  virtual LineSampleData getLineSamplesWithYC(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      int line_number, int sample_x, int preview_width) = 0;

  virtual std::optional<orc::SourceParameters> getVideoParameters(
      NodeID node_id) = 0;

  virtual LineSampleData getFieldSamplesForTiming(
      NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t output_index) = 0;

  virtual orc::FrameLineNavigationResult navigateFrameLine(
      NodeID node_id, orc::PreviewOutputType output_type,
      uint64_t current_field, int current_line, int direction,
      int field_height) = 0;

  virtual uint64_t triggerStage(NodeID node_id,
                                TriggerProgressCallback callback) = 0;
  virtual uint64_t triggerStage(
      NodeID node_id, TriggerProgressCallback callback,
      std::map<std::string, orc::ParameterValue> parameter_overrides) = 0;
  virtual void cancelTrigger() = 0;

  virtual bool savePNG(NodeID node_id, orc::PreviewOutputType output_type,
                       uint64_t output_index, const std::string& filename,
                       const std::string& option_id,
                       double aspect_correction) = 0;

  virtual orc::ImageToFieldMappingResult mapImageToField(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      int image_y, int image_height, const std::string& option_id) = 0;

  virtual orc::FieldToImageMappingResult mapFieldToImage(
      NodeID node_id, orc::PreviewOutputType output_type, uint64_t output_index,
      uint64_t field_index, int field_line, int image_height,
      const std::string& option_id) = 0;

  virtual orc::FrameFieldsResult getFrameFields(NodeID node_id,
                                                uint64_t frame_index) = 0;

  virtual std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      NodeID node_id, orc::VideoDataType data_type) = 0;

  virtual orc::PreviewViewDataResult requestPreviewViewData(
      NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate) = 0;
};

}  // namespace orc::presenters

/**
 * @brief Coordinator that owns all core rendering state in a worker thread
 *
 * Architecture:
 * - Worker thread owns: DAG, PreviewRenderer, DAGFrameRenderer, all decoders
 * - GUI thread sends requests via thread-safe queue
 * - Worker thread processes requests serially (no races possible)
 * - Responses sent back via Qt signals (thread-safe)
 *
 * Thread Safety:
 * - ALL public methods are thread-safe (called from GUI thread)
 * - Worker thread methods are private and run on worker thread only
 * - No shared mutable state between threads
 */
// Thread-safe: all public methods are safe to call from the GUI thread;
// worker-thread state is fully private.
class RenderCoordinator : public QObject {
  Q_OBJECT

 public:
  using RenderPresenterFactory =
      std::function<std::shared_ptr<orc::presenters::IRenderPresenter>(void*)>;

  explicit RenderCoordinator(QObject* parent = nullptr);
  explicit RenderCoordinator(RenderPresenterFactory presenter_factory,
                             QObject* parent = nullptr);
  ~RenderCoordinator();

  // Prevent copying/moving
  RenderCoordinator(const RenderCoordinator&) = delete;
  RenderCoordinator& operator=(const RenderCoordinator&) = delete;

  // ========================================================================
  // Public API (thread-safe, called from GUI thread)
  // ========================================================================

  /**
   * @brief Start the worker thread
   *
   * Must be called before any other operations.
   */
  void start();

  /**
   * @brief Stop the worker thread and wait for completion
   *
   * Blocks until worker thread exits cleanly.
   */
  void stop();

  /**
   * @brief Update the DAG being rendered
   *
   * This invalidates all caches and recreates renderers.
   *
   * @param dag New DAG to use (opaque handle)
   */
  void updateDAG(std::shared_ptr<const void> dag);

  /**
   * @brief Set the project for rendering
   *
   * Must be called before updateDAG to initialize the presenter.
   *
   * @param project Project pointer (opaque handle, must outlive
   * RenderCoordinator)
   */
  void setProject(void* project);

  /**
   * @brief Request a preview render (async)
   *
   * Result will be emitted via previewReady signal.
   *
   * Queuing a preview drops any preview still waiting in the queue: the worker
   * is serial and a superseded render's response is discarded on completion
   * anyway, so rendering it only delays the one the user is waiting for.
   *
   * @param node_id Node to render from
   * @param output_type Type of output (field/frame/etc)
   * @param output_index Which field/frame to render
   * @param option_id Rendering option (preview mode plus any signal suffix)
   * @param hint Sequential while playback is running so stages can pre-fetch;
   *        Random for scrubbing and single navigations
   * @return Request ID for matching response
   */
  uint64_t requestPreview(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, const std::string& option_id = "",
      orc::PreviewNavigationHint hint = orc::PreviewNavigationHint::Random);

  /**
   * @brief Request VBI data for a field (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler (same delivery path as requestObservations()).
   * The decoded view is emitted via vbiDataReady. No DAG execution runs on the
   * GUI thread.
   *
   * @param node_id Node to decode VBI from
   * @param field_id Field to decode
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestVBIData(const orc::NodeID& node_id, orc::FieldID field_id);

  /**
   * @brief Request closed caption bytes for the frame containing a field
   *        (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler (same delivery path as requestObservations()).
   * One request covers both fields of the field's parent frame; the extracted
   * per-field byte pairs are emitted via closedCaptionDataReady.
   *
   * @param node_id  Node whose output frame is observed
   * @param field_id Any field of the frame of interest
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestClosedCaptionData(const orc::NodeID& node_id,
                                    orc::FieldID field_id);

  /**
   * @brief Request a frame's observations without blocking the GUI (async)
   *
   * Answered from the provenance-keyed store when present, otherwise computed
   * on the background scheduler. The extracted observation view models are
   * emitted via observationDataReady. No DAG execution runs on the GUI thread.
   *
   * @param node_id  Node whose output frame is observed
   * @param field_id Field of interest (both fields of its frame are covered)
   * @return Request ID for matching / discarding stale responses
   */
  uint64_t requestObservations(const orc::NodeID& node_id,
                               orc::FieldID field_id);

  /**
   * @brief Request dropout analysis data for all fields (async)
   *
   * Result will be emitted via dropoutDataReady signal.
   *
   * @param node_id Node to analyze dropout from
   * @param mode Analysis mode (full field or visible area)
   * @return Request ID for matching response
   */
  uint64_t requestDropoutData(const orc::NodeID& node_id,
                              orc::DropoutAnalysisMode mode);

  /**
   * @brief Request SNR analysis data for all fields (async)
   *
   * Result will be emitted via snrDataReady signal.
   *
   * @param node_id Node to analyze SNR from
   * @param mode Analysis mode (white, black, or both)
   * @return Request ID for matching response
   */
  uint64_t requestSNRData(const orc::NodeID& node_id,
                          orc::SNRAnalysisMode mode);

  /**
   * @brief Request burst level analysis data for all fields (async)
   *
   * Result will be emitted via burstLevelDataReady signal.
   *
   * @param node_id Node to analyze burst level from
   * @return Request ID for matching response
   */
  uint64_t requestBurstLevelData(const orc::NodeID& node_id);

  /**
   * @brief Request the teletext page catalogue of an analysis sink (async)
   *
   * Fetch-or-trigger, as for the graph analyses: an untriggered stage is
   * triggered first (progress via teletextAnalysisProgress) and the catalogue
   * read back. Result is emitted via teletextAnalysisDataReady.
   *
   * @param node_id Teletext analysis sink node to read
   * @return Request ID for matching response
   */
  uint64_t requestTeletextAnalysisData(const orc::NodeID& node_id);

  /**
   * @brief Request available outputs for a node (async)
   *
   * Result will be emitted via availableOutputsReady signal.
   *
   * @param node_id Node to query
   * @return Request ID for matching response
   */
  uint64_t requestAvailableOutputs(const orc::NodeID& node_id);

  /**
   * @brief Request the node's audio channel pairs (async)
   *
   * Enumeration resolves the node's representation, which executes the DAG, so
   * it is routed through the worker like every other query. The result is
   * emitted via audioChannelPairsReady; an empty list means the node carries no
   * usable audio, which is the normal case for most projects.
   *
   * Only the newest request is answered — the viewed node changes faster than
   * enumeration completes on a heavy DAG.
   *
   * @param node_id Node to enumerate
   * @return Request ID for matching response
   */
  uint64_t requestAudioChannelPairs(const orc::NodeID& node_id);

  /**
   * @brief Request a playback reader for one audio channel pair (async)
   *
   * Creation executes the DAG and so must happen on the worker thread; the
   * reader is delivered via audioStreamReaderReady and may then be primed and
   * read from the playback thread. A null reader in the response means the pair
   * is not usable (no audio, unknown video system, or pair out of range).
   *
   * Only the newest request is answered, so a rapid pair reselection cannot
   * deliver a superseded reader.
   *
   * @param node_id Node whose representation supplies the audio
   * @param pair    Channel-pair index
   * @return Request ID for matching response
   */
  uint64_t requestAudioStreamReader(const orc::NodeID& node_id, size_t pair);

  /**
   * @brief Request line samples from a field (async)
   *
   * Result will be emitted via lineSamplesReady signal.
   *
   * @param node_id Node to get samples from
   * @param output_type Type of output (field/frame)
   * @param output_index Which field/frame
   * @param line_number Line number to retrieve (0-based)
   * @param sample_x Sample X position that was clicked (in preview image
   * coordinates)
   * @param preview_image_width Width of the preview image for coordinate
   * mapping
   * @return Request ID for matching response
   */
  uint64_t requestLineSamples(const orc::NodeID& node_id,
                              orc::PreviewOutputType output_type,
                              uint64_t output_index, int line_number,
                              int sample_x, int preview_image_width);

  /**
   * @brief Request field timing data (async)
   *
   * Result will be emitted via frameTimingDataReady signal.
   *
   * @param node_id Node to get samples from
   * @param output_type Type of output (field/frame)
   * @param output_index Which field/frame
   * @return Request ID for matching response
   */
  uint64_t requestFrameTimingData(const orc::NodeID& node_id,
                                  orc::PreviewOutputType output_type,
                                  uint64_t output_index);

  /**
   * @brief Request waveform monitor data (async)
   *
   * Result will be emitted via waveformMonitorDataReady signal.
   *
   * @param node_id Node to get samples from
   * @param output_type Type of output (field/frame)
   * @param output_index Which field/frame
   * @return Request ID for matching response
   */
  uint64_t requestWaveformMonitorData(const orc::NodeID& node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index);

  /**
   * @brief Map preview image coordinates to field coordinates (synchronous)
   *
   * This is a synchronous call that returns immediately with the mapping.
   * No async request needed since it's just a calculation.
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, etc.)
   * @param output_index The output index (0-based)
   * @param image_y Y coordinate in the preview image
   * @param image_height Total height of the image (for split mode)
   * @param option_id The preview option being displayed; frame outputs may be
   *        weaved or field-sequential and only the option id says which
   * @return Mapping result (field_index, field_line)
   */
  orc::ImageToFieldMappingResult mapImageToField(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, int image_y, int image_height,
      const std::string& option_id = "");

  /**
   * @brief Map field coordinates back to preview image coordinates
   * (synchronous)
   *
   * This is a synchronous call that returns immediately with the mapping.
   * No async request needed since it's just a calculation.
   *
   * @param node_id The node being displayed
   * @param output_type The output type (Field, Frame, etc.)
   * @param output_index The output index (0-based)
   * @param field_index The field index
   * @param field_line The line within the field
   * @param image_height Total height of the image (for split mode)
   * @param option_id The preview option being displayed (see mapImageToField)
   * @return Mapping result (image_y)
   */
  orc::FieldToImageMappingResult mapFieldToImage(
      const orc::NodeID& node_id, orc::PreviewOutputType output_type,
      uint64_t output_index, uint64_t field_index, int field_line,
      int image_height, const std::string& option_id = "");

  /**
   * @brief Get the field indices that make up a frame (synchronous)
   *
   * Returns which two fields comprise the given frame, accounting for field
   * ordering.
   *
   * @param node_id The node being displayed
   * @param frame_index The frame index (0-based)
   * @return Result with first_field and second_field indices
   */
  orc::FrameFieldsResult getFrameFields(const orc::NodeID& node_id,
                                        uint64_t frame_index);

  /**
   * @brief Get registry-driven preview views for node/data type (synchronous).
   */
  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      const orc::NodeID& node_id, orc::VideoDataType data_type);

  /**
   * @brief Request preview-view data through presenter registry contract
   * (synchronous).
   */
  orc::PreviewViewDataResult requestPreviewViewData(
      const orc::NodeID& node_id, const std::string& view_id,
      orc::VideoDataType data_type, const orc::PreviewCoordinate& coordinate);

  uint64_t requestSavePNG(const orc::NodeID& node_id,
                          orc::PreviewOutputType output_type,
                          uint64_t output_index, const std::string& filename,
                          const std::string& option_id = "",
                          double aspect_correction = 1.0);

  /**
   * @brief Trigger a stage for batch processing (async)
   *
   * Progress updates emitted via triggerProgress signal.
   * Completion emitted via triggerComplete signal.
   *
   * @param node_id Node to trigger
   * @return Request ID for matching response
   */
  uint64_t requestTrigger(const orc::NodeID& node_id);

  /**
   * @brief Cancel ongoing trigger operation
   */
  void cancelTrigger();

  /**
   * @brief Set whether to render dropout regions onto images
   *
   * Thread-safe - can be called from GUI thread.
   *
   * @param show True to render dropouts, false to hide
   */
  void setShowDropouts(bool show);

 signals:
  /**
   * @brief Emitted when a preview render completes
   *
   * @param request_id The request ID from requestPreview()
   * @param result The render result
   */
  void previewReady(uint64_t request_id, orc::PreviewRenderResult result);

  /**
   * @brief Emitted (on the GUI thread) when a requestVBIData() response is
   *        ready
   *
   * Carries the decoded view for the requested field; a default-constructed
   * view means no VBI was available. Marshalled from the worker/scheduler
   * thread via a queued connection, so responses to concurrent requests may
   * arrive in any order - match them by @p request_id.
   */
  void vbiDataReady(uint64_t request_id,
                    orc::presenters::VBIFieldInfoView info);

  /**
   * @brief Emitted (on the GUI thread) when a requestClosedCaptionData()
   *        response is ready
   *
   * Carries the recovered EIA-608 byte pairs for both fields of the requested
   * frame, in temporal order (field1 precedes field2).
   *
   * @param request_id      Id returned by requestClosedCaptionData()
   * @param available       True when the frame's observations were produced
   * @param field1_id_value First field of the frame (FieldID::value())
   * @param field1          Caption data for the first field
   * @param field2_id_value Second field of the frame
   * @param field2          Caption data for the second field
   *
   * Marshalled from the worker/scheduler thread via a queued connection.
   */
  void closedCaptionDataReady(
      uint64_t request_id, bool available, qulonglong field1_id_value,
      orc::presenters::ClosedCaptionFieldDataView field1,
      qulonglong field2_id_value,
      orc::presenters::ClosedCaptionFieldDataView field2);

  /**
   * @brief Emitted when dropout analysis data is ready
   */
  void dropoutDataReady(uint64_t request_id,
                        orc::presenters::DropoutDisplaySeries series);

  /**
   * @brief Emitted during dropout analysis progress
   */
  void dropoutProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when SNR analysis data is ready
   */
  void snrDataReady(uint64_t request_id,
                    orc::presenters::SNRDisplaySeries series);

  /**
   * @brief Emitted during SNR analysis progress
   */
  void snrProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when burst level analysis data is ready
   */
  void burstLevelDataReady(uint64_t request_id,
                           orc::presenters::BurstLevelDisplaySeries series);

  /**
   * @brief Emitted during burst level analysis progress
   */
  void burstLevelProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when the teletext page catalogue is ready
   */
  void teletextAnalysisDataReady(uint64_t request_id,
                                 orc::presenters::TeletextAnalysisView data);

  /**
   * @brief Emitted while an untriggered teletext analysis sink is decoding
   */
  void teletextAnalysisProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when available outputs query completes
   */
  void availableOutputsReady(uint64_t request_id,
                             std::vector<orc::PreviewOutputInfo> outputs);

  /**
   * @brief Emitted when an audio channel-pair enumeration completes
   *
   * An empty @p pairs list is the normal "this node has no audio" answer.
   * Marshalled from the worker thread via a queued connection.
   */
  void audioChannelPairsReady(uint64_t request_id,
                              std::vector<orc::AudioPairView> pairs);

  /**
   * @brief Emitted when a requested audio playback reader has been created
   *
   * @p reader is null when the pair turned out to be unusable. Marshalled from
   * the worker thread via a queued connection; the reader must be primed off
   * the GUI thread (an EFM decode can take minutes).
   */
  void audioStreamReaderReady(
      uint64_t request_id,
      std::shared_ptr<orc::presenters::IAudioStreamReader> reader);

  /**
   * @brief Emitted when line samples are ready
   */
  void lineSamplesReady(uint64_t request_id, uint64_t field_index,
                        int line_number, int sample_x,
                        std::vector<int16_t> samples,
                        std::optional<orc::SourceParameters> video_params,
                        std::vector<int16_t> y_samples,
                        std::vector<int16_t> c_samples);

  /**
   * @brief Emitted when field timing data is ready
   */
  void frameTimingDataReady(uint64_t request_id, uint64_t field_index,
                            std::optional<uint64_t> field_index_2,
                            std::vector<int16_t> samples,
                            std::vector<int16_t> samples_2,
                            std::vector<int16_t> y_samples,
                            std::vector<int16_t> c_samples,
                            std::vector<int16_t> y_samples_2,
                            std::vector<int16_t> c_samples_2,
                            int first_field_height, int second_field_height);

  /**
   * @brief Emitted when waveform monitor data is ready
   */
  void waveformMonitorDataReady(uint64_t request_id,
                                std::vector<int16_t> composite_samples,
                                std::vector<int16_t> y_samples,
                                std::vector<int16_t> c_samples,
                                int first_field_height,
                                int second_field_height);

  /**
   * @brief Emitted during trigger progress
   */
  void triggerProgress(size_t current, size_t total, QString message);

  /**
   * @brief Emitted when trigger completes
   */
  void triggerComplete(uint64_t request_id, bool success, QString status);

  /**
   * @brief Emitted when frame line navigation result is ready
   */
  void frameLineNavigationReady(uint64_t request_id,
                                orc::FrameLineNavigationResult result);

  /**
   * @brief Emitted on any error
   */
  void error(uint64_t request_id, QString message);

  /**
   * @brief Emitted (on the GUI thread) when a project edit invalidates stored
   *        observations
   *
   * Carries the ids of the nodes whose stored observations became stale
   * (edited node plus downstream descendants). Marshalled from the worker
   * thread via Qt's queued connection.
   */
  void observationsInvalidated(QVector<int> changed_node_ids);

  /**
   * @brief Emitted (on the GUI thread) when a requestObservations() response is
   *        ready
   *
   * @param request_id     Id returned by requestObservations()
   * @param available      True when the frame's observations were produced
   * @param field_id_value Field the observations are for (FieldID::value())
   * @param video_params   Video-parameter observer view model (empty if absent)
   * @param ntsc           NTSC observer view model (empty if absent)
   *
   * Marshalled from the worker/scheduler thread via a queued connection.
   */
  void observationDataReady(
      uint64_t request_id, bool available, qulonglong field_id_value,
      orc::presenters::VideoParameterObservationView video_params,
      orc::presenters::NtscFieldObservationsView ntsc);

  /**
   * @brief Emitted (on the GUI thread) when the background observation workload
   *        changes (Task 5.4)
   *
   * @param active            True while background observation work is running
   * @param percent_complete  Overall completion, 0..100
   * @param computing         True when the batch has actually computed frames;
   *                          false while it only verifies stored coverage
   * @param outstanding_nodes Distinct nodes with pending work
   *
   * Marshalled from the scheduler's worker thread via a queued connection.
   */
  void observationProgress(bool active, int percent_complete, bool computing,
                           qulonglong outstanding_nodes);

  /**
   * @brief Emitted (on the GUI thread) as each node of an on-demand preview
   *        execution starts
   *
   * A preview query (available outputs, render) executes the DAG up to the
   * queried node. Opening a large source takes many seconds inside a single
   * node, so the view uses these events to report what the worker is doing
   * instead of leaving the window silent.
   *
   * @param node_id_value NodeID::value() of the node about to run
   * @param current       1-based position in the execution order
   * @param total         Number of nodes in the execution order
   *
   * Marshalled from the worker thread via a queued connection.
   */
  void executionProgress(int node_id_value, qulonglong current,
                         qulonglong total);

 private:
  // ========================================================================
  // Worker thread methods (run on worker thread only)
  // ========================================================================

  /**
   * @brief Main worker thread loop
   */
  void workerLoop();

  /**
   * @brief Process a single request
   */
  void processRequest(std::unique_ptr<RenderRequest> request);

  /**
   * @brief Handle UpdateDAG request
   */
  void handleUpdateDAG(const UpdateDAGRequest& req);

  /**
   * @brief Handle RenderPreview request
   */
  void handleRenderPreview(const RenderPreviewRequest& req);

  /**
   * @brief Handle GetObservations request
   */
  void handleGetObservations(const GetObservationsRequest& req);

  /**
   * @brief Handle GetVBIData request
   */
  void handleGetVBIData(const GetVBIDataRequest& req);

  /**
   * @brief Handle GetClosedCaptionData request
   */
  void handleGetClosedCaptionData(const GetClosedCaptionDataRequest& req);

  /**
   * @brief Handle GetDropoutData request
   */
  void handleGetDropoutData(const GetDropoutDataRequest& req);

  /**
   * @brief Handle GetSNRData request
   */
  void handleGetSNRData(const GetSNRDataRequest& req);

  /**
   * @brief Handle GetBurstLevelData request
   */
  void handleGetBurstLevelData(const GetBurstLevelDataRequest& req);

  /**
   * @brief Handle GetTeletextAnalysisData request
   */
  void handleGetTeletextAnalysisData(const GetTeletextAnalysisDataRequest& req);

  /**
   * @brief Handle GetAvailableOutputs request
   */
  void handleGetAvailableOutputs(const GetAvailableOutputsRequest& req);

  /**
   * @brief Handle GetAudioChannelPairs request
   */
  void handleGetAudioChannelPairs(const GetAudioChannelPairsRequest& req);

  /**
   * @brief Handle CreateAudioStreamReader request
   */
  void handleCreateAudioStreamReader(const CreateAudioStreamReaderRequest& req);

  /**
   * @brief Handle GetLineSamples request
   */
  void handleGetLineSamples(const GetLineSamplesRequest& req);

  /**
   * @brief Handle GetFrameTiming request
   */
  void handleGetFrameTiming(const GetFrameTimingRequest& req);

  /**
   * @brief Handle GetWaveformMonitor request
   */
  void handleGetWaveformMonitor(const GetWaveformMonitorRequest& req);

  /**
   * @brief Handle NavigateFrameLine request
   */
  void handleNavigateFrameLine(const NavigateFrameLineRequest& req);

  void handleSavePNG(const SavePNGRequest& req);

  /**
   * @brief Handle TriggerStage request
   */
  void handleTriggerStage(const TriggerStageRequest& req);

  /**
   * @brief Enqueue a request (thread-safe)
   */
  void enqueueRequest(std::unique_ptr<RenderRequest> request);

  /**
   * @brief Drop every queued (not yet started) preview render
   *
   * Caller must hold queue_mutex_. Returns how many were removed, for logging.
   * The request being processed is already off the queue and is unaffected; it
   * still finishes and is dropped by the existing stale-response check.
   */
  size_t discardQueuedPreviewsLocked();

  /**
   * @brief Get next request ID (thread-safe)
   */
  uint64_t nextRequestId();

  // ========================================================================
  // Thread synchronization
  // ========================================================================

  std::thread worker_thread_;
  std::atomic<bool> shutdown_requested_{false};

  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  // A deque rather than a queue: superseded previews are removed from the
  // middle of the pending work (discardQueuedPreviewsLocked()).
  std::deque<std::unique_ptr<RenderRequest>> request_queue_;

  std::atomic<uint64_t> next_request_id_{1};
  std::atomic<uint64_t> latest_preview_request_id_{0};

  // Newest-only audio queries: the viewed node and the selected pair both
  // change faster than a heavy DAG can answer, so superseded responses are
  // dropped rather than delivered to a dialogue that has moved on.
  std::atomic<uint64_t> latest_audio_pairs_request_id_{0};
  std::atomic<uint64_t> latest_audio_reader_request_id_{0};

  // ========================================================================
  // Worker thread state (owned by worker thread, never accessed from GUI)
  // ========================================================================

  std::shared_ptr<const void> worker_dag_;
  std::shared_ptr<orc::presenters::IRenderPresenter> worker_render_presenter_;
  void* worker_project_{nullptr};  // Non-owning opaque handle for presenter
  RenderPresenterFactory presenter_factory_;

  // Phase 3: invalidation subscription held on the worker presenter (0 = none).
  uint64_t worker_invalidation_subscription_{0};

  // Phase 5: workload-progress subscription on the worker presenter (0 = none).
  uint64_t worker_progress_subscription_{0};

  // Phase 2.7: Trigger state now managed by RenderPresenter
  // Removed: trigger_cancel_requested_ and current_trigger_stage_
};

/**
 * @brief Create an IRenderPresenter backed by a concrete RenderPresenter
 *
 * Used by GUI components (e.g. the dropout editor) that render through the
 * presenter interface so tests can substitute a fake implementation.
 *
 * @param project_handle Opaque core project handle (must be non-null and
 * outlive the returned presenter)
 */
std::shared_ptr<orc::presenters::IRenderPresenter> makeRenderPresenterAdapter(
    void* project_handle);

#endif  // RENDER_COORDINATOR_H
