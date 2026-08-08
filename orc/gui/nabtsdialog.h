/*
 * File:        nabtsdialog.h
 * Module:      orc-gui
 * Purpose:     NABTS record viewer for the nabts_sink stage tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef NABTSDIALOG_H
#define NABTSDIALOG_H

#include <orc_nabts.h>

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <vector>

class NabtsCanvasWidget;

/**
 * @brief Viewer for the records a NABTS sink recovered
 *
 * The stage tool of one `nabts_sink` node: triggering the node decodes the
 * whole frame range in a single pass and this dialogue shows what it found.
 * Like the teletext viewer it replaced nothing and follows nothing — the
 * catalogue describes the entire source, so there is no window to slide.
 *
 * A NABTS service is a set of *records* rather than numbered pages (CEA-516
 * §5), each identified by its data channel, record address and version
 * (§5.2.1), so the catalogue is presented as a table of those. Selecting one
 * shows it:
 *
 * - A **presentation record** (types 0, 1 and 3) is NAPLPS, so it is drawn —
 *   see NabtsCanvasWidget. Its text is available beside the drawing, because a
 *   caption or an index page is usually read rather than looked at.
 * - An **application record** (type 2) is a list of function descriptors
 *   (§7.2.2) rather than anything displayable, so it is listed.
 *
 * Records step with the arrow buttons in the order §7.3's Next-record rule
 * defines: ascending record address within the channel, which is the order the
 * table is in. That is what "the next page" means to a receiver that has not
 * been told otherwise, and it wraps as the carousel does.
 *
 * Where the recording carried captioning (§7.3.10 — records with the Caption
 * Flag of §5.2.7.3), the cues are shown as their own track, because reading a
 * caption service one record at a time tells a viewer nothing about it.
 *
 * MainWindow drives it: setAnalysisData() with whatever the coordinator
 * delivered for the node, and clearContent() when there is nothing to show.
 */
class NabtsDialog : public QDialog {
  Q_OBJECT

 public:
  explicit NabtsDialog(QWidget* parent = nullptr);
  ~NabtsDialog();

  /// Show a "decoding" pending state while the stage trigger is in flight
  void showPending();

  /// Clear the display, the record list and the recovery readout
  void clearContent();

  /// Show one trigger run's catalogue, replacing whatever was displayed
  void setAnalysisData(const orc::presenters::NabtsAnalysisView& data);

  /// Records currently listed, in table order, as "channel/address v" (test
  /// seam)
  std::vector<QString> listedRecords() const;

  /// Index of the record on display, or -1 when none is (test seam)
  int currentRecordIndex() const { return current_index_; }

  /// The record on display (test seam; nullptr when none is)
  const orc::presenters::NabtsCatalogueRecordView* currentRecord() const;

  /// Select the record at |index| of the table (test seam)
  void selectRecord(int index);

  /// Step through the records as the navigation buttons do, wrapping at either
  /// end (test seam)
  void showNextRecord();
  void showPreviousRecord();

  /// Detail readout for the displayed record (test seam; empty when hidden)
  QString detailText() const;

  /// Run-wide recovery summary (test seam; empty before any data arrives)
  QString summaryText() const;

  /// Caption-track notice — "12 captions on A00/000" — or empty when the
  /// recording carried none (test seam)
  QString captionHintText() const;

  /// Caption cues currently listed, as their text, in cue order (test seam)
  std::vector<QString> listedCaptions() const;

 private slots:
  void onRecordSelected();
  void onShowErrorsToggled(bool checked);
  void onShowCaptionsToggled(bool checked);

 private:
  /// Record table columns
  enum RecordColumn {
    kColumnAddress = 0,  ///< "000/1A4 v2"
    kColumnType = 1,     ///< record type, abbreviated
    kColumnSeen = 2,     ///< copies counted over the analysed range
    kColumnFrame = 3,    ///< frames first and last seen at (1-based)
    kColumnCount = 4,
  };

  /// Which pane the right-hand side is showing
  enum DetailPage {
    kPageCanvas = 0,     ///< a presentation record, drawn
    kPageFunctions = 1,  ///< an application record's function descriptors
    kPageCaptions = 2,   ///< the caption track
    kPageNothing = 3,    ///< no record selected
  };

  void setupUI();
  /// Rebuild the record table from the current catalogue
  void refreshRecordList();
  /// Rebuild the caption track from the current catalogue
  void refreshCaptions();
  /// Show the record at |index|, or nothing when it is out of range
  void showRecord(int index);
  /// Step |delta| records, wrapping at either end
  void stepRecord(int delta);

  /// "000/1A4 v2", the identity §5.2.1 gives a record
  static QString formatIdentity(
      const orc::presenters::NabtsCatalogueRecordView& record);
  /// One-line description of the displayed record's condition
  static QString formatDetail(
      const orc::presenters::NabtsCatalogueRecordView& record);
  /// One-line summary of how the run went, for the readout under the display
  static QString formatSummary(
      const orc::presenters::NabtsRecoverySummaryView& summary);
  /// An application record's descriptors as a listing (§7.2.2)
  static QString formatFunctions(
      const orc::presenters::NabtsCatalogueRecordView& record);

  orc::presenters::NabtsAnalysisView data_;
  bool has_data_ = false;
  int current_index_ = -1;

  // Set while the table is being rebuilt or programmatically selected, so
  // selection changes do not feed back.
  bool updating_list_ = false;

  QCheckBox* show_errors_check_ = nullptr;
  QCheckBox* show_captions_check_ = nullptr;
  QTableWidget* records_table_ = nullptr;
  QStackedWidget* detail_stack_ = nullptr;
  NabtsCanvasWidget* canvas_ = nullptr;
  QPlainTextEdit* text_pane_ = nullptr;
  QPlainTextEdit* functions_pane_ = nullptr;
  QTableWidget* captions_table_ = nullptr;
  QLabel* empty_label_ = nullptr;
  QToolButton* prev_record_button_ = nullptr;
  QToolButton* next_record_button_ = nullptr;
  QLabel* caption_hint_ = nullptr;
  QLabel* summary_label_ = nullptr;
  QStatusBar* status_bar_ = nullptr;
  // "Record 000/1A4 seen 12 times (frames 5-4210)" for the displayed record.
  QLabel* seen_label_ = nullptr;
  // "complete, 3 blocks repaired" for the displayed record.
  QLabel* detail_label_ = nullptr;
  // Pending-state notice shown while the stage trigger is in flight.
  QLabel* status_label_ = nullptr;
};

#endif  // NABTSDIALOG_H
