/*
 * File:        nabtscanvaswidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget rasterising a NABTS/NAPLPS display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabtscanvaswidget.h"

#include <QBitmap>
#include <QBrush>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QString>
#include <algorithm>
#include <cmath>

namespace {

using orc::presenters::NabtsColourView;
using orc::presenters::NabtsLineTextureView;
using orc::presenters::NabtsPageView;
using orc::presenters::NabtsPointView;
using orc::presenters::NabtsPrimitiveKindView;
using orc::presenters::NabtsPrimitiveView;
using orc::presenters::NabtsTexturePatternView;

// Nominal resolution of the display area (T.101 Table II-3, X3.110 Table D1
// item 8), which is what the widget's size hint is built from. Its pixels are
// square: 1/256 of the unit screen across, and 0,78125/200 = 1/256 down.
constexpr int kNominalPixelsAcross = 256;
constexpr int kNominalPixelsDown = 200;

// A line whose logical pel is zero is the dimensionless drawing point of
// X3.110 §5.3.2.2.6; it still has to be visible, so it is drawn one device
// pixel wide.
constexpr qreal kMinimumPenWidth = 1.0;

QColor to_qcolor(const NabtsColourView& colour) {
  // §5.3.2.5's transparent colour shows the lower planes through. A viewer has
  // no video plane behind the page, so what shows through is nothing — black.
  if (colour.transparent) {
    return QColor(0, 0, 0);
  }
  return QColor(colour.red, colour.green, colour.blue);
}

Qt::PenStyle to_pen_style(NabtsLineTextureView texture) {
  switch (texture) {
    case NabtsLineTextureView::kSolid:
      return Qt::SolidLine;
    case NabtsLineTextureView::kDotted:
      return Qt::DotLine;
    case NabtsLineTextureView::kDashed:
      return Qt::DashLine;
    case NabtsLineTextureView::kDottedDashed:
      return Qt::DashDotLine;
  }
  return Qt::SolidLine;
}

/**
 * @brief The fill for a primitive's texture pattern (X3.110 §5.3.2.4.4)
 *
 * The four built-in patterns map onto Qt's hatch brushes directly. The four
 * programmable masks (§6.2.4) are 16 x 16 element bitmaps the record defined,
 * so they become a textured brush built from the mask; a mask the record never
 * defined falls back to solid, which is what §6.2.4 leaves an undefined mask
 * as.
 */
QBrush texture_brush(const NabtsPrimitiveView& primitive,
                     const NabtsPageView& page, const QColor& colour) {
  switch (primitive.texture_pattern) {
    case NabtsTexturePatternView::kSolid:
      return QBrush(colour, Qt::SolidPattern);
    case NabtsTexturePatternView::kVerticalHatch:
      return QBrush(colour, Qt::VerPattern);
    case NabtsTexturePatternView::kHorizontalHatch:
      return QBrush(colour, Qt::HorPattern);
    case NabtsTexturePatternView::kCrossHatch:
      return QBrush(colour, Qt::CrossPattern);
    default:
      break;
  }

  const size_t slot = static_cast<size_t>(primitive.texture_pattern) -
                      static_cast<size_t>(NabtsTexturePatternView::kMaskA);
  if (slot >= page.texture_masks.size() ||
      !page.texture_masks[slot].defined()) {
    return QBrush(colour, Qt::SolidPattern);
  }

  const auto& mask = page.texture_masks[slot];
  QImage tile(mask.width, mask.height, QImage::Format_ARGB32_Premultiplied);
  tile.fill(Qt::transparent);
  for (int row = 0; row < mask.height; ++row) {
    // Row 0 of the buffer is its bottom, matching unit space; a QImage's row 0
    // is its top.
    const int image_row = mask.height - 1 - row;
    for (int column = 0; column < mask.width; ++column) {
      if (mask.elements[static_cast<size_t>(row) * mask.width + column]) {
        tile.setPixelColor(column, image_row, colour);
      }
    }
  }
  return QBrush(tile);
}

/// A quadratic Bezier whose curve passes through |through| at its midpoint,
/// which is the circular arc of X3.110 §5.3.3.3 to within a pixel at this size.
QPainterPath arc_through(const QPointF& start, const QPointF& through,
                         const QPointF& end) {
  const QPointF control = 2.0 * through - (start + end) / 2.0;
  QPainterPath path(start);
  path.quadTo(control, end);
  return path;
}

