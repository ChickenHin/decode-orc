/*
 * File:        cataloguedialog.cpp
 * Module:      orc-gui
 * Purpose:     Generic browser for any stage that exposes ICatalogueResults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cataloguedialog.h"

#include <QAbstractItemView>
#include <QBrush>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPalette>
#include <QSplitter>
#include <QStringList>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cstddef>
#include <limits>

#include "cataloguecellgridwidget.h"
#include "cataloguedisplaylistwidget.h"

namespace {

/// The find box matches on a trimmed, case-folded key, which is as much
/// tolerance as a host can offer without knowing what the key means.
QString normalise_key(const QString& text) { return text.trimmed().toUpper(); }

QString to_qstring(const std::string& text) {
  return QString::fromStdString(text);
}

}  // namespace

CatalogueDialog::CatalogueDialog(QWidget* parent) : QDialog(parent) {
  setupUI();
  setWindowTitle(tr("Catalogue"));

  // Qt::Window so the viewer can be positioned independently of the main
  // window.
  setWindowFlags(Qt::Window);

  resize(880, 600);
  setMinimumSize(640, 440);
}

CatalogueDialog::~CatalogueDialog() = default;

void CatalogueDialog::setupUI() {
  // The status bar sits flush against the window edge, so the outer layout
  // carries no margins and the content above it supplies its own.
  auto* main_layout = new QVBoxLayout(this);
  main_layout->setContentsMargins(0, 0, 0, 0);
  main_layout->setSpacing(0);

  auto* content_layout = new QVBoxLayout();
  content_layout->setContentsMargins(9, 9, 9, 9);

  // Top row: the find box (where the schema asks for one), item navigation,
  // standing notices, and the damage overlay switch.
  auto* top_row = new QHBoxLayout();

  find_bar_ = new QWidget(this);
  auto* find_layout = new QHBoxLayout(find_bar_);
  find_layout->setContentsMargins(0, 0, 0, 0);
  find_label_ = new QLabel(find_bar_);
  find_layout->addWidget(find_label_);
  find_edit_ = new QLineEdit(find_bar_);
  find_edit_->setObjectName("catalogueFindEdit");
  find_edit_->setMaxLength(16);
  find_edit_->setFixedWidth(80);
  connect(find_edit_, &QLineEdit::textChanged, this,
          &CatalogueDialog::onFindTextChanged);
  find_layout->addWidget(find_edit_);
  find_bar_->setVisible(false);
  top_row->addWidget(find_bar_);

  item_nav_ = new QWidget(this);
  auto* nav_layout = new QHBoxLayout(item_nav_);
  nav_layout->setContentsMargins(0, 0, 0, 0);
  prev_item_button_ = new QToolButton(item_nav_);
  prev_item_button_->setObjectName("cataloguePrevItemButton");
  prev_item_button_->setArrowType(Qt::LeftArrow);
  connect(prev_item_button_, &QToolButton::clicked, this,
          [this] { stepItem(-1); });
  nav_layout->addWidget(prev_item_button_);
  next_item_button_ = new QToolButton(item_nav_);
  next_item_button_->setObjectName("catalogueNextItemButton");
  next_item_button_->setArrowType(Qt::RightArrow);
  connect(next_item_button_, &QToolButton::clicked, this,
          [this] { stepItem(1); });
  nav_layout->addWidget(next_item_button_);
  top_row->addWidget(item_nav_);

  notice_label_ = new QLabel(this);
  notice_label_->setObjectName("catalogueNoticeLabel");
  notice_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  notice_label_->setVisible(false);
  top_row->addWidget(notice_label_);
  top_row->addStretch();

  highlight_check_ = new QCheckBox(this);
  highlight_check_->setObjectName("catalogueHighlightCheck");
  highlight_check_->setVisible(false);
  connect(highlight_check_, &QCheckBox::toggled, this,
          &CatalogueDialog::onHighlightToggled);
  top_row->addWidget(highlight_check_);
  content_layout->addLayout(top_row);

  auto* body_row = new QHBoxLayout();

  // Left column: every item the analysed range carried. Which items exist is
  // itself a discovery, so the table is how the reader finds out rather than
  // having to guess.
  auto* list_column = new QVBoxLayout();
  list_heading_ = new QLabel(this);
  list_column->addWidget(list_heading_);

  items_table_ = new QTableWidget(0, 0, this);
  items_table_->setObjectName("catalogueItemsTable");
  items_table_->setMinimumWidth(240);
  items_table_->setMaximumWidth(380);
  items_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  items_table_->setSelectionMode(QAbstractItemView::SingleSelection);
  items_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  items_table_->setShowGrid(false);
  items_table_->setAlternatingRowColors(true);
  items_table_->setWordWrap(false);
  items_table_->setCornerButtonEnabled(false);
  items_table_->verticalHeader()->setVisible(false);
  items_table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
  connect(items_table_, &QTableWidget::itemSelectionChanged, this,
          &CatalogueDialog::onItemSelected);
  list_column->addWidget(items_table_, /*stretch=*/1);
  body_row->addLayout(list_column);

  // Right column: the payload, with the variant stepper beneath it. A variant
  // is a version of the item the service cycles through, so the control belongs
  // against the display it steps, not against the table of items.
  auto* payload_column = new QVBoxLayout();

  payload_stack_ = new QStackedWidget(this);
  payload_stack_->setObjectName("cataloguePayloadStack");

  empty_label_ = new QLabel(this);
  empty_label_->setAlignment(Qt::AlignCenter);
  empty_label_->setWordWrap(true);
  payload_stack_->insertWidget(kPageNothing, empty_label_);

  grid_widget_ = new CatalogueCellGridWidget(this);
  grid_widget_->setObjectName("catalogueCellGrid");
  payload_stack_->insertWidget(kPageCellGrid, grid_widget_);

  // A display list is often read as much as looked at, so a text form sits
  // beside it when the payload carries one.
  display_page_ = new QWidget(this);
  auto* display_layout = new QHBoxLayout(display_page_);
  display_layout->setContentsMargins(0, 0, 0, 0);
  auto* display_splitter = new QSplitter(Qt::Horizontal, display_page_);
  display_widget_ = new CatalogueDisplayListWidget(display_splitter);
  display_widget_->setObjectName("catalogueDisplayList");
  display_splitter->addWidget(display_widget_);
  companion_pane_ = new QPlainTextEdit(display_splitter);
  companion_pane_->setObjectName("catalogueCompanionPane");
  companion_pane_->setReadOnly(true);
  companion_pane_->setLineWrapMode(QPlainTextEdit::NoWrap);
  display_splitter->addWidget(companion_pane_);
  display_splitter->setStretchFactor(0, 3);
  display_splitter->setStretchFactor(1, 2);
  display_layout->addWidget(display_splitter);
  payload_stack_->insertWidget(kPageDisplayList, display_page_);

  text_pane_ = new QPlainTextEdit(this);
  text_pane_->setObjectName("catalogueTextPane");
  text_pane_->setReadOnly(true);
  text_pane_->setLineWrapMode(QPlainTextEdit::NoWrap);
  payload_stack_->insertWidget(kPageText, text_pane_);

  table_pane_ = new QTableWidget(0, 0, this);
  table_pane_->setObjectName("catalogueTablePane");
  table_pane_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_pane_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table_pane_->setAlternatingRowColors(true);
  table_pane_->verticalHeader()->setVisible(false);
  payload_stack_->insertWidget(kPageTable, table_pane_);

  payload_stack_->setCurrentIndex(kPageNothing);
  payload_column->addWidget(payload_stack_, /*stretch=*/1);

  variant_bar_ = new QWidget(this);
  variant_bar_->setObjectName("catalogueVariantBar");
  auto* variant_row = new QHBoxLayout(variant_bar_);
  variant_row->setContentsMargins(0, 0, 0, 0);
  variant_row->addStretch();
  prev_variant_button_ = new QToolButton(variant_bar_);
  prev_variant_button_->setObjectName("cataloguePrevVariantButton");
  prev_variant_button_->setArrowType(Qt::LeftArrow);
  connect(prev_variant_button_, &QToolButton::clicked, this,
          [this] { stepVariant(-1); });
  variant_row->addWidget(prev_variant_button_);
  variant_label_ = new QLabel(variant_bar_);
  variant_label_->setObjectName("catalogueVariantLabel");
  variant_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  variant_row->addWidget(variant_label_);
  next_variant_button_ = new QToolButton(variant_bar_);
  next_variant_button_->setObjectName("catalogueNextVariantButton");
  next_variant_button_->setArrowType(Qt::RightArrow);
  connect(next_variant_button_, &QToolButton::clicked, this,
          [this] { stepVariant(1); });
  variant_row->addWidget(next_variant_button_);
  variant_row->addStretch();
  variant_bar_->setVisible(false);
  payload_column->addWidget(variant_bar_);

  body_row->addLayout(payload_column, /*stretch=*/1);
  content_layout->addLayout(body_row, /*stretch=*/1);

  // How the run went as a whole: an empty item list means something quite
  // different on a recording that yielded nothing at all than on one whose data
  // never assembled into anything.
  summary_label_ = new QLabel(this);
  summary_label_->setObjectName("catalogueSummaryLabel");
  summary_label_->setWordWrap(true);
  summary_label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  summary_label_->setVisible(false);
  content_layout->addWidget(summary_label_);

  main_layout->addLayout(content_layout, /*stretch=*/1);

  status_bar_ = new QStatusBar(this);
  status_bar_->setObjectName("catalogueStatusBar");
  status_bar_->setSizeGripEnabled(false);

  // Item status on the left, condition and decode progress on the right; all
  // in the status bar so a message appearing never reflows the payload.
  headline_label_ = new QLabel(this);
  headline_label_->setObjectName("catalogueHeadlineLabel");
  status_bar_->addWidget(headline_label_, /*stretch=*/1);

  condition_label_ = new QLabel(this);
  condition_label_->setObjectName("catalogueConditionLabel");
  status_bar_->addPermanentWidget(condition_label_);
  condition_label_->setVisible(false);

  status_label_ = new QLabel(this);
  status_label_->setObjectName("observationStatusLabel");
  status_bar_->addPermanentWidget(status_label_);
  status_label_->setVisible(false);

  main_layout->addWidget(status_bar_);
}

