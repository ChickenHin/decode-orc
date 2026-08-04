# VBI Source Stage Plugin
#
# Ingests third-party raw VBI teletext captures (bt8x8 and family card dumps,
# cropped .tbc VBI files) and presents them to the teletext decoders as CVBS.
#
# The container model, transports, reader and validation build as a plugin-side
# static library so they can be unit tested directly. It compiles against the
# public SDK headers only and is linked privately into the stage plugin.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_library(orc-vbi-source STATIC
    vbi_source_format.cpp
    vbi_line_reader.cpp
    vbi_source_validation.cpp
    vbi_transport.cpp
    vbi_teletext_service.cpp
    vbi_frame_geometry.cpp
    vbi_line_mapping.cpp
    vbi_output_levels.cpp
    vbi_level_mapper.cpp
    vbi_resampler.cpp
    vbi_line_placement.cpp
    vbi_cri_template.cpp
    vbi_cri_correlator.cpp
    vbi_offset_calibration.cpp
    vbi_timing_cross_checks.cpp
    vbi_vertical_interval.cpp
    vbi_line_synthesis.cpp
    vbi_burst_synthesis.cpp
    vbi_frame_synthesis.cpp
    vbi_frame_sequence.cpp
)

target_include_directories(orc-vbi-source
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

set_target_properties(orc-vbi-source PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# The plugin SDK provides the <orc/support/...> contract headers and the
# spdlog/fmt usage requirements needed by <orc/support/logging.h>.
target_link_libraries(orc-vbi-source PUBLIC orc-plugin-sdk)

# FFmpeg unwraps FLAC-compressed captures (vbi_transport.cpp). FLAC is used
# here as a generic lossless byte compressor, not as audio, which is why
# libavformat/libavcodec are preferred to adding a libFLAC dependency.
target_link_libraries(orc-vbi-source PUBLIC PkgConfig::FFMPEG)
