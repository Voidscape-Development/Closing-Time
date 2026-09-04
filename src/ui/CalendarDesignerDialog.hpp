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
#include <QImage>
#include <QVector>
#include <QWidget>

#include <memory>

#include "model/CalendarModel.hpp"
#include "render/CalendarRenderer.hpp"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTabWidget;
class QTableWidget;
class QTimeEdit;
class QTimer;
class QToolButton;

namespace closingtime {

class BackgroundEditor;
class StyleEditor;

/*
 * The board, drawn to fit the pane, with the block under the pointer pickable.
 *
 * Click-to-select rather than drag-to-edit: the schedule is edited in the table, and the preview's
 * job is to say which row of it a block belongs to. That is the cheap half of an interactive
 * preview and very nearly all of its value -- finding the row for the block you are looking at is
 * the thing that is genuinely hard to do by eye on a board of eighty events.
 */
class CalendarPreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit CalendarPreviewWidget(QWidget *parent = nullptr);

	/* `hits` are in canvas coordinates, as CalendarBoard reports them. */
	void setBoard(const QImage &image, const QVector<CalendarHit> &hits, int page);
	void setSelectedEvent(int index);

signals:
	/* -1 when the click landed on no block, which is how the table's selection is cleared. */
	void eventPicked(int index);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;

private:
	/* Where the board image sits inside the widget, letterboxed and centered. */
	QRectF boardRect() const;

	QImage board;
	QVector<CalendarHit> hits;
	int page = 0;
	int selected = -1;
};

/*
 * The Calendar Display designer.
 *
 * Its own window rather than a mode of the credit roll's, because what it edits is a different
 * shape: a roll is an ordered list of sections and a board is a schedule crossed with a layout.
 * What the two share -- the preview-on-a-render-thread arrangement, the undo model, the preset
 * plumbing -- is shared by being written the same way against the same pieces rather than by one
 * window growing a second personality.
 *
 * Works on its own copy of the document and writes back to the source on Apply or OK.
 */
class CalendarDesignerDialog : public QDialog {
	Q_OBJECT

public:
	CalendarDesignerDialog(obs_source_t *source, QWidget *parent = nullptr);
	~CalendarDesignerDialog() override;

	/* True once the bound source has been destroyed and the window should go away. */
	bool sourceIsGone() const;

private:
	/* --- building the panes ------------------------------------------------------------ */

	QWidget *buildSchedulePane();
	QWidget *buildStructurePane();
	QWidget *buildBoardPane();
	QWidget *buildLivePane();
	QWidget *buildElementsPane();
	QWidget *buildStylePane();
	QWidget *buildPreviewPane();

	/* --- the document ------------------------------------------------------------------ */

	void loadFromSource();
	void writeToSource();

	/*
	 * Reads every field of every pane into `document`.
	 *
	 * One direction, one function: the panes never write into the document as they are typed into,
	 * they raise `edited()` and this reads the lot. A board has settings that depend on each other
	 * -- an axis mode that decides whether slots mean anything, a layout that decides whether lanes
	 * do -- and a hundred small write-backs is how those get out of step with each other.
	 */
	void commit();
	/* And the other way: fills every field from `document`. */
	void refreshFields();

	void refreshEventTable();
	void refreshEventDetails();
	void refreshStructureTables();
	void refreshElementList();
	void refreshElementDetails();
	/* Rebuilds every combo box that lists days, lanes, categories or slots by name. */
	void refreshReferenceCombos();

	void addEvent();
	void duplicateEvent();
	void removeEvent();
	int selectedEventRow() const;
	CalendarEvent *selectedEvent();

	void addElement();
	void removeElement();
	CalendarElement *selectedElement();

	void applyPreset();
	void importDelimited();
	void importJson();
	void exportJson();

	/* --- preview ----------------------------------------------------------------------- */

