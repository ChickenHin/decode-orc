/*
 * File:        teletextpagewidget.h
 * Module:      orc-gui
 * Purpose:     Widget painting a 40x25 Level 1 teletext page grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXTPAGEWIDGET_H
#define TELETEXTPAGEWIDGET_H

#include <orc_teletext.h>

#include <QRectF>
#include <QWidget>
#include <optional>

class QPainter;

/**
 * @brief Paints a 40×25 Level 1 teletext page
 *
 * The page is drawn into an aspect-locked rect built from the nominal 12×20
 * character rectangle, so glyphs and mosaic blocks keep their proportions
 * whatever shape the widget is given.
 *
 * Alphanumeric cells use the system monospace font; mosaic cells are painted
 * as 2×3 sixel blocks (ETSI EN 300 706 §15.7.1 Table 47, contiguous or
 * separated). Level 1 attributes are honoured: foreground/background colour,
 * double height (the character spans both rows at unchanged width), boxing on
 * newsflash/subtitle pages, conceal (rendered as SPACE — no reveal control
 * yet) and flash (rendered static). Teletext has its own fixed display
 * colours (§12.2 Table 26), so cell colours are deliberately not
 * theme-derived.
 *
 * Painting is two-pass — every background first, then every foreground —
 * because a double-height character occupies the row below its origin
 * (§12.2 code 0/D) and a row-sequential paint would erase its lower half.
 */
class TeletextPageWidget : public QWidget {
  Q_OBJECT

 public:
  explicit TeletextPageWidget(QWidget* parent = nullptr);

  void setPage(const orc::presenters::TeletextPageView& page);
  void clearPage();
  bool hasPage() const { return page_.has_value(); }

  /**
   * @brief Overlay the page with markers for data that never arrived
   *
   * A row no packet was recovered for renders exactly like a transmitted
   * blank row, and a parity-damaged byte exactly like a SPACE, so without
   * this overlay a recovery gap is indistinguishable from page content.
   * Off by default, because the markers are not part of the page.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /// Currently displayed page (test seam; nullptr when cleared)
  const orc::presenters::TeletextPageView* page() const {
    return page_ ? &*page_ : nullptr;
  }

  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  /// Hatch rows that carried no packet and outline parity-damaged cells
  static void paintDataErrorOverlay(
      QPainter& painter, const orc::presenters::TeletextPageView& page,
      const QRectF& page_rect, qreal cell_w, qreal cell_h);

  std::optional<orc::presenters::TeletextPageView> page_;
  bool show_data_errors_ = false;
};

#endif  // TELETEXTPAGEWIDGET_H
