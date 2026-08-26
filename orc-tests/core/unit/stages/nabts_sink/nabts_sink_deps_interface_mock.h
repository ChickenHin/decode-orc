/*
 * File:        nabts_sink_deps_interface_mock.h
 * Module:      orc-tests/core/unit/stages/nabts_sink
 * Purpose:     GoogleMock double for INabtsSinkStageDeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <gmock/gmock.h>

#include "nabts_sink_deps_interface.h"

namespace orc {
namespace tests {

class MockNabtsSinkStageDeps : public INabtsSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));
  MOCK_METHOD(NabtsSinkResult, analyse,
              (const VideoFrameRepresentation* representation,
               const NabtsSinkOptions& options),
              (override));
};

}  // namespace tests
}  // namespace orc
