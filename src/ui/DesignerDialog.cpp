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
#include <QApplication>
#include <QDialogButtonBox>
#include <QDropEvent>
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

#include "render/RenderThread.hpp"
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

/* Milliseconds of quiet before a run of small edits becomes its own undo step. */
constexpr int kEditBurstMs = 900;

/* How many steps back the designer can go before the oldest is forgotten. */
constexpr int kUndoDepth = 100;

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

SectionListWidget::SectionListWidget(QWidget *parent) : QListWidget(parent)
{
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setDropIndicatorShown(true);
	setSelectionMode(QAbstractItemView::SingleSelection);
}

void SectionListWidget::dropEvent(QDropEvent *event)
{
	const int from = currentRow();

	int to = indexAt(event->position().toPoint()).row();
	if (to < 0)
		to = count();
	else if (dropIndicatorPosition() == QAbstractItemView::BelowItem)
		++to;

	/*
	 * QListWidget is left out of it entirely. The base implementation would move the item
	 * itself, and the drag machinery would then remove the original row on a MoveAction,
	 * both of which fight the rebuild that follows from the document.
	 */
	event->setDropAction(Qt::IgnoreAction);
	event->accept();

	/* Dropping either side of where it already is changes nothing. */
	if (from < 0 || to == from || to == from + 1)
		return;

	emit rowMoved(from, to > from ? to - 1 : to);
}

DesignerDialog::DesignerDialog(obs_source_t *source, QWidget *parent) : QDialog(parent)
{
	weakSource = OBSGetWeakRef(source);
	designerRegistry().insert(source, this);
	sink->dialog = this;

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

	sectionList = new SectionListWidget(listPane);
	sectionList->setToolTip(moduleText("Designer.Sections.Tip"));
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
			beginUndoStep();
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
	fontWarningLabel = new QLabel(previewPane);
	fontWarningLabel->setWordWrap(true);
	fontWarningLabel->setStyleSheet(QStringLiteral("color: #e0a030;"));
	fontWarningLabel->hide();
	previewLayout->addWidget(fontWarningLabel);
	splitter->addWidget(previewPane);

	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 3);
	splitter->setStretchFactor(2, 2);
	outer->addWidget(splitter, 1);

	/* --- bottom: undo, import/export and the dialog buttons --- */
	auto *footer = new QHBoxLayout();
	undoButton = new QPushButton(moduleText("Designer.Undo"), this);
	redoButton = new QPushButton(moduleText("Designer.Redo"), this);
	/*
	 * These take Ctrl+Z / Ctrl+Shift+Z away from the text fields' own undo. That is the
	 * intent: one stack covering the whole document beats two that disagree about what the
	 * last change was, and an undo step here already restores the text along with it.
	 */
	undoButton->setShortcut(QKeySequence::Undo);
	redoButton->setShortcut(QKeySequence::Redo);
	footer->addWidget(undoButton);
	footer->addWidget(redoButton);

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

	editBurstTimer = new QTimer(this);
	editBurstTimer->setSingleShot(true);
	editBurstTimer->setInterval(kEditBurstMs);

	connect(refreshTimer, &QTimer::timeout, this, &DesignerDialog::refreshPreview);
	connect(editBurstTimer, &QTimer::timeout, this, [this] { editBurstOpen = false; });
	connect(undoButton, &QPushButton::clicked, this, &DesignerDialog::undo);
	connect(redoButton, &QPushButton::clicked, this, &DesignerDialog::redo);
	connect(sectionList, &QListWidget::currentRowChanged, this, &DesignerDialog::onSelectionChanged);
	connect(sectionList, &SectionListWidget::rowMoved, this, &DesignerDialog::moveSectionTo);
	connect(editor, &SectionEditor::changed, this, &DesignerDialog::onSectionEdited);
	connect(editor, &SectionEditor::presetSaveRequested, this, &DesignerDialog::savePreset);
	connect(editor, &SectionEditor::presetDeleteRequested, this, &DesignerDialog::deletePreset);
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
	refreshUndoButtons();
}

