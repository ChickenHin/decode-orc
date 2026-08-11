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
#include <array>
#include <set>
#include <utility>

#include "vbi-services/teletext_page_decoder.h"

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

/// §7.2.2's descriptors as the catalogue reports them.
std::vector<NabtsRecordFunction> render_functions(
    const std::vector<NabtsFunctionDescriptor>& functions) {
  std::vector<NabtsRecordFunction> out;
  out.reserve(functions.size());
  for (const NabtsFunctionDescriptor& function : functions) {
    NabtsRecordFunction rendered;
    rendered.code = function.code_text();
    rendered.control = function.is_control();
    rendered.arguments = arguments_text(function.arguments);
    out.push_back(std::move(rendered));
  }
  return out;
}

/// The length most copies agree on; the longest of them where none has more
/// support than another, since a copy is far likelier to have been cut short
/// than to have grown.
std::size_t voted_length(const std::vector<NabtsRecordCopy>& copies) {
  std::size_t best = 0;
  std::size_t best_support = 0;
  for (const auto& candidate : copies) {
    std::size_t support = 0;
    for (const auto& other : copies) {
      support += (other.data.size() == candidate.data.size()) ? 1 : 0;
    }
    if (support > best_support ||
        (support == best_support && candidate.data.size() > best)) {
      best = candidate.data.size();
      best_support = support;
    }
  }
  return best;
}

/**
 * @brief The same-channel More Record address a header extension names, if any
 *
 * §5.2.8.2 meaning 001 is "redefinition of More record Address", and §5.2.8.4
 * sizes the data by what it carries: 0 is record address zero, 3 a short
 * address, 9 a long one — all in the same data channel. The channel-switching
 * forms (6 and 12) name a record this catalogue keys under another channel;
 * they are rare and are left unresolved rather than half-followed. The macro
 * form (2) designates code, not a record.
 */
bool more_extension_address(const std::vector<NabtsHeaderExtension>& extensions,
                            uint64_t& address) {
  for (const NabtsHeaderExtension& extension : extensions) {
    if (extension.meaning != 1) {
      continue;
    }
    switch (extension.size) {
      case 0:
        address = 0;
        return true;
      case 3:
      case 9: {
        uint64_t value = 0;
        for (const uint8_t nibble : extension.data) {
          value = (value << 4) | (nibble & 0xF);
        }
        // A short address PQR is the long 0000PQR00 (§5.2.5).
        address = extension.size == 3 ? value << 8 : value;
        return true;
      }
      default:
        break;
    }
  }
  return false;
}

/**
 * @brief The algorithmic More address of §5.2.7.6, or false at the range's end
 *
 * "Adding 1 to the long version of the current Record Address", where §7.3.4
 * adds: "For the purposes of incrementing, the last two digits shall be
 * regarded as decimal numbers" — so 09 steps to 10, not to 0A.
 */
bool algorithmic_more_address(uint64_t address, uint64_t& successor) {
  const unsigned tens = (address >> 4) & 0xF;
  const unsigned units = address & 0xF;
  if (tens > 9 || units > 9) {
    return false;  // not a decimal pair; the increment is undefined on it
  }
  const unsigned value = tens * 10 + units + 1;
  if (value > 99) {
    return false;
  }
  successor = (address & ~uint64_t{0xFF}) | ((value / 10) << 4) | (value % 10);
  return true;
}

}  // namespace

