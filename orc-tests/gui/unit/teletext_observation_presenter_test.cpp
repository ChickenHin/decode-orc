/*
 * File:        teletext_observation_presenter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for TeletextObservationPresenter extraction and
 *              page-view conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_observation_presenter.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/teletext_page_decoder.h>
#include <orc/support/teletext_slicer.h>

namespace gui_unit_test {

using orc::FieldID;
using orc::kTeletextPacketBytes;
using orc::ObservationContext;
using orc::presenters::TeletextObservationPresenter;

namespace {

std::array<uint8_t, kTeletextPacketBytes> patternPacket(uint8_t seed) {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  for (size_t i = 0; i < packet.size(); ++i) {
    packet[i] = static_cast<uint8_t>(seed ^ (i * 13));
  }
  return packet;
}

}  // namespace

TEST(TeletextObservationPresenterTest, AbsentNamespace_ReportsUnobserved) {
  ObservationContext context;

  const auto view = TeletextObservationPresenter::extractFieldObservations(
      FieldID(4), &context);

  EXPECT_FALSE(view.observed);
  EXPECT_FALSE(view.present);
  EXPECT_EQ(view.line_count, 0);
  EXPECT_TRUE(view.packets.empty());
}

TEST(TeletextObservationPresenterTest, EmptyField_ObservedButNoPackets) {
  ObservationContext context;
  const FieldID field(6);
  context.set(field, "teletext", "present", false);
  context.set(field, "teletext", "line_count", int32_t{0});

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  EXPECT_TRUE(view.observed);
  EXPECT_FALSE(view.present);
  EXPECT_EQ(view.line_count, 0);
  EXPECT_TRUE(view.packets.empty());
}

TEST(TeletextObservationPresenterTest, DecodesPacketsInAscendingLineOrder) {
  ObservationContext context;
  const FieldID field(10);
  const auto line16 = patternPacket(0xA5);
  const auto line7 = patternPacket(0x3C);
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "line_count", int32_t{2});
  context.set(field, "teletext", "t42_16", orc::teletext_packet_to_hex(line16));
  context.set(field, "teletext", "t42_7", orc::teletext_packet_to_hex(line7));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  EXPECT_TRUE(view.observed);
  EXPECT_TRUE(view.present);
  EXPECT_EQ(view.line_count, 2);
  ASSERT_EQ(view.packets.size(), 2u);
  EXPECT_EQ(view.packets[0].field_line, 7);
  EXPECT_EQ(view.packets[0].bytes, line7);
  EXPECT_EQ(view.packets[1].field_line, 16);
  EXPECT_EQ(view.packets[1].bytes, line16);
}

TEST(TeletextObservationPresenterTest, DecodesPerByteConfidenceWhenPresent) {
  ObservationContext context;
  const FieldID field(11);
  const auto packet = patternPacket(0x5A);
  orc::TeletextPacketConfidence confidence{};
  for (size_t i = 0; i < confidence.size(); ++i) {
    confidence[i] = i % 2 == 0 ? 1.0F : 0.2F;
  }
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "line_count", int32_t{1});
  context.set(field, "teletext", "t42_7",
              orc::teletext_packet_to_hex(packet, confidence));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  ASSERT_EQ(view.packets.size(), 1u);
  EXPECT_EQ(view.packets[0].bytes, packet);
  ASSERT_TRUE(view.packets[0].has_confidence);
  for (size_t i = 0; i < confidence.size(); ++i) {
    EXPECT_NEAR(view.packets[0].confidence[i], confidence[i], 0.05F)
        << "byte " << i;
  }
}

TEST(TeletextObservationPresenterTest, LegacyPacketReportsFullConfidence) {
  // An observation stored before confidences existed must not be discounted
  // against one that measured itself.
  ObservationContext context;
  const FieldID field(12);
  const auto packet = patternPacket(0x11);
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "line_count", int32_t{1});
  context.set(field, "teletext", "t42_7", orc::teletext_packet_to_hex(packet));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  ASSERT_EQ(view.packets.size(), 1u);
  EXPECT_FALSE(view.packets[0].has_confidence);
  for (const float value : view.packets[0].confidence) {
    EXPECT_EQ(value, 1.0F);
  }
}

TEST(TeletextObservationPresenterTest, MalformedHexPacket_IsSkipped) {
  ObservationContext context;
  const FieldID field(2);
  const auto good = patternPacket(0x11);
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "line_count", int32_t{2});
  context.set(field, "teletext", "t42_5", std::string("not-hex"));
  context.set(field, "teletext", "t42_9", orc::teletext_packet_to_hex(good));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  ASSERT_EQ(view.packets.size(), 1u);
  EXPECT_EQ(view.packets[0].field_line, 9);
  EXPECT_EQ(view.packets[0].bytes, good);
}

TEST(TeletextObservationPresenterTest, PageView_MapsIdentityAndAsciiCells) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.magazine = 1;
  snapshot.page_number = 0x00;
  snapshot.subcode = 0x3F7F & 0x1FFF;
  snapshot.subtitle = true;
  snapshot.header_field_index = 84;
  snapshot.last_field_index = 90;
  snapshot.cells[1][0].character = 'H';
  snapshot.cells[1][1].character = 'i';
  snapshot.cells[1][1].foreground = orc::TeletextColour::Yellow;
  snapshot.cells[1][1].background = orc::TeletextColour::Blue;
  snapshot.cells[1][2].character = 0x23;  // English (the default subset): £
  snapshot.cells[2][0].character = 'X';
  snapshot.cells[2][0].parity_error = true;

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.magazine, 1);
  EXPECT_EQ(view.page_number, 0x00);
  EXPECT_TRUE(view.subtitle);
  EXPECT_EQ(view.header_field_index, 84);
  EXPECT_EQ(view.last_field_index, 90);
  EXPECT_EQ(view.cells[1][0].character, U'H');
  EXPECT_FALSE(view.cells[1][0].mosaic);
  EXPECT_EQ(view.cells[1][1].character, U'i');
  EXPECT_EQ(view.cells[1][1].foreground, 3);  // yellow
  EXPECT_EQ(view.cells[1][1].background, 4);  // blue
  EXPECT_EQ(view.cells[1][2].character, U'£');
  EXPECT_TRUE(view.cells[2][0].parity_error);
}

TEST(TeletextObservationPresenterTest, PageView_AppliesThePagesNationalOption) {
  // The same code renders differently depending on the sub-set the page
  // header selected (EN 300 706 §15.2, §15.6.2 Table 36) — 2/3 is "£" on an
  // English service and "#" on a German one, whose 7/E is "ß".
  orc::TeletextPageSnapshot snapshot;
  snapshot.national_option_subset =
      static_cast<int>(orc::TeletextNationalOption::German);
  snapshot.cells[1][0].character = 0x23;
  snapshot.cells[1][1].character = 0x7E;

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.cells[1][0].character, U'#');
  EXPECT_EQ(view.cells[1][1].character, U'ß');
}

TEST(TeletextObservationPresenterTest, PageView_MapsMosaicSixels) {
  orc::TeletextPageSnapshot snapshot;
  auto& all_set = snapshot.cells[3][0];
  all_set.character = 0x7F;  // all six sixels set
  all_set.mosaic = true;
  auto& mixed = snapshot.cells[3][1];
  // EN 300 706 §15.7.1 Table 47: bits 1-5 then bit 7 (0x40) select sixels.
  mixed.character = 0x20 | 0x01 | 0x40;  // top-left + bottom-right
  mixed.mosaic = true;
  mixed.separated_mosaic = true;
  auto& blast_through = snapshot.cells[3][2];
  blast_through.character = 'A';  // codes 4/0-5/F stay alphanumeric
  blast_through.mosaic = true;
  auto& held = snapshot.cells[3][3];
  held.character = 0x30;
  held.mosaic = false;
  held.held_mosaic = true;

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_TRUE(view.cells[3][0].mosaic);
  EXPECT_EQ(view.cells[3][0].mosaic_pattern, 0x3F);
  EXPECT_TRUE(view.cells[3][1].mosaic);
  EXPECT_EQ(view.cells[3][1].mosaic_pattern, 0x01 | 0x20);
  EXPECT_TRUE(view.cells[3][1].mosaic_separated);
  EXPECT_FALSE(view.cells[3][2].mosaic);
  EXPECT_EQ(view.cells[3][2].character, U'A');
  EXPECT_TRUE(view.cells[3][3].mosaic);
  EXPECT_EQ(view.cells[3][3].mosaic_pattern, 0x10);
}

TEST(TeletextObservationPresenterTest,
     PageView_DrawsBlastThroughCodesAsMosaics) {
  // A page whose service has no blast-through region reads every code from 2/0
  // up as a mosaic, so the codes that would otherwise put capitals through a
  // drawing become the block patterns the drawing is made of.
  orc::TeletextPageSnapshot snapshot;
  snapshot.mosaic_blast_through = false;
  auto& solid = snapshot.cells[3][0];
  solid.character = 0x5F;  // 'link' in the 625-line reading; solid block here
  solid.mosaic = true;
  auto& partial = snapshot.cells[3][1];
  partial.character = 0x57;  // 'W' in the 625-line reading
  partial.mosaic = true;
  auto& alpha = snapshot.cells[3][2];
  alpha.character = 'A';
  alpha.mosaic = false;  // not in mosaic mode: still a letter

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_TRUE(view.cells[3][0].mosaic);
  EXPECT_EQ(view.cells[3][0].mosaic_pattern, 0x3F);
  EXPECT_TRUE(view.cells[3][1].mosaic);
  EXPECT_EQ(view.cells[3][1].mosaic_pattern, 0x37);
  EXPECT_FALSE(view.cells[3][2].mosaic);
  EXPECT_EQ(view.cells[3][2].character, U'A');
}

TEST(TeletextObservationPresenterTest, PageView_SummarisesRecovery) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.row_received[0] = true;  // header row: not a display row
  for (int row = 1; row <= 20; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
  }
  // Rows 21-24 never arrived.
  snapshot.cells[5][0].parity_error = true;
  snapshot.cells[5][1].parity_error = true;
  snapshot.cells[6][0].parity_error = true;

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.rows_expected, 24);
  EXPECT_EQ(view.recovery.rows_received, 20);
  EXPECT_EQ(view.recovery.damaged_bytes, 3);
  EXPECT_FALSE(view.recovery.complete());
  EXPECT_TRUE(view.row_received[20]);
  EXPECT_FALSE(view.row_received[21]);
}

// A row that arrived once is only worth flagging where the page has rows the
// carousel has corrected against a repeat: that is what makes the odd one out
// odd. It is where a row carried onto the wrong address by a burst survives.
TEST(TeletextObservationPresenterTest, PageView_FlagsRowsSeenOnlyOnce) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row <= 5; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
    snapshot.row_copies[static_cast<size_t>(row)] = 3;
  }
  snapshot.row_copies[3] = 1;  // never confirmed by a repeat

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.unconfirmed_rows, 1);
  EXPECT_TRUE(view.row_unconfirmed[3]);
  EXPECT_FALSE(view.row_unconfirmed[2]);
  EXPECT_FALSE(view.row_unconfirmed[6]) << "a row never received is not this";
}

// On a page seen once nothing has been confirmed, so saying so of every row
// says nothing at all.
TEST(TeletextObservationPresenterTest, PageView_FirstSightingFlagsNothing) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row <= 5; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
    snapshot.row_copies[static_cast<size_t>(row)] = 1;
  }

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.unconfirmed_rows, 0);
}

// A row consumed by a double-height character above it carries no data by
// definition (EN 300 706 §12.2 code 0/D), so its absence is not a gap.
TEST(TeletextObservationPresenterTest,
     PageView_ExcludesDoubleHeightLowerRowsFromRecovery) {
  orc::TeletextPageSnapshot snapshot;
  for (int row = 1; row < orc::TeletextPageSnapshot::kRows; ++row) {
    snapshot.row_received[static_cast<size_t>(row)] = true;
  }
  snapshot.row_received[2] = false;  // lower row was never transmitted
  for (auto& cell : snapshot.cells[1]) {
    cell.double_height = true;
  }
  for (auto& cell : snapshot.cells[2]) {
    cell.double_height_lower = true;
  }

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.recovery.rows_expected, 23);
  EXPECT_EQ(view.recovery.rows_received, 23);
  EXPECT_TRUE(view.recovery.complete());
}

// ---------------------------------------------------------------------------
// 525-line WST (ITU-R BT.653 Table 1b): a 34-byte packet, its length carried by
// the observation string's own
// ---------------------------------------------------------------------------

TEST(TeletextObservationPresenterTest, Decodes525LinePacketsWithTheirLength) {
  ObservationContext context;
  const FieldID field(12);
  const auto packet = patternPacket(0x5A);
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "line_count", int32_t{1});
  context.set(
      field, "teletext", "t42_11",
      orc::teletext_packet_to_hex(packet, orc::kTeletext525PacketBytes));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  ASSERT_EQ(view.packets.size(), 1u);
  EXPECT_EQ(view.packets[0].field_line, 11);
  EXPECT_EQ(view.packets[0].byte_count,
            static_cast<int>(orc::kTeletext525PacketBytes));
  // The bytes the service sent survive; the rest are zero because they were
  // never transmitted.
  for (size_t i = 0; i < orc::kTeletext525PacketBytes; ++i) {
    EXPECT_EQ(view.packets[0].bytes[i], packet[i]) << "byte " << i;
  }
  for (size_t i = orc::kTeletext525PacketBytes; i < kTeletextPacketBytes; ++i) {
    EXPECT_EQ(view.packets[0].bytes[i], 0) << "byte " << i;
  }
}

TEST(TeletextObservationPresenterTest, A625LinePacketStillReportsFortyTwo) {
  ObservationContext context;
  const FieldID field(14);
  context.set(field, "teletext", "present", true);
  context.set(field, "teletext", "t42_7",
              orc::teletext_packet_to_hex(patternPacket(0x11)));

  const auto view =
      TeletextObservationPresenter::extractFieldObservations(field, &context);

  ASSERT_EQ(view.packets.size(), 1u);
  EXPECT_EQ(view.packets[0].byte_count, static_cast<int>(kTeletextPacketBytes));
}

TEST(TeletextObservationPresenterTest, PageViewCarriesTheServiceWidth) {
  orc::TeletextPageSnapshot snapshot;
  snapshot.columns = 32;
  snapshot.cells[1][31].character = 'A';
  // Beyond the service width the snapshot holds nothing; the view must not
  // walk there either.
  snapshot.cells[1][39].parity_error = true;

  const auto view = TeletextObservationPresenter::makePageView(snapshot);

  EXPECT_EQ(view.columns, 32);
  EXPECT_EQ(view.cells[1][31].character, U'A');
  EXPECT_EQ(view.recovery.damaged_bytes, 0);
}

}  // namespace gui_unit_test
