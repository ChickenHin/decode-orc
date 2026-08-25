/*
 * File:        source_join_notice_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Tier 1 tests for the Source Join connected-inputs notice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../orc/gui/source_join_notice.h"

#include <gtest/gtest.h>

namespace {

using orc::gui::ConnectedInputNode;

// The parameter asks for node IDs and the form gives no clue which numbers
// those are, so the note has to say what they are before it lists them.
TEST(SourceJoinNoticeTest, NamesTheNumbersAsNodeIds) {
  const auto note = orc::gui::sourceJoinInputNodesNotice(
      {ConnectedInputNode{12, "SP decode"}});
  EXPECT_NE(note.find("node ID"), std::string::npos);
}

TEST(SourceJoinNoticeTest, ListsConnectedNodesWithTheirLabels) {
  const auto note = orc::gui::sourceJoinInputNodesNotice(
      {ConnectedInputNode{12, "SP decode"},
       ConnectedInputNode{14, "LP decode"}});
  EXPECT_NE(note.find("12 (SP decode), 14 (LP decode)"), std::string::npos);
}

// The example is the IDs actually connected, so it can be typed as-is.
TEST(SourceJoinNoticeTest, ExampleUsesTheConnectedIds) {
  const auto note = orc::gui::sourceJoinInputNodesNotice(
      {ConnectedInputNode{12, "SP decode"}, ConnectedInputNode{14, ""}});
  EXPECT_NE(note.find("for example 12,14."), std::string::npos);
}

TEST(SourceJoinNoticeTest, UnlabelledNodeShowsItsIdAlone) {
  const auto note =
      orc::gui::sourceJoinInputNodesNotice({ConnectedInputNode{7, ""}});
  EXPECT_NE(note.find("Connected to this stage: 7."), std::string::npos);
}

// With nothing connected there are no IDs to offer, so the note says what to
// do first rather than listing an empty set.
TEST(SourceJoinNoticeTest, NothingConnectedSaysToConnectSourcesFirst) {
  const auto note = orc::gui::sourceJoinInputNodesNotice({});
  EXPECT_NE(note.find("Nothing is connected"), std::string::npos);
}

TEST(SourceJoinNoticeTest, WithNotice_AppendsBelowTheStageDescription) {
  const auto text = orc::gui::withSourceJoinInputNodesNotice(
      "Join several sources.", {ConnectedInputNode{3, "A"}});
  EXPECT_EQ(text.rfind("Join several sources.\n\n", 0), 0u);
  EXPECT_NE(text.find("Connected to this stage: 3 (A)."), std::string::npos);
}

}  // namespace
