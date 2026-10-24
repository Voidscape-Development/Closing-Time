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
#include <QHash>
#include <QImage>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "render/ChunkPipeline.hpp"
#include "render/RollCompositor.hpp"
#include "render/RollRenderer.hpp"

namespace closingtime {

/*
 * Draws the roll at a scale that fits the canvas across the widget, with the canvas itself framed
 * at the top of the pane and the roll below it dimmed as still to come.
 *
 * It draws from the same layout the source plays and through the same compositor -- the decision
 * about what goes where in what order is made once for both -- and asks for pictures only of what
 * is in the pane, rasterized at the pane's own scale rather than at the canvas's and then shrunk.
 * A long roll in a narrow pane is a sixteenth of the pixels it used to be, and only the part on
 * screen is drawn at all.
 */
class PreviewWidget : public QWidget {
	Q_OBJECT

public:
	explicit PreviewWidget(QWidget *parent = nullptr);

	/*
	 * The caches pictures are drawn from: the dialog's own, which the layouts it hands over were
	 * made with. Called once, before the first layout.
	 */
	void setCaches(std::shared_ptr<LogoCache> logos, std::shared_ptr<AnimatedLogoCache> animations);

	/* The roll to show. Its own document says how big the canvas is. */
	void setLayout(RollLayoutPtr layout, const QColor &background);

	/*
	 * The overlay of the rectangles the layout placed things in. They come with every layout, so
	 * switching the overlay on costs nothing.
	 */
	void setLayoutBoxesVisible(bool visible);

	/* The section drawn at full strength; every other one is dimmed. -1 highlights none. */
	void setHighlightedSection(int index);

	/*
	 * Runs the animated logos.
	 *
	 * Off by default, and deliberately so: the roll is what the designer is looking at while it is
	 * being written, and a pane that will not hold still to be read is a poor place to type into.
	 * With it off every animated logo shows its first frame, which is also the frame the layout
	 * was measured from.
	 *
	 * The roll is not scrolling here, so what plays is each animation on its own clock, honoring
	 * loop, speed and play-once. `startOnEnter` has no meaning without a scroll and is ignored.
	 */
	void setAnimationPlaying(bool playing);
	bool isAnimationPlaying() const { return animationPlaying; }
	/* True when there is anything for the play button to run. */
	bool hasAnimatedLogos() const { return layout && !layout->animatedLogos.isEmpty(); }

	/* Scrolls so that `stripY` (in strip pixels) sits at the top of the visible area. */
	void scrollToStripY(int stripY);

	QSize sizeHint() const override;

signals:
	/* A click landed on a section: its index in the document's top-level list. */
	void sectionClicked(int section);

protected:
	void paintEvent(QPaintEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;

private:
	int canvasWidth() const;
	int canvasHeight() const;
	/* Width the canvas is drawn at: the widget's, less the surround kept either side of it. */
	int canvasScreenWidth() const;
	qreal scaleFactor() const;
	/* Device pixels per roll pixel: what pictures are rasterized at. */
	qreal rasterScale() const;
	/* Height of the framed screenful at the top of the pane, in widget pixels. */
	qreal onAirHeight() const;
	int maxScroll() const;

	/* What the pane shows, in device pixels, as draws. */
	DrawList composeView(double clipBottomPixels) const;
	/* Asks for every picture the pane shows, or is about to, that it does not have. */
	void requestPictures(const DrawList &visible, const DrawList &ahead);
	/* Takes the pictures that have come back. */
	void collectPictures();
	/* Keeps the pictures held under a budget, least recently drawn out first. */
	void trimPictures();

	void paintLayoutBoxes(QPainter &painter, const QRect &canvasColumn, qreal scale) const;

	RollLayoutPtr layout;
	QColor background = QColor(0, 0, 0, 0);
	bool layoutBoxesVisible = false;
	int highlightedSection = -1;
	int scroll = 0;

	std::shared_ptr<ChunkPipeline> pipeline;

	/* Pictures that have come back, by chunk key, and the paint each was last drawn in. */
	struct Picture {
		QImage image;
		quint64 lastPainted = 0;
	};
	QHash<quint64, Picture> pictures;
	qint64 pictureBytes = 0;
	quint64 paintCount = 0;

	bool animationPlaying = false;
	/*
	 * One clock for every animation in the pane rather than one each, because they all start
	 * together when the button is pressed. Elapsed time drives the frame lookup, so a repaint the
	 * compositor skipped or a window that was hidden for a moment does not leave the animations
	 * behind where they would have been.
	 */
	QElapsedTimer animationClock;
	QTimer animationTimer;
};

} // namespace closingtime
