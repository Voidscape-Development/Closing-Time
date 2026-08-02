/*
Closing Time
Copyright (C) 2026 Voidscape Development <Eiondailey@live.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "ui/DesignerDialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "ui/PreviewWidget.hpp"
#include "ui/SectionEditor.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Milliseconds of quiet before an edit triggers a re-render of the preview strip. */
constexpr int kPreviewDebounceMs = 250;

/*
 * One designer window per source. Keyed by the raw pointer purely for identity; the
 * pointer is never dereferenced, and each dialog holds a weak reference for actual access.
 */
QHash<obs_source_t *, DesignerDialog *> &designerRegistry()
{
	static QHash<obs_source_t *, DesignerDialog *> registry;
	return registry;
}

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

} // namespace

void openDesignerFor(obs_source_t *source)
{
	if (!source)
		return;

	const auto existing = designerRegistry().constFind(source);
	if (existing != designerRegistry().constEnd()) {
		existing.value()->show();
		existing.value()->raise();
		existing.value()->activateWindow();
		return;
	}

	auto *dialog = new DesignerDialog(source, mainWindow());
	dialog->show();
}

void closeDesignerFor(obs_source_t *source)
{
	if (!source)
		return;

	/*
	 * Sources are destroyed on the graphics thread, so the window cannot be touched here.
	 * The task closes the dialog only once its weak reference has gone dead, which also
	 * covers the case where a later source happens to reuse this address.
	 */
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const auto it = designerRegistry().constFind(static_cast<obs_source_t *>(param));
			if (it == designerRegistry().constEnd())
				return;

			DesignerDialog *dialog = it.value();
			if (dialog->sourceIsGone())
				dialog->close();
		},
		source, false);
}

