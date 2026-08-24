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
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
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
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#include "model/StyleLibrary.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "ui/FontDialog.hpp"
#include "ui/PreviewWidget.hpp"
#include "ui/SectionEditor.hpp"
#include "ui/StyleLibraryDialog.hpp"
#include "ui/ToolButtons.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Milliseconds of quiet before an edit triggers a re-render of the preview strip. */
constexpr int kPreviewDebounceMs = 250;

/*
 * Milliseconds of quiet before edits to a linked preset are written to the library file.
 *
 * Long enough that typing a font name is one write rather than a dozen, short enough that another
 * OBS window watching the file sees the change while the user is still looking at this one.
 */
constexpr int kLibraryWriteDebounceMs = 400;

/* Milliseconds of quiet before a run of small edits becomes its own undo step. */
constexpr int kEditBurstMs = 900;

/* How many steps back the designer can go before the oldest is forgotten. */
constexpr int kUndoDepth = 100;

/* Opening widths of the section list, the editor and the preview, in pixels. */
const QList<int> kDefaultPaneSizes = {320, 560, 400};

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

/*
 * One source the Tools submenu offers. The reference is weak because the menu holds these for as
 * long as it is open: a strong one would keep a roll the user deleted mid-menu alive behind their
 * back, and the entry has to cope with the source going away regardless.
 */
struct MenuEntry {
	QString name;
	OBSWeakSource source;
};

struct MenuCollector {
	QByteArray sourceId;
	QVector<MenuEntry> entries;
};

/*
 * Every source of one type in the scene collection, whichever scene each sits in --
 * `obs_enum_sources` walks the collection's inputs rather than the current scene's items, which
 * is what makes a roll parked in a scene the user is not looking at reachable from here.
 */
QVector<MenuEntry> sourcesOfType(const QByteArray &sourceId)
{
	MenuCollector collector{sourceId, {}};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *found = static_cast<MenuCollector *>(param);

			const char *id = obs_source_get_id(source);
			if (id && found->sourceId == id)
				found->entries.append(MenuEntry{QString::fromUtf8(obs_source_get_name(source)),
								OBSGetWeakRef(source)});

			return true;
		},
		&collector);

	/* Listed by name: libobs hands them over in creation order, which reads as no order at all. */
	std::sort(collector.entries.begin(), collector.entries.end(), [](const MenuEntry &a, const MenuEntry &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});

	return collector.entries;
}

/* Rebuilds the Tools submenu from what is loaded right now. */
void fillDesignerMenu(QMenu *menu, const QByteArray &sourceId)
{
	menu->clear();

	const QVector<MenuEntry> entries = sourcesOfType(sourceId);
	if (entries.isEmpty()) {
		/* A disabled line rather than an empty menu, which reads as a broken one. */
		menu->addAction(moduleText("Designer.NoSources"))->setEnabled(false);
		return;
	}

	for (const MenuEntry &entry : entries) {
		QAction *open = menu->addAction(entry.name);

		/* Upgraded when picked, so a source destroyed while the menu is open opens nothing. */
		QObject::connect(open, &QAction::triggered, menu, [source = entry.source] {
			OBSSourceAutoRelease strong = obs_weak_source_get_source(source);
			if (strong)
				openDesignerFor(strong);
		});
	}
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

void openDesignerForAsync(obs_source_t *source)
{
	if (!source)
		return;

	/*
	 * A hotkey callback runs on the hotkey thread, and a window can only be opened on the UI
	 * one, so the request is queued rather than serviced where it lands. The weak reference is
	 * what makes a source destroyed in between the two ends of that queue a no-op.
	 */
	auto *weak = new OBSWeakSourceAutoRelease(obs_source_get_weak_source(source));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const std::unique_ptr<OBSWeakSourceAutoRelease> held(
				static_cast<OBSWeakSourceAutoRelease *>(param));

			OBSSourceAutoRelease strong = obs_weak_source_get_source(*held);
			if (strong)
				openDesignerFor(strong);
		},
		weak, false);
}

void registerDesignerToolsMenu(const char *sourceId)
{
	/*
	 * The qaction form rather than the plain menu item, because what goes in the Tools menu is
	 * a submenu rather than something to click: a roll is picked by name from a list of every
	 * one in the collection, which is a menu's own job and does not need a dialog to do it.
	 */
	auto *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ToolsMenu.Designer")));
	if (!action)
		return;

	/* Outlives the plugin's own objects, so it hangs off the window rather than off anything here. */
	auto *menu = new QMenu(mainWindow());
	action->setMenu(menu);

	const QByteArray id(sourceId);
	QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, id] { fillDesignerMenu(menu, id); });

	/*
	 * Filled once here as well, even though there is nothing to find at module load: an empty
	 * submenu is drawn as an unusable one by the macOS menu bar, which would leave the entry
	 * looking broken until something happened to open it. The placeholder is enough to keep it
	 * a submenu, and the first hover replaces it with what is actually loaded.
	 */
	fillDesignerMenu(menu, id);
}

