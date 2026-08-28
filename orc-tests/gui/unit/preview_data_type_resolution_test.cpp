/*
 * File:        preview_data_type_resolution_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tests for reconciling the previewed data type with what the
 *              selected stage declares it can preview
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview/preview_data_type_resolution.h"

#include <gtest/gtest.h>

namespace gui_unit_test {

TEST(PreviewDataTypeResolutionTest, ColourDomainTypesAreTheDecoderOutputs) {
  EXPECT_TRUE(orc::gui::isColourDomainDataType(orc::VideoDataType::ColourPAL));
  EXPECT_TRUE(orc::gui::isColourDomainDataType(orc::VideoDataType::ColourNTSC));

  EXPECT_FALSE(
      orc::gui::isColourDomainDataType(orc::VideoDataType::CompositePAL));
  EXPECT_FALSE(
      orc::gui::isColourDomainDataType(orc::VideoDataType::CompositeNTSC));
  EXPECT_FALSE(orc::gui::isColourDomainDataType(orc::VideoDataType::YC_PAL));
  EXPECT_FALSE(orc::gui::isColourDomainDataType(orc::VideoDataType::YC_NTSC));
}

TEST(PreviewDataTypeResolutionTest, StageDeclaringTheCandidate_KeepsIt) {
  // A decoding sink previewed in an interlaced frame mode really is showing
  // colour, and declares it.
  const std::vector<orc::VideoDataType> sink_types = {
      orc::VideoDataType::ColourPAL, orc::VideoDataType::CompositePAL};

  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::ColourPAL,
                                             sink_types),
            orc::VideoDataType::ColourPAL);
  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::CompositePAL,
                                             sink_types),
            orc::VideoDataType::CompositePAL);
}

TEST(PreviewDataTypeResolutionTest,
     SignalDomainStage_IsNeverReportedAsColourDomain) {
  // This is the case the reconciliation exists for: a source also offers an
  // interlaced "Frame" preview mode, which implies a colour-domain type, but
  // it produces no colour.  Taking the implication at face value filtered out
  // every view registered for the stage's real type.
  const std::vector<orc::VideoDataType> source_types = {
      orc::VideoDataType::CompositePAL};

  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::ColourPAL,
                                             source_types),
            orc::VideoDataType::CompositePAL);
  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::YC_PAL,
                                             source_types),
            orc::VideoDataType::CompositePAL);
}

TEST(PreviewDataTypeResolutionTest, UndeclaredCandidate_FallsBackToTheFirst) {
  // Stages list the domain they principally produce first, so that is the
  // best answer when the implied type is not on offer at all.
  const std::vector<orc::VideoDataType> types = {orc::VideoDataType::ColourNTSC,
                                                 orc::VideoDataType::YC_NTSC};

  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::CompositeNTSC,
                                             types),
            orc::VideoDataType::ColourNTSC);
}

TEST(PreviewDataTypeResolutionTest, StageWithNoCapability_LeavesTheCandidate) {
  // Nothing to reconcile against; the output type's implication stands.
  EXPECT_EQ(
      orc::gui::resolvePreviewDataType(orc::VideoDataType::CompositePAL, {}),
      orc::VideoDataType::CompositePAL);
  EXPECT_EQ(orc::gui::resolvePreviewDataType(orc::VideoDataType::ColourPAL, {}),
            orc::VideoDataType::ColourPAL);
}

}  // namespace gui_unit_test
