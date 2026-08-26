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

#pragma once

#include <obs.hpp>

#include <QDialog>
#include <QHash>
#include <QListWidget>
#include <QVector>

#include <memory>

#include "model/CreditsModel.hpp"
#include "render/StripRenderer.hpp"

class QCheckBox;
class QDialogButtonBox;
class QFileSystemWatcher;
class QLabel;
class QPushButton;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;

namespace closingtime {

class PreviewWidget;
class SectionEditor;

/*
 * The section list, with drag-and-drop reordering.
 *
 * The drop is reported rather than carried out. The dialog owns the section order, and
 * letting the view rearrange its own items too would leave two orders to keep in step --
 * so the view stays a pure display of whatever the document currently says.
 */
class SectionListWidget : public QListWidget {
	Q_OBJECT

public:
	explicit SectionListWidget(QWidget *parent = nullptr);

signals:
	/* `to` is where the dragged row lands, counted after it has been lifted out. */
	void rowMoved(int from, int to);

	/*
	 * A click on the fold arrow of a sticky block's row. Which rows carry one is the dialog's
	 * business and is marked on the item itself, so the list needs to know nothing about what a
	 * section is to keep the click off the selection and out of a drag.
	 */
	void blockFoldToggled(int row);

protected:
	void mousePressEvent(QMouseEvent *event) override;
	void dropEvent(QDropEvent *event) override;
};

/*
 * Opens the designer for `source`, raising the existing window if one is already open for
 * it. Must be called from the UI thread.
 */
void openDesignerFor(obs_source_t *source);

/*
 * The same, from anywhere. A hotkey arrives on the hotkey thread and a window can only be opened
 * on the UI one, so the call is queued; a weak reference makes a source destroyed in between the
 * two ends of that queue a no-op rather than a crash.
 */
void openDesignerForAsync(obs_source_t *source);

/*
 * Adds the Tools menu entry that opens the designer without going through a source's properties
 * window: a submenu listing every source of type `sourceId` in the scene collection by name,
 * whichever scene each one happens to sit in, opening that source's designer when picked.
 *
 * The list is rebuilt each time the submenu is opened, so it follows sources being added,
 * renamed and removed without anything having to watch for it. Must be called from the UI
 * thread, once the frontend exists.
 */
void registerDesignerToolsMenu(const char *sourceId);

/*
 * Adds the Style Library entry to the Tools menu.
 *
 * Separate from the designer's entry because the library is separate from any one roll: styles
 * shared by every source on the machine should not need a source to be found and opened before
 * they can be renamed or thrown away.
 */
void registerStyleLibraryToolsMenu();

/*
 * Adds the Style Library entry to the Tools menu.
 *
 * Separate from the designer's entry because the library is separate from any one roll: styles
 * shared by every source on the machine should not need a source to be found and opened before
 * they can be renamed or thrown away.
 */
void registerStyleLibraryToolsMenu();

/* Closes any designer window bound to `source`. Safe to call for sources with none. */
void closeDesignerFor(obs_source_t *source);

/*
 * The credit roll designer: a section list on the left, the editor for the selected
 * section in the middle, and a live preview of the whole strip on the right. It works on
 * its own copy of the document and only writes back to the source on Apply or OK.
 */
class DesignerDialog : public QDialog {
	Q_OBJECT

public:
	DesignerDialog(obs_source_t *source, QWidget *parent = nullptr);
	~DesignerDialog() override;

	/* True once the bound source has been destroyed and the window should go away. */
	bool sourceIsGone() const;

private:
	void loadFromSource();
	void writeToSource();

	void refreshSectionList(int selectRow);
	void onSelectionChanged();
	void onSectionEdited();

	void duplicateSection();
	void removeSection();
	/*
	 * Asks before a section goes, unless this window has been told not to. True when the delete
	 * should go ahead.
	 */
	bool confirmRemoveSection(const Section &section);
	void moveSection(int delta);
	void moveSectionTo(int from, int to);

	/* Style presets live on the document, so the editors route their edits through here. */
	void savePreset(const QString &name, const TextStyle &style);
	void deletePreset(const QString &name);
	/* The panels' twins of the two above; see BackgroundEditor and Document::backgroundPresets. */
	void saveBackgroundPreset(const QString &name, const BackgroundPanel &panel);
	void deleteBackgroundPreset(const QString &name);

	/*
	 * Decides what editing a style bound to a *linked* preset does: change it in the library, for
	 * every roll on the machine, or fork a copy that belongs to this document alone.
	 *
	 * Asked once per preset per window, and remembered, because the question arrives on a
	 * keystroke -- every character typed into a font name while a preset is bound is an edit --
	 * and a dialog per keystroke is not a question, it is an obstruction. Returns true when the
	 * edit should go to the library.
	 */
	/*
	 * Whether an edit to a preset that follows the library should change it everywhere, or fork a
	 * copy into this document.
	 *
	 * `choices` is the per-window memory of what was answered for a name, and is passed in rather
	 * than reached for, because a background and a text style may share a name and are two
	 * different things to be asked about: one memory each keeps the answer for "Card the panel"
	 * from answering for "Card the heading style".
	 */
	bool shouldEditLinkedPreset(const QString &name, QHash<QString, bool> *choices);

