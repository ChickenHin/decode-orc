/*
 * File:        nabts_record_catalogue_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the bounded NABTS record catalogue
 *
 * Covers: the {channel, address, version} identity CEA-516 §5.2.1 gives a
 * record, which copy of a repeated record is kept, the appearance counts a
 * reader compares to judge a recording, application function rendering, and the
 * cap that bounds a run by the size of the service rather than the length of
 * the recording.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record_catalogue.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

/// The long form of short address |short_address| (§5.2.5: 0000PQR00).
constexpr uint64_t long_of(uint16_t short_address) {
  return static_cast<uint64_t>(short_address) << 8;
}

/**
 * @brief A message as the record assembler would deliver it
 *
 * Built directly rather than decoded from bytes: these tests are about what the
 * catalogue does with a message, so stating the message is clearer than
 * encoding one and hoping it came back as intended. nabts_record_test.cpp is
 * where the decoding is pinned down.
 */
orc::NabtsMessage message(uint16_t channel, uint16_t short_address,
                          uint8_t version, std::vector<uint8_t> data,
                          bool complete = true, bool intact = true) {
  orc::NabtsMessage out;
  out.channel = channel;
  out.address.value = long_of(short_address);
  out.address.long_form = false;
  out.type = orc::kNabtsRecordTypeCyclicPresentation;
  out.version = version;
  out.classification.present = true;
  out.classification.version_present = true;
  out.classification.version = version;
  out.data = std::move(data);
  out.records = 1;
  out.links_expected = 1;
  out.complete = complete;
  out.intact = intact;
  return out;
}

/// |count| distinguishable bytes starting at |first|.
std::vector<uint8_t> bytes(uint8_t first, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(static_cast<uint8_t>(first + i));
  }
  return out;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Identity
////////////////////////////////////////////////////////////////////////////////////////////

// §7.1.2: a cyclic service brings the same record round throughout a recording,
// so a hundred copies are one catalogue entry seen a hundred times.
TEST(NabtsRecordCatalogue, CataloguesRepeatedCopiesAsOneRecord) {
  orc::NabtsRecordCatalogue catalogue;
  for (uint64_t frame = 0; frame < 5; ++frame) {
    catalogue.merge(message(0x000, 0x1A4, 2, bytes(0x40, 30)), frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 5u);
  EXPECT_EQ(records[0].times_intact, 5u);
  EXPECT_EQ(records[0].first_seen_frame, 0u);
  EXPECT_EQ(records[0].last_seen_frame, 4u);
  EXPECT_EQ(records[0].address_text, "1A4");
  EXPECT_EQ(records[0].channel_text, "000/1A4");
}

// §5.2.1 makes the version part of a record's identity: a service that revises
// a page increments Y16, and the old and new are different records sharing an
// address.
TEST(NabtsRecordCatalogue, KeepsVersionsOfOneAddressApart) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x1A4, 1, bytes(0x40, 10)), 0);
  catalogue.merge(message(0x000, 0x1A4, 2, bytes(0x50, 20)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].version, 1u);
  EXPECT_EQ(records[0].data.size(), 10u);
  EXPECT_EQ(records[1].version, 2u);
  EXPECT_EQ(records[1].data.size(), 20u);
}

// §3.2.3 makes the packet address the data channel, and the same record address
// on two channels is two records.
TEST(NabtsRecordCatalogue, KeepsChannelsApart) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 10)), 0);
  catalogue.merge(message(0x200, 0x001, 0, bytes(0x50, 10)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].channel, 0x000u);
  EXPECT_EQ(records[1].channel, 0x200u);
}

