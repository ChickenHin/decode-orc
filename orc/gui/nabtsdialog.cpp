/*
 * File:        nabtsdialog.cpp
 * Module:      orc-gui
 * Purpose:     NABTS record viewer for the nabts_sink stage tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabtsdialog.h"

#include <QAbstractItemView>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstddef>

#include "nabtscanvaswidget.h"

namespace {

using orc::presenters::NabtsCaptionCueView;
using orc::presenters::NabtsCatalogueRecordView;

// SMPTE 170M: 525-line NTSC scans 59.94 fields per second, so a frame is
// 1001/30000 of a second. Caption cue extents are frame counts, and this is
// what turns one into a time a reader recognises.
constexpr double kNtscFramesPerSecond = 30000.0 / 1001.0;

// Frame numbers are 1-based in the UI (see frame_numbering.h); the view models
// carry them 0-based.
qulonglong to_ui_frame(uint64_t frame) {
  return static_cast<qulonglong>(frame) + 1;
}

QString frame_range_text(uint64_t first, uint64_t last) {
  const qulonglong from = to_ui_frame(first);
  const qulonglong to = to_ui_frame(last);
  return from == to ? QString::number(from)
                    : QStringLiteral("%1-%2").arg(from).arg(to);
}

/// A frame index as a wall-clock position, HH:MM:SS.mmm.
QString frame_time_text(uint64_t frame) {
  const double seconds_total =
      static_cast<double>(frame) / kNtscFramesPerSecond;
  const qint64 millis_total =
      static_cast<qint64>(std::llround(seconds_total * 1000.0));
  return QStringLiteral("%1:%2:%3.%4")
      .arg(millis_total / 3'600'000, 2, 10, QLatin1Char('0'))
      .arg((millis_total / 60'000) % 60, 2, 10, QLatin1Char('0'))
      .arg((millis_total / 1'000) % 60, 2, 10, QLatin1Char('0'))
      .arg(millis_total % 1'000, 3, 10, QLatin1Char('0'));
}

/// Short form of a record type for the table, which has no room for the full
/// name (CEA-516 §5.2.2).
QString short_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return QObject::tr("Cyclic");
    case 1:
      return QObject::tr("Non-cyclic");
    case 2:
      return QObject::tr("Application");
    case 3:
      return QObject::tr("Priority");
    default:
      break;
  }
  return QObject::tr("Type %1").arg(type);
}

/// The classification flags a record declared (§5.2.7.2), as a list.
QStringList classification_flags(const NabtsCatalogueRecordView& record) {
  QStringList flags;
  if (record.caption) flags << QObject::tr("caption");
  if (record.cyclic_marker) flags << QObject::tr("cyclic marker");
  if (record.priority) flags << QObject::tr("priority");
  if (record.alarm) flags << QObject::tr("alarm");
  if (record.update) flags << QObject::tr("update");
  if (record.support_record) flags << QObject::tr("support record");
  if (record.index) flags << QObject::tr("index");
  if (record.more) flags << QObject::tr("more");
  return flags;
}

}  // namespace

NabtsDialog::NabtsDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("NABTS Records"));

  // Qt::Window so the viewer can be positioned independently of the main
  // window, as the teletext viewer is.
  setWindowFlags(Qt::Window);

  resize(880, 600);
  setMinimumSize(640, 440);
}

NabtsDialog::~NabtsDialog() = default;

void NabtsDialog::setupUI() {
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto* content_layout = new QVBoxLayout();
  content_layout->setContentsMargins(9, 9, 9, 9);

  // Top row: navigation, the caption notice, and the error overlay switch.
  auto* top_row = new QHBoxLayout();

  prev_record_button_ = new QToolButton(this);
  prev_record_button_->setObjectName("nabtsPrevRecordButton");
  prev_record_button_->setArrowType(Qt::LeftArrow);
  prev_record_button_->setToolTip(
      tr("Show the previous record, wrapping round at the start of the "
         "catalogue."));
  connect(prev_record_button_, &QToolButton::clicked, this,
          [this] { stepRecord(-1); });
  top_row->addWidget(prev_record_button_);

  next_record_button_ = new QToolButton(this);
  next_record_button_->setObjectName("nabtsNextRecordButton");
  next_record_button_->setArrowType(Qt::RightArrow);
  next_record_button_->setToolTip(
      tr("Show the next record. Records are ordered by data channel and record "
         "address, which is the order CEA-516 §7.3 has a receiver step a "
         "service in when the service has not said otherwise."));
  connect(next_record_button_, &QToolButton::clicked, this,
          [this] { stepRecord(1); });
  top_row->addWidget(next_record_button_);

  // Which records carry the captioning is a property of the recording, so it is
  // stated rather than left to be found by reading the flags column.
  caption_hint_ = new QLabel(this);
  caption_hint_->setObjectName("nabtsCaptionHint");
  caption_hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  caption_hint_->setVisible(false);
  top_row->addWidget(caption_hint_);

  show_captions_check_ = new QCheckBox(tr("Caption track"), this);
  show_captions_check_->setObjectName("nabtsShowCaptionsCheck");
  show_captions_check_->setToolTip(
      tr("Show the caption service as a list of cues with their timings "
         "instead of the selected record. CEA-516 §7.3.10 carries captioning "
         "as a run of records that each replace the last, so the cues are what "
         "the service actually says."));
  show_captions_check_->setEnabled(false);
  connect(show_captions_check_, &QCheckBox::toggled, this,
          &NabtsDialog::onShowCaptionsToggled);
  top_row->addWidget(show_captions_check_);

  top_row->addStretch();

  show_errors_check_ = new QCheckBox(tr("Show display area"), this);
  show_errors_check_->setObjectName("nabtsShowErrorsCheck");
  show_errors_check_->setToolTip(
      tr("Outline the part of the screen a receiver is guaranteed to show "
         "(the lower 0.78125 of the unit screen, ANSI X3.110 Table D1). A "
         "record drawn into one corner is otherwise hard to tell from one that "
         "was mis-scaled."));
  connect(show_errors_check_, &QCheckBox::toggled, this,
          &NabtsDialog::onShowErrorsToggled);
  top_row->addWidget(show_errors_check_);
  content_layout->addLayout(top_row);

  auto* splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setObjectName("nabtsSplitter");
  splitter->setChildrenCollapsible(false);

  // Left: every record the analysed range carried. Which records exist is
  // itself a discovery — a NABTS service has no page numbers to guess at.
  auto* list_column = new QWidget(splitter);
  auto* list_layout = new QVBoxLayout(list_column);
  list_layout->setContentsMargins(0, 0, 0, 0);
  list_layout->addWidget(new QLabel(tr("Records carried:"), list_column));

  records_table_ = new QTableWidget(0, kColumnCount, list_column);
  records_table_->setObjectName("nabtsRecordsTable");
  records_table_->setHorizontalHeaderLabels(
      {tr("Record"), tr("Type"), tr("Seen"), tr("Frames")});
  records_table_->horizontalHeaderItem(kColumnAddress)
      ->setToolTip(tr("Data channel, record address and version — the identity "
                      "CEA-516 §5.2.1 gives a record."));
  records_table_->horizontalHeaderItem(kColumnSeen)
      ->setToolTip(tr("How many copies of this record the analysed range "
                      "carried, and how many of those arrived undamaged."));
  records_table_->horizontalHeaderItem(kColumnFrame)
      ->setToolTip(tr("Frames carrying the first and last copy."));
  for (const int column : {kColumnSeen, kColumnFrame}) {
    records_table_->horizontalHeaderItem(column)->setTextAlignment(
        Qt::AlignRight | Qt::AlignVCenter);
  }
  records_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  records_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  records_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  records_table_->setShowGrid(false);
  records_table_->setAlternatingRowColors(true);
  records_table_->setWordWrap(false);
  records_table_->setCornerButtonEnabled(false);
  records_table_->verticalHeader()->setVisible(false);
  records_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  auto* header = records_table_->horizontalHeader();
  header->setSectionResizeMode(kColumnAddress, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColumnType, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColumnSeen, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(kColumnFrame, QHeaderView::Stretch);
  connect(records_table_, &QTableWidget::itemSelectionChanged, this,
          &NabtsDialog::onRecordSelected);
  list_layout->addWidget(records_table_, /*stretch=*/1);
  splitter->addWidget(list_column);

  // Right: whatever the selected record is. A presentation record is drawn, an
  // application record listed, and the caption track replaces both on request —
  // three genuinely different things, so a stack rather than one pane that has
  // to mean all of them.
  detail_stack_ = new QStackedWidget(splitter);
  detail_stack_->setObjectName("nabtsDetailStack");

  auto* canvas_page = new QWidget(detail_stack_);
  auto* canvas_layout = new QVBoxLayout(canvas_page);
  canvas_layout->setContentsMargins(0, 0, 0, 0);
  canvas_ = new NabtsCanvasWidget(canvas_page);
  canvas_layout->addWidget(canvas_, /*stretch=*/3);

  // The drawing is the record; the text is how it is read. A caption or an
  // index page is mostly words, and picking them off a rasterised page is a
  // poor way to read them.
  text_pane_ = new QPlainTextEdit(canvas_page);
  text_pane_->setObjectName("nabtsTextPane");
  text_pane_->setReadOnly(true);
  text_pane_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  text_pane_->setPlaceholderText(tr("This record drew no text."));
  text_pane_->setMaximumHeight(120);
  canvas_layout->addWidget(text_pane_, /*stretch=*/1);
  detail_stack_->insertWidget(kPageCanvas, canvas_page);

  functions_pane_ = new QPlainTextEdit(detail_stack_);
  functions_pane_->setObjectName("nabtsFunctionsPane");
  functions_pane_->setReadOnly(true);
  functions_pane_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  detail_stack_->insertWidget(kPageFunctions, functions_pane_);

  captions_table_ = new QTableWidget(0, 3, detail_stack_);
  captions_table_->setObjectName("nabtsCaptionsTable");
  captions_table_->setHorizontalHeaderLabels(
      {tr("Frames"), tr("Time"), tr("Caption")});
  captions_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  captions_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  captions_table_->setAlternatingRowColors(true);
  captions_table_->verticalHeader()->setVisible(false);
  captions_table_->horizontalHeader()->setSectionResizeMode(
      2, QHeaderView::Stretch);
  detail_stack_->insertWidget(kPageCaptions, captions_table_);

  empty_label_ = new QLabel(tr("Select a record"), detail_stack_);
  empty_label_->setObjectName("nabtsEmptyLabel");
  empty_label_->setAlignment(Qt::AlignCenter);
  detail_stack_->insertWidget(kPageNothing, empty_label_);
  detail_stack_->setCurrentIndex(kPageNothing);

  splitter->addWidget(detail_stack_);
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({260, 620});
  content_layout->addWidget(splitter, /*stretch=*/1);

  // How the run went as a whole: an empty record list means something quite
  // different on a recording that yielded no packets than on one whose packets
  // never assembled into a group.
  summary_label_ = new QLabel(this);
  summary_label_->setObjectName("nabtsSummaryLabel");
  summary_label_->setWordWrap(true);
  summary_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  summary_label_->setVisible(false);
  content_layout->addWidget(summary_label_);

  main_layout->addLayout(content_layout, /*stretch=*/1);

  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("nabtsStatusBar");
  status_bar_->setSizeGripEnabled(false);

  seen_label_ = new QLabel(tr("No record data"), this);
  seen_label_->setObjectName("nabtsSeenLabel");
  status_bar_->addWidget(seen_label_, /*stretch=*/1);

  detail_label_ = new QLabel(this);
  detail_label_->setObjectName("nabtsDetailLabel");
  status_bar_->addPermanentWidget(detail_label_);
  detail_label_->setVisible(false);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_bar_->addPermanentWidget(status_label_);
  status_label_->setVisible(false);

  main_layout->addWidget(status_bar_);
}

