/*
 * File:        render_coordinator_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Actor-style tests for RenderCoordinator request/response
 * behavior
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "render_coordinator.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/teletext_slicer.h>

#include <QCoreApplication>
#include <QMetaType>
#include <QSignalSpy>
#include <QThread>
#include <chrono>
#include <thread>

#include "mocks/mock_render_presenter.h"

Q_DECLARE_METATYPE(orc::PreviewRenderResult)
Q_DECLARE_METATYPE(orc::presenters::VideoParameterObservationView)
Q_DECLARE_METATYPE(orc::presenters::NtscFieldObservationsView)
Q_DECLARE_METATYPE(orc::presenters::TeletextFieldPacketsView)

namespace gui_unit_test {

using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

static bool registerRenderCoordinatorMetatypes() {
  qRegisterMetaType<orc::PreviewRenderResult>("orc::PreviewRenderResult");
  qRegisterMetaType<orc::presenters::VideoParameterObservationView>(
      "orc::presenters::VideoParameterObservationView");
  qRegisterMetaType<orc::presenters::NtscFieldObservationsView>(
      "orc::presenters::NtscFieldObservationsView");
  qRegisterMetaType<orc::presenters::TeletextFieldPacketsView>(
      "orc::presenters::TeletextFieldPacketsView");
  return true;
}

static const bool kMetatypesRegistered = registerRenderCoordinatorMetatypes();

static bool waitForCount(QSignalSpy& spy, int expected, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (spy.count() >= expected) {
      return true;
    }
    QThread::msleep(5);
  }
  return spy.count() >= expected;
}

TEST(RenderCoordinatorTest, TriggerRequestRoundTrip_EmitsTriggerComplete) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);
  EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(7), testing::_))
      .WillOnce(Return(1001));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy trigger_complete_spy(&coordinator,
                                  &RenderCoordinator::triggerComplete);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(123));

  const uint64_t request_id = coordinator.requestTrigger(orc::NodeID(7));

  ASSERT_TRUE(waitForCount(trigger_complete_spy, 1));
  ASSERT_EQ(trigger_complete_spy.count(), 1);
  EXPECT_EQ(trigger_complete_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(trigger_complete_spy.at(0).at(1).toBool());

  coordinator.stop();
}

TEST(RenderCoordinatorTest, WorkerLifecycleStartStop_IsStable) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  coordinator.start();
  coordinator.stop();

  coordinator.start();
  coordinator.stop();
}

TEST(RenderCoordinatorTest, TriggerRequests_AreProcessedInOrder) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  {
    InSequence seq;
    EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(1), testing::_))
        .WillOnce(Invoke(
            [](orc::NodeID,
               orc::presenters::IRenderPresenter::TriggerProgressCallback) {
              std::this_thread::sleep_for(std::chrono::milliseconds(15));
              return 2001ULL;
            }));
    EXPECT_CALL(*mock_presenter, triggerStage(orc::NodeID(2), testing::_))
        .WillOnce(Return(2002ULL));
  }

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy trigger_complete_spy(&coordinator,
                                  &RenderCoordinator::triggerComplete);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(234));

  const uint64_t first_id = coordinator.requestTrigger(orc::NodeID(1));
  const uint64_t second_id = coordinator.requestTrigger(orc::NodeID(2));

  ASSERT_TRUE(waitForCount(trigger_complete_spy, 2));
  ASSERT_EQ(trigger_complete_spy.count(), 2);

  EXPECT_EQ(trigger_complete_spy.at(0).at(0).toULongLong(), first_id);
  EXPECT_EQ(trigger_complete_spy.at(1).at(0).toULongLong(), second_id);

  coordinator.stop();
}

TEST(RenderCoordinatorTest, StalePreviewResponses_AreSuppressed) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  EXPECT_CALL(*mock_presenter, setDAG(testing::_)).Times(1);
  EXPECT_CALL(*mock_presenter, setShowDropouts(false)).Times(1);

  EXPECT_CALL(*mock_presenter,
              renderPreview(orc::NodeID(9),
                            orc::PreviewOutputType::Frame_Field1, 0, ""))
      .WillOnce(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return orc::PreviewRenderResult{
                {}, true, "", node_id, output_type, output_index, std::nullopt};
          }))
      .WillOnce(
          Invoke([](orc::NodeID node_id, orc::PreviewOutputType output_type,
                    uint64_t output_index, const std::string&) {
            return orc::PreviewRenderResult{
                {}, true, "", node_id, output_type, output_index, std::nullopt};
          }));

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy preview_spy(&coordinator, &RenderCoordinator::previewReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(345));

  const uint64_t first_id = coordinator.requestPreview(
      orc::NodeID(9), orc::PreviewOutputType::Frame_Field1, 0);
  const uint64_t second_id = coordinator.requestPreview(
      orc::NodeID(9), orc::PreviewOutputType::Frame_Field1, 0);

  ASSERT_TRUE(waitForCount(preview_spy, 1));
  EXPECT_EQ(preview_spy.count(), 1);
  EXPECT_EQ(preview_spy.at(0).at(0).toULongLong(), second_id);
  EXPECT_NE(first_id, second_id);

  coordinator.stop();
}

// Poll the GUI event loop until @p predicate holds or the timeout elapses.
template <typename Predicate>
static bool waitForPredicate(Predicate predicate, int timeout_ms = 2000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (predicate()) {
      return true;
    }
    QThread::msleep(5);
  }
  return predicate();
}

// Phase 3 Task 3.3: an invalidation from the presenter is forwarded to the
// observationsInvalidated signal with the correct node set.
TEST(RenderCoordinatorTest, ObservationInvalidation_ForwardedToSignal) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy invalidation_spy(&coordinator,
                              &RenderCoordinator::observationsInvalidated);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  // The coordinator subscribes when it creates the presenter on the worker.
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 1; }));

  mock_presenter->fireInvalidation({orc::NodeID(3), orc::NodeID(5)});

  ASSERT_TRUE(waitForCount(invalidation_spy, 1));
  ASSERT_EQ(invalidation_spy.count(), 1);
  const auto ids = invalidation_spy.at(0).at(0).value<QVector<int>>();
  EXPECT_EQ(ids, (QVector<int>{3, 5}));

  coordinator.stop();
}

// Once the coordinator unsubscribes (presenter torn down on empty project),
// further invalidations are not delivered.
TEST(RenderCoordinatorTest, ObservationInvalidation_UnsubscribeStopsDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy invalidation_spy(&coordinator,
                              &RenderCoordinator::observationsInvalidated);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 1; }));

  // A null DAG (empty project) tears down the presenter and unsubscribes.
  coordinator.updateDAG(nullptr);
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->invalidationSubscriberCount() == 0; }));

  mock_presenter->fireInvalidation({orc::NodeID(7)});
  QCoreApplication::processEvents();
  EXPECT_EQ(invalidation_spy.count(), 0);

  coordinator.stop();
}

// --- Phase 5 Task 5.1: async observation requests ------------------------

// A store hit answers immediately: the presenter fires the callback
// synchronously and the coordinator emits observationDataReady(available=true).
TEST(RenderCoordinatorTest,
     ObservationRequest_StoreHit_EmitsDataReadyImmediate) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(true);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(10));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  ASSERT_EQ(data_spy.count(), 1);
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());            // available
  EXPECT_EQ(data_spy.at(0).at(2).toULongLong(), 10ull);  // field id value

  coordinator.stop();
}

// A store miss defers: no signal until the awaited frame is delivered later.
TEST(RenderCoordinatorTest,
     ObservationRequest_Miss_EmitsAfterDeferredDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(4));

  // The worker has forwarded the request; it now awaits a deferred delivery.
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 1; }));
  QCoreApplication::processEvents();
  EXPECT_EQ(data_spy.count(), 0);  // nothing delivered yet

  // Background computation finishes -> the frame's observations arrive.
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(/*available=*/true));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());

  coordinator.stop();
}

