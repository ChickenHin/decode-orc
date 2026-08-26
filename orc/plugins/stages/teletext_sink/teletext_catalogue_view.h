/*
 * File:        teletext_catalogue_view.h
 * Module:      teletext_sink stage plugin
 * Purpose:     The page catalogue as an SDK CatalogueDataset the host can
 * browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_SINK_TELETEXT_CATALOGUE_VIEW_H
#define ORC_TELETEXT_SINK_TELETEXT_CATALOGUE_VIEW_H

#include <orc/stage/tooling/catalogue_results.h>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {

/**
 * @brief Build the browsable catalogue the host's viewer reads
 *
 * A page becomes a top-level item and each of its sub-pages a variant of it, so
 * a multi-page set (ETSI EN 300 706 Annex A.1) is stepped through under the
 * display rather than cluttering the list. The page's cells are resolved here —
 * G0 codes to Unicode through the national option sub-set the page's own header
 * selected, mosaic codes to sixel patterns — because the host must not need a
 * teletext decoder to draw a teletext page.
 */
CatalogueDataset build_teletext_catalogue(const TeletextAnalysisDataset& data);

/**
 * @brief One Level 1 page snapshot as a drawable cell grid
 *
 * Exposed for the unit tests, which check the cell mapping directly rather than
 * through the whole catalogue.
 */
CatalogueCellGrid teletext_page_grid(const TeletextPageSnapshot& snapshot,
                                     uint64_t lost_packets = 0);

}  // namespace orc

#endif  // ORC_TELETEXT_SINK_TELETEXT_CATALOGUE_VIEW_H