void NabtsDialog::showPending() {
  status_label_->setText(tr("Decoding…"));
  status_label_->setVisible(true);
}

void NabtsDialog::clearContent() {
  data_ = orc::presenters::NabtsAnalysisView{};
  has_data_ = false;
  current_index_ = -1;
  status_label_->setVisible(false);
  status_label_->clear();
  summary_label_->setVisible(false);
  summary_label_->clear();
  show_captions_check_->setChecked(false);
  show_captions_check_->setEnabled(false);
  refreshRecordList();
  refreshCaptions();
  seen_label_->setText(tr("No record data"));
  detail_label_->setVisible(false);
  canvas_->clearPage();
  text_pane_->clear();
  functions_pane_->clear();
  detail_stack_->setCurrentIndex(kPageNothing);
}

void NabtsDialog::setAnalysisData(
    const orc::presenters::NabtsAnalysisView& data) {
  data_ = data;
  has_data_ = true;
  status_label_->setVisible(false);
  status_label_->clear();

  const QString summary = formatSummary(data_.summary);
  summary_label_->setText(summary);
  summary_label_->setVisible(!summary.isEmpty());

  refreshRecordList();
  refreshCaptions();

  // The first record rather than nothing: a catalogue with one record in it
  // would otherwise open on an empty pane.
  showRecord(data_.records.empty() ? -1 : 0);
}

