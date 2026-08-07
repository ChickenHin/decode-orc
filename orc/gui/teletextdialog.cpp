/*
 * File:        teletextdialog.cpp
 * Module:      orc-gui
 * Purpose:     Teletext page viewer for the teletext analysis sink stage tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletextdialog.h"

#include <orc/support/teletext_page_decoder.h>

#include <QAbstractItemView>
#include <QBrush>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPalette>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cstddef>

#include "teletextpagewidget.h"

namespace {

// ETSI EN 300 706 §9.3.1.1: the page number is a two-digit hexadecimal field,
// but a receiver keypad can only select the decimal values, so viewable pages
// run 100-899. Numbers containing A-F address hidden data pages (and are what
// a misdecoded header usually produces).
bool isNonSelectablePage(int page_number) {
  return (page_number & 0x0F) > 9 || ((page_number >> 4) & 0x0F) > 9;
}

// Row order: the selectable pages 100-899 ascending, then the hex-digit pages
// (also ascending) below them. Magazine numbers are 1-8, so the key is just
// the page address with the hex pages biased past every decimal one.
int pageSortKey(int magazine, int page_number) {
  return (isNonSelectablePage(page_number) ? 0x1000 : 0) + magazine * 0x100 +
         page_number;
}

}  // namespace

TeletextDialog::TeletextDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("Teletext Pages"));

  // Use Qt::Window flag to allow independent positioning
  setWindowFlags(Qt::Window);

  resize(720, 540);
  setMinimumSize(520, 400);
}

TeletextDialog::~TeletextDialog() = default;

void TeletextDialog::setupUI() {
  // The status bar sits flush against the window edge, so the outer layout
  // carries no margins and the content above it supplies its own.
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto* content_layout = new QVBoxLayout();
  content_layout->setContentsMargins(9, 9, 9, 9);

  auto* page_row = new QHBoxLayout();
  auto* page_label = new QLabel(tr("Page:"), this);
  page_row->addWidget(page_label);

  // Conventional magazine + two-hex-digit page number, e.g. 100, 888, 1F0.
  page_edit_ = new QLineEdit(this);
  page_edit_->setObjectName("teletextPageEdit");
  page_edit_->setText("100");
  page_edit_->setMaxLength(3);
  page_edit_->setFixedWidth(60);
  connect(page_edit_, &QLineEdit::textChanged, this,
          &TeletextDialog::onPageNumberChanged);
  page_row->addWidget(page_edit_);

  // Which page the subtitles are on is a property of the recording, not a
  // constant: 888 is the broadcast convention, but a LaserDisc author was
  // free to use anything (190 on the discs this was developed against). The
  // service says so itself through C6, so the answer is put where the reader
  // is already typing a page number rather than left to be found by scanning
  // the table.
  subtitle_hint_ = new QLabel(this);
  subtitle_hint_->setObjectName("teletextSubtitleHint");
  subtitle_hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  subtitle_hint_->setVisible(false);
  page_row->addWidget(subtitle_hint_);
  page_row->addStretch();

  // A row that was never recovered renders exactly like a transmitted blank
  // row, so the overlay is the only way to tell a recovery gap from page
  // content. Off by default: the markers are not part of the page.
  show_errors_check_ = new QCheckBox(tr("Show data errors"), this);
  show_errors_check_->setObjectName("teletextShowErrorsCheck");
  show_errors_check_->setToolTip(
      tr("Outline characters whose transmitted byte failed its parity check, "
         "and hatch the missing rows of a page that lost packets. A page is "
         "free to leave rows out — most do — so rows are only hatched when "
         "something was actually lost."));
  connect(show_errors_check_, &QCheckBox::toggled, this,
          &TeletextDialog::onShowErrorsToggled);
  page_row->addWidget(show_errors_check_);
  content_layout->addLayout(page_row);

  auto* body_row = new QHBoxLayout();

  // Left column: every page the analysed range carried. Teletext is a
  // carousel, so which pages exist is itself a discovery — the table is how
  // the user finds out, rather than having to guess page numbers.
  auto* list_column = new QVBoxLayout();
  auto* list_heading = new QLabel(tr("Pages carried:"), this);
  list_column->addWidget(list_heading);

  pages_table_ = new QTableWidget(0, kColumnCount, this);
  pages_table_->setObjectName("teletextPagesTable");
  pages_table_->setHorizontalHeaderLabels(
      {tr("Page"), tr("Seen"), tr("Frames")});
  pages_table_->horizontalHeaderItem(kColumnSeen)
      ->setToolTip(tr("How many times the carousel brought this page round "
                      "over the analysed range."));
  pages_table_->horizontalHeaderItem(kColumnFrame)
      ->setToolTip(tr("Frames carrying the first and last transmission."));
  // The two numeric columns are right-aligned so their digits line up; their
  // headings follow them.
  for (const int column : {kColumnSeen, kColumnFrame}) {
    pages_table_->horizontalHeaderItem(column)->setTextAlignment(
        Qt::AlignRight | Qt::AlignVCenter);
  }
  pages_table_->setMinimumWidth(220);
  pages_table_->setMaximumWidth(320);
  pages_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  pages_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  pages_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  pages_table_->setShowGrid(false);
  pages_table_->setAlternatingRowColors(true);
  pages_table_->setWordWrap(false);
  pages_table_->setCornerButtonEnabled(false);
  pages_table_->verticalHeader()->setVisible(false);
  pages_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  auto* pages_header = pages_table_->horizontalHeader();
  pages_header->setSectionResizeMode(kColumnPage,
                                     QHeaderView::ResizeToContents);
  pages_header->setSectionResizeMode(kColumnSeen,
                                     QHeaderView::ResizeToContents);
  pages_header->setSectionResizeMode(kColumnFrame, QHeaderView::Stretch);
  connect(pages_table_, &QTableWidget::itemSelectionChanged, this,
          &TeletextDialog::onPageSelected);
  list_column->addWidget(pages_table_, /*stretch=*/1);
  body_row->addLayout(list_column);

  page_widget_ = new TeletextPageWidget(this);
  body_row->addWidget(page_widget_, /*stretch=*/1);
  content_layout->addLayout(body_row, /*stretch=*/1);

  // How the run went as a whole, below the page display: an empty page list
  // means something quite different on a recording that yielded no packets at
  // all than on one whose packets never assembled into a page.
  summary_label_ = new QLabel(this);
  summary_label_->setObjectName("teletextSummaryLabel");
  summary_label_->setWordWrap(true);
  summary_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  summary_label_->setVisible(false);
  content_layout->addWidget(summary_label_);

  main_layout->addLayout(content_layout, /*stretch=*/1);

  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("teletextStatusBar");
  status_bar_->setSizeGripEnabled(false);

  // Page status on the left, decode progress on the right; both live in the
  // status bar so a message appearing never reflows the page display.
  seen_label_ = new QLabel(tr("No page data"), this);
  seen_label_->setObjectName("teletextSeenLabel");
  status_bar_->addWidget(seen_label_, /*stretch=*/1);

  // Recovery readout: how much of the displayed page actually came back from
  // the slicer, so a gap on screen can be attributed to the data rather than
  // to the rendering.
  recovery_label_ = new QLabel(this);
  recovery_label_->setObjectName("teletextRecoveryLabel");
  status_bar_->addPermanentWidget(recovery_label_);
  recovery_label_->setVisible(false);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_bar_->addPermanentWidget(status_label_);
  status_label_->setVisible(false);

  main_layout->addWidget(status_bar_);
}

