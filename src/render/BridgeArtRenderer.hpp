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
#include <QVector>

#include "model/CreditsModel.hpp"
#include "render/SvgArt.hpp"

class QPainter;
class QPointF;
class QSvgRenderer;

namespace closingtime {

/*
 * The bridge's view of an SvgArtCache: it knows how a Section names its tile, and leaves the
 * parsing, caching and failure handling to the shared cache underneath.
 */
class BridgeArtCache {
public:
	/*
	 * The renderer for this section's bridge art. Null when the section's bridge is text,
	 * when a custom bridge has no file set, or when the file will not parse -- in which case
	 * the bridge simply is not drawn, the same way a missing logo leaves a placeholder rather
	 * than failing the strip.
	 */
	QSvgRenderer *get(const Section &section);

private:
	SvgArtCache cache;
};

/* Where a bridge's tiles land inside the span the row gave it. */
struct BridgeArtLayout {
	/* One tile at the section's thickness, before any spreading or scaling. */
	QSizeF tile;
	/* Tile rectangles, relative to the top-left of the span. */
	QVector<QRectF> tiles;
	/* Height of the art as drawn; 0 when there is nothing to draw. */
	qreal height = 0.0;

	bool isEmpty() const { return tiles.isEmpty(); }
};

/*
 * The natural width of one tile, before the section's end gaps are added to it. Zero when the
 * section's bridge is not art, or when its art cannot be resolved.
 */
qreal bridgeTileWidth(const Section &section, BridgeArtCache *cache);

/*
 * Places the art across a span `span` wide, keeping the section's gap clear at each end. Runs
 * with or without a painter, like the rest of the layout, so what gets measured and what gets
 * painted cannot drift apart.
 */
BridgeArtLayout layoutBridgeArt(const Section &section, BridgeArtCache *cache, qreal span);

/*
 * Paints a laid-out bridge with `origin` at the top-left of its span.
 *
 * Built-in art is painted through the section's own style -- the same fill, outline and shadow
 * the text either side of it gets -- so a leader belongs to the row rather than sitting on it.
 * `fillBox` is the block a gradient is mapped over, and callers pass the row's line box rather
 * than the art's own few pixels of height: a sweep crammed into a run of dots would show none
 * of itself while the words beside it showed all of it.
 */
void paintBridgeArt(QPainter *painter, const BridgeArtLayout &layout, const Section &section, const TextStyle &style,
		    BridgeArtCache *cache, const QPointF &origin, const QRectF &fillBox);

} // namespace closingtime
