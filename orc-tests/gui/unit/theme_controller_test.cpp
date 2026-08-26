/*
 * File:        theme_controller_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for ThemeController runtime theme application
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "theme_controller.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QPalette>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>

#include "theme_manager.h"

namespace gui_unit_test {
namespace {

// Redirects QSettings writes to an isolated temporary location so setMode()
// persistence does not touch the developer's real configuration.
struct IsolatedSettings {
  IsolatedSettings() { QStandardPaths::setTestModeEnabled(true); }
  ~IsolatedSettings() { QStandardPaths::setTestModeEnabled(false); }
};

// Constructs a QApplication for a single test. Only one may exist at a time, so
// every test owns its instance for the duration of the test body.
struct TestApp {
  int argc = 1;
  char arg0[5] = {'t', 'e', 's', 't', '\0'};
  char* argv[1] = {arg0};
  QApplication app{argc, argv};
};

}  // namespace

// =============================================================================
// Construction and application
// =============================================================================

TEST(ThemeControllerTest, Construct_DarkMode_AppliesDarkProperties) {
  TestApp harness;
  ThemeController controller(harness.app, "dark");

  EXPECT_EQ(controller.mode(), ThemeManager::Mode::Dark);
  EXPECT_TRUE(harness.app.property("isDarkTheme").toBool());
  EXPECT_EQ(harness.app.property("themeMode").toString().toStdString(), "dark");
}

TEST(ThemeControllerTest, Construct_LightMode_AppliesLightProperties) {
  TestApp harness;
  ThemeController controller(harness.app, "light");

  EXPECT_EQ(controller.mode(), ThemeManager::Mode::Light);
  EXPECT_FALSE(harness.app.property("isDarkTheme").toBool());
  EXPECT_EQ(harness.app.property("themeMode").toString().toStdString(),
            "light");
}

TEST(ThemeControllerTest, Construct_InvalidMode_FallsBackToAuto) {
  TestApp harness;
  ThemeController controller(harness.app, "chartreuse");

  EXPECT_EQ(controller.mode(), ThemeManager::Mode::Auto);
}

// =============================================================================
// instance() lifecycle
// =============================================================================

TEST(ThemeControllerTest, Instance_TracksActiveController) {
  TestApp harness;
  EXPECT_EQ(ThemeController::instance(), nullptr);
  {
    ThemeController controller(harness.app, "auto");
    EXPECT_EQ(ThemeController::instance(), &controller);
  }
  EXPECT_EQ(ThemeController::instance(), nullptr);
}

// =============================================================================
// setMode() runtime override
// =============================================================================

TEST(ThemeControllerTest, SetMode_ChangesModeAndReappliesTheme) {
  IsolatedSettings settings;
  TestApp harness;
  ThemeController controller(harness.app, "light");
  ASSERT_FALSE(harness.app.property("isDarkTheme").toBool());

  QSignalSpy spy(&controller, &ThemeController::modeChanged);
  controller.setMode(ThemeManager::Mode::Dark);

  EXPECT_EQ(controller.mode(), ThemeManager::Mode::Dark);
  EXPECT_TRUE(harness.app.property("isDarkTheme").toBool());
  ASSERT_EQ(spy.count(), 1);
  EXPECT_EQ(spy.takeFirst().at(0).value<ThemeManager::Mode>(),
            ThemeManager::Mode::Dark);
}

TEST(ThemeControllerTest, SetMode_SameMode_IsNoOp) {
  IsolatedSettings settings;
  TestApp harness;
  ThemeController controller(harness.app, "dark");

  QSignalSpy spy(&controller, &ThemeController::modeChanged);
  controller.setMode(ThemeManager::Mode::Dark);

  EXPECT_EQ(spy.count(), 0);
}

// =============================================================================
// Palette completeness
// =============================================================================

namespace {

// Roles a palette-driven style reads when painting frames, bevels and control
// faces. Any of them left unset would fall back to the platform system palette,
// which stays light on Windows even in dark mode.
constexpr QPalette::ColorRole kShadingRoles[] = {
    QPalette::Window, QPalette::Base,     QPalette::Button,
    QPalette::Light,  QPalette::Midlight, QPalette::Mid,
    QPalette::Dark,   QPalette::Shadow,   QPalette::AlternateBase,
};

}  // namespace

TEST(ThemeControllerTest, DarkMode_ShadingRolesAreAllDark) {
  TestApp harness;
  ThemeController controller(harness.app, "dark");

  const QPalette palette = harness.app.palette();
  for (const QPalette::ColorRole role : kShadingRoles) {
    EXPECT_LT(palette.color(QPalette::Active, role).lightness(), 128)
        << "active role " << static_cast<int>(role);
    EXPECT_LT(palette.color(QPalette::Disabled, role).lightness(), 128)
        << "disabled role " << static_cast<int>(role);
  }
}

TEST(ThemeControllerTest, DarkMode_TextContrastsWithItsBackground) {
  TestApp harness;
  ThemeController controller(harness.app, "dark");

  const QPalette palette = harness.app.palette();
  EXPECT_GT(palette.color(QPalette::WindowText).lightness(),
            palette.color(QPalette::Window).lightness());
  EXPECT_GT(palette.color(QPalette::ButtonText).lightness(),
            palette.color(QPalette::Button).lightness());
  EXPECT_GT(palette.color(QPalette::Text).lightness(),
            palette.color(QPalette::Base).lightness());
}

// =============================================================================
// Widget style selection
// =============================================================================

TEST(ThemeControllerTest, StyleForTheme_Windows10Dark_UsesFusion) {
  // Windows 10's native style cannot paint dark chrome, so the dark theme has
  // to be rendered by the palette-driven fallback (issue #250)...
  EXPECT_EQ(ThemeController::styleNameForTheme("windowsvista", true,
                                               Qt::ColorScheme::Dark)
                .toStdString(),
            "Fusion");

  // ...and the native style has to come back for the light theme.
  EXPECT_EQ(ThemeController::styleNameForTheme("windowsvista", false,
                                               Qt::ColorScheme::Dark)
                .toStdString(),
            "windowsvista");
}

TEST(ThemeControllerTest, StyleForTheme_CapableNativeStyle_IsKept) {
  EXPECT_EQ(
      ThemeController::styleNameForTheme("fusion", true, Qt::ColorScheme::Light)
          .toStdString(),
      "fusion");
  EXPECT_EQ(ThemeController::styleNameForTheme("windows11", true,
                                               Qt::ColorScheme::Dark)
                .toStdString(),
            "windows11");
}

TEST(ThemeControllerTest, StyleForTheme_FallbackStyleIsAvailable) {
  TestApp harness;
  // The fallback is only useful if Qt can actually create it.
  EXPECT_TRUE(QStyleFactory::keys().contains(
      ThemeManager::paletteFallbackStyleName(), Qt::CaseInsensitive));
}

TEST(ThemeControllerTest, Construct_KeepsPaletteDrivenNativeStyle) {
  // The fallback only replaces native styles that cannot render the requested
  // theme; the styles used on the test platform render it themselves.
  TestApp harness;
  // QApplication keeps the application stylesheet in process-wide state, so a
  // controller from an earlier test leaves this QApplication wrapped in an
  // unnamed QStyleSheetStyle. Clearing it restores the real platform style,
  // which is the state a freshly launched application starts in.
  harness.app.setStyleSheet(QString());
  ASSERT_NE(harness.app.style(), nullptr);
  const QString nativeStyle = harness.app.style()->name();
  ASSERT_FALSE(nativeStyle.isEmpty())
      << "QStyle::name() must identify the platform style for the fallback "
         "decision to be possible";
  ASSERT_FALSE(ThemeManager::styleNeedsPaletteFallback(nativeStyle, true,
                                                       Qt::ColorScheme::Light));

  IsolatedSettings settings;
  {
    ThemeController controller(harness.app, "dark");
    controller.setMode(ThemeManager::Mode::Light);
    controller.setMode(ThemeManager::Mode::Dark);
  }

  // Reading the name again needs the stylesheet wrapper out of the way.
  harness.app.setStyleSheet(QString());
  EXPECT_EQ(harness.app.style()->name().toStdString(),
            nativeStyle.toStdString());
}

}  // namespace gui_unit_test