// Concurrent requests carry distinct ids and each response echoes its own id,
// so a consumer can drop stale (superseded) responses.
TEST(RenderCoordinatorTest, ObservationRequest_DistinctIdsSupportStaleDrop) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::observationDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(4));
  const uint64_t second =
      coordinator.requestObservations(orc::NodeID(2), orc::FieldID(6));
  EXPECT_NE(first, second);

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 2; }));

  // Deliver both (oldest first). Each response carries its originating id.
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));

  ASSERT_TRUE(waitForCount(data_spy, 2));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), first);
  EXPECT_EQ(data_spy.at(1).at(0).toULongLong(), second);

  coordinator.stop();
}

// --- Teletext preview: GetTeletextData request/response -------------------

namespace {

// Seed one recovered packet on a field of the mock's delivered context.
std::array<uint8_t, orc::kTeletextPacketBytes> seedTeletextField(
    orc::presenters::test::MockRenderPresenter& mock, uint64_t field_value,
    int field_line, uint8_t byte_seed) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  for (size_t i = 0; i < packet.size(); ++i) {
    packet[i] = static_cast<uint8_t>(byte_seed ^ (i * 7));
  }
  const orc::FieldID field(field_value);
  mock.deliveredContext().set(field, "teletext", "present", true);
  mock.deliveredContext().set(field, "teletext", "line_count", int32_t{1});
  mock.deliveredContext().set(field, "teletext",
                              "t42_" + std::to_string(field_line),
                              orc::teletext_packet_to_hex(packet));
  return packet;
}

}  // namespace