void registerStyleLibraryToolsMenu()
{
	auto *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ToolsMenu.StyleLibrary")));
	if (!action)
		return;

	QObject::connect(action, &QAction::triggered, action, [] {
		/*
		 * Opened on no document: with no roll to publish from or link into, the window is the
		 * library alone. Parented to the main window and deleted on close, so it behaves like
		 * any other utility window rather than outliving the menu entry that opened it.
		 */
		auto *dialog = new StyleLibraryDialog(nullptr, mainWindow());
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->show();
	});
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
	splitter = new QSplitter(Qt::Horizontal, this);
	/* Folding a pane away is the collapse button's job now, not something a drag can do. */
	splitter->setChildrenCollapsible(false);

	/* --- left: the section list, in roll order --- */
	listPane = new QWidget(splitter);
	auto *listLayout = new QVBoxLayout(listPane);
	listLayout->setContentsMargins(0, 0, 0, 0);

	auto *listHeader = new QHBoxLayout();
	sectionsLabel = new QLabel(moduleText("Designer.Sections"), listPane);
	collapseButton = new QToolButton(listPane);
	collapseButton->setAutoRaise(true);
	collapseButton->setArrowType(Qt::LeftArrow);
	collapseButton->setToolTip(moduleText("Designer.Sections.Collapse"));
	listHeader->addWidget(sectionsLabel, 1);
	listHeader->addWidget(collapseButton);
	listLayout->addLayout(listHeader);

	sectionList = new SectionListWidget(listPane);
	sectionList->setToolTip(moduleText("Designer.Sections.Tip"));
	listLayout->addWidget(sectionList, 1);

	listButtonRow = new QWidget(listPane);
	auto *listButtons = new QHBoxLayout(listButtonRow);
	listButtons->setContentsMargins(0, 0, 0, 0);
	const auto addListButton = [&](QToolButton *button, auto slot) {
		listButtons->addWidget(button);
		connect(button, &QToolButton::clicked, this, slot);
	};

	/*
	 * Add is a menu button rather than a plain one: there are twelve section types and
	 * picking the type up front is what decides which editor fields appear.
	 */
	auto *addButton = makeGlyphButton(listButtonRow, QStringLiteral("+"), moduleText("Designer.Add"));
	addButton->setPopupMode(QToolButton::InstantPopup);
	listButtons->addWidget(addButton);

	addListButton(makeGlyphButton(listButtonRow, QStringLiteral("−"), moduleText("Designer.Remove")),
		      &DesignerDialog::removeSection);
	addListButton(makeArrowButton(listButtonRow, Qt::UpArrow, moduleText("Designer.MoveUp")),
		      [this] { moveSection(-1); });
	addListButton(makeArrowButton(listButtonRow, Qt::DownArrow, moduleText("Designer.MoveDown")),
		      [this] { moveSection(1); });
	/* No glyph says "duplicate" without a theme icon behind it, so this one keeps its word. */
	addListButton(makeLabelledButton(listButtonRow, moduleText("Designer.Duplicate")),
		      &DesignerDialog::duplicateSection);
	listButtons->addStretch();
	listLayout->addWidget(listButtonRow);

	auto *addMenu = new QMenu(addButton);
	for (SectionType type : allSectionTypes()) {
		QAction *action = addMenu->addAction(QString::fromUtf8(sectionTypeName(type)));
		connect(action, &QAction::triggered, this, [this, type] {
			commitCurrentSection();
			beginUndoStep();

			/*
			 * Next to whatever is selected, in the same container: a section added while a
			 * child of a sticky block is selected joins that block, which is the only
			 * reading of "add one here" that does not surprise anyone.
			 */
			const SectionPath at = currentPath.isValid()
						       ? SectionPath{currentPath.parent, currentPath.index + 1}
						       : SectionPath{-1, static_cast<int>(document.sections.size())};

			const SectionPath added = insertSection(at, Section::makeDefault(type));
			refreshSectionList(-1);
			refreshSectionList(rowOf(added));
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
	preview->setToolTip(moduleText("Designer.Preview.Tip"));
	previewLayout->addWidget(preview, 1);
	/*
	 * Off by default: this is a view of the layout rather than of the roll, wanted only while
	 * a section is not landing where its settings say it should.
	 */
	layoutBoxesCheck = new QCheckBox(moduleText("Designer.LayoutBoxes"), previewPane);
	layoutBoxesCheck->setToolTip(moduleText("Designer.LayoutBoxes.Tip"));
	previewLayout->addWidget(layoutBoxesCheck);
	/*
	 * Off by default, like the overlay above it and for the same reason: this pane is what a roll
	 * is written in, and something moving in the corner of it while a name is being typed is a
	 * distraction rather than a feature. With it off every animated logo shows its first frame,
	 * which is the frame the layout was measured from.
	 */
	animateCheck = new QCheckBox(moduleText("Designer.PlayAnimations"), previewPane);
	animateCheck->setToolTip(moduleText("Designer.PlayAnimations.Tip"));
	animateCheck->setEnabled(false);
	previewLayout->addWidget(animateCheck);
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
	/*
	 * Wider than the stretch factors alone would open it: the list carries a section's own
	 * label, and a roll's sections are told apart by reading them rather than by counting them.
	 */
	splitter->setSizes(kDefaultPaneSizes);
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

	auto *libraryButton = new QPushButton(moduleText("Designer.StyleLibrary"), this);
	libraryButton->setToolTip(moduleText("Designer.StyleLibrary.Tip"));
	auto *fontsButton = new QPushButton(moduleText("Designer.Fonts"), this);
	fontsButton->setToolTip(moduleText("Designer.Fonts.Tip"));
	auto *importButton = new QPushButton(moduleText("Designer.ImportJson"), this);
	auto *exportButton = new QPushButton(moduleText("Designer.ExportJson"), this);
	footer->addWidget(libraryButton);
	footer->addWidget(fontsButton);
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

	libraryWriteTimer = new QTimer(this);
	libraryWriteTimer->setSingleShot(true);
	libraryWriteTimer->setInterval(kLibraryWriteDebounceMs);

	connect(layoutBoxesCheck, &QCheckBox::toggled, this, [this](bool on) { preview->setLayoutBoxesVisible(on); });
	connect(animateCheck, &QCheckBox::toggled, this, [this](bool on) { preview->setAnimationPlaying(on); });
	connect(refreshTimer, &QTimer::timeout, this, &DesignerDialog::refreshPreview);
	connect(editBurstTimer, &QTimer::timeout, this, [this] { editBurstOpen = false; });
	connect(libraryWriteTimer, &QTimer::timeout, this, &DesignerDialog::flushLibraryEdits);
	connect(collapseButton, &QToolButton::clicked, this, [this] { setSectionsCollapsed(!sectionsCollapsed); });
	connect(undoButton, &QPushButton::clicked, this, &DesignerDialog::undo);
	connect(redoButton, &QPushButton::clicked, this, &DesignerDialog::redo);
	connect(sectionList, &QListWidget::currentRowChanged, this, &DesignerDialog::onSelectionChanged);
	connect(sectionList, &SectionListWidget::rowMoved, this, &DesignerDialog::moveSectionTo);
	connect(editor, &SectionEditor::changed, this, &DesignerDialog::onSectionEdited);
	connect(editor, &SectionEditor::presetSaveRequested, this, &DesignerDialog::savePreset);
	connect(editor, &SectionEditor::presetDeleteRequested, this, &DesignerDialog::deletePreset);
	connect(libraryButton, &QPushButton::clicked, this, &DesignerDialog::openStyleLibrary);
	connect(fontsButton, &QPushButton::clicked, this, &DesignerDialog::openFonts);
	connect(importButton, &QPushButton::clicked, this, &DesignerDialog::importJson);
	connect(exportButton, &QPushButton::clicked, this, &DesignerDialog::exportJson);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		commitCurrentSection();
		flushLibraryEdits();
		writeToSource();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &DesignerDialog::reject);
	connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] {
		commitCurrentSection();
		flushLibraryEdits();
		writeToSource();
	});

	/*
	 * A library edited in another OBS window, imported from elsewhere, or written by hand shows up
	 * here without anything being reopened. Watching the file rather than polling it is what makes
	 * a shared library feel shared: the edit lands in the preview as it is made.
	 */
	libraryWatcher = new QFileSystemWatcher(this);
	librarySerial = StyleLibrary::instance().serial();
	if (const QString path = StyleLibrary::instance().filePath(); !path.isEmpty()) {
		libraryWatcher->addPath(QFileInfo(path).absolutePath());
		if (QFileInfo::exists(path))
			libraryWatcher->addPath(path);
	}
	connect(libraryWatcher, &QFileSystemWatcher::fileChanged, this, &DesignerDialog::reloadStyleLibrary);
	connect(libraryWatcher, &QFileSystemWatcher::directoryChanged, this, &DesignerDialog::reloadStyleLibrary);

	loadFromSource();
	refreshUndoButtons();
}