void TeletextDialog::showPending() {
  status_label_->setText(tr("Decoding…"));
  status_label_->setVisible(true);
}

void TeletextDialog::clearContent() {
  data_ = orc::presenters::TeletextAnalysisView{};
  has_data_ = false;
  current_page_.reset();
  status_label_->setVisible(false);
  status_label_->clear();
  summary_label_->setVisible(false);
  summary_label_->clear();
  refreshPageList();
  seen_label_->setText(tr("No page data"));
  recovery_label_->setVisible(false);
  page_widget_->clearPage();
}

void TeletextDialog::setAnalysisData(
    const orc::presenters::TeletextAnalysisView& data) {
  data_ = data;
  has_data_ = true;
  status_label_->setVisible(false);
  status_label_->clear();

  const QString summary = formatSummary(data_.summary);
  summary_label_->setText(summary);
  summary_label_->setVisible(!summary.isEmpty());

  refreshPageList();
  renderPage();
}

QString TeletextDialog::pageNumberText() const { return page_edit_->text(); }

void TeletextDialog::setPageNumberText(const QString& text) {
  page_edit_->setText(text);
}

std::vector<QString> TeletextDialog::listedPages() const {
  std::vector<QString> labels;
  labels.reserve(static_cast<std::size_t>(pages_table_->rowCount()));
  for (int row = 0; row < pages_table_->rowCount(); ++row) {
    labels.push_back(
        pages_table_->item(row, kColumnPage)->data(Qt::UserRole).toString());
  }
  return labels;
}