void CatalogueDialog::showPending() {
  status_label_->setText(tr("Decoding…"));
  status_label_->setVisible(true);
}

void CatalogueDialog::clearContent() {
  data_ = orc::CatalogueDataset{};
  has_data_ = false;
  current_row_ = -1;
  variant_index_ = 0;
  status_label_->setVisible(false);
  status_label_->clear();
  summary_label_->setVisible(false);
  summary_label_->clear();
  refreshItemList();
  showItem(-1);
}

void CatalogueDialog::setCatalogue(const orc::CatalogueDataset& data) {
  if (!data.consistent()) {
    // A dataset whose payloads do not line up with its items is a plugin bug;
    // showing nothing beats indexing past the end of the vector.
    clearContent();
    headline_label_->setText(
        tr("The stage returned an inconsistent catalogue"));
    return;
  }

  // The find box's current text survives a fresh delivery of the same
  // catalogue, so a reader stepping a carousel is not thrown back to the top.
  const QString previous_key = normalise_key(find_edit_->text());

  data_ = data;
  has_data_ = true;
  status_label_->setVisible(false);
  status_label_->clear();

  // Schema-driven chrome, applied before the table is built from it.
  const auto& schema = data_.schema;
  const QString item_noun =
      schema.item_noun.empty() ? tr("Item") : to_qstring(schema.item_noun);
  list_heading_->setText(tr("%1s carried:").arg(item_noun));
  prev_item_button_->setToolTip(
      tr("Show the previous %1, wrapping round at the start of the list.")
          .arg(item_noun.toLower()));
  next_item_button_->setToolTip(
      tr("Show the next %1, wrapping round at the end of the list.")
          .arg(item_noun.toLower()));

  find_bar_->setVisible(!schema.find_label.empty());
  if (!schema.find_label.empty()) {
    find_label_->setText(to_qstring(schema.find_label));
    find_edit_->setPlaceholderText(to_qstring(schema.find_placeholder));
  }

  highlight_check_->setVisible(!schema.highlight_label.empty());
  if (!schema.highlight_label.empty()) {
    highlight_check_->setText(to_qstring(schema.highlight_label));
  }

  empty_label_->setText(schema.empty_message.empty()
                            ? tr("Nothing was recovered")
                            : to_qstring(schema.empty_message));

  refreshItemList();

  // Restore the reader's position where the same key is still listed; fall
  // back to the first item otherwise, which is what a fresh catalogue wants.
  int row = top_level_.empty() ? -1 : 0;
  if (!previous_key.isEmpty()) {
    for (size_t i = 0; i < top_level_.size(); ++i) {
      if (normalise_key(to_qstring(data_.items[top_level_[i]].find_key)) ==
          previous_key) {
        row = static_cast<int>(i);
        break;
      }
    }
  }
  showItem(row);
}