	/* Opens the library manager on this document. */
	void openStyleLibrary();
	/* The font window: what this roll carries, and what stands in for what it cannot. */
	void openFonts();

	/*
	 * Writes the pending edits to a linked preset out to the library file.
	 *
	 * Debounced, because the edit that reaches savePreset() is a keystroke: a bound style being
	 * typed into raises one per character, and each would otherwise be a write of a file every
	 * other source on the machine is watching. The document is updated immediately either way --
	 * this is only about when the file catches up -- and a flush is forced on Apply, OK and close
	 * so nothing is lost by a window going away mid-run.
	 */
	void flushLibraryEdits();

	/* Re-reads the library and restyles the roll when the file has changed underneath it. */
	void reloadStyleLibrary();

	void importJson();
	void exportJson();

	/*
	 * Queues a re-render of the preview strip on the render thread. The result comes back
	 * through applyPreview once it is ready; the previous strip stays on screen until then.
	 */
	void refreshPreview();
	void schedulePreviewRefresh();

	/*
	 * Shows a finished strip along with the duration readout and font warning. Takes the
	 * document the strip was rendered from rather than reading the live one, so the canvas
	 * outline and the readouts always describe the pixels actually on screen.
	 */
	void applyPreview(const Document &rendered, const Strip &strip, const LayoutBoxes &boxes);

	/* Commits the editor's current state into `document` at `currentPath`. */
	void commitCurrentSection();

	/*
	 * Undo covers exactly what the designer owns -- sections and style presets -- because
	 * canvas and playback settings belong to the properties dialog and have their own
	 * lifecycle. Whole-document snapshots rather than per-field commands: a document is a
	 * handful of kilobytes of implicitly shared containers, and there is no edit here that
	 * a command object would meaningfully compress.
	 */
	struct DocumentSnapshot {
		QVector<Section> sections;
		QVector<StylePreset> stylePresets;
		QVector<BackgroundPreset> backgroundPresets;
		/*
		 * The font settings ride along because the font window is undoable like everything
		 * else here. Carrying the bundle costs nothing per step: a QVector of QByteArrays
		 * copies by sharing, and nothing here ever edits one in place.
		 */
		bool bundleFonts = true;
		QVector<BundledFont> bundledFonts;
		QVector<FontSubstitution> fontSubstitutions;
		int currentIndex = -1;
	};

	DocumentSnapshot snapshot() const;
	void restore(const DocumentSnapshot &state);

	/* Records the state to come back to. Call before anything that mutates content. */
	void beginUndoStep();

	/*
	 * Opens an undo step covering a run of small edits, so typing a name is one undo step
	 * rather than one per keystroke. The run closes on the next selection change, on any
	 * structural edit, or after a period of quiet.
	 */
	void beginEditUndoStep();

	void undo();
	void redo();
	void refreshUndoButtons();

	/*
	 * Folds the section list away to the button in its own header, and back.
	 *
	 * Dragging the splitter shut did this before, which is easy to do by accident and leaves
	 * nothing on screen saying how to undo it. The splitter no longer collapses its children at
	 * all; the button is the one way in and, because it stays put when the pane folds, the one
	 * way back out.
	 */
	void setSectionsCollapsed(bool collapsed);

	/*
	 * The same for the preview, which folds away to the right the way the list folds away to the
	 * left. A roll spends long stretches being typed rather than looked at, and the pane the
	 * words are typed into is the one that wants the room back.
	 */
	void setPreviewCollapsed(bool collapsed);

	/*
	 * The mechanics both of the above share: pinning a pane to the width of its own button, and
	 * giving it back the width it had.
	 *
	 * `rememberedWidth` is that pane's own, rather than a snapshot of the whole splitter, so
	 * folding one pane while the other is folded cannot hand the second one the first one's
	 * folded width when it is opened again. What is given up and taken back is the editor's
	 * share in the middle, which is the pane with room to spare.
	 */
	void setPaneFolded(QWidget *pane, QToolButton *button, int *rememberedWidth, bool folded);

	/*
	 * How a finished render finds its way back to a window that may since have closed.
	 * Held by shared_ptr because the render thread needs something it can safely keep hold
	 * of; the pointer inside is set in the constructor and cleared in the destructor, both
	 * on the UI thread, and only ever read on the UI thread.
	 */
	struct PreviewSink {
		DesignerDialog *dialog = nullptr;
	};

	OBSWeakSource weakSource;

	Document document;

	/*
	 * Where a section lives, now that the list is not flat: a sticky block holds sections of its
	 * own and shows them indented under it.
	 *
	 * A path rather than an index because the row a user has selected may be a child, and every
	 * operation on it -- edit, duplicate, delete, move -- has to act on the container it really
	 * belongs to rather than on the top-level list it merely appears in.
	 */
	struct SectionPath {
		/* Index of the sticky block holding it, or -1 when the section is top-level. */
		int parent = -1;
		int index = -1;

		bool isValid() const { return index >= 0; }
	};