// A store hit answers immediately with the extracted packet views for both
// fields of the requested frame (the request may name either field).
TEST(RenderCoordinatorTest, TeletextRequest_StoreHit_EmitsBothFieldsOfFrame) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(true);
  const auto field8_packet = seedTeletextField(*mock_presenter, 8, 7, 0x2A);
  const auto field9_packet = seedTeletextField(*mock_presenter, 9, 16, 0x55);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::teletextDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  // Request via the frame's SECOND field; the response still carries the
  // frame's fields in temporal order (8 then 9).
  const uint64_t request_id =
      coordinator.requestTeletextData(orc::NodeID(2), orc::FieldID(9));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  ASSERT_EQ(data_spy.count(), 1);
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());
  EXPECT_EQ(data_spy.at(0).at(2).toULongLong(), 8ull);
  EXPECT_EQ(data_spy.at(0).at(4).toULongLong(), 9ull);

  const auto field1 =
      data_spy.at(0).at(3).value<orc::presenters::TeletextFieldPacketsView>();
  ASSERT_EQ(field1.packets.size(), 1u);
  EXPECT_EQ(field1.packets[0].field_line, 7);
  EXPECT_EQ(field1.packets[0].bytes, field8_packet);

  const auto field2 =
      data_spy.at(0).at(5).value<orc::presenters::TeletextFieldPacketsView>();
  ASSERT_EQ(field2.packets.size(), 1u);
  EXPECT_EQ(field2.packets[0].field_line, 16);
  EXPECT_EQ(field2.packets[0].bytes, field9_packet);

  coordinator.stop();
}

// A store miss defers: no signal until the awaited frame is delivered later.
TEST(RenderCoordinatorTest, TeletextRequest_Miss_EmitsAfterDeferredDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::teletextDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t request_id =
      coordinator.requestTeletextData(orc::NodeID(2), orc::FieldID(4));

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 1; }));
  QCoreApplication::processEvents();
  EXPECT_EQ(data_spy.count(), 0);  // nothing delivered yet

  ASSERT_TRUE(mock_presenter->deliverOldestObservation(/*available=*/true));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), request_id);
  EXPECT_TRUE(data_spy.at(0).at(1).toBool());

  coordinator.stop();
}

