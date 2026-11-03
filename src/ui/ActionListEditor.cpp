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

#include "ui/ActionListEditor.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/ToolButtons.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Selects the entry carrying `data`, or puts `text` in an editable box that has no such entry. */
void selectValue(QComboBox *box, const QString &value)
{
	const int index = box->findData(value);
	if (index >= 0) {
		box->setCurrentIndex(index);
		return;
	}

	if (box->isEditable()) {
		box->setCurrentIndex(-1);
		box->setEditText(value);
		return;
	}

	box->setCurrentIndex(box->count() > 0 ? 0 : -1);
}

/* What an editable box holds: its typed text when that is not one of its entries' labels. */
QString valueOf(const QComboBox *box)
{
	if (box->isEditable()) {
		const QString text = box->currentText();
		const int index = box->findText(text);
		if (index >= 0)
			return box->itemData(index).toString();
		return text;
	}
	return box->currentData().toString();
}

QComboBox *makeNameBox(QWidget *parent, bool editable)
{
	auto *box = new QComboBox(parent);
	box->setEditable(editable);
	box->setInsertPolicy(QComboBox::NoInsert);
	box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	box->setMinimumContentsLength(12);
	return box;
}

} // namespace

ActionListEditor::ActionListEditor(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	view = new QListWidget(this);
	view->setMinimumHeight(90);
	view->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(view);

	auto *buttons = new QHBoxLayout();
	QToolButton *addButton = makeGlyphButton(this, QStringLiteral("+"), moduleText("Designer.Action.Add"));
	removeButton = makeGlyphButton(this, QStringLiteral("−"), moduleText("Designer.Action.Remove"));
	upButton = makeArrowButton(this, Qt::UpArrow, moduleText("Designer.MoveUp"));
	downButton = makeArrowButton(this, Qt::DownArrow, moduleText("Designer.MoveDown"));
	for (QToolButton *button : {addButton, removeButton, upButton, downButton})
		buttons->addWidget(button);
	buttons->addStretch();
	layout->addLayout(buttons);

	connect(addButton, &QToolButton::clicked, this, &ActionListEditor::addAction);
	connect(removeButton, &QToolButton::clicked, this, &ActionListEditor::removeAction);
	connect(upButton, &QToolButton::clicked, this, [this] { moveAction(-1); });
	connect(downButton, &QToolButton::clicked, this, [this] { moveAction(1); });

	emptyNote = new QLabel(moduleText("Designer.Action.Empty"), this);
	emptyNote->setWordWrap(true);
	layout->addWidget(emptyNote);

	details = new QWidget(this);
	form = new QFormLayout(details);
	form->setContentsMargins(0, 6, 0, 0);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	layout->addWidget(details);

	type = new QComboBox(details);
	for (EndingActionType kind : allEndingActionTypes()) {
		/* "Do nothing" is what an empty list already says; as a row in one it is a row to delete. */
		if (kind == EndingActionType::None)
			continue;
		const QString key = QStringLiteral("Designer.Action.Type.") + QString::fromLatin1(endingActionId(kind));
		const QString label = moduleText(key.toUtf8().constData());
		type->addItem(label == key ? QString::fromUtf8(endingActionName(kind)) : label, static_cast<int>(kind));
	}
	form->addRow(moduleText("Designer.Action.Type"), type);

	scene = makeNameBox(details, true);
	form->addRow(moduleText("Designer.Action.Scene"), scene);

	itemSource = makeNameBox(details, true);
	form->addRow(moduleText("Designer.Action.ItemSource"), itemSource);

	itemScene = makeNameBox(details, true);
	itemScene->setToolTip(moduleText("Designer.Action.ItemScene.Tip"));
	form->addRow(moduleText("Designer.Action.ItemScene"), itemScene);

	itemMode = new QComboBox(details);
	itemMode->addItem(moduleText("EndingAction.ItemMode.Show"), static_cast<int>(FilterToggleMode::Enable));
	itemMode->addItem(moduleText("EndingAction.ItemMode.Hide"), static_cast<int>(FilterToggleMode::Disable));
	itemMode->addItem(moduleText("EndingAction.FilterMode.Toggle"), static_cast<int>(FilterToggleMode::Toggle));
	form->addRow(moduleText("EndingAction.ItemMode"), itemMode);

	filterSource = makeNameBox(details, true);
	form->addRow(moduleText("EndingAction.FilterSource"), filterSource);

	filterName = makeNameBox(details, true);
	form->addRow(moduleText("EndingAction.Filter"), filterName);

	filterMode = new QComboBox(details);
	filterMode->addItem(moduleText("EndingAction.FilterMode.Enable"), static_cast<int>(FilterToggleMode::Enable));
	filterMode->addItem(moduleText("EndingAction.FilterMode.Disable"), static_cast<int>(FilterToggleMode::Disable));
	filterMode->addItem(moduleText("EndingAction.FilterMode.Toggle"), static_cast<int>(FilterToggleMode::Toggle));
	form->addRow(moduleText("EndingAction.FilterMode"), filterMode);

	hotkey = makeNameBox(details, false);
	form->addRow(moduleText("EndingAction.Hotkey"), hotkey);

	delay = new QDoubleSpinBox(details);
	delay->setRange(0.0, 3600.0);
	delay->setDecimals(2);
	delay->setSingleStep(0.25);
	delay->setSuffix(moduleText("Designer.Seconds"));
	delay->setToolTip(moduleText("Designer.Action.Delay.Tip"));
	form->addRow(moduleText("Designer.Action.Delay"), delay);

	revertNote = new QLabel(moduleText("Designer.Action.Reverts"), details);
	revertNote->setWordWrap(true);
	form->addRow(QString(), revertNote);

	connect(view, &QListWidget::currentRowChanged, this, [this] {
		if (!loading)
			showSelected();
	});

	/*
	 * The filters on offer are the chosen source's own. Connected ahead of the handler that stores
	 * the edit, so the list has been refilled by the time the filter name is read back out of it.
	 */
	connect(filterSource, &QComboBox::currentTextChanged, this, [this] {
		if (!loading)
			refreshFilters(filterName->currentText());
	});

	const auto edited = [this] {
		if (loading)
			return;
		storeSelected();
		applyVisibility();
		const int row = selected();
		if (row >= 0)
			if (QListWidgetItem *item = view->item(row))
				item->setText(itemLabel(list.at(row)));
		emit changed();
	};

	for (QComboBox *box : {type, itemMode, filterMode, hotkey})
		connect(box, &QComboBox::currentIndexChanged, this, edited);
	for (QComboBox *box : {scene, itemScene, itemSource, filterSource, filterName})
		connect(box, &QComboBox::currentTextChanged, this, edited);
	connect(delay, &QDoubleSpinBox::valueChanged, this, edited);

	refreshChoices();
	rebuildList(-1);
}

