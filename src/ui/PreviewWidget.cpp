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

void paintChecker(QPainter &painter, const QRect &rect)
{
	painter.fillRect(rect, QColor(48, 48, 48));

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(62, 62, 62));
	for (int y = rect.top() / kCheckerSize; y <= rect.bottom() / kCheckerSize; ++y) {
		for (int x = rect.left() / kCheckerSize; x <= rect.right() / kCheckerSize; ++x) {
			if ((x + y) % 2 == 0)
				continue;
			painter.drawRect(x * kCheckerSize, y * kCheckerSize, kCheckerSize, kCheckerSize);
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

qreal PreviewWidget::scaleFactor() const
{
	return static_cast<qreal>(width()) / static_cast<qreal>(canvasWidth);
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

void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const qreal scale = scaleFactor();

	if (background.alpha() >= 255) {
		painter.fillRect(rect(), background);
	} else {
		/* A checkerboard makes it obvious which parts of the roll are transparent. */
		paintChecker(painter, rect());
		if (background.alpha() > 0)
			painter.fillRect(rect(), background);
	}

	painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);

	for (const StripTile &tile : strip.tiles) {
		const qreal top = tile.top * scale - scroll;
		const qreal tileHeight = tile.image.height() * scale;

		if (top + tileHeight < 0 || top > height())
			continue;

		painter.drawImage(QRectF(0, top, width(), tileHeight), tile.image);
	}

	/*
	 * The canvas outline shows how much of the roll is on screen at once, which is the
	 * single most useful thing to see while sizing type and padding.
	 */
	const qreal canvasScaledHeight = canvasHeight * scale;
	painter.setBrush(Qt::NoBrush);
	QPen pen(QColor(255, 200, 80, 200));
	pen.setStyle(Qt::DashLine);
	pen.setWidth(1);
	painter.setPen(pen);
	painter.drawRect(QRectF(0.5, 0.5, width() - 1.0, std::min<qreal>(canvasScaledHeight, height()) - 1.0));
}

} // namespace closingtime
