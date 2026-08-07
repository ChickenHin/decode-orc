/*
 * File:        teletextdialog.h
 * Module:      orc-gui
 * Purpose:     Teletext page viewer for the teletext sink stage tool
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
#include <QStatusBar>
#include <QString>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class TeletextPageWidget;

/**
 * @brief Viewer for the pages a teletext sink recovered
 *
 * The stage tool of one `teletext_sink` node: triggering the node
 * decodes the whole frame range in a single pass and this dialogue shows what
 * it found. Unlike the preview observer dialogues it replaced, it does not
 * follow the previewer — the catalogue describes the entire source, so there is
 * no window to slide and nothing to accumulate here.
 *
 * The catalogue is presented as a clickable table of the pages the range
 * carried — page number, how many times the carousel brought it round, and the
 * frames it was first and last seen at; selecting a row renders that page.
 * Pages transmitted with the C6 subtitle control bit are marked in the table
 * and named beside the page-number entry: which page carries the subtitles is a
 * property of the recording (888 is only the broadcast convention), and it is
 * what the sink stages need to be told to export them.
 *
 * A page number may carry a sequence of sub-pages that the service cycles
 * through (the multi-page set of ETSI EN 300 706 Annex A.1 — on a receiver, a
 * page that changes on its own every few seconds). Those are catalogued
 * separately, so the display carries its own control: how many sub-pages the
 * page has, which of them is on screen, and a step either way through the
 * sequence, wrapping as the carousel itself does.
 *
 * MainWindow drives it: setAnalysisData() with whatever the coordinator
 * delivered for the node, and clearContent() when there is nothing to show.
 */
class TeletextDialog : public QDialog {
  Q_OBJECT

 public:
  explicit TeletextDialog(QWidget* parent = nullptr);
  ~TeletextDialog();

  /// Show a "decoding" pending state while the stage trigger is in flight
  void showPending();

  /// Clear the display, the page list and the recovery readout
  void clearContent();

  /// Show one trigger run's catalogue, replacing whatever was displayed
  void setAnalysisData(const orc::presenters::TeletextAnalysisView& data);

  /// Currently rendered page (test seam; nullopt when no page is shown)
  const std::optional<orc::presenters::TeletextPageView>& currentPage() const {
    return current_page_;
  }

  /// Page-number entry text (test seam)
  QString pageNumberText() const;
  void setPageNumberText(const QString& text);

  /// Pages currently listed, in table order (test seam)
  std::vector<QString> listedPages() const;

  /// Times-seen count shown for @p page_label, or 0 when it is not listed
  /// (test seam)
  uint64_t listedSeenCount(const QString& page_label) const;

  /// Sub-page readout — "Sub-page 2 of 8 (0002)" — for the displayed page
  /// (test seam; empty when the control is hidden)
  QString subpageText() const;

  /// Sub-pages of the displayed page, and which of them is shown (test seam;
  /// 0 and -1 when no page is displayed)
  int subpageCount() const;
  int subpageIndex() const;

  /// Step through the displayed page's sub-pages, wrapping at either end, as
  /// the navigation buttons do (test seam)
  void showNextSubpage();
  void showPreviousSubpage();

  /// Recovery readout for the displayed page (test seam; empty when hidden)
  QString recoveryText() const;

  /// Run-wide recovery summary (test seam; empty before any data arrives)
  QString summaryText() const;

  /// Subtitle-page notice text (test seam; empty when no subtitle page was
  /// seen and the notice is hidden)
  QString subtitleHintText() const;

 private slots:
  void onPageNumberChanged();
  void onPageSelected();
  void onShowErrorsToggled(bool checked);

 private:
  /// Seen-pages table columns
  enum PageColumn {
    kColumnPage = 0,   ///< "100"
    kColumnSeen = 1,   ///< transmissions counted over the analysed range
    kColumnFrame = 2,  ///< frames the page was first and last seen at (1-based)
    kColumnCount = 3,
  };

  void setupUI();
  /// Re-render the requested page from the catalogue
  void renderPage();
  /// Show the sub-page @p index of the displayed page, wrapping either way
  void stepSubpage(int delta);
  /// Update the sub-page control for the page currently displayed
  void refreshSubpageControl(
      const orc::presenters::TeletextCataloguedPageView* entry);
  /// Rebuild the seen-pages table from the current catalogue
  void refreshPageList();
  /// Create the three items of a table row, styling non-selectable pages
  void createPageRow(int row,
                     const orc::presenters::TeletextCataloguedPageView& entry);
  /// Write the volatile columns (subtitle marker, seen count, frame range)
  void updatePageRow(int row,
                     const orc::presenters::TeletextCataloguedPageView& entry);
  /// Select the table row matching the page-number entry, if it is listed
  void syncListSelection(const QString& page_label);
  /// Look up a catalogue entry by page address (nullptr when not carried)
  const orc::presenters::TeletextCataloguedPageView* findPage(
      int magazine, int page_number) const;

  /// Conventional magazine + two-hex-digit page label, e.g. "100", "1F0"
  static QString formatPageLabel(int magazine, int page_number);

  /// The four sub-code digits S4 S3 S2 S1 as they are conventionally written
  /// (ETSI EN 300 706 Annex A.1, e.g. "0002"), from the packed 13-bit field
  static QString formatSubcode(int subcode);

  /// One-line summary of how much of the displayed page came back from the
  /// recovery chain
  static QString formatRecovery(const orc::presenters::TeletextPageView& page);

  /// One-line summary of how the run went, for the status bar
  static QString formatSummary(
      const orc::presenters::TeletextRecoverySummaryView& summary);

  orc::presenters::TeletextAnalysisView data_;
  bool has_data_ = false;
  std::optional<orc::presenters::TeletextPageView> current_page_;

  // Which sub-page of the displayed page is on screen, and the page address it
  // belongs to: stepping through a carousel has to survive the re-render it
  // causes, while typing a different page number starts at the top of that
  // page's sequence.
  int subpage_index_ = 0;
  std::optional<std::pair<int, int>> subpage_address_;

  // Set while the table is being rebuilt or programmatically selected, so
  // selection changes do not feed back into the page-number entry.
  bool updating_list_ = false;

  QLineEdit* page_edit_ = nullptr;
  QCheckBox* show_errors_check_ = nullptr;
  // Sub-page navigation, under the page display: hidden while no page is
  // shown, and inert on a page that has no sub-pages.
  QWidget* subpage_bar_ = nullptr;
  QToolButton* prev_subpage_button_ = nullptr;
  QToolButton* next_subpage_button_ = nullptr;
  QLabel* subpage_label_ = nullptr;
  QTableWidget* pages_table_ = nullptr;
  QStatusBar* status_bar_ = nullptr;
  // "rows 23/24, 4 damaged byte(s)" for the displayed page.
  QLabel* recovery_label_ = nullptr;
  // Pending-state notice shown while the stage trigger is in flight.
  QLabel* status_label_ = nullptr;
  // "Page 100 seen 12 times (frames 5-4210)" for the displayed page.
  QLabel* seen_label_ = nullptr;
  // "Subtitles on 190" — the pages seen carrying the C6 subtitle control bit.
  QLabel* subtitle_hint_ = nullptr;
  // Run-wide recovery figures, so a viewer can tell an empty page list from a
  // recording that carried no teletext at all.
  QLabel* summary_label_ = nullptr;
  TeletextPageWidget* page_widget_ = nullptr;
};

#endif  // TELETEXTDIALOG_H