void CatalogueDialog::refreshItemList() {
  updating_list_ = true;

  top_level_.clear();
  for (size_t i = 0; i < data_.items.size(); ++i) {
    if (data_.items[i].parent_id.empty()) {
      top_level_.push_back(i);
    }
  }

  const auto& columns = data_.schema.columns;
  items_table_->clearContents();
  items_table_->setColumnCount(static_cast<int>(columns.size()));
  QStringList headings;
  headings.reserve(static_cast<int>(columns.size()));
  for (const auto& column : columns) {
    headings << to_qstring(column.title);
  }
  items_table_->setHorizontalHeaderLabels(headings);
  for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
    auto* header_item = items_table_->horizontalHeaderItem(column);
    if (header_item != nullptr &&
        columns[static_cast<size_t>(column)].numeric) {
      // Numeric columns are right-aligned so their digits line up; their
      // headings follow them.
      header_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
  }
  auto* header = items_table_->horizontalHeader();
  for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
    header->setSectionResizeMode(column,
                                 column + 1 == static_cast<int>(columns.size())
                                     ? QHeaderView::Stretch
                                     : QHeaderView::ResizeToContents);
  }

  // One trigger run delivers the catalogue whole, so the table is built once
  // per delivery rather than merged: there is no playback to keep it still for.
  items_table_->setRowCount(static_cast<int>(top_level_.size()));
  const QBrush muted = palette().brush(QPalette::Disabled, QPalette::Text);
  for (int row = 0; row < static_cast<int>(top_level_.size()); ++row) {
    const auto& item = data_.items[top_level_[static_cast<size_t>(row)]];
    const QString tooltip = to_qstring(item.tooltip);
    for (int column = 0; column < static_cast<int>(columns.size()); ++column) {
      QString text;
      if (column < static_cast<int>(item.values.size())) {
        text = to_qstring(item.values[static_cast<size_t>(column)]);
      }
      // Badges ride on the first column: there is nowhere in a table this
      // narrow to put a legend, so they are spelled out against the value.
      if (column == 0) {
        for (const auto& badge : item.badges) {
          text += QLatin1Char(' ') + to_qstring(badge);
        }
      }
      auto* cell = new QTableWidgetItem(text);
      if (columns[static_cast<size_t>(column)].numeric) {
        cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
      }
      if (!item.selectable) {
        // Kept listed — it is genuinely recovered data — but muted, so the
        // items a reader can actually use are the ones that stand out.
        cell->setForeground(muted);
      }
      if (!tooltip.isEmpty()) {
        cell->setToolTip(tooltip);
      }
      items_table_->setItem(row, column, cell);
    }
  }
  updating_list_ = false;

  const bool navigable = top_level_.size() > 1;
  prev_item_button_->setEnabled(navigable);
  next_item_button_->setEnabled(navigable);
  item_nav_->setVisible(!top_level_.empty());

  QStringList notices;
  for (const auto& notice : data_.summary.notices) {
    notices << to_qstring(notice);
  }
  notice_label_->setText(notices.join(QStringLiteral("   ")));
  notice_label_->setVisible(!notices.isEmpty());

  const QString summary = to_qstring(data_.summary.headline);
  summary_label_->setText(summary);
  summary_label_->setVisible(!summary.isEmpty());
}

