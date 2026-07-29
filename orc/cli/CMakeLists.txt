# Section: CLI build configuration
# Defines the orc-cli executable and associated build settings
add_executable(orc-cli 
    orc_cli.cpp
    command_process.cpp
    command_filter.cpp
    command_plugins.cpp
    stage_category.cpp
)

# Section: Architecture enforcement (MVP)
# CLI uses presenter and common layers; prevents direct core linkage
target_link_libraries(orc-cli PRIVATE 
    orc-presenters
    orc-common
)

# Section: Public include paths for CLI
target_include_directories(orc-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/presenters/include
    ${CMAKE_SOURCE_DIR}/orc/public
    ${CMAKE_SOURCE_DIR}/orc/common/include
    ${CMAKE_BINARY_DIR}/generated
)

# Section: Build-time flags for MVP enforcement
target_compile_definitions(orc-cli PRIVATE
    ORC_CLI_BUILD
)

if(APPLE)
    set_target_properties(orc-cli PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
elseif(UNIX AND NOT WIN32)
    set_target_properties(orc-cli PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN/../lib/orc-stage-plugins")
endif()

# Section: Install rules
if(APPLE)
    # Embed CLI inside the macOS app bundle so drag-and-drop install contains both tools
    install(TARGETS orc-cli
        RUNTIME DESTINATION "orc-gui.app/Contents/MacOS"
        COMPONENT Runtime
    )
else()
    install(TARGETS orc-cli
        RUNTIME DESTINATION bin
        COMPONENT Runtime
    )
endif()

if(BUILD_UNIT_TESTS)
    add_test(
        NAME CLI.HelpShowsProcessOnlyCommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsProcessOnlyCommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--process"
    )

    add_test(
        NAME CLI.HelpShowsPluginsSubcommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsPluginsSubcommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "plugins"
    )

    add_test(
        NAME CLI.HelpShowsSafeCorePluginsOption
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsSafeCorePluginsOption PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--safe-core-plugins"
    )

    add_test(
        NAME CLI.PluginsListWithSafeCorePluginsBeforeSubcommandSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins --safe-core-plugins list
    )
    set_tests_properties(CLI.PluginsListWithSafeCorePluginsBeforeSubcommandSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-before;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-before"
    )

    add_test(
        NAME CLI.PluginsListWithSafeCorePluginsAfterSubcommandSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins list --safe-core-plugins
    )
    set_tests_properties(CLI.PluginsListWithSafeCorePluginsAfterSubcommandSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-after;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-after"
    )

    add_test(
        NAME CLI.PluginsListSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins list
    )
    set_tests_properties(CLI.PluginsListSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
    )

    add_test(
        NAME CLI.PluginsAddMissingPathFails
        COMMAND $<TARGET_FILE:orc-cli> plugins add
    )
    set_tests_properties(CLI.PluginsAddMissingPathFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsRemoveMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins remove
    )
    set_tests_properties(CLI.PluginsRemoveMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsEnableMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins enable
    )
    set_tests_properties(CLI.PluginsEnableMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsDisableMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins disable
    )
    set_tests_properties(CLI.PluginsDisableMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsSearchMissingTermFails
        COMMAND $<TARGET_FILE:orc-cli> plugins search
    )
    set_tests_properties(CLI.PluginsSearchMissingTermFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsInfoMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins info
    )
    set_tests_properties(CLI.PluginsInfoMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsInstallMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins install
    )
    set_tests_properties(CLI.PluginsInstallMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsHelpShowsDiscoverySubcommands
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsDiscoverySubcommands PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "search"
    )

    add_test(
        NAME CLI.PluginsUnknownSubcommandFails
        COMMAND $<TARGET_FILE:orc-cli> plugins frobnicate
    )
    set_tests_properties(CLI.PluginsUnknownSubcommandFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.RejectsPreviewExportOption
        COMMAND $<TARGET_FILE:orc-cli> --preview-export
    )
    set_tests_properties(CLI.RejectsPreviewExportOption PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.ProcessNoSinksFails
        COMMAND $<TARGET_FILE:orc-cli> ${CMAKE_CURRENT_SOURCE_DIR}/test-data/no-sinks.orcprj --process
    )
    set_tests_properties(CLI.ProcessNoSinksFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # --- Filtergraph mode (input/filters/output triad) ------------------------

    add_test(
        NAME CLI.HelpShowsFilterCommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsFilterCommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--source"
    )

    add_test(
        NAME CLI.FilterUnknownStageFails
        COMMAND $<TARGET_FILE:orc-cli>
                --source "definitely_not_a_stage" --sink "video_sink"
    )
    set_tests_properties(CLI.FilterUnknownStageFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.FilterMalformedGraphFails
        COMMAND $<TARGET_FILE:orc-cli> --source "src=badparam" --sink sink
    )
    set_tests_properties(CLI.FilterMalformedGraphFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.FilterRejectsProjectFileCombination
        COMMAND $<TARGET_FILE:orc-cli> project.orcprj --source "src" --sink sink
    )
    set_tests_properties(CLI.FilterRejectsProjectFileCombination PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.ProcessAndTriadCannotCombine
        COMMAND $<TARGET_FILE:orc-cli> project.orcprj --process --source "src"
    )
    set_tests_properties(CLI.ProcessAndTriadCannotCombine PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.TriadRejectsSinkStageInSource
        COMMAND $<TARGET_FILE:orc-cli> --source "video_sink" --sink "video_sink"
    )
    set_tests_properties(CLI.TriadRejectsSinkStageInSource PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.HelpShowsExportProjectOption
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsExportProjectOption PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--export-project"
    )

    add_test(
        NAME CLI.ExportProjectRejectedWithProjectFile
        COMMAND $<TARGET_FILE:orc-cli> project.orcprj --process
                --export-project out.orcprj
    )
    set_tests_properties(CLI.ExportProjectRejectedWithProjectFile PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.VideoFormatRejectedWithoutTriad
        COMMAND $<TARGET_FILE:orc-cli> --video-format NTSC
    )
    set_tests_properties(CLI.VideoFormatRejectedWithoutTriad PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.VideoFormatWorksWithoutExportProject
        COMMAND $<TARGET_FILE:orc-cli> --source "src" --sink "sink"
                --video-format NTSC
    )
    set_tests_properties(CLI.VideoFormatWorksWithoutExportProject PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE  # fails on "unknown stage 'src'", not on --video-format
        FAIL_REGULAR_EXPRESSION "only makes sense with"
    )

    add_test(
        NAME CLI.HelpShowsVideoFormatOption
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsVideoFormatOption PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--video-format"
    )

    add_test(
        NAME CLI.SourceTypeRejectedWithoutTriad
        COMMAND $<TARGET_FILE:orc-cli> --source-type composite
    )
    set_tests_properties(CLI.SourceTypeRejectedWithoutTriad PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.SourceTypeWorksWithoutExportProject
        COMMAND $<TARGET_FILE:orc-cli> --source "src" --sink "sink"
                --source-type composite
    )
    set_tests_properties(CLI.SourceTypeWorksWithoutExportProject PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE  # fails on "unknown stage 'src'", not on --source-type
        FAIL_REGULAR_EXPRESSION "only makes sense with"
    )

    add_test(
        NAME CLI.HelpShowsSourceTypeOption
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsSourceTypeOption PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--source-type"
    )

    # Pure unit test for the filtergraph parser (no orc-core dependency).
    find_package(GTest REQUIRED)
    add_executable(orc-cli-filtergraph-parser-test
        filtergraph_parser_test.cpp
        ${CMAKE_SOURCE_DIR}/orc/presenters/src/filtergraph_parser.cpp
    )
    target_include_directories(orc-cli-filtergraph-parser-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/orc/presenters/include
    )
    target_link_libraries(orc-cli-filtergraph-parser-test PRIVATE
        GTest::gtest_main
    )
    add_test(
        NAME CLI.FiltergraphParserUnit
        COMMAND $<TARGET_FILE:orc-cli-filtergraph-parser-test>
    )
    set_tests_properties(CLI.FiltergraphParserUnit PROPERTIES
        LABELS "unit;cli"
    )
endif()