std::vector<uint8_t> nabts_vote_record_data(
    const std::vector<NabtsRecordCopy>& copies) {
  if (copies.empty()) {
    return {};
  }
  if (copies.size() == 1) {
    return copies.front().data;  // A vote of one is the copy itself.
  }

  const std::size_t length = voted_length(copies);
  std::vector<uint8_t> out(length, 0);

  // One pass over the copies per position, tallying per byte value. The
  // accumulators are epoch-marked by position so none of them has to be cleared
  // between positions.
  std::array<uint64_t, 256> weight_of{};
  std::array<std::size_t, 256> newest_of{};
  std::array<std::size_t, 256> seen_at{};  // epoch: position + 1, 0 = never
  std::array<uint8_t, 256> distinct{};

  for (std::size_t position = 0; position < length; ++position) {
    const std::size_t epoch = position + 1;
    std::size_t distinct_count = 0;
    for (std::size_t copy = 0; copy < copies.size(); ++copy) {
      const NabtsRecordCopy& candidate = copies[copy];
      if (position >= candidate.data.size()) {
        continue;  // a copy cut short votes only over the bytes it has
      }
      if (position < candidate.present.size() &&
          candidate.present[position] == 0) {
        // A byte a lost packet carried. The hole is held open so the bytes
        // after it keep their offsets, but there is nothing here to vote with.
        continue;
      }
      const uint8_t value = candidate.data[position];
      if (seen_at[value] != epoch) {
        seen_at[value] = epoch;
        weight_of[value] = 0;
        newest_of[value] = 0;
        distinct[distinct_count++] = value;
      }
      // What the detector made of this byte, or full weight where nothing
      // measured it — see NabtsRecordCopy::confidence. One is the floor, so a
      // byte the detector could not decide at all still counts for more than a
      // copy that has no byte here at all.
      const uint64_t weight =
          position < candidate.confidence.size()
              ? std::max<uint64_t>(1, candidate.confidence[position])
              : 255;
      weight_of[value] += weight;
      newest_of[value] = copy;  // copies are held oldest first
    }

    bool best_clean = false;
    // NUL stands where no copy had this byte at all. X3.110 §6.1.4 makes it a
    // transparent control with no presentation effect, so a hole costs the
    // drawing nothing beyond the bytes that were actually lost — which is the
    // point of holding it open rather than letting the record close up over it.
    uint8_t best = 0;
    uint64_t best_weight = 0;
    std::size_t best_newest = 0;
    for (std::size_t k = 0; k < distinct_count; ++k) {
      const uint8_t value = distinct[k];
      const bool clean = teletext_odd_parity_valid(value);
      if (k > 0) {
        if (best_clean && !clean) {
          continue;  // a byte known corrupt never beats a parity-clean one
        }
        if (clean == best_clean && !(weight_of[value] > best_weight ||
                                     (weight_of[value] == best_weight &&
                                      newest_of[value] > best_newest))) {
          continue;
        }
      }
      best = value;
      best_clean = clean;
      best_weight = weight_of[value];
      best_newest = newest_of[value];
    }
    out[position] = best;
  }
  return out;
}

NabtsRecordCatalogue::NabtsRecordCatalogue(std::size_t max_records,
                                           std::size_t max_copies)
    : max_records_(std::max<std::size_t>(1, max_records)),
      max_copies_(std::max<std::size_t>(1, max_copies)) {}

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
  record.support_needed = message.classification.support_needed;
  record.index = message.classification.index;
  record.more = message.classification.more;
  record.records_in_message = message.records;
  record.complete = message.complete;
  record.data = message.data;

  record.functions = render_functions(message.functions);
  record.has_more_address =
      more_extension_address(message.extensions, record.more_address);
  entry.kept_copy_intact = message.complete && message.intact;
}

void NabtsRecordCatalogue::add_copy(Entry& entry,
                                    const NabtsMessage& message) const {
  if (message.complete && message.intact) {
    // The record has arrived whole, so there is nothing left for a vote to
    // improve on and the copies held for one can go.
    entry.copies.clear();
    entry.copies.shrink_to_fit();
    return;
  }
  if (entry.kept_copy_intact) {
    return;  // an undamaged copy arrived earlier and is what will be shown
  }
  if (!message.aligned) {
    // Bytes that have moved cannot be compared position for position with the
    // rest, and voting them in would corrupt every position after the hole.
    return;
  }
  if (entry.copies.size() >= max_copies_) {
    // Oldest out, so the copies held stay the most recent — which is what the
    // vote falls back to when a position has nothing to choose between.
    entry.copies.erase(entry.copies.begin());
  }
  entry.copies.push_back(
      NabtsRecordCopy{message.data, message.present, message.confidence});
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
  add_copy(entry, message);
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
    NabtsCataloguedRecord& record = out.back();

    // Copies are held only where no undamaged one ever arrived, so this is the
    // recording that never gave up a clean copy of the record: combine what did
    // arrive rather than show whichever copy happened to be longest.
    if (!entry.second.copies.empty()) {
      record.data = nabts_vote_record_data(entry.second.copies);
      record.copies_voted = static_cast<uint32_t>(entry.second.copies.size());
      if (record.record_type == kNabtsRecordTypeApplication) {
        // The descriptors were rendered from one copy; the vote may have
        // recovered bytes that copy had wrong (§7.2.2).
        record.functions =
            render_functions(nabts_decode_application_record(record.data));
      }
    }
  }

  return out;
}

