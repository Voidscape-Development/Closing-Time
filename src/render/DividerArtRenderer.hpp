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

#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include "model/CreditsModel.hpp"
#include "render/SvgArt.hpp"

class QPainter;
class QSvgRenderer;

namespace closingtime {

/*
 * The divider's view of an SvgArtCache: it knows how a shape names its tile, and leaves the
 * parsing, caching and failure handling to the shared cache underneath. One cache serves all
 * three slots, so a diamond used as both a cap and a centrepiece is parsed once.
 */
class DividerArtCache {
public:
	/*
	 * The renderer for one shape. Null when the shape draws nothing, when a custom shape has
	 * no file set, or when the file will not parse -- in which case that part simply is not
	 * drawn, the same way a missing logo leaves a placeholder rather than failing the strip.
	 */
	QSvgRenderer *get(DividerShape shape, const QString &file);

private:
	SvgArtCache cache;
};

/* One piece of a divider's artwork, placed in strip space. */
struct DividerArtPlacement {
	QRectF rect;
	DividerShape shape;
	/* Set only when `shape` is Custom. */
	QString file;
	/*
	 * Flipped along x about the rect's own centre. Caps are authored pointing outward along
	 * -x, so the right-hand end of a divider is always its cap drawn mirrored -- which is
	 * what makes a cap a single choice that cannot come out different at the two ends.
	 */
	bool mirrored = false;
	/*
	 * Degrees clockwise about the rect's own centre, from the piece that placed it. Applied
	 * inside the mirror above, so a turned cap and its reflection at the other end lean away
	 * from each other rather than both the same way.
	 *
	 * The rect is the room the untilted shape asked for and does not grow with the angle; see
	 * DividerPiece::rotation for why the layout deliberately stays still.
	 */
	qreal rotation = 0.0;
};

/*
 * The drawn size of a shape in one of the slots that place it whole -- a cap or a centrepiece.
 *
 * The height is the divider's thickness times the proportion the shape's own table row declares
 * times `scale`, and the width follows from the artwork's proportions, so one thickness sizes a
 * whole divider and an arrowhead stays an arrowhead when it is made heavier. Null when the shape
 * draws nothing or its artwork cannot be resolved.
 */
QSizeF dividerShapeSize(DividerShape shape, const QString &file, DividerArtCache *cache, qreal thickness,
			qreal scale = 1.0);

/*
 * The tiles one arm is built from, filling `span` -- a rectangle as tall as the divider's rule.
 *
 * A scaling shape covers the span exactly, because a rule left short of its cap by a fraction of
 * a tile reads as damage rather than as design. A spreading shape lays whole tiles only and
 * shares what is left over out *between* them rather than at the ends, so a run of dots meets
 * both ends of the arm however long the arm turned out to be. That is the one place a divider's
 * arm deliberately parts company with a bridge, which has three fill modes because the user is
 * choosing how a leader sits between two words; an arm has two fixed ends and only one sensible
 * answer.
 */
QVector<QRectF> layoutDividerArm(DividerShape shape, const QString &file, DividerArtCache *cache, const QRectF &span);

/*
 * Paints placed artwork through the section's ink -- the same fill, gradient, outline and shadow
 * the text around it gets.
 *
 * Every tinted piece is rasterised into one silhouette and inked once, so an outline surrounds
 * the divider rather than each part of it and one gradient runs the whole way across instead of
 * restarting at every diamond. `fillBox` is the block that gradient is mapped over; callers pass
 * the divider's whole box rather than any one part's few pixels of height.
 *
 * Custom artwork left untinted is drawn straight to the strip in the colours it was authored
 * with, and so takes no part in that silhouette.
 */
void paintDividerArt(QPainter *painter, const QVector<DividerArtPlacement> &art, const Section &section,
		     const TextStyle &style, DividerArtCache *cache, const QRectF &fillBox);

} // namespace closingtime
