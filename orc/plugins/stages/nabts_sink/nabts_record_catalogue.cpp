/*
 * File:        nabts_record_catalogue.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Bounded NABTS record catalogue implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record_catalogue.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace orc {

namespace {

/// Function descriptor arguments as text: printable code-table characters as
/// themselves, everything else as its hexadecimal value.
std::string arguments_text(const std::vector<uint8_t>& arguments) {
  std::string out;
  out.reserve(arguments.size());
  for (const uint8_t argument : arguments) {
    // §7.2.2 confines arguments to 2/0 through 7/15, which is exactly the
    // printable range of the code table, so anything outside it is damage.
    if (argument >= 0x20 && argument < 0x7F) {
      out.push_back(static_cast<char>(argument));
    } else {
      out += fmt::format("<{:02X}>", argument);
    }
  }
  return out;
}

}  // namespace

NabtsRecordCatalogue::NabtsRecordCatalogue(std::size_t max_records)
    : max_records_(std::max<std::size_t>(1, max_records)) {}

bool NabtsRecordCatalogue::copy_is_better(const Entry& entry,
                                          const NabtsMessage& message) {
  const bool candidate_intact = message.complete && message.intact;
  if (candidate_intact != entry.kept_copy_intact) {
    // A clean copy always beats a damaged one, and never the other way round.
    return candidate_intact;
  }
  // Equal quality: the longer copy carries more of the record. Equal length
  // keeps what is already there rather than churning between identical copies.
  return message.data.size() > entry.record.data.size();
}

void NabtsRecordCatalogue::take_copy(Entry& entry,
                                     const NabtsMessage& message) {
  NabtsCataloguedRecord& record = entry.record;
  record.record_type = message.type;
  record.caption = message.classification.caption;
  record.cyclic_marker = message.classification.cyclic_marker;
  record.priority = message.classification.priority;
  record.alarm = message.classification.alarm;
  record.update = message.classification.update;
  record.support_record = message.classification.support_record;
  record.index = message.classification.index;
  record.more = message.classification.more;
  record.records_in_message = message.records;
  record.complete = message.complete;
  record.data = message.data;

  record.functions.clear();
  record.functions.reserve(message.functions.size());
  for (const NabtsFunctionDescriptor& function : message.functions) {
    NabtsRecordFunction out;
    out.code = function.code_text();
    out.control = function.is_control();
    out.arguments = arguments_text(function.arguments);
    record.functions.push_back(std::move(out));
  }

  entry.kept_copy_intact = message.complete && message.intact;
}

void NabtsRecordCatalogue::merge(const NabtsMessage& message,
                                 uint64_t frame_id) {
  const Key key{message.channel, message.address.value, message.version};
  ++touch_counter_;

  auto it = records_.find(key);
  if (it == records_.end()) {
    Entry entry;
    entry.record.channel = message.channel;
    entry.record.address = message.address.value;
    entry.record.address_text = message.address.text();
    entry.record.channel_text =
        fmt::format("{:03X}/{}", message.channel, message.address.text());
    entry.record.version = message.version;
    entry.record.reserved_purpose = message.reserved_purpose;
    entry.record.first_seen_frame = frame_id;
    take_copy(entry, message);
    it = records_.emplace(key, std::move(entry)).first;
  } else if (copy_is_better(it->second, message)) {
    take_copy(it->second, message);
  }

  Entry& entry = it->second;
  entry.record.last_seen_frame = frame_id;
  ++entry.record.times_seen;
  if (message.complete && message.intact) {
    ++entry.record.times_intact;
  }
  entry.last_touched = touch_counter_;

  enforce_bounds();
}

void NabtsRecordCatalogue::enforce_bounds() {
  while (records_.size() > max_records_) {
    auto oldest = records_.begin();
    for (auto candidate = records_.begin(); candidate != records_.end();
         ++candidate) {
      if (candidate->second.last_touched < oldest->second.last_touched) {
        oldest = candidate;
      }
    }
    records_.erase(oldest);
    truncated_ = true;
  }
}

std::vector<NabtsCataloguedRecord> NabtsRecordCatalogue::records() const {
  std::vector<NabtsCataloguedRecord> out;
  out.reserve(records_.size());
  // The map is keyed on {channel, address, version}, so iteration is already
  // the order a reader would list a service in.
  for (const auto& entry : records_) {
    out.push_back(entry.second.record);
  }
  return out;
}

}  // namespace orc
