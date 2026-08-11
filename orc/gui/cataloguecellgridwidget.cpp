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
#include <QHideEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QShowEvent>

#include "catalogue_flash_clock.h"
#include "teletext_glyph_painter.h"

namespace {

// The widest grid worth hinting a size for. paintEvent fits whatever grid the
// widget actually holds into the rectangle this ends up giving, so a narrower
// one is not squeezed by the hint being generous.
constexpr int kHintColumns = 40;
constexpr int kHintRows = 25;

// The narrowest a saved page is worth being. A character rectangle is nominally
// a dozen pixels across, which is legible on a receiver's line-doubled display
// and not in a still that is looked at on its own, so the page is scaled up
// until it is at least this wide.
constexpr int kSavedPixelsAcross = 720;

// The fixed-pitch font the cells the character generator has no glyph for are
// drawn in, with families named after the platform's own so a glyph it lacks
// is still drawn by something.
//
// A cell grid is not ASCII and never was. The Latin G0 set of a teletext page
// reaches the arrows, the vulgar fractions and the double vertical bar of
// ETSI EN 300 706 Table 36, every set puts a filled rectangle at 7/F, and a
// page in one of the Cyrillic G0 sets is Cyrillic throughout. Which of that a
// platform's fixed font covers is not something this widget can assume — a
// missing glyph is drawn as a box, or as nothing at all, and either reads as a
// decoding fault rather than as the font problem it is.
//
// Naming fallbacks is what fixes it: Qt matches the families in order per
// character, so the platform font still draws everything it can and only the
// characters it cannot fall through to the broad-coverage families after it.
QFont cell_grid_font() {
  QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

  QStringList families = font.families();
  if (families.isEmpty() && !font.family().isEmpty()) {
    families << font.family();
  }
  // Broad-coverage monospaced faces, then the generic alias fontconfig and the
  // other platforms resolve to whatever is installed. Each is skipped when the
  // platform font is already it, so the common case names one family.
  for (const char* fallback :
       {"DejaVu Sans Mono", "Noto Sans Mono", "Liberation Mono", "Consolas",
        "Menlo", "Courier New", "monospace"}) {
    const QString name = QString::fromLatin1(fallback);
    if (!families.contains(name, Qt::CaseInsensitive)) {
      families << name;
    }
  }
  font.setFamilies(families);
  // Says what the fallbacks are for, so a substitution Qt makes on its own
  // stays fixed-pitch and the columns keep their alignment.
  font.setStyleHint(QFont::Monospace);
  return font;
}

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

// Whether this cell's flash attribute makes a visible difference.
//
// ETSI EN 300 706 §12.2 code 0/8 flashes "the foreground pixels of the
// following alphanumeric and mosaics characters", so a cell that paints no
// foreground in the lit phase paints none in the blank phase either and never
// changes. That is not a rare case: the attribute is set once and runs to the
// end of the row, so most flashing rows are mostly flagged SPACEs.
bool cell_flash_is_visible(const orc::CatalogueCellGrid& grid,
                           const orc::CatalogueCell& cell) {
  if (!cell.flash) {
    return false;
  }
  if (grid.boxed_only && !cell.boxed) {
    return false;
  }
  // The lower cell of a double-height pair carries background only, and a
  // concealed cell displays as SPACE until revealed.
  if (cell.double_height_lower || cell.concealed) {
    return false;
  }
  if (cell.mosaic) {
    return cell.mosaic_pattern != 0;
  }
  return cell.character != U' ';
}

}  // namespace

CatalogueCellGridWidget::CatalogueCellGridWidget(QWidget* parent)
    : QWidget(parent), placeholder_(tr("Nothing to show")) {
  // A cell grid is always presented on a black screen.
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
}

CatalogueCellGridWidget::~CatalogueCellGridWidget() {
  if (flash_subscribed_ && !flash_clock_.isNull()) {
    flash_clock_->release();
  }
}

