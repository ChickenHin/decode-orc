/*
 * File:        theme_manager.h
 * Module:      orc-gui
 * Purpose:     Centralized theme mode parsing and resolution
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <QApplication>
#include <QString>

class ThemeManager {
 public:
  enum class Mode {
    Auto,
    Light,
    Dark,
  };

  struct Resolution {
    Mode mode;
    Qt::ColorScheme scheme;
    bool isDark;
    bool usedPaletteFallback;
    QString source;
  };

  explicit ThemeManager(const QString& modeArgument);

  bool hadInvalidMode() const;
  QString invalidMode() const;

  Mode mode() const;
  QString modeName() const;
  bool shouldTrackSystemChanges() const;

  Resolution resolve(const QApplication& app) const;

  static QString modeToString(Mode mode);
  static QString colorSchemeToString(Qt::ColorScheme scheme);

  // Reports whether a widget style named `styleName` has to be replaced by the
  // palette-driven fallback style in order to render a dark (`isDark`) or light
  // theme correctly, given that Qt reports `osScheme` for the desktop.
  //
  // Some native styles paint their controls from the OS theme instead of the
  // application palette. Applying a palette they ignore leaves native light
  // chrome with light palette text drawn on top, which is unreadable.
  static bool styleNeedsPaletteFallback(const QString& styleName, bool isDark,
                                        Qt::ColorScheme osScheme);

  // Name of the fully palette-driven style used when the native style cannot
  // render the requested theme.
  static QString paletteFallbackStyleName();

 private:
  static bool isPaletteDark(const QPalette& palette);

  Mode mode_;
  QString invalidMode_;
};

// Enable ThemeManager::Mode to travel through QVariant and Qt signal/slot
// connections (used by ThemeController::modeChanged).
Q_DECLARE_METATYPE(ThemeManager::Mode)