std::vector<size_t> CatalogueDialog::variantsOf(int row) const {
  std::vector<size_t> variants;
  if (row < 0 || row >= static_cast<int>(top_level_.size())) {
    return variants;
  }
  const std::string& parent_id =
      data_.items[top_level_[static_cast<size_t>(row)]].id;
  for (size_t i = 0; i < data_.items.size(); ++i) {
    if (data_.items[i].parent_id == parent_id) {
      variants.push_back(i);
    }
  }
  return variants;
}

size_t CatalogueDialog::displayedIndex() const {
  if (current_row_ < 0 || current_row_ >= static_cast<int>(top_level_.size())) {
    return std::numeric_limits<size_t>::max();
  }
  const std::vector<size_t> variants = variantsOf(current_row_);
  if (variants.empty()) {
    return top_level_[static_cast<size_t>(current_row_)];
  }
  const size_t index = static_cast<size_t>(
      std::clamp(variant_index_, 0, static_cast<int>(variants.size()) - 1));
  return variants[index];
}

void CatalogueDialog::showItem(int row) {
  const bool valid = row >= 0 && row < static_cast<int>(top_level_.size());
  if (current_row_ != row) {
    // A different item starts at the top of its sequence; a re-render of the
    // same one (stepping the carousel, or a fresh delivery of the same
    // catalogue) keeps the variant the reader is on.
    variant_index_ = 0;
  }
  current_row_ = valid ? row : -1;

  // Keep the table in step without the selection feeding back into here.
  updating_list_ = true;
  if (valid) {
    items_table_->setCurrentCell(row, 0);
  } else {
    items_table_->setCurrentCell(-1, -1);
    items_table_->clearSelection();
  }
  updating_list_ = false;

  // Keep the find box showing what is on screen, without re-triggering a
  // lookup.
  if (valid && find_bar_->isVisibleTo(this)) {
    const QString key =
        to_qstring(data_.items[top_level_[static_cast<size_t>(row)]].find_key);
    if (!key.isEmpty() && key != find_edit_->text()) {
      updating_list_ = true;
      find_edit_->setText(key);
      updating_list_ = false;
    }
  }

  refreshVariantControl();
  renderPayload();
}

