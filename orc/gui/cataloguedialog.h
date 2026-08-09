/*
 * File:        cataloguedialog.h
 * Module:      orc-gui
 * Purpose:     Generic browser for any stage that exposes ICatalogueResults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUEDIALOG_H
#define CATALOGUEDIALOG_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QStatusBar>
#include <QString>
#include <QTableWidget>
#include <QToolButton>
#include <QWidget>
#include <cstdint>
#include <string>
#include <vector>

class CatalogueCellGridWidget;
class CatalogueDisplayListWidget;

/**
 * @brief Browser for the catalogue a stage produced from one trigger run
 *
 * The stage tool of any node whose stage advertises
 * StageToolKind::CatalogueBrowser: triggering the node decodes the whole frame
 * range in a single pass and this dialogue shows what it found. It does not
 * follow the previewer — a catalogue describes the entire source, so there is
 * no window to slide and nothing to accumulate here.
 *
 * The catalogue is a table of items on the left and the selected item's payload
 * on the right. What the columns are called, what one item is called, whether
 * there is a find box, whether items have variants to step through — all of it
 * comes from the dataset, so this dialogue knows nothing about teletext, NABTS
 * or whatever service arrives next.
 *
 * An item may nest one level: a variant is a version of its parent that the
 * service cycles through, stepped under the payload rather than listed
 * separately. Selecting a parent shows its first variant.
 *
 * MainWindow drives it: setCatalogue() with whatever the coordinator delivered
 * for the node, and clearContent() when there is nothing to show.
 */
class CatalogueDialog : public QDialog {
  Q_OBJECT

 public:
  explicit CatalogueDialog(QWidget* parent = nullptr);
  ~CatalogueDialog() override;

  /// Show a "decoding" pending state while the stage trigger is in flight
  void showPending();

  /// Replace the pending state with why no catalogue is coming
  void showError(const QString& message);

  /// Clear the payload, the item list and the readouts
  void clearContent();

  /// Show one trigger run's catalogue, replacing whatever was displayed
  void setCatalogue(const orc::CatalogueDataset& data);

  // ---- Test seams --------------------------------------------------------

  /// First-column values of the listed items, in table order
  std::vector<QString> listedItems() const;
  /// Value of |column| for the row whose first column is |key|, or empty
  QString listedValue(const QString& key, int column) const;
  /// Index of the listed item on display, or -1 when none is
  int currentItemIndex() const { return current_row_; }
  void selectItem(int index);
  void showNextItem();
  void showPreviousItem();

  /// Find-box text (empty when the schema offers no find box)
  QString findText() const;
  void setFindText(const QString& text);

  /// Variant readout — "Sub-page 2 of 8 (0002)" — for the displayed item
  /// (empty when the variant bar is hidden)
  QString variantText() const;
  int variantCount() const;
  int variantIndex() const;
  void showNextVariant();
  void showPreviousVariant();

  /// Status-bar readouts (empty when hidden)
  QString headlineText() const;
  QString conditionText() const;
  QString summaryText() const;
  QString noticeText() const;

  /// The payload on display (test seams; nullptr when none is)
  const orc::CatalogueCellGrid* currentGrid() const;
  const orc::CatalogueDisplayList* currentDisplayList() const;
  /// Text of a text payload, or the companion text beside a visual one
  QString currentText() const;
  /// Rows of a table payload, one joined string per row
  std::vector<QString> listedTableRows() const;

 private slots:
  void onFindTextChanged();
  void onItemSelected();
  void onHighlightToggled(bool checked);

 private:
  /// Which pane the payload side is showing
  enum PayloadPage {
    kPageNothing = 0,
    kPageCellGrid = 1,
    kPageDisplayList = 2,
    kPageText = 3,
    kPageTable = 4,
  };

  void setupUI();
  /// Rebuild the item table and the notices from the current dataset
  void refreshItemList();
  /// Show the top-level item at |row| of the table, or nothing when out of
  /// range
  void showItem(int row);
  /// Step |delta| top-level items, wrapping at either end
  void stepItem(int delta);
  /// Show variant |variant_index_| of the displayed item
  void renderPayload();
  /// Step |delta| variants of the displayed item, wrapping at either end
  void stepVariant(int delta);
  /// Update the variant stepper for the item currently displayed
  void refreshVariantControl();

  /// Indices into data_.items of the top-level items, in listing order
  const std::vector<size_t>& topLevelRows() const { return top_level_; }
  /// Indices into data_.items of the variants of top-level row |row|
  std::vector<size_t> variantsOf(int row) const;
  /// The dataset index the payload pane is showing, or SIZE_MAX
  size_t displayedIndex() const;

  orc::CatalogueDataset data_;
  bool has_data_ = false;

  // Listing order: the dataset's own order, parents only.
  std::vector<size_t> top_level_;
  // Which top-level row is selected, and which of its variants is on screen.
  int current_row_ = -1;
  int variant_index_ = 0;

  // Set while the table is being rebuilt or programmatically selected, so
  // selection changes do not feed back into the find box.
  bool updating_list_ = false;

  QWidget* find_bar_ = nullptr;
  QLabel* find_label_ = nullptr;
  QLineEdit* find_edit_ = nullptr;
  QCheckBox* highlight_check_ = nullptr;
  QToolButton* prev_item_button_ = nullptr;
  QToolButton* next_item_button_ = nullptr;
  QWidget* item_nav_ = nullptr;

  QLabel* list_heading_ = nullptr;
  QTableWidget* items_table_ = nullptr;

  QStackedWidget* payload_stack_ = nullptr;
  CatalogueCellGridWidget* grid_widget_ = nullptr;
  CatalogueDisplayListWidget* display_widget_ = nullptr;
  QPlainTextEdit* companion_pane_ = nullptr;
  QWidget* display_page_ = nullptr;
  QPlainTextEdit* text_pane_ = nullptr;
  QTableWidget* table_pane_ = nullptr;
  QLabel* empty_label_ = nullptr;

  QWidget* variant_bar_ = nullptr;
  QToolButton* prev_variant_button_ = nullptr;
  QToolButton* next_variant_button_ = nullptr;
  QLabel* variant_label_ = nullptr;

  QLabel* notice_label_ = nullptr;
  QLabel* summary_label_ = nullptr;
  QStatusBar* status_bar_ = nullptr;
  QLabel* headline_label_ = nullptr;
  QLabel* condition_label_ = nullptr;
  QLabel* status_label_ = nullptr;
};

#endif  // CATALOGUEDIALOG_H