void DesignerDialog::reloadStyleLibrary()
{
	StyleLibrary &library = StyleLibrary::instance();

	/*
	 * Watching a file that is replaced rather than written in place -- which is what a save
	 * through QSaveFile does -- drops the watch with it, so the path is re-added on every
	 * notification. The directory watch is what catches a library created after this window
	 * opened.
	 */
	const QString path = library.filePath();
	if (!path.isEmpty() && QFileInfo::exists(path) && !libraryWatcher->files().contains(path))
		libraryWatcher->addPath(path);

	library.load();
	if (library.serial() == librarySerial)
		return;

	librarySerial = library.serial();

	if (!document.refreshLinkedPresets())
		return;

	/*
	 * Not an undo step. This is not an edit the user made here, and putting it on the stack would
	 * mean Ctrl+Z appearing to undo a change made in another window -- which it could not do, since
	 * the library would still hold the new style and the next reload would bring it straight back.
	 */
	editor->setPresets(document.stylePresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::flushLibraryEdits()
{
	if (pendingLibraryEdits.isEmpty())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	for (auto it = pendingLibraryEdits.cbegin(); it != pendingLibraryEdits.cend(); ++it)
		library.set(it.key(), it.value());

	pendingLibraryEdits.clear();
	/* Our own writes must not read back as somebody else's change on the next reload. */
	librarySerial = library.serial();
}

void DesignerDialog::openFonts()
{
	/* A family typed into the section editor and not yet committed is one this window is about. */
	commitCurrentSection();

	FontDialog dialog(&document, this);
	connect(&dialog, &FontDialog::documentAboutToChange, this, [this] { beginUndoStep(); });
	connect(&dialog, &FontDialog::documentChanged, this, [this] { schedulePreviewRefresh(); });

	dialog.exec();
}

void DesignerDialog::openStyleLibrary()
{
	commitCurrentSection();

	/* The manager reads the library, so anything still queued here belongs in it first. */
	flushLibraryEdits();

	StyleLibraryDialog dialog(&document, this);
	connect(&dialog, &StyleLibraryDialog::documentAboutToChange, this, [this] { beginUndoStep(); });
	connect(&dialog, &StyleLibraryDialog::documentChanged, this, [this] {
		editor->setPresets(document.stylePresets);
		schedulePreviewRefresh();
	});

	dialog.exec();

	/* The library's own serial moved if anything was published, imported or deleted in there. */
	librarySerial = StyleLibrary::instance().serial();
}

DesignerDialog::~DesignerDialog()
{
	/*
	 * An edit to a shared style is not this window's to discard: Cancel drops the roll's own
	 * changes, but "change it everywhere" was answered about the library, and the library is not
	 * what Cancel is about.
	 */
	flushLibraryEdits();

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
	merged.bundleFonts = document.bundleFonts;
	merged.fontSubstitutions = document.fontSubstitutions;
	merged.bundledFonts = document.bundledFonts;

	/*
	 * Applying is where the roll's fonts are collected, because it is the one moment the content
	 * is settled and the window is not being typed into: a font added to a heading two keystrokes
	 * ago is in the bundle by the time the source is told about it, and a family that has not
	 * changed costs a comparison rather than a walk of the machine's font directories.
	 */
	merged.refreshFontBundle();
	document.bundledFonts = merged.bundledFonts;

	OBSDataAutoRelease updated = obs_data_create();
	merged.save(updated);
	obs_source_update(source, updated);
}

Section *DesignerDialog::sectionAt(const SectionPath &path)
{
	return const_cast<Section *>(std::as_const(*this).sectionAt(path));
}

const Section *DesignerDialog::sectionAt(const SectionPath &path) const
{
	if (path.index < 0)
		return nullptr;

	if (path.parent < 0)
		return path.index < document.sections.size() ? &document.sections.at(path.index) : nullptr;

	if (path.parent >= document.sections.size())
		return nullptr;

	const std::vector<Section> &children = document.sections.at(path.parent).children;
	return static_cast<size_t>(path.index) < children.size() ? &children[static_cast<size_t>(path.index)]
								 : nullptr;
}

QVector<DesignerDialog::SectionPath> DesignerDialog::pathsInOrder() const
{
	QVector<SectionPath> paths;
	paths.reserve(document.sections.size());

	for (int index = 0; index < document.sections.size(); ++index) {
		paths.append(SectionPath{-1, index});

		const std::vector<Section> &children = document.sections.at(index).children;
		for (int child = 0; child < static_cast<int>(children.size()); ++child)
			paths.append(SectionPath{index, child});
	}

	return paths;
}

int DesignerDialog::rowOf(const SectionPath &path) const
{
	for (int row = 0; row < rowPaths.size(); ++row) {
		if (rowPaths.at(row).parent == path.parent && rowPaths.at(row).index == path.index)
			return row;
	}
	return -1;
}

DesignerDialog::SectionPath DesignerDialog::insertSection(const SectionPath &path, const Section &section)
{
	/*
	 * A sticky block never goes inside another one -- the model says so and the loader enforces
	 * it -- so one dropped into a block lands after it instead of vanishing into it.
	 */
	if (path.parent >= 0 && section.type == SectionType::StickyBlock) {
		const int after = std::min(path.parent + 1, static_cast<int>(document.sections.size()));
		document.sections.insert(after, section);
		return SectionPath{-1, after};
	}

	if (path.parent < 0) {
		const int at = std::clamp(path.index, 0, static_cast<int>(document.sections.size()));
		document.sections.insert(at, section);
		return SectionPath{-1, at};
	}

	if (path.parent >= document.sections.size())
		return SectionPath{};

	std::vector<Section> &children = document.sections[path.parent].children;
	const int at = std::clamp(path.index, 0, static_cast<int>(children.size()));
	children.insert(children.begin() + at, section);
	return SectionPath{path.parent, at};
}

void DesignerDialog::removeSectionAt(const SectionPath &path)
{
	if (!sectionAt(path))
		return;

	if (path.parent < 0) {
		document.sections.removeAt(path.index);
		return;
	}

	std::vector<Section> &children = document.sections[path.parent].children;
	children.erase(children.begin() + path.index);
}

int DesignerDialog::highlightFor(const SectionPath &path) const
{
	/*
	 * The overlay and the preview know sections by their top-level index -- a child is drawn as
	 * part of its block's picture and has no box of its own out there -- so selecting a child
	 * highlights the block it belongs to.
	 */
	return path.parent >= 0 ? path.parent : path.index;
}

void DesignerDialog::refreshSectionList(int selectRow)
{
	const QSignalBlocker blocker(sectionList);

	sectionList->clear();
	rowPaths = pathsInOrder();

	for (const SectionPath &path : rowPaths) {
		const Section *section = sectionAt(path);
		if (!section)
			continue;

		/*
		 * Children are indented by their label rather than by a delegate: a list is the right
		 * widget for a roll that is a run of sections with one shallow exception in it, and a
		 * tree would trade the drag-and-drop reordering that works for one that has to be
		 * taught which drops mean what.
		 */
		const QString label = path.parent >= 0 ? QStringLiteral("      ") + section->displayLabel()
						       : section->displayLabel();

		auto *item = new QListWidgetItem(label, sectionList);
		item->setToolTip(QString::fromUtf8(sectionTypeName(section->type)));
		if (!section->visible)
			item->setForeground(Qt::gray);
	}

	const int row = std::clamp(selectRow, -1, static_cast<int>(rowPaths.size()) - 1);
	sectionList->setCurrentRow(row);
	currentRow = row;
	currentPath = row >= 0 ? rowPaths.at(row) : SectionPath{};
	/* Set here as well as on selection: this path moves the row with the list's signals blocked. */
	preview->setHighlightedSection(row >= 0 ? highlightFor(currentPath) : -1);

	if (const Section *section = sectionAt(currentPath))
		editor->setSection(*section);

	editorScroll->setEnabled(row >= 0);
}

DesignerDialog::DocumentSnapshot DesignerDialog::snapshot() const
{
	return DocumentSnapshot{document.sections,     document.stylePresets,      document.bundleFonts,
				document.bundledFonts, document.fontSubstitutions, currentRow};
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
	document.bundleFonts = state.bundleFonts;
	document.bundledFonts = state.bundledFonts;
	document.fontSubstitutions = state.fontSubstitutions;

	/* Cleared first so refreshSectionList cannot write the editor back into a stale row. */
	currentPath = SectionPath{};
	currentRow = -1;
	editor->setPresets(document.stylePresets);
	refreshSectionList(-1);
	refreshSectionList(std::min(state.currentIndex, static_cast<int>(rowPaths.size()) - 1));
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

void DesignerDialog::setSectionsCollapsed(bool collapsed)
{
	if (collapsed == sectionsCollapsed)
		return;

	/* Taken before anything is hidden, so the pane comes back the width it went away at. */
	if (collapsed)
		expandedSizes = splitter->sizes();

	sectionsCollapsed = collapsed;

	sectionsLabel->setVisible(!collapsed);
	sectionList->setVisible(!collapsed);
	listButtonRow->setVisible(!collapsed);

	collapseButton->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
	collapseButton->setToolTip(moduleText(collapsed ? "Designer.Sections.Expand" : "Designer.Sections.Collapse"));

	/*
	 * The pane is pinned to the width of the button rather than hidden outright: the button is
	 * what reopens it, so it has to stay on screen and stay where it was.
	 */
	if (collapsed) {
		/* Re-laid out first, so the width asked for is the folded one and not the stale one. */
		listPane->layout()->activate();
		listPane->setMaximumWidth(
			std::max(collapseButton->sizeHint().width(), listPane->layout()->minimumSize().width()));
		return;
	}

	listPane->setMaximumWidth(QWIDGETSIZE_MAX);
	splitter->setSizes(expandedSizes.size() == splitter->count() ? expandedSizes : kDefaultPaneSizes);
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
	currentRow = row;
	currentPath = row >= 0 && row < rowPaths.size() ? rowPaths.at(row) : SectionPath{};
	editorScroll->setEnabled(row >= 0);
	preview->setHighlightedSection(currentPath.isValid() ? highlightFor(currentPath) : -1);

	if (const Section *section = sectionAt(currentPath))
		editor->setSection(*section);
}

void DesignerDialog::commitCurrentSection()
{
	Section *section = sectionAt(currentPath);
	if (!section)
		return;

	/*
	 * The editor knows nothing about children -- a sticky block's contents are edited as their
	 * own rows -- so what it hands back carries none, and the block's own are put back over the
	 * top of it. Writing the editor's copy through unchanged would empty the block on the first
	 * keystroke made anywhere on its form.
	 */
	std::vector<Section> children = std::move(section->children);
	*section = editor->section();
	section->children = std::move(children);
}

void DesignerDialog::onSectionEdited()
{
	beginEditUndoStep();
	commitCurrentSection();

	const Section *section = sectionAt(currentPath);
	if (section && currentRow >= 0 && currentRow < sectionList->count()) {
		const QSignalBlocker blocker(sectionList);
		QListWidgetItem *item = sectionList->item(currentRow);
		item->setText(currentPath.parent >= 0 ? QStringLiteral("      ") + section->displayLabel()
						      : section->displayLabel());
		item->setForeground(section->visible ? QBrush() : QBrush(Qt::gray));
	}

	schedulePreviewRefresh();
}

void DesignerDialog::duplicateSection()
{
	commitCurrentSection();

	const Section *section = sectionAt(currentPath);
	if (!section)
		return;

	beginUndoStep();
	/* Copied first: inserting into the container can move the section the pointer names. */
	const Section copy = *sectionAt(currentPath);
	const SectionPath added = insertSection(SectionPath{currentPath.parent, currentPath.index + 1}, copy);
	refreshSectionList(-1);
	refreshSectionList(rowOf(added));
	schedulePreviewRefresh();
}

bool DesignerDialog::confirmRemoveSection(const Section &section)
{
	if (!askBeforeRemovingSection)
		return true;

	QMessageBox box(this);
	box.setWindowTitle(moduleText("Designer.RemoveSection.Title"));
	/*
	 * Named rather than merely counted, because the section list is a column of short labels and
	 * "the selected one" is exactly what a misclick makes uncertain.
	 */
	box.setText(moduleText("Designer.RemoveSection.Confirm").arg(section.displayLabel()));
	box.setIcon(QMessageBox::Question);
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
	box.setDefaultButton(QMessageBox::Cancel);

	/*
	 * Remembered for this window rather than for the machine: undo already covers a delete, so
	 * the box is a courtesy rather than the safety net, and someone who has switched it off
	 * gets it back by opening the designer again rather than by hunting for a setting.
	 */
	auto *remember = new QCheckBox(moduleText("Designer.RemoveSection.DontAsk"), &box);
	box.setCheckBox(remember);

	const bool confirmed = box.exec() == QMessageBox::Yes;
	if (confirmed && remember->isChecked())
		askBeforeRemovingSection = false;

	return confirmed;
}

void DesignerDialog::removeSection()
{
	const Section *section = sectionAt(currentPath);
	if (!section)
		return;

	if (!confirmRemoveSection(*section))
		return;

	beginUndoStep();

	const int removedRow = currentRow;
	/* Drop the stale selection first so the editor cannot write back into the gap. */
	const SectionPath removed = currentPath;
	currentPath = SectionPath{};
	currentRow = -1;
	removeSectionAt(removed);

	refreshSectionList(-1);
	refreshSectionList(std::min(removedRow, static_cast<int>(rowPaths.size()) - 1));
	schedulePreviewRefresh();
}

void DesignerDialog::moveSection(int delta)
{
	commitCurrentSection();

	/*
	 * Within the section's own container. The buttons are for ordering a run, and a section that
	 * hopped into or out of a sticky block on its way up the list would be doing something the
	 * arrow it was clicked on never said it would; dragging is what moves a section between
	 * containers, where the drop says plainly where it landed.
	 */
	const SectionPath target{currentPath.parent, currentPath.index + delta};
	if (!sectionAt(currentPath) || !sectionAt(target))
		return;

	beginUndoStep();

	if (currentPath.parent < 0) {
		document.sections.swapItemsAt(currentPath.index, target.index);
	} else {
		std::vector<Section> &children = document.sections[currentPath.parent].children;
		std::swap(children[static_cast<size_t>(currentPath.index)],
			  children[static_cast<size_t>(target.index)]);
	}

	refreshSectionList(-1);
	refreshSectionList(rowOf(target));
	schedulePreviewRefresh();
}

void DesignerDialog::moveSectionTo(int from, int to)
{
	commitCurrentSection();

	const int count = rowPaths.size();
	if (from < 0 || from >= count || to < 0 || to >= count || from == to)
		return;

	const SectionPath fromPath = rowPaths.at(from);
	const Section *moved = sectionAt(fromPath);
	if (!moved)
		return;

	const Section section = *moved;

	beginUndoStep();
	removeSectionAt(fromPath);

	/*
	 * Where it lands is decided by the row it now follows, which is the rule the list is already
	 * showing: drop something under a sticky block, or under one of that block's own rows, and it
	 * joins the block; drop it under anything else and it is top-level. Reading the drop off what
	 * precedes it is what lets one drag both put a section into a block and take it back out,
	 * without a second gesture to learn.
	 */
	const QVector<SectionPath> after = pathsInOrder();
	const int predecessorRow = std::min(to, static_cast<int>(after.size())) - 1;

	SectionPath at{-1, 0};
	if (predecessorRow >= 0 && predecessorRow < after.size()) {
		const SectionPath &before = after.at(predecessorRow);
		const Section *precedes = sectionAt(before);

		if (before.parent >= 0) {
			at = SectionPath{before.parent, before.index + 1};
		} else if (precedes && precedes->type == SectionType::StickyBlock) {
			at = SectionPath{before.index, 0};
		} else {
			at = SectionPath{-1, before.index + 1};
		}
	}

	const SectionPath landed = insertSection(at, section);
	refreshSectionList(-1);
	refreshSectionList(rowOf(landed));
	schedulePreviewRefresh();
}

bool DesignerDialog::shouldEditLinkedPreset(const QString &name)
{
	if (StyleLibrary::instance().alwaysEditLinked())
		return true;

	const auto answered = linkedEditChoices.constFind(name);
	if (answered != linkedEditChoices.constEnd())
		return *answered;

	QMessageBox box(this);
	box.setWindowTitle(moduleText("Library.EditLinked.Title"));
	box.setText(moduleText("Library.EditLinked").arg(name));
	box.setIcon(QMessageBox::Question);

	QPushButton *everywhere = box.addButton(moduleText("Library.EditLinked.Everywhere"), QMessageBox::YesRole);
	QPushButton *fork = box.addButton(moduleText("Library.EditLinked.Fork"), QMessageBox::NoRole);
	/* Forking is the default because it is the one that cannot surprise anybody else. */
	box.setDefaultButton(fork);

	auto *remember = new QCheckBox(moduleText("Library.EditLinked.Remember"), &box);
	box.setCheckBox(remember);

	box.exec();

	const bool editLibrary = box.clickedButton() == everywhere;
	linkedEditChoices.insert(name, editLibrary);

	/*
	 * Only "change everywhere" is worth remembering across windows. Someone who wants to be asked
	 * again after choosing to fork has nothing to turn back on, whereas a remembered fork would
	 * quietly make the library read-only from the editor with no way back to it.
	 */
	if (remember->isChecked() && editLibrary)
		StyleLibrary::instance().setAlwaysEditLinked(true);

	return editLibrary;
}

void DesignerDialog::savePreset(const QString &name, const TextStyle &style)
{
	/*
	 * An edit step rather than a structural one: this also fires on every keystroke and
	 * spinbox tick made while a preset is bound.
	 */
	beginEditUndoStep();

	/*
	 * A preset that follows the library is shared with every other roll on the machine, so an
	 * edit to one has to say which of the two it means: restyle everything, or take a copy for
	 * this roll. Asked once per preset per window -- see shouldEditLinkedPreset.
	 */
	for (StylePreset &preset : document.stylePresets) {
		if (preset.name != name || !preset.linked)
			continue;

		if (shouldEditLinkedPreset(name)) {
			pendingLibraryEdits.insert(name, style);
			libraryWriteTimer->start();
		} else {
			/* The document's own copy from here on; the library carries on as it was. */
			preset.linked = false;
		}
		break;
	}

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
	currentPath = SectionPath{};
	currentRow = -1;
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
	postRenderJob([rendered = document, cache = logos, animationCache = animations, target = sink] {
		const StripRenderer renderer(cache.get(), animationCache.get());
		/*
		 * Collected on every preview render rather than only while the overlay is showing,
		 * so switching it on draws what is already on screen instead of waiting on a
		 * rebuild. It is a few rectangles per section against a full rasterisation.
		 */
		LayoutBoxes boxes;
		Strip strip = renderer.render(rendered, &boxes);

		QMetaObject::invokeMethod(
			qApp,
			[rendered, strip = std::move(strip), boxes = std::move(boxes), target] {
				if (target->dialog)
					target->dialog->applyPreview(rendered, strip, boxes);
			},
			Qt::QueuedConnection);
	});
}

void DesignerDialog::applyPreview(const Document &rendered, const Strip &strip, const LayoutBoxes &boxes)
{
	previewInFlight = false;

	preview->setStrip(strip, rendered.width, rendered.height, rendered.background);
	preview->setLayoutBoxes(boxes);
	preview->setHighlightedSection(currentPath.isValid() ? highlightFor(currentPath) : -1);

	const double travel = rendered.height + strip.height;
	const double seconds = rendered.scrollSpeed > 0.0 ? travel / rendered.scrollSpeed : 0.0;

	durationLabel->setText(QStringLiteral("%1 %2:%3  ·  %4 %5 px")
				       .arg(moduleText("Designer.Duration"))
				       .arg(static_cast<int>(seconds) / 60)
				       .arg(static_cast<int>(seconds) % 60, 2, 10, QLatin1Char('0'))
				       .arg(moduleText("Designer.StripHeight"))
				       .arg(strip.height));

	QStringList warnings;

	/*
	 * Only the families nothing has been done about are a warning. One the roll carries its own
	 * file for is not missing at all by the time this is asked -- the render registered it -- and
	 * one with a stand-in recorded is reported separately, because a deliberate substitution is
	 * worth saying and is not a problem to go and fix.
	 */
	const QStringList unresolvedFonts = unresolvedFontFamilies(rendered);
	if (!unresolvedFonts.isEmpty())
		warnings.append(moduleText("Designer.MissingFonts").arg(unresolvedFonts.join(QStringLiteral(", "))));

	QStringList substituted;
	for (const QString &family : missingFontFamilies(rendered)) {
		const QString substitute = rendered.fontSubstitute(family);
		if (!substitute.isEmpty())
			substituted.append(QStringLiteral("%1 → %2").arg(family, substitute));
	}

	if (!substituted.isEmpty())
		warnings.append(moduleText("Designer.SubstitutedFonts").arg(substituted.join(QStringLiteral(", "))));

	/*
	 * Reported from the strip rather than from the document, because it is an answer only the
	 * decoder has: how long a file turned out to be once every frame of it had been read.
	 */
	const bool truncated = std::any_of(strip.animatedLogos.cbegin(), strip.animatedLogos.cend(),
					   [](const AnimatedLogoPlacement &placement) {
						   return placement.animation && placement.animation->truncated;
					   });
	if (truncated)
		warnings.append(moduleText("Designer.LogoTruncated").arg(kMaxLogoDurationMs / 1000));

	fontWarningLabel->setVisible(!warnings.isEmpty());
	fontWarningLabel->setText(warnings.join(QStringLiteral("\n")));

	animateCheck->setEnabled(preview->hasAnimatedLogos());
	if (!preview->hasAnimatedLogos() && animateCheck->isChecked())
		animateCheck->setChecked(false);

	/* Edits made while this render was out are picked up now, against the live document. */
	if (previewAgain) {
		previewAgain = false;
		refreshPreview();
	}
}

} // namespace closingtime
