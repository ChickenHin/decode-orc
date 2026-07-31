/*
 * File:        command_plugins.cpp
 * Module:      orc-cli
 * Purpose:     Plugin registry management subcommand
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "command_plugins.h"

#include <plugin_ux_strings.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "cli_exit_codes.h"
#include "detail_fields.h"
#include "json_writer.h"
#include "plugin_details.h"
#include "plugin_json.h"
#include "plugin_load_state.h"
#include "plugin_selector.h"
#include "project_presenter.h"

namespace orc {
namespace cli {

namespace {

// One line of the post-change note, worded once in plugin_ux_strings.h.
void print_registry_change_note() {
  std::cout << plugin_ux::kNotePrefix << plugin_ux::kRegistryChangeNote << "\n";
}

// --- Trust confirmation -----------------------------------------------------
//
// Ticking Enabled, adding, installing and updating are the trust-granting
// actions in the GUI, each gated by the same warning. The CLI asks the same
// question in the same words, with --yes standing in for the answer when there
// is nobody to ask.

bool stdin_is_interactive() {
#if defined(_WIN32)
  return _isatty(_fileno(stdin)) != 0;
#else
  return isatty(fileno(stdin)) != 0;
#endif
}

enum class TrustDecision {
  Granted,         ///< The user said yes, or passed --yes.
  Declined,        ///< The user answered anything else.
  NotInteractive,  ///< No terminal to ask on and no --yes.
};

TrustDecision ask_for_trust(bool assume_yes) {
  if (assume_yes) {
    return TrustDecision::Granted;
  }
  if (!stdin_is_interactive()) {
    // Never block a script on an answer that cannot arrive.
    return TrustDecision::NotInteractive;
  }

  std::cout << plugin_ux::kTrustWarning << "\n"
            << plugin_ux::kTrustPrompt << std::flush;

  std::string answer;
  if (!std::getline(std::cin, answer)) {
    return TrustDecision::Declined;
  }
  for (char& ch : answer) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  const size_t first = answer.find_first_not_of(" \t\r\n");
  const size_t last = answer.find_last_not_of(" \t\r\n");
  answer = (first == std::string::npos)
               ? std::string()
               : answer.substr(first, last - first + 1);

  return (answer == "y" || answer == "yes") ? TrustDecision::Granted
                                            : TrustDecision::Declined;
}

/// Report a refusal and map it onto the exit-code contract.
int report_trust_refusal(TrustDecision decision) {
  if (decision == TrustDecision::NotInteractive) {
    std::cerr << "Error: " << plugin_ux::kTrustNotInteractive << "\n";
  } else {
    std::cerr << plugin_ux::kTrustDeclined << "\n";
  }
  return kExitTrustDeclined;
}

/// Map a failed mutation onto the exit-code contract: a script must be able
/// to tell "does not exist" from "the network was down" from anything else.
int mutation_exit_code(
    const orc::presenters::PluginRegistryMutationResult& result) {
  switch (result.failure) {
    case orc::presenters::PluginMutationFailure::NotFound:
      return kExitNotFound;
    case orc::presenters::PluginMutationFailure::IndexUnavailable:
      return kExitIndexUnavailable;
    case orc::presenters::PluginMutationFailure::Other:
      break;
  }
  return kExitUsage;
}

/// Grant trust to an entry that has just been recorded or rewritten. Returns
/// kExitSuccess, or the contract code describing the failure.
int record_trust(const std::string& selector) {
  const auto result =
      orc::presenters::ProjectPresenter::setPluginRegistryEntryTrusted(selector,
                                                                       true);
  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return mutation_exit_code(result);
  }
  return kExitSuccess;
}

// --- Selector resolution ----------------------------------------------------

/// Resolve a selector, reporting not-found and ambiguity in the canonical
/// wording. Returns false when the caller should stop; @p exit_code carries the
/// contract code to return.
bool resolve_selector_or_report(
    const std::string& selector,
    orc::presenters::PluginSelectorResolution* resolution, int* exit_code) {
  *resolution =
      orc::presenters::ProjectPresenter::resolvePluginRegistrySelector(
          selector);
  if (resolution->status == orc::presenters::PluginSelectorStatus::Ambiguous) {
    std::cerr << "Error: "
              << orc::presenters::describeAmbiguousPluginSelector(
                     selector, resolution->candidates)
              << "\n";
    *exit_code = kExitNotFound;
    return false;
  }
  if (resolution->status != orc::presenters::PluginSelectorStatus::Resolved) {
    std::cerr << "Error: No plugin matching '" << selector
              << "' found in registry\n";
    *exit_code = kExitNotFound;
    return false;
  }
  return true;
}

// --- Update status ----------------------------------------------------------

/// The words the GUI's Update column uses, for the CLI's `update:` field.
std::string update_status_label(
    const orc::presenters::PluginUpdateStatusInfo& status) {
  return orc::presenters::pluginUpdateStatusLabel(status);
}

/// Aligned "  <field>: " label for a listing block. The field word comes from
/// the shared vocabulary in plugin_ux_strings.h, so the words in this output
/// cannot drift from the details projection both front ends render.
std::string list_field_label(const char* field, size_t width) {
  std::string label = "  ";
  label += field;
  label += ':';
  while (label.size() < width) {
    label += ' ';
  }
  return label;
}

void print_plugins_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " plugins <subcommand> [options]\n";
  std::cerr << "\n";
  std::cerr << "A <selector> is any identifier 'plugins list' prints for an "
               "entry: its\n";
  std::cerr << "plugin id, a 'path:<path>' or 'url:<asset-url>' selector, or a "
               "bare path\n";
  std::cerr << "or release asset URL.\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  list [options]                 Show installed plugins (core "
               "plugins are hidden)\n";
  std::cerr << "  add <path> [options]           Add a local plugin file to "
               "the persistent registry\n";
  std::cerr << "  add --url <releases-url>       Add a plugin from a GitHub "
               "releases URL\n";
  std::cerr << "  remove [--dry-run] <selector>  Remove a plugin from the "
               "persistent registry\n";
  std::cerr << "  enable <selector>              Enable a registered plugin "
               "(grants trust if needed)\n";
  std::cerr << "  disable <selector>             Disable a registered plugin\n";
  std::cerr << "  trust <selector>               Mark a registered plugin as "
               "trusted (allows download and loading)\n";
  std::cerr << "  untrust <selector>             Mark a registered plugin as "
               "untrusted (blocks download and loading)\n";
  std::cerr << "  search [term] [filters]        List or search the available "
               "plugins in the curated index\n";
  std::cerr << "  info <selector>                Show details for an installed "
               "or available plugin\n";
  std::cerr << "  install <id>                   Install an indexed plugin's "
               "latest release\n";
  std::cerr << "  updates                        Check registered plugins for "
               "newer upstream releases\n";
  std::cerr << "  update <selector> | --all      Update registered plugins to "
               "their latest release\n";
  std::cerr << "  doctor                         Report plugin search paths "
               "and load diagnostics\n";
  std::cerr << "\n";
  std::cerr << "Options for 'list':\n";
  std::cerr << "  --core, --all                  Include the core plugins "
               "that ship with Decode-Orc\n";
  std::cerr << "  --check-updates                Also report each entry's "
               "update status (needs network)\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'list', 'search', 'info',\n";
  std::cerr << "                                 'updates' and 'doctor'; each "
               "object carries the\n";
  std::cerr << "                                 selector to pass back\n";
  std::cerr << "\n";
  std::cerr << "Options for 'add':\n";
  std::cerr << "  --url URL                      GitHub releases URL to "
               "resolve the plugin from\n";
  std::cerr << "  --id ID                        Plugin identifier (e.g. "
               "com.example.myplugin)\n";
  std::cerr << "  --version VER                  Plugin version string\n";
  std::cerr << "  --license SPDX                 License identifier (e.g. MIT, "
               "GPL-3.0-or-later)\n";
  std::cerr << "\n";
  std::cerr << "Options for 'remove':\n";
  std::cerr << "  --dry-run                      Report the entry that would "
               "be removed and exit\n";
  std::cerr << "\n";
  std::cerr << "Options for 'search':\n";
  std::cerr << "  --installed                    Only entries already in the "
               "registry\n";
  std::cerr << "  --available                    Only entries not yet "
               "installed\n";
  std::cerr << "  --compatible                   Only entries with a build for "
               "this host\n";
  std::cerr << "\n";
  std::cerr << "Trust: 'add', 'install', 'update', 'enable' and 'trust' let a "
               "plugin binary run,\n";
  std::cerr << "so each asks for confirmation first.\n";
  std::cerr << "  --yes                          Confirm without prompting "
               "(required when stdin is not a terminal)\n";
  std::cerr << "\n";
  std::cerr << plugin_ux::kNotePrefix << plugin_ux::kRegistryChangeNote << "\n";
}

int cmd_plugins_list(int argc, char* argv[]) {
  bool show_core = false;
  bool check_updates = false;
  bool as_json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--core" || arg == "--all") {
      show_core = true;
    } else if (arg == "--check-updates") {
      check_updates = true;
    } else if (arg == "--json") {
      as_json = true;
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      std::cerr << "Usage: orc-cli plugins list [--core|--all] "
                   "[--check-updates] [--json]\n";
      return kExitUsage;
    }
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  const auto registry = orc::presenters::ProjectPresenter::readPluginRegistry();
  const auto loaded = orc::presenters::ProjectPresenter::getLoadedPlugins();

  // Only --check-updates goes to the network; the default listing is local.
  std::map<std::string, orc::presenters::PluginUpdateStatusInfo> update_status;
  if (check_updates) {
    for (auto& status :
         orc::presenters::ProjectPresenter::checkRegisteredPluginUpdates()) {
      update_status.emplace(status.plugin_id, std::move(status));
    }
  }

  // Core plugins ship with the application, so they are hidden unless asked
  // for — the same default as the Plugin Manager's Show core plugins tick.
  size_t hidden_core = 0;
  std::vector<const orc::presenters::PluginRegistryEntryInfo*> visible;
  for (const auto& e : registry.entries) {
    if (e.is_core_plugin && !show_core) {
      ++hidden_core;
      continue;
    }
    visible.push_back(&e);
  }

  if (as_json) {
    // The same entries the table lists, projected for a script: one object per
    // registry entry, in the shape PluginRegistryInfo holds them.
    JsonWriter json(std::cout);
    json.begin_object();
    json.member("registry_path", registry.registry_path);
    json.key("entries");
    json.begin_array();
    for (const auto* entry : visible) {
      const auto it = update_status.find(entry->plugin_id);
      write_registry_entry_json(
          &json, *entry, it == update_status.end() ? nullptr : &it->second);
    }
    json.end_array();
    json.key("loaded_plugins");
    json.begin_array();
    for (const auto& p : loaded) {
      if (p.is_core_plugin && !show_core) {
        continue;
      }
      write_loaded_plugin_json(&json, p);
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  std::cout << "Registry path: "
            << (registry.registry_path.empty() ? "<none>"
                                               : registry.registry_path)
            << "\n\n";

  if (visible.empty()) {
    std::cout << "No plugins installed.\n";
  } else {
    std::cout << "Installed plugins (" << visible.size() << "):\n";
    for (const auto* entry : visible) {
      const auto& e = *entry;
      const std::string version =
          e.plugin_version.empty() ? "-" : e.plugin_version;
      const std::string license = e.license_spdx.empty() ? "-" : e.license_spdx;

      // The selector leads: it is the string every other subcommand takes, so
      // a line of this output is usable as input without editing.
      // static: the label lambda below reads kColumn with an empty capture
      // list, which MSVC rejects for an automatic constexpr (C3493).
      static constexpr size_t kColumn = 12;
      const auto label = [](const char* field) {
        return list_field_label(field, kColumn);
      };
      std::cout << label(plugin_ux::kFieldSelector) << e.selector << "\n";
      if (!e.plugin_id.empty() && e.plugin_id != e.selector) {
        std::cout << label(plugin_ux::kFieldId) << e.plugin_id << "\n";
      }
      std::cout << label(plugin_ux::kFieldPath) << e.path << "\n";
      std::cout << label(plugin_ux::kFieldVersion) << version << "\n";
      std::cout << label(plugin_ux::kFieldLicense) << license << "\n";
      std::cout << label(plugin_ux::kFieldSource) << e.source_label << "\n";
      std::cout << label(plugin_ux::kFieldEnabled) << (e.enabled ? "yes" : "no")
                << "\n";
      std::cout << label(plugin_ux::kFieldStatus)
                << orc::presenters::pluginLoadStateSummary(e.load_state,
                                                           e.load_state_detail)
                << "\n";
      if (check_updates) {
        const auto it = update_status.find(e.plugin_id);
        std::cout << label(plugin_ux::kFieldUpdate)
                  << (it == update_status.end()
                          ? std::string(plugin_ux::kUpdateStatusNone)
                          : update_status_label(it->second))
                  << "\n";
      }
      std::cout << label(plugin_ux::kFieldCore)
                << (e.is_core_plugin ? "yes" : "no") << "\n";
      std::cout << label(plugin_ux::kFieldExists)
                << (e.path_exists ? "yes" : "no") << "\n";
      std::cout << label(plugin_ux::kFieldLoaded)
                << (e.is_loaded ? "yes" : "no") << "\n";
      if (e.required_host_abi != 0) {
        std::cout << label(plugin_ux::kFieldHostAbi) << "requires "
                  << e.required_host_abi << " (host " << e.host_abi_version
                  << ")\n";
      }
      std::cout << "\n";
    }
  }

  if (hidden_core > 0) {
    std::cout << plugin_ux::kNotePrefix << hidden_core
              << " core plugin(s) hidden; pass --core to include them.\n\n";
  }

  size_t shown_loaded = 0;
  for (const auto& p : loaded) {
    if (p.is_core_plugin && !show_core) {
      continue;
    }
    if (shown_loaded == 0) {
      std::cout << "Loaded plugins this session:\n";
    }
    ++shown_loaded;
    // The id is the plugin's selector; a plugin loaded without one is still
    // addressable by its path, so never print an empty identifier here.
    std::cout << "  " << (p.plugin_id.empty() ? p.path : p.plugin_id) << " v"
              << p.plugin_version << " (" << p.registered_stage_names.size()
              << " stage(s))"
              << "\n";
  }

  return kExitSuccess;
}

int cmd_plugins_add(int argc, char* argv[]) {
  // argv[0] = "add", argv[1..] = <path> and/or options
  std::string path;
  std::string releases_url;
  std::string plugin_id;
  std::string plugin_version;
  std::string license_spdx;
  bool assume_yes = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--url" && i + 1 < argc) {
      releases_url = argv[++i];
    } else if (arg == "--id" && i + 1 < argc) {
      plugin_id = argv[++i];
    } else if (arg == "--version" && i + 1 < argc) {
      plugin_version = argv[++i];
    } else if (arg == "--license" && i + 1 < argc) {
      license_spdx = argv[++i];
    } else if (arg == "--yes" || arg == "--trusted") {
      // --trusted is the pre-harmonization spelling: adding a plugin has
      // always granted trust, so it now means "yes, I trust it".
      assume_yes = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else if (path.empty()) {
      path = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (path.empty() && releases_url.empty()) {
    std::cerr << "Error: 'add' requires either a local plugin path or "
                 "--url <releases-url>\n";
    std::cerr << "Usage: orc-cli plugins add <path> [--id ID] [--version VER] "
                 "[--license SPDX] [--yes]\n";
    std::cerr << "       orc-cli plugins add --url <releases-url> [--yes]\n";
    return kExitUsage;
  }
  if (!path.empty() && !releases_url.empty()) {
    std::cerr << "Error: 'add' takes either a local plugin path or "
                 "--url <releases-url>, not both\n";
    return kExitUsage;
  }
  if (!releases_url.empty() && (!plugin_id.empty() || !plugin_version.empty() ||
                                !license_spdx.empty())) {
    std::cerr << "Error: --id, --version and --license only apply when adding "
                 "a local plugin file; a remote plugin's metadata comes from "
                 "its release manifest\n";
    return kExitUsage;
  }

  // Adding a plugin means it will run as native code, so trust is confirmed
  // before anything is recorded — exactly as the GUI's Add Plugin... does.
  const auto decision = ask_for_trust(assume_yes);
  if (decision != TrustDecision::Granted) {
    return report_trust_refusal(decision);
  }

  const auto result =
      releases_url.empty()
          ? orc::presenters::ProjectPresenter::addPluginToRegistry(
                path, plugin_id, plugin_version, license_spdx, false,
                /*trusted=*/true)
          : orc::presenters::ProjectPresenter::addPluginFromReleasesUrl(
                releases_url, /*trusted=*/true);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return kExitUsage;
  }

  std::cout << "Plugin added to registry and trusted.\n";
  print_registry_change_note();
  return kExitSuccess;
}