// An unavailable delivery reports available=false with empty packet views.
TEST(RenderCoordinatorTest, TeletextRequest_Unavailable_EmitsEmptyViews) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::teletextDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  coordinator.requestTeletextData(orc::NodeID(2), orc::FieldID(4));
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 1; }));
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(/*available=*/false));

  ASSERT_TRUE(waitForCount(data_spy, 1));
  EXPECT_FALSE(data_spy.at(0).at(1).toBool());
  const auto field1 =
      data_spy.at(0).at(3).value<orc::presenters::TeletextFieldPacketsView>();
  EXPECT_FALSE(field1.observed);
  EXPECT_TRUE(field1.packets.empty());

  coordinator.stop();
}

// Concurrent requests carry distinct ids and each response echoes its own id,
// so a consumer can drop stale (superseded) responses.
TEST(RenderCoordinatorTest, TeletextRequest_DistinctIdsSupportStaleDrop) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();
  mock_presenter->setImmediateAnswer(false);

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy data_spy(&coordinator, &RenderCoordinator::teletextDataReady);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  const uint64_t first =
      coordinator.requestTeletextData(orc::NodeID(2), orc::FieldID(4));
  const uint64_t second =
      coordinator.requestTeletextData(orc::NodeID(2), orc::FieldID(6));
  EXPECT_NE(first, second);

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->pendingObservationCount() == 2; }));

  // Deliver both (oldest first). Each response carries its originating id.
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));
  ASSERT_TRUE(mock_presenter->deliverOldestObservation(true));

  ASSERT_TRUE(waitForCount(data_spy, 2));
  EXPECT_EQ(data_spy.at(0).at(0).toULongLong(), first);
  EXPECT_EQ(data_spy.at(1).at(0).toULongLong(), second);

  coordinator.stop();
}

// --- Phase 5 Task 5.4: background-workload progress ----------------------

// A scheduler workload snapshot is forwarded to the observationProgress signal.
TEST(RenderCoordinatorTest, ObservationProgress_ForwardedToSignal) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy progress_spy(&coordinator,
                          &RenderCoordinator::observationProgress);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));

  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 1; }));

  orc::presenters::ObservationProgressEvent event;
  event.active = true;
  event.percent_complete = 42;
  event.computing = true;
  event.outstanding_nodes = 3;
  mock_presenter->fireProgress(event);

  ASSERT_TRUE(waitForCount(progress_spy, 1));
  EXPECT_TRUE(progress_spy.at(0).at(0).toBool());
  EXPECT_EQ(progress_spy.at(0).at(1).toInt(), 42);
  EXPECT_TRUE(progress_spy.at(0).at(2).toBool());
  EXPECT_EQ(progress_spy.at(0).at(3).toULongLong(), 3ull);

  // Idle snapshot delivered when the queue drains.
  orc::presenters::ObservationProgressEvent idle;
  mock_presenter->fireProgress(idle);
  ASSERT_TRUE(waitForCount(progress_spy, 2));
  EXPECT_FALSE(progress_spy.at(1).at(0).toBool());

  coordinator.stop();
}

// Progress delivery stops once the coordinator tears the presenter down.
TEST(RenderCoordinatorTest, ObservationProgress_UnsubscribeStopsDelivery) {
  (void)kMetatypesRegistered;

  auto mock_presenter =
      std::make_shared<NiceMock<orc::presenters::test::MockRenderPresenter>>();

  RenderCoordinator coordinator(
      [mock_presenter](
          void*) -> std::shared_ptr<orc::presenters::IRenderPresenter> {
        return mock_presenter;
      });

  QSignalSpy progress_spy(&coordinator,
                          &RenderCoordinator::observationProgress);

  coordinator.start();
  coordinator.setProject(reinterpret_cast<void*>(0x1));
  coordinator.updateDAG(std::make_shared<int>(1));
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 1; }));

  coordinator.updateDAG(nullptr);  // tears down the presenter, unsubscribes
  ASSERT_TRUE(waitForPredicate(
      [&] { return mock_presenter->progressSubscriberCount() == 0; }));

  orc::presenters::ObservationProgressEvent event;
  event.active = true;
  mock_presenter->fireProgress(event);
  QCoreApplication::processEvents();
  EXPECT_EQ(progress_spy.count(), 0);

  coordinator.stop();
}

}  // namespace gui_unit_test
