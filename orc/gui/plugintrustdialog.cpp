/*
 * File:        plugintrustdialog.cpp
 * Module:      orc-gui
 * Purpose:     Explicit trust confirmation for plugin binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugintrustdialog.h"

#include <QMessageBox>

namespace orc {

bool confirmPluginTrust(QWidget* parent) {
  const auto answer = QMessageBox::warning(
      parent, "Plugin Trust",
      "Warning! Plugins execute code locally on your computer - Are you sure "
      "you trust the source and author of this plugin?",
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  return answer == QMessageBox::Ok;
}

}  // namespace orc