int cmd_plugins_remove(int argc, char* argv[]) {
  std::string selector;
  bool dry_run = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dry-run") {
      dry_run = true;
    } else if (arg == "--yes") {
      // Removal never prompts (it only takes capability away), but the flag is
      // accepted so a script can pass it uniformly to every mutating command.
    } else if (selector.empty()) {
      selector = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (selector.empty()) {
    std::cerr << "Error: 'remove' requires a plugin selector\n";
    std::cerr << "Usage: orc-cli plugins remove [--dry-run] <selector>\n";
    return kExitUsage;
  }

  if (dry_run) {
    // Resolve only: the registry is not written, so a script can check a
    // selector round-trips before committing to the removal.
    orc::presenters::PluginSelectorResolution resolution;
    int exit_code = kExitNotFound;
    if (!resolve_selector_or_report(selector, &resolution, &exit_code)) {
      return exit_code;
    }
    constexpr size_t kColumn = 12;
    std::cout << "Would remove:\n";
    std::cout << list_field_label(plugin_ux::kFieldSelector, kColumn)
              << resolution.entry.selector << "\n";
    if (!resolution.entry.path.empty()) {
      std::cout << list_field_label(plugin_ux::kFieldPath, kColumn)
                << resolution.entry.path << "\n";
    }
    std::cout << "Nothing was written.\n";
    return kExitSuccess;
  }

  const auto result =
      orc::presenters::ProjectPresenter::removePluginRegistryEntry(selector);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return mutation_exit_code(result);
  }

  std::cout << "Plugin '" << selector << "' removed from registry.\n";
  print_registry_change_note();
  return kExitSuccess;
}

