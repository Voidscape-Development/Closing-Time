#include "render/BridgeArtRenderer.hpp"

#include <QPainter>
#include <QPointF>
#include <QSvgRenderer>

#include <algorithm>

#include "render/SvgArt.hpp"

namespace closingtime {

namespace {

/*
 * Ceiling on how many tiles one bridge is built from. A thickness of a fraction of a pixel
 * across a wide row would otherwise ask for an unbounded run of them.
 */
constexpr int kMaxTiles = 4096;

/* Tile proportions: the built-in table's, or a custom file's own viewBox. */
qreal tileAspect(const Section &section, QSvgRenderer *renderer)
{
	if (!bridgeTypeUsesFile(section.bridgeType)) {
		const qreal aspect = bridgeTypeInfo(section.bridgeType).aspect;
		return aspect > 0.0 ? aspect : 1.0;
	}

	return svgArtAspect(renderer, 1.0);
}

/* The span the laid-out tiles cover, in painter space. */
QRectF artSpan(const BridgeArtLayout &layout, const QPointF &origin)
{
	qreal left = layout.tiles.first().left();
	qreal right = layout.tiles.first().right();

	for (const QRectF &tile : layout.tiles) {
		left = std::min(left, tile.left());
		right = std::max(right, tile.right());
	}

	return QRectF(origin.x() + left, origin.y(), std::max(0.0, right - left), layout.height);
}

} // namespace

QSvgRenderer *BridgeArtCache::get(const Section &section)
{
	if (!bridgeTypeUsesArt(section.bridgeType))
		return nullptr;

	if (bridgeTypeUsesFile(section.bridgeType))
		return cache.fromFile(section.bridgeSvg);

	return cache.builtIn(bridgeTypeId(section.bridgeType), bridgeTypeSvg(section.bridgeType));
}

qreal bridgeTileWidth(const Section &section, BridgeArtCache *cache)
{
	if (!cache || !bridgeTypeUsesArt(section.bridgeType) || section.bridgeThickness <= 0.0)
		return 0.0;

	QSvgRenderer *renderer = cache->get(section);
	if (!renderer)
		return 0.0;

	return section.bridgeThickness * tileAspect(section, renderer);
}

BridgeArtLayout layoutBridgeArt(const Section &section, BridgeArtCache *cache, qreal span)
{
	BridgeArtLayout layout;

	const qreal tileWidth = bridgeTileWidth(section, cache);
	if (tileWidth <= 0.0 || span <= 0.0)
		return layout;

	/* The art keeps clear of the words it joins, which is space it does not get to fill. */
	const qreal gap = std::max(0.0, section.bridgeGap);
	const qreal width = span - gap * 2.0;
	if (width <= 0.0)
		return layout;

	layout.tile = QSizeF(tileWidth, section.bridgeThickness);
	layout.height = section.bridgeThickness;

	const auto tileAt = [&layout, gap](qreal x, qreal tileSpan) {
		layout.tiles.append(QRectF(gap + x, 0.0, tileSpan, layout.height));
	};

	const bool scales = bridgeTypeInfo(section.bridgeType).stretch == BridgeStretch::Scale;

	if (section.bridgeFill == BridgeFill::Fixed) {
		/* One tile at its own width, centered in the gap, as a fixed text bridge is. */
		tileAt((width - tileWidth) / 2.0, tileWidth);
	} else if (scales) {
		/* A continuous rule has no copies to count, so it covers the span it was given. */
		tileAt(0.0, width);
	} else {
		/*
		 * Whole tiles only. A partial one would cut the art mid-shape, which reads as
		 * damage rather than design -- the same reason a repeating text bridge refuses a
		 * partial copy. What is left over goes either side of the run under Repeat, and
		 * between the tiles under Stretch, so that run meets both ends of the gap.
		 */
		const int copies = std::min(static_cast<int>(width / tileWidth), kMaxTiles);
		const bool spread = section.bridgeFill == BridgeFill::Stretch && copies > 1;

		const qreal pitch = spread ? (width - tileWidth) / (copies - 1) : tileWidth;
		const qreal start = spread ? 0.0 : (width - copies * tileWidth) / 2.0;

		for (int copy = 0; copy < copies; ++copy)
			tileAt(start + copy * pitch, tileWidth);
	}

	/* Nothing placed means nothing drawn, and a row that must not be pushed down for it. */
	if (layout.tiles.isEmpty())
		layout.height = 0.0;

	return layout;
}

void paintBridgeArt(QPainter *painter, const BridgeArtLayout &layout, const Section &section, const TextStyle &style,
		    BridgeArtCache *cache, const QPointF &origin, const QRectF &fillBox)
{
	if (!painter || !cache || layout.isEmpty())
		return;

	QSvgRenderer *renderer = cache->get(section);
	if (!renderer)
		return;

	/*
	 * The built-in tiles are drawn white so they can be used as a stencil for the section's
	 * own fill, which is what keeps a leader in the same color, sweep, outline and shadow as
	 * the text either side of it. Only a file the user supplied has colors of its own worth
	 * keeping, so only there does the flag get a say.
	 */
	if (bridgeTypeUsesFile(section.bridgeType) && !section.bridgeTint) {
		for (const QRectF &tile : layout.tiles)
			renderer->render(painter, tile.translated(origin));
		return;
	}

	/* Every tile at once, so one outline surrounds the run rather than each copy in it. */
	paintInkedArt(painter, artSpan(layout, origin), style, fillBox, [&](QPainter *art) {
		for (const QRectF &tile : layout.tiles)
			renderer->render(art, tile.translated(origin));
	});
}

} // namespace closingtime
