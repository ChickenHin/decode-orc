/*
 * File:        command_plugins.cpp
 * Module:      orc-cli
 * Purpose:     Plugin registry management subcommand
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "command_plugins.h"

#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include "project_presenter.h"

namespace orc {
namespace cli {

namespace {

void print_plugins_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " plugins <subcommand> [options]\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  list                           Show registry entries and "
               "loaded plugins\n";
  std::cerr << "  add <path> [options]           Add a plugin to the "
               "persistent registry\n";
  std::cerr << "  remove <id>                    Remove a plugin from the "
               "persistent registry\n";
  std::cerr << "  enable <id>                    Enable a registered plugin\n";
  std::cerr << "  disable <id>                   Disable a registered plugin\n";
  std::cerr << "  trust <id>                     Mark a registered plugin as "
               "trusted (allows download and loading)\n";
  std::cerr << "  untrust <id>                   Mark a registered plugin as "
               "untrusted (blocks download and loading)\n";
  std::cerr << "  search <term>                  Search the curated plugin "
               "index\n";
  std::cerr << "  info <id>                      Show details for an indexed "
               "plugin\n";
  std::cerr << "  install <id>                   Install an indexed plugin's "
               "latest release (untrusted until confirmed)\n";
  std::cerr << "  updates                        Check registered plugins for "
               "newer upstream releases\n";
  std::cerr << "  update <id>                    Update a registered plugin to "
               "its latest release (untrusted until confirmed)\n";
  std::cerr << "\n";
  std::cerr << "Options for 'add':\n";
  std::cerr << "  --id ID                        Plugin identifier (e.g. "
               "com.example.myplugin)\n";
  std::cerr << "  --version VER                  Plugin version string\n";
  std::cerr << "  --license SPDX                 License identifier (e.g. MIT, "
               "GPL-3.0-or-later)\n";
  std::cerr << "  --trusted                      Mark plugin as trusted\n";
  std::cerr << "\n";
  std::cerr
      << "Note: Registry changes take effect on the next application launch.\n";
}

int cmd_plugins_list() {
  const auto registry = orc::presenters::ProjectPresenter::readPluginRegistry();
  const auto loaded = orc::presenters::ProjectPresenter::getLoadedPlugins();

  std::cout << "Registry path: "
            << (registry.registry_path.empty() ? "<none>"
                                               : registry.registry_path)
            << "\n\n";

  if (registry.entries.empty()) {
    std::cout << "No plugins registered.\n";
  } else {
    std::cout << "Registered plugins (" << registry.entries.size() << "):\n";
    for (const auto& e : registry.entries) {
      const std::string id = e.plugin_id.empty() ? "<unnamed>" : e.plugin_id;
      const std::string version =
          e.plugin_version.empty() ? "-" : e.plugin_version;
      const std::string license = e.license_spdx.empty() ? "-" : e.license_spdx;

      std::cout << "  id:       " << id << "\n";
      std::cout << "  path:     " << e.path << "\n";
      std::cout << "  version:  " << version << "\n";
      std::cout << "  license:  " << license << "\n";
      std::cout << "  enabled:  " << (e.enabled ? "yes" : "no") << "\n";
      std::cout << "  trusted:  "
                << ((e.is_core_plugin || e.trust_state == "trusted") ? "yes"
                                                                     : "no")
                << "\n";
      std::cout << "  core:     " << (e.is_core_plugin ? "yes" : "no") << "\n";
      std::cout << "  exists:   " << (e.path_exists ? "yes" : "no") << "\n";
      std::cout << "  loaded:   " << (e.is_loaded ? "yes" : "no") << "\n";
      if (e.required_host_abi != 0) {
        std::cout << "  host ABI: requires " << e.required_host_abi << " (host "
                  << e.host_abi_version << ")";
        if (!e.abi_compatible) {
          std::cout << " — needs rebuild for ABI " << e.host_abi_version;
        }
        std::cout << "\n";
      }
      std::cout << "\n";
    }
  }

  if (!loaded.empty()) {
    std::cout << "Loaded plugins this session (" << loaded.size() << "):\n";
    for (const auto& p : loaded) {
      std::cout << "  " << p.plugin_id << " v" << p.plugin_version << " ("
                << p.registered_stage_names.size() << " stage(s))"
                << "\n";
    }
  }

  return 0;
}

int cmd_plugins_add(int argc, char* argv[]) {
  // argv[0] = "add", argv[1] = <path>, remaining = options
  if (argc < 2) {
    std::cerr << "Error: 'add' requires a plugin path\n";
    std::cerr << "Usage: orc-cli plugins add <path> [--id ID] [--version VER] "
                 "[--license SPDX] [--trusted]\n";
    return 1;
  }

  std::string path = argv[1];
  std::string plugin_id;
  std::string plugin_version;
  std::string license_spdx;
  bool trusted = false;

  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--id" && i + 1 < argc) {
      plugin_id = argv[++i];
    } else if (arg == "--version" && i + 1 < argc) {
      plugin_version = argv[++i];
    } else if (arg == "--license" && i + 1 < argc) {
      license_spdx = argv[++i];
    } else if (arg == "--trusted") {
      trusted = true;
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return 1;
    }
  }

  const auto result = orc::presenters::ProjectPresenter::addPluginToRegistry(
      path, plugin_id, plugin_version, license_spdx, false, trusted);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  std::cout << "Plugin added to registry.\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

int cmd_plugins_remove(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Error: 'remove' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins remove <id>\n";
    return 1;
  }

  const std::string plugin_id = argv[1];
  const auto result =
      orc::presenters::ProjectPresenter::removePluginFromRegistry(plugin_id);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  std::cout << "Plugin '" << plugin_id << "' removed from registry.\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

int cmd_plugins_set_enabled(int argc, char* argv[], bool enabled) {
  const std::string subcommand = enabled ? "enable" : "disable";
  if (argc < 2) {
    std::cerr << "Error: '" << subcommand << "' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins " << subcommand << " <id>\n";
    return 1;
  }

  const std::string plugin_id = argv[1];
  const auto result =
      orc::presenters::ProjectPresenter::setPluginRegistryEntryEnabled(
          plugin_id, enabled);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  std::cout << "Plugin '" << plugin_id << "' "
            << (enabled ? "enabled" : "disabled") << ".\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

int cmd_plugins_set_trusted(int argc, char* argv[], bool trusted) {
  const std::string subcommand = trusted ? "trust" : "untrust";
  if (argc < 2) {
    std::cerr << "Error: '" << subcommand << "' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins " << subcommand << " <id>\n";
    return 1;
  }

  const std::string plugin_id = argv[1];
  const auto result =
      orc::presenters::ProjectPresenter::setPluginRegistryEntryTrusted(
          plugin_id, trusted);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  std::cout << "Plugin '" << plugin_id << "' marked as "
            << (trusted ? "trusted" : "untrusted") << ".\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

// Print a one-line note about how the index was sourced (live vs cache) so the
// user understands staleness when offline.
void print_index_status(const orc::presenters::PluginIndexInfo& index) {
  if (index.offline && index.from_cache) {
    std::cout << "(offline — showing the last cached index)\n\n";
  } else if (index.offline) {
    std::cout << "(offline — no cached index available)\n\n";
  }
}

int cmd_plugins_search(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Error: 'search' requires a search term\n";
    std::cerr << "Usage: orc-cli plugins search <term>\n";
    return 1;
  }
  const std::string term = argv[1];
  const auto index = orc::presenters::ProjectPresenter::readPluginIndex();
  if (!index.available) {
    std::cerr << "Error: "
              << (index.error_message.empty()
                      ? "the plugin index could not be loaded"
                      : index.error_message)
              << "\n";
    return 1;
  }

  print_index_status(index);

  size_t matches = 0;
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

  for (const auto& e : index.entries) {
    bool hit = contains_ci(e.id) || contains_ci(e.display_name) ||
               contains_ci(e.description);
    for (const auto& tag : e.tags) {
      hit = hit || contains_ci(tag);
    }
    if (!hit) {
      continue;
    }
    ++matches;
    std::cout << e.id << "  " << e.display_name << "\n";
    if (!e.description.empty()) {
      std::cout << "    " << e.description << "\n";
    }
    std::cout << "    "
              << (e.has_compatible_build
                      ? "compatible with this host"
                      : (e.compatibility_message.empty()
                             ? "no compatible build for this host"
                             : e.compatibility_message))
              << (e.already_installed ? "; already installed" : "") << "\n";
  }

  if (matches == 0) {
    std::cout << "No plugins matched '" << term << "'.\n";
  }
  return 0;
}

int cmd_plugins_info(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Error: 'info' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins info <id>\n";
    return 1;
  }
  const std::string plugin_id = argv[1];
  const auto index = orc::presenters::ProjectPresenter::readPluginIndex();
  if (!index.available) {
    std::cerr << "Error: "
              << (index.error_message.empty()
                      ? "the plugin index could not be loaded"
                      : index.error_message)
              << "\n";
    return 1;
  }

  const orc::presenters::PluginIndexEntryInfo* entry = nullptr;
  for (const auto& e : index.entries) {
    if (e.id == plugin_id) {
      entry = &e;
      break;
    }
  }
  if (entry == nullptr) {
    std::cerr << "Error: no plugin with id '" << plugin_id
              << "' is listed in the index\n";
    return 1;
  }

  print_index_status(index);

  std::cout << "id:          " << entry->id << "\n";
  std::cout << "name:        " << entry->display_name << "\n";
  if (!entry->description.empty()) {
    std::cout << "description: " << entry->description << "\n";
  }
  if (!entry->maintainer.empty()) {
    std::cout << "maintainer:  " << entry->maintainer << "\n";
  }
  if (!entry->license_spdx.empty()) {
    std::cout << "license:     " << entry->license_spdx << "\n";
  }
  if (!entry->source_repo_url.empty()) {
    std::cout << "source:      " << entry->source_repo_url << "\n";
  }
  if (!entry->tags.empty()) {
    std::cout << "tags:        ";
    for (size_t i = 0; i < entry->tags.size(); ++i) {
      std::cout << (i == 0 ? "" : ", ") << entry->tags[i];
    }
    std::cout << "\n";
  }
  if (entry->release_unreachable) {
    std::cout << "latest:      unreachable"
              << (entry->compatibility_message.empty()
                      ? ""
                      : " — " + entry->compatibility_message)
              << "\n";
  } else if (!entry->version.empty()) {
    std::cout << "latest:      " << entry->version << "\n";
  }
  std::cout << "compatible:  "
            << (entry->has_compatible_build
                    ? "yes"
                    : (entry->compatibility_message.empty()
                           ? "no"
                           : "no — " + entry->compatibility_message))
            << "\n";
  std::cout << "installed:   " << (entry->already_installed ? "yes" : "no")
            << "\n";
  return 0;
}

int cmd_plugins_install(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Error: 'install' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins install <id>\n";
    return 1;
  }
  const std::string plugin_id = argv[1];
  const auto result =
      orc::presenters::ProjectPresenter::installIndexedPlugin(plugin_id);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  std::cout << "Plugin '" << plugin_id
            << "' added to the registry (untrusted).\n";
  std::cout << "Trust it before it will be downloaded and loaded:\n";
  std::cout << "  orc-cli plugins trust " << plugin_id << "\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

int cmd_plugins_updates() {
  const auto statuses =
      orc::presenters::ProjectPresenter::checkRegisteredPluginUpdates();

  if (statuses.empty()) {
    std::cout << "No registered plugins to check for updates.\n";
    return 0;
  }

  bool any_update = false;
  for (const auto& s : statuses) {
    std::cout << "  id:        " << s.plugin_id << "\n";
    std::cout << "  installed: "
              << (s.installed_version.empty() ? "-" : s.installed_version)
              << "\n";
    switch (s.status) {
      case orc::presenters::PluginUpdateStatus::UpToDate:
        std::cout << "  status:    up to date (latest " << s.latest_tag
                  << ")\n";
        break;
      case orc::presenters::PluginUpdateStatus::UpdateAvailable:
        any_update = true;
        std::cout << "  status:    update available (" << s.latest_version
                  << ")\n";
        break;
      case orc::presenters::PluginUpdateStatus::Unreachable:
        std::cout << "  status:    unreachable (" << s.message << ")\n";
        break;
      case orc::presenters::PluginUpdateStatus::Unknown:
        std::cout << "  status:    latest is " << s.latest_version
                  << " (installed version unknown)\n";
        break;
      case orc::presenters::PluginUpdateStatus::NotApplicable:
        std::cout << "  status:    no source repository to check\n";
        break;
    }
    std::cout << "\n";
  }

  if (any_update) {
    std::cout << "Update a plugin with: orc-cli plugins update <id>\n";
  }
  return 0;
}

int cmd_plugins_update(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Error: 'update' requires a plugin id\n";
    std::cerr << "Usage: orc-cli plugins update <id>\n";
    return 1;
  }
  const std::string plugin_id = argv[1];
  const auto result =
      orc::presenters::ProjectPresenter::updateRegisteredPluginToLatestRelease(
          plugin_id);

  if (!result.success) {
    std::cerr << "Error: " << result.error_message << "\n";
    return 1;
  }

  if (!result.error_message.empty()) {
    std::cout << "Warning: " << result.error_message << "\n";
  }
  std::cout << "Plugin '" << plugin_id
            << "' updated to its latest release (untrusted).\n";
  std::cout << "Trust it before it will be downloaded and loaded:\n";
  std::cout << "  orc-cli plugins trust " << plugin_id << "\n";
  std::cout << "Note: Changes take effect on the next application launch.\n";
  return 0;
}

}  // namespace

int plugins_command(int argc, char* argv[]) {
  // argv[0] = "plugins"
  if (argc < 2) {
    print_plugins_usage(argv[0]);
    return 1;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_plugins_usage(argv[0]);
    return 0;
  }

  if (subcommand == "list") {
    return cmd_plugins_list();
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

  if (subcommand == "install") {
    return cmd_plugins_install(argc - 1, argv + 1);
  }

  if (subcommand == "updates") {
    return cmd_plugins_updates();
  }

  if (subcommand == "update") {
    return cmd_plugins_update(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown plugins subcommand: " << subcommand << "\n\n";
  print_plugins_usage(argv[0]);
  return 1;
}

}  // namespace cli
}  // namespace orc
