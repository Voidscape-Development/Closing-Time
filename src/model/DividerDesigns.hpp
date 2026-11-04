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

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * Finished dividers to start from.
 *
 * A divider is composed from lanes of lines and pieces, which is what makes it flexible, and is
 * also what makes a blank one slow to start. A design is all of those decisions made at once, so
 * the first thing somebody does with a divider is pick the one nearest what they want and adjust
 * it, rather than build it.
 *
 * A design is *not* a binding. Picking one writes its figure into the section and is forgotten:
 * everything it set is still the section's own and still editable, and nothing here is read at
 * render time -- a divider somebody has worked on must not change because the design it started
 * from was retuned in a later release.
 *
 * What a design sets is the figure: the lanes and everything in them, whether it is symmetric, and
 * the gaps. It leaves alone what is about the section rather than the figure -- the divider's
 * length and alignment, whether a user's files are tinted, and the section's style, padding and
 * panels.
 */
struct DividerDesign {
	/* Stable id, for locale keys and tests. Never persisted: a section does not remember its design. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;
	/* The figure. Its length, alignment and tint are not part of the design and are never read. */
	Divider figure;
};

/* Every built-in design, in the order the designer offers them. */
const QVector<DividerDesign> &dividerDesigns();

/* The design carrying `id`, or null. */
const DividerDesign *findDividerDesign(const char *id);

/* Writes the design's figure into `section`; see DividerDesign for what that does and does not touch. */
void applyDividerDesign(Section &section, const DividerDesign &design);

/*
 * True when the two dividers draw the same figure: the same lanes, the same symmetry, the same
 * gaps. Settings the figure does not consult are not compared -- the line gap where no line meets a
 * piece, the lane gap with one lane, a piece's file when it is a built-in shape -- so a divider is not
 * called changed over a number it cannot show. Length, alignment and tint are not the figure.
 */
bool sameDividerFigure(const Divider &a, const Divider &b);

/*
 * The design whose figure the section currently draws, or null once anything it sets has been
 * changed. This is how the designer's picker shows which design a divider still is.
 */
const DividerDesign *matchingDividerDesign(const Section &section);

} // namespace closingtime
