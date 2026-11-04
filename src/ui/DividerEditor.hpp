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

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>

#include "model/CreditsModel.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QSpinBox;
class QTabBar;
class QTimer;
class QToolButton;

namespace closingtime {

class ColorButton;

/* The drag payloads the palette, the track and the lane tabs trade in. */
extern const char *const kDividerElementMime;
extern const char *const kDividerMoveMime;

/*
 * The divider as it draws, drawn by the real renderer.
 *
 * What the user is shaping is a picture, and the chips below it are the handles: the preview is what
 * says whether a gap is right or a diamond too big. It is drawn at the roll's own scale and then
 * shrunk to fit, so a hairline looks like a hairline rather than like whatever a smaller canvas
 * would have made of it.
 */
class DividerPreview : public QWidget {
	Q_OBJECT

public:
	explicit DividerPreview(QWidget *parent = nullptr);

	void setImage(const QImage &image);
	QSize sizeHint() const override;

	/* Plays a draw-on across the picture once, so a reveal can be seen without running the roll. */
	void playReveal(DividerReveal mode);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QImage picture;
	DividerReveal revealMode = DividerReveal::None;
	/* 0 to 1 while a reveal plays; 1 when it is not playing. */
	double revealProgress = 1.0;
	QTimer *revealTimer = nullptr;
	qint64 revealStartedMs = 0;
};

/*
 * Everything that can be dragged onto a divider: the lines, the shapes, a word and an image.
 *
 * Laid out as tiles under three headings rather than as lists, because a shape is recognised by its
 * outline long before its name is read. Dragging a tile onto the track puts it where it is dropped;
 * clicking one adds it after whatever is selected. A chip dragged back here is taken off the divider,
 * the same gesture in reverse.
 */
class DividerPalette : public QWidget {
	Q_OBJECT

public:
	explicit DividerPalette(QWidget *parent = nullptr);

	bool hasHeightForWidth() const override { return true; }
	int heightForWidth(int width) const override;
	QSize sizeHint() const override;

signals:
	void activated(const closingtime::DividerElement &element);
	/* A chip from the track was dropped here: take it off. */
	void removeRequested(int index);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void leaveEvent(QEvent *event) override;
	bool event(QEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	struct Tile {
		DividerElement element;
		QString name;
		int group = 0;
		QRect rect;
	};

	/* Places every tile for `width`, and returns the height that took. */
	int layoutTiles(int width) const;
	int tileAt(const QPoint &pos) const;

	mutable QVector<Tile> tiles;
	QStringList groupTitles;
	mutable QVector<QRect> headings;
	int hovered = -1;
	int pressed = -1;
	QPoint pressPos;
	bool removing = false;
};

/*
 * One lane of a divider as a row of chips -- a line, a diamond, a word -- in the order they are
 * drawn, to be clicked, dragged into another order, dragged back out to remove, and, for a line,
 * stretched by its edge to give it more of the room.
 *
 * In a symmetric divider the chips are the left half, and the half they reflect into is drawn after
 * them faded, past a mirror line -- so the whole divider is on screen while only the half that is
 * actually edited can be grabbed. Clicking a reflected chip selects the one it reflects.
 */
class DividerTrack : public QWidget {
	Q_OBJECT

public:
	explicit DividerTrack(QWidget *parent = nullptr);

	void setLane(const QVector<DividerElement> &elements, bool symmetric, bool centerPiece);
	void setSelected(int index);
	int selected() const { return selectedIndex; }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	void selectionChanged(int index);
	/* `to` is the slot it was dropped in, 0..count, counted before the move. */
	void moveRequested(int from, int to);
	void insertRequested(int at, const closingtime::DividerElement &element);
	void removeRequested(int index);
	void duplicateRequested(int index);
	/* A line's edge dragged: its new weight, live. */
	void weightChanged(int index, double weight);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	struct Chip {
		QRect rect;
		/* Into the lane's own list; for a reflected chip, the one it reflects. */
		int source = -1;
		bool ghost = false;
	};

	void layoutChips();
	int chipAt(const QPoint &pos, bool includeGhosts) const;
	bool onHandle(int chip, const QPoint &pos) const;
	/* The slot, 0..count, a drop at `x` would land in. */
	int slotAt(int x) const;

	QVector<DividerElement> elements;
	bool symmetric = true;
	bool centerPiece = true;
	QVector<Chip> chips;
	/* Where the mirror line is drawn, or -1. */
	int axisX = -1;
	int selectedIndex = -1;

	int pressedChip = -1;
	QPoint pressPos;
	bool resizing = false;
	double resizeStartWeight = 1.0;
	int dropSlot = -1;
};

/*
 * The whole of editing a Section Divider: a design to start from, the preview, the palette, a track
 * per lane, and the settings of whatever is selected.
 *
 * It owns a copy of the divider and edits that, emitting `changed` after every edit; the section
 * editor reads it back with `divider()` the way it reads every other field. Everything about the
 * figure lives here, so a divider is worked on in one place.
 */
class DividerEditor : public QWidget {
	Q_OBJECT

public:
	explicit DividerEditor(QWidget *parent = nullptr);

