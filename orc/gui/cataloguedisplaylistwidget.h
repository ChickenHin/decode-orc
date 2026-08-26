/*
 * File:        cataloguedisplaylistwidget.h
 * Module:      orc-gui
 * Purpose:     Widget rasterising a CataloguePayload 2D display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUEDISPLAYLISTWIDGET_H
#define CATALOGUEDISPLAYLISTWIDGET_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QWidget>
#include <optional>

class CatalogueFlashClock;
class QPainter;

/**
 * @brief Draws a display-list payload
 *
 * The list is walked front to back into a drawable area of the payload's own
 * aspect, centred in the widget. Unit space has y running upwards from the
 * bottom left, which is inverted here once and never again.
 *
 * Nothing outside the drawable area is painted: a display list is authored
 * against a screen the receiver guarantees is visible, and the strip around it
 * is border.
 *
 * An operation in a blink process is animated against a CatalogueFlashClock:
 * its blank phase draws the operation in its background colour, or in black
 * where it has none. Without a clock, or with animation switched off, the lit
 * phase is held.
 */
class CatalogueDisplayListWidget : public QWidget {
  Q_OBJECT

 public:
  explicit CatalogueDisplayListWidget(QWidget* parent = nullptr);
  ~CatalogueDisplayListWidget() override;

  void setDisplayList(const orc::CatalogueDisplayList& list);
  void clearDisplayList();
  bool hasDisplayList() const { return list_.has_value(); }

  /**
   * @brief Outline the drawable area
   *
   * A list drawn entirely in one corner looks mis-scaled without it. Off by
   * default, because the outline is not part of the content.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /**
   * @brief Drive the blink phase from |clock|
   *
   * nullptr detaches, which leaves the list in its lit phase. The dialogue
   * hands the same clock to every payload view so they blink in step.
   */
  void setFlashClock(CatalogueFlashClock* clock);

  /// Animate blinking operations. Off holds the lit phase.
  void setAnimationsEnabled(bool enabled);
  bool animationsEnabled() const { return animations_enabled_; }

  /// Whether the list holds a blinking operation (test seam; also what gates
  /// the clock subscription)
  bool hasBlinkingOps() const { return has_blinking_ops_; }

  /// The blink phase being painted; true is the lit one. Settable so tests can
  /// exercise both phases without a wall clock (test seam).
  bool flashLit() const { return flash_lit_; }
  void setFlashLit(bool lit);

  /// Operations painted by the last paint (test seam)
  int opsPainted() const { return ops_painted_; }

  /// Placeholder shown when no list is set
  void setPlaceholderText(const QString& text);

  QSize sizeHint() const override;

  /// The drawable area within the widget, in device pixels (test seam)
  QRectF displayAreaRect() const { return displayAreaRectIn(QSizeF(size())); }

  /**
   * @brief The drawable area alone as an image |size| pixels across and down
   *
   * The area is fitted to |size| at the list's own aspect exactly as it is
   * fitted to the widget, so a size of that aspect — which is what
   * pageImageSize() gives — leaves no border around it.
   *
   * Always the lit phase, whatever phase the view is holding: a blinking figure
   * is on the page because the service put it there, and a still of the other
   * phase would lose it. The drawable-area outline follows the view's own
   * setting.
   *
   * Null when there is no list to draw or |size| is empty.
   */
  QImage renderPageImage(const QSize& size) const;

  /// The size renderPageImage() is worth being asked for, at the list's own
  /// aspect. Null when there is no list.
  QSize pageImageSize() const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  /**
   * @brief How one paint maps unit space onto the surface it draws to
   *
   * The two scales differ wherever the list's nominal pixels are not square:
   * the drawable area takes the shape the list is displayed at, while unit y
   * still runs over the extent the list covers, so a unit of y is not the same
   * number of device pixels as a unit of x.
   */
  struct Mapping {
    /// Device pixels per unit of x, and per unit of y.
    qreal unit_x = 0.0;
    qreal unit_y = 0.0;
    /// The thinnest stroke that is drawn, in device pixels. One pixel of the
    /// list's own grid where it names one, and one device pixel otherwise.
    qreal minimum_pen = 1.0;
  };

  static void paintOp(QPainter& painter, const orc::CatalogueDisplayList& list,
                      const orc::CatalogueDrawOp& op, const QRectF& area,
                      const Mapping& mapping, bool lit);

  /// The mapping from unit space onto a drawable area of |area|
  Mapping mappingFor(const QRectF& area) const;

  /// The drawable area within a paint surface |bounds| pixels across and down,
  /// whose origin is (0, 0) — the widget itself, or the image a save renders
  /// into
  QRectF displayAreaRectIn(const QSizeF& bounds) const;

  /// Draw the list into |bounds| of |painter|, in the given blink phase. What
  /// both the widget's own paint and an exported image are.
  void paintContent(QPainter& painter, const QRectF& bounds, bool lit) const;

  /// Subscribe to the clock only while this view is visible and holds
  /// something that blinks; release it otherwise
  void updateFlashSubscription();

  std::optional<orc::CatalogueDisplayList> list_;
  QString placeholder_;
  bool show_data_errors_ = false;
  mutable int ops_painted_ = 0;

  QPointer<CatalogueFlashClock> flash_clock_;
  bool animations_enabled_ = true;
  bool flash_lit_ = true;
  bool has_blinking_ops_ = false;
  bool flash_subscribed_ = false;
};

#endif  // CATALOGUEDISPLAYLISTWIDGET_H
