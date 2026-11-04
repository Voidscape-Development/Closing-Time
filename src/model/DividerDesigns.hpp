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
 * The shape library is parts, and a divider is composed from them -- which is what makes it
 * flexible, and is also what makes a blank one hard to start: an end, an arm, a center stack, two
 * gaps, a join and a rule count are a lot of decisions to make before anything looks like an
 * ornamental rule. A design is all of those decisions made at once, so the first thing somebody
 * does with a divider is pick the one nearest what they want and adjust it, rather than build it.
 *
 * A design is *not* a binding. Picking one writes its fields into the section and is forgotten:
 * every value it set is still the section's own and still editable, and nothing here is read at
 * render time. That is deliberate -- a divider somebody has worked on must not change because the
 * design it started from was retuned in a later release.
 *
 * What a design sets is the divider's figure: its ends, its arm, its center, how they are joined
 * and spaced, how many rules there are and where the center sits. It leaves alone everything that
 * is about the section rather than the figure -- its style and ink, its width and padding, its
 * panels -- and leaves the right-hand end list untouched as mirroring always does, so picking a
 * design never costs an end somebody set up by hand.
 */
struct DividerDesign {
	/* Stable id, for locale keys and tests. Never persisted: a section does not remember its design. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;

	QVector<DividerPiece> ends;
	DividerShape arm = DividerShape::Rule;
	double thickness = 4.0;
	bool connect = false;
	double gap = 12.0;
	double pieceGap = 10.0;
	QVector<DividerPiece> center;
	int rules = 1;
	double ruleGap = 6.0;
	double ruleInset = 0.0;
	double centerPosition = 0.5;
};

/* Every built-in design, in the order the designer offers them. */
const QVector<DividerDesign> &dividerDesigns();

/* The design carrying `id`, or null. */
const DividerDesign *findDividerDesign(const char *id);

/* Writes the design's figure into `section`; see DividerDesign for what that does and does not touch. */
void applyDividerDesign(Section &section, const DividerDesign &design);

/*
 * The design whose figure the section currently draws, or null once anything it sets has been
 * changed. This is how the designer's picker shows which design a divider still is, and falls back
 * to saying it is the section's own the moment it is not.
 */
const DividerDesign *matchingDividerDesign(const Section &section);

} // namespace closingtime
