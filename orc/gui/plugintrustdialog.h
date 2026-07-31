/*
 * File:        plugintrustdialog.h
 * Module:      orc-gui
 * Purpose:     Explicit trust confirmation for plugin binaries
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLUGINTRUSTDIALOG_H
#define ORC_GUI_PLUGINTRUSTDIALOG_H

class QWidget;

namespace orc {

/**
 * @brief Ask the user to confirm they trust a plugin's source and author.
 *
 * Trust is the gate that allows a plugin binary to be downloaded and executed
 * as native code, so it is always granted through this explicit warning.
 * Returns true when the user confirms (OK); false on Cancel. Cancel defaults,
 * so a stray Enter cannot grant trust.
 */
bool confirmPluginTrust(QWidget* parent);

}  // namespace orc

#endif  // ORC_GUI_PLUGINTRUSTDIALOG_H
