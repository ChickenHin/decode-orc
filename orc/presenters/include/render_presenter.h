/*
 * File:        render_presenter.h
 * Module:      orc-presenters
 * Purpose:     Rendering and preview presenter - MVP architecture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // PreviewOutputType
#include <orc/stage/field_id.h>
#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>   // Public API SourceParameters
#include <orc/stage/params/parameter_types.h>  // ParameterValue
#include <orc/stage/preview/orc_rendering.h>   // Public API rendering types
#include <orc_analysis_series.h>  // Analysis display-series view types
#include <orc_preview_views.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "observation_invalidation_view.h"  // ObservationInvalidationEvent
#include "observation_progress_view.h"  // ObservationProgressEvent, ObservationDataReadyCallback
#include "vbi_view_models.h"  // VBIFieldInfoView

// Forward declare core types
namespace orc {
class Project;
class DAG;
}  // namespace orc

namespace orc::presenters {

// Forward declarations
struct QualityMetrics;  // From metrics_presenter.h
using ProgressCallback = std::function<void(
    size_t, size_t, const std::string&)>;  // From project_presenter.h

/**
 * @brief VBI data for a single field
 */
struct VBIData {
  bool has_vbi;
  bool is_clv;
  std::string chapter_number;
  std::string frame_number;
  std::string picture_number;
  std::string picture_stop_code;
  std::string user_code;
  std::vector<std::string> raw_vbi_lines;
};

/**
 * @brief Export format options
 */
enum class ExportFormat { PNG, TIFF, FFV1, ProRes };

/**
 * @brief Export options for sequence rendering
 */
struct ExportOptions {
  std::string output_path;  ///< Output file/directory path
  ExportFormat format;      ///< Export format
  int start_field;          ///< First field to export (-1 for all)
  int end_field;            ///< Last field to export (-1 for all)
  bool deinterlace;         ///< Whether to deinterlace
  int quality;              ///< Quality setting (0-100)
};

// Use public API types
using RenderProgress = orc::RenderProgress;

/**
 * @brief RenderPresenter - Manages preview and export rendering
 *
 * This presenter extracts rendering logic from the GUI layer.
 * It provides a clean interface for:
 * - Rendering preview images for specific nodes/fields
 * - Batch rendering with progress callbacks
 * - VBI data extraction
 * - Analysis data requests (dropout, SNR, burst level)
 * - Managing render cache
 *
 * The presenter uses the core rendering pipeline but provides
 * a simplified interface suitable for GUI consumption.
 *
 * Thread safety: Methods are thread-safe when explicitly noted.
 * Preview rendering should be done from a worker thread.
 */
class RenderPresenter {
 public:
  /**
   * @brief Construct presenter for a project
   * @param project_handle Opaque handle to project
   */
  explicit RenderPresenter(void* project_handle);

  /**
   * @brief Destructor
   */
  ~RenderPresenter();

  // Disable copy, enable move
  RenderPresenter(const RenderPresenter&) = delete;
  RenderPresenter& operator=(const RenderPresenter&) = delete;
  RenderPresenter(RenderPresenter&&) noexcept;
  RenderPresenter& operator=(RenderPresenter&&) noexcept;

  // === DAG Management ===

  /**
   * @brief Update the internal DAG from the current project state
   *
   * Call this whenever the project changes (nodes added/removed/modified).
   * This rebuilds the internal DAG and rendering state.
   *
   * @return true if DAG was built successfully
   */
  bool updateDAG();

  /**
   * @brief Set the DAG directly (for coordination with external DAG management)
   * @param dag_handle Opaque handle to DAG
   *
   * @note Used for coordination with external DAG management.
   * The DAG is typically obtained from ProjectPresenter.
   */
  void setDAG(std::shared_ptr<void> dag_handle);

  // === Observation Invalidation (Phase 3) ===

