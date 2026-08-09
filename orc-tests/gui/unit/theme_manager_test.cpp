/*
 * File:        theme_manager_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for ThemeManager theme mode parsing and resolution
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QString>

// Include headers from their installed locations
#include "theme_manager.h"

namespace gui_unit_test {

// =============================================================================
// ThemeManager Constructor and Mode Parsing Tests
// =============================================================================

TEST(ThemeManagerTest, Constructor_AutoModeFromEmptyString) {
  // Empty string should default to Auto mode
  ThemeManager manager("");
  EXPECT_EQ(manager.mode(), ThemeManager::Mode::Auto);
  EXPECT_FALSE(manager.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_AutoModeFromAutoString) {
  // "auto" string should set Auto mode
  ThemeManager manager("auto");
  EXPECT_EQ(manager.mode(), ThemeManager::Mode::Auto);
  EXPECT_FALSE(manager.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_LightMode) {
  // "light" string should set Light mode
  ThemeManager manager("light");
  EXPECT_EQ(manager.mode(), ThemeManager::Mode::Light);
  EXPECT_FALSE(manager.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_DarkMode) {
  // "dark" string should set Dark mode
  ThemeManager manager("dark");
  EXPECT_EQ(manager.mode(), ThemeManager::Mode::Dark);
  EXPECT_FALSE(manager.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_LightModeCaseInsensitive) {
  // Case variations should be accepted for "light"
  ThemeManager manager1("LIGHT");
  EXPECT_EQ(manager1.mode(), ThemeManager::Mode::Light);
  EXPECT_FALSE(manager1.hadInvalidMode());

  ThemeManager manager2("Light");
  EXPECT_EQ(manager2.mode(), ThemeManager::Mode::Light);
  EXPECT_FALSE(manager2.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_DarkModeCaseInsensitive) {
  // Case variations should be accepted for "dark"
  ThemeManager manager1("DARK");
  EXPECT_EQ(manager1.mode(), ThemeManager::Mode::Dark);
  EXPECT_FALSE(manager1.hadInvalidMode());

  ThemeManager manager2("Dark");
  EXPECT_EQ(manager2.mode(), ThemeManager::Mode::Dark);
  EXPECT_FALSE(manager2.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_AutoModeCaseInsensitive) {
  // Case variations should be accepted for "auto"
  ThemeManager manager1("AUTO");
  EXPECT_EQ(manager1.mode(), ThemeManager::Mode::Auto);
  EXPECT_FALSE(manager1.hadInvalidMode());

  ThemeManager manager2("Auto");
  EXPECT_EQ(manager2.mode(), ThemeManager::Mode::Auto);
  EXPECT_FALSE(manager2.hadInvalidMode());
}

TEST(ThemeManagerTest, Constructor_InvalidModeRecordsError) {
  // Invalid mode string should be recorded
  ThemeManager manager("invalid_mode");
  EXPECT_EQ(manager.mode(), ThemeManager::Mode::Auto);  // Defaults to Auto
  EXPECT_TRUE(manager.hadInvalidMode());
  EXPECT_EQ(manager.invalidMode().toStdString(), "invalid_mode");
}

TEST(ThemeManagerTest, ConstructorInvalidMode_PreservesOriginal) {
  // Invalid mode should preserve the original string (including
  // leading/trailing spaces)
  ThemeManager manager("   Invalid   ");
  EXPECT_TRUE(manager.hadInvalidMode());
  // The invalidMode_ stores the untrimmed original string
  EXPECT_EQ(manager.invalidMode().toStdString(), "   Invalid   ");
}

// =============================================================================
// Mode and Name Queries Tests
// =============================================================================

TEST(ThemeManagerTest, ModeName_ReturnsCorrectNameForAuto) {
  ThemeManager manager("auto");
  EXPECT_EQ(manager.modeName().toStdString(), "auto");
}

TEST(ThemeManagerTest, ModeName_ReturnsCorrectNameForLight) {
  ThemeManager manager("light");
  EXPECT_EQ(manager.modeName().toStdString(), "light");
}

TEST(ThemeManagerTest, ModeName_ReturnsCorrectNameForDark) {
  ThemeManager manager("dark");
  EXPECT_EQ(manager.modeName().toStdString(), "dark");
}

TEST(ThemeManagerTest, Should_TrackSystemChangesTrueForAuto) {
  // Auto mode should track system changes
  ThemeManager manager("auto");
  EXPECT_TRUE(manager.shouldTrackSystemChanges());
}

TEST(ThemeManagerTest, Should_TrackSystemChangesFalseForLight) {
  // Light mode should not track system changes (explicitly set)
  ThemeManager manager("light");
  EXPECT_FALSE(manager.shouldTrackSystemChanges());
}

TEST(ThemeManagerTest, Should_TrackSystemChangesFalseForDark) {
  // Dark mode should not track system changes (explicitly set)
  ThemeManager manager("dark");
  EXPECT_FALSE(manager.shouldTrackSystemChanges());
}

// =============================================================================
// Static String Conversion Tests
// =============================================================================

TEST(ThemeManagerTest, Mode_ToStringAuto) {
  QString result = ThemeManager::modeToString(ThemeManager::Mode::Auto);
  EXPECT_EQ(result.toStdString(), "auto");
}

TEST(ThemeManagerTest, Mode_ToStringLight) {
  QString result = ThemeManager::modeToString(ThemeManager::Mode::Light);
  EXPECT_EQ(result.toStdString(), "light");
}

TEST(ThemeManagerTest, Mode_ToStringDark) {
  QString result = ThemeManager::modeToString(ThemeManager::Mode::Dark);
  EXPECT_EQ(result.toStdString(), "dark");
}

TEST(ThemeManagerTest, Color_SchemeToStringDark) {
  QString result = ThemeManager::colorSchemeToString(Qt::ColorScheme::Dark);
  EXPECT_EQ(result.toStdString(), "dark");
}

TEST(ThemeManagerTest, Color_SchemeToStringLight) {
  QString result = ThemeManager::colorSchemeToString(Qt::ColorScheme::Light);
  EXPECT_EQ(result.toStdString(), "light");
}

TEST(ThemeManagerTest, Color_SchemeToStringUnknown) {
  QString result = ThemeManager::colorSchemeToString(Qt::ColorScheme::Unknown);
  EXPECT_EQ(result.toStdString(), "unknown");
}

// =============================================================================
// Resolution Tests
// =============================================================================

TEST(ThemeManagerTest, ResolveLightMode_ReturnsLight) {
  ThemeManager manager("light");

  // Create a minimal QApplication for testing
  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);

  EXPECT_EQ(resolution.mode, ThemeManager::Mode::Light);
  EXPECT_EQ(resolution.scheme, Qt::ColorScheme::Light);
  EXPECT_FALSE(resolution.isDark);
  EXPECT_FALSE(resolution.usedPaletteFallback);
  EXPECT_EQ(resolution.source.toStdString(), "cli override");
}

TEST(ThemeManagerTest, ResolveDarkMode_ReturnsDark) {
  ThemeManager manager("dark");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);

  EXPECT_EQ(resolution.mode, ThemeManager::Mode::Dark);
  EXPECT_EQ(resolution.scheme, Qt::ColorScheme::Dark);
  EXPECT_TRUE(resolution.isDark);
  EXPECT_FALSE(resolution.usedPaletteFallback);
  EXPECT_EQ(resolution.source.toStdString(), "cli override");
}

TEST(ThemeManagerTest, ResolveAutoMode_UsesSystemScheme) {
  ThemeManager manager("auto");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);

  // Auto mode should resolve to something (either Light or Dark)
  EXPECT_EQ(resolution.mode, ThemeManager::Mode::Auto);
  EXPECT_TRUE(resolution.scheme == Qt::ColorScheme::Light ||
              resolution.scheme == Qt::ColorScheme::Dark ||
              resolution.scheme == Qt::ColorScheme::Unknown);

  // Should indicate what was used (style hints or palette fallback)
  EXPECT_TRUE(resolution.source.contains("style hints") ||
              resolution.source.contains("palette fallback"));
}

TEST(ThemeManagerTest, Resolve_LightModeNoFallback) {
  // Light mode should not use palette fallback (it's explicitly set)
  ThemeManager manager("light");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);
  EXPECT_FALSE(resolution.usedPaletteFallback);
}

TEST(ThemeManagerTest, Resolve_DarkModeNoFallback) {
  // Dark mode should not use palette fallback (it's explicitly set)
  ThemeManager manager("dark");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);
  EXPECT_FALSE(resolution.usedPaletteFallback);
}

// =============================================================================
// Resolution Structure Tests
// =============================================================================

TEST(ThemeManagerTest, Resolution_IncludesAllFields) {
  ThemeManager manager("light");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);

  // Verify all fields are populated
  EXPECT_FALSE(resolution.source.isEmpty());

  // isDark and usedPaletteFallback should have sensible values
  EXPECT_TRUE(resolution.isDark == false);  // Explicitly set light mode
  EXPECT_TRUE(resolution.usedPaletteFallback == false);
}

TEST(ThemeManagerTest, ResolutionLight_IsDarkIsFalse) {
  ThemeManager manager("light");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);
  EXPECT_FALSE(resolution.isDark);
}

TEST(ThemeManagerTest, ResolutionDark_IsDarkIsTrue) {
  ThemeManager manager("dark");

  char* argv[] = {const_cast<char*>("test")};
  int argc = 1;
  QApplication app(argc, argv);

  auto resolution = manager.resolve(app);
  EXPECT_TRUE(resolution.isDark);
}

// =============================================================================
// Native Style Fallback Tests
// =============================================================================

TEST(ThemeManagerTest, StyleFallback_WindowsVistaDark_NeedsFallback) {
  // The Windows 10 style has no dark rendering at all, so a dark palette on top
  // of it leaves unreadable light chrome (issue #250).
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback("windowsvista", true,
                                                      Qt::ColorScheme::Dark));
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback("windowsvista", true,
                                                      Qt::ColorScheme::Light));
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback(
      "windowsvista", true, Qt::ColorScheme::Unknown));
}

TEST(ThemeManagerTest, StyleFallback_WindowsVistaLight_KeepsNativeStyle) {
  EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback("windowsvista", false,
                                                       Qt::ColorScheme::Light));
  EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback("windowsvista", false,
                                                       Qt::ColorScheme::Dark));
}

TEST(ThemeManagerTest, StyleFallback_Windows11MatchingScheme_KeepsNativeStyle) {
  // The Windows 11 style paints whichever scheme Qt reports for the desktop.
  EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback("windows11", true,
                                                       Qt::ColorScheme::Dark));
  EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback("windows11", false,
                                                       Qt::ColorScheme::Light));
}

TEST(ThemeManagerTest, StyleFallback_Windows11OpposedScheme_NeedsFallback) {
  // Forcing a theme the desktop is not using cannot be done natively.
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback("windows11", true,
                                                      Qt::ColorScheme::Light));
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback("windows11", false,
                                                      Qt::ColorScheme::Dark));
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback(
      "windows11", true, Qt::ColorScheme::Unknown));
}

TEST(ThemeManagerTest, StyleFallback_StyleNameIsCaseInsensitive) {
  EXPECT_TRUE(ThemeManager::styleNeedsPaletteFallback(" WindowsVista ", true,
                                                      Qt::ColorScheme::Dark));
}

TEST(ThemeManagerTest, StyleFallback_PaletteDrivenStyles_KeepNativeStyle) {
  // Fusion and the Linux desktop styles follow the palette; macOS handles both
  // schemes itself. None of them need replacing.
  for (const char* styleName : {"fusion", "macos", "breeze", "windows", ""}) {
    EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback(
        styleName, true, Qt::ColorScheme::Light))
        << styleName;
    EXPECT_FALSE(ThemeManager::styleNeedsPaletteFallback(styleName, false,
                                                         Qt::ColorScheme::Dark))
        << styleName;
  }
}

TEST(ThemeManagerTest, StyleFallback_FallbackStyleIsFusion) {
  EXPECT_EQ(ThemeManager::paletteFallbackStyleName().toStdString(), "Fusion");
}

}  // namespace gui_unit_test