// Ordered by {channel, address, version}, which is how a reader would list a
// service.
TEST(NabtsRecordCatalogue, ListsRecordsInChannelThenAddressThenVersionOrder) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x200, 0x001, 0, bytes(0x40, 4)), 0);
  catalogue.merge(message(0x000, 0x1A4, 3, bytes(0x40, 4)), 1);
  catalogue.merge(message(0x000, 0x1A4, 1, bytes(0x40, 4)), 2);
  catalogue.merge(message(0x000, 0x002, 0, bytes(0x40, 4)), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 4u);
  EXPECT_EQ(records[0].channel_text, "000/002");
  EXPECT_EQ(records[1].channel_text, "000/1A4");
  EXPECT_EQ(records[1].version, 1u);
  EXPECT_EQ(records[2].channel_text, "000/1A4");
  EXPECT_EQ(records[2].version, 3u);
  EXPECT_EQ(records[3].channel_text, "200/001");
}

// §5.2.5's equivalence is the address value's business, so a record transmitted
// once with a short address and once with its long equivalent is one record.
TEST(NabtsRecordCatalogue, TreatsAShortAndItsLongAddressAsOneRecord) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x1A4, 0, bytes(0x40, 10)), 0);

  auto long_form = message(0x000, 0x1A4, 0, bytes(0x40, 10));
  long_form.address.long_form = true;
  catalogue.merge(long_form, 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Which copy is kept
////////////////////////////////////////////////////////////////////////////////////////////

// A clean copy is never replaced by a damaged one, so a service recorded off
// air keeps the copy that arrived cleanly rather than the last one before the
// tape ran out.
TEST(NabtsRecordCatalogue, ACleanCopyIsNotReplacedByADamagedOne) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 0);
  // Later, longer, but damaged.
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/true,
                          /*intact=*/false),
                  1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].times_intact, 1u);
}

// And a damaged copy is replaced the moment a clean one arrives, however short.
TEST(NabtsRecordCatalogue, ADamagedCopyIsReplacedByACleanOne) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/true,
                          /*intact=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_TRUE(records[0].complete);
}

// An incomplete copy is damaged for this purpose: §5.2.6 makes a message the
// whole linked series, and a series missing a link is missing record data.
TEST(NabtsRecordCatalogue, AnIncompleteCopyLosesToACompleteOne) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
}

// Among copies of equal quality the longer one carries more of the record.
TEST(NabtsRecordCatalogue, AmongEqualQualityCopiesTheLongerWins) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 20), /*complete=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 40), /*complete=*/false),
                  1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x70, 40));
}

// Replacing like with like would only churn, so an identical later copy is
// counted and otherwise ignored.
TEST(NabtsRecordCatalogue, AnEquallyGoodCopyDoesNotDisplaceTheFirst) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].times_intact, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// What the entry carries
////////////////////////////////////////////////////////////////////////////////////////////

TEST(NabtsRecordCatalogue, CarriesTheClassificationFlagsWorthListing) {
  orc::NabtsRecordCatalogue catalogue;
  auto flagged = message(0x000, 0x001, 0, bytes(0x40, 10));
  flagged.type = orc::kNabtsRecordTypePriorityPresentation;
  flagged.classification.caption = true;
  flagged.classification.cyclic_marker = true;
  flagged.classification.priority = true;
  flagged.classification.alarm = true;
  flagged.classification.update = true;
  flagged.classification.support_record = true;
  flagged.classification.index = true;
  flagged.classification.more = true;
  catalogue.merge(flagged, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].record_type, orc::kNabtsRecordTypePriorityPresentation);
  EXPECT_TRUE(records[0].caption);
  EXPECT_TRUE(records[0].cyclic_marker);
  EXPECT_TRUE(records[0].priority);
  EXPECT_TRUE(records[0].alarm);
  EXPECT_TRUE(records[0].update);
  EXPECT_TRUE(records[0].support_record);
  EXPECT_TRUE(records[0].index);
  EXPECT_TRUE(records[0].more);
}

TEST(NabtsRecordCatalogue, CarriesTheReservedPurposeOfAnAddress) {
  orc::NabtsRecordCatalogue catalogue;
  auto reserved = message(0xA00, 0x000, 0, bytes(0x40, 10));
  reserved.reserved_purpose = "Start of captioning";
  catalogue.merge(reserved, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].reserved_purpose, "Start of captioning");
}

