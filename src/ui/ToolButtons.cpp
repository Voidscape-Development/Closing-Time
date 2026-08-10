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

#include "ui/ToolButtons.hpp"

#include <algorithm>

namespace closingtime {

namespace {

QToolButton *makeButton(QWidget *parent, const QString &tooltip)
{
	auto *button = new QToolButton(parent);
	button->setAutoRaise(true);
	button->setToolTip(tooltip);
	/* The label a glyph button no longer shows is still what a screen reader should read out. */
	button->setAccessibleName(tooltip);
	return button;
}

/*
 * Squared off from the button's own size hint rather than from a pixel count, so the row keeps
 * its proportions at whatever font size and DPI the running OBS theme works out to.
 */
void squareOff(QToolButton *button)
{
	const QSize hint = button->sizeHint();
	button->setFixedWidth(std::max(hint.width(), hint.height()));
}

} // namespace

QToolButton *makeGlyphButton(QWidget *parent, const QString &glyph, const QString &tooltip)
{
	QToolButton *button = makeButton(parent, tooltip);
	button->setText(glyph);
	squareOff(button);
	return button;
}

QToolButton *makeArrowButton(QWidget *parent, Qt::ArrowType arrow, const QString &tooltip)
{
	QToolButton *button = makeButton(parent, tooltip);
	button->setArrowType(arrow);
	squareOff(button);
	return button;
}

QToolButton *makeLabelledButton(QWidget *parent, const QString &text, const QString &tooltip)
{
	QToolButton *button = makeButton(parent, tooltip.isEmpty() ? text : tooltip);
	button->setText(text);
	button->setToolButtonStyle(Qt::ToolButtonTextOnly);
	return button;
}

} // namespace closingtime