DesignerDialog::DesignerDialog(obs_source_t *source, QWidget *parent) : QDialog(parent)
{
	weakSource = OBSGetWeakRef(source);
	designerRegistry().insert(source, this);

	setWindowTitle(QStringLiteral("%1 — %2").arg(moduleText("Designer.Title"),
						     QString::fromUtf8(obs_source_get_name(source))));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1280, 800);

	auto *outer = new QVBoxLayout(this);
	auto *splitter = new QSplitter(Qt::Horizontal, this);

	/* --- left: the section list, in roll order --- */
	auto *listPane = new QWidget(splitter);
	auto *listLayout = new QVBoxLayout(listPane);
	listLayout->setContentsMargins(0, 0, 0, 0);
	listLayout->addWidget(new QLabel(moduleText("Designer.Sections"), listPane));

	sectionList = new QListWidget(listPane);
	listLayout->addWidget(sectionList, 1);

	auto *listButtons = new QHBoxLayout();
	const auto addListButton = [&](const char *key, auto slot) {
		auto *button = new QPushButton(moduleText(key), listPane);
		listButtons->addWidget(button);
		connect(button, &QPushButton::clicked, this, slot);
		return button;
	};

	addListButton("Designer.Duplicate", &DesignerDialog::duplicateSection);
	addListButton("Designer.Remove", &DesignerDialog::removeSection);
	addListButton("Designer.MoveUp", [this] { moveSection(-1); });
	addListButton("Designer.MoveDown", [this] { moveSection(1); });

	/*
	 * Add is a menu button rather than a plain one: there are twelve section types and
	 * picking the type up front is what decides which editor fields appear.
	 */
	auto *addButton = new QPushButton(moduleText("Designer.Add"), listPane);
	listButtons->insertWidget(0, addButton);
	listLayout->addLayout(listButtons);

	auto *addMenu = new QMenu(addButton);
	for (SectionType type : allSectionTypes()) {
		QAction *action = addMenu->addAction(QString::fromUtf8(sectionTypeName(type)));
		connect(action, &QAction::triggered, this, [this, type] {
			commitCurrentSection();
			const int insertAt = currentIndex >= 0 ? currentIndex + 1 : document.sections.size();
			document.sections.insert(insertAt, Section::makeDefault(type));
			refreshSectionList(insertAt);
			schedulePreviewRefresh();
		});
	}
	addButton->setMenu(addMenu);

	splitter->addWidget(listPane);

	/* --- middle: the editor for whichever section is selected --- */
	editorScroll = new QScrollArea(splitter);
	editorScroll->setWidgetResizable(true);
	editor = new SectionEditor(editorScroll);
	editorScroll->setWidget(editor);
	splitter->addWidget(editorScroll);

	/* --- right: live preview of the whole strip --- */
	auto *previewPane = new QWidget(splitter);
	auto *previewLayout = new QVBoxLayout(previewPane);
	previewLayout->setContentsMargins(0, 0, 0, 0);
	previewLayout->addWidget(new QLabel(moduleText("Designer.Preview"), previewPane));
	preview = new PreviewWidget(previewPane);
	previewLayout->addWidget(preview, 1);
	durationLabel = new QLabel(previewPane);
	previewLayout->addWidget(durationLabel);
	splitter->addWidget(previewPane);

	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 3);
	splitter->setStretchFactor(2, 2);
	outer->addWidget(splitter, 1);

	/* --- bottom: import/export and the dialog buttons --- */
	auto *footer = new QHBoxLayout();
	auto *importButton = new QPushButton(moduleText("Designer.ImportJson"), this);
	auto *exportButton = new QPushButton(moduleText("Designer.ExportJson"), this);
	footer->addWidget(importButton);
	footer->addWidget(exportButton);
	footer->addStretch();

	buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
	footer->addWidget(buttons);
	outer->addLayout(footer);

	refreshTimer = new QTimer(this);
	refreshTimer->setSingleShot(true);
	refreshTimer->setInterval(kPreviewDebounceMs);

	connect(refreshTimer, &QTimer::timeout, this, &DesignerDialog::refreshPreview);
	connect(sectionList, &QListWidget::currentRowChanged, this, &DesignerDialog::onSelectionChanged);
	connect(editor, &SectionEditor::changed, this, &DesignerDialog::onSectionEdited);
	connect(importButton, &QPushButton::clicked, this, &DesignerDialog::importJson);
	connect(exportButton, &QPushButton::clicked, this, &DesignerDialog::exportJson);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		commitCurrentSection();
		writeToSource();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &DesignerDialog::reject);
	connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
		commitCurrentSection();
		writeToSource();
	});

	loadFromSource();
}

DesignerDialog::~DesignerDialog()
{
	for (auto it = designerRegistry().begin(); it != designerRegistry().end(); ++it) {
		if (it.value() == this) {
			designerRegistry().erase(it);
			break;
		}
	}
}

bool DesignerDialog::sourceIsGone() const
{
	return obs_weak_source_expired(weakSource);
}

void DesignerDialog::loadFromSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	document.load(settings);

	if (document.sections.isEmpty())
		document.sections.append(Section::makeDefault(SectionType::Title));

	refreshSectionList(0);
	refreshPreview();
}

void DesignerDialog::writeToSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	/*
	 * The properties window owns canvas size, speed and the ending action; the designer
	 * owns content. Re-reading the live settings before writing means edits made in the
	 * properties window while this dialog was open are not clobbered on Apply.
	 */
	OBSDataAutoRelease settings = obs_source_get_settings(source);

	Document merged;
	merged.load(settings);
	merged.sections = document.sections;

	OBSDataAutoRelease updated = obs_data_create();
	merged.save(updated);
	obs_source_update(source, updated);
}

void DesignerDialog::refreshSectionList(int selectRow)
{
	const QSignalBlocker blocker(sectionList);

	sectionList->clear();
	for (const Section &section : document.sections) {
		auto *item = new QListWidgetItem(section.displayLabel(), sectionList);
		item->setToolTip(QString::fromUtf8(sectionTypeName(section.type)));
		if (!section.visible)
			item->setForeground(Qt::gray);
	}

	const int row = std::clamp(selectRow, -1, static_cast<int>(document.sections.size()) - 1);
	sectionList->setCurrentRow(row);
	currentIndex = row;

	if (row >= 0)
		editor->setSection(document.sections.at(row));

	editorScroll->setEnabled(row >= 0);
}

