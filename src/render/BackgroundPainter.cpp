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

#include "render/BackgroundPainter.hpp"

#include "render/StripRenderer.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace closingtime {

namespace {

/*
 * The image a panel is filled with, at its own size.
 *
 * Its own size rather than the capped height a logo asks for, because a panel may have to *scale
 * up* to cover its box and cannot do that from artwork already reduced to fit something else.
 */
QImage panelImage(LogoCache *images, const QString &path)
{
	if (!images || path.isEmpty())
		return QImage();

	/*
	 * The cache caps a decode at the height it is handed and leaves anything shorter alone, so
	 * asking for the largest height there is asks for the file as it was authored.
	 */
	return images->get(path, std::numeric_limits<int>::max());
}

/* Draws the image into `rect` according to `fit`. The caller has already clipped to the panel. */
void paintImage(QPainter *painter, const QImage &image, const QRectF &rect, BackgroundImageFit fit)
{
	if (image.isNull() || rect.isEmpty())
		return;

	const qreal imageWidth = image.width();
	const qreal imageHeight = image.height();
	if (imageWidth <= 0.0 || imageHeight <= 0.0)
		return;

	switch (fit) {
	case BackgroundImageFit::Stretch:
		painter->drawImage(rect, image);
		return;

	case BackgroundImageFit::Tile:
		/*
		 * Anchored to the panel's own top-left rather than to the strip's origin, so a texture
		 * starts at the corner of the thing it is behind. Anchoring it to the strip would make a
		 * section's pattern depend on how far down the roll it happened to land, which would
		 * shift under the user every time they added a section above it.
		 */
		painter->drawTiledPixmap(rect, QPixmap::fromImage(image), QPointF(0, 0));
		return;

	case BackgroundImageFit::Cover:
	case BackgroundImageFit::Contain: {
		const qreal scaleX = rect.width() / imageWidth;
		const qreal scaleY = rect.height() / imageHeight;
		const qreal scale = fit == BackgroundImageFit::Cover ? std::max(scaleX, scaleY)
								     : std::min(scaleX, scaleY);

		const QSizeF drawn(imageWidth * scale, imageHeight * scale);
		/* Centred either way: what Cover crops is taken off both edges, and what Contain
		 * leaves over is left on both. */
		const QRectF target(rect.center().x() - drawn.width() / 2.0, rect.center().y() - drawn.height() / 2.0,
				    drawn.width(), drawn.height());
		painter->drawImage(target, image);
		return;
	}
	}
}

} // namespace

QBrush gradientBrush(const GradientSpec &spec, bool radial, const QRectF &box)
{
	QGradientStops stops;
	for (const QPair<qreal, QColor> &stop : spec.resolvedStops())
		stops.append(QGradientStop(stop.first, stop.second));

	if (radial) {
		/* Half the diagonal, so the last stop lands on the corners rather than inside them. */
		const qreal radius = std::hypot(box.width(), box.height()) / 2.0;
		QRadialGradient gradient(box.center(), std::max(radius, 1.0));
		gradient.setStops(stops);
		return QBrush(gradient);
	}

	/* Clockwise from straight down: 0 runs top to bottom, 90 left to right. */
	const qreal radians = qDegreesToRadians(spec.angle);
	const QPointF axis(std::sin(radians), std::cos(radians));
	const QPointF centre = box.center();
	/* The box's own extent along that axis, so the stops span exactly the block. */
	const qreal half = (std::abs(axis.x()) * box.width() + std::abs(axis.y()) * box.height()) / 2.0;

	QLinearGradient gradient(centre - axis * half, centre + axis * half);
	gradient.setStops(stops);
	return QBrush(gradient);
}

