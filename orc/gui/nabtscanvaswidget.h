/*
 * File:        nabtscanvaswidget.h
 * Module:      orc-gui
 * Purpose:     Widget rasterising a NABTS/NAPLPS display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef NABTSCANVASWIDGET_H
#define NABTSCANVASWIDGET_H

#include <orc_nabts.h>

#include <QRectF>
#include <QWidget>
#include <optional>

class QPainter;

/**
 * @brief Rasterises a decoded NABTS presentation record
 *
 * A NAPLPS record is a drawing program, not a character grid (CEA-516 §6.1,
 * ANSI X3.110-1983), so this walks the display list the presenter resolved and
 * paints each primitive with the attributes it carries. Nothing is decoded here
 * and no state is carried between primitives — that is the point of the display
 * list.
 *
 * **Geometry.** Coordinates arrive as unit-space fractions, which is what keeps
 * the 12 bits of internal precision X3.110 Appendix B asks for at a 256-pixel
 * resolution: the widget scales them into whatever rectangle it is given rather
 * than snapping them to a fixed raster. The display area is the lower 0,78125
 * of the unit screen (Table D1 item 10) and T.101 Table II-3 puts 256 x 200
 * nominal pixels in it, so its pixels are square — 1/256 across and 0,78125/200
 * = 1/256 down. The drawing rect therefore keeps a 1 : 0,78125 aspect and the
 * unit scale is the same on both axes, which is what stops a circle coming out
 * an ellipse.
 *
 * **Colour.** The transparent colour of §5.3.2.5 shows the program video
 * through; there is no video behind a viewer, so it is drawn as black, which is
 * what the standard's own "lower planes" reduce to when there are none. Blink
 * processes (§6.2.8.1) are drawn in their lit phase and left static, as the
 * teletext viewer draws flash.
 */
class NabtsCanvasWidget : public QWidget {
  Q_OBJECT

 public:
  explicit NabtsCanvasWidget(QWidget* parent = nullptr);

  void setPage(const orc::presenters::NabtsPageView& page);
  void clearPage();
  bool hasPage() const { return page_.has_value(); }

  /**
   * @brief Outline the display area and mark what the interpreter could not do
   *
   * A record that lost packets stops drawing part way through and looks exactly
   * like a short record. Off by default, because the markers are not part of
   * the page.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /// Currently displayed page (test seam; nullptr when cleared)
  const orc::presenters::NabtsPageView* page() const {
    return page_ ? &*page_ : nullptr;
  }

  /// Primitives the last paint drew (test seam; 0 before the first paint)
  int primitivesPainted() const { return primitives_painted_; }

  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  /// The largest 1 : kNabtsDisplayAreaHeightView rectangle that fits, centred.
  QRectF displayAreaRect() const;

  /// Paint one primitive of |page| into |area|, whose scale is |unit| pixels
  /// per unit of x (and of y — the mapping is isotropic). |page| is passed
  /// rather than read from page_ because the primitive's texture masks and DRCS
  /// glyphs live in it.
  static void paintPrimitive(
      QPainter& painter, const orc::presenters::NabtsPageView& page,
      const orc::presenters::NabtsPrimitiveView& primitive, const QRectF& area,
      qreal unit);

  std::optional<orc::presenters::NabtsPageView> page_;
  bool show_data_errors_ = false;
  mutable int primitives_painted_ = 0;
};

#endif  // NABTSCANVASWIDGET_H
