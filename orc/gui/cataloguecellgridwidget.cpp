/*
 * File:        cataloguecellgridwidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget painting a CataloguePayload character-cell grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cataloguecellgridwidget.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPaintEvent>
#include <QPainter>

namespace {

// The widest grid worth hinting a size for. paintEvent fits whatever grid the
// widget actually holds into the rectangle this ends up giving, so a narrower
// one is not squeezed by the hint being generous.
constexpr int kHintColumns = 40;
constexpr int kHintRows = 25;

QColor palette_colour(const orc::CatalogueCellGrid& grid, uint8_t index) {
  if (grid.palette.empty()) {
    return index == 0 ? QColor(0, 0, 0) : QColor(255, 255, 255);
  }
  const size_t slot = std::min<size_t>(index, grid.palette.size() - 1);
  const orc::CatalogueColour& colour = grid.palette[slot];
  // A transparent cell colour shows whatever is behind the page, which here is
  // the black screen the grid is drawn on.
  if (colour.transparent) {
    return QColor(0, 0, 0);
  }
  return QColor(colour.red, colour.green, colour.blue);
}

// Paint the six sub-element blocks of a mosaic cell: pattern bit 0 top-left
// through bit 5 bottom-right, in a 2x3 grid. In separated mode each block is
// surrounded by a border of the background colour, so it is inset on all four
// sides.
void paint_mosaic(QPainter& painter, const QRectF& cell, uint8_t pattern,
                  bool separated, const QColor& foreground) {
  const qreal block_w = cell.width() / 2.0;
  const qreal block_h = cell.height() / 3.0;
  const qreal inset_x = separated ? block_w / 6.0 : 0.0;
  const qreal inset_y = separated ? block_h / 6.0 : 0.0;
  for (int block = 0; block < 6; ++block) {
    if ((pattern & (1 << block)) == 0) {
      continue;
    }
    const int block_col = block % 2;
    const int block_row = block / 2;
    QRectF rect(cell.left() + block_col * block_w,
                cell.top() + block_row * block_h, block_w, block_h);
    rect.adjust(inset_x, inset_y, -inset_x, -inset_y);
    painter.fillRect(rect, foreground);
  }
}

}  // namespace

CatalogueCellGridWidget::CatalogueCellGridWidget(QWidget* parent)
    : QWidget(parent), placeholder_(tr("Nothing to show")) {
  // A cell grid is always presented on a black screen.
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
}

void CatalogueCellGridWidget::setGrid(const orc::CatalogueCellGrid& grid) {
  grid_ = grid;
  update();
}

void CatalogueCellGridWidget::clearGrid() {
  grid_.reset();
  update();
}

void CatalogueCellGridWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

void CatalogueCellGridWidget::setPlaceholderText(const QString& text) {
  placeholder_ = text;
  update();
}

QSize CatalogueCellGridWidget::sizeHint() const {
  const int cell_w = grid_ ? grid_->cell_aspect_width : 12;
  const int cell_h = grid_ ? grid_->cell_aspect_height : 20;
  return QSize(kHintColumns * cell_w, kHintRows * cell_h);
}

void CatalogueCellGridWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);

  if (!grid_ || !grid_->valid()) {
    painter.setPen(Qt::gray);
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  const orc::CatalogueCellGrid& grid = *grid_;
  const int rows = grid.rows;
  const int columns = grid.columns;
  const int aspect_w = grid.cell_aspect_width > 0 ? grid.cell_aspect_width : 1;
  const int aspect_h =
      grid.cell_aspect_height > 0 ? grid.cell_aspect_height : 1;

  // Fit the grid into the widget at the character-rectangle aspect ratio; a
  // stretched grid would misshape every glyph and mosaic block.
  const qreal page_aspect = static_cast<qreal>(columns * aspect_w) /
                            static_cast<qreal>(rows * aspect_h);
  qreal page_w = width();
  qreal page_h = page_w / page_aspect;
  if (page_h > height()) {
    page_h = height();
    page_w = page_h * page_aspect;
  }
  const QRectF page_rect((width() - page_w) / 2.0, (height() - page_h) / 2.0,
                         page_w, page_h);
  const qreal cell_w = page_rect.width() / columns;
  const qreal cell_h = page_rect.height() / rows;
  if (cell_w < 1.0 || cell_h < 1.0) {
    return;
  }

  painter.setClipRect(page_rect);

  const auto cell_rect_at = [&](int row, int col) {
    return QRectF(page_rect.left() + col * cell_w,
                  page_rect.top() + row * cell_h, cell_w, cell_h);
  };

  // Pass 1: backgrounds. Double height stretches a character across two rows,
  // so every background must be down before any foreground is drawn —
  // otherwise the lower row's fill erases the bottom half of the character
  // above it.
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < columns; ++col) {
      const orc::CatalogueCell& cell = grid.at(row, col);
      if (grid.boxed_only && !cell.boxed) {
        continue;
      }
      if (cell.background != 0) {
        painter.fillRect(cell_rect_at(row, col),
                         palette_colour(grid, cell.background));
      }
    }
  }

  // The glyph is sized to fill the character rectangle vertically and then
  // squeezed if the font is wider than the cell, so neighbouring characters
  // never overlap. Double height reuses this glyph under a 2x vertical scale:
  // height doubles, width does not.
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(qMax(1, static_cast<int>(cell_h)));
  const QFontMetricsF metrics(mono);
  const qreal advance = metrics.horizontalAdvance(QChar(u'M'));
  const qreal squeeze =
      advance > 0.0 ? qMin<qreal>(1.0, cell_w / advance) : 1.0;
  painter.setFont(mono);

  // Pass 2: foregrounds.
  for (int row = 0; row < rows; ++row) {
    for (int col = 0; col < columns; ++col) {
      const orc::CatalogueCell& cell = grid.at(row, col);
      if (grid.boxed_only && !cell.boxed) {
        continue;
      }
      // Lower cells of a double-height pair carry no foreground data of their
      // own; the origin row paints the character's lower half over them.
      if (cell.double_height_lower) {
        continue;
      }
      // Concealed cells display as SPACE until revealed; there is no reveal
      // control.
      if (cell.concealed) {
        continue;
      }

      const QRectF cell_rect = cell_rect_at(row, col);
      const qreal height_scale = cell.double_height ? 2.0 : 1.0;

      if (cell.mosaic) {
        QRectF draw_rect = cell_rect;
        draw_rect.setHeight(cell_h * height_scale);
        paint_mosaic(painter, draw_rect, cell.mosaic_pattern,
                     cell.mosaic_separated,
                     palette_colour(grid, cell.foreground));
        continue;
      }

      if (cell.character == U' ') {
        continue;
      }
      painter.save();
      painter.setPen(palette_colour(grid, cell.foreground));
      painter.translate(cell_rect.topLeft());
      painter.scale(squeeze, height_scale);
      painter.drawText(QRectF(0, 0, cell_w / squeeze, cell_h), Qt::AlignCenter,
                       QString::fromUcs4(&cell.character, 1));
      painter.restore();
    }
  }

  if (show_data_errors_) {
    paintDataErrorOverlay(painter, grid, page_rect, cell_w, cell_h);
  }
}

void CatalogueCellGridWidget::paintDataErrorOverlay(
    QPainter& painter, const orc::CatalogueCellGrid& grid,
    const QRectF& page_rect, qreal cell_w, qreal cell_h) {
  const bool have_status =
      grid.row_status.size() == static_cast<size_t>(grid.rows);

  // Rows no packet was recovered for, but only once something is known to have
  // gone astray.
  //
  // A missing row on its own says nothing. Services habitually leave out the
  // blank rows that space a page out instead of transmitting 40 spaces, so
  // marking every un-received row put three or four bands on a page that had
  // arrived perfectly — which is worse than not marking at all, because it
  // trains the reader to ignore the marks. When data really was lost the page
  // cannot say which row each packet would have carried, so every gap becomes
  // a candidate and all of them are banded.
  if (grid.data_lost && have_status) {
    QBrush hatch(QColor(255, 80, 80, 90), Qt::BDiagPattern);
    for (int row = 1; row < grid.rows; ++row) {
      if (grid.row_status[static_cast<size_t>(row)].received) {
        continue;
      }
      // A row consumed by double height above it carries no data by design.
      if (grid.at(row, 0).double_height_lower) {
        continue;
      }
      const QRectF band(page_rect.left(), page_rect.top() + row * cell_h,
                        page_rect.width(), cell_h);
      painter.fillRect(band, hatch);
    }
  }

  // Rows resting on a single unchecked copy while other rows have been
  // corrected against a repeat. Amber rather than red, and drawn under the
  // damage outlines: nothing is known to be wrong with these rows, they have
  // simply never been checked — which is where a row carried onto the wrong
  // address by a burst survives to the screen looking like content.
  if (have_status) {
    QBrush unchecked(QColor(255, 190, 60, 55), Qt::FDiagPattern);
    for (int row = 1; row < grid.rows; ++row) {
      if (!grid.row_status[static_cast<size_t>(row)].unconfirmed) {
        continue;
      }
      const QRectF band(page_rect.left(), page_rect.top() + row * cell_h,
                        page_rect.width(), cell_h);
      painter.fillRect(band, unchecked);
    }
  }

  // Individual bytes known damaged: outline the character rectangle that was
  // substituted.
  painter.setPen(QPen(QColor(255, 80, 80, 200), 0));
  painter.setBrush(Qt::NoBrush);
  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.columns; ++col) {
      if (!grid.at(row, col).damaged) {
        continue;
      }
      painter.drawRect(QRectF(page_rect.left() + col * cell_w,
                              page_rect.top() + row * cell_h, cell_w, cell_h)
                           .adjusted(0.5, 0.5, -0.5, -0.5));
    }
  }
}