void ActionListEditor::refreshChoices()
{
	const QSignalBlocker blockScene(scene);
	const QSignalBlocker blockItemScene(itemScene);
	const QSignalBlocker blockItemSource(itemSource);
	const QSignalBlocker blockFilterSource(filterSource);
	const QSignalBlocker blockHotkey(hotkey);

	scene->clear();
	itemScene->clear();
	itemScene->addItem(moduleText("EndingAction.ItemScene.All"), QString());

	if (char **names = obs_frontend_get_scene_names()) {
		for (char **name = names; *name; ++name) {
			const QString text = QString::fromUtf8(*name);
			scene->addItem(text, text);
			itemScene->addItem(text, text);
		}
		bfree(names);
	}

	itemSource->clear();
	filterSource->clear();
	obs_enum_all_sources(
		[](void *param, obs_source_t *source) {
			auto *self = static_cast<ActionListEditor *>(param);
			const char *name = obs_source_get_name(source);
			if (!name || !*name)
				return true;

			const QString text = QString::fromUtf8(name);
			const obs_source_type kind = obs_source_get_type(source);
			/* Anything a scene item can be backed by: inputs, and scenes nested in other scenes. */
			if (kind == OBS_SOURCE_TYPE_INPUT || kind == OBS_SOURCE_TYPE_SCENE)
				self->itemSource->addItem(text, text);
			/* Anything that can carry filters. */
			if (kind != OBS_SOURCE_TYPE_FILTER && kind != OBS_SOURCE_TYPE_TRANSITION)
				self->filterSource->addItem(text, text);
			return true;
		},
		this);

	hotkey->clear();
	hotkey->addItem(QString(), QString());
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id, obs_hotkey_t *entry) {
			auto *box = static_cast<QComboBox *>(param);

			const QString name = QString::fromUtf8(obs_hotkey_get_name(entry));
			QString partner;
			QString label = QString::fromUtf8(obs_hotkey_get_description(entry));

			if (obs_hotkey_get_registerer_type(entry) == OBS_HOTKEY_REGISTERER_SOURCE) {
				auto *weak = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(entry));
				OBSSourceAutoRelease owner = obs_weak_source_get_source(weak);
				if (owner) {
					partner = QString::fromUtf8(obs_source_get_name(owner));
					label = QStringLiteral("%1: %2").arg(partner, label);
				}
			}

			box->addItem(label, EndingActionConfig::encodeHotkey(name, partner));
			return true;
		},
		hotkey);
}

void ActionListEditor::refreshFilters(const QString &keep)
{
	filterName->clear();

	const QString source = valueOf(filterSource);
	if (!source.isEmpty()) {
		OBSSourceAutoRelease target = obs_get_source_by_name(source.toUtf8().constData());
		if (target) {
			obs_source_enum_filters(
				target,
				[](obs_source_t *, obs_source_t *filter, void *param) {
					if (const char *name = obs_source_get_name(filter)) {
						const QString text = QString::fromUtf8(name);
						static_cast<QComboBox *>(param)->addItem(text, text);
					}
				},
				filterName);
		}
	}

	selectValue(filterName, keep);
}

