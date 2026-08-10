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

#include <QToolButton>

namespace closingtime {

/*
 * The controls that act on a list -- add, remove, move up, move down -- as the OBS UI draws
 * them: a row of small square buttons carrying a glyph, rather than a row of buttons carrying
 * a word each.
 *
 * The shape is worth sharing because the width it saves is what a pane can then be dragged
 * down to. Five labelled buttons under the section list held that pane open at well over
 * 400 px whether or not the user had any use for the space; the same five as glyphs come to
 * less than half of it, and the label each one loses moves to its tooltip.
 */

/* A square button carrying `glyph` -- "+", "−" and the like. */
QToolButton *makeGlyphButton(QWidget *parent, const QString &glyph, const QString &tooltip);

/* A square button carrying one of Qt's own arrows, which follow the running theme. */
QToolButton *makeArrowButton(QWidget *parent, Qt::ArrowType arrow, const QString &tooltip);

/*
 * A button that keeps its label, for actions no glyph says clearly. Still a QToolButton, so a
 * row mixing the two reads as one set of controls.
 */
QToolButton *makeLabelledButton(QWidget *parent, const QString &text, const QString &tooltip = QString());

} // namespace closingtime
