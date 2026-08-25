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

#include "render/DividerArtRenderer.hpp"

#include <QPainter>
#include <QSvgRenderer>
#include <QTransform>

#include <algorithm>

namespace closingtime {

namespace {

/*
 * Ceiling on how many tiles one arm is built from. A thickness of a fraction of a pixel across
 * a wide canvas would otherwise ask for an unbounded run of them.
 */
constexpr int kMaxTiles = 4096;

/* A shape's proportions: the built-in table's, or a custom file's own viewBox. */
qreal shapeAspect(DividerShape shape, QSvgRenderer *renderer)
{
	if (!dividerShapeUsesFile(shape)) {
		const qreal aspect = dividerShapeInfo(shape).aspect;
		return aspect > 0.0 ? aspect : 1.0;
	}

	return svgArtAspect(renderer, 1.0);
}

/*
 * Renders one tile, mirrored along x and turned about its own centre when asked. Qt has no flag
 * for either, so the painter is transformed around the rectangle and the tile drawn into it the
 * usual way -- which means a mirrored piece is the same artwork rather than a second copy of it
 * that has to be kept in step.
 *
 * The flip is applied outside the turn, so a mirrored end is the reflection of the other end
 * whatever angle it was given rather than the same lean drawn twice.
 */
void renderTile(QPainter *painter, QSvgRenderer *renderer, const QRectF &rect, bool mirrored, qreal rotation)
{
	if (!mirrored && rotation == 0.0) {
		renderer->render(painter, rect);
		return;
	}

	painter->save();
	painter->translate(rect.center());
	if (mirrored)
		painter->scale(-1.0, 1.0);
	if (rotation != 0.0)
		painter->rotate(rotation);
	painter->translate(-rect.center());
	renderer->render(painter, rect);
	painter->restore();
}

/*
 * What a placement covers once it is turned: the box its artwork is drawn in, which is the rect
 * itself while the piece stands square and its corners' swept extent once it does not.
 *
 * Only ever asked of the silhouette the tinted pieces are inked through, never of the layout: a
 * turned piece reaches past the room it was given on purpose (see DividerPiece::rotation), and a
 * stencil cut to the untilted rect would take the corners off it.
 */
QRectF placementBounds(const DividerArtPlacement &piece)
{
	if (piece.rotation == 0.0)
		return piece.rect;

	QTransform turn;
	turn.translate(piece.rect.center().x(), piece.rect.center().y());
	turn.rotate(piece.rotation);
	turn.translate(-piece.rect.center().x(), -piece.rect.center().y());

	return turn.mapRect(piece.rect);
}

/* True when this piece is painted through the section's ink rather than in its own colours. */
bool isTinted(const DividerArtPlacement &piece, const Section &section)
{
	/*
	 * The built-in shapes are drawn white precisely so they can be used as a stencil, so they
	 * are always tinted. Only a file the user supplied has colours of its own worth keeping.
	 */
	return !dividerShapeUsesFile(piece.shape) || section.dividerTint;
}

} // namespace

QSvgRenderer *DividerArtCache::get(DividerShape shape, const QString &file)
{
	if (dividerShapeIsEmpty(shape))
		return nullptr;

	if (dividerShapeUsesFile(shape))
		return cache.fromFile(file);

	return cache.builtIn(dividerShapeId(shape), dividerShapeSvg(shape));
}

QSizeF dividerShapeSize(DividerShape shape, const QString &file, DividerArtCache *cache, qreal thickness, qreal scale)
{
	if (!cache || dividerShapeIsEmpty(shape) || thickness <= 0.0 || scale <= 0.0)
		return QSizeF();

	QSvgRenderer *renderer = cache->get(shape, file);
	if (!renderer)
		return QSizeF();

	const qreal proportion = dividerShapeInfo(shape).height;
	const qreal height = thickness * (proportion > 0.0 ? proportion : 1.0) * scale;
	if (height <= 0.0)
		return QSizeF();

	return QSizeF(height * shapeAspect(shape, renderer), height);
}

QVector<QRectF> layoutDividerArm(DividerShape shape, const QString &file, DividerArtCache *cache, const QRectF &span)
{
	QVector<QRectF> tiles;

	if (!cache || dividerShapeIsEmpty(shape) || span.width() <= 0.0 || span.height() <= 0.0)
		return tiles;

	QSvgRenderer *renderer = cache->get(shape, file);
	if (!renderer)
		return tiles;

	/*
	 * An arm *is* the rule, so it is drawn at exactly the divider's thickness whatever height
	 * its table row declares -- that number describes a cap or a centrepiece's proportion to
	 * the rule, and a rule has no proportion to itself.
	 */
	const qreal tileWidth = span.height() * shapeAspect(shape, renderer);
	if (tileWidth <= 0.0)
		return tiles;

	if (dividerShapeInfo(shape).stretch == DividerStretch::Scale) {
		/* A continuous rule has no copies to count, so it covers the span it was given. */
		tiles.append(span);
		return tiles;
	}

	/*
	 * Whole tiles only. A partial one would cut the art mid-shape, which reads as damage
	 * rather than design -- the same reason a repeating bridge refuses a partial copy.
	 */
	const int copies = std::min(static_cast<int>(span.width() / tileWidth), kMaxTiles);
	if (copies < 1)
		return tiles;

	/*
	 * The leftover is opened up between the tiles rather than left at the ends, so the run
	 * meets the cap outside it and the centre inside it. With one tile there is nothing to open
	 * up between, so it is centred instead.
	 */
	const qreal pitch = copies > 1 ? (span.width() - tileWidth) / (copies - 1) : 0.0;
	const qreal start = copies > 1 ? span.left() : span.left() + (span.width() - tileWidth) / 2.0;

	tiles.reserve(copies);
	for (int copy = 0; copy < copies; ++copy)
		tiles.append(QRectF(start + copy * pitch, span.top(), tileWidth, span.height()));

	return tiles;
}

void paintDividerArt(QPainter *painter, const QVector<DividerArtPlacement> &art, const Section &section,
		     const TextStyle &style, DividerArtCache *cache, const QRectF &fillBox)
{
	if (!painter || !cache || art.isEmpty())
		return;

	/*
	 * Untinted custom artwork goes straight to the strip, and takes no part in the silhouette:
	 * it has colours of its own, so there is nothing for a fill to stencil and no reason for an
	 * outline to trace it.
	 */
	for (const DividerArtPlacement &piece : art) {
		if (isTinted(piece, section))
			continue;

		if (QSvgRenderer *renderer = cache->get(piece.shape, piece.file))
			renderTile(painter, renderer, piece.rect, piece.mirrored, piece.rotation);
	}

	QRectF bounds;
	for (const DividerArtPlacement &piece : art) {
		if (isTinted(piece, section)) {
			const QRectF box = placementBounds(piece);
			bounds = bounds.isNull() ? box : bounds.united(box);
		}
	}

	if (bounds.isEmpty())
		return;

	/*
	 * Everything tinted at once, so the outline surrounds the divider rather than each diamond
	 * in it and one gradient runs the whole way across.
	 */
	paintInkedArt(painter, bounds, style, fillBox, [&](QPainter *target) {
		for (const DividerArtPlacement &piece : art) {
			if (!isTinted(piece, section))
				continue;

			if (QSvgRenderer *renderer = cache->get(piece.shape, piece.file))
				renderTile(target, renderer, piece.rect, piece.mirrored, piece.rotation);
		}
	});
}

} // namespace closingtime
