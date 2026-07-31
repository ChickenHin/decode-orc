/*
 * File:        pluginbrowsedialog.cpp
 * Module:      orc-gui
 * Purpose:     Browse and install plugins from the curated index
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "pluginbrowsedialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

#include "plugintrustdialog.h"

namespace orc {

namespace {
constexpr int kEntryIdRole = Qt::UserRole + 1;
}  // namespace

PluginBrowseDialog::PluginBrowseDialog(
    orc::presenters::IProjectPresenter& presenter, QWidget* parent)
    : QDialog(parent), model_(presenter) {
  setWindowTitle("Browse Plugins");
  resize(820, 520);
  buildUI();
  startRefresh();
}

PluginBrowseDialog::~PluginBrowseDialog() {
  if (refresh_thread_) {
    refresh_thread_->wait();
    delete refresh_thread_;
    refresh_thread_ = nullptr;
  }
}

void PluginBrowseDialog::buildUI() {
  auto* root = new QVBoxLayout(this);

  status_label_ = new QLabel("Loading the plugin index…", this);
  status_label_->setWordWrap(true);
  root->addWidget(status_label_);

  search_edit_ = new QLineEdit(this);
  search_edit_->setPlaceholderText("Search plugins…");
  search_edit_->setClearButtonEnabled(true);
  root->addWidget(search_edit_);

  auto* split = new QHBoxLayout();
  list_ = new QListWidget(this);
  list_->setMinimumWidth(320);
  split->addWidget(list_, 1);

  details_label_ = new QLabel(this);
  details_label_->setWordWrap(true);
  details_label_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  details_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
  split->addWidget(details_label_, 1);
  root->addLayout(split, 1);

  auto* button_row = new QHBoxLayout();
  install_button_ = new QPushButton("Install…", this);
  install_button_->setEnabled(false);
  button_row->addWidget(install_button_);
  button_row->addStretch();
  root->addLayout(button_row);

  auto* close_box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  root->addWidget(close_box);

  auto* note = new QLabel(
      "Note: Installed plugins take effect on the next application launch.",
      this);
  note->setEnabled(false);
  note->setWordWrap(true);
  root->addWidget(note);

  connect(search_edit_, &QLineEdit::textChanged, this,
          &PluginBrowseDialog::onSearchChanged);
  connect(list_, &QListWidget::itemSelectionChanged, this,
          &PluginBrowseDialog::onSelectionChanged);
  connect(install_button_, &QPushButton::clicked, this,
          &PluginBrowseDialog::onInstall);
  connect(close_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

  // Interaction is disabled until the first refresh completes.
  search_edit_->setEnabled(false);
}

void PluginBrowseDialog::startRefresh() {
  if (refresh_thread_) {
    return;
  }
  status_label_->setText("Loading the plugin index…");
  search_edit_->setEnabled(false);
  install_button_->setEnabled(false);

  // model_.refresh() performs the network fetch off the UI thread. The
  // finished() signal is delivered to this (main-thread) object queued, which
  // establishes ordering with the model write.
  refresh_thread_ = QThread::create([this]() { model_.refresh(); });
  connect(refresh_thread_, &QThread::finished, this,
          &PluginBrowseDialog::onRefreshFinished);
  refresh_thread_->start();
}

void PluginBrowseDialog::onRefreshFinished() {
  if (refresh_thread_) {
    refresh_thread_->deleteLater();
    refresh_thread_ = nullptr;
  }
  status_label_->setText(QString::fromStdString(model_.statusMessage()));
  search_edit_->setEnabled(model_.available());
  populateList();
}

void PluginBrowseDialog::populateList() {
  list_->clear();
  const auto entries = model_.search(search_edit_->text().toStdString());
  for (const auto& entry : entries) {
    QString label = QString::fromStdString(
        entry.display_name.empty() ? entry.id : entry.display_name);
    if (entry.release_unreachable) {
      label += "  (unreachable)";
    } else if (!entry.has_compatible_build) {
      label += "  (incompatible)";
    } else if (entry.already_installed) {
      label += "  (installed)";
    }
    auto* item = new QListWidgetItem(label, list_);
    item->setData(kEntryIdRole, QString::fromStdString(entry.id));
  }
  if (list_->count() > 0) {
    list_->setCurrentRow(0);
  } else {
    updateDetails();
  }
}

const orc::presenters::PluginIndexEntryInfo* PluginBrowseDialog::selectedEntry()
    const {
  auto* item = list_->currentItem();
  if (!item) {
    return nullptr;
  }
  const std::string id = item->data(kEntryIdRole).toString().toStdString();
  for (const auto& entry : model_.index().entries) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

void PluginBrowseDialog::updateDetails() {
  const auto* entry = selectedEntry();
  if (!entry) {
    details_label_->clear();
    install_button_->setEnabled(false);
    return;
  }

  QString text;
  text += "<b>" +
          QString::fromStdString(
              entry->display_name.empty() ? entry->id : entry->display_name)
              .toHtmlEscaped() +
          "</b><br>";
  text += QString::fromStdString(entry->id).toHtmlEscaped() + "<br><br>";
  if (!entry->description.empty()) {
    text +=
        QString::fromStdString(entry->description).toHtmlEscaped() + "<br><br>";
  }
  if (!entry->version.empty()) {
    text += "Latest release: " +
            QString::fromStdString(entry->version).toHtmlEscaped() + "<br>";
  }
  if (!entry->maintainer.empty()) {
    text += "Maintainer: " +
            QString::fromStdString(entry->maintainer).toHtmlEscaped() + "<br>";
  }
  if (!entry->license_spdx.empty()) {
    text += "License: " +
            QString::fromStdString(entry->license_spdx).toHtmlEscaped() +
            "<br>";
  }
  if (!entry->source_repo_url.empty()) {
    text += "Source: " +
            QString::fromStdString(entry->source_repo_url).toHtmlEscaped() +
            "<br>";
  }
  text += "<br>";
  if (entry->has_compatible_build) {
    text += "Compatible with this host.";
  } else if (entry->release_unreachable) {
    text += "Release information unreachable.";
    if (!entry->compatibility_message.empty()) {
      text +=
          "<br>" +
          QString::fromStdString(entry->compatibility_message).toHtmlEscaped();
    }
  } else {
    text += QString::fromStdString(
                entry->compatibility_message.empty()
                    ? std::string("No compatible build for this host.")
                    : entry->compatibility_message)
                .toHtmlEscaped();
  }
  if (entry->already_installed) {
    text += "<br>Already installed.";
  }
  details_label_->setText(text);

  install_button_->setEnabled(entry->has_compatible_build &&
                              !entry->already_installed);
}

void PluginBrowseDialog::onSearchChanged() { populateList(); }

void PluginBrowseDialog::onSelectionChanged() { updateDetails(); }

void PluginBrowseDialog::onInstall() {
  const auto* entry = selectedEntry();
  if (!entry) {
    return;
  }
  const std::string id = entry->id;

  // Installing means the latest release will run as native code, so trust is
  // confirmed explicitly before anything is recorded.
  if (!confirmPluginTrust(this)) {
    return;  // Cancelled — nothing added.
  }

  const auto install_result = model_.install(id);
  if (!install_result.success) {
    QMessageBox::warning(this, "Install Failed",
                         QString::fromStdString(install_result.error_message));
    return;
  }

  changes_made_ = true;

  const auto trust_result = model_.trust(id);
  if (!trust_result.success) {
    QMessageBox::warning(this, "Trust Failed",
                         QString::fromStdString(trust_result.error_message));
  }

  QMessageBox::information(
      this, "Plugin Installed",
      QString::fromStdString(id) + " installed and trusted.");
  install_button_->setEnabled(false);
}

}  // namespace orc
