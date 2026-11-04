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

#include "ui/CollapsibleGroup.hpp"

#include <QHBoxLayout>
#include <QLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/DesignerWidgets.hpp"

namespace closingtime {

namespace {

/* What holds a group's title clear of the fold arrow beside it; see refreshHeader. */
const QString kTitleIndent = QStringLiteral("  ");

} // namespace

CollapsibleGroup::CollapsibleGroup(const QString &title, QWidget *parent) : QWidget(parent), titleText(title)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);

	/*
	 * A row rather than the button alone, because a checkable group puts a checkbox beside the
	 * title and the two have to read as one heading. The checkbox is added to the front of this
	 * when one is asked for; until then the row holds only the button and looks exactly as it
	 * did before.
	 */
	auto *headerWidget = new QWidget(this);
	headerRow = new QHBoxLayout(headerWidget);
	headerRow->setContentsMargins(0, 0, 0, 0);
	headerRow->setSpacing(0);

	header = new QToolButton(headerWidget);
	header->setCheckable(true);
	/*
	 * Open to start with. A tab that opened as a column of folded headings hid the very fields the
	 * reader came for -- the words of a title sat behind "Content Properties" until somebody
	 * thought to click it. Folding away what one never wants is remembered instead; see
	 * setStateKey.
	 */
	header->setChecked(true);
	header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	header->setArrowType(Qt::DownArrow);
	/*
	 * Flat and full width, so the row reads as a heading over the settings under it rather than
	 * as a button among them -- which is what it is: the fold is a convenience, the name is the
	 * point.
	 */
	header->setAutoRaise(true);
	header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	headerRow->addWidget(header);
	layout->addWidget(headerWidget);

	body = new QWidget(this);
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(12, 0, 0, 6);
	layout->addWidget(body);
	/*
	 * Matched to the header by hand: the button was set unchecked before anything was listening,
	 * so there was no `toggled` to hide the body the way a later fold does.
	 */
	body->setVisible(header->isChecked());

	connect(header, &QToolButton::toggled, this, [this](bool expanded) {
		body->setVisible(expanded);
		refreshHeader();
		if (!stateKey.isEmpty() && !enable)
			setDesignerUiValue(stateKey, expanded);
		emit expandedChanged(expanded);
	});

	refreshHeader();
}

void CollapsibleGroup::addWidget(QWidget *widget)
{
	body->layout()->addWidget(widget);
}

void CollapsibleGroup::addLayout(QLayout *layout)
{
	static_cast<QVBoxLayout *>(body->layout())->addLayout(layout);
}

void CollapsibleGroup::setTitle(const QString &title)
{
	titleText = title;
	refreshHeader();
}

void CollapsibleGroup::setHeaderToolTip(const QString &tip)
{
	header->setToolTip(tip);
	if (enable)
		enable->setToolTip(tip);
}

bool CollapsibleGroup::isExpanded() const
{
	return header->isChecked();
}

void CollapsibleGroup::setExpanded(bool expanded)
{
	header->setChecked(expanded);
}

void CollapsibleGroup::setStateKey(const QString &key)
{
	stateKey = QStringLiteral("fold/") + key;
	if (!enable)
		setExpanded(designerUiValue(stateKey, true).toBool());
}

void CollapsibleGroup::setCheckable(bool checkable)
{
	if (checkable == (enable != nullptr))
		return;

	if (!checkable) {
		delete enable;
		enable = nullptr;
		body->setEnabled(true);
		return;
	}

	enable = new ToggleSwitch(header->parentWidget());
	enable->setChecked(true);
	enable->setToolTip(header->toolTip());
	/*
	 * No text of its own: the title beside it is the group's name and saying it twice would
	 * read as two settings. The checkbox is the whole of the control, and the fold arrow that
	 * follows it belongs to the title.
	 */
	headerRow->insertWidget(0, enable);
	headerRow->insertSpacing(1, 6);

	connect(enable, &QAbstractButton::toggled, this, [this](bool checked) {
		/*
		 * The fold follows the switch, so the row is one decision: on shows the settings that
		 * now apply, off puts away settings that no longer do. They keep their values either
		 * way, and come straight back with the switch.
		 */
		body->setEnabled(checked);
		setExpanded(checked);
		emit toggled(checked);
	});

	/* A switched-off group starts folded, the same as one switched off by hand. */
	setExpanded(enable->isChecked());
}

bool CollapsibleGroup::isChecked() const
{
	return enable ? enable->isChecked() : true;
}

void CollapsibleGroup::setChecked(bool checked)
{
	if (enable)
		enable->setChecked(checked);
}

void CollapsibleGroup::refreshHeader()
{
	/*
	 * A gap of its own between the arrow and the title. A QToolButton sets the two a couple of
	 * pixels apart and offers no way to say otherwise -- neither the icon size, which is the
	 * arrow's own size, nor a stylesheet's padding, which moves the pair together -- so the space
	 * is written into the text, where it is the one thing that does sit between them.
	 */
	header->setText(kTitleIndent + titleText);
	header->setArrowType(header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
}

} // namespace closingtime
