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

#include <QWidget>
#include <optional>

/**
 * @brief Paints a 40×25 Level 1 teletext page
 *
 * Alphanumeric cells use the system monospace font; mosaic cells are painted
 * as 2×3 sixel blocks (ETSI EN 300 706 §15.7.1 Table 47, contiguous or
 * separated). Level 1 attributes are honoured: foreground/background colour,
 * double height, conceal (rendered as SPACE — no reveal control yet) and
 * flash (rendered static). Teletext has its own fixed display colours
 * (§12.2 Table 26), so cell colours are deliberately not theme-derived.
 */
class TeletextPageWidget : public QWidget {
  Q_OBJECT

 public:
  explicit TeletextPageWidget(QWidget* parent = nullptr);

  void setPage(const orc::presenters::TeletextPageView& page);
  void clearPage();
  bool hasPage() const { return page_.has_value(); }

  /// Currently displayed page (test seam; nullptr when cleared)
  const orc::presenters::TeletextPageView* page() const {
    return page_ ? &*page_ : nullptr;
  }

  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  std::optional<orc::presenters::TeletextPageView> page_;
};

#endif  // TELETEXTPAGEWIDGET_H