void CatalogueCellGridWidget::setGrid(const orc::CatalogueCellGrid& grid) {
  grid_ = grid;
  has_flashing_cells_ = false;
  if (grid_->valid()) {
    for (const orc::CatalogueCell& cell : grid_->cells) {
      if (cell_flash_is_visible(*grid_, cell)) {
        has_flashing_cells_ = true;
        break;
      }
    }
  }
  updateFlashSubscription();
  update();
}

void CatalogueCellGridWidget::clearGrid() {
  grid_.reset();
  has_flashing_cells_ = false;
  updateFlashSubscription();
  update();
}

void CatalogueCellGridWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

void CatalogueCellGridWidget::setFlashClock(CatalogueFlashClock* clock) {
  if (flash_clock_ == clock) {
    return;
  }
  if (flash_subscribed_ && !flash_clock_.isNull()) {
    flash_clock_->release();
  }
  flash_subscribed_ = false;
  flash_clock_ = clock;
  if (clock != nullptr) {
    connect(clock, &CatalogueFlashClock::litChanged, this, [this](bool lit) {
      // A view that has released the clock is not animating, whatever the
      // clock goes on to do for the views that still are.
      if (flash_subscribed_) {
        setFlashLit(lit);
      }
    });
  }
  updateFlashSubscription();
}

void CatalogueCellGridWidget::setAnimationsEnabled(bool enabled) {
  if (animations_enabled_ == enabled) {
    return;
  }
  animations_enabled_ = enabled;
  updateFlashSubscription();
}

void CatalogueCellGridWidget::setFlashLit(bool lit) {
  if (flash_lit_ == lit) {
    return;
  }
  flash_lit_ = lit;
  repaintFlashingCells();
}

void CatalogueCellGridWidget::updateFlashSubscription() {
  const bool wanted = animations_enabled_ && has_flashing_cells_ &&
                      isVisible() && !flash_clock_.isNull();
  if (wanted == flash_subscribed_) {
    return;
  }
  flash_subscribed_ = wanted;
  if (wanted) {
    flash_clock_->acquire();
    setFlashLit(flash_clock_->lit());
    return;
  }
  if (!flash_clock_.isNull()) {
    flash_clock_->release();
  }
  setFlashLit(true);
}

void CatalogueCellGridWidget::repaintFlashingCells() {
  const PageGeometry geometry = pageGeometry();
  if (!grid_ || !grid_->valid() || !geometry.valid) {
    update();
    return;
  }
  const orc::CatalogueCellGrid& grid = *grid_;
  for (int row = 0; row < grid.rows; ++row) {
    for (int col = 0; col < grid.columns; ++col) {
      const orc::CatalogueCell& cell = grid.at(row, col);
      if (!cell_flash_is_visible(grid, cell)) {
        continue;
      }
      // A double-height character occupies the row below its origin as well.
      const qreal height = geometry.cell_h * (cell.double_height ? 2.0 : 1.0);
      const QRectF dirty(geometry.page.left() + col * geometry.cell_w,
                         geometry.page.top() + row * geometry.cell_h,
                         geometry.cell_w, height);
      update(dirty.toAlignedRect());
    }
  }
}

void CatalogueCellGridWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  updateFlashSubscription();
}

void CatalogueCellGridWidget::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  updateFlashSubscription();
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

CatalogueCellGridWidget::PageGeometry CatalogueCellGridWidget::pageGeometryIn(
    const QSizeF& bounds) const {
  PageGeometry geometry;
  if (!grid_ || !grid_->valid()) {
    return geometry;
  }

  const int rows = grid_->rows;
  const int columns = grid_->columns;
  const int aspect_w =
      grid_->cell_aspect_width > 0 ? grid_->cell_aspect_width : 1;
  const int aspect_h =
      grid_->cell_aspect_height > 0 ? grid_->cell_aspect_height : 1;

  // Fit the grid into the widget at the character-rectangle aspect ratio; a
  // stretched grid would misshape every glyph and mosaic block.
  const qreal page_aspect = static_cast<qreal>(columns * aspect_w) /
                            static_cast<qreal>(rows * aspect_h);
  qreal page_w = bounds.width();
  qreal page_h = page_w / page_aspect;
  if (page_h > bounds.height()) {
    page_h = bounds.height();
    page_w = page_h * page_aspect;
  }
  geometry.page = QRectF((bounds.width() - page_w) / 2.0,
                         (bounds.height() - page_h) / 2.0, page_w, page_h);
  geometry.cell_w = geometry.page.width() / columns;
  geometry.cell_h = geometry.page.height() / rows;
  geometry.valid = geometry.cell_w >= 1.0 && geometry.cell_h >= 1.0;
  return geometry;
}

void CatalogueCellGridWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  paintContent(painter, QRectF(rect()), flash_lit_);
}

QSize CatalogueCellGridWidget::pageImageSize() const {
  if (!grid_ || !grid_->valid()) {
    return {};
  }
  const int aspect_w = qMax(1, grid_->cell_aspect_width);
  const int aspect_h = qMax(1, grid_->cell_aspect_height);
  const int nominal_w = grid_->columns * aspect_w;
  const int nominal_h = grid_->rows * aspect_h;
  // Whole multiples of the character rectangle: at a fractional scale a cell
  // boundary falls inside a pixel and the columns stop lining up.
  int scale = 1;
  while (nominal_w * scale < kSavedPixelsAcross) {
    ++scale;
  }
  return QSize(nominal_w * scale, nominal_h * scale);
}

QImage CatalogueCellGridWidget::renderPageImage(const QSize& size) const {
  if (!grid_ || !grid_->valid() || size.isEmpty()) {
    return {};
  }
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::black);
  QPainter painter(&image);
  paintContent(painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)),
               /*lit=*/true);
  return image;
}

void CatalogueCellGridWidget::paintContent(QPainter& painter,
                                           const QRectF& bounds,
                                           bool lit) const {
  painter.fillRect(bounds, Qt::black);

  if (!grid_ || !grid_->valid()) {
    painter.setPen(Qt::gray);
    painter.drawText(bounds, Qt::AlignCenter, placeholder_);
    return;
  }

  const PageGeometry geometry = pageGeometryIn(bounds.size());
  if (!geometry.valid) {
    return;
  }

  const orc::CatalogueCellGrid& grid = *grid_;
  const int rows = grid.rows;
  const int columns = grid.columns;
  const QRectF& page_rect = geometry.page;
  const qreal cell_w = geometry.cell_w;
  const qreal cell_h = geometry.cell_h;

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

  // The fallback font, for the characters the character generator's face has
  // no glyph for. It is sized to fill the character rectangle vertically and
  // then squeezed if the font is wider than the cell, so neighbouring
  // characters never overlap. Double height reuses the glyph under a 2x
  // vertical scale: height doubles, width does not.
  QFont mono = cell_grid_font();
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
      // The blank phase of a flash leaves the background alone and draws no
      // foreground, which is what alternating the foreground pixels to the
      // background colour comes to.
      if (cell.flash && !lit) {
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

      // The character generator's own letterform, scaled from its character
      // rectangle to this cell's. Antialiasing is on for it because the cell
      // is rarely a whole number of sub-pixels across, and a sub-pixel that
      // lands between device pixels either thickens or vanishes without it.
      if (const QPainterPath* path = glyphPath(cell.character);
          path != nullptr) {
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(cell_rect.topLeft());
        painter.scale(cell_w / kTeletextRoundedColumns,
                      cell_h * height_scale / kTeletextRoundedRows);
        painter.fillPath(*path, palette_colour(grid, cell.foreground));
        painter.restore();
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

const QPainterPath* CatalogueCellGridWidget::glyphPath(
    char32_t character) const {
  const auto cached = glyph_paths_.find(character);
  if (cached != glyph_paths_.end()) {
    return cached->second.isEmpty() ? nullptr : &cached->second;
  }
  // An empty path is cached too: it is the answer for a character the face
  // does not hold, and looking that up again on every repaint is the same
  // work as looking up one it does.
  const auto inserted =
      glyph_paths_.emplace(character, teletext_glyph_path(character)).first;
  return inserted->second.isEmpty() ? nullptr : &inserted->second;
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
