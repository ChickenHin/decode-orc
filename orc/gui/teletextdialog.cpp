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

#include <QHBoxLayout>
#include <QListWidgetItem>
#include <QVBoxLayout>
#include <cstddef>

#include "teletextpagewidget.h"

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
  page_row->addStretch();

  // A row that was never recovered renders exactly like a transmitted blank
  // row, so the overlay is the only way to tell a recovery gap from page
  // content. Off by default: the markers are not part of the page.
  show_errors_check_ = new QCheckBox(tr("Show data errors"), this);
  show_errors_check_->setObjectName("teletextShowErrorsCheck");
  show_errors_check_->setToolTip(
      tr("Hatch rows no teletext packet was recovered for, and outline "
         "characters whose transmitted byte failed its parity check."));
  connect(show_errors_check_, &QCheckBox::toggled, this,
          &TeletextDialog::onShowErrorsToggled);
  page_row->addWidget(show_errors_check_);
  content_layout->addLayout(page_row);

  auto* body_row = new QHBoxLayout();

  // Left column: the pages seen since the last discontinuity. Teletext is a
  // carousel, so which pages exist is itself a discovery — the list is how
  // the user finds out, rather than having to guess page numbers.
  auto* list_column = new QVBoxLayout();
  auto* list_heading = new QLabel(tr("Pages seen:"), this);
  list_column->addWidget(list_heading);

  pages_list_ = new QListWidget(this);
  pages_list_->setObjectName("teletextPagesList");
  pages_list_->setMinimumWidth(150);
  pages_list_->setMaximumWidth(200);
  pages_list_->setUniformItemSizes(true);
  pages_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  connect(pages_list_, &QListWidget::itemSelectionChanged, this,
          &TeletextDialog::onPageSelected);
  list_column->addWidget(pages_list_, /*stretch=*/1);
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
  const std::size_t outstanding = assembler_.framesNeedingData().size();
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
  labels.reserve(static_cast<std::size_t>(pages_list_->count()));
  for (int row = 0; row < pages_list_->count(); ++row) {
    labels.push_back(pages_list_->item(row)->data(Qt::UserRole).toString());
  }
  return labels;
}

QString TeletextDialog::recoveryText() const {
  return recovery_label_->isVisible() ? recovery_label_->text() : QString();
}

void TeletextDialog::onPageNumberChanged() { renderPage(); }

void TeletextDialog::onShowErrorsToggled(bool checked) {
  page_widget_->setShowDataErrors(checked);
}

QString TeletextDialog::formatRecovery(
    const orc::presenters::TeletextPageRecoveryView& recovery) {
  // Rows are reported as a fraction rather than as an error count: a page is
  // free to leave rows blank, so a row that carried no packet is a gap in
  // what was recovered, not necessarily a fault.
  const QString rows =
      tr("rows %1/%2").arg(recovery.rows_received).arg(recovery.rows_expected);
  if (recovery.damaged_bytes == 0) {
    return recovery.rows_received == recovery.rows_expected
               ? tr("Complete (%1)").arg(rows)
               : rows;
  }
  return tr("%1, %n damaged byte(s)", nullptr, recovery.damaged_bytes)
      .arg(rows);
}

void TeletextDialog::onPageSelected() {
  if (updating_list_) {
    return;  // programmatic selection following the page-number entry
  }
  const auto* item = pages_list_->currentItem();
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

void TeletextDialog::refreshPageList() {
  const uint64_t revision = assembler_.catalogueRevision();
  if (list_populated_ && revision == listed_revision_) {
    return;  // nothing new seen; leave the user's selection alone
  }
  listed_revision_ = revision;
  list_populated_ = true;

  updating_list_ = true;
  pages_list_->clear();
  for (const auto& listing : assembler_.cataloguedPages()) {
    const QString label =
        formatPageLabel(listing.magazine, listing.page_number);
    // Frame numbers are 1-based in the UI (see frame_numbering.h).
    auto* item = new QListWidgetItem(
        tr("%1 — frame %2")
            .arg(label)
            .arg(static_cast<qulonglong>(listing.seen_frame) + 1),
        pages_list_);
    item->setData(Qt::UserRole, label);
  }
  updating_list_ = false;
}

void TeletextDialog::syncListSelection(const QString& page_label) {
  updating_list_ = true;
  QListWidgetItem* match = nullptr;
  for (int row = 0; row < pages_list_->count(); ++row) {
    QListWidgetItem* item = pages_list_->item(row);
    if (item->data(Qt::UserRole).toString() == page_label) {
      match = item;
      break;
    }
  }
  if (match != nullptr) {
    pages_list_->setCurrentItem(match);
  } else {
    pages_list_->setCurrentItem(nullptr);
    pages_list_->clearSelection();
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
        tr("Page %1 last seen at frame %2")
            .arg(label)
            .arg(static_cast<qulonglong>(entry->seen_frame) + 1));
    recovery_label_->setText(formatRecovery(current_page_->recovery));
    recovery_label_->setVisible(true);
    page_widget_->setPage(*current_page_);
  } else {
    current_page_.reset();
    seen_label_->setText(pages_list_->count() == 0
                             ? tr("No teletext pages seen yet")
                             : tr("Page %1 not seen yet").arg(label));
    recovery_label_->setVisible(false);
    page_widget_->clearPage();
  }
  syncListSelection(label);
}
