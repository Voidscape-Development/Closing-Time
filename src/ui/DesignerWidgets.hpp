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

#include <QAbstractButton>
#include <QFrame>
#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QVariant>
#include <QWidget>

#include <functional>

#include "model/CreditsModel.hpp"

class QButtonGroup;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPainter;
class QSpinBox;
class QTimer;
class QToolButton;

namespace closingtime {

/* --- what a section looks like at a glance ------------------------------------------------- */

/*
 * The families the twenty section types fall into, for the purpose of drawing them.
 *
 * Coarser than the type on purpose: a badge is read in the corner of an eye while scanning a list
 * of forty rows, and nine shapes in nine colours is about as many as that kind of glance can tell
 * apart. Which heading, or which kind of list, is what the row's second line is for.
 */
enum class SectionFamily { Heading, LogoHeading, List, Columns, Bridged, Divider, Spacer, Sticky, Action };

SectionFamily sectionFamily(SectionType type);

/* The family's tile colour and the colour its mark is drawn in. Both read on a light or dark theme. */
QColor familyTileColor(SectionFamily family);
QColor familyMarkColor(SectionFamily family);

/* Draws a type's badge -- a rounded tile with the family's mark on it -- into `rect`. */
void paintTypeBadge(QPainter &painter, const QRectF &rect, SectionType type);

/* The same badge as a pixmap, for a QLabel or an icon, at the screen's own pixel ratio. */
QPixmap typeBadgePixmap(SectionType type, int size, qreal devicePixelRatio);

/*
 * A small sketch of what a type puts on the roll -- a wide bar over a thin one for a title, rows
 * joined by dots for a bridged list -- for the add-section gallery, where a type is chosen by what
 * it looks like before what it is called.
 */
void paintTypeThumbnail(QPainter &painter, const QRectF &rect, SectionType type);

/*
 * The second line of a section's row in the list: what kind of thing it is and how much of it
 * there is -- "Bridged · 6 rows", "3 columns · 12 entries", "Holds 5 s".
 */
QString describeSection(const Section &section);

/* The amber dot that marks a tab whose settings differ from the type's defaults. */
QIcon changedDotIcon();

/* A small circled "i" for help that lives in a tooltip. */
QPixmap infoPixmap(int size, const QColor &color, qreal devicePixelRatio);

/* Undo and redo arrows, drawn rather than taken from a theme that may not carry them. */
QIcon undoIcon(bool redo, const QColor &color);

/* An open eye, or one struck through, for the list's show and hide control. */
void paintEye(QPainter &painter, const QRectF &rect, bool open, const QColor &color);

/* Fades a label's text to the secondary weight used for hints and second lines. */
void styleAsHint(QWidget *widget);

/* --- remembered view state ------------------------------------------------------------------- */

/*
 * Small per-machine memories of how somebody likes the designer laid out: which settings groups
 * they keep folded. Kept in the plugin's own config folder, apart from any roll, because none of
 * it is about a roll.
 */
QVariant designerUiValue(const QString &key, const QVariant &fallback = QVariant());
void setDesignerUiValue(const QString &key, const QVariant &value);

/* --- controls ------------------------------------------------------------------------------- */

/*
 * An on/off switch. A checkbox says the same thing, but a switch beside a section's name reads as
 * a property of the section ("in the roll, or not") rather than as one more option in a form.
 */
class ToggleSwitch : public QAbstractButton {
	Q_OBJECT

public:
	explicit ToggleSwitch(QWidget *parent = nullptr);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	bool hovered = false;
};

/* A rounded, checkable pill: "✓ Subtitle", "+ Logo". */
QToolButton *makeChip(const QString &text, const QString &tooltip, QWidget *parent);

/*
 * A row of exclusive buttons standing in for a combo box with a handful of options.
 *
 * Bound to a combo rather than replacing it: the combo stays the one place the value is read from
 * and written to -- with its signals, which the rest of the editor already listens to -- and this is
 * only a quicker way to see and set it when there are three choices rather than thirty.
 */
class SegmentedControl : public QFrame {
	Q_OBJECT

public:
	explicit SegmentedControl(QWidget *parent = nullptr);