void ActionListEditor::setActions(const QVector<EndingActionConfig> &actions)
{
	if (actions == list)
		return;

	const int keep = selected();
	list = actions;

	/* A section picked again may name a scene or a source made since the last one was shown. */
	refreshChoices();
	rebuildList(keep >= 0 && keep < list.size() ? keep : (list.isEmpty() ? -1 : 0));
}

QString ActionListEditor::itemLabel(const EndingActionConfig &action) const
{
	QString label = action.summary();
	if (action.delay > 0.0)
		label += QStringLiteral("  ·  +%1 s").arg(action.delay, 0, 'g', 4);
	return label;
}

void ActionListEditor::rebuildList(int select)
{
	{
		const QSignalBlocker block(view);
		loading = true;
		view->clear();
		for (const EndingActionConfig &action : list)
			view->addItem(itemLabel(action));
		view->setCurrentRow(select);
		loading = false;
	}
	showSelected();
}

int ActionListEditor::selected() const
{
	const int row = view->currentRow();
	return row >= 0 && row < list.size() ? row : -1;
}

void ActionListEditor::showSelected()
{
	const int row = selected();
	details->setVisible(row >= 0);
	emptyNote->setVisible(list.isEmpty());
	removeButton->setEnabled(row >= 0);
	upButton->setEnabled(row > 0);
	downButton->setEnabled(row >= 0 && row + 1 < list.size());
	if (row < 0)
		return;

	const EndingActionConfig &action = list.at(row);

	loading = true;
	{
		const int index = type->findData(static_cast<int>(action.type));
		type->setCurrentIndex(index >= 0 ? index : 0);
	}
	selectValue(scene, action.sceneName);
	selectValue(itemScene, action.itemSceneName);
	selectValue(itemSource, action.itemSourceName);
	itemMode->setCurrentIndex(std::max(0, itemMode->findData(static_cast<int>(action.itemMode))));
	selectValue(filterSource, action.filterSourceName);
	refreshFilters(action.filterName);
	filterMode->setCurrentIndex(std::max(0, filterMode->findData(static_cast<int>(action.filterMode))));
	{
		const QString encoded = EndingActionConfig::encodeHotkey(action.hotkeyName, action.hotkeyPartner);
		int index = hotkey->findData(encoded);
		if (index < 0 && !encoded.isEmpty()) {
			/* A hotkey OBS does not have right now is kept rather than quietly forgotten. */
			hotkey->addItem(action.hotkeyName, encoded);
			index = hotkey->count() - 1;
		}
		hotkey->setCurrentIndex(std::max(0, index));
	}
	delay->setValue(action.delay);
	loading = false;

	applyVisibility();
}

void ActionListEditor::storeSelected()
{
	const int row = selected();
	if (row < 0)
		return;

	EndingActionConfig &action = list[row];
	action.type = static_cast<EndingActionType>(type->currentData().toInt());
	action.sceneName = valueOf(scene);
	action.itemSceneName = valueOf(itemScene);
	action.itemSourceName = valueOf(itemSource);
	action.itemMode = static_cast<FilterToggleMode>(itemMode->currentData().toInt());
	action.filterSourceName = valueOf(filterSource);
	action.filterName = valueOf(filterName);
	action.filterMode = static_cast<FilterToggleMode>(filterMode->currentData().toInt());
	EndingActionConfig::decodeHotkey(hotkey->currentData().toString(), &action.hotkeyName, &action.hotkeyPartner);
	action.delay = delay->value();
}

void ActionListEditor::applyVisibility()
{
	const auto kind = static_cast<EndingActionType>(type->currentData().toInt());

	const auto show = [this](QWidget *field, bool visible) {
		form->setRowVisible(field, visible);
	};

	show(scene, kind == EndingActionType::SwitchScene);
	show(itemSource, kind == EndingActionType::SetSourceVisible);
	show(itemScene, kind == EndingActionType::SetSourceVisible);
	show(itemMode, kind == EndingActionType::SetSourceVisible);
	show(filterSource, kind == EndingActionType::SetFilterEnabled);
	show(filterName, kind == EndingActionType::SetFilterEnabled);
	show(filterMode, kind == EndingActionType::SetFilterEnabled);
	show(hotkey, kind == EndingActionType::FireHotkey);
	show(revertNote, EndingActionConfig::isRevertible(kind));
}

void ActionListEditor::addAction()
{
	EndingActionConfig action;
	action.type = EndingActionType::SetSourceVisible;
	list.append(action);
	rebuildList(list.size() - 1);
	emit changed();
}

void ActionListEditor::removeAction()
{
	const int row = selected();
	if (row < 0)
		return;

	list.removeAt(row);
	rebuildList(list.isEmpty() ? -1 : std::min(row, static_cast<int>(list.size()) - 1));
	emit changed();
}

void ActionListEditor::moveAction(int delta)
{
	const int row = selected();
	const int target = row + delta;
	if (row < 0 || target < 0 || target >= list.size())
		return;

	list.swapItemsAt(row, target);
	rebuildList(target);
	emit changed();
}

} // namespace closingtime