void CatalogueDialog::renderPayload() {
  const size_t index = displayedIndex();
  if (index == std::numeric_limits<size_t>::max()) {
    grid_widget_->clearGrid();
    display_widget_->clearDisplayList();
    companion_pane_->clear();
    text_pane_->clear();
    table_pane_->clearContents();
    table_pane_->setRowCount(0);
    condition_label_->setVisible(false);
    payload_stack_->setCurrentIndex(kPageNothing);
    if (!has_data_) {
      headline_label_->setText(tr("No data"));
    } else if (top_level_.empty()) {
      headline_label_->setText(empty_label_->text());
    } else {
      const QString typed = find_edit_->text().trimmed();
      const QString noun = data_.schema.item_noun.empty()
                               ? tr("Item")
                               : to_qstring(data_.schema.item_noun);
      headline_label_->setText(
          typed.isEmpty() ? tr("Nothing selected")
                          : tr("%1 %2 was not carried").arg(noun, typed));
    }
    return;
  }

  const orc::CataloguePayload& payload = data_.payloads[index];
  headline_label_->setText(to_qstring(payload.headline));
  const QString condition = to_qstring(payload.condition);
  condition_label_->setText(condition);
  condition_label_->setVisible(!condition.isEmpty());

  switch (payload.kind) {
    case orc::CataloguePayload::Kind::kCellGrid:
      grid_widget_->setGrid(payload.grid);
      grid_widget_->setShowDataErrors(highlight_check_->isChecked());
      payload_stack_->setCurrentIndex(kPageCellGrid);
      return;

    case orc::CataloguePayload::Kind::kDisplayList: {
      display_widget_->setDisplayList(payload.display_list);
      display_widget_->setShowDataErrors(highlight_check_->isChecked());
      const QString companion = to_qstring(payload.companion_text);
      companion_pane_->setPlainText(companion);
      companion_pane_->setVisible(!companion.isEmpty());
      payload_stack_->setCurrentIndex(kPageDisplayList);
      return;
    }

    case orc::CataloguePayload::Kind::kText: {
      // A listing wants its columns to line up; prose does not.
      text_pane_->setFont(
          payload.document.monospace
              ? QFontDatabase::systemFont(QFontDatabase::FixedFont)
              : QFontDatabase::systemFont(QFontDatabase::GeneralFont));
      text_pane_->setPlainText(to_qstring(payload.document.text));
      payload_stack_->setCurrentIndex(kPageText);
      return;
    }

    case orc::CataloguePayload::Kind::kTable: {
      const auto& table = payload.table;
      table_pane_->clearContents();
      table_pane_->setColumnCount(static_cast<int>(table.columns.size()));
      QStringList headings;
      for (const auto& column : table.columns) {
        headings << to_qstring(column.title);
      }
      table_pane_->setHorizontalHeaderLabels(headings);
      table_pane_->setRowCount(static_cast<int>(table.rows.size()));
      for (int row = 0; row < static_cast<int>(table.rows.size()); ++row) {
        const auto& values = table.rows[static_cast<size_t>(row)];
        for (int column = 0; column < static_cast<int>(table.columns.size()) &&
                             column < static_cast<int>(values.size());
             ++column) {
          auto* cell = new QTableWidgetItem(
              to_qstring(values[static_cast<size_t>(column)]));
          if (table.columns[static_cast<size_t>(column)].numeric) {
            cell->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
          }
          table_pane_->setItem(row, column, cell);
        }
      }
      auto* header = table_pane_->horizontalHeader();
      for (int column = 0; column < static_cast<int>(table.columns.size());
           ++column) {
        header->setSectionResizeMode(
            column, column + 1 == static_cast<int>(table.columns.size())
                        ? QHeaderView::Stretch
                        : QHeaderView::ResizeToContents);
      }
      payload_stack_->setCurrentIndex(kPageTable);
      return;
    }

    case orc::CataloguePayload::Kind::kNone:
      break;
  }

  // A parent with no payload and no variants has nothing to draw.
  grid_widget_->clearGrid();
  display_widget_->clearDisplayList();
  payload_stack_->setCurrentIndex(kPageNothing);
}

