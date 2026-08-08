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

#include "render/BridgeArtRenderer.hpp"

#include <plugin-support.h>

#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QSvgRenderer>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "render/ImageEffects.hpp"
#include "render/StripRenderer.hpp"

namespace closingtime {

namespace {

/*
 * Ceiling on how many tiles one bridge is built from. A thickness of a fraction of a pixel
 * across a wide row would otherwise ask for an unbounded run of them.
 */
constexpr int kMaxTiles = 4096;

/* Largest buffer the art is rasterised into, in pixels. Past this it is left undrawn. */
constexpr qint64 kMaxArtPixels = 64 * 1024 * 1024;

/* Tile proportions: the built-in table's, or a custom file's own viewBox. */
qreal tileAspect(const Section &section, QSvgRenderer *renderer)
{
	if (!bridgeTypeUsesFile(section.bridgeType)) {
		const qreal aspect = bridgeTypeInfo(section.bridgeType).aspect;
		return aspect > 0.0 ? aspect : 1.0;
	}

	const QRectF viewBox = renderer->viewBoxF();
	if (viewBox.width() > 0.0 && viewBox.height() > 0.0)
		return viewBox.width() / viewBox.height();

	const QSize size = renderer->defaultSize();
	if (size.width() > 0 && size.height() > 0)
		return static_cast<qreal>(size.width()) / size.height();

	return 1.0;
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

/*
 * Draws `ink` at `at`, grown by `grow` pixels every way.
 *
 * There is no path left to stroke once a tile has been rasterised, so the silhouette is spread
 * into a ring of offset copies instead. The copies are dense enough to overlap within half a
 * pixel of each other, which at the few pixels an outline is ever set to is indistinguishable
 * from a stroke around the art.
 */
void drawGrown(QPainter *painter, const QImage &ink, const QPointF &at, qreal grow)
{
	if (grow > 0.0) {
		const int steps = std::clamp(qCeil(grow * 8.0), 12, 48);
		for (int step = 0; step < steps; ++step) {
			const qreal angle = qDegreesToRadians(360.0 * step / steps);
			painter->drawImage(at + QPointF(std::cos(angle), std::sin(angle)) * grow, ink);
		}
	}

	painter->drawImage(at, ink);
}

/*
 * The shadow the art casts, softened the same way the text's is. It is cast by the outline as
 * well, since the two are drawn as one silhouette, which is what keeps an outlined leader's
 * shadow the same shape as an outlined word's.
 */
void paintArtShadow(QPainter *painter, const QImage &art, const QPoint &at, const TextStyle &style)
{
	const TextShadow &shadow = style.shadow;
	const qreal grow = style.outline.enabled ? style.outline.width : 0.0;

	const QImage ink = tintedImage(art, shadow.color);
	const QPointF offset(shadow.offsetX, shadow.offsetY);
	const int radius = std::clamp(qRound(shadow.blur / 2.0), 0, 100);

	if (radius < 1) {
		drawGrown(painter, ink, QPointF(at) + offset, grow);
		return;
	}

	/* Three passes of box radius r reach 3r, so that plus the growth is the margin needed. */
	const int margin = radius * 3 + qCeil(grow) + 1;
	const QSize size(ink.width() + margin * 2, ink.height() + margin * 2);

	if (static_cast<qint64>(size.width()) * size.height() > kMaxArtPixels) {
		obs_log(LOG_WARNING, "bridge shadow blur too large to buffer; drawing it hard instead");
		drawGrown(painter, ink, QPointF(at) + offset, grow);
		return;
	}

	QImage buffer(size, QImage::Format_ARGB32_Premultiplied);
	if (buffer.isNull())
		return;

	buffer.fill(Qt::transparent);

	QPainter bufferPainter(&buffer);
	drawGrown(&bufferPainter, ink, QPointF(margin, margin), grow);
	bufferPainter.end();

	blurImage(buffer, radius);

	painter->drawImage(QPointF(at) + offset - QPointF(margin, margin), buffer);
}

/*
 * The silhouette recoloured with `brush`. The buffer is offset back to where it will be drawn,
 * so a gradient lands where the row's text puts it rather than at the image's own origin.
 */
QImage inkedArt(const QImage &art, const QBrush &brush, const QPoint &at)
{
	QImage ink = art;

	QPainter painter(&ink);
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.translate(-at);
	painter.fillRect(QRect(at, art.size()), brush);
	painter.end();

	return ink;
}

} // namespace

BridgeArtCache::BridgeArtCache() = default;
BridgeArtCache::~BridgeArtCache() = default;

QSvgRenderer *BridgeArtCache::get(const Section &section)
{
	if (!bridgeTypeUsesArt(section.bridgeType))
		return nullptr;

	const bool fromFile = bridgeTypeUsesFile(section.bridgeType);
	if (fromFile && section.bridgeSvg.isEmpty())
		return nullptr;

	const QString key = fromFile ? QStringLiteral("custom|") + section.bridgeSvg
				     : QString::fromLatin1(bridgeTypeId(section.bridgeType));

	const auto it = cache.constFind(key);
	if (it != cache.constEnd())
		return it->get();

	auto renderer = std::make_shared<QSvgRenderer>();
	/*
	 * Before the load, because that is where an animated file would otherwise start a timer.
	 * The strip is a still image whatever the artwork thinks it is, and the render thread is
	 * a plain std::thread with no event loop for a timer to run on (see RenderThread.hpp).
	 */
	renderer->setFramesPerSecond(0);

	const bool loaded = fromFile ? renderer->load(section.bridgeSvg)
				     : renderer->load(bridgeTypeSvg(section.bridgeType).toUtf8());

	if (!loaded || !renderer->isValid()) {
		if (fromFile) {
			obs_log(LOG_WARNING, "could not load bridge artwork '%s'",
				section.bridgeSvg.toUtf8().constData());
		} else {
			obs_log(LOG_WARNING, "built-in bridge artwork '%s' failed to parse", key.toUtf8().constData());
		}
		/* Cached as a failure so a whole section of rows reports it once rather than once a row. */
		renderer.reset();
	}

	cache.insert(key, renderer);
	return renderer.get();
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
		/* One tile at its own width, centred in the gap, as a fixed text bridge is. */
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
	 * own fill, which is what keeps a leader in the same colour, sweep, outline and shadow as
	 * the text either side of it. Only a file the user supplied has colours of its own worth
	 * keeping, so only there does the flag get a say.
	 */
	if (bridgeTypeUsesFile(section.bridgeType) && !section.bridgeTint) {
		for (const QRectF &tile : layout.tiles)
			renderer->render(painter, tile.translated(origin));
		return;
	}

	/* A pixel of slack every way, so an antialiased edge is not clipped by its own buffer. */
	const QRect span = artSpan(layout, origin).toAlignedRect().adjusted(-1, -1, 1, 1);
	if (span.isEmpty() || static_cast<qint64>(span.width()) * span.height() > kMaxArtPixels)
		return;

	QImage art(span.size(), QImage::Format_ARGB32_Premultiplied);
	if (art.isNull())
		return;

	art.fill(Qt::transparent);

	QPainter artPainter(&art);
	artPainter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	artPainter.translate(-span.topLeft());
	for (const QRectF &tile : layout.tiles)
		renderer->render(&artPainter, tile.translated(origin));
	artPainter.end();

	painter->save();

	if (style.shadow.enabled)
		paintArtShadow(painter, art, span.topLeft(), style);

	/*
	 * The outline goes under the fill, so the half of it that falls inside the art is covered
	 * back over and the stroke reads as growing outward only -- the same rule the text's does.
	 */
	if (style.outline.enabled && style.outline.width > 0.0)
		drawGrown(painter, tintedImage(art, style.outline.color), QPointF(span.topLeft()), style.outline.width);

	painter->drawImage(span.topLeft(), inkedArt(art, textFillBrush(style, fillBox), span.topLeft()));

	painter->restore();
}

} // namespace closingtime
