/*
 * File:        tbc_sink_stage_deps_interface_mock.h
 * Module:      orc-core-tests
 * Purpose:     Mock to support unit tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_TESTS_TBC_SINK_STAGE_DEPS_INTERFACE_MOCK_H
#define ORC_CORE_TESTS_TBC_SINK_STAGE_DEPS_INTERFACE_MOCK_H

#include <gmock/gmock.h>

#include <cstddef>
#include <string>

#include "tbc_sink_stage_deps_interface.h"

namespace orc_unit_test {
using orc::IObservationContext;
using orc::VideoFrameRepresentation;

class MockTBCSinkStageDeps : public orc::ITBCSinkStageDeps {
 public:
  MOCK_METHOD(bool, write_tbc_and_metadata,
              (const VideoFrameRepresentation*, const std::string&, size_t,
               IObservationContext&),
              (override));
};
}  // namespace orc_unit_test

#endif  // ORC_CORE_TESTS_TBC_SINK_STAGE_DEPS_INTERFACE_MOCK_H
