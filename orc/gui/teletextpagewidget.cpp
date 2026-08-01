/*
 * File:        teletextpagewidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget painting a 40x25 Level 1 teletext page grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletextpagewidget.h"

#include <QFontDatabase>
#include <QPaintEvent>
#include <QPainter>

namespace {

// Level 1 display colours in spacing-attribute code order
// (ETSI EN 300 706 §12.2 Table 26): black, red, green, yellow, blue,
// magenta, cyan, white. Fixed by the standard — not theme-derived.
const QColor kTeletextColours[8] = {QColor(0, 0, 0),     QColor(255, 0, 0),
                                    QColor(0, 255, 0),   QColor(255, 255, 0),
                                    QColor(0, 0, 255),   QColor(255, 0, 255),
                                    QColor(0, 255, 255), QColor(255, 255, 255)};

QColor teletext_colour(uint8_t index) {
  return kTeletextColours[index < 8 ? index : 7];
}

// Paint the six sixel blocks of a mosaic cell (EN 300 706 §15.7.1 Table 47):
// pattern bit 0 top-left through bit 5 bottom-right, in a 2×3 grid.
// Separated mosaics leave a background gap on the right and bottom edges of
// each block (§15.7.1 NOTE 6).
void paint_mosaic(QPainter& painter, const QRectF& cell, uint8_t pattern,
                  bool separated, const QColor& foreground) {
  const qreal block_w = cell.width() / 2.0;
  const qreal block_h = cell.height() / 3.0;
  for (int block = 0; block < 6; ++block) {
    if ((pattern & (1 << block)) == 0) {
      continue;
    }
    const int block_col = block % 2;
    const int block_row = block / 2;
    QRectF rect(cell.left() + block_col * block_w,
                cell.top() + block_row * block_h, block_w, block_h);
    if (separated) {
      rect.adjust(0, 0, -block_w / 3.0, -block_h / 3.0);
    }
    painter.fillRect(rect, foreground);
  }
}

}  // namespace

TeletextPageWidget::TeletextPageWidget(QWidget* parent) : QWidget(parent) {
  // Teletext pages are always presented on a black screen.
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
}

void TeletextPageWidget::setPage(
    const orc::presenters::TeletextPageView& page) {
  page_ = page;
  update();
}

void TeletextPageWidget::clearPage() {
  page_.reset();
  update();
}

QSize TeletextPageWidget::sizeHint() const {
  // 12×16 pixel cells give a comfortable default 480×400 page.
  return QSize(orc::presenters::TeletextPageView::kColumns * 12,
               orc::presenters::TeletextPageView::kRows * 16);
}

void TeletextPageWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);

  if (!page_) {
    painter.setPen(Qt::gray);
    painter.drawText(rect(), Qt::AlignCenter, tr("No page"));
    return;
  }

  constexpr int kRows = orc::presenters::TeletextPageView::kRows;
  constexpr int kColumns = orc::presenters::TeletextPageView::kColumns;
  const qreal cell_w = static_cast<qreal>(width()) / kColumns;
  const qreal cell_h = static_cast<qreal>(height()) / kRows;

  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(qMax(1, static_cast<int>(cell_h * 0.85)));
  QFont mono_double = mono;
  mono_double.setPixelSize(qMax(1, static_cast<int>(cell_h * 1.7)));

  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const auto& cell =
          page_->cells[static_cast<size_t>(row)][static_cast<size_t>(col)];
      const QRectF cell_rect(col * cell_w, row * cell_h, cell_w, cell_h);

      if (cell.background != 0) {
        painter.fillRect(cell_rect, teletext_colour(cell.background));
      }

      // Lower cells of a double-height pair carry background only; the
      // origin row paints the glyph across both rows.
      if (cell.double_height_lower) {
        continue;
      }
      // Concealed cells display as SPACE until revealed (EN 300 706 §12.2
      // code 1/8); the dialog has no reveal control yet.
      if (cell.concealed) {
        continue;
      }

      const QRectF draw_rect =
          cell.double_height
              ? QRectF(cell_rect.left(), cell_rect.top(), cell_w, cell_h * 2)
              : cell_rect;

      if (cell.mosaic) {
        paint_mosaic(painter, draw_rect, cell.mosaic_pattern,
                     cell.mosaic_separated, teletext_colour(cell.foreground));
        continue;
      }

      if (cell.character == U' ') {
        continue;
      }
      painter.setPen(teletext_colour(cell.foreground));
      painter.setFont(cell.double_height ? mono_double : mono);
      painter.drawText(draw_rect, Qt::AlignCenter,
                       QString::fromUcs4(&cell.character, 1));
    }
  }
}
