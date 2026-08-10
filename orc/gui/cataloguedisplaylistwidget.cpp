/*
 * File:        cataloguedisplaylistwidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget rasterising a CataloguePayload 2D display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cataloguedisplaylistwidget.h"

#include <QBrush>
#include <QFont>
#include <QFontDatabase>
#include <QHideEvent>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "catalogue_flash_clock.h"

namespace {

using orc::CatalogueColour;
using orc::CatalogueDisplayList;
using orc::CatalogueDrawKind;
using orc::CatalogueDrawOp;
using orc::CatalogueFillPattern;
using orc::CatalogueLineStyle;
using orc::CataloguePoint;

// Nominal size of the drawable area, which is what the widget's size hint is
// built from. The height follows the payload's own aspect at paint time; this
// is only a starting shape.
constexpr int kNominalPixelsAcross = 256;
constexpr int kNominalPixelsDown = 200;

// A line whose pen is zero is a dimensionless drawing point; it still has to be
// visible, so it is drawn one device pixel wide.
constexpr qreal kMinimumPenWidth = 1.0;

QColor to_qcolor(const CatalogueColour& colour) {
  // A transparent colour shows the lower planes through. A viewer has no video
  // plane behind the page, so what shows through is nothing — black.
  if (colour.transparent) {
    return QColor(0, 0, 0);
  }
  return QColor(colour.red, colour.green, colour.blue);
}

Qt::PenStyle to_pen_style(CatalogueLineStyle style) {
  switch (style) {
    case CatalogueLineStyle::kSolid:
      return Qt::SolidLine;
    case CatalogueLineStyle::kDotted:
      return Qt::DotLine;
    case CatalogueLineStyle::kDashed:
      return Qt::DashLine;
    case CatalogueLineStyle::kDashDotted:
      return Qt::DashDotLine;
  }
  return Qt::SolidLine;
}

/**
 * @brief The fill for an operation's pattern
 *
 * The four built-in patterns map onto Qt's hatch brushes directly. The four
 * programmable masks are element bitmaps the source defined, so they become a
 * textured brush built from the mask; a mask that was never defined falls back
 * to solid, which is what an undefined mask leaves.
 */
