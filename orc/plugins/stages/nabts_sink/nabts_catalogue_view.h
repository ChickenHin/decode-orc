/*
 * File:        nabts_catalogue_view.h
 * Module:      nabts_sink stage plugin
 * Purpose:     The record catalogue as an SDK CatalogueDataset the host can
 *              browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H
#define ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H

#include <orc/stage/tooling/catalogue_results.h>

#include <vector>

#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief The receiver choices the viewer offers, coarsest first
 *
 * One per NaplpsRenderMode, each keyed by the name the mode is stored under, so
 * an option the reader picks names a mode straight back through
 * naplps_render_mode_from_name().
 */
std::vector<CatalogueViewOption> naplps_view_options();

/**
 * @brief Build the browsable catalogue the host's viewer reads
 *
 * Each record is a top-level item; records have no variants, so nothing nests.
 * A presentation record (types 0, 1 and 3) becomes a display list, an
 * application record (type 2) a function listing, and the caption service — the
 * records CEA-516 §5.2.7.3 flags, read in order — becomes one further item of
 * its own, because reading a caption service one record at a time tells a
 * viewer nothing about it.
 *
 * @p mode is the receiver resolution the display lists are resolved against;
 * nothing else in the catalogue depends on it.
 */
CatalogueDataset build_nabts_catalogue(
    const NabtsAnalysisDataset& data,
    NaplpsRenderMode mode = NaplpsRenderMode::kReference);

/**
 * @brief One NAPLPS record snapshot as a drawable display list
 *
 * Runs the per-character primitives the interpreter emitted into coalesced text
 * runs, resolves colours out of the three-bits-per-gun GRB of X3.110 §5.3.1 and
 * indexes the downloadable glyphs, so the host draws without a NAPLPS decoder.
 * Exposed for the unit tests.
 *
 * @p mode names the receiver the page is drawn against, which the list carries
 * so a renderer can size what X3.110 sizes in physical pixels.
 */
CatalogueDisplayList nabts_page_display_list(
    const NabtsPageSnapshot& snapshot,
    NaplpsRenderMode mode = NaplpsRenderMode::kReference);

}  // namespace orc

#endif  // ORC_NABTS_SINK_NABTS_CATALOGUE_VIEW_H
