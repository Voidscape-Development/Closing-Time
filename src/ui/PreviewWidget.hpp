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

#include <QColor>
#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include "render/StripRenderer.hpp"

namespace closingtime {

/*
 * Draws the rendered strip at a scale that fits the canvas across the widget, with the canvas
 * itself framed at the top of the pane and the roll below it dimmed as still to come. This is
 * the same Strip the source uploads to the GPU, so what the designer shows is what the roll
 * will look like.
 */
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWidget(QWidget *parent = nullptr);

	void setStrip(const Strip &strip, int canvasWidth, int canvasHeight, const QColor &background);

	/*
	 * The rectangles the layout placed things in, drawn over the roll when the overlay is on.
	 * They are kept whether or not it is showing, so switching it on costs no re-render.
	 */
	void setLayoutBoxes(const LayoutBoxes &boxes);
	void setLayoutBoxesVisible(bool visible);

	/* The section drawn at full strength; every other one is dimmed. -1 highlights none. */
	void setHighlightedSection(int index);

	/*
	 * Runs the animated logos.
	 *
	 * Off by default, and deliberately so: the strip is what the designer is looking at while a
	 * roll is being written, and a pane that will not hold still to be read is a poor place to
	 * type into. With it off every animated logo shows its first frame, which is also the frame
	 * the layout was measured from.
	 *
	 * The roll is not scrolling here, so what plays is each animation on its own clock, honouring
	 * loop, speed and play-once. `startOnEnter` has no meaning without a scroll and is ignored:
	 * an animation held off until its logo enters a frame it will never enter would simply never
	 * be seen.
	 */
	void setAnimationPlaying(bool playing);
	bool isAnimationPlaying() const { return animationPlaying; }
	/* True when there is anything for the play button to run. */
	bool hasAnimatedLogos() const { return !strip.animatedLogos.isEmpty(); }

	/* Scrolls so that `stripY` (in strip pixels) sits at the top of the visible area. */
	void scrollToStripY(int stripY);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	/* Width the canvas is drawn at: the widget's, less the surround kept either side of it. */
	int canvasScreenWidth() const;
	qreal scaleFactor() const;
	/* Height of the framed screenful at the top of the pane, in widget pixels. */
	qreal onAirHeight() const;
	int maxScroll() const;

	void paintLayoutBoxes(QPainter &painter, const QRect &canvasColumn, qreal scale) const;
	/*
	 * Draws the sticky blocks into the slots the strip left for them, so the preview shows the
	 * roll as it is written rather than as it will be pinned.
	 */
	void paintStickyBlocks(QPainter &painter, const QRect &canvasColumn, qreal scale) const;
	/*
	 * Draws the animated logos into the holes the strip left for them. Always called, playing or
	 * not: a hole with nothing drawn into it is a missing logo, not a paused one.
	 */
	void paintAnimatedLogos(QPainter &painter, const QRect &canvasColumn, qreal scale) const;

	Strip strip;
	LayoutBoxes layoutBoxes;
	bool layoutBoxesVisible = false;
	int highlightedSection = -1;
	int canvasWidth = 1920;
	int canvasHeight = 1080;
	QColor background = QColor(0, 0, 0, 0);
	int scroll = 0;

	bool animationPlaying = false;
	/*
	 * One clock for every animation in the pane rather than one each, because they all start
	 * together when the button is pressed. Elapsed time drives the frame lookup, so a repaint
	 * the compositor skipped or a window that was hidden for a moment does not leave the
	 * animations behind where they would have been.
	 */
	QElapsedTimer animationClock;
	QTimer animationTimer;
};

} // namespace closingtime