std::vector<QString> NabtsDialog::listedRecords() const {
  std::vector<QString> labels;
  labels.reserve(static_cast<std::size_t>(records_table_->rowCount()));
  for (int row = 0; row < records_table_->rowCount(); ++row) {
    labels.push_back(records_table_->item(row, kColumnAddress)->text());
  }
  return labels;
}

const orc::presenters::NabtsCatalogueRecordView* NabtsDialog::currentRecord()
    const {
  if (current_index_ < 0 ||
      current_index_ >= static_cast<int>(data_.records.size())) {
    return nullptr;
  }
  return &data_.records[static_cast<std::size_t>(current_index_)];
}

void NabtsDialog::selectRecord(int index) { showRecord(index); }

void NabtsDialog::showNextRecord() { stepRecord(1); }

void NabtsDialog::showPreviousRecord() { stepRecord(-1); }

// The three readouts below report empty when their label is hidden. The test is
// isVisibleTo(this) rather than isVisible(): a dialogue that has never been
// shown has no visible widgets at all, and these have to answer the same way
// whether or not the window is on screen.
QString NabtsDialog::detailText() const {
  return detail_label_->isVisibleTo(this) ? detail_label_->text() : QString();
}

QString NabtsDialog::summaryText() const {
  return summary_label_->isVisibleTo(this) ? summary_label_->text() : QString();
}