void nabts_interpret_records(std::vector<NabtsCataloguedRecord>& records,
                             NaplpsRenderGrid grid) {
  NaplpsInterpreter interpreter(grid);

  // §8.7.1.4: one Support Record per Data Channel, at address FFF with the
  // Support Record Flag set — the record that "contain[s] one or more macro
  // definitions ... invoked directly by other Presentation Records in the same
  // Data Channel". Where versions differ the highest wins, which iteration
  // order delivers for free: same channel and address, ascending version.
  std::map<uint16_t, const NabtsCataloguedRecord*> support_by_channel;
  for (const NabtsCataloguedRecord& record : records) {
    if (record.support_record &&
        nabts_type_is_presentation(record.record_type)) {
      support_by_channel[record.channel] = &record;
    }
  }

  // The latest catalogued version at each {channel, address}, which is what a
  // More chain is followed through: §5.2.7.6 addresses the next record of a
  // chain, not a particular version of it. Ascending iteration leaves the
  // highest version in place.
  std::map<std::pair<uint16_t, uint64_t>, const NabtsCataloguedRecord*> latest;
  for (const NabtsCataloguedRecord& record : records) {
    if (nabts_type_is_presentation(record.record_type)) {
      latest[{record.channel, record.address}] = &record;
    }
  }

  // Who names each record as its More Record. §7.3.4's receiver algorithm
  // consults an explicit header extension address first (§5.2.8.4) and the
  // More Flag's algorithmic long-address-plus-one second (§5.2.7.6). The map
  // runs successor -> predecessor, which is the direction a chain is walked
  // to find what a record is presented over.
  std::map<std::pair<uint16_t, uint64_t>, const NabtsCataloguedRecord*>
      predecessor_of;
  for (const auto& entry : latest) {
    const NabtsCataloguedRecord* candidate = entry.second;
    uint64_t successor = 0;
    if (candidate->has_more_address) {
      successor = candidate->more_address;
    } else if (!candidate->more ||
               !algorithmic_more_address(candidate->address, successor)) {
      continue;
    }
    if (successor == candidate->address) {
      continue;  // a record naming itself is damage, not a chain
    }
    predecessor_of[{candidate->channel, successor}] = candidate;
  }

  // §6.1: a presentation record's data is NAPLPS, so this is where it becomes
  // something a viewer can draw — once per record, on whatever the vote
  // settled on. An application record's data is function descriptors (§7.2.2),
  // rendered above, and running it as presentation code would draw nonsense.
  //
  // Each record is presented the way a receiver would present the page: the
  // general reset of §8.5, then — for a record whose Support-Needed Flag is
  // set (§5.2.7.9) — the channel's Support Record, whose macros, DRCS, texture
  // masks and colour map persist into the page, then the caption preset of
  // §5.2.7.3 where the Caption Flag asks for it, and then the record itself.
  //
  // A More Record is presented last of all of its chain: §5.2.7.8 has it
  // "presented after the completion of the presentation of the current
  // Record" — over the standing display, not over a cleared one. So a
  // record's predecessors are walked back through the links above, executed
  // in order with the display carried between them, and the record itself
  // drawn on top of what they left.
  for (NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    if (record.data.empty()) {
      // Nothing to run, so nothing to draw: a record whose every copy was lost
      // has no presentation code, and running it would only replace the page it
      // already carries with the empty one it would have produced anyway.
      continue;
    }

    // The chain back to its base, oldest first, excluding |record| itself.
    // The visited set is what ends the walk around a closed loop of links.
    std::vector<const NabtsCataloguedRecord*> prefix;
    std::set<uint64_t> visited{record.address};
    uint64_t address = record.address;
    bool ring = false;
    while (true) {
      const auto predecessor = predecessor_of.find({record.channel, address});
      if (predecessor == predecessor_of.end()) {
        break;
      }
      if (!visited.insert(predecessor->second->address).second) {
        // After the de-duplication above every record has at most one
        // predecessor and one successor, so a revisit can only be the walk
        // arriving back where it started: the chain is a ring.
        ring = true;
        break;
      }
      prefix.insert(prefix.begin(), predecessor->second);
      address = predecessor->second->address;
    }

    if (ring) {
      // A ring — a rotating set the service cycles through (§7.3.4 note on
      // the Random More function) — has no transmitted base, and each member
      // walking to a different stop would scatter one set across as many
      // single-member "chains". The smallest address is taken as the base by
      // every member alike, and the prefix trimmed to the members between it
      // and this record. |prefix| holds the other members in link order
      // ending at this record's own predecessor, so the trim is a suffix.
      size_t base_index = 0;
      uint64_t base = record.address;
      for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i]->address < base) {
          base = prefix[i]->address;
          base_index = i;
        }
      }
      if (base == record.address) {
        prefix.clear();
      } else {
        prefix.erase(prefix.begin(),
                     prefix.begin() + static_cast<ptrdiff_t>(base_index));
      }
      address = base;
    }

    record.chain_base_address = address;
    record.chain_position = static_cast<uint32_t>(prefix.size());

    bool needs_support = record.support_needed;
    bool caption = record.caption;
    for (const NabtsCataloguedRecord* member : prefix) {
      needs_support = needs_support || member->support_needed;
      caption = caption || member->caption;
    }

    interpreter.reset_decoder();
    if (needs_support && !record.support_record) {
      const auto support = support_by_channel.find(record.channel);
      if (support != support_by_channel.end()) {
        // Run for its definitions; what the support record itself drew is not
        // part of this page.
        (void)interpreter.run(support->second->data);
      }
    }
    if (caption) {
      interpreter.apply_caption_state();
    }
    bool keep_display = false;
    for (const NabtsCataloguedRecord* member : prefix) {
      (void)interpreter.run(member->data, keep_display);
      keep_display = true;
    }
    record.page = interpreter.run(record.data, keep_display);
  }
}

}  // namespace orc