void CatalogueDialog::refreshVariantControl() {
  const std::vector<size_t> variants = variantsOf(current_row_);
  const QString noun = data_.schema.variant_noun.empty()
                           ? tr("Variant")
                           : to_qstring(data_.schema.variant_noun);

  if (data_.schema.variant_noun.empty() || variants.empty()) {
    variant_index_ = 0;
    variant_bar_->setVisible(false);
    variant_label_->clear();
    return;
  }

  const int count = static_cast<int>(variants.size());
  variant_index_ = std::clamp(variant_index_, 0, count - 1);
  const auto& variant =
      data_.items[variants[static_cast<size_t>(variant_index_)]];

  if (count == 1) {
    // Said rather than hidden, so "one of them" is distinguishable from a
    // control that has not been noticed.
    variant_label_->setText(tr("No %1s").arg(noun.toLower()));
  } else {
    variant_label_->setText(tr("%1 %2 of %3 (%4)")
                                .arg(noun)
                                .arg(variant_index_ + 1)
                                .arg(count)
                                .arg(to_qstring(variant.variant_label)));
  }
  prev_variant_button_->setEnabled(count > 1);
  next_variant_button_->setEnabled(count > 1);
  variant_bar_->setVisible(true);
}

void CatalogueDialog::stepVariant(int delta) {
  const std::vector<size_t> variants = variantsOf(current_row_);
  if (variants.size() < 2) {
    return;
  }
  // Wrapping, because the carousel itself does: stepping past the last variant
  // is what the service does next.
  const int count = static_cast<int>(variants.size());
  variant_index_ = ((variant_index_ + delta) % count + count) % count;
  refreshVariantControl();
  renderPayload();
}

void CatalogueDialog::stepItem(int delta) {
  const int count = static_cast<int>(top_level_.size());
  if (count == 0) {
    return;
  }
  const int base = current_row_ < 0 ? 0 : current_row_;
  showItem(((base + delta) % count + count) % count);
}

void CatalogueDialog::onFindTextChanged() {
  if (updating_list_) {
    return;  // programmatic sync following a selection
  }
  const QString key = normalise_key(find_edit_->text());
  for (size_t i = 0; i < top_level_.size(); ++i) {
    if (normalise_key(to_qstring(data_.items[top_level_[i]].find_key)) == key) {
      showItem(static_cast<int>(i));
      return;
    }
  }
  showItem(-1);
}

void CatalogueDialog::onItemSelected() {
  if (updating_list_) {
    return;  // programmatic selection following the find box
  }
  const int row = items_table_->currentRow();
  if (row < 0) {
    return;
  }
  showItem(row);
}