	/* Shows `divider`, keeping the lane and element selected when they still exist. */
	void setDivider(const Divider &divider);
	Divider divider() const { return model; }

	/*
	 * The section the preview draws: the editor's own section, with this divider in it. Asked for
	 * rather than held, so the preview always has the style, width and panels the section has now.
	 */
	void setPreviewSource(std::function<Section()> source);
	/* Redraws the preview shortly, coalescing a run of edits into one render. */
	void refreshPreview();

	/* Selects an element of a lane, for the editor's own use and for tests. */
	void selectElement(int lane, int index);
	int selectedLane() const { return lane; }
	int selectedElement() const { return element; }

signals:
	void changed();

protected:
	/* A preview skipped while hidden is drawn as soon as there is somewhere to show it. */
	void showEvent(QShowEvent *event) override;

private:
	DividerLane &currentLane();
	DividerElement *currentElement();

	/* After any edit: redraws what shows the model and tells the section editor. */
	void edited(bool rebuildTrack = true);
	void rebuildLaneTabs();
	void rebuildTrack();
	void refreshInspector();
	void refreshDesign();
	/* Fills the design picker: the built-in designs, then the ones saved to the library. */
	void rebuildDesignPicker();
	void rebuildDesignMenu();
	void saveDesign();
	void renameSavedDesign(const QString &name);
	void deleteSavedDesign(const QString &name);
	void renderPreview();

	void insertElement(int at, const DividerElement &added);
	void removeElement(int index);
	void duplicateElement(int index);
	void moveElement(int from, int to);

	void addLane();
	void duplicateLane();
	void removeLane();
	void moveLane(int delta);

	void browseForFile();

	Divider model;
	int lane = 0;
	int element = -1;
	/* True while the fields are being written from the model, so their signals are not edits. */
	bool loading = false;

	std::function<Section()> previewSource;
	QTimer *previewTimer = nullptr;

	QComboBox *design = nullptr;
	/* Save, rename and delete the dividers saved to the library. */
	QToolButton *designMenuButton = nullptr;
	QMenu *designMenu = nullptr;
	/* The library's serial when the picker was last filled, so a change made elsewhere shows up. */
	quint64 pickerSerial = ~0ULL;
	QCheckBox *symmetric = nullptr;
	DividerPreview *preview = nullptr;
	DividerPalette *palette = nullptr;
	QTabBar *laneTabs = nullptr;
	QToolButton *removeLaneButton = nullptr;
	QToolButton *laneUpButton = nullptr;
	QToolButton *laneDownButton = nullptr;
	DividerTrack *track = nullptr;

	/* The selected element. */
	QGroupBox *inspector = nullptr;
	QFormLayout *inspectorForm = nullptr;
	QLabel *emptyHint = nullptr;
	QComboBox *pieceBox = nullptr;
	QComboBox *lineBox = nullptr;
	QLineEdit *textEdit = nullptr;
	QWidget *fileRow = nullptr;
	QLineEdit *fileEdit = nullptr;
	QDoubleSpinBox *sizeBox = nullptr;
	QSpinBox *imageHeight = nullptr;
	QDoubleSpinBox *weightBox = nullptr;
	QSpinBox *minLengthBox = nullptr;
	QCheckBox *joinBox = nullptr;
	QDoubleSpinBox *rotationBox = nullptr;
	QSpinBox *offsetBox = nullptr;
	QCheckBox *flipBox = nullptr;
	QWidget *colorRow = nullptr;
	QCheckBox *ownColorBox = nullptr;
	ColorButton *colorButton = nullptr;
	QCheckBox *spanBox = nullptr;
	QCheckBox *optionalBox = nullptr;

	/* The selected lane. */
	QGroupBox *laneGroup = nullptr;
	QFormLayout *laneForm = nullptr;
	QSpinBox *thicknessBox = nullptr;
	QSpinBox *insetBox = nullptr;
	QSpinBox *laneOffsetBox = nullptr;
	QCheckBox *centerPieceBox = nullptr;

	/* The whole divider. */
	QFormLayout *spacingForm = nullptr;
	QSpinBox *pieceGapBox = nullptr;
	QSpinBox *lineGapBox = nullptr;
	QSpinBox *laneGapBox = nullptr;
	QSpinBox *lengthBox = nullptr;
	QComboBox *alignBox = nullptr;
	QCheckBox *tintBox = nullptr;

	/* The draw-on. */
	QFormLayout *revealForm = nullptr;
	QComboBox *revealBox = nullptr;
	QSpinBox *revealStartBox = nullptr;
	QSpinBox *revealLengthBox = nullptr;
	QToolButton *revealPlay = nullptr;
};

/* A divider shape's name, through the locale table. Shared with anything else that lists shapes. */
QString dividerShapeText(DividerShape shape);

/* A shape drawn for a picker or a tile -- once at its own size, or as a line run across the box. */
QIcon dividerShapeIcon(DividerShape shape, bool asLine);

} // namespace closingtime
