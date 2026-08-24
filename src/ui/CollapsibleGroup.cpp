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

#include <QLayout>
#include <QToolButton>
#include <QVBoxLayout>

namespace closingtime {

CollapsibleGroup::CollapsibleGroup(const QString &title, QWidget *parent) : QWidget(parent), titleText(title)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);

	header = new QToolButton(this);
	header->setCheckable(true);
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
	layout->addWidget(header);

	body = new QWidget(this);
	auto *bodyLayout = new QVBoxLayout(body);
	bodyLayout->setContentsMargins(12, 0, 0, 6);
	layout->addWidget(body);

	connect(header, &QToolButton::toggled, this, [this](bool expanded) {
		body->setVisible(expanded);
		refreshHeader();
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

bool CollapsibleGroup::isExpanded() const
{
	return header->isChecked();
}

void CollapsibleGroup::setExpanded(bool expanded)
{
	header->setChecked(expanded);
}

void CollapsibleGroup::refreshHeader()
{
	header->setText(titleText);
	header->setArrowType(header->isChecked() ? Qt::DownArrow : Qt::RightArrow);
}

} // namespace closingtime
