/*
 * File:        vbi_line_mapping_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for stored record to CVBS frame line placement
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_mapping.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

VBITeletextLineMap pal_wst_line_map() {
  VBITeletextLineMap line_map;
  std::string error;
  EXPECT_TRUE(make_vbi_teletext_line_map(
      VBITVSystem::kPAL, VBITeletextSystem::kWST, line_map, error))
      << error;
  return line_map;
}

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(expand_vbi_source_preset("bt8x8-pal", format, error)) << error;
  return format;
}

// WST occupies broadcast frame lines 7-22 and 320-335, which are stored frame
// lines 6-21 and 319-334.
TEST(VBILineMapping, PALWSTLineTableMatchesTheStandardForBothFields) {
  const VBITeletextLineMap line_map = pal_wst_line_map();

  ASSERT_EQ(line_map.field1.size(), 16u);
  ASSERT_EQ(line_map.field2.size(), 16u);

  EXPECT_EQ(line_map.field1,
            (std::vector<uint32_t>{6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                   18, 19, 20, 21}));
  EXPECT_EQ(line_map.field2,
            (std::vector<uint32_t>{319, 320, 321, 322, 323, 324, 325, 326, 327,
                                   328, 329, 330, 331, 332, 333, 334}));

  EXPECT_EQ(&line_map.for_tv_field(1), &line_map.field1);
  EXPECT_EQ(&line_map.for_tv_field(2), &line_map.field2);
  EXPECT_TRUE(line_map.for_tv_field(3).empty());
}

// The two fields' ranges are not related by a constant offset, which is why
// the mapping is a table rather than an offset and a stride.
TEST(VBILineMapping, FieldRangesAreNotSeparatedByAConstantOffset) {
  const VBITeletextLineMap line_map = pal_wst_line_map();

  EXPECT_EQ(line_map.field2.front() - line_map.field1.front(), 313u);
  EXPECT_NE(line_map.field2.front() - line_map.field1.front(),
            line_map.field1.size() * 2u);
}

TEST(VBILineMapping, EveryBt8x8RecordResolvesToItsStandardFrameLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  for (uint32_t record = 0; record < 16; ++record) {
    uint32_t frame_line = 0;
    std::string error;

    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 0, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 6u + record);

    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 1, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 319u + record);
  }
}

// Which television field a stored field carries is a driver convention, so it
// is configuration; getting it backwards swaps the line numbering between the
// two fields.
TEST(VBILineMapping, FirstFieldConfigurationSwapsTheTelevisionFieldOrder) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 0), 1u);
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 1), 2u);

  format.first_field = 2;
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 0), 2u);
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 1), 1u);

  uint32_t frame_line = 0;
  std::string error;
  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 0, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 319u);
}

// A source carrying fewer lines than the standard maps contiguously from the
// head of the field's line list.
TEST(VBILineMapping, ShorterSourcesMapContiguouslyFromTheFieldRangeStart) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{2, 9};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 2, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 6u);

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 9, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 13u);

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 1, 2, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 319u);
}

TEST(VBILineMapping, RecordsOutsideTheFieldRangeCarryNoDataService) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{1, 8};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 0, frame_line, error));
  EXPECT_NE(error.find("field_range"), std::string::npos);

  error.clear();
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 9, frame_line, error));
  EXPECT_NE(error.find("field_range"), std::string::npos);
}

// Excess records have nowhere to go; that is a configuration error and never
// a silent truncation.
TEST(VBILineMapping, RecordsBeyondTheStandardsLineListAreRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_lines = 18;
  format.field_range = VBIFieldRange{0, 17};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 15, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 21u);

  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 16, frame_line, error));
  EXPECT_NE(error.find("16"), std::string::npos);
  EXPECT_NE(error.find("WST"), std::string::npos);
}

TEST(VBILineMapping, OnlyTwoStoredFieldsExistPerFrame) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 2, 0, frame_line, error));
  EXPECT_FALSE(error.empty());
}

// Placement for systems the stage does not synthesise is withheld rather than
// half-supported.
TEST(VBILineMapping, UnavailableAndUndefinedSystemPairingsAreRefused) {
  VBITeletextLineMap line_map;
  std::string error;

  EXPECT_FALSE(make_vbi_teletext_line_map(
      VBITVSystem::kNTSC, VBITeletextSystem::kNABTS, line_map, error));
  EXPECT_NE(error.find("NABTS"), std::string::npos);

  error.clear();
  EXPECT_FALSE(make_vbi_teletext_line_map(
      VBITVSystem::kPAL, VBITeletextSystem::kNABTS, line_map, error));
  EXPECT_NE(error.find("PAL"), std::string::npos);
  EXPECT_TRUE(line_map.field1.empty());
}

}  // namespace
}  // namespace orc