uint64_t TeletextDialog::listedSeenCount(const QString& page_label) const {
  for (int row = 0; row < pages_table_->rowCount(); ++row) {
    if (pages_table_->item(row, kColumnPage)->data(Qt::UserRole).toString() ==
        page_label) {
      return pages_table_->item(row, kColumnSeen)->text().toULongLong();
    }
  }
  return 0;
}

QString TeletextDialog::recoveryText() const {
  return recovery_label_->isVisible() ? recovery_label_->text() : QString();
}

QString TeletextDialog::summaryText() const {
  return summary_label_->isVisible() ? summary_label_->text() : QString();
}

QString TeletextDialog::subtitleHintText() const {
  return subtitle_hint_->isVisible() ? subtitle_hint_->text() : QString();
}

void TeletextDialog::onPageNumberChanged() { renderPage(); }

void TeletextDialog::onShowErrorsToggled(bool checked) {
  page_widget_->setShowDataErrors(checked);
}

QString TeletextDialog::formatRecovery(
    const orc::presenters::TeletextPageView& page) {
  const auto& recovery = page.recovery;
  // A plain count, not a fraction of the 24-row grid. Services leave rows out
  // as a matter of course — the blank lines that space a page out are simply
  // not transmitted — so "rows 21/24" read as three rows missing when nothing
  // was wrong at all.
  const QString rows = tr("%n row(s)", nullptr, recovery.rows_received);

  // A page whose last transmission was still arriving when the range ran out
  // looks exactly like a finished one with rows missing.
  if (!page.transmission_complete) {
    return tr("Partial - still arriving (%1 so far)").arg(rows);
  }

  QStringList faults;
  if (recovery.lost_packets > 0) {
    faults << tr("%n packet(s) lost", nullptr, recovery.lost_packets);
  }
  if (recovery.damaged_bytes > 0) {
    faults << tr("%n damaged byte(s)", nullptr, recovery.damaged_bytes);
  }
  // Not damage, but not settled either: the carousel has corrected most of
  // this page against a repeat and these rows have not been checked by one.
  if (recovery.unconfirmed_rows > 0) {
    faults << tr("%n row(s) seen only once", nullptr,
                 recovery.unconfirmed_rows);
  }
  if (faults.isEmpty()) {
    return tr("Complete (%1)").arg(rows);
  }
  return tr("%1, %2").arg(rows, faults.join(tr(", ")));
}

QString TeletextDialog::formatSummary(
    const orc::presenters::TeletextRecoverySummaryView& summary) {
  if (summary.frames_analysed == 0 && summary.packets_recovered == 0) {
    return {};
  }

  QStringList parts;
  parts << tr("%1 packets recovered from %2 fields over %3 frames")
               .arg(static_cast<qulonglong>(summary.packets_recovered))
               .arg(static_cast<qulonglong>(summary.fields_with_data))
               .arg(static_cast<qulonglong>(summary.frames_analysed));
  // Odd parity (ETSI EN 300 706 §8.1) is the only damage measure available
  // without the original transmission, and it is a floor: a byte damaged in
  // two bits passes it.
  if (summary.characters_written > 0) {
    parts << tr("%1 of %2 characters known damaged")
                 .arg(static_cast<qulonglong>(summary.characters_damaged))
                 .arg(static_cast<qulonglong>(summary.characters_written));
  }
  if (summary.packets_corrected > 0) {
    parts << tr("%1 rows corrected by repeats")
                 .arg(static_cast<qulonglong>(summary.packets_corrected));
  }
  if (summary.bytes_repaired > 0) {
    parts << tr("%1 bytes parity-repaired")
                 .arg(static_cast<qulonglong>(summary.bytes_repaired));
  }
  if (summary.lost_packets_estimate > 0) {
    parts << tr("about %1 packets lost")
                 .arg(static_cast<qulonglong>(summary.lost_packets_estimate));
  }
  if (summary.pages_truncated) {
    parts << tr("page list truncated at the catalogue limit");
  }
  return parts.join(tr("; "));
}

