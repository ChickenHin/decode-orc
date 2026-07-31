/*
 * File:        plugin_json.cpp
 * Module:      orc-cli
 * Purpose:     Machine-readable projection of the plugin presenter types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plugin_json.h"

#include "plugin_details.h"
#include "plugin_load_state.h"

namespace orc {
namespace cli {

void write_registry_entry_json(
    JsonWriter* json, const orc::presenters::PluginRegistryEntryInfo& entry,
    const orc::presenters::PluginUpdateStatusInfo* update) {
  json->begin_object();
  // The selector leads here as it does in the table: it is what every command
  // that takes a plugin accepts, so a script needs `.selector` and never
  // string surgery on the other fields.
  json->member("selector", entry.selector);
  json->member("plugin_id", entry.plugin_id);
  json->member("plugin_version", entry.plugin_version);
  json->member("path", entry.path);
  json->member("source_repo_url", entry.source_repo_url);
  json->member("artifact_source", entry.artifact_source);
  json->member("release_asset_url", entry.release_asset_url);
  json->member("release_tag", entry.release_tag);
  json->member("release_asset_name", entry.release_asset_name);
  json->member("target_platform", entry.target_platform);
  json->member("local_dev_path", entry.local_dev_path);
  json->member("source_label", entry.source_label);
  json->member_bool("enabled", entry.enabled);
  json->member("trust_state", entry.trust_state);
  json->member("license_spdx", entry.license_spdx);
  json->member_bool("is_core_plugin", entry.is_core_plugin);
  json->member_int("required_host_abi", entry.required_host_abi);
  json->member_int("host_abi_version", entry.host_abi_version);
  json->member_bool("abi_compatible", entry.abi_compatible);
  json->member("sha256", entry.sha256);
  json->member_bool("is_loaded", entry.is_loaded);
  json->member_bool("path_exists", entry.path_exists);
  json->member("load_state",
               orc::presenters::pluginLoadStateId(entry.load_state));
  json->member("load_state_detail", entry.load_state_detail);

  // Checking for updates costs a network request, so an entry listed without
  // --check-updates says so with null rather than by dropping the field.
  if (update == nullptr) {
    json->member_null("update");
  } else {
    json->key("update");
    write_update_status_json(json, *update);
  }
  json->end_object();
}

void write_index_entry_json(
    JsonWriter* json, const orc::presenters::PluginIndexEntryInfo& entry) {
  json->begin_object();
  json->member("selector", entry.id);
  json->member("id", entry.id);
  json->member("display_name", entry.display_name);
  json->member("description", entry.description);
  json->member("version", entry.version);
  json->member("latest_tag", entry.latest_tag);
  json->member("maintainer", entry.maintainer);
  json->member("license_spdx", entry.license_spdx);
  json->member("source_repo_url", entry.source_repo_url);
  json->member_strings("tags", entry.tags);
  json->member_bool("has_compatible_build", entry.has_compatible_build);
  json->member_bool("release_unreachable", entry.release_unreachable);
  json->member_bool("already_installed", entry.already_installed);
  json->member("compatibility_message", entry.compatibility_message);
  json->end_object();
}

void write_update_status_json(
    JsonWriter* json, const orc::presenters::PluginUpdateStatusInfo& status) {
  json->begin_object();
  // The plugin id an update check reports is the entry's selector, so this
  // output feeds `plugins update` directly.
  json->member("selector", status.plugin_id);
  json->member("plugin_id", status.plugin_id);
  json->member("installed_version", status.installed_version);
  json->member("latest_version", status.latest_version);
  json->member("latest_tag", status.latest_tag);
  json->member("status", orc::presenters::pluginUpdateStatusId(status.status));
  json->member("message", status.message);
  json->end_object();
}

void write_diagnostic_json(
    JsonWriter* json, const orc::presenters::PluginDiagnosticInfo& diagnostic) {
  json->begin_object();
  json->member("severity", orc::presenters::pluginDiagnosticSeverityId(
                               diagnostic.severity));
  json->member("path", diagnostic.path);
  json->member("message", diagnostic.message);
  json->end_object();
}

void write_loaded_plugin_json(JsonWriter* json,
                              const orc::presenters::LoadedPluginInfo& loaded) {
  json->begin_object();
  // A loaded plugin is addressable too — `plugins info` accepts its id — so
  // the object carries a selector like every other addressable object.
  json->member("selector",
               loaded.plugin_id.empty() ? loaded.path : loaded.plugin_id);
  json->member("plugin_id", loaded.plugin_id);
  json->member("plugin_version", loaded.plugin_version);
  json->member("path", loaded.path);
  json->member("license_spdx", loaded.license_spdx);
  json->member_bool("is_core_plugin", loaded.is_core_plugin);
  json->member_strings("registered_stage_names", loaded.registered_stage_names);
  json->end_object();
}

}  // namespace cli
}  // namespace orc
