set(STAGE_PLUGIN_SOURCES
    plugin.cpp
    teletext_sink_stage.cpp
    teletext_sink_stage_deps.cpp
)

orc_add_stage_plugin(
    orc-stage-plugin-teletext-sink
    OUTPUT_NAME orc-stage-plugin-teletext-sink
    PLUGIN_VERSION "${ORC_VERSION}"
    SOURCES ${STAGE_PLUGIN_SOURCES}
)