QBrush texture_brush(const CatalogueDrawOp& op,
                     const CatalogueDisplayList& list, const QColor& colour) {
  switch (op.fill_pattern) {
    case CatalogueFillPattern::kSolid:
      return QBrush(colour, Qt::SolidPattern);
    case CatalogueFillPattern::kVerticalHatch:
      return QBrush(colour, Qt::VerPattern);
    case CatalogueFillPattern::kHorizontalHatch:
      return QBrush(colour, Qt::HorPattern);
    case CatalogueFillPattern::kCrossHatch:
      return QBrush(colour, Qt::CrossPattern);
    default:
      break;
  }

  const size_t slot = static_cast<size_t>(op.fill_pattern) -
                      static_cast<size_t>(CatalogueFillPattern::kMask0);
  if (slot >= list.fill_masks.size() || !list.fill_masks[slot].defined()) {
    return QBrush(colour, Qt::SolidPattern);
  }

  const auto& mask = list.fill_masks[slot];
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

// Two points closer than this in device pixels are the same point — which is
// what makes an arc a circle — and a circumcircle determinant smaller than this
// means three colinear points.
constexpr double kCoincidentPixels = 1e-6;
constexpr double kColinearDeterminant = 1e-12;

constexpr double kPi = 3.14159265358979323846;

double distance_between(const QPointF& a, const QPointF& b) {
  return std::hypot(a.x() - b.x(), a.y() - b.y());
}

/// The angle of |point| about |centre| in the degrees QPainterPath::arcTo uses:
/// zero at three o'clock, increasing counter-clockwise on screen. The y term is
/// negated because the widget's y runs downwards and unit space's does not.
double arc_angle(const QPointF& centre, const QPointF& point) {
  return std::atan2(centre.y() - point.y(), point.x() - centre.x()) * 180.0 /
         kPi;
}

double normalised_degrees(double angle) {
  double out = std::fmod(angle, 360.0);
  if (out < 0.0) {
    out += 360.0;
  }
  return out;
}

/**
 * @brief A circular arc through three points
 *
 * An arc runs from a start point to an end point through an intermediate point
 * on it, plus the two degenerate readings: a circle when the start and end are
 * coincident, with the intermediate point defining the diameter, and a straight
 * line when the three points are colinear.
 *
 * A real circle rather than a curve fitted to the three points: a quadratic
 * through them is visibly wrong at any useful radius, and on a circle — where
 * the start and end coincide — it collapses to nothing at all.
 */
QPainterPath arc_through(const QPointF& start, const QPointF& through,
                         const QPointF& end) {
  QPainterPath path(start);

  if (distance_between(start, end) < kCoincidentPixels) {
    // A circle: the intermediate point is diametrically opposite the start.
    const QPointF centre((start.x() + through.x()) / 2.0,
                         (start.y() + through.y()) / 2.0);
    const double radius = distance_between(start, through) / 2.0;
    if (radius < kCoincidentPixels) {
      return path;
    }
    const QRectF box(centre.x() - radius, centre.y() - radius, radius * 2.0,
                     radius * 2.0);
    path.arcMoveTo(box, arc_angle(centre, start));
    path.arcTo(box, arc_angle(centre, start), 360.0);
    return path;
  }

  // The circumcircle of the three points; a zero determinant means they are
  // colinear.
  const double ax = start.x();
  const double ay = start.y();
  const double bx = through.x();
  const double by = through.y();
  const double cx = end.x();
  const double cy = end.y();
  const double determinant =
      2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::fabs(determinant) < kColinearDeterminant) {
    path.lineTo(end);
    return path;
  }

  const double a2 = ax * ax + ay * ay;
  const double b2 = bx * bx + by * by;
  const double c2 = cx * cx + cy * cy;
  const QPointF centre(
      (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / determinant,
      (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / determinant);
  const double radius = distance_between(start, centre);
  const QRectF box(centre.x() - radius, centre.y() - radius, radius * 2.0,
                   radius * 2.0);

  const double from = arc_angle(centre, start);
  const double via = arc_angle(centre, through);
  const double to = arc_angle(centre, end);
  // Sweep the way round that passes through the intermediate point, which is
  // what picks the major arc from the minor one.
  const double counter_clockwise = normalised_degrees(to - from);
  const double sweep = normalised_degrees(via - from) <= counter_clockwise
                           ? counter_clockwise
                           : counter_clockwise - 360.0;
  path.arcMoveTo(box, from);
  path.arcTo(box, from, sweep);
  return path;
}

/// A curve through every control point, for the spline form (four points or
/// more). Chained quadratics through consecutive triples, which keeps the curve
/// on its points.
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
  // better than dropping a vertex the source transmitted.
  if (points.size() % 2 == 0) {
    path.lineTo(points.back());
  }
  return path;
}

/// The six sub-elements of a 2x3 mosaic cell: bit 0 top-left through bit 5
/// bottom-right. In separated mode each lit element is shrunk by the pen and
/// left-and-bottom justified within its area.
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
      // Reduced in each dimension by the pen, left and bottom justified — so
      // the gap appears on the right and the top.
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

CatalogueDisplayListWidget::CatalogueDisplayListWidget(QWidget* parent)
    : QWidget(parent), placeholder_(tr("Nothing to show")) {
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
}

CatalogueDisplayListWidget::~CatalogueDisplayListWidget() {
  if (flash_subscribed_ && !flash_clock_.isNull()) {
    flash_clock_->release();
  }
}

void CatalogueDisplayListWidget::setDisplayList(
    const orc::CatalogueDisplayList& list) {
  list_ = list;
  has_blinking_ops_ =
      std::any_of(list_->ops.begin(), list_->ops.end(),
                  [](const CatalogueDrawOp& op) { return op.blinking; });
  updateFlashSubscription();
  update();
}

void CatalogueDisplayListWidget::clearDisplayList() {
  list_.reset();
  ops_painted_ = 0;
  has_blinking_ops_ = false;
  updateFlashSubscription();
  update();
}

void CatalogueDisplayListWidget::setFlashClock(CatalogueFlashClock* clock) {
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

void CatalogueDisplayListWidget::setAnimationsEnabled(bool enabled) {
  if (animations_enabled_ == enabled) {
    return;
  }
  animations_enabled_ = enabled;
  updateFlashSubscription();
}

void CatalogueDisplayListWidget::setFlashLit(bool lit) {
  if (flash_lit_ == lit) {
    return;
  }
  flash_lit_ = lit;
  // A display list places its operations anywhere in the drawable area and
  // they overlap freely, so there is no dirty rectangle worth deriving: the
  // area is repainted whole.
  update();
}

void CatalogueDisplayListWidget::updateFlashSubscription() {
  const bool wanted = animations_enabled_ && has_blinking_ops_ && isVisible() &&
                      !flash_clock_.isNull();
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

void CatalogueDisplayListWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  updateFlashSubscription();
}

void CatalogueDisplayListWidget::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  updateFlashSubscription();
}

void CatalogueDisplayListWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

void CatalogueDisplayListWidget::setPlaceholderText(const QString& text) {
  placeholder_ = text;
  update();
}

QSize CatalogueDisplayListWidget::sizeHint() const {
  return QSize(kNominalPixelsAcross, kNominalPixelsDown);
}

QRectF CatalogueDisplayListWidget::displayAreaRect() const {
  const double aspect_height =
      list_ && list_->aspect_height > 0.0 ? list_->aspect_height : 1.0;
  const qreal available_w = static_cast<qreal>(width());
  const qreal available_h = static_cast<qreal>(height());
  qreal draw_w = available_w;
  qreal draw_h = draw_w * aspect_height;
  if (draw_h > available_h) {
    draw_h = available_h;
    draw_w = draw_h / aspect_height;
  }
  return QRectF((available_w - draw_w) / 2.0, (available_h - draw_h) / 2.0,
                draw_w, draw_h);
}

void CatalogueDisplayListWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.fillRect(rect(), Qt::black);
  ops_painted_ = 0;

  if (!list_) {
    painter.setPen(Qt::gray);
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  const QRectF area = displayAreaRect();
  // The mapping is isotropic: the drawable area is |unit| pixels per unit of x,
  // and the same per unit of y, because its nominal pixels are square.
  const qreal unit = area.width();

  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.save();
  // Nothing outside the drawable area is guaranteed visible on a receiver, so
  // nothing outside it is drawn here either.
  painter.setClipRect(area);

  for (const auto& op : list_->ops) {
    paintOp(painter, *list_, op, area, unit, flash_lit_);
    ++ops_painted_;
  }
  painter.restore();

  if (show_data_errors_) {
    // The drawable area's edge, so a list drawn entirely in one corner is
    // visibly in a corner rather than looking mis-scaled.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(QColor(255, 96, 96), 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area.adjusted(0, 0, -1, -1));
  }
}

void CatalogueDisplayListWidget::paintOp(QPainter& painter,
                                         const CatalogueDisplayList& list,
                                         const CatalogueDrawOp& op,
                                         const QRectF& area, qreal unit,
                                         bool lit) {
  // Unit space has y upwards from the bottom left; the widget's y runs down.
  const auto map = [&area, unit](const CataloguePoint& point) {
    return QPointF(area.left() + point.x * unit,
                   area.bottom() - point.y * unit);
  };

  const QColor background =
      op.has_background ? to_qcolor(op.background) : QColor(0, 0, 0);
  // The other phase of a blink process draws the operation in its blink-to
  // colour, which the service names and which is not always a ground colour —
  // a figure alternating with a second colour twinkles rather than flashes.
  // Substituting the drawing colour rather than dropping the operation is what
  // makes that possible, and what keeps a blinking character's field, and
  // anything drawn under it, on screen through the phase.
  const bool blank = op.blinking && !lit;
  const QColor colour = blank ? to_qcolor(op.blink_to) : to_qcolor(op.colour);

  // The pen is what gives a line its width. Both dimensions map to one pen, so
  // the larger governs — a pen has no orientation.
  const qreal pel_w = std::fabs(op.pen_size.dx) * unit;
  const qreal pel_h = std::fabs(op.pen_size.dy) * unit;
  const qreal pen_width = std::max({pel_w, pel_h, kMinimumPenWidth});

  QPen pen(colour);
  pen.setWidthF(pen_width);
  pen.setStyle(to_pen_style(op.line_style));
  pen.setCapStyle(Qt::FlatCap);
  pen.setJoinStyle(Qt::MiterJoin);

  QVector<QPointF> points;
  points.reserve(static_cast<int>(op.points.size()));
  for (const auto& point : op.points) {
    points.push_back(map(point));
  }

  // A highlighted figure is filled as usual and outlined in nominal black, or
  // in the background colour where the colour mode has one.
  const auto outline_pen = [&]() {
    QPen outline(op.has_background ? background : QColor(0, 0, 0));
    outline.setWidthF(std::max(pen_width, kMinimumPenWidth));
    return outline;
  };

  /**
   * The pen that traces a filled figure's own outline.
   *
   * X3.110 §5.3.3.3, §5.3.3.4.1, §5.3.3.5.1 and §5.3.3.6.5 all put "the region
   * of the outline traced by the logical pel" *inside* the filled area, and
   * §5.3.2.4.3 defines that region as what the pel sweeps along the outline. A
   * filled figure is therefore its enclosed area plus a stroke of the pel, in
   * the same colour and texture — not the enclosed area alone.
   *
   * That is not a refinement at the edges. A service draws a letterform or a
   * thin mark as a path that encloses almost nothing and lets the pel give it
   * its weight: the NCAA roundel of the reference ExtraVision recording draws
   * each letter this way, and filling the enclosed area alone leaves a few
   * disconnected slivers where the letter should be.
   *
   * A dimensionless pel (the default 0,0 of §5.3.2.2.6) traces nothing, so it
   * gets no stroke — unlike an outlined figure, which needs a visible minimum
   * because the stroke is all there is of it.
   */
  const qreal traced_width = std::max(pel_w, pel_h);
  const auto fill_outline_pen = [&](const QBrush& brush) {
    if (traced_width <= 0.0) {
      return QPen(Qt::NoPen);
    }
    QPen traced(brush, traced_width);
    // Solid whatever the line texture: this region is part of the fill, not a
    // textured line.
    traced.setStyle(Qt::SolidLine);
    traced.setCapStyle(Qt::FlatCap);
    traced.setJoinStyle(Qt::MiterJoin);
    return traced;
  };

  switch (op.kind) {
    case CatalogueDrawKind::kPoint: {
      if (points.isEmpty()) {
        return;
      }
      // A visible point is the pen itself, so it is drawn as that rectangle
      // rather than as a dot of arbitrary size.
      const QRectF pel(points.front().x(),
                       points.front().y() - std::max(pel_h, kMinimumPenWidth),
                       std::max(pel_w, kMinimumPenWidth),
                       std::max(pel_h, kMinimumPenWidth));
      painter.fillRect(pel, colour);
      return;
    }

    case CatalogueDrawKind::kLine: {
      if (points.size() < 2) {
        return;
      }
      painter.setPen(pen);
      painter.setBrush(Qt::NoBrush);
      painter.drawPolyline(QPolygonF(points));
      return;
    }

    case CatalogueDrawKind::kArc: {
      if (points.size() < 2) {
        return;
      }
      // Three points are a circle or a segment of one, and more than three a
      // spline. Two is what a truncated source leaves behind, and a line
      // through them is the most that can be said of it.
      QPainterPath path;
      if (points.size() == 3) {
        path = arc_through(points[0], points[1], points[2]);
      } else if (points.size() > 3) {
        path = spline_through(points);
      } else {
        path = QPainterPath(points[0]);
        path.lineTo(points[1]);
      }
      if (!op.filled) {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        return;
      }

      // §5.3.3.3: "the area enclosed by the outline and the chord (including
      // the region of the outline and the chord traced by the logical pel)".
      QPainterPath closed = path;
      closed.closeSubpath();
      const QBrush brush = texture_brush(op, list, colour);
      painter.setBrush(brush);
      painter.setPen(fill_outline_pen(brush));
      painter.drawPath(closed);

      if (op.outlined) {
        // §5.3.2.4.3's highlight, over the fill. §5.3.3.3 keeps the chord out
        // of it — "the chord is not considered a part of the arc and, as such,
        // is not highlighted" — so the open arc is stroked, not the closure.
        painter.setBrush(Qt::NoBrush);
        painter.setPen(outline_pen());
        painter.drawPath(path);
      }
      return;
    }

    case CatalogueDrawKind::kRectangle: {
      // A negative extent is a rectangle drawn back from its origin, which
      // normalized() resolves.
      const QPointF origin = map(op.origin);
      const QPointF far(origin.x() + op.size.dx * unit,
                        origin.y() - op.size.dy * unit);
      const QRectF box = QRectF(origin, far).normalized();
      if (op.filled) {
        const QBrush brush = texture_brush(op, list, colour);
        painter.setPen(op.outlined ? outline_pen() : fill_outline_pen(brush));
        painter.setBrush(brush);
      } else {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
      }
      painter.drawRect(box);
      return;
    }

    case CatalogueDrawKind::kPolygon: {
      if (points.size() < 2) {
        return;
      }
      const QPolygonF polygon(points);
      if (op.filled) {
        const QBrush brush = texture_brush(op, list, colour);
        painter.setPen(op.outlined ? outline_pen() : fill_outline_pen(brush));
        painter.setBrush(brush);
        painter.drawPolygon(polygon);
      } else {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(polygon);
      }
      return;
    }

    case CatalogueDrawKind::kColourRun: {
      // Colours deposited raster-sequentially across the field, one pen
      // apiece, wrapping at the field's right edge.
      if (op.colour_run.empty()) {
        return;
      }
      const qreal step_x = std::max(pel_w, kMinimumPenWidth);
      const qreal step_y = std::max(pel_h, kMinimumPenWidth);
      const qreal field_w = std::fabs(op.size.dx) * unit;
      const int columns =
          std::max(1, static_cast<int>(std::floor(field_w / step_x)));
      const QPointF origin = map(op.origin);
      // The field's origin is its lower left, and a raster runs top down.
      const qreal top = origin.y() - std::fabs(op.size.dy) * unit;
      for (int i = 0; i < static_cast<int>(op.colour_run.size()); ++i) {
        const int column = i % columns;
        const int row = i / columns;
        const QRectF pel(origin.x() + column * step_x, top + row * step_y,
                         step_x, step_y);
        // A run carries its own colour per pel, so the blank phase of a blink
        // has to be applied to each of them rather than to |colour|.
        painter.fillRect(
            pel,
            blank ? colour : to_qcolor(op.colour_run[static_cast<size_t>(i)]));
      }
      return;
    }

    case CatalogueDrawKind::kText: {
      if (op.text.empty()) {
        return;
      }
      const qreal field_w = std::fabs(op.size.dx) * unit;
      const qreal field_h = std::fabs(op.size.dy) * unit;
      if (field_w <= 0.0 || field_h <= 0.0) {
        return;
      }

      const QPointF origin = map(op.origin);
      painter.save();
      // Rotation is counterclockwise about the character field origin, and the
      // widget's y is inverted, so the sign flips.
      if (op.rotation_degrees != 0) {
        painter.translate(origin);
        painter.rotate(-op.rotation_degrees);
        painter.translate(-origin);
      }

      QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
      font.setPixelSize(std::max(1, static_cast<int>(field_h * 0.9)));
      font.setUnderline(op.underlined);
      painter.setFont(font);

      const QString text = QString::fromUtf8(op.text.c_str());
      const qreal advance_x = op.advance.dx * unit;
      const qreal advance_y = -op.advance.dy * unit;

      // One character field at a time: the source placed the characters on a
      // fixed pitch, and letting the font's own advances accumulate would drift
      // away from where it put them. A cell is a base character plus whatever
      // combining marks follow it, which is one character of the repertoire and
      // so one field.
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
        if (op.reverse_video) {
          // The field is filled and the character shape left undrawn, except
          // where a background colour gives it something to be drawn in.
          painter.fillRect(field, colour);
          painter.setPen(op.has_background ? background : QColor(0, 0, 0));
        } else {
          if (op.has_background) {
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

    case CatalogueDrawKind::kMosaic: {
      const QPointF origin = map(op.origin);
      const qreal field_w = std::fabs(op.size.dx) * unit;
      const qreal field_h = std::fabs(op.size.dy) * unit;
      const QRectF field(origin.x(), origin.y() - field_h, field_w, field_h);
      if (op.has_background) {
        painter.fillRect(field, background);
      }
      paint_mosaic(painter, field, op.mosaic_pattern, op.mosaic_separated,
                   pel_w, pel_h, colour);
      return;
    }

    case CatalogueDrawKind::kGlyph: {
      const QPointF origin = map(op.origin);
      const qreal field_w = std::fabs(op.size.dx) * unit;
      const qreal field_h = std::fabs(op.size.dy) * unit;
      const QRectF field(origin.x(), origin.y() - field_h, field_w, field_h);
      if (op.has_background) {
        painter.fillRect(field, background);
      }
      if (op.glyph_index < 0 ||
          op.glyph_index >= static_cast<int>(list.glyphs.size())) {
        return;  // a character never defined is displayed as SPACE
      }
      const auto& glyph = list.glyphs[static_cast<size_t>(op.glyph_index)];
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