void DesignerDialog::onSelectionChanged()
{
	commitCurrentSection();

	const int row = sectionList->currentRow();
	currentIndex = row;
	editorScroll->setEnabled(row >= 0);

	if (row >= 0 && row < document.sections.size())
		editor->setSection(document.sections.at(row));
}

void DesignerDialog::commitCurrentSection()
{
	if (currentIndex < 0 || currentIndex >= document.sections.size())
		return;

	document.sections[currentIndex] = editor->section();
}

void DesignerDialog::onSectionEdited()
{
	commitCurrentSection();

	if (currentIndex >= 0 && currentIndex < sectionList->count()) {
		const QSignalBlocker blocker(sectionList);
		const Section &section = document.sections.at(currentIndex);
		QListWidgetItem *item = sectionList->item(currentIndex);
		item->setText(section.displayLabel());
		item->setForeground(section.visible ? QBrush() : QBrush(Qt::gray));
	}

	schedulePreviewRefresh();
}

void DesignerDialog::duplicateSection()
{
	commitCurrentSection();
	if (currentIndex < 0 || currentIndex >= document.sections.size())
		return;

	document.sections.insert(currentIndex + 1, document.sections.at(currentIndex));
	refreshSectionList(currentIndex + 1);
	schedulePreviewRefresh();
}

void DesignerDialog::removeSection()
{
	if (currentIndex < 0 || currentIndex >= document.sections.size())
		return;

	const int removed = currentIndex;
	/* Drop the stale selection first so the editor cannot write back into the gap. */
	currentIndex = -1;
	document.sections.removeAt(removed);

	refreshSectionList(std::min(removed, static_cast<int>(document.sections.size()) - 1));
	schedulePreviewRefresh();
}

void DesignerDialog::moveSection(int delta)
{
	commitCurrentSection();

	const int target = currentIndex + delta;
	if (currentIndex < 0 || target < 0 || target >= document.sections.size())
		return;

	document.sections.swapItemsAt(currentIndex, target);
	refreshSectionList(target);
	schedulePreviewRefresh();
}

void DesignerDialog::importJson()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.ImportJson"), QString(),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, moduleText("Designer.ImportJson"), file.errorString());
		return;
	}

	Document loaded;
	QString error;
	if (!loaded.fromJson(QString::fromUtf8(file.readAll()), &error)) {
		QMessageBox::warning(this, moduleText("Designer.ImportJson"), error);
		return;
	}

	/* Only the content is taken; canvas and playback settings stay as configured here. */
	document.sections = loaded.sections;
	currentIndex = -1;
	refreshSectionList(document.sections.isEmpty() ? -1 : 0);
	refreshPreview();
}

void DesignerDialog::exportJson()
{
	commitCurrentSection();

	const QString path = QFileDialog::getSaveFileName(this, moduleText("Designer.ExportJson"),
							  QStringLiteral("credits.json"),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QMessageBox::warning(this, moduleText("Designer.ExportJson"), file.errorString());
		return;
	}

	file.write(document.toJson().toUtf8());
}

void DesignerDialog::schedulePreviewRefresh()
{
	refreshTimer->start();
}

void DesignerDialog::refreshPreview()
{
	const StripRenderer renderer(&logos);
	const Strip strip = renderer.render(document);

	preview->setStrip(strip, document.width, document.height, document.background);

	const double travel = document.height + strip.height;
	const double seconds = document.scrollSpeed > 0.0 ? travel / document.scrollSpeed : 0.0;

	durationLabel->setText(QStringLiteral("%1 %2:%3  ·  %4 %5 px")
				       .arg(moduleText("Designer.Duration"))
				       .arg(static_cast<int>(seconds) / 60)
				       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'))
				       .arg(moduleText("Designer.StripHeight"))
				       .arg(strip.height));
}

} // namespace closingtime
