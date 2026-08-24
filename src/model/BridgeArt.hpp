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
 * What a bridge is drawn from.
 *
 * `Text` is the original: a string set in the section's font and laid across the gap, which
 * means the leader can only ever be whatever characters the font happens to carry, at whatever
 * weight it draws them. Every other type is vector art -- an SVG tile the renderer lays across
 * the gap instead -- so a leader is a shape with a thickness of its own, drawn the same at any
 * size, and a new one costs a tile rather than a change to the renderer.
 *
 * `Custom` takes its tile from a file the user picks. Every other art type takes it from the
 * table in BridgeArt.cpp, and adding one there is a row plus the markup for its tile.
 *
 * `None` draws nothing at all. It is a real type rather than an absence because a row's two
 * texts held apart by a plain gap is a layout people ask for -- a cast list set as two columns
 * with nothing running between them -- and expressing it as "the bridge is empty" keeps every
 * other setting on the row meaning exactly what it always did. What it costs is one number,
 * `Section::bridgeMinGap`, since an empty bridge has no natural width of its own to keep the
 * two texts off each other with.
 *
 * Persisted by string id (bridgeTypeId), never by ordinal, so the enum may be reordered.
 */
enum class BridgeType { None, Text, Dots, Dashes, Line, DoubleLine, Diamonds, Custom };

/*
 * What a type does when asked to cover a gap wider than one tile.
 *
 *   Spread - whole tiles keep their own size and the space between them opens up. This is what
 *            a leader wants: dots stay round however long the run turns out to be.
 *   Scale  - one tile is stretched across the whole span. This is what a continuous rule wants,
 *            since it has no copies to count and tiling it would leave it short of the gap.
 */
enum class BridgeStretch { Spread, Scale };

/* One row of the built-in bridge table. */
struct BridgeTypeInfo {
	BridgeType type;
	/* Persisted id, and the key the renderer's tile cache is keyed on. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;
	/*
	 * The shapes one tile is made of, drawn in a box one unit tall and `aspect` units wide.
	 * bridgeTypeSvg wraps them in the <svg> element, so the viewBox is derived from `aspect`
	 * rather than written out twice and left to drift. Describing the art in units of its own
	 * height is what lets a single thickness in pixels scale the whole thing, and drawing it
	 * white is what lets the renderer use it as a stencil for the section's own fill.
	 *
	 * Empty for Text, which has no art, and for Custom, whose art is a file.
	 */
	const char *svg;
	/* Tile width as a multiple of its height. */
	double aspect;
	BridgeStretch stretch;
};

const BridgeTypeInfo &bridgeTypeInfo(BridgeType type);

const char *bridgeTypeId(BridgeType type);
BridgeType bridgeTypeFromId(const char *id, BridgeType fallback = BridgeType::Text);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *bridgeTypeName(BridgeType type);

/* Every bridge type in the order the designer's picker should list them. */
const QVector<BridgeType> &allBridgeTypes();

/* True when the type is drawn from vector art rather than from the section's bridge string. */
bool bridgeTypeUsesArt(BridgeType type);

/*
 * True when the type draws nothing, leaving the space between the two texts empty.
 *
 * Worth asking by name rather than comparing against None at each call site: an empty bridge is
 * neither text nor art, so both of the questions above answer "no" for it and neither of them
 * says what it actually is.
 */
bool bridgeTypeIsEmpty(BridgeType type);

/* True when the art comes from a file the user picks rather than from the built-in table. */
bool bridgeTypeUsesFile(BridgeType type);

/* The markup for a built-in type's tile. Empty for Text and for Custom. */
QString bridgeTypeSvg(BridgeType type);

} // namespace closingtime
