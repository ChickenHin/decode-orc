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
#include <QFontMetricsF>
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

// Nominal shape of a teletext character rectangle. The page is drawn into a
// rect of this aspect so glyphs and mosaic blocks keep the proportions of
// Fig. 8 of the BBC 1976 specification rather than following the widget.
constexpr int kCellWidthUnits = 12;
constexpr int kCellHeightUnits = 20;

QColor teletext_colour(uint8_t index) {
  return kTeletextColours[index < 8 ? index : 7];
}

// Paint the six sixel blocks of a mosaic cell (EN 300 706 §15.7.1 Table 47):
// pattern bit 0 top-left through bit 5 bottom-right, in a 2×3 grid. In
// separated mode each block is "surrounded by a border of the background
// colour" (§12.2 code 1/A; BBC 1976 Fig. 8 shows the border around *and*
// between the blocks), so the block is inset on all four sides.
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

void TeletextPageWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

QSize TeletextPageWidget::sizeHint() const {
  return QSize(orc::presenters::TeletextPageView::kColumns * kCellWidthUnits,
               orc::presenters::TeletextPageView::kRows * kCellHeightUnits);
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

  // Fit the page into the widget at the character-rectangle aspect ratio; a
  // stretched grid would misshape every glyph and mosaic block.
  const qreal page_aspect = static_cast<qreal>(kColumns * kCellWidthUnits) /
                            static_cast<qreal>(kRows * kCellHeightUnits);
  qreal page_w = width();
  qreal page_h = page_w / page_aspect;
  if (page_h > height()) {
    page_h = height();
    page_w = page_h * page_aspect;
  }
  const QRectF page_rect((width() - page_w) / 2.0, (height() - page_h) / 2.0,
                         page_w, page_h);
  const qreal cell_w = page_rect.width() / kColumns;
  const qreal cell_h = page_rect.height() / kRows;
  if (cell_w < 1.0 || cell_h < 1.0) {
    return;
  }

  // On newsflash (C5) and subtitle (C6) pages only the boxed area is
  // displayed; everything outside it is transparent to the video picture
  // (EN 300 706 §12.2 codes 0/A-0/B), which here means the black screen.
  const bool boxed_only = page_->newsflash || page_->subtitle;

  painter.setClipRect(page_rect);

  const auto cell_rect_at = [&](int row, int col) {
    return QRectF(page_rect.left() + col * cell_w,
                  page_rect.top() + row * cell_h, cell_w, cell_h);
  };

  // Pass 1: backgrounds. Double height stretches a character across two rows
  // (§12.2 code 0/D), so every background must be down before any foreground
  // is drawn — otherwise the lower row's fill erases the bottom half of the
  // character above it.
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const auto& cell =
          page_->cells[static_cast<size_t>(row)][static_cast<size_t>(col)];
      if (boxed_only && !cell.boxed) {
        continue;
      }
      if (cell.background != 0) {
        painter.fillRect(cell_rect_at(row, col),
                         teletext_colour(cell.background));
      }
    }
  }

  // The glyph is sized to fill the character rectangle vertically and then
  // squeezed if the font is wider than the cell, so neighbouring characters
  // never overlap. Double height reuses this glyph under a 2x vertical
  // scale: "only the upper half ... stretched vertically to fill the
  // rectangle. On Row 'R+1', the corresponding lower half ... is similarly
  // displayed" (BBC Broadcast Teletext 1976 §3.1.5) — height doubles, width
  // does not.
  QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  mono.setPixelSize(qMax(1, static_cast<int>(cell_h)));
  const QFontMetricsF metrics(mono);
  const qreal advance = metrics.horizontalAdvance(QChar(u'M'));
  const qreal squeeze =
      advance > 0.0 ? qMin<qreal>(1.0, cell_w / advance) : 1.0;
  painter.setFont(mono);

  // Pass 2: foregrounds.
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      const auto& cell =
          page_->cells[static_cast<size_t>(row)][static_cast<size_t>(col)];
      if (boxed_only && !cell.boxed) {
        continue;
      }
      // Lower cells of a double-height pair carry no foreground data of their
      // own; the origin row paints the character's lower half over them.
      if (cell.double_height_lower) {
        continue;
      }
      // Concealed cells display as SPACE until revealed (EN 300 706 §12.2
      // code 1/8); the dialog has no reveal control yet.
      if (cell.concealed) {
        continue;
      }

      const QRectF cell_rect = cell_rect_at(row, col);
      const qreal height_scale = cell.double_height ? 2.0 : 1.0;

      if (cell.mosaic) {
        QRectF draw_rect = cell_rect;
        draw_rect.setHeight(cell_h * height_scale);
        paint_mosaic(painter, draw_rect, cell.mosaic_pattern,
                     cell.mosaic_separated, teletext_colour(cell.foreground));
        continue;
      }

      if (cell.character == U' ') {
        continue;
      }
      painter.save();
      painter.setPen(teletext_colour(cell.foreground));
      painter.translate(cell_rect.topLeft());
      painter.scale(squeeze, height_scale);
      painter.drawText(QRectF(0, 0, cell_w / squeeze, cell_h), Qt::AlignCenter,
                       QString::fromUcs4(&cell.character, 1));
      painter.restore();
    }
  }

  if (show_data_errors_) {
    paintDataErrorOverlay(painter, *page_, page_rect, cell_w, cell_h);
  }
}

void TeletextPageWidget::paintDataErrorOverlay(
    QPainter& painter, const orc::presenters::TeletextPageView& page,
    const QRectF& page_rect, qreal cell_w, qreal cell_h) {
  constexpr int kRows = orc::presenters::TeletextPageView::kRows;
  constexpr int kColumns = orc::presenters::TeletextPageView::kColumns;

  // Rows no packet was recovered for: a hatched band across the whole row,
  // so a recovery gap cannot be mistaken for a transmitted blank row.
  QBrush hatch(QColor(255, 80, 80, 90), Qt::BDiagPattern);
  for (int row = 1; row < kRows; ++row) {
    if (page.row_received[static_cast<size_t>(row)]) {
      continue;
    }
    // A row consumed by double height above it carries no data by design.
    if (page.cells[static_cast<size_t>(row)][0].double_height_lower) {
      continue;
    }
    const QRectF band(page_rect.left(), page_rect.top() + row * cell_h,
                      page_rect.width(), cell_h);
    painter.fillRect(band, hatch);
  }

  // Individual bytes that failed odd parity: outline the character
  // rectangle that was substituted with SPACE.
  painter.setPen(QPen(QColor(255, 80, 80, 200), 0));
  painter.setBrush(Qt::NoBrush);
  for (int row = 0; row < kRows; ++row) {
    for (int col = 0; col < kColumns; ++col) {
      if (!page.cells[static_cast<size_t>(row)][static_cast<size_t>(col)]
               .parity_error) {
        continue;
      }
      painter.drawRect(QRectF(page_rect.left() + col * cell_w,
                              page_rect.top() + row * cell_h, cell_w, cell_h)
                           .adjusted(0.5, 0.5, -0.5, -0.5));
    }
  }
}
