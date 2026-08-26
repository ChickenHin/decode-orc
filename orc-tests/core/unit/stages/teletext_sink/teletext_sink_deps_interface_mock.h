/*
 * File:        teletext_sink_deps_interface_mock.h
 * Module:      orc-core-tests
 * Purpose:     gMock double for ITeletextSinkStageDeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef DECODE_ORC_ROOT_TELETEXT_SINK_DEPS_INTERFACE_MOCK_H
#define DECODE_ORC_ROOT_TELETEXT_SINK_DEPS_INTERFACE_MOCK_H

#include <gmock/gmock.h>

#include "teletext_sink_deps_interface.h"

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

class MockTeletextSinkStageDeps : public orc::ITeletextSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));

  MOCK_METHOD(orc::TeletextSinkResult, analyse,
              (const orc::VideoFrameRepresentation* representation,
               const orc::TeletextSinkOptions& options),
              (override));
};

}  // namespace orc_unit_test

#endif  // DECODE_ORC_ROOT_TELETEXT_SINK_DEPS_INTERFACE_MOCK_H