void TeletextDialog::onPageSelected() {
  if (updating_list_) {
    return;  // programmatic selection following the page-number entry
  }
  const int row = pages_table_->currentRow();
  if (row < 0) {
    return;
  }
  const auto* item = pages_table_->item(row, kColumnPage);
  if (item == nullptr) {
    return;
  }
  const QString label = item->data(Qt::UserRole).toString();
  if (!label.isEmpty() && label != page_edit_->text()) {
    page_edit_->setText(label);  // textChanged renders the page
  }
}

QString TeletextDialog::formatPageLabel(int magazine, int page_number) {
  return QStringLiteral("%1%2")
      .arg(magazine)
      .arg(page_number, 2, 16, QLatin1Char('0'))
      .toUpper();
}

const orc::presenters::TeletextCataloguedPageView* TeletextDialog::findPage(
    int magazine, int page_number) const {
  const auto match = std::find_if(
      data_.pages.begin(), data_.pages.end(),
      [magazine,
       page_number](const orc::presenters::TeletextCataloguedPageView& entry) {
        return entry.magazine == magazine && entry.page_number == page_number;
      });
  return match == data_.pages.end() ? nullptr : &*match;
}

void TeletextDialog::createPageRow(
    int row, const orc::presenters::TeletextCataloguedPageView& entry) {
  const QString label = formatPageLabel(entry.magazine, entry.page_number);

  auto* page_item = new QTableWidgetItem(label);
  page_item->setData(Qt::UserRole, label);
  auto* seen_item = new QTableWidgetItem();
  seen_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  auto* frame_item = new QTableWidgetItem();
  frame_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

  if (isNonSelectablePage(entry.page_number)) {
    // Kept listed — it is genuinely recovered data — but greyed, so the pages
    // a viewer could actually tune to are the ones that stand out.
    const QBrush muted = palette().brush(QPalette::Disabled, QPalette::Text);
    const QString hint =
        tr("Page %1 contains hexadecimal digits: a hidden data page, or a "
           "misdecoded header. It cannot be selected on a receiver.")
            .arg(label);
    for (auto* item : {page_item, seen_item, frame_item}) {
      item->setForeground(muted);
      item->setToolTip(hint);
    }
  }

  pages_table_->setItem(row, kColumnPage, page_item);
  pages_table_->setItem(row, kColumnSeen, seen_item);
  pages_table_->setItem(row, kColumnFrame, frame_item);
}

void TeletextDialog::updatePageRow(
    int row, const orc::presenters::TeletextCataloguedPageView& entry) {
  auto* page_item = pages_table_->item(row, kColumnPage);
  const QString label = page_item->data(Qt::UserRole).toString();

  // The subtitle marker is spelled out rather than shown as a symbol because
  // there is nowhere in this table to put a legend.
  page_item->setText(entry.subtitle ? tr("%1 subs").arg(label) : label);
  if (entry.subtitle) {
    page_item->setToolTip(
        tr("Page %1 is transmitted with the C6 subtitle control bit set: it "
           "is the page this service carries its subtitles on. Use it as the "
           "subtitle page when exporting subtitles.")
            .arg(label));
  }

  pages_table_->item(row, kColumnSeen)
      ->setText(QString::number(static_cast<qulonglong>(entry.times_seen)));

  // Frame numbers are 1-based in the UI (see frame_numbering.h). A page seen
  // once has no range to show.
  const qulonglong first = static_cast<qulonglong>(entry.first_seen_frame) + 1;
  const qulonglong last = static_cast<qulonglong>(entry.last_seen_frame) + 1;
  pages_table_->item(row, kColumnFrame)
      ->setText(first == last ? QString::number(first)
                              : QStringLiteral("%1-%2").arg(first).arg(last));
}

