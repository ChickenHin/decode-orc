/*
 * File:        teletextdialog.h
 * Module:      orc-gui
 * Purpose:     Teletext page preview dialog (observer dialog)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TELETEXTDIALOG_H
#define TELETEXTDIALOG_H

#include <orc_teletext.h>

#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <cstdint>
#include <optional>
#include <vector>

#include "teletext_page_assembler.h"

class TeletextPageWidget;

/**
 * @brief Modeless preview of the teletext page carried by the current frame
 *
 * An observer dialog (owned by MainWindow, raised from the preview window's
 * Observers menu) that follows the frame previewer. Unlike its stateless
 * VBI/NTSC siblings it is stateful: it owns a trailing-frame-window packet
 * cache and page assembler, because teletext is a carousel medium — the
 * dialog surfaces "page seen at frame N" rather than pretending continuous
 * reception (see TeletextPageAssembler).
 *
 * MainWindow drives it: setCurrentFrame() on frame changes, then issues one
 * requestTeletextData() per frame reported by framesNeedingData() and feeds
 * the responses back through deliverFrameData().
 */
class TeletextDialog : public QDialog {
  Q_OBJECT

 public:
  explicit TeletextDialog(QWidget* parent = nullptr);
  ~TeletextDialog();

  /**
   * @brief Show a "computing" pending state while observation requests are
   *        in flight, cleared once deliveries arrive.
   */
  void showPending();

  /// Clear the display and the packet cache (project closed / node invalid)
  void clearContent();

  /// Drop cached packets only (view node or DAG changed)
  void clearCache();

  /// Advance the trailing window to end at @p frame_index and re-render
  void setCurrentFrame(uint64_t frame_index);

  /// Frames in the current window still lacking packet data
  std::vector<uint64_t> framesNeedingData() const {
    return assembler_.framesNeedingData();
  }

  /**
   * @brief Deliver a teletextDataReady response for one frame
   *
   * @param available       False when the frame could not be observed
   * @param field1_id_value First field of the frame (FieldID::value())
   * @param field1          Packet view for the first field
   * @param field2          Packet view for the second field
   */
  void deliverFrameData(
      bool available, uint64_t field1_id_value,
      const orc::presenters::TeletextFieldPacketsView& field1,
      const orc::presenters::TeletextFieldPacketsView& field2);

  /// Currently rendered page (test seam; nullopt when no page is shown)
  const std::optional<orc::presenters::TeletextPageView>& currentPage() const {
    return current_page_;
  }

  /// Page-number entry text (test seam)
  QString pageNumberText() const;
  void setPageNumberText(const QString& text);

 private slots:
  void onPageNumberChanged();

 private:
  void setupUI();
  /// Re-assemble and render the requested page from the cached window
  void renderPage();

  TeletextPageAssembler assembler_;
  std::optional<orc::presenters::TeletextPageView> current_page_;

  QLineEdit* page_edit_ = nullptr;
  // Pending-state notice shown while async observation requests are in flight.
  QLabel* status_label_ = nullptr;
  // "Page last seen at frame N" / "not seen in the last N frames" notice.
  QLabel* seen_label_ = nullptr;
  TeletextPageWidget* page_widget_ = nullptr;
};

#endif  // TELETEXTDIALOG_H
