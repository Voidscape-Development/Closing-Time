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

#include <QDateTime>
#include <QImage>
#include <QRectF>
#include <QSizeF>
#include <QVector>

#include "model/CalendarModel.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/StripRenderer.hpp"

namespace closingtime {

/*
 * An animated logo the board placed, and everything needed to draw it.
 *
 * The same bargain the credit roll strikes, for the same reason: a board is rasterized once and
 * then held, scaled, paged or scrolled, and an animation is by definition not that. So the artwork
 * is not painted into the page at all -- the space it would have occupied is left empty and it is
 * drawn over the top from a texture of its own.
 *
 * Everything here is in **canvas** coordinates, already carried through the fit scale and the page
 * offset, because the compositor has no way to work those out and no business knowing they exist.
 */
struct CalendarAnimation {
	/* Where it goes on the canvas, at the size it is actually drawn. */
	QRectF rect;
	LogoAnimationPtr animation;
	LogoPlayback playback;

	/*
	 * Which page it belongs to, or -1 for one on the free layer -- an animated bug in a corner
	 * belongs to every page, because the free layer is drawn over all of them.
	 */
	int page = 0;

	/*
	 * True when it travels with the board rather than with the canvas. A scrolling board moves its
	 * blocks under a clock that holds still, and the two kinds of animation have to move with the
	 * thing they were placed against.
	 */
	bool scrolls = true;

	/*
	 * What this placement *is*, as a string two rebuilds can be compared by: the artwork, the page
	 * and the rectangle.
	 *
	 * A board with any clock feature on is re-rasterized every refresh, which without this would
	 * hand the compositor a brand-new set of placements every thirty seconds and restart every
	 * animation on the board. Matching on the key lets playback carry across a rebuild that did not
	 * actually move anything. A logo that *has* moved gets a new key and starts again, which is the
	 * right answer for what is a different placement of it.
	 */
	QString key;
};

/*
 * Where the layout put one event, in canvas coordinates.
 *
 * What it is for is the designer: clicking a block in the preview has to find the row it came from,
 * and an overlay has to be able to outline the selected one. Collected during the layout pass, which
 * visits each event exactly once, so a block cannot be reported at a position other than the one it
 * was drawn at.
 */
struct CalendarHit {
	QRectF rect;
	/* Index into CalendarDocument::events, or -1 for a piece of furniture. */
	int event = -1;
	int page = 0;
};

/* One canvas-worth of the board. A board that fits its canvas has exactly one. */
struct CalendarPage {
	QVector<StripTile> tiles;
	int height = 0;
};

/*
 * A rendered board.
 *
 * `pages` is what the source draws: one page for a board that fits or scrolls, several for one that
 * is paged. A scrolling board's single page is taller than the canvas and is cut into tiles the same
 * way a credit roll's strip is, for the same reason -- a tall schedule can exceed the texture height
 * a GPU will take.
 */
struct CalendarBoard {
	QVector<CalendarPage> pages;

	/*
	 * The free layer, canvas-sized, drawn over whichever page is showing.
	 *
	 * Its own picture rather than painted into the pages, because an element belongs to the canvas
	 * rather than to the board: a clock pinned to a corner must not scale with a fitted board and
	 * must not travel with a scrolling one. Baking it in would do both. Null when the board carries
	 * no elements, which is the ordinary case for a plain grid.
	 */
	QImage overlay;

	/* Canvas size, which is also every page's width. */
	int width = 0;
	int height = 0;

	/* What the board wanted to be before it was fitted, in board pixels. */
	double boardWidth = 0.0;
	double boardHeight = 0.0;
	/* What it was scaled by to land on the canvas. 1.0 when it fitted, or was paged or scrolled. */
	double scale = 1.0;

	/*
	 * True when the board did not fit and the overflow mode had to do something about it. The
	 * designer says so; a board that scrolls or pages is not a problem, but one that hit the
	 * minimum scale and is still too big is about to be drawn with its edge cut off.
	 */
	bool overflowed = false;
	bool clipped = false;

	/*
	 * The animated artwork on this board, in canvas coordinates. Empty for a renderer built without
	 * an animation cache, which paints every logo as a still -- see the constructor.
	 */
	QVector<CalendarAnimation> animations;

	QVector<CalendarHit> hits;
	/* How many events were actually placed, which is not the same as how many the schedule holds. */
	int placedEvents = 0;

	bool isEmpty() const { return pages.isEmpty(); }
};

/*
 * Lays a calendar document out and rasterizes it.
 *
 * **Layout and painting are two passes over one plan.** The layout pass turns the document into a
 * list of placed items -- panels, rules, runs of text, artwork -- in board coordinates; the paint
 * pass walks that list and draws whatever falls inside the piece of the board it is drawing. Where
 * the credit roll's renderer guarantees measure/paint parity by running the same code with a null
 * painter, this guarantees it by painting only what the layout planned: the two cannot disagree
 * because there is only one description of where anything is.
 *
 * That difference is not a preference. A board is drawn at three different scales and offsets --
 * scaled to fit, one page at a time, or scrolling -- and a plan can be painted at each of them from
 * the same measurements, where a measure-and-draw walk would have to be run once per page.
 *
 * Painting is into a QImage, so this runs on the shared render thread exactly as the strip renderer
 * does.
 */
class CalendarRenderer {
public:
	/*
	 * `animations` may be null, and a renderer without one draws every logo as a still: an animated
	 * file contributes its first frame, painted into the page exactly as artwork always was. That is
	 * what the test harness wants, and what the designer's preview wants -- a board being typed into
	 * should hold still -- so animation is something a consumer opts into by being able to honor it
	 * rather than something the renderer assumes of everybody.
	 */
	explicit CalendarRenderer(LogoCache *logos, AnimatedLogoCache *animations = nullptr)
		: logos(logos),
		  animations(animations)
	{
	}

	/* Maximum tile height in pixels, matching the strip renderer's for the same GPU reason. */
	static constexpr int kTileHeight = 2048;

	/*
	 * Rasterizes the board as it stands at `now`.
	 *
	 * `now` is passed in rather than read from the clock so that a board is a pure function of its
	 * document and an instant: the designer can show what the board will look like at eight this
	 * evening, and the test harness can assert what a now-line does without waiting for a minute to
	 * pass.
	 */
	CalendarBoard render(const CalendarDocument &document, const QDateTime &now) const;

	/*
	 * The board's laid-out size, without rasterizing anything. What the designer reports while the
	 * schedule is still being typed, and what tells it that a board has overflowed its canvas.
	 */
	QSizeF measure(const CalendarDocument &document, const QDateTime &now) const;

	/*
	 * Where the layout would put every event, in **board** coordinates, without rasterizing.
	 *
	 * Board coordinates rather than canvas ones, because nothing has been fitted, paged or scrolled
	 * yet: this is what the layout decided, before the overflow mode has had its say. What a click
	 * needs is `CalendarBoard::hits`, which is the same rectangles carried through that.
	 *
	 * For anything asking where the layout put something -- a check, a report of what collided --
	 * rather than what is under the mouse.
	 */
	QVector<CalendarHit> hitBoxes(const CalendarDocument &document, const QDateTime &now) const;

private:
	LogoCache *logos;
	AnimatedLogoCache *animations;
};

} // namespace closingtime
