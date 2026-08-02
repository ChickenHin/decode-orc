/*
 * File:        closed_caption_observation_presenter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for ClosedCaptionObservationPresenter extraction
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "closed_caption_observation_presenter.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

namespace gui_unit_test {

using orc::FieldID;
using orc::ObservationContext;
using orc::presenters::ClosedCaptionObservationPresenter;

TEST(ClosedCaptionObservationPresenterTest, AbsentNamespace_ReportsUnobserved) {
  ObservationContext context;

  const auto view = ClosedCaptionObservationPresenter::extractFieldObservations(
      FieldID(4), &context);

  EXPECT_FALSE(view.observed);
  EXPECT_FALSE(view.present);
  EXPECT_EQ(view.data0, 0);
  EXPECT_EQ(view.data1, 0);
}

// Every field of an uncaptioned recording, and the unused field of every
// captioned NTSC frame, looks like this: observed, with nothing on it.
TEST(ClosedCaptionObservationPresenterTest,
     FieldWithoutData_ObservedNotPresent) {
  ObservationContext context;
  const FieldID field(6);
  context.set(field, "closed_caption", "present", false);

  const auto view = ClosedCaptionObservationPresenter::extractFieldObservations(
      field, &context);

  EXPECT_TRUE(view.observed);
  EXPECT_FALSE(view.present);
}

TEST(ClosedCaptionObservationPresenterTest, ExtractsBytePairAndParity) {
  ObservationContext context;
  const FieldID field(10);
  context.set(field, "closed_caption", "present", true);
  context.set(field, "closed_caption", "data0", int32_t{0x14});
  context.set(field, "closed_caption", "data1", int32_t{0x2F});
  context.set(field, "closed_caption", "parity0_valid", true);
  context.set(field, "closed_caption", "parity1_valid", false);

  const auto view = ClosedCaptionObservationPresenter::extractFieldObservations(
      field, &context);

  EXPECT_TRUE(view.observed);
  EXPECT_TRUE(view.present);
  EXPECT_EQ(view.data0, 0x14);
  EXPECT_EQ(view.data1, 0x2F);
  EXPECT_TRUE(view.parity0_valid);
  EXPECT_FALSE(view.parity1_valid);
}

// The observer writes the data keys only alongside a true "present", so a
// context missing them must not be read as a byte pair of zeroes that passed
// its parity checks.
TEST(ClosedCaptionObservationPresenterTest, MissingDataKeys_LeaveDefaults) {
  ObservationContext context;
  const FieldID field(12);
  context.set(field, "closed_caption", "present", true);

  const auto view = ClosedCaptionObservationPresenter::extractFieldObservations(
      field, &context);

  EXPECT_TRUE(view.present);
  EXPECT_EQ(view.data0, 0);
  EXPECT_EQ(view.data1, 0);
  EXPECT_FALSE(view.parity0_valid);
  EXPECT_FALSE(view.parity1_valid);
}

TEST(ClosedCaptionObservationPresenterTest, NullContext_ReportsUnobserved) {
  const auto view = ClosedCaptionObservationPresenter::extractFieldObservations(
      FieldID(0), nullptr);

  EXPECT_FALSE(view.observed);
  EXPECT_FALSE(view.present);
}

}  // namespace gui_unit_test
