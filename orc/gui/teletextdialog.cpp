/*
 * File:        teletextdialog.cpp
 * Module:      orc-gui
 * Purpose:     Teletext page preview dialog (observer dialog)
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
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cstddef>

#include "teletextpagewidget.h"

namespace {

// Role carrying a row's page-order key, so the table can be merged against a
// freshly sorted catalogue without re-parsing the displayed labels.
constexpr int kSortKeyRole = Qt::UserRole + 1;

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

  // Don't destroy on close, just hide
  setAttribute(Qt::WA_DeleteOnClose, false);

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

  // Left column: the pages seen since the last discontinuity. Teletext is a
  // carousel, so which pages exist is itself a discovery — the table is how
  // the user finds out, rather than having to guess page numbers.
  auto* list_column = new QVBoxLayout();
  auto* list_heading = new QLabel(tr("Pages seen:"), this);
  list_column->addWidget(list_heading);

  pages_table_ = new QTableWidget(0, kColumnCount, this);
  pages_table_->setObjectName("teletextPagesTable");
  pages_table_->setHorizontalHeaderLabels(
      {tr("Page"), tr("Seen"), tr("Frame")});
  pages_table_->horizontalHeaderItem(kColumnSeen)
      ->setToolTip(tr("How many times the carousel has brought this page "
                      "round since the last seek."));
  pages_table_->horizontalHeaderItem(kColumnFrame)
      ->setToolTip(tr("Frame carrying the most recent transmission."));
  // The two numeric columns are right-aligned so their digits line up; their
  // headings follow them.
  for (const int column : {kColumnSeen, kColumnFrame}) {
    pages_table_->horizontalHeaderItem(column)->setTextAlignment(
        Qt::AlignRight | Qt::AlignVCenter);
  }
  pages_table_->setMinimumWidth(200);
  pages_table_->setMaximumWidth(280);
  pages_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  pages_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  pages_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  pages_table_->setShowGrid(false);
  pages_table_->setAlternatingRowColors(true);
  pages_table_->setWordWrap(false);
  pages_table_->setCornerButtonEnabled(false);
  pages_table_->verticalHeader()->setVisible(false);
  // Per-pixel scrolling keeps the view anchored where the user left it while
  // rows are merged in underneath during playback.
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

  main_layout->addLayout(content_layout, /*stretch=*/1);

  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("teletextStatusBar");
  status_bar_->setSizeGripEnabled(false);

  // Page status on the left, observation progress on the right; both live in
  // the status bar so a message appearing never reflows the page display.
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

void TeletextDialog::showPending() { updatePendingStatus(); }

void TeletextDialog::updatePendingStatus() {
  // The whole outstanding count, not the capped batch, so the readout counts
  // down towards zero instead of sitting at the batch size.
  const std::size_t outstanding = assembler_.framesNeedingDataCount();
  if (outstanding == 0) {
    status_label_->setVisible(false);
    status_label_->clear();
    return;
  }
  status_label_->setText(
      tr("Reading %1 frames…").arg(static_cast<qulonglong>(outstanding)));
  status_label_->setVisible(true);
}

void TeletextDialog::clearContent() {
  assembler_.clear();
  current_page_.reset();
  list_populated_ = false;
  status_label_->setVisible(false);
  status_label_->clear();
  refreshPageList();
  seen_label_->setText(tr("No page data"));
  recovery_label_->setVisible(false);
  page_widget_->clearPage();
}

void TeletextDialog::clearCache() {
  assembler_.clear();
  list_populated_ = false;
  renderPage();
}

void TeletextDialog::setCurrentFrame(uint64_t frame_index) {
  assembler_.setCurrentFrame(frame_index);
  renderPage();
}