QPainterPath backgroundPath(const BackgroundPanel &panel, const QRectF &rect)
{
	QPainterPath path;
	if (rect.isEmpty())
		return path;

	qreal topLeft = std::max(0.0, panel.radiusTopLeft);
	qreal topRight = std::max(0.0, panel.radiusTopRight);
	qreal bottomRight = std::max(0.0, panel.radiusBottomRight);
	qreal bottomLeft = std::max(0.0, panel.radiusBottomLeft);

	if (topLeft <= 0.0 && topRight <= 0.0 && bottomRight <= 0.0 && bottomLeft <= 0.0) {
		path.addRect(rect);
		return path;
	}

	/*
	 * Two radii sharing an edge cannot together be longer than that edge, or the arcs cross and
	 * the shape folds through itself. Every pair is checked and the *whole* figure scaled by the
	 * worst of them, rather than each corner being clipped on its own: scaling keeps the corners
	 * in the proportion they were set in, so a panel with one large corner and three small ones
	 * put behind a short section is still recognisably that panel.
	 */
	qreal scale = 1.0;
	const auto limit = [&scale](qreal a, qreal b, qreal edge) {
		const qreal total = a + b;
		if (total > edge && total > 0.0)
			scale = std::min(scale, edge / total);
	};

	limit(topLeft, topRight, rect.width());
	limit(bottomLeft, bottomRight, rect.width());
	limit(topLeft, bottomLeft, rect.height());
	limit(topRight, bottomRight, rect.height());

	topLeft *= scale;
	topRight *= scale;
	bottomRight *= scale;
	bottomLeft *= scale;

	const qreal left = rect.left();
	const qreal top = rect.top();
	const qreal right = rect.right();
	const qreal bottom = rect.bottom();

	path.moveTo(left + topLeft, top);
	path.lineTo(right - topRight, top);
	if (topRight > 0.0)
		path.arcTo(QRectF(right - topRight * 2.0, top, topRight * 2.0, topRight * 2.0), 90.0, -90.0);

	path.lineTo(right, bottom - bottomRight);
	if (bottomRight > 0.0) {
		path.arcTo(QRectF(right - bottomRight * 2.0, bottom - bottomRight * 2.0, bottomRight * 2.0,
				  bottomRight * 2.0),
			   0.0, -90.0);
	}

	path.lineTo(left + bottomLeft, bottom);
	if (bottomLeft > 0.0) {
		path.arcTo(QRectF(left, bottom - bottomLeft * 2.0, bottomLeft * 2.0, bottomLeft * 2.0), 270.0, -90.0);
	}

	path.lineTo(left, top + topLeft);
	if (topLeft > 0.0)
		path.arcTo(QRectF(left, top, topLeft * 2.0, topLeft * 2.0), 180.0, -90.0);

	path.closeSubpath();
	return path;
}

void paintBackgroundPanel(QPainter *painter, const BackgroundPanel &panel, const QRectF &box, LogoCache *images)
{
	/*
	 * Null on the measure pass, which runs the very same code as the draw. A panel measures
	 * nothing -- it never takes part in layout -- so there is genuinely nothing to do here, and
	 * saying so once means no caller has to guard the call.
	 */
	if (!painter || !panel.isVisible())
		return;

	const QRectF rect = panel.outsetRect(box);
	if (rect.width() <= 0.0 || rect.height() <= 0.0)
		return;

	const QPainterPath path = backgroundPath(panel, rect);

	painter->save();
	/*
	 * Composed with whatever opacity the painter is already carrying rather than replacing it, so
	 * a panel drawn inside something already faded fades with it instead of coming back to full
	 * strength.
	 */
	painter->setOpacity(painter->opacity() * std::clamp(panel.opacity, 0.0, 1.0));
	painter->setPen(Qt::NoPen);

	switch (panel.fill) {
	case BackgroundFill::None:
		break;

	case BackgroundFill::Color:
		if (panel.color.alpha() > 0)
			painter->fillPath(path, panel.color);
		break;

	case BackgroundFill::LinearGradient:
	case BackgroundFill::RadialGradient:
		painter->fillPath(path,
				  gradientBrush(panel.gradient, panel.fill == BackgroundFill::RadialGradient, rect));
		break;

	case BackgroundFill::Image: {
		const QImage image = panelImage(images, panel.imagePath);
		if (image.isNull())
			break;

		/*
		 * Clipped to the panel's own path rather than to its bounding rectangle, which is the
		 * whole of what makes a rounded corner mean anything to an image fill: Cover crops to
		 * the shape, not merely to the box the shape sits in.
		 */
		painter->save();
		painter->setClipPath(path, Qt::IntersectClip);
		paintImage(painter, image, rect, panel.imageFit);
		painter->restore();
		break;
	}
	}

	if (panel.border.enabled && panel.border.width > 0.0 && panel.border.color.alpha() > 0) {
		/*
		 * Stroked along a path inset by half the width, so the stroke's outer edge lands exactly
		 * on the panel's own bounds. Stroking the bounds themselves would put half the width
		 * outside them, which would make a heavier border a wider panel -- and a panel whose size
		 * depends on its border is one whose outset stops describing what it paints.
		 */
		const qreal inset = panel.border.width / 2.0;
		const QRectF inner = rect.adjusted(inset, inset, -inset, -inset);
		if (inner.width() > 0.0 && inner.height() > 0.0) {
			BackgroundPanel shrunk = panel;
			shrunk.radiusTopLeft = std::max(0.0, panel.radiusTopLeft - inset);
			shrunk.radiusTopRight = std::max(0.0, panel.radiusTopRight - inset);
			shrunk.radiusBottomRight = std::max(0.0, panel.radiusBottomRight - inset);
			shrunk.radiusBottomLeft = std::max(0.0, panel.radiusBottomLeft - inset);

			QPen pen(panel.border.color);
			pen.setWidthF(panel.border.width);
			pen.setJoinStyle(Qt::MiterJoin);
			painter->setPen(pen);
			painter->setBrush(Qt::NoBrush);
			painter->drawPath(backgroundPath(shrunk, inner));
		}
	}

	painter->restore();
}

} // namespace closingtime
