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

#include <QVector>
#include <QWidget>

#include "model/EndingAction.hpp"

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QListWidget;
class QToolButton;

namespace closingtime {

/*
 * The list of actions an Action section runs, and the settings of whichever one is selected.
 *
 * A list on top and a form under it rather than a table: the actions are of different kinds, each
 * reading a different handful of fields, and a table would need a column for every field any kind
 * reads with most of them blank on every row. The form shows only what the selected kind uses, the
 * way the source's own ending action does in its properties.
 *
 * The scene, source, filter and hotkey pickers list what OBS has right now, and the name ones are
 * editable as well, so an action can name something that will only exist by the time the roll runs.
 */
class ActionListEditor : public QWidget {
	Q_OBJECT

public:
	explicit ActionListEditor(QWidget *parent = nullptr);

	void setActions(const QVector<EndingActionConfig> &actions);
	QVector<EndingActionConfig> actions() const { return list; }

signals:
	void changed();

private:
	void refreshChoices();
	void rebuildList(int select);
	void showSelected();
	void storeSelected();
	void applyVisibility();
	void refreshFilters(const QString &keep);
	QString itemLabel(const EndingActionConfig &action) const;

	void addAction();
	void removeAction();
	void moveAction(int delta);

	int selected() const;

	QVector<EndingActionConfig> list;
	bool loading = false;

	QListWidget *view = nullptr;
	QToolButton *removeButton = nullptr;
	QToolButton *upButton = nullptr;
	QToolButton *downButton = nullptr;

	QWidget *details = nullptr;
	QFormLayout *form = nullptr;
	QComboBox *type = nullptr;
	QComboBox *scene = nullptr;
	QComboBox *itemScene = nullptr;
	QComboBox *itemSource = nullptr;
	QComboBox *itemMode = nullptr;
	QComboBox *filterSource = nullptr;
	QComboBox *filterName = nullptr;
	QComboBox *filterMode = nullptr;
	QComboBox *hotkey = nullptr;
	QDoubleSpinBox *delay = nullptr;
	QLabel *revertNote = nullptr;
	QLabel *emptyNote = nullptr;
};

} // namespace closingtime
