/*
 * File:        vectorscope_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests for the vectorscope dialog's acquisition
 *              controls and their round-trip into a preview coordinate
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview/vectorscope_dialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QGroupBox>
#include <QRadioButton>
#include <QSignalSpy>

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-vectorscope-dialog-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

// The dialog owns its controls privately; drive them the way a user would, by
// name, so the test exercises the same wiring the UI does.
QRadioButton* findRadio(QWidget& parent, const QString& text) {
  for (QRadioButton* button : parent.findChildren<QRadioButton*>()) {
    if (button->text() == text) return button;
  }
  return nullptr;
}

}  // namespace

TEST(VectorscopeDialogTest, CanShowAndClose) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();
  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();
  EXPECT_FALSE(dialog.isVisible());
}

TEST(VectorscopeDialogTest, DefaultsToTheDecodedAcquisition) {
  (void)ensureApplication();

  VectorscopeDialog dialog;

  // A colour-domain stage is the decoded plot's home, and that is the state
  // the dialog starts in until told otherwise.
  EXPECT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::DecodedComponent);
  EXPECT_EQ(dialog.sampleWindow(), orc::VectorscopeSampleWindow::WholeLine);
  EXPECT_EQ(dialog.firstLine(), 0u);
  EXPECT_EQ(dialog.lastLine(), 0u);
}

TEST(VectorscopeDialogTest, SamplingControls_RoundTripIntoACoordinate) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();

  QRadioButton* burst_only = findRadio(dialog, "Burst only");
  ASSERT_NE(burst_only, nullptr);
  burst_only->click();
  QCoreApplication::processEvents();

  EXPECT_EQ(dialog.sampleWindow(), orc::VectorscopeSampleWindow::BurstOnly);

  // The acquisition itself is not part of the request: it follows the data
  // type the stage produces, which the registry resolves.
  orc::PreviewCoordinate coordinate;
  dialog.applyAcquisitionTo(coordinate);
  EXPECT_EQ(coordinate.vectorscope_window,
            orc::VectorscopeSampleWindow::BurstOnly);
  // "All lines" is still set, so the range is left open-ended.
  EXPECT_EQ(coordinate.vectorscope_first_line, 0u);
  EXPECT_EQ(coordinate.vectorscope_last_line, 0u);
}

TEST(VectorscopeDialogTest, SamplingControls_AreHiddenForTheDecodedPlot) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  QWidget* sampling = nullptr;
  QWidget* measurements = nullptr;
  for (QGroupBox* box : dialog.findChildren<QGroupBox*>()) {
    if (box->title() == "Sampling") sampling = box;
    if (box->title() == "Measurements") measurements = box;
  }
  ASSERT_NE(sampling, nullptr);
  ASSERT_NE(measurements, nullptr);

  // The sampling window, line select and burst readouts only describe a
  // composite acquisition; on the decoded plot they would be controls with
  // nothing to act on.
  EXPECT_FALSE(sampling->isVisible());
  EXPECT_FALSE(measurements->isVisible());

  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();

  EXPECT_TRUE(sampling->isVisible());
  EXPECT_TRUE(measurements->isVisible());

  dialog.close();
  QCoreApplication::processEvents();
}

TEST(VectorscopeDialogTest, LineRange_IsReportedZeroBased) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);

  QCheckBox* all_lines = nullptr;
  for (QCheckBox* box : dialog.findChildren<QCheckBox*>()) {
    if (box->text() == "All lines") all_lines = box;
  }
  ASSERT_NE(all_lines, nullptr);

  auto spin_boxes = dialog.findChildren<QSpinBox*>();
  ASSERT_GE(spin_boxes.size(), 3);

  all_lines->setChecked(false);
  QCoreApplication::processEvents();

  // The spin boxes are the only ones with a 1..625 range; the gain spin box
  // uses 1..10.
  QSpinBox* first = nullptr;
  QSpinBox* last = nullptr;
  for (QSpinBox* box : spin_boxes) {
    if (box->maximum() != 625) continue;
    if (first == nullptr) {
      first = box;
    } else if (last == nullptr) {
      last = box;
    }
  }
  ASSERT_NE(first, nullptr);
  ASSERT_NE(last, nullptr);

  first->setValue(101);
  last->setValue(140);
  QCoreApplication::processEvents();

  // Line numbers are 1-based in the UI and 0-based in the contract.
  EXPECT_EQ(dialog.firstLine(), 100u);
  EXPECT_EQ(dialog.lastLine(), 139u);
}

TEST(VectorscopeDialogTest, LineRange_IsNormalisedWhenInverted) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);

  QCheckBox* all_lines = nullptr;
  for (QCheckBox* box : dialog.findChildren<QCheckBox*>()) {
    if (box->text() == "All lines") all_lines = box;
  }
  ASSERT_NE(all_lines, nullptr);
  all_lines->setChecked(false);

  QSpinBox* first = nullptr;
  QSpinBox* last = nullptr;
  for (QSpinBox* box : dialog.findChildren<QSpinBox*>()) {
    if (box->maximum() != 625) continue;
    if (first == nullptr) {
      first = box;
    } else if (last == nullptr) {
      last = box;
    }
  }
  ASSERT_NE(first, nullptr);
  ASSERT_NE(last, nullptr);

  first->setValue(200);
  last->setValue(150);
  QCoreApplication::processEvents();

  EXPECT_EQ(dialog.firstLine(), 149u);
  EXPECT_EQ(dialog.lastLine(), 199u);
}

TEST(VectorscopeDialogTest, ChangingAcquisitionRequestsFreshData) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  QSignalSpy spy(&dialog, &VectorscopeDialog::dataRefreshRequested);

  // Selecting a signal-domain stage switches the acquisition.  The two are
  // different data sets, so it has to re-ask rather than re-plot what is
  // already on screen.
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();
  EXPECT_EQ(dialog.acquisitionMode(),
            orc::VectorscopeAcquisitionMode::CompositeCarrier);
  EXPECT_EQ(spy.count(), 1);

  // Re-stating the same acquisition is not a change and must not re-acquire.
  dialog.setAcquisitionMode(orc::VectorscopeAcquisitionMode::CompositeCarrier);
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 1);

  QRadioButton* active_line = findRadio(dialog, "Active line");
  ASSERT_NE(active_line, nullptr);
  active_line->click();
  QCoreApplication::processEvents();
  EXPECT_EQ(spy.count(), 2);
}

TEST(VectorscopeDialogTest, RendersACompositeAcquisitionWithoutCrashing) {
  (void)ensureApplication();

  VectorscopeDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  orc::VectorscopeData data;
  data.system = orc::VideoSystem::PAL;
  data.cvbs_white = 844;
  data.cvbs_blanking = 256;
  data.acquisition_mode = orc::VectorscopeAcquisitionMode::CompositeCarrier;
  data.sample_window = orc::VectorscopeSampleWindow::WholeLine;
  data.first_line = 0;
  data.last_line = 624;
  data.sample_stride = 2;
  data.field_number = 3;
  data.measurements.valid = true;
  data.measurements.burst_amplitude_ire = 21.4;
  data.measurements.burst_amplitude_percent = 99.9;
  data.measurements.burst_phase_jitter_degrees = 0.4;
  data.measurements.burst_line_count = 576;
  data.measurements.chroma_to_burst_ratio = 1.5;

  data.samples.emplace_back(-5000.0, 5000.0, 0,
                            orc::VectorscopeSampleClass::Burst,
                            orc::VectorscopeLinePhase::VPositive, 100);
  data.samples.emplace_back(-5000.0, -5000.0, 0,
                            orc::VectorscopeSampleClass::Burst,
                            orc::VectorscopeLinePhase::VNegative, 101);
  data.samples.emplace_back(-3600.0, 15100.0, 0,
                            orc::VectorscopeSampleClass::Picture,
                            orc::VectorscopeLinePhase::VPositive, 100);

  dialog.updateVectorscope(data);
  QCoreApplication::processEvents();

  EXPECT_TRUE(dialog.isVisible());

  dialog.close();
  QCoreApplication::processEvents();
}

}  // namespace gui_unit_test