void CatalogueDialog::onHighlightToggled(bool checked) {
  grid_widget_->setShowDataErrors(checked);
  display_widget_->setShowDataErrors(checked);
}

// ---------------------------------------------------------------------------
// Test seams
// ---------------------------------------------------------------------------

std::vector<QString> CatalogueDialog::listedItems() const {
  std::vector<QString> labels;
  labels.reserve(static_cast<size_t>(items_table_->rowCount()));
  for (int row = 0; row < items_table_->rowCount(); ++row) {
    const auto* cell = items_table_->item(row, 0);
    labels.push_back(cell == nullptr ? QString() : cell->text());
  }
  return labels;
}

QString CatalogueDialog::listedValue(const QString& key, int column) const {
  for (int row = 0; row < items_table_->rowCount(); ++row) {
    const size_t index = top_level_[static_cast<size_t>(row)];
    if (to_qstring(data_.items[index].find_key) == key ||
        to_qstring(data_.items[index].id) == key) {
      const auto* cell = items_table_->item(row, column);
      return cell == nullptr ? QString() : cell->text();
    }
  }
  return {};
}

void CatalogueDialog::selectItem(int index) { showItem(index); }

void CatalogueDialog::showNextItem() { stepItem(1); }

void CatalogueDialog::showPreviousItem() { stepItem(-1); }

QString CatalogueDialog::findText() const {
  return find_bar_->isVisibleTo(this) ? find_edit_->text() : QString();
}

void CatalogueDialog::setFindText(const QString& text) {
  find_edit_->setText(text);
}

// The readouts below report empty when their widget is hidden. The test is
// isVisibleTo(this) rather than isVisible(): a dialogue that has never been
// shown has no visible widgets at all, and these have to answer the same way
// whether or not the window is on screen.
QString CatalogueDialog::variantText() const {
  return variant_bar_->isVisibleTo(this) ? variant_label_->text() : QString();
}

int CatalogueDialog::variantCount() const {
  return static_cast<int>(variantsOf(current_row_).size());
}

int CatalogueDialog::variantIndex() const {
  return current_row_ < 0 ? -1 : variant_index_;
}

void CatalogueDialog::showNextVariant() { stepVariant(1); }

void CatalogueDialog::showPreviousVariant() { stepVariant(-1); }

QString CatalogueDialog::headlineText() const {
  return headline_label_->text();
}

QString CatalogueDialog::conditionText() const {
  return condition_label_->isVisibleTo(this) ? condition_label_->text()
                                             : QString();
}

QString CatalogueDialog::summaryText() const {
  return summary_label_->isVisibleTo(this) ? summary_label_->text() : QString();
}

QString CatalogueDialog::noticeText() const {
  return notice_label_->isVisibleTo(this) ? notice_label_->text() : QString();
}

const orc::CatalogueCellGrid* CatalogueDialog::currentGrid() const {
  return payload_stack_->currentIndex() == kPageCellGrid ? grid_widget_->grid()
                                                         : nullptr;
}

const orc::CatalogueDisplayList* CatalogueDialog::currentDisplayList() const {
  const size_t index = displayedIndex();
  if (index == std::numeric_limits<size_t>::max() ||
      payload_stack_->currentIndex() != kPageDisplayList) {
    return nullptr;
  }
  return &data_.payloads[index].display_list;
}

QString CatalogueDialog::currentText() const {
  if (payload_stack_->currentIndex() == kPageText) {
    return text_pane_->toPlainText();
  }
  if (payload_stack_->currentIndex() == kPageDisplayList) {
    return companion_pane_->toPlainText();
  }
  return {};
}

std::vector<QString> CatalogueDialog::listedTableRows() const {
  std::vector<QString> rows;
  if (payload_stack_->currentIndex() != kPageTable) {
    return rows;
  }
  rows.reserve(static_cast<size_t>(table_pane_->rowCount()));
  for (int row = 0; row < table_pane_->rowCount(); ++row) {
    QStringList values;
    for (int column = 0; column < table_pane_->columnCount(); ++column) {
      const auto* cell = table_pane_->item(row, column);
      values << (cell == nullptr ? QString() : cell->text());
    }
    rows.push_back(values.join(QStringLiteral(" | ")));
  }
  return rows;
}
