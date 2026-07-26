/*
 * File:        orc_cli.cpp
 * Module:      orc-cli
 * Purpose:     CLI application with subcommands
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/error_types.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "command_filter.h"
#include "command_plugins.h"
#include "command_process.h"
#include "crash_handler.h"
#include "logging.h"
#include "project_presenter.h"
#include "version.h"

namespace fs = std::filesystem;

using namespace orc;  // NOLINT(google-build-using-namespace)

/**
 * @brief Print command-line usage information
 *
 * Displays help text showing available commands, options, and examples
 * for the orc-cli command-line tool.
 *
 * @param program_name Name of the executable (argv[0])
 */
void print_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " <project-file> [options]\n";
  std::cerr << "       " << program_name
            << " --filter <graph> | --input/--filters/--output <...>\n";
  std::cerr << "       " << program_name << " plugins <subcommand> [options]\n";
  std::cerr << "\n";
  std::cerr << "Commands:\n";
  std::cerr << "  --process                      Process the whole DAG chain "
               "(trigger all sinks)\n";
  std::cerr << "  --filter GRAPH                 Build and process a DAG from "
               "an ffmpeg-style\n";
  std::cerr << "                                 filtergraph string (no "
               ".orcprj file)\n";
  std::cerr << "  --filter-file FILE             Read the filtergraph from "
               "FILE ('-' for stdin)\n";
  std::cerr << "  --input GRAPH                  Input (source) stage(s), for "
               "the input/filters/\n";
  std::cerr << "                                 output triad — an "
               "alternative to --filter\n";
  std::cerr << "  --filters GRAPH                Processing stage(s), for the "
               "triad\n";
  std::cerr << "  --output GRAPH                 Output (sink) stage(s), for "
               "the triad\n";
  std::cerr << "\n";
  std::cerr << "Note: video format and source signal type are detected "
               "automatically\n";
  std::cerr << "from the stage modules used, and never need to be specified.\n";
  std::cerr << "\n";
  std::cerr << "Plugin Management:\n";
  std::cerr << "  plugins list                   List registry entries and "
               "loaded plugins\n";
  std::cerr << "  plugins add <path> [options]   Add a plugin to the "
               "persistent registry\n";
  std::cerr << "  plugins remove <id>            Remove a plugin from the "
               "persistent registry\n";
  std::cerr << "  plugins enable <id>            Enable a registered plugin\n";
  std::cerr << "  plugins disable <id>           Disable a registered plugin\n";
  std::cerr << "  plugins search <term>          Search the curated plugin "
               "index\n";
  std::cerr << "  plugins info <id>              Show details for an indexed "
               "plugin\n";
  std::cerr << "  plugins install <id>           Install a plugin from the "
               "curated index\n";
  std::cerr << "\n";
  std::cerr << "Options:\n";
  std::cerr << "  --log-level LEVEL              Set logging verbosity\n";
  std::cerr << "                                 (trace, debug, info, warn, "
               "error, critical, off)\n";
  std::cerr << "                                 Default: info\n";
  std::cerr
      << "  --log-file FILE                Write logs to specified file\n";
  std::cerr << "  --safe-core-plugins            Clear plugin registry and "
               "ignore ORC_STAGE_PLUGIN_PATHS\n";
  std::cerr
      << "                                 for this run (core plugins only)\n";
  std::cerr << "\n";
  std::cerr << "Examples:\n";
  std::cerr << "  " << program_name << " project.orcprj --process\n";
  std::cerr << "  " << program_name
            << " project.orcprj --process --log-level debug\n";
  std::cerr << "  " << program_name
            << " --filter \"tbc_source=input_path=a.tbc, video_sink\"\n";
  std::cerr << "  " << program_name
            << " --input tbc_source=input_path=a.tbc \\\n";
  std::cerr << "      --filters hvd_chroma_decoder --output "
               "video_sink=output_path=a.mp4\n";
  std::cerr << "  " << program_name << " plugins list\n";
  std::cerr << "  " << program_name
            << " plugins add /path/to/libmyplugin.so --id com.example.my "
               "--license MIT\n";
}

/**
 * @brief Main entry point for orc-cli
 *
 * Parses command-line arguments and dispatches to the appropriate command
 * handler. Supports processing projects, analyzing field mappings, and
 * analyzing source alignments.
 *
 * @param argc Argument count
 * @param argv Argument values
 * @return Exit code (0 = success, non-zero = error)
 */