QString NabtsDialog::captionHintText() const {
  return caption_hint_->isVisibleTo(this) ? caption_hint_->text() : QString();
}

std::vector<QString> NabtsDialog::listedCaptions() const {
  std::vector<QString> cues;
  cues.reserve(static_cast<std::size_t>(captions_table_->rowCount()));
  for (int row = 0; row < captions_table_->rowCount(); ++row) {
    cues.push_back(captions_table_->item(row, 2)->text());
  }
  return cues;
}

void NabtsDialog::onShowErrorsToggled(bool checked) {
  canvas_->setShowDataErrors(checked);
}

void NabtsDialog::onShowCaptionsToggled(bool checked) {
  if (checked) {
    detail_stack_->setCurrentIndex(kPageCaptions);
  } else {
    showRecord(current_index_);
  }
}

void NabtsDialog::onRecordSelected() {
  if (updating_list_) {
    return;  // programmatic selection
  }
  const int row = records_table_->currentRow();
  if (row < 0) {
    return;
  }
  showRecord(row);
}

QString NabtsDialog::formatIdentity(const NabtsCatalogueRecordView& record) {
  // §5.2.1: channel, record address and version together are the identity, so
  // all three are shown — two records at one address in different versions are
  // different records.
  return QStringLiteral("%1 v%2")
      .arg(QString::fromStdString(record.channel_text))
      .arg(record.version, 1, 16)
      .toUpper();
}