int cmd_plugins_set_enabled(int argc, char* argv[], bool enabled) {
  const std::string subcommand = enabled ? "enable" : "disable";
  std::string selector;
  bool assume_yes = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--yes") {
      assume_yes = true;
    } else if (selector.empty()) {
      selector = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (selector.empty()) {
    std::cerr << "Error: '" << subcommand << "' requires a plugin selector\n";
    std::cerr << "Usage: orc-cli plugins " << subcommand << " <selector>\n";
    return kExitUsage;
  }

  if (enabled) {
    orc::presenters::PluginSelectorResolution resolution;
    int exit_code = kExitNotFound;
    if (!resolve_selector_or_report(selector, &resolution, &exit_code)) {
      return exit_code;
    }

    // Enabling an entry that is not trusted yet means its binary is about to
    // be downloaded and run, so that is where the confirmation belongs — the
    // same rule the GUI applies to ticking Enabled.
    if (resolution.entry.load_state ==
        orc::presenters::PluginLoadState::NotTrusted) {
      const auto decision = ask_for_trust(assume_yes);
      if (decision != TrustDecision::Granted) {
        return report_trust_refusal(decision);
      }
      const int trust_code = record_trust(selector);
      if (trust_code != kExitSuccess) {
        return trust_code;
      }
    }
  }

  const auto result =
      orc::presenters::ProjectPresenter::setPluginRegistryEntryEnabled(selector,
                                                                       enabled);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return mutation_exit_code(result);
  }

  std::cout << "Plugin '" << selector << "' "
            << (enabled ? "enabled" : "disabled") << ".\n";
  print_registry_change_note();
  return kExitSuccess;
}