DesignerDialog::~DesignerDialog()
{
	/* Any render still in flight now has nowhere to deliver to, and quietly drops itself. */
	sink->dialog = nullptr;

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

	editor->setPresets(document.stylePresets);
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
	merged.stylePresets = document.stylePresets;

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

DesignerDialog::DocumentSnapshot DesignerDialog::snapshot() const
{
	return DocumentSnapshot{document.sections, document.stylePresets, currentIndex};
}

void DesignerDialog::beginUndoStep()
{
	editBurstOpen = false;
	editBurstTimer->stop();

	undoStack.append(snapshot());
	if (undoStack.size() > kUndoDepth)
		undoStack.removeFirst();

	/* Any new edit invalidates the branch that was undone away from. */
	redoStack.clear();
	refreshUndoButtons();
}

void DesignerDialog::beginEditUndoStep()
{
	if (editBurstOpen) {
		editBurstTimer->start();
		return;
	}

	beginUndoStep();
	editBurstOpen = true;
	editBurstTimer->start();
}

void DesignerDialog::restore(const DocumentSnapshot &state)
{
	editBurstOpen = false;
	editBurstTimer->stop();

	document.sections = state.sections;
	document.stylePresets = state.stylePresets;

	/* Cleared first so refreshSectionList cannot write the editor back into a stale row. */
	currentIndex = -1;
	editor->setPresets(document.stylePresets);
	refreshSectionList(std::min(state.currentIndex, static_cast<int>(document.sections.size()) - 1));
	refreshPreview();
}

void DesignerDialog::undo()
{
	if (undoStack.isEmpty())
		return;

	commitCurrentSection();
	redoStack.append(snapshot());
	restore(undoStack.takeLast());
	refreshUndoButtons();
}

void DesignerDialog::redo()
{
	if (redoStack.isEmpty())
		return;

	commitCurrentSection();
	undoStack.append(snapshot());
	restore(redoStack.takeLast());
	refreshUndoButtons();
}

void DesignerDialog::refreshUndoButtons()
{
	undoButton->setEnabled(!undoStack.isEmpty());
	redoButton->setEnabled(!redoStack.isEmpty());
}

void DesignerDialog::onSelectionChanged()
{
	commitCurrentSection();

	/* Moving to another section ends the run of edits made on the last one. */
	editBurstOpen = false;
	editBurstTimer->stop();

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
	beginEditUndoStep();
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

	beginUndoStep();
	document.sections.insert(currentIndex + 1, document.sections.at(currentIndex));
	refreshSectionList(currentIndex + 1);
	schedulePreviewRefresh();
}

void DesignerDialog::removeSection()
{
	if (currentIndex < 0 || currentIndex >= document.sections.size())
		return;

	beginUndoStep();

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

	beginUndoStep();
	document.sections.swapItemsAt(currentIndex, target);
	refreshSectionList(target);
	schedulePreviewRefresh();
}

void DesignerDialog::moveSectionTo(int from, int to)
{
	commitCurrentSection();

	const int count = document.sections.size();
	if (from < 0 || from >= count || to < 0 || to >= count || from == to)
		return;

	beginUndoStep();
	document.sections.move(from, to);
	refreshSectionList(to);
	schedulePreviewRefresh();
}

void DesignerDialog::savePreset(const QString &name, const TextStyle &style)
{
	/*
	 * An edit step rather than a structural one: this also fires on every keystroke and
	 * spinbox tick made while a preset is bound.
	 */
	beginEditUndoStep();
	document.setStylePreset(name, style);

	/* Re-publishing the list is what binds the editor that raised this to the preset. */
	editor->setPresets(document.stylePresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::deletePreset(const QString &name)
{
	beginUndoStep();
	document.removeStylePreset(name);

	editor->setPresets(document.stylePresets);
	commitCurrentSection();
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

	beginUndoStep();

	/* Only the content is taken; canvas and playback settings stay as configured here. */
	document.sections = loaded.sections;
	document.stylePresets = loaded.stylePresets;
	currentIndex = -1;
	editor->setPresets(document.stylePresets);
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
	if (previewInFlight) {
		previewAgain = true;
		return;
	}
	previewInFlight = true;

	/*
	 * The whole document goes to the render thread by value, so the user can keep editing
	 * while a long roll rasterises.
	 */
	postRenderJob([rendered = document, cache = logos, target = sink] {
		const StripRenderer renderer(cache.get());
		Strip strip = renderer.render(rendered);

		QMetaObject::invokeMethod(
			qApp,
			[rendered, strip = std::move(strip), target] {
				if (target->dialog)
					target->dialog->applyPreview(rendered, strip);
			},
			Qt::QueuedConnection);
	});
}

void DesignerDialog::applyPreview(const Document &rendered, const Strip &strip)
{
	previewInFlight = false;

	preview->setStrip(strip, rendered.width, rendered.height, rendered.background);

	const double travel = rendered.height + strip.height;
	const double seconds = rendered.scrollSpeed > 0.0 ? travel / rendered.scrollSpeed : 0.0;

	durationLabel->setText(QStringLiteral("%1 %2:%3  ·  %4 %5 px")
				       .arg(moduleText("Designer.Duration"))
				       .arg(static_cast<int>(seconds) / 60)
				       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'))
				       .arg(moduleText("Designer.StripHeight"))
				       .arg(strip.height));

	const QStringList missingFonts = missingFontFamilies(rendered);
	fontWarningLabel->setVisible(!missingFonts.isEmpty());
	if (!missingFonts.isEmpty())
		fontWarningLabel->setText(
			moduleText("Designer.MissingFonts").arg(missingFonts.join(QStringLiteral(", "))));

	/* Edits made while this render was out are picked up now, against the live document. */
	if (previewAgain) {
		previewAgain = false;
		refreshPreview();
	}
}

} // namespace closingtime
