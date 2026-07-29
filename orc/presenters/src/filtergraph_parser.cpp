/*
 * File:        filtergraph_parser.cpp
 * Module:      orc-presenters
 * Purpose:     Implementation of the ffmpeg-style filtergraph parser.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 *
 * This translation unit deliberately depends only on the C++ standard library
 * so the parser can be unit-tested in isolation from orc-core/presenters.
 */

#include "filtergraph_parser.h"

#include <cctype>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace orc {
namespace presenters {

namespace {

using std::size_t;

bool is_ws(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

/**
 * Detect a Windows drive-letter colon such as the one in "C:\Users\..." or
 * "D:/data". Such a colon must never be treated as a top-level ':' argument
 * separator, or an unquoted Windows absolute path silently corrupts parsing.
 *
 * Matches when: s[i] == ':', the preceding character is a single ASCII
 * letter that starts a token (i.e. is at the very start of the string or is
 * preceded by a non-alphanumeric, non-underscore character), and the
 * following character is '\' or '/'.
 */
bool is_drive_letter_colon(const std::string& s, size_t i) {
  if (s[i] != ':' || i == 0 || i + 1 >= s.size()) {
    return false;
  }
  unsigned char prev = static_cast<unsigned char>(s[i - 1]);
  if (!std::isalpha(prev)) {
    return false;
  }
  if (i >= 2) {
    unsigned char before_prev = static_cast<unsigned char>(s[i - 2]);
    if (std::isalnum(before_prev) || before_prev == '_') {
      return false;  // e.g. "abc:" - not a standalone drive letter
    }
  }
  return s[i + 1] == '\\' || s[i + 1] == '/';
}

/// A half-open [begin, end) slice of the source string.
struct Range {
  size_t begin = 0;
  size_t end = 0;
  bool empty() const { return begin >= end; }
};

/// Error carrier used throughout parsing.
struct ParseError {
  bool set = false;
  std::string message;
  size_t offset = 0;

  void fail(const std::string& msg, size_t off) {
    if (!set) {  // keep first error
      set = true;
      message = msg;
      offset = off;
    }
  }
};

/**
 * Split [begin, end) of `s` at top-level occurrences of any separator in
 * `seps`. "Top level" means not inside a quoted span (single OR double
 * quotes), not escaped by a preceding backslash, and not nested inside
 * square brackets. Returns the list of sub-ranges (raw, not unescaped, not
 * trimmed). Empty ranges are preserved so callers can diagnose things like
 * "a,,b".
 */
std::vector<Range> split_top_level(const std::string& s, size_t begin,
                                   size_t end, const std::string& seps,
                                   ParseError& err) {
  std::vector<Range> out;
  size_t seg_start = begin;
  size_t i = begin;
  int depth = 0;
  char quote_char = '\0';  // '\0' = not currently inside a quoted span

  while (i < end) {
    char c = s[i];

    if (quote_char != '\0') {
      if (c == quote_char) {
        quote_char = '\0';
      }
      ++i;
      continue;
    }

    if (c == '\\') {  // escape: skip next char literally
      i += 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote_char = c;
      ++i;
      continue;
    }
    if (c == '[') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ']') {
      if (depth > 0) {
        --depth;
      }
      ++i;
      continue;
    }

    if (depth == 0 && seps.find(c) != std::string::npos) {
      if (c == ':' && is_drive_letter_colon(s, i)) {
        ++i;
        continue;
      }
      out.push_back(Range{seg_start, i});
      seg_start = i + 1;
      ++i;
      continue;
    }
    ++i;
  }

  if (quote_char != '\0') {
    err.fail(std::string("Unterminated ") +
                 (quote_char == '\'' ? "single" : "double") + " quote",
             begin);
  }
  if (depth > 0) {
    err.fail("Unbalanced '[' in filtergraph", begin);
  }

  out.push_back(Range{seg_start, end});
  return out;
}

/// Trim leading/trailing unquoted ASCII whitespace from a range in place.
Range trim(const std::string& s, Range r) {
  while (r.begin < r.end && is_ws(s[r.begin])) {
    ++r.begin;
  }
  while (r.end > r.begin && is_ws(s[r.end - 1])) {
    --r.end;
  }
  return r;
}

/**
 * Produce the literal value of a token, resolving quoting (single OR double
 * quotes) and backslash escaping. Backslash outside quotes escapes the
 * following character when it is one of the syntax-special characters;
 * inside a quoted span everything is literal until the matching closing
 * quote (the other quote character is not special inside it, so a value can
 * freely contain the other kind of quote character).
 */
std::string resolve_token(const std::string& s, Range r) {
  std::string out;
  size_t i = r.begin;
  while (i < r.end) {
    char c = s[i];
    if (c == '\\' && i + 1 < r.end) {
      char next = s[i + 1];
      static const std::string kEscapable = ":,;[]'\" \t\\";
      if (kEscapable.find(next) != std::string::npos) {
        out.push_back(next);
        i += 2;
        continue;
      }
      // Not a recognised escape (e.g. a Windows path like "C:\Users\..."):
      // keep the backslash literally and let the following character be
      // processed normally on the next iteration.
      out.push_back(c);
      ++i;
      continue;
    }
    if (c == '\'' || c == '"') {
      const char quote_char = c;
      ++i;
      while (i < r.end && s[i] != quote_char) {
        out.push_back(s[i]);
        ++i;
      }
      if (i < r.end) {
        ++i;  // consume closing quote
      }
      continue;
    }
    out.push_back(c);
    ++i;
  }
  return out;
}

/**
 * Consume a run of `[label]` groups starting at r.begin (whitespace allowed
 * between groups). Appends resolved labels to `labels` and returns the offset
 * just past the last consumed group. Stops as soon as the next non-whitespace
 * character is not '['.
 */
size_t consume_label_groups(const std::string& s, Range r,
                            std::vector<std::string>& labels, ParseError& err) {
  size_t i = r.begin;
  while (true) {
    while (i < r.end && is_ws(s[i])) {
      ++i;
    }
    if (i >= r.end || s[i] != '[') {
      return i;
    }
    size_t label_start = i + 1;
    size_t j = label_start;
    while (j < r.end && s[j] != ']') {
      ++j;
    }
    if (j >= r.end) {
      err.fail("Unterminated '[' label", i);
      return r.end;
    }
    Range inner = trim(s, Range{label_start, j});
    if (inner.empty()) {
      err.fail("Empty link label '[]'", i);
      return r.end;
    }
    labels.push_back(resolve_token(s, inner));
    i = j + 1;  // past ']'
  }
}

/// Find the first top-level '[' in [begin, end) (unquoted, unescaped). Returns
/// `end` if none is found.
size_t find_top_level_open_bracket(const std::string& s, size_t begin,
                                   size_t end) {
  size_t i = begin;
  char quote_char = '\0';
  while (i < end) {
    char c = s[i];
    if (quote_char != '\0') {
      if (c == quote_char) {
        quote_char = '\0';
      }
      ++i;
      continue;
    }
    if (c == '\\') {
      i += 2;
      continue;
    }
    if (c == '\'' || c == '"') {
      quote_char = c;
      ++i;
      continue;
    }
    if (c == '[') {
      return i;
    }
    ++i;
  }
  return end;
}

/// Parse the "key=value:key=value" argument list of a stage.
void parse_args(const std::string& s, Range r, FilterStage& stage,
                ParseError& err) {
  r = trim(s, r);
  if (r.empty()) {
    return;
  }
  std::vector<Range> args = split_top_level(s, r.begin, r.end, ":", err);
  for (const Range& raw : args) {
    Range arg = trim(s, raw);
    if (arg.empty()) {
      err.fail("Empty parameter (stray ':')", raw.begin);
      continue;
    }
    // Split on the first top-level '='.
    std::vector<Range> kv = split_top_level(s, arg.begin, arg.end, "=", err);
    if (kv.size() < 2) {
      err.fail("Parameter '" + resolve_token(s, arg) +
                   "' must be of the form key=value",
               arg.begin);
      continue;
    }
    Range key_r = trim(s, kv[0]);
    // Re-join everything after the first '=' as the value (values may contain
    // '=', e.g. base64 or query strings).
    Range val_r{kv[1].begin, arg.end};
    val_r = trim(s, val_r);
    std::string key = resolve_token(s, key_r);
    if (key.empty()) {
      err.fail("Empty parameter name before '='", arg.begin);
      continue;
    }
    stage.params[key] = resolve_token(s, val_r);
  }
}

/// Parse a single filter: [in]... stage=args [out]...
void parse_filter(const std::string& s, Range r, FilterStage& stage,
                  ParseError& err) {
  r = trim(s, r);
  if (r.empty()) {
    err.fail("Empty filter (stray ',' or ';')", r.begin);
    return;
  }

  // 1. Leading input labels.
  size_t after_inputs = consume_label_groups(s, r, stage.input_labels, err);
  if (err.set) {
    return;
  }

  // 2. Stage spec runs until the first top-level '[' (start of output labels)
  //    or the end of the range.
  size_t out_start = find_top_level_open_bracket(s, after_inputs, r.end);
  Range spec = trim(s, Range{after_inputs, out_start});
  if (spec.empty()) {
    err.fail("Missing stage name", after_inputs);
    return;
  }

  // 3. Trailing output labels (if any).
  if (out_start < r.end) {
    size_t after_outputs = consume_label_groups(s, Range{out_start, r.end},
                                                stage.output_labels, err);
    if (err.set) {
      return;
    }
    Range leftover = trim(s, Range{after_outputs, r.end});
    if (!leftover.empty()) {
      err.fail("Unexpected text after output labels: '" +
                   resolve_token(s, leftover) + "'",
               after_outputs);
      return;
    }
  }

  // 4. Split stage spec into name and optional args on first top-level '='.
  std::vector<Range> name_args =
      split_top_level(s, spec.begin, spec.end, "=", err);
  if (err.set) {
    return;
  }
  Range name_r = trim(s, name_args[0]);
  stage.stage_name = resolve_token(s, name_r);
  if (stage.stage_name.empty()) {
    err.fail("Empty stage name", spec.begin);
    return;
  }
  if (name_args.size() >= 2) {
    Range args_r{name_args[1].begin, spec.end};
    parse_args(s, args_r, stage, err);
  }
}

/// Resolve edges from parsed chains: implicit comma adjacency + shared labels.
void resolve_edges(FilterGraph& graph,
                   const std::vector<std::vector<size_t>>& chains) {
  std::set<std::pair<size_t, size_t>> seen;
  auto add_edge = [&](size_t from, size_t to) {
    if (from == to) {
      return;
    }
    if (seen.insert({from, to}).second) {
      graph.edges.emplace_back(from, to);
    }
  };

  // Implicit adjacency: within a chain, connect consecutive filters when the
  // downstream filter has no explicit input labels.
  for (const auto& chain : chains) {
    for (size_t k = 1; k < chain.size(); ++k) {
      size_t prev = chain[k - 1];
      size_t cur = chain[k];
      if (graph.stages[cur].input_labels.empty()) {
        add_edge(prev, cur);
      }
    }
  }

  // Explicit shared labels: an output label connects to any filter that lists
  // the same name as an input label.
  for (size_t producer = 0; producer < graph.stages.size(); ++producer) {
    for (const std::string& out_label : graph.stages[producer].output_labels) {
      for (size_t consumer = 0; consumer < graph.stages.size(); ++consumer) {
        const auto& in = graph.stages[consumer].input_labels;
        for (const std::string& in_label : in) {
          if (in_label == out_label) {
            add_edge(producer, consumer);
          }
        }
      }
    }
  }
}

}  // namespace

FilterGraphParseResult parse_filtergraph(const std::string& input) {
  FilterGraphParseResult result;
  ParseError err;

  Range whole = trim(input, Range{0, input.size()});
  if (whole.empty()) {
    result.error = "Empty filtergraph";
    return result;
  }

  // Split into filterchains on ';'.
  std::vector<Range> chain_ranges =
      split_top_level(input, whole.begin, whole.end, ";", err);
  if (err.set) {
    result.error =
        err.message + " (at offset " + std::to_string(err.offset) + ")";
    return result;
  }

  std::vector<std::vector<size_t>> chains;
  for (const Range& chain_raw : chain_ranges) {
    Range chain_r = trim(input, chain_raw);
    if (chain_r.empty()) {
      err.fail("Empty filterchain (stray ';')", chain_raw.begin);
      break;
    }

    std::vector<Range> filter_ranges =
        split_top_level(input, chain_r.begin, chain_r.end, ",", err);
    if (err.set) {
      break;
    }

    std::vector<size_t> chain_indices;
    for (const Range& filter_raw : filter_ranges) {
      FilterStage stage;
      parse_filter(input, filter_raw, stage, err);
      if (err.set) {
        break;
      }
      chain_indices.push_back(result.graph.stages.size());
      result.graph.stages.push_back(std::move(stage));
    }
    if (err.set) {
      break;
    }
    chains.push_back(std::move(chain_indices));
  }

  if (err.set) {
    result.error =
        err.message + " (at offset " + std::to_string(err.offset) + ")";
    return result;
  }

  resolve_edges(result.graph, chains);
  result.ok = true;
  return result;
}

}  // namespace presenters
}  // namespace orc