int cmd_plugins_set_trusted(int argc, char* argv[], bool trusted) {
  const std::string subcommand = trusted ? "trust" : "untrust";
  std::string selector;
  bool assume_yes = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--yes") {
      // Accepted by both so a script can pass it uniformly; only the granting
      // direction ever prompts.
      assume_yes = true;
    } else if (selector.empty()) {
      selector = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (selector.empty()) {
    std::cerr << "Error: '" << subcommand << "' requires a plugin selector\n";
    std::cerr << "Usage: orc-cli plugins " << subcommand << " <selector>\n";
    return kExitUsage;
  }

  if (trusted) {
    const auto decision = ask_for_trust(assume_yes);
    if (decision != TrustDecision::Granted) {
      return report_trust_refusal(decision);
    }
  }

  const auto result =
      orc::presenters::ProjectPresenter::setPluginRegistryEntryTrusted(selector,
                                                                       trusted);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return mutation_exit_code(result);
  }

  std::cout << "Plugin '" << selector << "' marked as "
            << (trusted ? orc::presenters::kPluginTrustStateTrusted
                        : orc::presenters::kPluginTrustStateUntrusted)
            << ".\n";
  print_registry_change_note();
  return kExitSuccess;
}

/// The one line describing how the index listing was sourced; the GUI browse
/// dialog shows the same sentence in its status banner.
std::string index_status_message(
    const orc::presenters::PluginIndexInfo& index) {
  return plugin_ux::indexStatusMessage(index.available, index.offline,
                                       index.from_cache, index.entries.size(),
                                       index.error_message);
}

