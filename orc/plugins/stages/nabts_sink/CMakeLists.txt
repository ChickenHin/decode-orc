set(STAGE_PLUGIN_SOURCES
    plugin.cpp
    nabts_sink_stage.cpp
    nabts_sink_deps.cpp
    nabts_frame_slicer.cpp
    nabts_scan_state.cpp
    nabts_block_scanner.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-nabts-sink
    OUTPUT_NAME orc-stage-plugin-nabts-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
)
