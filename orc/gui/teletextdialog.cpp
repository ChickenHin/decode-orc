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
#include <QVBoxLayout>

#include "teletextpagewidget.h"

TeletextDialog::TeletextDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("Teletext Pages"));

  // Use Qt::Window flag to allow independent positioning
  setWindowFlags(Qt::Window);

  // Don't destroy on close, just hide
  setAttribute(Qt::WA_DeleteOnClose, false);

  resize(520, 520);
  setMinimumSize(360, 380);
}

TeletextDialog::~TeletextDialog() = default;

void TeletextDialog::setupUI() {
  auto* main_layout = new QVBoxLayout(this);

  // Pending-state notice (hidden until an async request is in flight).
  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_label_->setVisible(false);
  main_layout->addWidget(status_label_);

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
  main_layout->addLayout(page_row);

  seen_label_ = new QLabel(this);
  seen_label_->setObjectName("teletextSeenLabel");
  seen_label_->setText(tr("No page data"));
  main_layout->addWidget(seen_label_);

  page_widget_ = new TeletextPageWidget(this);
  main_layout->addWidget(page_widget_, /*stretch=*/1);
}

void TeletextDialog::showPending() {
  status_label_->setText(tr("Computing observations…"));
  status_label_->setVisible(true);
}

void TeletextDialog::clearContent() {
  assembler_.clear();
  current_page_.reset();
  status_label_->setVisible(false);
  seen_label_->setText(tr("No page data"));
  page_widget_->clearPage();
}

void TeletextDialog::clearCache() {
  assembler_.clear();
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
  }
  if (assembler_.framesNeedingData().empty()) {
    status_label_->setVisible(false);
  }
  renderPage();
}

QString TeletextDialog::pageNumberText() const { return page_edit_->text(); }

void TeletextDialog::setPageNumberText(const QString& text) {
  page_edit_->setText(text);
}

void TeletextDialog::onPageNumberChanged() { renderPage(); }

void TeletextDialog::renderPage() {
  const auto page_address = orc::TeletextPageDecoder::parse_page_number(
      page_edit_->text().toStdString());
  if (!page_address) {
    current_page_.reset();
    seen_label_->setText(tr("Invalid page number (e.g. 100, 888)"));
    page_widget_->clearPage();
    return;
  }

  current_page_ =
      assembler_.assemblePage(page_address->first, page_address->second);
  if (current_page_) {
    // Carousel media make random access approximate: report where the page
    // transmission was actually seen (1-based frame numbering in the UI).
    const uint64_t seen_frame =
        static_cast<uint64_t>(current_page_->header_field_index) / 2;
    seen_label_->setText(tr("Page last seen at frame %1").arg(seen_frame + 1));
    page_widget_->setPage(*current_page_);
  } else {
    seen_label_->setText(
        tr("Page not seen in the last %1 frames")
            .arg(TeletextPageAssembler::kTrailingWindowFrames));
    page_widget_->clearPage();
  }
}