QString NabtsDialog::formatDetail(const NabtsCatalogueRecordView& record) {
  QStringList parts;
  if (!record.complete) {
    // §5.2.6: a message missing a link renders short, and there is no way to
    // tell that from a record the service meant to be short.
    parts << QObject::tr("incomplete");
  } else if (record.times_intact == 0) {
    parts << QObject::tr("no undamaged copy");
  } else {
    parts << QObject::tr("complete");
  }
  if (record.records_in_message > 1) {
    parts << QObject::tr("%n linked record(s)", nullptr,
                         static_cast<int>(record.records_in_message));
  }
  parts << QObject::tr("%n byte(s)", nullptr,
                       static_cast<int>(record.data_bytes));

  if (record.presentation) {
    const auto& recovery = record.page.recovery;
    if (recovery.truncated_pdis > 0) {
      parts << QObject::tr("%n truncated instruction(s)", nullptr,
                           static_cast<int>(recovery.truncated_pdis));
    }
    if (recovery.unresolved_macros > 0) {
      parts << QObject::tr("%n unresolved macro(s)", nullptr,
                           static_cast<int>(recovery.unresolved_macros));
    }
    if (recovery.storage_refusals > 0) {
      parts << QObject::tr("%n definition(s) over the storage budget", nullptr,
                           static_cast<int>(recovery.storage_refusals));
    }
  }
  return parts.join(QObject::tr(", "));
}

QString NabtsDialog::formatSummary(
    const orc::presenters::NabtsRecoverySummaryView& summary) {
  if (summary.frames_analysed == 0 && summary.packets_recovered == 0) {
    return {};
  }

  QStringList parts;
  parts << tr("%1 packets recovered from %2 fields over %3 frames")
               .arg(static_cast<qulonglong>(summary.packets_recovered))
               .arg(static_cast<qulonglong>(summary.fields_with_data))
               .arg(static_cast<qulonglong>(summary.frames_analysed));
  parts << tr("%1 data groups complete, %2 incomplete")
               .arg(static_cast<qulonglong>(summary.groups_completed))
               .arg(static_cast<qulonglong>(summary.groups_incomplete));
  parts << tr("%1 records complete, %2 partial")
               .arg(static_cast<qulonglong>(summary.messages_complete))
               .arg(static_cast<qulonglong>(summary.messages_partial));
  if (summary.packets_prefix_rejected > 0) {
    // §3.2.2: a packet whose Hamming prefix will not decode cannot even be
    // filed under a channel, so it is lost before any of the above.
    parts << tr("%1 packets refused on their prefix")
                 .arg(static_cast<qulonglong>(summary.packets_prefix_rejected));
  }
  if (summary.blocks_corrected > 0 || summary.blocks_damaged > 0) {
    parts << tr("%1 blocks repaired, %2 beyond repair")
                 .arg(static_cast<qulonglong>(summary.blocks_corrected))
                 .arg(static_cast<qulonglong>(summary.blocks_damaged));
  }
  if (summary.lost_packets_estimate > 0) {
    parts << tr("about %1 packets lost")
                 .arg(static_cast<qulonglong>(summary.lost_packets_estimate));
  }
  if (summary.records_truncated) {
    parts << tr("record list truncated at the catalogue limit");
  }
  return parts.join(tr("; "));
}

