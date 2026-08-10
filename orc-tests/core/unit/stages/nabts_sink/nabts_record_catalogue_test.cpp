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
                          bool complete = true, bool intact = true,
                          bool aligned = true) {
  orc::NabtsMessage out;
  // The assembler never emits an incomplete message as aligned: a missing link
  // takes bytes out of the middle of the concatenation, so everything after it
  // moves earlier (see NabtsMessage::aligned).
  out.aligned = aligned && complete;
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
// Combining damaged copies
////////////////////////////////////////////////////////////////////////////////////////////

/// |value| in the low seven bits with b8 set to give the odd parity CEA-516
/// §3.3 requires of every data byte of a type-zero group.
constexpr uint8_t odd(uint8_t value) {
  uint8_t bits = 0;
  for (int bit = 0; bit < 7; ++bit) {
    bits = static_cast<uint8_t>(bits + ((value >> bit) & 1));
  }
  return static_cast<uint8_t>((bits % 2 == 0) ? (value | 0x80) : value);
}

/// A damaged copy: complete and aligned, so it votes, but not undamaged.
orc::NabtsMessage damaged(std::vector<uint8_t> data) {
  return message(0x000, 0x001, 0, std::move(data), /*complete=*/true,
                 /*intact=*/false);
}

/// A damaged copy with holes: |present| marks, per byte, whether it arrived.
orc::NabtsMessage holed(std::vector<uint8_t> data,
                        std::vector<uint8_t> present) {
  auto out = damaged(std::move(data));
  out.present = std::move(present);
  return out;
}

/// A damaged copy the detector measured: |confidence| is 0-255 per byte.
orc::NabtsMessage measured(std::vector<uint8_t> data,
                           std::vector<uint8_t> confidence) {
  auto out = damaged(std::move(data));
  out.confidence = std::move(confidence);
  return out;
}

// The point of the exercise: a byte no single copy has right is recovered from
// the copies that do, position by position.
TEST(NabtsRecordCatalogue, VotesDamagedCopiesIntoOneBestEstimate) {
  orc::NabtsRecordCatalogue catalogue;
  // Each copy is wrong in a different place; between them every position has a
  // majority.
  catalogue.merge(damaged({odd('N'), odd('B'), odd('!')}), 0);
  catalogue.merge(damaged({odd('N'), odd('?'), odd('C')}), 1);
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 3u);
  EXPECT_EQ(records[0].times_intact, 0u);
}

// §3.3's odd parity detects every single-bit error, so a byte that fails it is
// known to be corrupt and loses to a parity-clean candidate however often it
// arrived.
TEST(NabtsRecordCatalogue, AParityCleanByteBeatsAMoreNumerousCorruptOne) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('A') ^ 0x01);
  catalogue.merge(damaged({corrupt}), 0);
  catalogue.merge(damaged({corrupt}), 1);
  catalogue.merge(damaged({odd('A')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// Where every copy of a position is damaged there is no clean candidate to
// prefer, and the most numerous reading is still the best available one.
TEST(NabtsRecordCatalogue, FallsBackToTheCommonestByteWhereEveryCopyIsDamaged) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t a = static_cast<uint8_t>(odd('A') ^ 0x01);
  const uint8_t b = static_cast<uint8_t>(odd('A') ^ 0x02);
  catalogue.merge(damaged({a}), 0);
  catalogue.merge(damaged({b}), 1);
  catalogue.merge(damaged({a}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{a}));
}

// A copy whose packets were lost has bytes missing from the middle of it
// (§3.2.4), so it no longer lines up with the others and must not vote — one
// admitted would corrupt every position after its hole.
TEST(NabtsRecordCatalogue, AMisalignedCopyDoesNotVote) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 0);
  // Two misaligned copies agreeing on a different byte would carry the vote if
  // they were let into it.
  for (uint64_t frame = 1; frame <= 2; ++frame) {
    catalogue.merge(message(0x000, 0x001, 0, {odd('X'), odd('Y'), odd('Z')},
                            /*complete=*/true,
                            /*intact=*/false, /*aligned=*/false),
                    frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
}

// The record is as long as the length its copies agree on; one cut short still
// votes over the bytes it does have.
TEST(NabtsRecordCatalogue, TakesTheLengthTheCopiesAgreeOnAndLetsAShortOneVote) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('B') ^ 0x01);
  catalogue.merge(damaged({odd('N'), corrupt, odd('C')}), 0);
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 1);
  catalogue.merge(damaged({odd('N'), odd('B')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
}

// An undamaged copy is the record, so it is shown as it arrived rather than
// voted against the damaged copies that came before it.
TEST(NabtsRecordCatalogue, AnUndamagedCopyEndsTheVote) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 0);
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 1);
  catalogue.merge(message(0x000, 0x001, 0, {odd('N'), odd('B'), odd('C')}), 2);
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 0u);
  EXPECT_EQ(records[0].times_intact, 1u);
}

