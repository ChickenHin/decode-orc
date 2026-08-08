set(STAGE_PLUGIN_SOURCES
    plugin.cpp
    teletext_sink_stage.cpp
    teletext_sink_deps.cpp
    teletext_frame_slicer.cpp
    teletext_scan_state.cpp
    teletext_block_scanner.cpp
    teletext_page_catalogue.cpp
    teletext_squash_stats.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-teletext-sink
    OUTPUT_NAME orc-stage-plugin-teletext-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
)
