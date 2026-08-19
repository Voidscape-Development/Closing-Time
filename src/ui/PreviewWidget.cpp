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

/*
 * The layout overlay's colours, one per kind of box.
 *
 * Chosen to stay apart from each other rather than to be pretty: the point of the overlay is to
 * say at a glance which rectangle is which, over a roll whose own colours are the user's.
 */
QColor layoutBoxColour(LayoutBox::Kind kind)
{
	switch (kind) {
	case LayoutBox::Kind::Section:
		return QColor(90, 180, 255);
	case LayoutBox::Kind::Content:
		return QColor(120, 220, 150);
	case LayoutBox::Kind::Logo:
		return QColor(255, 170, 60);
	case LayoutBox::Kind::Bridge:
		return QColor(235, 225, 110);
	case LayoutBox::Kind::Divider:
		return QColor(180, 140, 255);
	case LayoutBox::Kind::Text:
	default:
		return QColor(255, 120, 200);
	}
}

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

void PreviewWidget::setLayoutBoxes(const LayoutBoxes &boxes)
{
	layoutBoxes = boxes;
	if (layoutBoxesVisible)
		update();
}

void PreviewWidget::setLayoutBoxesVisible(bool visible)
{
	if (visible == layoutBoxesVisible)
		return;

	layoutBoxesVisible = visible;
	update();
}

void PreviewWidget::setHighlightedSection(int index)
{
	if (index == highlightedSection)
		return;

	highlightedSection = index;
	if (layoutBoxesVisible)
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

qreal PreviewWidget::onAirHeight() const
{
	return std::min<qreal>(canvasHeight * scaleFactor(), height());
}

/*
 * The roll stops scrolling once its last pixel has reached the bottom of the framed canvas, not
 * once it has reached the bottom of the pane.
 *
 * The frame is the only part of the pane that says anything about what goes to air; everything
 * below it is content that has not got there yet. Stopping at the pane's own bottom edge left the
 * end of the roll parked in that dimmed run and refused to bring it any further, so the last
 * sections could be looked at but never seen in the frame they are being designed for -- and the
 * taller the pane, the more of the roll that was true of.
 */
int PreviewWidget::maxScroll() const
{
	const int scaledHeight = static_cast<int>(strip.height * scaleFactor());
	return std::max(0, scaledHeight - static_cast<int>(onAirHeight()));
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

	const qreal framedHeight = onAirHeight();
	const QRectF onAir(canvasColumn.left() + 0.5, 0.5, canvasColumn.width() - 1.0, framedHeight - 1.0);

	/* Dimmed rather than hidden: this is still the content being edited, just not on air yet. */
	if (framedHeight < height())
		painter.fillRect(QRectF(canvasColumn.left(), framedHeight, canvasColumn.width(),
					height() - framedHeight),
				 QColor(0, 0, 0, kUpcomingScrimAlpha));

	/* Under the canvas frame, which has to stay readable over whatever the overlay draws. */
	if (layoutBoxesVisible)
		paintLayoutBoxes(painter, canvasColumn, scale);

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

/*
 * Draws the layout's own rectangles over the roll: each section's box, the content area left
 * inside its margins and padding, and every block of text, logo and bridge that was placed.
 *
 * This is a debugging view of the layout rather than a part of the design, so it says as much as
 * it can while getting in the way as little as it can: hairlines rather than filled shapes, a
 * section box dashed so it reads as a bound rather than as something drawn, and everything
 * outside the selected section taken down to a quarter strength so the section being edited is
 * the one that stands out. Boxes are drawn back to front -- the section's own box first, its
 * contents over the top -- so a text column sitting exactly on its section's edge is still
 * visible.
 */
void PreviewWidget::paintLayoutBoxes(QPainter &painter, const QRect &canvasColumn, qreal scale) const
{
	if (layoutBoxes.isEmpty())
		return;

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, false);
	painter.setBrush(Qt::NoBrush);

	const auto draw = [&](LayoutBox::Kind kind) {
		for (const LayoutBox &box : layoutBoxes) {
			if (box.kind != kind)
				continue;

			const QRectF mapped(canvasColumn.left() + box.rect.x() * scale, box.rect.y() * scale - scroll,
					    box.rect.width() * scale, box.rect.height() * scale);
			if (mapped.bottom() < 0 || mapped.top() > height())
				continue;

			const bool dim = highlightedSection >= 0 && box.section != highlightedSection;

			QColor colour = layoutBoxColour(kind);
			colour.setAlpha(dim ? 60 : 230);

			QPen boxPen(colour, 1);
			if (kind == LayoutBox::Kind::Section)
				boxPen.setStyle(Qt::DashLine);
			else if (kind == LayoutBox::Kind::Content)
				boxPen.setStyle(Qt::DotLine);

			painter.setPen(boxPen);
			/* Half-pixel inset so a one-pixel pen lands on the boundary, not either side of it. */
			painter.drawRect(mapped.adjusted(0.5, 0.5, -0.5, -0.5));
		}
	};

	draw(LayoutBox::Kind::Section);
	draw(LayoutBox::Kind::Content);
	draw(LayoutBox::Kind::Text);
	draw(LayoutBox::Kind::Logo);
	draw(LayoutBox::Kind::Bridge);
	draw(LayoutBox::Kind::Divider);

	painter.restore();
}

} // namespace closingtime
