/*
 * File:        mock_render_presenter.h
 * Module:      orc-tests/gui/unit
 * Purpose:     GMock scaffold for RenderCoordinator's IRenderPresenter seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <gmock/gmock.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "render_coordinator.h"

namespace orc::presenters::test {

class MockRenderPresenter : public IRenderPresenter {
 public:
  MOCK_METHOD(void, setDAG, (std::shared_ptr<void> dag_handle), (override));
  MOCK_METHOD(bool, getShowDropouts, (), (const, override));
  MOCK_METHOD(void, setShowDropouts, (bool show), (override));

  // Real (non-mocked) invalidation registry so tests can exercise the
  // coordinator's subscribe/fire/unsubscribe wiring end to end.
  uint64_t subscribeInvalidation(
      orc::presenters::ObservationInvalidationCallback callback) override {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    const uint64_t id = next_invalidation_id_++;
    invalidation_subscribers_.emplace(id, std::move(callback));
    return id;
  }

  void unsubscribeInvalidation(uint64_t subscription_id) override {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    invalidation_subscribers_.erase(subscription_id);
  }

  // Test helper: simulate a project edit invalidating the given node ids.
  void fireInvalidation(const std::vector<orc::NodeID>& changed_nodes) {
    std::vector<orc::presenters::ObservationInvalidationCallback> callbacks;
    {
      std::lock_guard<std::mutex> lock(invalidation_mutex_);
      for (const auto& [id, cb] : invalidation_subscribers_) {
        callbacks.push_back(cb);
      }
    }
    orc::presenters::ObservationInvalidationEvent event;
    event.changed_nodes = changed_nodes;
    for (const auto& cb : callbacks) {
      if (cb) {
        cb(event);
      }
    }
  }

  // Test helper: number of active invalidation subscribers.
  std::size_t invalidationSubscriberCount() {
    std::lock_guard<std::mutex> lock(invalidation_mutex_);
    return invalidation_subscribers_.size();
  }

  MOCK_METHOD(orc::PreviewRenderResult, renderPreview,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& option_id),
              (override));

  MOCK_METHOD((std::optional<VBIFieldInfoView>), getVBIData,
              (NodeID node_id, FieldID field_id), (override));
  MOCK_METHOD((std::optional<orc::presenters::DropoutDisplaySeries>),
              getDropoutAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::optional<orc::presenters::SNRDisplaySeries>),
              getSNRAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::optional<orc::presenters::BurstLevelDisplaySeries>),
              getBurstLevelAnalysisData, (NodeID node_id), (override));
  MOCK_METHOD((std::vector<orc::PreviewOutputInfo>), getAvailableOutputs,
              (NodeID node_id), (override));

  MOCK_METHOD(LineSampleData, getLineSamplesWithYC,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, int line_number, int sample_x,
               int preview_width),
              (override));

  MOCK_METHOD((std::optional<orc::SourceParameters>), getVideoParameters,
              (NodeID node_id), (override));

  MOCK_METHOD(LineSampleData, getFieldSamplesForTiming,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index),
              (override));

  MOCK_METHOD(orc::FrameLineNavigationResult, navigateFrameLine,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t current_field, int current_line, int direction,
               int field_height),
              (override));

  MOCK_METHOD(uint64_t, triggerStage,
              (NodeID node_id, TriggerProgressCallback callback), (override));
  MOCK_METHOD(
      uint64_t, triggerStage,
      (NodeID node_id, TriggerProgressCallback callback,
       (std::map<std::string, orc::ParameterValue> parameter_overrides)),
      (override));
  MOCK_METHOD(void, cancelTrigger, (), (override));

  MOCK_METHOD(bool, savePNG,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, const std::string& filename,
               const std::string& option_id, double aspect_correction),
              (override));

  MOCK_METHOD(orc::ImageToFieldMappingResult, mapImageToField,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, int image_y, int image_height),
              (override));

  MOCK_METHOD(orc::FieldToImageMappingResult, mapFieldToImage,
              (NodeID node_id, orc::PreviewOutputType output_type,
               uint64_t output_index, uint64_t field_index, int field_line,
               int image_height),
              (override));

  MOCK_METHOD(orc::FrameFieldsResult, getFrameFields,
              (NodeID node_id, uint64_t frame_index), (override));

  MOCK_METHOD((std::vector<orc::PreviewViewDescriptor>),
              getAvailablePreviewViews,
              (NodeID node_id, orc::VideoDataType data_type), (override));

  MOCK_METHOD(orc::PreviewViewDataResult, requestPreviewViewData,
              (NodeID node_id, const std::string& view_id,
               orc::VideoDataType data_type,
               const orc::PreviewCoordinate& coordinate),
              (override));

 private:
  std::mutex invalidation_mutex_;
  std::map<uint64_t, orc::presenters::ObservationInvalidationCallback>
      invalidation_subscribers_;
  uint64_t next_invalidation_id_ = 1;
};

}  // namespace orc::presenters::test
