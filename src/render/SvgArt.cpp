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

#include "render/SvgArt.hpp"

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

/* Largest buffer art is rasterized into, in pixels. Past this it is left undrawn. */
constexpr qint64 kMaxArtPixels = 64 * 1024 * 1024;

/*
 * Draws `ink` at `at`, grown by `grow` pixels every way.
 *
 * There is no path left to stroke once art has been rasterized, so the silhouette is spread
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
		obs_log(LOG_WARNING, "art shadow blur too large to buffer; drawing it hard instead");
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
 * The silhouette recolored with `brush`. The buffer is offset back to where it will be drawn,
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

SvgArtCache::SvgArtCache() = default;
SvgArtCache::~SvgArtCache() = default;

QSvgRenderer *SvgArtCache::load(const QString &key, const QString &path, const QString &markup)
{
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

	const bool fromFile = !path.isEmpty();
	const bool loaded = fromFile ? renderer->load(path) : renderer->load(markup.toUtf8());

	if (!loaded || !renderer->isValid()) {
		if (fromFile)
			obs_log(LOG_WARNING, "could not load artwork '%s'", path.toUtf8().constData());
		else
			obs_log(LOG_WARNING, "built-in artwork '%s' failed to parse", key.toUtf8().constData());
		/* Cached as a failure so a whole section reports it once rather than once a piece. */
		renderer.reset();
	}

	cache.insert(key, renderer);
	return renderer.get();
}

QSvgRenderer *SvgArtCache::builtIn(const char *id, const QString &markup)
{
	if (!id || markup.isEmpty())
		return nullptr;

	return load(QString::fromLatin1(id), QString(), markup);
}

QSvgRenderer *SvgArtCache::fromFile(const QString &path)
{
	if (path.isEmpty())
		return nullptr;

	return load(QStringLiteral("file|") + path, path, QString());
}

qreal svgArtAspect(QSvgRenderer *renderer, qreal fallback)
{
	if (!renderer)
		return fallback;

	const QRectF viewBox = renderer->viewBoxF();
	if (viewBox.width() > 0.0 && viewBox.height() > 0.0)
		return viewBox.width() / viewBox.height();

	const QSize size = renderer->defaultSize();
	if (size.width() > 0 && size.height() > 0)
		return static_cast<qreal>(size.width()) / size.height();

	return fallback;
}

void paintInkedArt(QPainter *painter, const QRectF &bounds, const TextStyle &style, const QRectF &fillBox,
		   const std::function<void(QPainter *)> &drawSilhouette)
{
	if (!painter || !drawSilhouette || bounds.isEmpty())
		return;

	/* A pixel of slack every way, so an antialiased edge is not clipped by its own buffer. */
	const QRect span = bounds.toAlignedRect().adjusted(-1, -1, 1, 1);
	if (span.isEmpty() || static_cast<qint64>(span.width()) * span.height() > kMaxArtPixels)
		return;

	QImage art(span.size(), QImage::Format_ARGB32_Premultiplied);
	if (art.isNull())
		return;

	art.fill(Qt::transparent);

	QPainter artPainter(&art);
	artPainter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
	artPainter.translate(-span.topLeft());
	drawSilhouette(&artPainter);
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
