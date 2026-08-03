/*
 * File:        project_load_status_formatter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the project-load progress dialog's
 *              label formatting helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "project_load_status_formatter.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

// U+2026 HORIZONTAL ELLIPSIS, as emitted by the formatter.
constexpr const char* kEllipsis = "\xE2\x80\xA6";

TEST(ProjectLoadStatusFormatter, ReportsGenericTextBeforeFirstProgressEvent) {
  // Nothing has executed yet: the worker is still rebuilding renderers.
  EXPECT_EQ(formatProjectLoadStatus("", 0, 0, 0),
            std::string("Preparing preview") + kEllipsis);
}

TEST(ProjectLoadStatusFormatter, NamesTheExecutingStage) {
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 1, 3, 0),
            std::string("Executing tbc_source (1 of 3)") + kEllipsis);
}

TEST(ProjectLoadStatusFormatter, OmitsPositionForSingleNodeExecutions) {
  // "1 of 1" tells the user nothing, so a single-node execution just names it.
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 1, 1, 0),
            std::string("Executing tbc_source") + kEllipsis);
}

TEST(ProjectLoadStatusFormatter, AppendsElapsedSecondsOncePastOneSecond) {
  // Sub-second loads would flicker between two texts, so the counter starts at
  // 1s — the point at which a stalled-looking window needs the reassurance.
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 1, 1, 0),
            std::string("Executing tbc_source") + kEllipsis);
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 1, 1, 1),
            std::string("Executing tbc_source") + kEllipsis + " 1s");
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 2, 4, 47),
            std::string("Executing tbc_source (2 of 4)") + kEllipsis + " 47s");
  EXPECT_EQ(formatProjectLoadStatus("", 0, 0, 12),
            std::string("Preparing preview") + kEllipsis + " 12s");
}

TEST(ProjectLoadStatusFormatter, FallsBackToGenericTextOnInconsistentInput) {
  // A labelled node with a zero total cannot be positioned in an execution
  // order, so the honest text is the generic one.
  EXPECT_EQ(formatProjectLoadStatus("tbc_source", 0, 0, 3),
            std::string("Preparing preview") + kEllipsis + " 3s");
}

}  // namespace
}  // namespace orc::gui