void TeletextDialog::deliverFrameData(
    bool available, uint64_t field1_id_value,
    const orc::presenters::TeletextFieldPacketsView& field1,
    const orc::presenters::TeletextFieldPacketsView& field2) {
  const uint64_t frame_index = field1_id_value / 2;
  if (available) {
    assembler_.storeFrame(frame_index, field1, field2);
  } else {
    assembler_.markFrameUnavailable(frame_index);
  }
  updatePendingStatus();
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

  // A page part-way through its transmission looks exactly like a finished
  // one with rows missing — teletext is sent a packet at a time, and on a
  // sparse insertion a single page takes several frames to arrive. Saying so
  // is the difference between "this recording is damaged" and "wait".
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

void TeletextDialog::createPageRow(
    int row, const TeletextPageAssembler::PageListing& listing) {
  const QString label = formatPageLabel(listing.magazine, listing.page_number);

  auto* page_item = new QTableWidgetItem(label);
  page_item->setData(Qt::UserRole, label);
  page_item->setData(kSortKeyRole,
                     pageSortKey(listing.magazine, listing.page_number));
  auto* seen_item = new QTableWidgetItem();
  seen_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
  auto* frame_item = new QTableWidgetItem();
  frame_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

  if (isNonSelectablePage(listing.page_number)) {
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
    int row, const TeletextPageAssembler::PageListing& listing) {
  auto* page_item = pages_table_->item(row, kColumnPage);

  // The subtitle flag is not known when the row is created — a page can be
  // listed from a transmission before the one that declares C6 — so the marker
  // is written here with the volatile columns. It is spelled out rather than
  // shown as a symbol because there is nowhere in this table to put a legend.
  const QString label = page_item->data(Qt::UserRole).toString();
  const QString page_text = listing.subtitle ? tr("%1 subs").arg(label) : label;
  if (page_item->text() != page_text) {
    page_item->setText(page_text);
    if (listing.subtitle) {
      page_item->setToolTip(
          tr("Page %1 is transmitted with the C6 subtitle control bit set: it "
             "is the page this service carries its subtitles on. Use it as the "
             "subtitle page when exporting subtitles.")
              .arg(label));
    }
  }

  // Frame numbers are 1-based in the UI (see frame_numbering.h).
  const QString seen = QString::number(listing.times_seen);
  // A page still arriving has no settled "last seen" frame yet; an ellipsis
  // marks it so a row that is about to change does not read as a final
  // answer.
  const QString frame =
      QString::number(static_cast<qulonglong>(listing.seen_frame) + 1) +
      (listing.transmission_complete ? QString() : QStringLiteral("…"));
  auto* seen_item = pages_table_->item(row, kColumnSeen);
  auto* frame_item = pages_table_->item(row, kColumnFrame);
  if (seen_item->text() != seen) {
    seen_item->setText(seen);
  }
  if (frame_item->text() != frame) {
    frame_item->setText(frame);
    frame_item->setToolTip(
        listing.transmission_complete
            ? QString()
            : tr("This page is still being transmitted; rows are still "
                 "arriving."));
  }
}

void TeletextDialog::refreshPageList() {
  const uint64_t revision = assembler_.catalogueRevision();
  if (list_populated_ && revision == listed_revision_) {
    return;  // nothing new seen; leave the user's selection alone
  }
  listed_revision_ = revision;
  list_populated_ = true;

  // The catalogue is page-address ordered; re-sort so the pages a receiver
  // could select come first and the hex-digit ones settle below them.
  auto listings = assembler_.cataloguedPages();
  std::sort(listings.begin(), listings.end(),
            [](const TeletextPageAssembler::PageListing& lhs,
               const TeletextPageAssembler::PageListing& rhs) {
              return pageSortKey(lhs.magazine, lhs.page_number) <
                     pageSortKey(rhs.magazine, rhs.page_number);
            });

  // Rows are merged in place rather than rebuilt. Playback bumps the
  // catalogue revision on almost every frame, and clearing the table each
  // time would drop the scroll position and the selection out from under a
  // user trying to click a row.
  updating_list_ = true;
  QStringList subtitle_pages;
  int row = 0;
  for (const auto& listing : listings) {
    if (listing.subtitle) {
      subtitle_pages << formatPageLabel(listing.magazine, listing.page_number);
    }
    const int key = pageSortKey(listing.magazine, listing.page_number);
    while (row < pages_table_->rowCount() &&
           pages_table_->item(row, kColumnPage)->data(kSortKeyRole).toInt() <
               key) {
      pages_table_->removeRow(row);  // page evicted from the catalogue
    }
    if (row >= pages_table_->rowCount() ||
        pages_table_->item(row, kColumnPage)->data(kSortKeyRole).toInt() !=
            key) {
      pages_table_->insertRow(row);
      createPageRow(row, listing);
    }
    updatePageRow(row, listing);
    ++row;
  }
  while (pages_table_->rowCount() > row) {
    pages_table_->removeRow(row);
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
    // Already current. Re-selecting would scroll the row back into view on
    // every delivered frame, fighting a user who has scrolled elsewhere.
    return;
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
  refreshPageList();

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
  const auto* entry =
      assembler_.findPage(page_address->first, page_address->second);
  if (entry != nullptr) {
    // Carousel media make random access approximate: report where the page
    // transmission was actually seen (1-based frame numbering in the UI).
    current_page_ = entry->page;
    seen_label_->setText(
        tr("Page %1 last seen at frame %2 (%n transmission(s))", nullptr,
           static_cast<int>(entry->times_seen))
            .arg(label)
            .arg(static_cast<qulonglong>(entry->seen_frame) + 1));
    recovery_label_->setText(formatRecovery(*current_page_));
    recovery_label_->setVisible(true);
    page_widget_->setPage(*current_page_);
  } else {
    current_page_.reset();
    seen_label_->setText(pages_table_->rowCount() == 0
                             ? tr("No teletext pages seen yet")
                             : tr("Page %1 not seen yet").arg(label));
    recovery_label_->setVisible(false);
    page_widget_->clearPage();
  }
  syncListSelection(label);
}