	void schedulePreviewRefresh();
	void refreshPreview();
	void applyPreview(const CalendarBoard &board, const CalendarDocument &rendered);

	/* The instant the preview is drawn for: now, or whatever the time box says. */
	QDateTime previewTime() const;

	/* --- undo -------------------------------------------------------------------------- */

	/*
	 * Whole-document snapshots, exactly as the roll's designer takes them and for the same reason:
	 * a document is a handful of kilobytes of implicitly shared containers, and there is no edit
	 * here a command object would meaningfully compress.
	 */
	void beginUndoStep();
	void undo();
	void redo();
	void refreshUndoButtons();

	/* --- state ------------------------------------------------------------------------- */

	/*
	 * How a finished render finds its way back to a window that may since have closed. Held by
	 * shared_ptr because the render thread needs something it can safely keep hold of.
	 */
	struct PreviewSink {
		CalendarDesignerDialog *dialog = nullptr;
	};

	OBSWeakSource weakSource;
	CalendarDocument document;

	QVector<CalendarDocument> undoStack;
	QVector<CalendarDocument> redoStack;
	/* Set while a field is being written from the document, so echoes are not recorded as edits. */
	bool loading = false;

	std::shared_ptr<PreviewSink> sink;
	LogoCache logos;
	QTimer *previewTimer = nullptr;
	/* Set while a render is in flight, so a burst of keystrokes collapses into one. */
	bool renderInFlight = false;
	bool renderAgain = false;

	CalendarPreviewWidget *preview = nullptr;
	QLabel *previewSummary = nullptr;
	QCheckBox *previewAtEnabled = nullptr;
	QDateTimeEdit *previewAt = nullptr;
	QSpinBox *previewPage = nullptr;

	QSplitter *splitter = nullptr;
	QTabWidget *tabs = nullptr;

	QToolButton *undoButton = nullptr;
	QToolButton *redoButton = nullptr;

	/* Schedule pane. */
	QTableWidget *eventTable = nullptr;
	QLineEdit *eventTitle = nullptr;
	QLineEdit *eventSubtitle = nullptr;
	QComboBox *eventDay = nullptr;
	QComboBox *eventLane = nullptr;
	QComboBox *eventCategory = nullptr;
	QTimeEdit *eventStart = nullptr;
	QTimeEdit *eventEnd = nullptr;
	QCheckBox *eventOpenEnded = nullptr;
	QComboBox *eventSlot = nullptr;
	QComboBox *eventEndSlot = nullptr;
	QComboBox *eventStatus = nullptr;
	QCheckBox *eventBand = nullptr;
	QComboBox *eventContinuation = nullptr;
	QLineEdit *eventLocation = nullptr;
	QLineEdit *eventZone = nullptr;
	QLineEdit *eventTags = nullptr;
	QLineEdit *eventLogo = nullptr;
	QPlainTextEdit *eventNotes = nullptr;
	QLabel *overlapWarning = nullptr;

	/* Structure pane. */
	QTableWidget *dayTable = nullptr;
	QTableWidget *laneTable = nullptr;
	QTableWidget *slotTable = nullptr;
	QTableWidget *categoryTable = nullptr;

