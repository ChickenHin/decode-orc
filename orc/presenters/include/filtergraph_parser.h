/*
 * File:        filtergraph_parser.h
 * Module:      orc-cli
 * Purpose:     Parse an ffmpeg-style complex filtergraph string into a
 *              stage/edge description that can drive the project presenter.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace orc {
namespace cli {

/**
 * @brief A single stage instance parsed from a filtergraph.
 *
 * Mirrors one ffmpeg "filter": an optional set of input link labels, a stage
 * name, its key=value parameters, and an optional set of output link labels.
 */
struct FilterStage {
  std::string stage_name;                     ///< e.g. "tbc_source"
  std::map<std::string, std::string> params;  ///< key -> value
  std::vector<std::string> input_labels;      ///< leading [labels]
  std::vector<std::string> output_labels;     ///< trailing [labels]
};

/**
 * @brief Fully parsed filtergraph.
 *
 * `stages` are listed in the order they appear. `edges` are resolved
 * connections expressed as (from_stage_index, to_stage_index) pairs, combining
 * both implicit comma-chain adjacency and explicit shared-label links.
 */
struct FilterGraph {
  std::vector<FilterStage> stages;
  std::vector<std::pair<std::size_t, std::size_t>> edges;
};

/**
 * @brief Result of parsing a filtergraph string.
 *
 * On failure, `ok` is false and `error` contains a human-readable description
 * (with the approximate character offset where parsing failed).
 */
struct FilterGraphParseResult {
  bool ok = false;
  std::string error;
  FilterGraph graph;
};

/**
 * @brief Parse an ffmpeg-style complex filtergraph.
 *
 * Grammar (informal):
 * @code
 *   graph       := filterchain (';' filterchain)*
 *   filterchain := filter (',' filter)*
 *   filter      := inlabels? stagespec outlabels?
 *   inlabels    := ('[' label ']')+
 *   outlabels   := ('[' label ']')+
 *   stagespec   := stage_name ('=' args)?
 *   args        := arg (':' arg)*
 *   arg         := key '=' value
 * @endcode
 *
 * Connection rules:
 * - Within a filterchain, a filter that has no explicit input labels is
 *   auto-connected to the immediately preceding filter in the same chain.
 * - Any output label is connected to every filter that lists the same name as
 *   an input label, anywhere in the graph (ffmpeg-style shared labels).
 *
 * Quoting: values may be wrapped in single OR double quotes ('...' or "...")
 * to include ':' '[' ']' ',' ';' and whitespace literally; the two quote
 * styles are interchangeable and a value quoted with one may freely contain
 * the other character. A backslash escapes the next character anywhere
 * outside quotes. Leading/trailing unquoted whitespace is trimmed.
 *
 * @param input Filtergraph string.
 * @return Parse result; check `.ok` before using `.graph`.
 */
FilterGraphParseResult parse_filtergraph(const std::string& input);

}  // namespace cli
}  // namespace orc