/// Short annotation for one index entry, in the browse dialog's wording.
std::string index_entry_annotation(
    const orc::presenters::PluginIndexEntryInfo& entry) {
  if (entry.release_unreachable) {
    return plugin_ux::kIndexLabelUnreachable;
  }
  if (!entry.has_compatible_build) {
    return plugin_ux::kIndexLabelIncompatible;
  }
  if (entry.already_installed) {
    return plugin_ux::kIndexLabelInstalled;
  }
  return std::string();
}

int cmd_plugins_search(int argc, char* argv[]) {
  std::string term;
  bool only_installed = false;
  bool only_available = false;
  bool only_compatible = false;
  bool as_json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--installed") {
      only_installed = true;
    } else if (arg == "--available") {
      only_available = true;
    } else if (arg == "--compatible") {
      only_compatible = true;
    } else if (arg == "--json") {
      as_json = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else if (term.empty()) {
      term = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (only_installed && only_available) {
    std::cerr << "Error: --installed and --available select opposite sets; "
                 "pass at most one\n";
    return kExitUsage;
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  const auto index = orc::presenters::ProjectPresenter::readPluginIndex();
  if (!index.available) {
    std::cerr << "Error: " << index_status_message(index) << "\n";
    return kExitIndexUnavailable;
  }

  const std::string needle = [&term]() {
    std::string lower = term;
    for (char& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower;
  }();
  auto contains_ci = [&needle](const std::string& text) {
    std::string lower = text;
    for (char& ch : lower) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower.find(needle) != std::string::npos;
  };

  std::vector<const orc::presenters::PluginIndexEntryInfo*> matched;
  for (const auto& e : index.entries) {
    if (!needle.empty()) {
      bool hit = contains_ci(e.id) || contains_ci(e.display_name) ||
                 contains_ci(e.description);
      for (const auto& tag : e.tags) {
        hit = hit || contains_ci(tag);
      }
      if (!hit) {
        continue;
      }
    }
    if (only_installed && !e.already_installed) {
      continue;
    }
    if (only_available && e.already_installed) {
      continue;
    }
    if (only_compatible && !e.has_compatible_build) {
      continue;
    }
    matched.push_back(&e);
  }

  if (as_json) {
    // The index's own state travels with the entries: a script that acts on a
    // cached listing needs to know it was cached.
    JsonWriter json(std::cout);
    json.begin_object();
    json.member_int("schema_version", index.schema_version);
    json.member_bool("available", index.available);
    json.member_bool("from_cache", index.from_cache);
    json.member_bool("offline", index.offline);
    json.member_int("host_abi_version", index.host_abi_version);
    json.member("source_url", index.source_url);
    json.member("error_message", index.error_message);
    json.key("entries");
    json.begin_array();
    for (const auto* entry : matched) {
      write_index_entry_json(&json, *entry);
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  // With no term this is the browse dialog's opening state: the whole index.
  std::cout << index_status_message(index) << "\n\n";

  for (const auto* entry : matched) {
    const auto& e = *entry;
    const std::string annotation = index_entry_annotation(e);
    // The id leads and is printed verbatim: 'info' and 'install' take it, and
    // for an installed entry it is also its registry selector.
    std::cout << e.id << "  " << e.display_name
              << (annotation.empty() ? std::string() : "  (" + annotation + ")")
              << "\n";
    if (!e.description.empty()) {
      std::cout << "    " << e.description << "\n";
    }
    if (!e.compatibility_message.empty()) {
      std::cout << "    " << e.compatibility_message << "\n";
    }
  }

  if (matched.empty()) {
    if (term.empty()) {
      std::cout << "No plugins to show.\n";
    } else {
      std::cout << "No plugins matched '" << term << "'.\n";
    }
  }
  return kExitSuccess;
}

/// True for a selector that names an entry by path, asset URL or position —
/// forms the curated index never uses, so the index need not be consulted.
bool selector_is_registry_only(const std::string& selector) {
  return selector.rfind(orc::presenters::kPluginSelectorPathPrefix, 0) == 0 ||
         selector.rfind(orc::presenters::kPluginSelectorUrlPrefix, 0) == 0 ||
         selector.rfind(orc::presenters::kPluginSelectorIndexPrefix, 0) == 0;
}

int cmd_plugins_info(int argc, char* argv[]) {
  std::string selector;
  bool as_json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      as_json = true;
      continue;
    }
    if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    }
    if (selector.empty()) {
      selector = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (selector.empty()) {
    std::cerr << "Error: 'info' requires a plugin selector\n";
    std::cerr << "Usage: orc-cli plugins info <selector> [--json]\n";
    return kExitUsage;
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  // A plugin can be described by the curated index, by the local registry, or
  // by both; 'info' covers all three so every identifier either listing prints
  // is accepted here.
  orc::presenters::PluginIndexInfo index;
  const orc::presenters::PluginIndexEntryInfo* indexed = nullptr;
  bool index_consulted = false;
  if (!selector_is_registry_only(selector)) {
    index = orc::presenters::ProjectPresenter::readPluginIndex();
    index_consulted = index.available;
    for (const auto& e : index.entries) {
      if (e.id == selector) {
        indexed = &e;
        break;
      }
    }
  }

  const auto resolution =
      orc::presenters::ProjectPresenter::resolvePluginRegistrySelector(
          selector);
  // Ambiguity is an error even when the string also names an index entry:
  // describing only the index match would silently guess which of the local
  // entries the user meant.
  if (resolution.status == orc::presenters::PluginSelectorStatus::Ambiguous) {
    std::cerr << "Error: "
              << orc::presenters::describeAmbiguousPluginSelector(
                     selector, resolution.candidates)
              << "\n";
    return kExitNotFound;
  }

  const orc::presenters::PluginRegistryEntryInfo* installed =
      resolution.status == orc::presenters::PluginSelectorStatus::Resolved
          ? &resolution.entry
          : nullptr;

  // Core plugins ship with the application and are discovered rather than
  // registered, so the registry cannot resolve them — but `stages list` prints
  // their ids as the owning plugin, and every identifier a listing prints has
  // to be accepted here.
  orc::presenters::PluginRegistryEntryInfo bundled;
  if (installed == nullptr) {
    for (const auto& plugin :
         orc::presenters::ProjectPresenter::getLoadedPlugins()) {
      if (!plugin.plugin_id.empty() && plugin.plugin_id == selector) {
        bundled = orc::presenters::makeEntryForLoadedPlugin(plugin);
        installed = &bundled;
        break;
      }
    }
  }

  if (indexed == nullptr && installed == nullptr) {
    // Name both places that were searched, so the user knows the id was not
    // simply misspelled for one of them.
    std::cerr << "Error: no plugin matching '" << selector
              << "' was found in the installed registry or in the available "
                 "plugin index";
    if (!index_consulted) {
      std::cerr << " (" << index_status_message(index) << ")";
    }
    std::cerr << "\n";
    return index_consulted ? kExitNotFound : kExitIndexUnavailable;
  }

  if (as_json) {
    // Which of the two descriptions exist is itself information, so both keys
    // are always present and hold null when the plugin is not known there.
    JsonWriter json(std::cout);
    json.begin_object();
    json.member("selector",
                installed != nullptr ? installed->selector : indexed->id);
    json.key("installed");
    if (installed == nullptr) {
      json.value_null();
    } else {
      write_registry_entry_json(&json, *installed, nullptr);
    }
    json.key("indexed");
    if (indexed == nullptr) {
      json.value_null();
    } else {
      write_index_entry_json(&json, *indexed);
    }
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  // Details are not a listing, so the banner appears only when it changes how
  // to read them: a cached index may describe an older release.
  if (indexed != nullptr && index.offline) {
    std::cout << index_status_message(index) << "\n\n";
  }

  print_detail_fields(
      orc::presenters::makePluginDetails(installed, indexed, nullptr));
  return kExitSuccess;
}

int cmd_plugins_doctor(int argc, char* argv[]) {
  bool as_json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      as_json = true;
      continue;
    }
    std::cerr << "Error: Unexpected argument: " << arg << "\n";
    std::cerr << "Usage: orc-cli plugins doctor [--json]\n";
    return kExitUsage;
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  const auto registry = orc::presenters::ProjectPresenter::readPluginRegistry();
  const auto search_paths =
      orc::presenters::ProjectPresenter::getPluginSearchPaths();
  const auto diagnostics =
      orc::presenters::ProjectPresenter::getPluginDiagnostics();

  if (as_json) {
    JsonWriter json(std::cout);
    json.begin_object();
    json.member("registry_path", registry.registry_path);
    json.member_strings("search_paths", search_paths);
    json.key("diagnostics");
    json.begin_array();
    for (const auto& diagnostic : diagnostics) {
      write_diagnostic_json(&json, diagnostic);
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  std::cout << "Registry path: "
            << (registry.registry_path.empty() ? "<none>"
                                               : registry.registry_path)
            << "\n\n";

  std::cout << plugin_ux::kSearchPathsTitle << " (" << search_paths.size()
            << "):\n";
  for (const auto& path : search_paths) {
    std::cout << "  " << path << "\n";
  }
  std::cout << "\n";

  // The same lines the Plugin Manager's Diagnostics section shows.
  if (diagnostics.empty()) {
    std::cout << plugin_ux::kDiagnosticsNone << "\n";
    return kExitSuccess;
  }

  std::cout << plugin_ux::kDiagnosticsTitle << " (" << diagnostics.size()
            << "):\n";
  for (const auto& diagnostic : diagnostics) {
    std::cout << "  " << orc::presenters::formatPluginDiagnostic(diagnostic)
              << "\n";
  }
  // A reported problem is the answer to the question asked, not a failure of
  // the command, so 'doctor' succeeds whatever it finds.
  return kExitSuccess;
}

int cmd_plugins_install(int argc, char* argv[]) {
  std::string plugin_id;
  bool assume_yes = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--yes") {
      assume_yes = true;
    } else if (plugin_id.empty()) {
      plugin_id = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (plugin_id.empty()) {
    std::cerr << "Error: 'install' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins install <id> [--yes]\n";
    return kExitUsage;
  }

  // Resolve the id before asking anything, so a misspelled id reports
  // not-found rather than a trust refusal — the same order 'enable' uses.
  {
    const auto index = orc::presenters::ProjectPresenter::readPluginIndex();
    if (!index.available) {
      std::cerr << "Error: " << index_status_message(index) << "\n";
      return kExitIndexUnavailable;
    }
    const bool listed = std::any_of(
        index.entries.begin(), index.entries.end(),
        [&plugin_id](const orc::presenters::PluginIndexEntryInfo& entry) {
          return entry.id == plugin_id;
        });
    if (!listed) {
      std::cerr << "Error: No plugin with id '" << plugin_id
                << "' is listed in the index\n";
      return kExitNotFound;
    }
  }

  // Installing downloads and runs a binary, so confirm before recording
  // anything — the browse dialog's Install... asks the same question.
  const auto decision = ask_for_trust(assume_yes);
  if (decision != TrustDecision::Granted) {
    const int code = report_trust_refusal(decision);
    std::cerr << "Nothing was installed. To install and trust it in one step:\n"
              << "  orc-cli plugins install " << plugin_id << " --yes\n";
    return code;
  }

  const auto result =
      orc::presenters::ProjectPresenter::installIndexedPlugin(plugin_id);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return mutation_exit_code(result);
  }

  const int trust_code = record_trust(plugin_id);
  if (trust_code != kExitSuccess) {
    return trust_code;
  }

  std::cout << "Plugin '" << plugin_id << "' installed and trusted.\n";
  print_registry_change_note();
  return kExitSuccess;
}

int cmd_plugins_updates(int argc, char* argv[]) {
  bool as_json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      as_json = true;
      continue;
    }
    std::cerr << "Error: Unexpected argument: " << arg << "\n";
    std::cerr << "Usage: orc-cli plugins updates [--json]\n";
    return kExitUsage;
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  const auto statuses =
      orc::presenters::ProjectPresenter::checkRegisteredPluginUpdates();

  if (as_json) {
    JsonWriter json(std::cout);
    json.begin_array();
    for (const auto& s : statuses) {
      write_update_status_json(&json, s);
    }
    json.end_array();
    json.finish();
    return kExitSuccess;
  }

  if (statuses.empty()) {
    std::cout << "No registered plugins to check for updates.\n";
    return kExitSuccess;
  }

  bool any_update = false;
  for (const auto& s : statuses) {
    // The plugin id is the entry's selector, so this output feeds 'update'.
    constexpr size_t kColumn = 13;
    std::cout << list_field_label(plugin_ux::kFieldSelector, kColumn)
              << s.plugin_id << "\n";
    std::cout << list_field_label(plugin_ux::kFieldInstalled, kColumn)
              << (s.installed_version.empty() ? "-" : s.installed_version)
              << "\n";
    std::cout << list_field_label(plugin_ux::kFieldUpdate, kColumn)
              << update_status_label(s) << "\n";
    any_update =
        any_update ||
        s.status == orc::presenters::PluginUpdateStatus::UpdateAvailable;
    std::cout << "\n";
  }

  if (any_update) {
    std::cout << "Update a plugin with: orc-cli plugins update <selector>\n";
    std::cout << "Update every plugin with: orc-cli plugins update --all\n";
  }
  return kExitSuccess;
}

/// Update one already-resolved entry and record trust for the new binary.
/// Returns kExitSuccess, or the contract code describing the failure.
int update_one(const std::string& selector) {
  const auto result =
      orc::presenters::ProjectPresenter::updateRegisteredPluginToLatestRelease(
          selector);
  if (!result.success) {
    std::cerr << "Error: " << selector << ": " << result.error_message << "\n";
    return mutation_exit_code(result);
  }
  if (!result.error_message.empty()) {
    std::cout << "Warning: " << result.error_message << "\n";
  }
  // The rewritten entry points at a fresh binary, which resets trust; the
  // confirmation already given covers it.
  const int trust_code = record_trust(selector);
  if (trust_code != kExitSuccess) {
    return trust_code;
  }
  std::cout << "Plugin '" << selector
            << "' updated to its latest release and trusted.\n";
  return kExitSuccess;
}

int cmd_plugins_update_all(bool assume_yes) {
  const auto statuses =
      orc::presenters::ProjectPresenter::checkRegisteredPluginUpdates();

  std::vector<const orc::presenters::PluginUpdateStatusInfo*> outdated;
  for (const auto& s : statuses) {
    if (s.status == orc::presenters::PluginUpdateStatus::UpdateAvailable) {
      outdated.push_back(&s);
    }
  }

  if (outdated.empty()) {
    std::cout << "All registered plugins are up to date.\n";
    return kExitSuccess;
  }

  std::cout << "Plugins to update (" << outdated.size() << "):\n";
  for (const auto* s : outdated) {
    std::cout << "  " << s->plugin_id << "  "
              << (s->installed_version.empty() ? "-" : s->installed_version)
              << " -> " << s->latest_version << "\n";
  }
  std::cout << "\n";

  // One confirmation covers the batch: every binary about to be downloaded is
  // listed above.
  const auto decision = ask_for_trust(assume_yes);
  if (decision != TrustDecision::Granted) {
    return report_trust_refusal(decision);
  }

  size_t failures = 0;
  int first_failure_code = kExitUsage;
  for (const auto* s : outdated) {
    const int code = update_one(s->plugin_id);
    if (code != kExitSuccess) {
      if (failures == 0) {
        first_failure_code = code;
      }
      ++failures;
    }
  }

  print_registry_change_note();
  if (failures > 0) {
    std::cerr << "Error: " << failures << " of " << outdated.size()
              << " plugin(s) could not be updated.\n";
    return first_failure_code;
  }
  return kExitSuccess;
}

int cmd_plugins_update(int argc, char* argv[]) {
  std::string selector;
  bool update_all = false;
  bool assume_yes = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--all") {
      update_all = true;
    } else if (arg == "--yes") {
      assume_yes = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else if (selector.empty()) {
      selector = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (update_all && !selector.empty()) {
    std::cerr << "Error: 'update' takes either a selector or --all, not both\n";
    return kExitUsage;
  }
  if (!update_all && selector.empty()) {
    std::cerr << "Error: 'update' requires a plugin selector, or --all\n";
    std::cerr << "Usage: orc-cli plugins update <selector> [--yes]\n";
    std::cerr << "       orc-cli plugins update --all [--yes]\n";
    return kExitUsage;
  }

  if (update_all) {
    return cmd_plugins_update_all(assume_yes);
  }

  // Resolve before asking anything, so a misspelled selector reports
  // not-found rather than a trust refusal — the same order 'enable' uses.
  {
    orc::presenters::PluginSelectorResolution resolution;
    int exit_code = kExitNotFound;
    if (!resolve_selector_or_report(selector, &resolution, &exit_code)) {
      return exit_code;
    }
  }

  // The update replaces the binary, so it is confirmed like a fresh install.
  const auto decision = ask_for_trust(assume_yes);
  if (decision != TrustDecision::Granted) {
    return report_trust_refusal(decision);
  }

  const int code = update_one(selector);
  if (code != kExitSuccess) {
    return code;
  }
  print_registry_change_note();
  return kExitSuccess;
}

}  // namespace

int plugins_command(int argc, char* argv[]) {
  // argv[0] = "plugins"
  if (argc < 2) {
    print_plugins_usage(argv[0]);
    return kExitUsage;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_plugins_usage(argv[0]);
    return kExitSuccess;
  }

  if (subcommand == "list") {
    return cmd_plugins_list(argc - 1, argv + 1);
  }

  if (subcommand == "add") {
    // Pass remaining args starting from "add"
    return cmd_plugins_add(argc - 1, argv + 1);
  }

  if (subcommand == "remove") {
    return cmd_plugins_remove(argc - 1, argv + 1);
  }

  if (subcommand == "enable") {
    return cmd_plugins_set_enabled(argc - 1, argv + 1, true);
  }

  if (subcommand == "disable") {
    return cmd_plugins_set_enabled(argc - 1, argv + 1, false);
  }

  if (subcommand == "trust") {
    return cmd_plugins_set_trusted(argc - 1, argv + 1, true);
  }

  if (subcommand == "untrust") {
    return cmd_plugins_set_trusted(argc - 1, argv + 1, false);
  }

  if (subcommand == "search") {
    return cmd_plugins_search(argc - 1, argv + 1);
  }

  if (subcommand == "info") {
    return cmd_plugins_info(argc - 1, argv + 1);
  }

  if (subcommand == "doctor") {
    return cmd_plugins_doctor(argc - 1, argv + 1);
  }

  if (subcommand == "install") {
    return cmd_plugins_install(argc - 1, argv + 1);
  }

  if (subcommand == "updates") {
    return cmd_plugins_updates(argc - 1, argv + 1);
  }

  if (subcommand == "update") {
    return cmd_plugins_update(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown plugins subcommand: " << subcommand << "\n\n";
  print_plugins_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
