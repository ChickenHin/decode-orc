/*
 * File:        teletext_analysis_sink_deps_interface_mock.h
 * Module:      orc-core-tests
 * Purpose:     gMock double for ITeletextAnalysisSinkStageDeps
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef DECODE_ORC_ROOT_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_MOCK_H
#define DECODE_ORC_ROOT_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_MOCK_H

#include <gmock/gmock.h>

#include "teletext_analysis_sink_deps_interface.h"

// using different namespace from module-under-test so that we can use the same
// class names in the tests as in the module-under-test
namespace orc_unit_test {

class MockTeletextAnalysisSinkStageDeps
    : public orc::ITeletextAnalysisSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));

  MOCK_METHOD(orc::TeletextAnalysisSinkResult, analyse,
              (const orc::VideoFrameRepresentation* representation,
               const orc::TeletextAnalysisSinkOptions& options),
              (override));
};

}  // namespace orc_unit_test

#endif  // DECODE_ORC_ROOT_TELETEXT_ANALYSIS_SINK_DEPS_INTERFACE_MOCK_H
