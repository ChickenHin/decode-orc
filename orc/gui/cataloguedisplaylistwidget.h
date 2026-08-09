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

#include <QRectF>
#include <QWidget>
#include <optional>

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
 */
class CatalogueDisplayListWidget : public QWidget {
  Q_OBJECT

 public:
  explicit CatalogueDisplayListWidget(QWidget* parent = nullptr);

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

  /// Operations painted by the last paint (test seam)
  int opsPainted() const { return ops_painted_; }

  /// Placeholder shown when no list is set
  void setPlaceholderText(const QString& text);

  QSize sizeHint() const override;

  /// The drawable area within the widget, in device pixels (test seam)
  QRectF displayAreaRect() const;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  static void paintOp(QPainter& painter, const orc::CatalogueDisplayList& list,
                      const orc::CatalogueDrawOp& op, const QRectF& area,
                      qreal unit);

  std::optional<orc::CatalogueDisplayList> list_;
  QString placeholder_;
  bool show_data_errors_ = false;
  mutable int ops_painted_ = 0;
};

#endif  // CATALOGUEDISPLAYLISTWIDGET_H
