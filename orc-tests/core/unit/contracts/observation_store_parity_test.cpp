/*
 * File:        observation_store_parity_test.cpp
 * Module:      orc-core tests
 * Purpose:     Consumer parity — store-sourced vs freshly-observed context
 *              contents are identical for a mocked frame (Task 2.4)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <string>
#include <vector>

#include "../observation/observation_test_doubles.h"
#include "dag_frame_renderer.h"
#include "frame_provenance.h"
#include "observation_store.h"

namespace orc {
namespace {

using test::FakeVideoFrameRepresentation;
using test::make_observer_info;
using test::SpyObservationService;

// Every consumer (MetricsPresenter, VbiPresenter, cc_sink, observation
// presenters) reads observations exclusively through IObservationContext. So a
// sufficient parity guarantee is: the context a consumer sees when values are
// served from the store is byte-for-byte identical to the context produced by a
// fresh observer run. This exercises all ObservationValue variants across
// multiple namespaces and both fields of the frame.
TEST(ObservationStoreParity_StoreSourcedMatchesFreshRun, IdenticalContexts) {
  const std::vector<ObserverInfo> observers = {
      make_observer_info("white_snr", "1.0.0"),
      make_observer_info("biphase", "2.0.0"),
      make_observer_info("closed_caption", "1.3.0")};
  const NodeFingerprint fp{"parity-node-fingerprint"};
  const FrameID frame = 7;

  FakeVideoFrameRepresentation vfr;

  // Reference: a fresh run with no caching (observers write straight to
  // context).
  SpyObservationService fresh_service;
  ObservationContext fresh_ctx;
  run_frame_observer_pass(fresh_service, observers, vfr, frame,
                          /*fingerprint=*/nullptr, /*store=*/nullptr,
                          fresh_ctx);

  // Store-sourced: first pass populates the store (miss), second pass serves
  // it.
  SpyObservationService store_service;
  ObservationStore store;
  ObservationContext warm_ctx;
  run_frame_observer_pass(store_service, observers, vfr, frame, &fp, &store,
                          warm_ctx);
  const int runs_after_first = store_service.total_runs;

  ObservationContext store_ctx;
  run_frame_observer_pass(store_service, observers, vfr, frame, &fp, &store,
                          store_ctx);
  // Second pass served entirely from the store.
  EXPECT_EQ(store_service.total_runs, runs_after_first);

  // Parity across both fields of the frame.
  for (FieldID::value_type f = 0; f < 2; ++f) {
    const FieldID field(frame * 2 + f);
    EXPECT_EQ(fresh_ctx.get_all_observations(field),
              store_ctx.get_all_observations(field))
        << "field " << field.value();
    // And the warm (populate) pass matches too.
    EXPECT_EQ(warm_ctx.get_all_observations(field),
              store_ctx.get_all_observations(field))
        << "warm vs store, field " << field.value();
  }
}

}  // namespace
}  // namespace orc