QString NabtsDialog::formatFunctions(const NabtsCatalogueRecordView& record) {
  if (record.functions.empty()) {
    return QObject::tr(
        "This application record carried no function descriptors.");
  }
  QStringList lines;
  lines << QObject::tr("%n function descriptor(s) (CEA-516 §7.2.2):", nullptr,
                       static_cast<int>(record.functions.size()));
  lines << QString();
  for (const auto& function : record.functions) {
    const QString code = QString::fromStdString(function.code);
    const QString kind =
        function.control ? QObject::tr("control") : QObject::tr("information");
    const QString arguments = QString::fromStdString(function.arguments);
    if (arguments.isEmpty()) {
      // §7.2.3.1: a descriptor with no arguments asks for that function's
      // initial state back.
      lines << QStringLiteral("%1  [%2]  %3")
                   .arg(code, kind, QObject::tr("(restore initial state)"));
    } else {
      lines << QStringLiteral("%1  [%2]  %3").arg(code, kind, arguments);
    }
  }
  return lines.join(QLatin1Char('\n'));
}

void NabtsDialog::refreshRecordList() {
  // One trigger run delivers the catalogue whole, so the table is built once
  // per delivery rather than merged.
  updating_list_ = true;
  records_table_->clearContents();
  records_table_->setRowCount(static_cast<int>(data_.records.size()));

  int row = 0;
  for (const auto& record : data_.records) {
    auto* address_item = new QTableWidgetItem(formatIdentity(record));
    auto* type_item = new QTableWidgetItem(short_type_name(record.record_type));
    auto* seen_item = new QTableWidgetItem(
        QString::number(static_cast<qulonglong>(record.times_seen)));
    seen_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* frame_item = new QTableWidgetItem(
        frame_range_text(record.first_seen_frame, record.last_seen_frame));
    frame_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

    QStringList tips;
    if (!record.reserved_purpose.empty()) {
      // §7.1.5 reserves a handful of channel/address pairings; a reader has no
      // way of knowing which without being told.
      tips << tr("CEA-516 §7.1.5 reserves this address for: %1")
                  .arg(QString::fromStdString(record.reserved_purpose));
    }
    const QStringList flags = classification_flags(record);
    if (!flags.isEmpty()) {
      tips << tr("Classification flags (§5.2.7.2): %1")
                  .arg(flags.join(QStringLiteral(", ")));
    }
    tips << tr("%1 of %2 copies arrived undamaged")
                .arg(static_cast<qulonglong>(record.times_intact))
                .arg(static_cast<qulonglong>(record.times_seen));
    const QString tip = tips.join(QStringLiteral("\n\n"));
    for (auto* item : {address_item, type_item, seen_item, frame_item}) {
      item->setToolTip(tip);
    }

    records_table_->setItem(row, kColumnAddress, address_item);
    records_table_->setItem(row, kColumnType, type_item);
    records_table_->setItem(row, kColumnSeen, seen_item);
    records_table_->setItem(row, kColumnFrame, frame_item);
    ++row;
  }
  updating_list_ = false;

  const bool navigable = data_.records.size() > 1;
  prev_record_button_->setEnabled(navigable);
  next_record_button_->setEnabled(navigable);
}