// A carousel runs for the length of the recording, so the copies retained are
// bounded — and it is the most recent that are kept.
TEST(NabtsRecordCatalogue, RetainsOnlyTheMostRecentCopiesForTheVote) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/8, /*max_copies=*/2);
  catalogue.merge(damaged({odd('A')}), 0);
  catalogue.merge(damaged({odd('B')}), 1);
  catalogue.merge(damaged({odd('C')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].copies_voted, 2u);
  // 'A' has been evicted; 'B' and 'C' each have one vote, and the tie goes to
  // the more recent.
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('C')}));
}

// §7.2.2's descriptors are read out of the record data, so a vote that recovers
// a byte has to be reflected in them too.
TEST(NabtsRecordCatalogue, RereadsApplicationDescriptorsFromTheVotedData) {
  orc::NabtsRecordCatalogue catalogue;
  const auto application = [](std::vector<uint8_t> data) {
    auto out = damaged(std::move(data));
    out.type = orc::kNabtsRecordTypeApplication;
    return out;
  };
  // 2/0 with arguments "AB", the second copy damaged in the 'B'.
  catalogue.merge(application({odd(0x20), odd('A'), odd('B')}), 0);
  catalogue.merge(
      application({odd(0x20), odd('A'), static_cast<uint8_t>(odd('B') ^ 0x01)}),
      1);
  catalogue.merge(application({odd(0x20), odd('A'), odd('B')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 1u);
  EXPECT_EQ(records[0].functions[0].code, "2/0");
  EXPECT_EQ(records[0].functions[0].arguments, "AB");
}

// A lost packet's bytes are held open as a hole rather than closing up, so a
// copy abstains where it received nothing and still votes either side of it.
// This is what lets a recording that never received any one copy whole have
// every position decided by whichever copies did receive it.
TEST(NabtsRecordCatalogue, ACopyAbstainsWhereItsPacketsWereLost) {
  orc::NabtsRecordCatalogue catalogue;
  // Two copies, each holding a hole where the other has the byte.
  catalogue.merge(holed({odd('N'), 0x00, odd('C')}, {1, 0, 1}), 0);
  catalogue.merge(holed({odd('N'), odd('B'), 0x00}, {1, 1, 0}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
}

// A hole never outvotes a byte, however many copies share it: a copy that
// received nothing at a position has nothing to say about it.
TEST(NabtsRecordCatalogue, HolesDoNotOutvoteTheOneCopyThatArrived) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({0x00}, {0}), 0);
  catalogue.merge(holed({0x00}, {0}), 1);
  catalogue.merge(holed({odd('A')}, {1}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// Where no copy received a position at all it stays NUL, which X3.110 §6.1.4
// makes a transparent control — so the hole costs the drawing nothing beyond
// the bytes that were actually lost.
TEST(NabtsRecordCatalogue, APositionNoCopyReceivedIsLeftAsNul) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({odd('N'), 0x00}, {1, 0}), 0);
  catalogue.merge(holed({odd('N'), 0x00}, {1, 0}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('N'), 0x00}));
}

// An absent mask is the ordinary case of a copy that lost nothing, and must not
// read as a copy that received nothing.
TEST(NabtsRecordCatalogue, AnAbsentMaskMeansEveryByteArrived) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({0x00, 0x00}, {0, 0}), 0);
  catalogue.merge(damaged({odd('N'), odd('B')}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('N'), odd('B')}));
}

// A copy contributes how sure the detector was of its byte rather than one vote
// apiece, so a value read cleanly outweighs more copies of one the detector
// nearly decided the other way.
TEST(NabtsRecordCatalogue, AConfidentByteOutweighsMoreNumerousDoubtfulOnes) {
  orc::NabtsRecordCatalogue catalogue;
  // Two barely-decided copies of 'X' against one clear reading of 'A'. Both
  // values carry correct parity, so only the weighting separates them.
  catalogue.merge(measured({odd('X')}, {10}), 0);
  catalogue.merge(measured({odd('X')}, {10}), 1);
  catalogue.merge(measured({odd('A')}, {255}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// And the weighting never overrides the parity gate: a byte known to be corrupt
// loses to a parity-clean one however sure the detector was of it.
TEST(NabtsRecordCatalogue, ConfidenceDoesNotOverrideTheParityGate) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('A') ^ 0x01);
  catalogue.merge(measured({corrupt}, {255}), 0);
  catalogue.merge(measured({corrupt}, {255}), 1);
  catalogue.merge(measured({odd('A')}, {1}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// The threshold detector has no path metric and so cannot express doubt. Its
// copies must weigh as much as a confident measurement rather than as nothing,
// or a run without MLSE would have every copy counting for zero.
TEST(NabtsRecordCatalogue, AnUnmeasuredCopyVotesAtFullWeight) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(measured({odd('X')}, {200}), 0);
  catalogue.merge(damaged({odd('A')}), 1);  // no confidence measured at all

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  // 255 (unmeasured) beats the 200 the other copy was measured at.
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// A byte the detector could not decide at all still says more than a copy that
// has no byte at this position.
TEST(NabtsRecordCatalogue, AZeroConfidenceByteStillBeatsAHole) {
  orc::NabtsRecordCatalogue catalogue;
  auto doubtful = measured({odd('A')}, {0});
  catalogue.merge(doubtful, 0);
  catalogue.merge(holed({0x00}, {0}), 1);
  catalogue.merge(holed({0x00}, {0}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
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