	/* Board pane. */
	QComboBox *layoutBox = nullptr;
	QComboBox *axisBox = nullptr;
	QComboBox *orientationBox = nullptr;
	QCheckBox *daysAsColumns = nullptr;
	QComboBox *gutterBox = nullptr;
	QCheckBox *gutter24Hour = nullptr;
	QSpinBox *gutterStep = nullptr;
	QDoubleSpinBox *gutterWidth = nullptr;
	QCheckBox *showLaneHeaders = nullptr;
	QDoubleSpinBox *laneHeaderSize = nullptr;
	QCheckBox *showDayHeaders = nullptr;
	QDoubleSpinBox *dayHeaderHeight = nullptr;
	QLineEdit *dayFormat = nullptr;
	QLineEdit *dateFormat = nullptr;
	QDoubleSpinBox *pixelsPerHour = nullptr;
	QDoubleSpinBox *slotSize = nullptr;
	QDoubleSpinBox *laneSize = nullptr;
	QDoubleSpinBox *laneGap = nullptr;
	QDoubleSpinBox *dayGap = nullptr;
	QDoubleSpinBox *marginX = nullptr;
	QDoubleSpinBox *marginY = nullptr;
	QCheckBox *showBlockTimes = nullptr;
	QCheckBox *showBlockSubtitles = nullptr;
	QCheckBox *showBlockTags = nullptr;
	QCheckBox *showBlockLocation = nullptr;
	QCheckBox *showBlockLogos = nullptr;
	QCheckBox *showTimeLines = nullptr;
	QCheckBox *showLaneLines = nullptr;
	QComboBox *overflowBox = nullptr;
	QDoubleSpinBox *pageDwell = nullptr;
	QCheckBox *pageByDay = nullptr;
	QDoubleSpinBox *scrollSpeed = nullptr;
	QComboBox *zoneMode = nullptr;
	QLineEdit *zoneName = nullptr;
	QSpinBox *upNextCount = nullptr;
	QDoubleSpinBox *upNextRowHeight = nullptr;
	QDoubleSpinBox *upNextTimeWidth = nullptr;
	QCheckBox *upNextIncludesPast = nullptr;

	/* Live pane. */
	QCheckBox *nowLine = nullptr;
	QLineEdit *nowLineLabel = nullptr;
	QCheckBox *dimFinished = nullptr;
	QDoubleSpinBox *finishedOpacity = nullptr;
	QCheckBox *highlightCurrent = nullptr;
	QCheckBox *dropFinished = nullptr;
	QSpinBox *dropGrace = nullptr;
	QSpinBox *refreshSeconds = nullptr;

	/* Elements pane. */
	QTableWidget *elementTable = nullptr;
	QComboBox *elementType = nullptr;
	QLineEdit *elementLabel = nullptr;
	QComboBox *elementAnchor = nullptr;
	QDoubleSpinBox *elementX = nullptr;
	QDoubleSpinBox *elementY = nullptr;
	QDoubleSpinBox *elementWidth = nullptr;
	QDoubleSpinBox *elementHeight = nullptr;
	QPlainTextEdit *elementText = nullptr;
	QLineEdit *elementImage = nullptr;
	QLineEdit *elementClockFormat = nullptr;
	QCheckBox *elementClockLocal = nullptr;
	QLineEdit *elementClockLabel = nullptr;
	QComboBox *elementLegendSource = nullptr;
	QSpinBox *elementLegendColumns = nullptr;
	StyleEditor *elementStyle = nullptr;
	BackgroundEditor *elementPanel = nullptr;

	/* Style pane. */
	StyleEditor *titleStyle = nullptr;
	StyleEditor *subtitleStyle = nullptr;
	StyleEditor *metaStyle = nullptr;
	BackgroundEditor *blockPanel = nullptr;
	QComboBox *textFit = nullptr;
	QSpinBox *minPixelSize = nullptr;
	StyleEditor *gutterStyle = nullptr;
	StyleEditor *laneStyle = nullptr;
	StyleEditor *dayStyle = nullptr;
};

/*
 * Opens the calendar designer for `source`, raising the existing window if one is already open for
 * it. Must be called from the UI thread.
 */
void openCalendarDesignerFor(obs_source_t *source);

/* The same from anywhere: a hotkey arrives on the hotkey thread and a window opens on the UI one. */
void openCalendarDesignerForAsync(obs_source_t *source);

/* Adds the Tools menu entry listing every Calendar Display in the scene collection. */
void registerCalendarDesignerToolsMenu(const char *sourceId);

/* Closes any calendar designer bound to `source`. Safe to call for sources with none. */
void closeCalendarDesignerFor(obs_source_t *source);

} // namespace closingtime