void NabtsDialog::refreshCaptions() {
  captions_table_->clearContents();
  captions_table_->setRowCount(static_cast<int>(data_.captions.size()));

  int row = 0;
  for (const NabtsCaptionCueView& cue : data_.captions) {
    auto* frames =
        new QTableWidgetItem(frame_range_text(cue.start_frame, cue.end_frame));
    frames->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto* time = new QTableWidgetItem(QStringLiteral("%1 → %2").arg(
        frame_time_text(cue.start_frame), frame_time_text(cue.end_frame)));
    // A cue is often two lines; the table shows it on one so the list stays
    // scannable, and the tooltip has it as transmitted.
    QString text = QString::fromStdString(cue.text);
    auto* caption = new QTableWidgetItem(
        text.replace(QLatin1Char('\n'), QStringLiteral(" ")));
    caption->setToolTip(QString::fromStdString(cue.text));
    captions_table_->setItem(row, 0, frames);
    captions_table_->setItem(row, 1, time);
    captions_table_->setItem(row, 2, caption);
    ++row;
  }

  const bool have_captions = !data_.captions.empty();
  show_captions_check_->setEnabled(have_captions);
  if (!have_captions) {
    show_captions_check_->setChecked(false);
    caption_hint_->setVisible(false);
    caption_hint_->clear();
    return;
  }

  // Which channel carries them, because §7.3.10 makes A00 the entry point but
  // the captions themselves are wherever the Caption Flag is set.
  QStringList channels;
  for (const NabtsCaptionCueView& cue : data_.captions) {
    const QString label = QStringLiteral("%1/%2")
                              .arg(cue.channel, 3, 16, QLatin1Char('0'))
                              .arg(QString::fromStdString(cue.address_text))
                              .toUpper();
    if (!channels.contains(label)) {
      channels << label;
    }
  }
  caption_hint_->setText(tr("%n caption(s) on %1", nullptr,
                            static_cast<int>(data_.captions.size()))
                             .arg(channels.join(QStringLiteral(", "))));
  caption_hint_->setToolTip(
      tr("These records were transmitted with the Caption Flag set (CEA-516 "
         "§5.2.7.3), which is what makes a record part of the captioning "
         "service of §7.3.10. Tick \"Caption track\" to read them in order."));
  caption_hint_->setVisible(true);
}

void NabtsDialog::showRecord(int index) {
  const bool valid =
      index >= 0 && index < static_cast<int>(data_.records.size());
  current_index_ = valid ? index : -1;

  // Keep the table in step without the selection feeding back into here.
  updating_list_ = true;
  if (valid) {
    records_table_->setCurrentCell(index, kColumnAddress);
  } else {
    records_table_->setCurrentCell(-1, -1);
    records_table_->clearSelection();
  }
  updating_list_ = false;

  if (show_captions_check_->isChecked()) {
    detail_stack_->setCurrentIndex(kPageCaptions);
  }

  if (!valid) {
    canvas_->clearPage();
    text_pane_->clear();
    functions_pane_->clear();
    detail_label_->setVisible(false);
    seen_label_->setText(has_data_ ? tr("No NABTS records were recovered")
                                   : tr("No record data"));
    if (!show_captions_check_->isChecked()) {
      detail_stack_->setCurrentIndex(kPageNothing);
    }
    return;
  }

  const auto& record = data_.records[static_cast<std::size_t>(index)];

  seen_label_->setText(tr("Record %1 (%2) seen %n time(s), frames %3", nullptr,
                          static_cast<int>(record.times_seen))
                           .arg(formatIdentity(record),
                                QString::fromStdString(record.record_type_name),
                                frame_range_text(record.first_seen_frame,
                                                 record.last_seen_frame)));
  detail_label_->setText(formatDetail(record));
  detail_label_->setVisible(true);

  if (record.presentation) {
    canvas_->setPage(record.page);
    text_pane_->setPlainText(QString::fromStdString(record.page.text));
    if (!show_captions_check_->isChecked()) {
      detail_stack_->setCurrentIndex(kPageCanvas);
    }
  } else {
    canvas_->clearPage();
    functions_pane_->setPlainText(formatFunctions(record));
    if (!show_captions_check_->isChecked()) {
      detail_stack_->setCurrentIndex(kPageFunctions);
    }
  }
}

void NabtsDialog::stepRecord(int delta) {
  const int count = static_cast<int>(data_.records.size());
  if (count == 0) {
    return;
  }
  // Wrapping, because a cyclic service does: stepping past the last record is
  // what the carousel does next.
  const int base = current_index_ < 0 ? 0 : current_index_;
  showRecord(((base + delta) % count + count) % count);
}
