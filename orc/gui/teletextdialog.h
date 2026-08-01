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

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStatusBar>
#include <QString>
#include <cstdint>
#include <optional>
#include <vector>

#include "teletext_page_assembler.h"

class TeletextPageWidget;

/**
 * @brief Modeless preview of the teletext pages the previewer has seen
 *
 * An observer dialog (owned by MainWindow, raised from the preview window's
 * Observers menu) that follows the frame previewer. Unlike its stateless
 * VBI/NTSC siblings it is stateful: it owns a trailing-frame-window packet
 * cache and a page catalogue that accumulates as the user moves through the
 * recording, because teletext is a carousel medium — a single frame carries
 * only a fragment of one page (see TeletextPageAssembler).
 *
 * The catalogue is presented as a clickable list of the pages seen so far,
 * each with the frame it was last seen at; selecting one renders it. Status
 * text lives in a status bar along the bottom edge so transient messages
 * never reflow the page display.
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
   * @brief Show a "reading" pending state while observation requests are
   *        in flight, cleared once every window frame has been delivered.
   */
  void showPending();

  /// Clear the display, the packet cache and the page list (project closed)
  void clearContent();

  /// Drop cached packets and the page list (view node or DAG changed)
  void clearCache();

  /// Advance the trailing window to end at @p frame_index and re-render
  void setCurrentFrame(uint64_t frame_index);

  /// Frames in the current window still lacking packet data
  std::vector<uint64_t> framesNeedingData() const {
    return assembler_.framesNeedingData();
  }

  /// First frame of the current trailing window
  uint64_t windowStartFrame() const { return assembler_.windowStartFrame(); }

  /// Last frame of the current trailing window (the previewer's frame)
  uint64_t currentFrame() const { return assembler_.currentFrame(); }

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

  /// Pages currently listed, in list order (test seam)
  std::vector<QString> listedPages() const;

  /// Recovery readout for the displayed page (test seam; empty when hidden)
  QString recoveryText() const;

 private slots:
  void onPageNumberChanged();
  void onPageSelected();
  void onShowErrorsToggled(bool checked);

 private:
  void setupUI();
  /// Re-render the requested page and refresh the seen-pages list
  void renderPage();
  /// Rebuild the seen-pages list when the catalogue has changed
  void refreshPageList();
  /// Select the list row matching the page-number entry, if it is listed
  void syncListSelection(const QString& page_label);
  /// Update the pending-observation notice from the outstanding frame count
  void updatePendingStatus();

  /// Conventional magazine + two-hex-digit page label, e.g. "100", "1F0"
  static QString formatPageLabel(int magazine, int page_number);

  /// One-line summary of how much of a page came back from the recovery chain
  static QString formatRecovery(
      const orc::presenters::TeletextPageRecoveryView& recovery);

  TeletextPageAssembler assembler_;
  std::optional<orc::presenters::TeletextPageView> current_page_;

  // Catalogue revision the page list was last built from; guards needless
  // rebuilds (which would fight the user's selection and scroll position).
  uint64_t listed_revision_ = 0;
  bool list_populated_ = false;
  // Set while the list is being rebuilt or programmatically selected, so
  // selection changes do not feed back into the page-number entry.
  bool updating_list_ = false;

  QLineEdit* page_edit_ = nullptr;
  QCheckBox* show_errors_check_ = nullptr;
  QListWidget* pages_list_ = nullptr;
  QStatusBar* status_bar_ = nullptr;
  // "rows 23/24, 4 damaged byte(s)" for the displayed page.
  QLabel* recovery_label_ = nullptr;
  // Pending-state notice shown while async observation requests are in flight.
  QLabel* status_label_ = nullptr;
  // "Page last seen at frame N" / "not seen yet" notice.
  QLabel* seen_label_ = nullptr;
  TeletextPageWidget* page_widget_ = nullptr;
};

#endif  // TELETEXTDIALOG_H
