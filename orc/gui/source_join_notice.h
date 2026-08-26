/*
 * File:        source_join_notice.h
 * Module:      orc-gui
 * Purpose:     Pure helper naming the nodes connected to a Source Join input
 *              (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// withNotice() is the shared "append a note to a stage description" helper;
// it lives alongside the audio notices but is not audio-specific.
#include "audio_channel_pair_notice.h"

namespace orc::gui {

// One node feeding a Source Join stage's input, in the order the stage
// receives it. |name| is the node's user label, or its stage display name when
// the user has not renamed it.
struct ConnectedInputNode {
  int32_t node_id;
  std::string name;
};

// Note shown in the Source Join parameter dialog. The Input Order parameter
// asks for node IDs, and nothing in the form says which numbers those are: a
// multi-input stage has a single input port, so the connections themselves do
// not name their sources. This lists the nodes actually connected, with the
// IDs the graph editor draws on them, so the numbers to type are in front of
// the user while they type them.
inline std::string sourceJoinInputNodesNotice(
    const std::vector<ConnectedInputNode>& inputs) {
  if (inputs.empty()) {
    return "Input Order takes node IDs — the number shown in the corner of "
           "each "
           "node in the graph. Nothing is connected to this stage yet: connect "
           "the sources you want to join first, then list their IDs here in "
           "the "
           "order they should follow one another.";
  }

  std::string list;
  std::string example;
  for (size_t i = 0; i < inputs.size(); ++i) {
    const std::string id = std::to_string(inputs[i].node_id);
    if (i > 0) {
      list += ", ";
      example += ",";
    }
    list += id;
    example += id;
    if (!inputs[i].name.empty()) {
      list += " (" + inputs[i].name + ")";
    }
  }

  return "Input Order takes node IDs — the number shown in the corner of each "
         "node in the graph. Connected to this stage: " +
         list +
         ". List those IDs in the order they should follow one another, for "
         "example " +
         example + ".";
}

// Appends sourceJoinInputNodesNotice() to a stage description.
inline std::string withSourceJoinInputNodesNotice(
    const std::string& description,
    const std::vector<ConnectedInputNode>& inputs) {
  return withNotice(description, sourceJoinInputNodesNotice(inputs));
}

}  // namespace orc::gui
