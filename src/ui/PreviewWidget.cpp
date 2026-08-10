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

#include "ui/PreviewWidget.hpp"

#include <QPainter>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>

namespace closingtime {

namespace {

/* Checkerboard cell size, in widget pixels, used behind a transparent background. */
constexpr int kCheckerSize = 12;

/*
 * Clear space kept either side of the canvas, in widget pixels.
 *
 * The strip used to be drawn edge to edge, which left the canvas's own left and right edges
 * sitting exactly on the widget's border: the frame marking them had nothing on the far side of
 * it to mark them off against, so what it was outlining stopped being legible. A few pixels of
 * surround is the whole difference between a frame around the canvas and a border around a pane.
 */
constexpr int kCanvasInset = 10;

/* Laid over the part of the roll that has not reached the canvas yet. */
constexpr int kUpcomingScrimAlpha = 64;

void paintChecker(QPainter &painter, const QRect &rect)
{
	painter.fillRect(rect, QColor(48, 48, 48));

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(62, 62, 62));
	for (int y = rect.top() / kCheckerSize; y <= rect.bottom() / kCheckerSize; ++y) {
		for (int x = rect.left() / kCheckerSize; x <= rect.right() / kCheckerSize; ++x) {
			if ((x + y) % 2 == 0)
				continue;
			painter.drawRect(
				QRect(x * kCheckerSize, y * kCheckerSize, kCheckerSize, kCheckerSize).intersected(rect));
		}
	}
}

} // namespace

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(240);
	setFocusPolicy(Qt::WheelFocus);
}

void PreviewWidget::setStrip(const Strip &newStrip, int newCanvasWidth, int newCanvasHeight,
			     const QColor &newBackground)
{
	strip = newStrip;
	canvasWidth = std::max(1, newCanvasWidth);
	canvasHeight = std::max(1, newCanvasHeight);
	background = newBackground;

	scroll = std::min(scroll, maxScroll());
	update();
}

void PreviewWidget::scrollToStripY(int stripY)
{
	scroll = std::clamp(static_cast<int>(stripY * scaleFactor()), 0, maxScroll());
	update();
}

int PreviewWidget::canvasScreenWidth() const
{
	return std::max(1, width() - kCanvasInset * 2);
}

qreal PreviewWidget::scaleFactor() const
{
	return static_cast<qreal>(canvasScreenWidth()) / static_cast<qreal>(canvasWidth);
}

int PreviewWidget::maxScroll() const
{
	const int scaledHeight = static_cast<int>(strip.height * scaleFactor());
	return std::max(0, scaledHeight - height());
}

QSize PreviewWidget::sizeHint() const
{
	return QSize(480, 640);
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	scroll = std::min(scroll, maxScroll());
}

void PreviewWidget::wheelEvent(QWheelEvent *event)
{
	const int delta = event->angleDelta().y();
	if (delta == 0) {
		QWidget::wheelEvent(event);
		return;
	}

	scroll = std::clamp(scroll - delta, 0, maxScroll());
	update();
	event->accept();
}

/*
 * What the preview is showing, and what the dashed frame means:
 *
 * The pane is the whole roll laid out end to end, drawn at whatever scale fits the canvas across
 * it, with the wheel moving through it. The dashed frame is the canvas itself -- one screenful,
 * held at the top of the pane -- so the roll runs up through it exactly the way it will on air.
 * Everything below the frame is content that has not reached the canvas yet, and it is dimmed to
 * say so: the frame marks a real boundary rather than an outline whose relationship to the
 * pixels around it has to be guessed at.
 *
 * The scale is taken from the canvas's width rather than the pane's, which is what puts the
 * canvas's own left and right edges inside the pane instead of on top of its border.
 */
void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const qreal scale = scaleFactor();
	const QRect canvasColumn(kCanvasInset, 0, canvasScreenWidth(), height());

	/* Anything outside the canvas is not part of the roll, so it is left as plain surround. */
	painter.fillRect(rect(), palette().window());

	if (background.alpha() >= 255) {
		painter.fillRect(canvasColumn, background);
	} else {
		/* A checkerboard makes it obvious which parts of the roll are transparent. */
		paintChecker(painter, canvasColumn);
		if (background.alpha() > 0)
			painter.fillRect(canvasColumn, background);
	}

	painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);

	for (const StripTile &tile : strip.tiles) {
		const qreal top = tile.top * scale - scroll;
		const qreal tileHeight = tile.image.height() * scale;

		if (top + tileHeight < 0 || top > height())
			continue;

		painter.drawImage(QRectF(canvasColumn.left(), top, canvasColumn.width(), tileHeight), tile.image);
	}

	const qreal onAirHeight = std::min<qreal>(canvasHeight * scale, height());
	const QRectF onAir(canvasColumn.left() + 0.5, 0.5, canvasColumn.width() - 1.0, onAirHeight - 1.0);

	/* Dimmed rather than hidden: this is still the content being edited, just not on air yet. */
	if (onAirHeight < height())
		painter.fillRect(QRectF(canvasColumn.left(), onAirHeight, canvasColumn.width(), height() - onAirHeight),
				 QColor(0, 0, 0, kUpcomingScrimAlpha));

	painter.setBrush(Qt::NoBrush);

	/* The canvas runs the full height of the pane; only the screenful at the top is on air. */
	painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
	painter.drawLine(QPointF(canvasColumn.left() + 0.5, 0), QPointF(canvasColumn.left() + 0.5, height()));
	painter.drawLine(QPointF(canvasColumn.right() + 0.5, 0), QPointF(canvasColumn.right() + 0.5, height()));

	QPen pen(QColor(255, 200, 80, 200));
	pen.setStyle(Qt::DashLine);
	pen.setWidth(1);
	painter.setPen(pen);
	painter.drawRect(onAir);
}

} // namespace closingtime