	/* Builds one segment per item in `combo` and keeps the two in step from here on. */
	void bindTo(QComboBox *combo);

	/* For a control with no combo behind it. */
	int addSegment(const QString &text, const QString &tooltip = QString());
	void setCurrentIndex(int index);
	int currentIndex() const;

signals:
	void currentIndexChanged(int index);

private:
	QHBoxLayout *row = nullptr;
	QButtonGroup *group = nullptr;
	QPointer<QComboBox> bound;
};

/*
 * The message that stands in for "are you sure?" -- "Removed 'Cast list' · Undo" -- floating at the
 * bottom of the window it belongs to and gone after a few seconds.
 *
 * Undo already covers every structural edit, so asking first only ever costs a click. What a
 * misclick needs is to be told what happened and handed the way back, at the moment and in the
 * place it happened.
 */
class UndoToast : public QFrame {
	Q_OBJECT

public:
	explicit UndoToast(QWidget *window);

	/* Shows `message`; the Undo button runs `undo` once and the toast goes. */
	void showMessage(const QString &message, std::function<void()> undo);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void place();

	QLabel *label = nullptr;
	QToolButton *undoButton = nullptr;
	QTimer *hideTimer = nullptr;
	std::function<void()> undoAction;
};

/*
 * The spacing around a section, drawn as the box it describes: the four numbers sit on the edges
 * they move, around a box standing for the content, so "padding below" is found below rather than
 * as the second of six rows of spin boxes.
 *
 * The spin boxes are made and owned here and handed out, so the editor reads and writes them
 * exactly as it did when they were form rows.
 */
class SpacingDiagram : public QWidget {
	Q_OBJECT

public:
	explicit SpacingDiagram(QWidget *parent = nullptr);

	QSpinBox *above() const { return aboveBox; }
	QSpinBox *below() const { return belowBox; }
	QSpinBox *sides() const { return sidesBox; }
	QSpinBox *offset() const { return offsetBox; }

	/* Which edges apply to the section being edited. A marker has none; a sticky block no sides. */
	void setEdgesVisible(bool vertical, bool sides, bool offset);
	/* What the inner box says: the section's width, for one. */
	void setContentText(const QString &text);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void place();
	QRect contentRect() const;

	QSpinBox *aboveBox = nullptr;
	QSpinBox *belowBox = nullptr;
	QSpinBox *sidesBox = nullptr;
	QSpinBox *offsetBox = nullptr;
	QLabel *aboveLabel = nullptr;
	QLabel *belowLabel = nullptr;
	QLabel *sidesLabel = nullptr;
	QLabel *offsetLabel = nullptr;
	QString contentText;
};

/*
 * The preview's timeline: how far through the roll the preview is, with a tick at every moment an
 * Action fires. Drag or click to go to any point.
 */
class TimelineScrubber : public QWidget {
	Q_OBJECT

public:
	explicit TimelineScrubber(QWidget *parent = nullptr);

	void setDuration(double seconds);
	double duration() const { return length; }
	void setPosition(double seconds);
	double position() const { return at; }
	void setMarkers(const QVector<double> &seconds);

	QSize sizeHint() const override;

signals:
	/* The user dragged or clicked to `seconds`. */
	void seeked(double seconds);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;

private:
	QRectF track() const;
	void seekTo(qreal x);

	double length = 0.0;
	double at = 0.0;
	QVector<double> markers;
};

/* "1:05" -- minutes and seconds, the way a running time is read. */
QString formatClock(double seconds);

/*
 * The designer's own stylesheet: chips, segmented controls, the toolbar's status pills and the
 * editor's tab strip. Built from palette roles rather than fixed colours so it sits inside whichever
 * OBS theme is running.
 */
QString designerStyleSheet();

} // namespace closingtime
