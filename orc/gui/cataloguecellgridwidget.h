/*
 * File:        cataloguecellgridwidget.h
 * Module:      orc-gui
 * Purpose:     Widget painting a CataloguePayload character-cell grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUECELLGRIDWIDGET_H
#define CATALOGUECELLGRIDWIDGET_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QRectF>
#include <QWidget>
#include <optional>

class QPainter;

/**
 * @brief Paints a character-cell grid payload
 *
 * The grid is drawn into an aspect-locked rect built from the nominal character
 * rectangle the payload carries, so glyphs and mosaic blocks keep their
 * proportions whatever shape the widget is given.
 *
 * Alphanumeric cells use the system monospace font; mosaic cells are painted as
 * 2x3 sub-element blocks, contiguous or separated. Cell attributes are
 * honoured: foreground and background out of the payload's own palette, double
 * height (the character spans both rows at unchanged width), boxing on a
 * boxed-only page, conceal (rendered as SPACE — no reveal control) and flash
 * (rendered static). The palette travels with the payload rather than being
 * theme-derived, because a service with a fixed display palette means it.
 *
 * Painting is two-pass — every background first, then every foreground —
 * because a double-height character occupies the row below its origin and a
 * row-sequential paint would erase its lower half.
 */
class CatalogueCellGridWidget : public QWidget {
  Q_OBJECT

 public:
  explicit CatalogueCellGridWidget(QWidget* parent = nullptr);

  void setGrid(const orc::CatalogueCellGrid& grid);
  void clearGrid();
  bool hasGrid() const { return grid_.has_value(); }

  /**
   * @brief Overlay the grid with markers for data that never arrived
   *
   * A row no packet was recovered for renders exactly like a transmitted blank
   * row, and a damaged byte exactly like a SPACE, so without this overlay a
   * recovery gap is indistinguishable from content. Off by default, because
   * the markers are not part of the page.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /// Currently displayed grid (test seam; nullptr when cleared)
  const orc::CatalogueCellGrid* grid() const {
    return grid_ ? &*grid_ : nullptr;
  }

  /// Placeholder shown when no grid is set
  void setPlaceholderText(const QString& text);

  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  /// Hatch rows that carried no packet and outline damaged cells
  static void paintDataErrorOverlay(QPainter& painter,
                                    const orc::CatalogueCellGrid& grid,
                                    const QRectF& page_rect, qreal cell_w,
                                    qreal cell_h);

  std::optional<orc::CatalogueCellGrid> grid_;
  QString placeholder_;
  bool show_data_errors_ = false;
};

#endif  // CATALOGUECELLGRIDWIDGET_H
