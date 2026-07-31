/*
 * File:        plugin_json.h
 * Module:      orc-cli
 * Purpose:     Machine-readable projection of the plugin presenter types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CLI_PLUGIN_JSON_H
#define ORC_CLI_PLUGIN_JSON_H

#include "json_writer.h"
#include "project_presenter_types.h"

namespace orc {
namespace cli {

// The human table and this JSON share one source — the presenter's `*Info`
// struct — and are two projections of it, so neither can drift about *what* is
// described while each stays free about how.
//
// JSON is never serialised from the PluginDetailField list: those hold display
// labels ("Plugin ID"), pre-rendered strings and omitted-when-empty entries, so
// serialising them would emit label-keyed, type-erased output. Here every key
// is the struct's own field name, enums are written with the presenter's
// stable-id helpers rather than their labels, booleans and numbers are written
// as JSON booleans and numbers, and a field with nothing to say is still
// present — as "", [] or null — so a script sees one object shape per command.

/**
 * @brief Write one installed plugin.
 *
 * @param json Receives the object.
 * @param entry Registry entry to describe.
 * @param update Update check for @p entry, or nullptr when none was made; the
 *               `update` member is null in that case rather than absent.
 */
void write_registry_entry_json(
    JsonWriter* json, const orc::presenters::PluginRegistryEntryInfo& entry,
    const orc::presenters::PluginUpdateStatusInfo* update);

/// Write one plugin the curated index offers. Carries `selector` alongside
/// `id` — the same string — so one field name means "pass this back" whichever
/// listing a script read.
void write_index_entry_json(JsonWriter* json,
                            const orc::presenters::PluginIndexEntryInfo& entry);

/// Write one update-check result, keyed by the selector it applies to.
void write_update_status_json(
    JsonWriter* json, const orc::presenters::PluginUpdateStatusInfo& status);

/// Write one plugin-runtime diagnostic.
void write_diagnostic_json(
    JsonWriter* json, const orc::presenters::PluginDiagnosticInfo& diagnostic);

/// Write one plugin loaded by the running process.
void write_loaded_plugin_json(JsonWriter* json,
                              const orc::presenters::LoadedPluginInfo& loaded);

}  // namespace cli
}  // namespace orc

#endif  // ORC_CLI_PLUGIN_JSON_H
