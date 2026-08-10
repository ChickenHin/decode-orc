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

#include <QPointer>
#include <QRectF>
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
  QRectF displayAreaRect() const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  static void paintOp(QPainter& painter, const orc::CatalogueDisplayList& list,
                      const orc::CatalogueDrawOp& op, const QRectF& area,
                      qreal unit, bool lit);

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