/// A polyline through every control point, for the spline forms of §5.3.3.3
/// (four points or more). Chained quadratics through consecutive triples, which
/// keeps the curve on its points.
QPainterPath spline_through(const QVector<QPointF>& points) {
  QPainterPath path(points.front());
  for (int i = 1; i + 1 < points.size(); i += 2) {
    const QPointF& through = points[i];
    const QPointF& end = points[i + 1];
    const QPointF control =
        2.0 * through - (path.currentPosition() + end) / 2.0;
    path.quadTo(control, end);
  }
  // An even number of points leaves one unvisited; a straight run to it is
  // better than dropping a vertex the record transmitted.
  if (points.size() % 2 == 0) {
    path.lineTo(points.back());
  }
  return path;
}

/// The six sub-elements of a 2x3 mosaic cell (X3.110 §5.4, Figure 62): bit 0
/// top-left through bit 5 bottom-right. In separated mode each lit element is
/// shrunk by the logical pel and left-and-bottom justified within its area.
void paint_mosaic(QPainter& painter, const QRectF& cell, uint8_t pattern,
                  bool separated, qreal inset_x, qreal inset_y,
                  const QColor& colour) {
  const qreal element_w = cell.width() / 2.0;
  const qreal element_h = cell.height() / 3.0;
  for (int element = 0; element < 6; ++element) {
    if ((pattern & (1 << element)) == 0) {
      continue;
    }
    const int column = element % 2;
    const int row = element / 2;
    QRectF rect(cell.left() + column * element_w, cell.top() + row * element_h,
                element_w, element_h);
    if (separated) {
      // §5.4: reduced in each dimension by the logical pel, left and bottom
      // justified — so the gap appears on the right and the top.
      rect.setWidth(std::max<qreal>(0.0, rect.width() - inset_x));
      const qreal height = std::max<qreal>(0.0, rect.height() - inset_y);
      rect.setTop(rect.bottom() - height);
    }
    if (rect.width() > 0.0 && rect.height() > 0.0) {
      painter.fillRect(rect, colour);
    }
  }
}

}  // namespace

NabtsCanvasWidget::NabtsCanvasWidget(QWidget* parent) : QWidget(parent) {
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
}

void NabtsCanvasWidget::setPage(const NabtsPageView& page) {
  page_ = page;
  update();
}

void NabtsCanvasWidget::clearPage() {
  page_.reset();
  primitives_painted_ = 0;
  update();
}

void NabtsCanvasWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

QSize NabtsCanvasWidget::sizeHint() const {
  return QSize(kNominalPixelsAcross, kNominalPixelsDown);
}

QRectF NabtsCanvasWidget::displayAreaRect() const {
  const double aspect_height =
      page_ ? page_->display_area_height
            : orc::presenters::kNabtsDisplayAreaHeightView;
  const qreal available_w = static_cast<qreal>(width());
  const qreal available_h = static_cast<qreal>(height());
  qreal draw_w = available_w;
  qreal draw_h = draw_w * aspect_height;
  if (draw_h > available_h) {
    draw_h = available_h;
    draw_w = aspect_height > 0.0 ? draw_h / aspect_height : available_w;
  }
  return QRectF((available_w - draw_w) / 2.0, (available_h - draw_h) / 2.0,
                draw_w, draw_h);
}

void NabtsCanvasWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  primitives_painted_ = 0;

  if (!page_) {
    painter.setPen(Qt::gray);
    painter.drawText(rect(), Qt::AlignCenter, tr("No record"));
    return;
  }

  const QRectF area = displayAreaRect();
  // The mapping is isotropic: the display area is |unit| pixels per unit of x,
  // and the same per unit of y, because its nominal pixels are square.
  const qreal unit = area.width();

  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.save();
  // Nothing outside the display area is guaranteed visible on a receiver
  // (Table D1 item 10), so nothing outside it is drawn here either.
  painter.setClipRect(area);

  for (const auto& primitive : page_->primitives) {
    paintPrimitive(painter, *page_, primitive, area, unit);
    ++primitives_painted_;
  }
  painter.restore();

  if (show_data_errors_) {
    // The display area's edge, so a record drawn entirely in one corner is
    // visibly in a corner rather than looking mis-scaled.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(255, 96, 96), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area.adjusted(0, 0, -1, -1));
  }
}