  /**
   * @brief Subscribe to observation-invalidation notifications.
   *
   * The callback fires whenever a DAG rebuild changes one or more nodes'
   * provenance fingerprints (a parameter or topology edit). It is invoked
   * synchronously on the thread that calls updateDAG()/setDAG(); subscribers
   * must marshal to their own thread. The first DAG build does not notify (it
   * populates rather than invalidates).
   *
   * @param callback Invoked with the changed-node set (view types only).
   * @return Subscription id for unsubscribeInvalidation().
   *
   * Thread-safe: the subscriber registry is mutex-guarded.
   */
  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback);

  /**
   * @brief Cancel a subscription created by subscribeInvalidation().
   *
   * A no-op for an unknown id. Thread-safe.
   */
  void unsubscribeInvalidation(uint64_t subscription_id);

  // === Async Observations (Phase 5) ===

  /**
   * @brief Request a frame's observations without blocking on a render.
   *
   * Answered immediately (the callback fires synchronously before this returns)
   * when the provenance-keyed store already holds every observer's record for
   * the frame. Otherwise the frame is enqueued on the background scheduler at
   * interactive priority and @p callback fires later, on the scheduler's worker
   * thread, once the frame has been observed (or once its observation attempt
   * fails). No synchronous DAG execution happens on the calling thread.
   *
   * The delivered ObservationContext is valid only for the duration of the
   * callback; extract value-type view models inside it (see
   * ObservationDataReadyCallback).
   *
   * @param node_id  Node whose output frame is observed.
   * @param field_id Field of interest; both fields of its parent frame are
   *                 covered.
   * @param callback Delivery callback carrying the returned request id.
   * @return Request id echoed to @p callback for stale-response suppression.
   *
   * Thread-safe.
   */
  uint64_t requestObservations(
      NodeID node_id, FieldID field_id,
      orc::presenters::ObservationDataReadyCallback callback);

  /**
   * @brief Subscribe to background-observation workload snapshots (Task 5.4).
   *
   * The callback fires whenever the outstanding workload changes and returns to
   * an idle snapshot when the queue drains. Invoked on the scheduler's worker
   * thread; subscribers marshal to their own thread. Payload is view types
   * only.
   *
   * @return Subscription id for unsubscribeObservationProgress().
   *
   * Thread-safe.
   */
  uint64_t subscribeObservationProgress(
      orc::presenters::ObservationProgressCallback callback);

  /**
   * @brief Cancel a subscription created by subscribeObservationProgress().
   *
   * A no-op for an unknown id. Thread-safe.
   */
  void unsubscribeObservationProgress(uint64_t subscription_id);

  // === Preview Rendering ===

  /**
   * @brief Render a preview image for a specific output
   *
   * @param node_id Node to render from
   * @param output_type Type of output (Field, Frame, Luma, etc.)
   * @param output_index Index of the output (0-based)
   * @param option_id Optional rendering option ID
   * @return Preview render result with RGB image
   *
   * Thread-safe: Yes (uses internal DAG)
   */
  orc::PreviewRenderResult renderPreview(NodeID node_id,
                                         orc::PreviewOutputType output_type,
                                         uint64_t output_index,
                                         const std::string& option_id = "");

  /**
   * @brief Get available output types for a node
   *
   * @param node_id Node to query
   * @return Vector of available output info
   *
   * Thread-safe: Yes
   */
  std::vector<orc::PreviewOutputInfo> getAvailableOutputs(NodeID node_id);

  /**
   * @brief Save a preview as PNG file
   *
   * @param node_id Node to render from
   * @param output_type Type of output
   * @param output_index Index of output
   * @param filename Path to save PNG
   * @param option_id Optional rendering option ID
   * @return true on success
   */
  bool savePNG(NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id = "",
               double aspect_correction = 1.0);

  /**
   * @brief Get registry-driven preview views applicable to a node/data type.
   */
  std::vector<orc::PreviewViewDescriptor> getAvailablePreviewViews(
      NodeID node_id, orc::VideoDataType data_type);

  /**
   * @brief Request preview-view data via the Phase 3 registry contract.
   */
  orc::PreviewViewDataResult requestPreviewViewData(
      NodeID node_id, const std::string& view_id, orc::VideoDataType data_type,
      const orc::PreviewCoordinate& coordinate);

  // === VBI Data Extraction ===

  /**
   * @brief Get VBI data for a specific field (fully decoded)
   *
   * @param node_id Node to extract from
   * @param field_id Field to decode
   * @return Full VBI field info view with all decoded data, or empty optional
   * if no VBI found
   */
  std::optional<VBIFieldInfoView> getVBIData(NodeID node_id, FieldID field_id);

  // === Analysis Data Access ===

  /**
   * @brief Get the decimated dropout display series from a sink stage
   *
   * The node must be a DropoutAnalysisSinkStage with results. The sink's
   * full-resolution per-frame series is decimated to the display budget and
   * returned as a typed view series; this method abstracts DAG traversal from
   * the GUI layer.
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered dropout sink
   */
  std::optional<DropoutDisplaySeries> getDropoutAnalysisData(NodeID node_id);

  /**
   * @brief Get the decimated SNR display series from a sink stage
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered SNR sink
   */
  std::optional<SNRDisplaySeries> getSNRAnalysisData(NodeID node_id);

  /**
   * @brief Get the decimated burst-level display series from a sink stage
   *
   * @param node_id Node to get data from
   * @return The decimated series, or std::nullopt if the node is not a
   * triggered burst-level sink
   */
  std::optional<BurstLevelDisplaySeries> getBurstLevelAnalysisData(
      NodeID node_id);

  /**
   * @brief Request dropout analysis data from a sink node (deprecated - use
   * getDropoutAnalysisData)
   *
   * The node must be a DropoutAnalysisSinkStage that has been triggered.
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID for async tracking
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestDropoutData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  /**
   * @brief Request SNR analysis data from a sink node (deprecated - use
   * getSNRAnalysisData)
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestSNRData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  /**
   * @brief Request burst level analysis data from a sink node (deprecated - use
   * getBurstLevelAnalysisData)
   *
   * @param node_id Node to get data from
   * @param request_id Unique request ID
   * @param callback Callback when data is ready
   * @return true if request was queued
   */
  bool requestBurstLevelData(
      NodeID node_id, uint64_t request_id,
      std::function<void(uint64_t id, bool success, const std::string& error)>
          callback);

  // === Batch Rendering (Triggering) ===

  /**
   * @brief Trigger a triggerable stage (start batch processing)
   *
   * This method synchronously executes the trigger operation.
   * It should be called from a worker thread to avoid blocking the UI.
   *
   * @param node_id Node to trigger
   * @param callback Progress callback (called from the same thread)
   * @return Request ID for tracking
   * @throws std::runtime_error if triggering fails
   */
  uint64_t triggerStage(NodeID node_id, ProgressCallback callback);

  /**
   * @brief Trigger a stage with transient parameter overrides
   *
   * Like triggerStage() but merges the supplied overrides into the node's
   * stored parameters for this trigger only — the node's saved parameters
   * are not modified.
   *
   * @param node_id Node to trigger
   * @param callback Progress callback
   * @param parameter_overrides Key/value pairs that shadow the node's params
   * @return Request ID for tracking
   * @throws std::runtime_error if triggering fails
   */
  uint64_t triggerStage(
      NodeID node_id, ProgressCallback callback,
      std::map<std::string, ParameterValue> parameter_overrides);

  /**
   * @brief Cancel ongoing trigger operation
   *
   * This sets a cancellation flag that the trigger operation will check.
   * The operation may not stop immediately.
   */
  void cancelTrigger();

  /**
   * @brief Enable or disable this presenter's background observation pipeline.
   *
   * Enabled (the default), the first DAG build attaches the durable
   * observation sidecar and starts the worker-pool scheduler with its
   * background sweeps. Auxiliary presenters that only render frames or read
   * parameters (the dropout editor's, and MainWindow's short-lived helper
   * presenters) must disable this *before* the first setDAG()/updateDAG():
   * they then run with a small in-memory store only — no sidecar attach, no
   * version purge/GC against a potentially multi-GB database, no scheduler,
   * no sweeps — keeping construction cheap enough for the GUI thread and
   * avoiding duplicate background pipelines (one per process is enough). No
   * effect once the store/scheduler have been created.
   */
  void setBackgroundObservationEnabled(bool enabled);

  // === Dropout Visualization ===

  /**
   * @brief Enable/disable dropout highlighting in previews
   * @param show Whether to show dropouts
   */
  void setShowDropouts(bool show);

  /**
   * @brief Get current dropout highlighting state
   * @return true if showing dropouts
   */
  bool getShowDropouts() const;

  // === Coordinate Mapping (for interactive features) ===

  /**
   * @brief Map image coordinates to field coordinates
   *
   * Used for determining which field/line user clicked on in preview.
   *
   * @param node_id Node being previewed
   * @param output_type Output type being displayed
   * @param output_index Output index being displayed
   * @param image_y Y coordinate in preview image
   * @param image_height Height of preview image
   * @param option_id Preview option being displayed; frame outputs can be
   *        weaved or field-sequential and only the option id says which
   * @return Mapping result with field index and line number
   */
  struct ImageToFieldMapping {
    bool is_valid;
    uint64_t field_index;
    int field_line;
  };

  ImageToFieldMapping mapImageToField(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int image_y,
                                      int image_height,
                                      const std::string& option_id = "");

  /**
   * @brief Map field coordinates to image coordinates
   *
   * @param node_id Node being previewed
   * @param output_type Output type being displayed
   * @param output_index Output index being displayed
   * @param field_index Field index
   * @param field_line Line within field
   * @param image_height Height of preview image
   * @param option_id Preview option being displayed (see mapImageToField)
   * @return Image Y coordinate (is_valid=false if out of bounds)
   */
  struct FieldToImageMapping {
    bool is_valid;
    int image_y;
  };

  FieldToImageMapping mapFieldToImage(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index,
                                      uint64_t field_index, int field_line,
                                      int image_height,
                                      const std::string& option_id = "");

  /**
   * @brief Get which fields comprise a frame
   *
   * @param node_id Node being queried
   * @param frame_index Frame index
   * @return Field indices (is_valid=false if invalid)
   */
  struct FrameFields {
    bool is_valid;
    uint64_t first_field;
    uint64_t second_field;
  };

  FrameFields getFrameFields(NodeID node_id, uint64_t frame_index);

  /**
   * @brief Navigate to next/previous line in frame preview
   *
   * @param node_id Node being previewed
   * @param output_type Output type
   * @param current_field Current field index
   * @param current_line Current line in field
   * @param direction +1 for down, -1 for up
   * @param field_height Height of a single field
   * @return New field index and line number
   */
  struct FrameLineNavigation {
    bool is_valid;
    uint64_t new_field_index;
    int new_line_number;
  };

  FrameLineNavigation navigateFrameLine(NodeID node_id,
                                        orc::PreviewOutputType output_type,
                                        uint64_t current_field,
                                        int current_line, int direction,
                                        int field_height);

  // === Line Samples (for waveform display) ===

  /**
   * @brief Line sample data with optional Y/C separation
   *
   * For frame outputs (two fields), heights reflect the actual field heights
   * from the VFR descriptors, which may be different for first vs second field.
   * For single field outputs, only first_field_height is used.
   */
  struct LineSampleData {
    std::vector<int16_t>
        composite_samples;  ///< Composite/combined samples (always populated)
    std::vector<int16_t> y_samples;  ///< Luma samples (only for Y/C sources)
    std::vector<int16_t> c_samples;  ///< Chroma samples (only for Y/C sources)
    bool has_separate_channels;      ///< True if Y/C separation is available
    int first_field_height = 0;  ///< Height of first field from VFR descriptor
    int second_field_height =
        0;  ///< Height of second field (0 if single field)
  };

  /**
   * @brief Get line samples with Y/C separation for oscilloscope display
   *
   * For Y/C sources, returns separate Y and C samples in addition to composite.
   * For composite sources, only composite_samples is populated.
   *
   * @param node_id Node to get samples from
   * @param output_type Output type
   * @param output_index Output index
   * @param line_number Line number in the field/frame
   * @param sample_x X coordinate hint (for field selection in frames)
   * @param preview_width Width of preview image (for coordinate mapping)
   * @return LineSampleData with composite and optional Y/C samples
   */
  LineSampleData getLineSamplesWithYC(NodeID node_id,
                                      orc::PreviewOutputType output_type,
                                      uint64_t output_index, int line_number,
                                      int sample_x, int preview_width);

  /**
   * @brief Get all field samples for timing display
   *
   * Returns all samples from one or two fields (depending on output type).
   * For field output: returns samples from single field.
   * For frame outputs: returns samples from both fields in field order.
   *
   * @param node_id Node to get samples from
   * @param output_type Output type
   * @param output_index Output index
   * @return LineSampleData with all field samples concatenated
   */
  LineSampleData getFieldSamplesForTiming(NodeID node_id,
                                          orc::PreviewOutputType output_type,
                                          uint64_t output_index);

  /**
   * @brief Get video parameters for a node
   *
   * @param node_id Node to get parameters from
   * @return Video parameters if available
   */
  std::optional<orc::SourceParameters> getVideoParameters(NodeID node_id);

  /**
   * @brief Audio channel-pair descriptor names present at a node's output
   *
   * Resolves the representation produced at @p node_id and returns one entry
   * per audio channel pair — the pair's descriptor name, or an empty string
   * when it has none. Used by the GUI to build the audio_channel_map target
   * dropdown (index plus description) restricted to the pairs the node's input
   * actually carries. The pair count is the returned vector's size.
   *
   * @param node_id Node whose output representation is inspected
   * @return One name per channel pair; empty when no audio or unavailable
   */
  std::vector<std::string> getAudioChannelPairNames(NodeID node_id);

  /**
   * @brief Execute DAG to a specific node and return field representation
   *
   * This executes the DAG up to (not including) the specified node and returns
   * the field representation output. This is used for analysis tools that need
   * access to intermediate field data.
   *
   * @param node_id Node to execute to
   * @return Shared pointer to field representation (as void* for encapsulation)
   *
   * @note Returns core VideoFrameRepresentation. Analysis tools should
   * eventually migrate to presenter-based data access.
   */
  std::shared_ptr<const void> executeToNode(NodeID node_id);

  /**
   * @brief Get observation context after rendering a field
   *
   * This renders the field and returns the observation context which can be
   * used by presenters to extract various metrics and observations.
   *
   * @param node_id Node to render at
   * @param field_id Field to render
   * @return Pointer to observation context (as void* for encapsulation)
   *
   * @note Returns core ObservationContext. Metric presenters use this
   * to extract quality data without GUI having direct core access.
   */
  const void* getObservationContext(NodeID node_id, FieldID field_id);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace orc::presenters
