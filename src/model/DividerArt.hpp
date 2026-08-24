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

#include <QString>
#include <QVector>

namespace closingtime {

/*
 * The shapes a Section Divider is composed from.
 *
 * A divider is not one piece of artwork but three kinds of part -- an end cap, an arm running
 * inward from it, and whatever sits in the middle -- so a handful of tiles here produce far more
 * dividers than there are rows in the table. That is the whole reason this is a separate library
 * from BridgeArt rather than more rows added to it: a bridge is one tile laid across a gap and
 * has no notion of an end or a centre, and the two would only have `svg` and `aspect` in common.
 *
 * Which slots a shape may be picked for is `roles`, not its name: a diamond is a perfectly good
 * end cap and a perfectly good centrepiece, and saying so once is better than a second Diamond
 * in a second table that has to be kept looking like the first. Every shape that is drawn once at
 * its own size now carries the one role that says so, so anything offered at an end is offered in
 * the middle and the other way about.
 *
 * Persisted by string id (dividerShapeId), never by ordinal, so the enum may be reordered.
 */
enum class DividerShape {
	/* No part at all: a divider with no caps, or an unbroken rule with no centre. */
	None,

	/* Ends. */
	Arrow,
	Spear,
	Teardrop,
	ScrollEnd,
	DecoStep,
	Chevron,

	/* Rules. */
	Rule,
	DoubleRule,
	Taper,
	Dots,
	Dashes,
	DiamondChain,
	DecoTicks,

	/* Centrepieces. Diamond, Dot and Sparkle cap an arm just as well as they break one. */
	Diamond,
	NestedDiamond,
	Sparkle,
	Star,
	Dot,
	Heart,
	Scroll,
	Filigree,
	DecoStack,

	/* Artwork from a file the user picks, in any of the three slots. */
	Custom,
};

/*
 * Which slots a shape is offered for. A shape may serve more than one -- see the note on
 * `roles` above.
 *
 * There are two slots rather than three because an end and a middle are the same slot: both are a
 * stack of pieces, so a shape that suits one suits the other, and keeping them apart only meant
 * two lists that had to be argued about shape by shape. An arm is the exception and stays its own
 * role -- it is the rule itself, tiled along a span, which is a different job from being drawn
 * once at its own size.
 */
enum DividerRole : unsigned {
	/* An end cap or a centrepiece: anything drawn once, at its own size, in a stack. */
	DividerRolePiece = 1u << 0,
	DividerRoleArm = 1u << 1,
};

/*
 * What a shape does when asked to cover an arm longer than one tile. The same distinction
 * BridgeStretch draws, and for the same reason.
 *
 *   Spread - whole tiles keep their own size and the space between them opens up, so a run of
 *            dots stays round however long the arm turns out to be.
 *   Scale  - one tile is stretched along the arm, which is what a continuous rule wants: tiling
 *            it would leave it short of the cap by up to a whole tile, and on an unbroken line
 *            that reads as damage rather than as design.
 *
 * Only ever consulted for a shape in the arm slot. A cap and a centrepiece are each drawn once,
 * at their own size, so neither has a span to cover.
 */
enum class DividerStretch { Spread, Scale };

/* One row of the built-in shape table. */
struct DividerShapeInfo {
	DividerShape shape;
	/* Persisted id, and the key the renderer's tile cache is keyed on. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;
	/*
	 * The shapes one tile is made of, drawn in a box `aspect` units wide and one unit tall,
	 * with the divider's own midline running along y = 0.5. dividerShapeSvg wraps them in the
	 * <svg> element, so the viewBox is derived from `aspect` rather than written out twice and
	 * left to drift.
	 *
	 * Drawn white, so the renderer can use the tile as a stencil for the section's own fill --
	 * which is what lets a divider carry the same gold sweep as the title above it instead of
	 * being a separate colour that has to be matched by hand.
	 *
	 * Empty for None, which draws nothing, and for Custom, whose art is a file.
	 */
	const char *svg;
	/* Tile width as a multiple of its own drawn height. */
	double aspect;
	/*
	 * How tall the shape is drawn, as a multiple of the divider's thickness.
	 *
	 * An arm is the rule itself and so is exactly the thickness: 1.0. Everything else is a
	 * proportion of it -- an arrowhead some five times the rule it caps, a centre diamond four
	 * -- and keeping that ratio here rather than in a second set of spin boxes is what lets one
	 * thickness size a whole divider and still have an arrowhead look like an arrowhead.
	 */
	double height;
	DividerStretch stretch;
	unsigned roles;
};

const DividerShapeInfo &dividerShapeInfo(DividerShape shape);

const char *dividerShapeId(DividerShape shape);
DividerShape dividerShapeFromId(const char *id, DividerShape fallback = DividerShape::None);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *dividerShapeName(DividerShape shape);

/*
 * Every shape offered for `role`, in the order the designer's picker should list them. Each
 * role's list is built once and kept, so a picker rebuild costs a copy of a small vector.
 */
const QVector<DividerShape> &dividerShapesForRole(DividerRole role);

/* True when the shape may be picked for that slot. */
bool dividerShapeHasRole(DividerShape shape, DividerRole role);

/* True when the shape draws nothing at all. */
bool dividerShapeIsEmpty(DividerShape shape);

/* True when the art comes from a file the user picks rather than from the built-in table. */
bool dividerShapeUsesFile(DividerShape shape);

/* The markup for a built-in shape's tile. Empty for None and for Custom. */
QString dividerShapeSvg(DividerShape shape);

} // namespace closingtime