void NabtsCanvasWidget::paintPrimitive(QPainter& painter,
                                       const NabtsPageView& page,
                                       const NabtsPrimitiveView& primitive,
                                       const QRectF& area, qreal unit) {
  // Unit space has y upwards from the bottom left; the widget's y runs down.
  const auto map = [&area, unit](const NabtsPointView& point) {
    return QPointF(area.left() + point.x * unit,
                   area.bottom() - point.y * unit);
  };

  const QColor colour = to_qcolor(primitive.colour);
  const QColor background = primitive.has_background
                                ? to_qcolor(primitive.background)
                                : QColor(0, 0, 0);

  // §5.3.2.2.6: the logical pel is what gives a line its width. Both dimensions
  // map to one pen, so the larger governs — a pen has no orientation.
  const qreal pel_w = std::fabs(primitive.logical_pel.dx) * unit;
  const qreal pel_h = std::fabs(primitive.logical_pel.dy) * unit;
  const qreal pen_width = std::max({pel_w, pel_h, kMinimumPenWidth});

  QPen pen(colour);
  pen.setWidthF(pen_width);
  pen.setStyle(to_pen_style(primitive.line_texture));
  pen.setCapStyle(Qt::FlatCap);
  pen.setJoinStyle(Qt::MiterJoin);

  QVector<QPointF> points;
  points.reserve(static_cast<int>(primitive.points.size()));
  for (const auto& point : primitive.points) {
    points.push_back(map(point));
  }

  // §5.3.2.4.3: a highlighted figure is filled as usual and outlined in nominal
  // black, or in the background colour where the colour mode has one.
  const auto outline_pen = [&]() {
    QPen outline(primitive.has_background ? background : QColor(0, 0, 0));
    outline.setWidthF(std::max(pen_width, kMinimumPenWidth));
    return outline;
  };

  switch (primitive.kind) {
    case NabtsPrimitiveKindView::kPoint: {
      if (points.isEmpty()) {
        return;
      }
      // A visible point is the logical pel itself (§5.3.3.1), so it is drawn as
      // that rectangle rather than as a dot of arbitrary size.
      const QRectF pel(points.front().x(),
                       points.front().y() - std::max(pel_h, kMinimumPenWidth),
                       std::max(pel_w, kMinimumPenWidth),
                       std::max(pel_h, kMinimumPenWidth));
      painter.fillRect(pel, colour);
      return;
    }

    case NabtsPrimitiveKindView::kLine: {
      if (points.size() < 2) {
        return;
      }
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPolyline(QPolygonF(points));
      return;
    }

    case NabtsPrimitiveKindView::kArc: {
      if (points.size() < 2) {
        return;
      }
      QPainterPath path = points.size() == 3
                              ? arc_through(points[0], points[1], points[2])
                              : (points.size() < 3 ? QPainterPath(points[0])
                                                   : spline_through(points));
      if (points.size() == 2) {
        path.lineTo(points[1]);
      }
      if (primitive.filled) {
        path.closeSubpath();
        painter.setPen(primitive.highlighted ? outline_pen() : QPen(Qt::NoPen));
        painter.setBrush(texture_brush(primitive, page, colour));
      } else {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
      }
      painter.drawPath(path);
      return;
    }

    case NabtsPrimitiveKindView::kRectangle: {
      // A negative extent is a rectangle drawn back from its origin
      // (§5.3.3.4), which normalized() resolves.
      const QPointF origin = map(primitive.origin);
      const QPointF far(origin.x() + primitive.size.dx * unit,
                        origin.y() - primitive.size.dy * unit);
      const QRectF box = QRectF(origin, far).normalized();
      if (primitive.filled) {
        painter.setPen(primitive.highlighted ? outline_pen() : QPen(Qt::NoPen));
        painter.setBrush(texture_brush(primitive, page, colour));
      } else {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
      }
      painter.drawRect(box);
      return;
    }

    case NabtsPrimitiveKindView::kPolygon: {
      if (points.size() < 2) {
        return;
      }
      const QPolygonF polygon(points);
      if (primitive.filled) {
        painter.setPen(primitive.highlighted ? outline_pen() : QPen(Qt::NoPen));
        painter.setBrush(texture_brush(primitive, page, colour));
        painter.drawPolygon(polygon);
      } else {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(polygon);
      }
      return;
    }

    case NabtsPrimitiveKindView::kIncrementalPoints: {
      // §5.3.3.6.3: colours deposited raster-sequentially across the active
      // field, one logical pel apiece, wrapping at the field's right edge.
      if (primitive.incremental_colours.empty()) {
        return;
      }
      const qreal step_x = std::max(pel_w, kMinimumPenWidth);
      const qreal step_y = std::max(pel_h, kMinimumPenWidth);
      const qreal field_w = std::fabs(primitive.size.dx) * unit;
      const int columns =
          std::max(1, static_cast<int>(std::floor(field_w / step_x)));
      const QPointF origin = map(primitive.origin);
      // The field's origin is its lower left, and a raster runs top down.
      const qreal top = origin.y() - std::fabs(primitive.size.dy) * unit;
      for (int i = 0;
           i < static_cast<int>(primitive.incremental_colours.size()); ++i) {
        const int column = i % columns;
        const int row = i / columns;
        const QRectF pel(origin.x() + column * step_x, top + row * step_y,
                         step_x, step_y);
        painter.fillRect(
            pel,
            to_qcolor(primitive.incremental_colours[static_cast<size_t>(i)]));
      }
      return;
    }

    case NabtsPrimitiveKindView::kText: {
      if (primitive.text.empty()) {
        return;
      }
      const qreal field_w = std::fabs(primitive.size.dx) * unit;
      const qreal field_h = std::fabs(primitive.size.dy) * unit;
      if (field_w <= 0.0 || field_h <= 0.0) {
        return;
      }

      const QPointF origin = map(primitive.origin);
      painter.save();
      // §5.3.2.3.2: rotation is counterclockwise about the character field
      // origin, and the widget's y is inverted, so the sign flips.
      if (primitive.rotation_degrees != 0) {
        painter.translate(origin);
        painter.rotate(-primitive.rotation_degrees);
        painter.translate(-origin);
      }

      QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
      font.setPixelSize(std::max(1, static_cast<int>(field_h * 0.9)));
      font.setUnderline(primitive.underlined);
      painter.setFont(font);

      const QString text = QString::fromUtf8(primitive.text.c_str());
      const qreal advance_x = primitive.advance.dx * unit;
      const qreal advance_y = -primitive.advance.dy * unit;

      // One character field at a time: the record placed the characters on a
      // fixed pitch, and letting the font's own advances accumulate would drift
      // away from where it put them. A cell is a base character plus whatever
      // combining marks follow it, which §7.1 makes one character of the
      // repertoire and so one field.
      QStringList cells;
      QString cell;
      for (const QChar& character : text) {
        if (!cell.isEmpty() && character.category() != QChar::Mark_NonSpacing) {
          cells << cell;
          cell.clear();
        }
        cell.append(character);
      }
      if (!cell.isEmpty()) {
        cells << cell;
      }

      int index = 0;
      for (const QString& glyph : cells) {
        const QRectF field(origin.x() + index * advance_x,
                           origin.y() + index * advance_y - field_h, field_w,
                           field_h);
        if (primitive.reverse_video) {
          // §6.2.7.4: the field is filled and the character shape left undrawn,
          // except in colour mode 2 where it is drawn in the background colour.
          painter.fillRect(field, colour);
          painter.setPen(primitive.has_background ? background
                                                  : QColor(0, 0, 0));
        } else {
          if (primitive.has_background) {
            painter.fillRect(field, background);
          }
          painter.setPen(colour);
        }
        painter.drawText(field, Qt::AlignLeft | Qt::AlignVCenter, glyph);
        ++index;
      }
      painter.restore();
      return;
    }

    case NabtsPrimitiveKindView::kMosaic: {
      const QPointF origin = map(primitive.origin);
      const qreal field_w = std::fabs(primitive.size.dx) * unit;
      const qreal field_h = std::fabs(primitive.size.dy) * unit;
      const QRectF field(origin.x(), origin.y() - field_h, field_w, field_h);
      if (primitive.has_background) {
        painter.fillRect(field, background);
      }
      paint_mosaic(painter, field, primitive.mosaic_pattern,
                   primitive.mosaic_separated, pel_w, pel_h, colour);
      return;
    }

    case NabtsPrimitiveKindView::kDrcs: {
      const QPointF origin = map(primitive.origin);
      const qreal field_w = std::fabs(primitive.size.dx) * unit;
      const qreal field_h = std::fabs(primitive.size.dy) * unit;
      const QRectF field(origin.x(), origin.y() - field_h, field_w, field_h);
      if (primitive.has_background) {
        painter.fillRect(field, background);
      }
      if (primitive.drcs_index < 0 ||
          primitive.drcs_index >= static_cast<int>(page.drcs.size())) {
        return;  // §5.6: a character never defined is displayed as SPACE
      }
      const auto& glyph = page.drcs[static_cast<size_t>(primitive.drcs_index)];
      if (!glyph.defined()) {
        return;
      }
      const qreal cell_w = field_w / glyph.width;
      const qreal cell_h = field_h / glyph.height;
      for (int row = 0; row < glyph.height; ++row) {
        // Row 0 of the buffer is its bottom, matching unit space.
        const qreal top = field.bottom() - (row + 1) * cell_h;
        for (int column = 0; column < glyph.width; ++column) {
          if (glyph.elements[static_cast<size_t>(row) * glyph.width + column]) {
            painter.fillRect(
                QRectF(field.left() + column * cell_w, top, cell_w, cell_h),
                colour);
          }
        }
      }
      return;
    }
  }
}