void TeletextDialog::refreshPageList() {
  // The catalogue is page-address ordered; re-sort so the pages a receiver
  // could select come first and the hex-digit ones settle below them.
  auto entries = data_.pages;
  std::sort(entries.begin(), entries.end(),
            [](const orc::presenters::TeletextCataloguedPageView& lhs,
               const orc::presenters::TeletextCataloguedPageView& rhs) {
              return pageSortKey(lhs.magazine, lhs.page_number) <
                     pageSortKey(rhs.magazine, rhs.page_number);
            });

  // One trigger run delivers the catalogue whole, so the table is built once
  // per delivery rather than merged: there is no playback to keep it still
  // for, and rebuilding keeps the row order and the data in step.
  updating_list_ = true;
  pages_table_->clearContents();
  pages_table_->setRowCount(static_cast<int>(entries.size()));
  QStringList subtitle_pages;
  int row = 0;
  for (const auto& entry : entries) {
    if (entry.subtitle) {
      subtitle_pages << formatPageLabel(entry.magazine, entry.page_number);
    }
    createPageRow(row, entry);
    updatePageRow(row, entry);
    ++row;
  }
  updating_list_ = false;

  // Plural in the general case: a multi-service recording, or a service
  // running subtitles in more than one language, declares C6 on each page it
  // uses for them.
  if (subtitle_pages.isEmpty()) {
    subtitle_hint_->setVisible(false);
    subtitle_hint_->clear();
  } else {
    subtitle_hint_->setText(
        tr("Subtitles on %1").arg(subtitle_pages.join(QStringLiteral(", "))));
    subtitle_hint_->setToolTip(
        tr("These pages were transmitted with the C6 subtitle control bit set "
           "(ETSI EN 300 706 §9.3.1.3). The broadcast convention is page 888, "
           "but a recording may carry its subtitles anywhere — use what is "
           "listed here as the subtitle page when exporting subtitles."));
    subtitle_hint_->setVisible(true);
  }
}

void TeletextDialog::syncListSelection(const QString& page_label) {
  int match = -1;
  for (int row = 0; row < pages_table_->rowCount(); ++row) {
    if (pages_table_->item(row, kColumnPage)->data(Qt::UserRole).toString() ==
        page_label) {
      match = row;
      break;
    }
  }
  if (match == pages_table_->currentRow()) {
    return;  // already current; re-selecting would fight the user's scroll
  }
  updating_list_ = true;
  if (match >= 0) {
    pages_table_->setCurrentCell(match, kColumnPage);
  } else {
    pages_table_->setCurrentCell(-1, -1);
    pages_table_->clearSelection();
  }
  updating_list_ = false;
}

void TeletextDialog::renderPage() {
  const auto page_address = orc::TeletextPageDecoder::parse_page_number(
      page_edit_->text().toStdString());
  if (!page_address) {
    current_page_.reset();
    seen_label_->setText(tr("Invalid page number (e.g. 100, 888)"));
    recovery_label_->setVisible(false);
    page_widget_->clearPage();
    syncListSelection(QString());
    return;
  }

  const QString label =
      formatPageLabel(page_address->first, page_address->second);
  const auto* entry = findPage(page_address->first, page_address->second);
  if (entry != nullptr) {
    current_page_ = entry->page;
    // 1-based frame numbering in the UI (see frame_numbering.h).
    seen_label_->setText(
        tr("Page %1 seen %n time(s), frames %2-%3", nullptr,
           static_cast<int>(entry->times_seen))
            .arg(label)
            .arg(static_cast<qulonglong>(entry->first_seen_frame) + 1)
            .arg(static_cast<qulonglong>(entry->last_seen_frame) + 1));
    recovery_label_->setText(formatRecovery(*current_page_));
    recovery_label_->setVisible(true);
    page_widget_->setPage(*current_page_);
  } else {
    current_page_.reset();
    if (!has_data_) {
      seen_label_->setText(tr("No page data"));
    } else {
      seen_label_->setText(pages_table_->rowCount() == 0
                               ? tr("No teletext pages were recovered")
                               : tr("Page %1 was not carried").arg(label));
    }
    recovery_label_->setVisible(false);
    page_widget_->clearPage();
  }
  syncListSelection(label);
}
