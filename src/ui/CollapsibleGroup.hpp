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

#include <QWidget>

class QCheckBox;
class QHBoxLayout;
class QLayout;
class QToolButton;
class QVBoxLayout;

namespace closingtime {

/*
 * A named run of settings that folds away to its own title.
 *
 * The section editor is one long column of controls, and which of them apply changes with the type
 * being edited -- so the pane is either a wall of rows or a list of them with no shape to it. A
 * group gives each run a name and a place, and folding one away is how somebody working on the
 * words gets the geometry out of the way without losing where it was.
 *
 * A QGroupBox with `setCheckable` was the obvious thing and is the wrong thing: its checkbox reads
 * as *switching the group off*, which is exactly what the checkable groups already in this editor
 * mean (a secondary style either applies or it does not). A disclosure triangle says "there is more
 * here" and nothing else, which is the whole of what this does.
 *
 * Those two readings are both wanted on the style groups, so a group can carry both: a checkbox
 * saying whether the settings apply and, beside it, the triangle saying whether they are on screen.
 * They are deliberately independent -- folding a group away never switches it off, and switching a
 * group off never hides the settings somebody may be about to come back to. Each has its own
 * signal, `toggled` and `expandedChanged`, so a caller listening for a document change is not woken
 * by somebody tidying the pane.
 *
 * A group starts folded. The fold state is per group and per window -- unfolding one keeps it
 * unfolded as the reader moves between sections -- and nothing about it is written to the document.
 * The checked state is a document setting and belongs to whoever put the checkbox there.
 */
class CollapsibleGroup : public QWidget {
	Q_OBJECT

public:
	explicit CollapsibleGroup(const QString &title, QWidget *parent = nullptr);

	/* Where a caller puts the group's own widgets. */
	QWidget *content() const { return body; }
	void addWidget(QWidget *widget);
	void addLayout(QLayout *layout);

	void setTitle(const QString &title);
	/*
	 * A tooltip for the header row itself. Setting one on the group reaches only the strip of
	 * it no child covers, which on an expanded group is almost nowhere -- and the title is the
	 * part somebody hovers when they want to know what the group is for.
	 */
	void setHeaderToolTip(const QString &tip);

	bool isExpanded() const;
	void setExpanded(bool expanded);

	/*
	 * Whether the group carries a checkbox, and what it says. A group without one is never off,
	 * so `isChecked` answers true for it: "these settings apply" is the truth about a group that
	 * was never given a way to be switched off.
	 */
	void setCheckable(bool checkable);
	bool isCheckable() const { return enable != nullptr; }
	bool isChecked() const;
	void setChecked(bool checked);

signals:
	/* The checkbox only. Folding is `expandedChanged`. */
	void toggled(bool checked);
	void expandedChanged(bool expanded);

private:
	void refreshHeader();

	QHBoxLayout *headerRow = nullptr;
	/* Null until setCheckable(true); see the note on the two readings above. */
	QCheckBox *enable = nullptr;
	QToolButton *header = nullptr;
	QWidget *body = nullptr;
	QString titleText;
};

} // namespace closingtime
