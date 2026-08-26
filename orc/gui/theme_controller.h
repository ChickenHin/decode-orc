/*
 * File:        theme_controller.h
 * Module:      orc-gui
 * Purpose:     Runtime application of theme modes and OS colour-scheme tracking
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>

#include "theme_manager.h"

class QApplication;

// Owns the process-wide theme state and applies it to a QApplication.
//
// ThemeManager parses/resolves a theme mode into a concrete light/dark
// resolution; ThemeController drives the impure side of theming: it applies the
// resolved palette and stylesheet, broadcasts theme updates to live widgets,
// persists the user's choice, and (in auto mode) tracks OS colour-scheme
// changes. A single instance is expected to exist for the lifetime of the GUI;
// instance() exposes it so menu actions can change the theme at runtime.
//
// Thread-safety: must only be used on the GUI (main) thread.
class ThemeController : public QObject {
  Q_OBJECT

 public:
  // Constructs the controller from an initial mode argument (as accepted by
  // ThemeManager, e.g. "auto"/"light"/"dark") and applies it immediately. The
  // initial mode is not persisted; only subsequent setMode() calls are, so a
  // command-line --theme override remains a one-shot for the run.
  ThemeController(QApplication& app, const QString& initialModeArgument,
                  QObject* parent = nullptr);
  ~ThemeController() override;

  // Returns the active controller, or nullptr if none has been constructed.
  static ThemeController* instance();

  // Widget style that should be installed to render an `isDark` theme on a
  // platform whose native style is `nativeStyleName`, with Qt reporting
  // `osScheme` for the desktop. Returns the native style unless it cannot paint
  // the requested theme, in which case the palette-driven fallback is used.
  static QString styleNameForTheme(const QString& nativeStyleName, bool isDark,
                                   Qt::ColorScheme osScheme);

  ThemeManager::Mode mode() const;

  // Changes the active theme mode, persists it, re-applies the theme, and
  // updates OS colour-scheme tracking. Emits modeChanged().
  void setMode(ThemeManager::Mode mode);

 signals:
  void modeChanged(ThemeManager::Mode mode);

 private:
  // Resolves the current mode against the application and applies the result.
  void applyCurrentMode(const QString& changeReason);
  void applyResolution(const ThemeManager::Resolution& resolution);
  // Connects/disconnects OS colour-scheme tracking to match the current mode.
  void updateSystemTracking();
  // Swaps in the palette-driven fallback style when the platform's native style
  // cannot render the requested theme, and restores the native style otherwise.
  void applyStyle(bool isDark);

  // Applies the light or dark palette and menu/message-box stylesheet.
  static void applyPalette(QApplication& app, bool isDark);

  QApplication& app_;
  ThemeManager::Mode mode_;
  QMetaObject::Connection tracking_connection_;
  // Style Qt selected for the platform, captured before any theme is applied.
  QString native_style_name_;
  // Style currently installed on the application (native or fallback).
  QString applied_style_name_;

  static ThemeController* s_instance;
};