// §7.2.2's function descriptors are rendered for a reader: the code in the
// standard's own column/row notation, and the arguments as text.
TEST(NabtsRecordCatalogue, RendersApplicationFunctionDescriptorsAsText) {
  orc::NabtsRecordCatalogue catalogue;
  auto application = message(0x000, 0xFFE, 0, {});
  application.type = orc::kNabtsRecordTypeApplication;
  application.functions.push_back(
      orc::NabtsFunctionDescriptor{0x20, {0x41, 0x42, 0x43}});
  application.functions.push_back(orc::NabtsFunctionDescriptor{0x31, {}});
  catalogue.merge(application, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 2u);
  EXPECT_EQ(records[0].functions[0].code, "2/0");
  EXPECT_TRUE(records[0].functions[0].control);
  EXPECT_EQ(records[0].functions[0].arguments, "ABC");
  EXPECT_EQ(records[0].functions[1].code, "3/1");
  EXPECT_FALSE(records[0].functions[1].control);
  EXPECT_TRUE(records[0].functions[1].arguments.empty());
}

// §7.2.2 confines arguments to the printable range 2/0 to 7/15, so a byte
// outside it is damage and is shown as its value rather than as a control
// character in the middle of a listing.
TEST(NabtsRecordCatalogue, ShowsAnUnprintableArgumentAsItsHexadecimalValue) {
  orc::NabtsRecordCatalogue catalogue;
  auto application = message(0x000, 0xFFE, 0, {});
  application.type = orc::kNabtsRecordTypeApplication;
  application.functions.push_back(
      orc::NabtsFunctionDescriptor{0x20, {0x41, 0x07, 0x42, 0x7F}});
  catalogue.merge(application, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 1u);
  EXPECT_EQ(records[0].functions[0].arguments, "A<07>B<7F>");
}

////////////////////////////////////////////////////////////////////////////////////////////
// The cap
////////////////////////////////////////////////////////////////////////////////////////////

// The cap bounds a run by the size of the service rather than the length of the
// recording. Reaching it drops the least recently seen record and says so,
// since a catalogue silently missing records would read as a service that never
// sent them.
TEST(NabtsRecordCatalogue, DropsTheLeastRecentlySeenRecordAtTheCap) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/3);

  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 0);
  catalogue.merge(message(0x000, 0x002, 0, bytes(0x40, 4)), 1);
  catalogue.merge(message(0x000, 0x003, 0, bytes(0x40, 4)), 2);
  EXPECT_EQ(catalogue.size(), 3u);
  EXPECT_FALSE(catalogue.truncated());

  // Touch 001 so 002 becomes the oldest, then overflow.
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 3);
  catalogue.merge(message(0x000, 0x004, 0, bytes(0x40, 4)), 4);

  EXPECT_EQ(catalogue.size(), 3u);
  EXPECT_TRUE(catalogue.truncated());
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].address_text, "001");
  EXPECT_EQ(records[1].address_text, "003");
  EXPECT_EQ(records[2].address_text, "004");
}

TEST(NabtsRecordCatalogue, IsEmptyBeforeAnythingIsMerged) {
  const orc::NabtsRecordCatalogue catalogue;
  EXPECT_EQ(catalogue.size(), 0u);
  EXPECT_FALSE(catalogue.truncated());
  EXPECT_TRUE(catalogue.records().empty());
}

// A cap of zero would be a catalogue that can hold nothing and so truncates for
// ever; one is the floor.
TEST(NabtsRecordCatalogue, HoldsAtLeastOneRecordWhateverTheCapAsksFor) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 0);
  EXPECT_EQ(catalogue.size(), 1u);
  EXPECT_FALSE(catalogue.truncated());
}

}  // namespace
}  // namespace orc_unit_test
