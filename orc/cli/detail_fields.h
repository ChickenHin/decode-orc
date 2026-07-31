/*
 * File:        detail_fields.h
 * Module:      orc-cli
 * Purpose:     Print a presenter-built label/value field list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace orc {
namespace cli {

// The presenter decides which fields describe a plugin or a stage, in what
// order and with what labels; all the CLI adds is the column. Every listing
// that prints a field list shares this, so `plugins info` and `stages info`
// cannot drift into laying their output out differently.
//
// Templated on the field type rather than tied to one: a field is anything
// with a `label` and a `value`, and the presenter has one such type per kind
// of thing it describes.

/// Widest label in a field list, so a caller can align several lists against
/// one another rather than letting each block find its own column.
template <typename Field>
std::size_t widest_label(const std::vector<Field>& fields) {
  std::size_t width = 0;
  for (const auto& field : fields) {
    width = std::max(width, field.label.size());
  }
  return width;
}

/**
 * @brief Print a field list with the labels column-aligned.
 *
 * @param fields Presenter-built fields, in the order they should read.
 * @param indent Written before every line.
 * @param width Column to align to; widened to fit @p fields. Pass the widest
 *              label across several lists to align them all as one table.
 */
template <typename Field>
void print_detail_fields(const std::vector<Field>& fields,
                         const std::string& indent = std::string(),
                         std::size_t width = 0) {
  width = std::max(width, widest_label(fields));
  for (const auto& field : fields) {
    std::cout << indent << field.label << ":"
              << std::string(width - field.label.size() + 1, ' ') << field.value
              << "\n";
  }
}

}  // namespace cli
}  // namespace orc