	/* How one row of the section list reads: its branch or fold arrow, its name, and its count. */
	QString rowLabel(const SectionPath &path, const Section &section) const;

	/* Folds a sticky block's children away in the list, and back. View state only; see Section. */
	void toggleBlockFold(int row);

	/* The section a path names, or null when the path does not resolve. */
	Section *sectionAt(const SectionPath &path);
	const Section *sectionAt(const SectionPath &path) const;

	/* The path each row of the list stands for, in visual order. */
	QVector<SectionPath> pathsInOrder() const;

	/* The row a path is drawn on, or -1 when it is not on screen. */
	int rowOf(const SectionPath &path) const;

	/* Inserts `section` into the container `path` names, at `path.index`. Returns where it went. */
	SectionPath insertSection(const SectionPath &path, const Section &section);

	/* Takes the section out of its container. */
	void removeSectionAt(const SectionPath &path);

	/* Which section the preview should highlight for a path: a child highlights its own block. */
	int highlightFor(const SectionPath &path) const;

	int currentRow = -1;
	SectionPath currentPath;
	/* Rebuilt by refreshSectionList, so a row can be turned back into a path. */
	QVector<SectionPath> rowPaths;

	/*
	 * Private to this dialog: the source keeps its own cache. Shared ownership because a
	 * render job in flight outlives the window that queued it.
	 */
	std::shared_ptr<LogoCache> logos = std::make_shared<LogoCache>();
	std::shared_ptr<AnimatedLogoCache> animations = std::make_shared<AnimatedLogoCache>();
	std::shared_ptr<PreviewSink> sink = std::make_shared<PreviewSink>();

	QVector<DocumentSnapshot> undoStack;
	QVector<DocumentSnapshot> redoStack;
	bool editBurstOpen = false;

	/*
	 * Preview renders coalesce the same way the source's do: while one is out, further
	 * edits set a flag instead of queueing another job, so a roll slow enough to rasterise
	 * cannot build a backlog of stale frames behind the one the user is waiting for.
	 * UI thread only.
	 */
	bool previewInFlight = false;
	bool previewAgain = false;

	QSplitter *splitter = nullptr;
	SectionListWidget *sectionList = nullptr;
	QWidget *listPane = nullptr;
	QLabel *sectionsLabel = nullptr;
	QWidget *listButtonRow = nullptr;
	QToolButton *collapseButton = nullptr;
	bool sectionsCollapsed = false;
	/* The width to come back to, taken the moment the list is folded away. */
	int listExpandedWidth = 0;

	SectionEditor *editor = nullptr;
	QScrollArea *editorScroll = nullptr;
	PreviewWidget *preview = nullptr;
	QWidget *previewPane = nullptr;
	QLabel *previewLabel = nullptr;
	/* Everything under the preview's own header, folded away as one. */
	QWidget *previewBody = nullptr;
	QToolButton *previewCollapseButton = nullptr;
	bool previewCollapsed = false;
	int previewExpandedWidth = 0;
	/* Switches the layout overlay on; the boxes themselves come with every render. */
	QCheckBox *layoutBoxesCheck = nullptr;
	/* Runs the animated logos in the preview. Disabled when the roll holds none. */
	QCheckBox *animateCheck = nullptr;
	QLabel *durationLabel = nullptr;
	/*
	 * Hidden unless something about the roll will not come out as designed on this machine: a
	 * font that is not installed, artwork longer than an animated logo may be, a video logo in a
	 * build that cannot decode one.
	 */
	QLabel *fontWarningLabel = nullptr;
	QDialogButtonBox *buttons = nullptr;
	QPushButton *undoButton = nullptr;
	QPushButton *redoButton = nullptr;
	/*
	 * Renders are debounced so that typing into a text field does not re-rasterise the
	 * entire strip on every keystroke.
	 */
	QTimer *refreshTimer = nullptr;
	/* Closes an open edit run once the user stops typing. */
	QTimer *editBurstTimer = nullptr;
	/* Notices a library edited from another OBS window, or by hand. */
	QFileSystemWatcher *libraryWatcher = nullptr;
	/* Collects a run of edits to linked presets; see flushLibraryEdits. */
	QTimer *libraryWriteTimer = nullptr;
	QHash<QString, TextStyle> pendingLibraryEdits;
	QHash<QString, BackgroundPanel> pendingLibraryBackgroundEdits;
	/* The library's serial as of the last refresh, so an unchanged reload costs nothing. */
	quint64 librarySerial = 0;
	/*
	 * Answers already given for "edit the library, or fork a copy?", by preset name. Cleared
	 * with the window: the question is about a train of edits, not about a document.
	 */
	QHash<QString, bool> linkedEditChoices;
	/* The same memory for the panels, in a keyspace of its own; see shouldEditLinkedPreset. */
	QHash<QString, bool> linkedBackgroundEditChoices;
	/*
	 * Whether deleting a section still asks first. Per window, like the answers above: undo
	 * already covers a delete, so this is a courtesy rather than the safety net, and a window
	 * opened afresh asks again rather than leaving a setting switched off somewhere unfindable.
	 */
	bool askBeforeRemovingSection = true;
};

} // namespace closingtime
