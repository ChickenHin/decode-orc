/*
 * File:        plugintrustdialog.cpp
 * Module:      orc-gui
 * Purpose:     Explicit trust confirmation for plugin binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugintrustdialog.h"

#include <plugin_ux_strings.h>

#include <QMessageBox>

namespace orc {

bool confirmPluginTrust(QWidget* parent) {
  const auto answer = QMessageBox::warning(
      parent, QString::fromUtf8(plugin_ux::kTrustDialogTitle),
      QString::fromUtf8(plugin_ux::kTrustWarning),
      QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Cancel);
  return answer == QMessageBox::Ok;
}

}  // namespace orc
