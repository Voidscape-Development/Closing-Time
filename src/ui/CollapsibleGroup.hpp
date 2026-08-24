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
 * The fold state is per group and per window; nothing about it is written to the document.
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

	bool isExpanded() const;
	void setExpanded(bool expanded);

private:
	void refreshHeader();

	QToolButton *header = nullptr;
	QWidget *body = nullptr;
	QString titleText;
};

} // namespace closingtime