int main(int argc, char* argv[]) {
  try {
    // Parse command line arguments
    std::string project_path;
    std::string log_level = "info";
    std::string log_file;
    bool safe_core_plugins = false;

    // Filtergraph mode: either a full graph string, or the input/filters/
    // output triad. Mutually exclusive with each other and with a project
    // file. There is deliberately no video-format/source-format/project-name
    // option here — those are auto-detected from the stages used.
    std::string filtergraph;
    std::string filtergraph_file;
    std::string input_stages;
    std::string filters_stages;
    std::string output_stages;
    bool graph_string_provided = false;  // --filter / --filter-file
    bool triad_provided = false;         // --input / --filters / --output

    // Command flags
    bool do_process = false;

    // Check for help or empty args
    if (argc < 2) {
      std::cerr << "Error: No project file or command specified\n\n";
      print_usage(argv[0]);
      return 1;
    }

    std::string first_arg = argv[1];
    if (first_arg == "--help" || first_arg == "-h") {
      print_usage(argv[0]);
      return 0;
    }

    for (int i = 1; i < argc; ++i) {
      if (std::string(argv[i]) == "--safe-core-plugins") {
        safe_core_plugins = true;
        break;
      }
    }

    auto apply_safe_mode = []() -> bool {
#if defined(_WIN32)
      _putenv_s("ORC_STAGE_PLUGIN_PATHS", "");
#else
      unsetenv("ORC_STAGE_PLUGIN_PATHS");
#endif

      const auto clear_result =
          orc::presenters::ProjectPresenter::clearPluginRegistryForSafeMode();
      if (!clear_result.success) {
        std::cerr << "Error: Failed to clear plugin registry for safe startup: "
                  << clear_result.error_message << "\n";
        return false;
      }

      return true;
    };

    if (safe_core_plugins && !apply_safe_mode()) {
      return 1;
    }

    // Route 'plugins' subcommand — does not require a project file
    if (first_arg == "plugins") {
      // Rewrite argv so that argv[0] is the program name for usage messages,
      // and forward all remaining args to the plugins command handler.
      // plugins_command expects argv[0] = program name, argv[1] = subcommand.
      // Skip argv[1] ("plugins") since plugins_command handles its own
      // subcommand routing.
      std::vector<char*> plugins_argv;
      plugins_argv.push_back(argv[0]);
      for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--safe-core-plugins") {
          continue;
        }
        plugins_argv.push_back(argv[i]);
      }
      return cli::plugins_command(static_cast<int>(plugins_argv.size()),
                                  plugins_argv.data());
    }

    // Parse all arguments
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      if (arg == "--help" || arg == "-h") {
        print_usage(argv[0]);
        return 0;
      } else if (arg == "--log-level" && i + 1 < argc) {
        log_level = argv[++i];
      } else if (arg == "--log-file" && i + 1 < argc) {
        log_file = argv[++i];
      } else if (arg == "--safe-core-plugins") {
        // Handled before dispatch.
      } else if (arg == "--process") {
        do_process = true;
      } else if (arg == "--filter" && i + 1 < argc) {
        filtergraph = argv[++i];
        graph_string_provided = true;
      } else if (arg == "--filter-file" && i + 1 < argc) {
        filtergraph_file = argv[++i];
        graph_string_provided = true;
      } else if (arg == "--input" && i + 1 < argc) {
        input_stages = argv[++i];
        triad_provided = true;
      } else if (arg == "--filters" && i + 1 < argc) {
        filters_stages = argv[++i];
        triad_provided = true;
      } else if (arg == "--output" && i + 1 < argc) {
        output_stages = argv[++i];
        triad_provided = true;
      } else if (arg[0] != '-') {
        // Positional argument - project file
        if (project_path.empty()) {
          project_path = arg;
        } else {
          std::cerr << "Error: Multiple project files specified\n";
          print_usage(argv[0]);
          return 1;
        }
      } else {
        std::cerr << "Error: Unknown option: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
      }
    }

    const bool filtergraph_mode = graph_string_provided || triad_provided;

    if (filtergraph_mode) {
      if (graph_string_provided && triad_provided) {
        std::cerr << "Error: --filter/--filter-file cannot be combined with "
                     "--input/--filters/--output\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (!project_path.empty()) {
        std::cerr << "Error: --filter/--input/--filters/--output cannot be "
                     "combined with a project file\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (do_process) {
        std::cerr << "Error: --process is only for a project file and "
                     "cannot be combined with --filter/--input/--filters/"
                     "--output\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (graph_string_provided && !filtergraph.empty() &&
          !filtergraph_file.empty()) {
        std::cerr
            << "Error: use either --filter or --filter-file, not both\n\n";
        print_usage(argv[0]);
        return 1;
      }
      if (triad_provided && input_stages.empty() && filters_stages.empty() &&
          output_stages.empty()) {
        std::cerr
            << "Error: --input, --filters and --output were all empty\n\n";
        print_usage(argv[0]);
        return 1;
      }
    } else {
      // Check if project file was provided
      if (project_path.empty()) {
        std::cerr << "Error: No project file specified\n\n";
        print_usage(argv[0]);
        return 1;
      }

      // Check if at least one command was specified
      if (!do_process) {
        std::cerr << "Error: No command specified. You must use --process\n\n";
        print_usage(argv[0]);
        return 1;
      }
    }

    // Initialize logging - both app logger and core logger
    orc::init_app_logging(log_level, "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v",
                          log_file, "cli");
    orc::presenters::initCoreLogging(
        log_level, "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v", log_file);

    if (safe_core_plugins) {
      ORC_LOG_WARN(
          "Safe startup mode enabled: plugin registry cleared and "
          "ORC_STAGE_PLUGIN_PATHS ignored for this run");
    }

    // Initialize crash handler
    CrashHandlerConfig crash_config;
    crash_config.application_name = "orc-cli";
    crash_config.version = ORC_VERSION;
    crash_config.output_directory = fs::current_path().string();
    crash_config.primary_log_file = log_file;
    crash_config.enable_coredump = true;
    crash_config.auto_upload_info = true;

    // Add callback for custom application state
    crash_config.custom_info_callback = [&project_path,
                                         filtergraph_mode]() -> std::string {
      std::ostringstream info;
      if (filtergraph_mode) {
        info << "Mode: filtergraph\n";
      } else {
        info << "Project file: " << project_path << "\n";
      }
      info << "Working directory: " << fs::current_path().string() << "\n";
      return info.str();
    };

    if (!init_crash_handler(crash_config)) {
      ORC_LOG_WARN("Failed to initialize crash handler");
    }

    // Execute processing command
    int exit_code = 0;

    try {
      if (filtergraph_mode) {
        cli::FilterOptions options;

        if (graph_string_provided) {
          // Resolve the filtergraph source (inline string, file, or stdin).
          std::string graph = filtergraph;
          if (!filtergraph_file.empty()) {
            if (filtergraph_file == "-") {
              std::ostringstream buffer;
              buffer << std::cin.rdbuf();
              graph = buffer.str();
            } else {
              std::ifstream in(filtergraph_file);
              if (!in) {
                ORC_LOG_ERROR("Cannot open filtergraph file: {}",
                              filtergraph_file);
                cleanup_crash_handler();
                return 1;
              }
              std::ostringstream buffer;
              buffer << in.rdbuf();
              graph = buffer.str();
            }
          }
          options.filtergraph = graph;
        } else {
          options.input_stages = input_stages;
          options.filters_stages = filters_stages;
          options.output_stages = output_stages;
        }

        exit_code = cli::filter_command(options);
      } else {
        cli::ProcessOptions options;
        options.project_path = project_path;

        exit_code = cli::process_command(options);
      }
    } catch (const UserDataError& e) {
      ORC_LOG_WARN("Processing failed: {}", e.what());
      std::cerr << "\nWARNING: " << e.what() << "\n";

      cleanup_crash_handler();
      return 1;
    } catch (const std::exception& e) {
      const std::string error_message = e.what();

      std::cerr << "\nFATAL ERROR: " << error_message << "\n";

      // Create crash bundle for unhandled exceptions
      std::string bundle_path =
          create_crash_bundle(std::string("Exception: ") + error_message);
      if (!bundle_path.empty()) {
        std::cerr << "\nDiagnostic bundle created: " << bundle_path << "\n";
        std::cerr << "Please report this issue at: "
                     "https://github.com/simoninns/decode-orc/issues\n";
      }

      cleanup_crash_handler();
      return 1;
    } catch (...) {
      std::cerr << "\nFATAL ERROR: Unknown exception occurred\n";

      // Create crash bundle for unknown exceptions
      std::string bundle_path = create_crash_bundle("Unknown exception");
      if (!bundle_path.empty()) {
        std::cerr << "\nDiagnostic bundle created: " << bundle_path << "\n";
        std::cerr << "Please report this issue at: "
                     "https://github.com/simoninns/decode-orc/issues\n";
      }

      cleanup_crash_handler();
      return 1;
    }

    cleanup_crash_handler();
    return exit_code;
  } catch (const std::exception& e) {
    std::cerr << "\nFATAL ERROR: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "\nFATAL ERROR: Unknown exception\n";
    return 1;
  }
}
