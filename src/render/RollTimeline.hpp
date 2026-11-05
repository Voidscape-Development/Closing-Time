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

#include <QVector>

#include <limits>

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * Where everything in a roll is, as a function of how long the roll has been running.
 *
 * The roll used to be advanced a tick at a time: a phase, a clock that told rolling from coasting
 * from paused from scrubbed, a counter bumped on every return to the start, and a state per sticky
 * block, all stepped forward by whatever the frame interval was and all living in the source, which
 * nothing outside a running OBS could exercise. What a frame showed depended on every frame before
 * it, including -- for a roll parked under manual scroll -- the direction it had been dragged from.
 *
 * Here it is worked out once instead. Building the timeline simulates the blocks event by event --
 * a block reaching its pin, a fade finishing, a hold running out, a block leaving the frame, the
 * roll clearing it, the roll finishing -- and records when each one happens. Asking about a moment
 * is then a comparison against those times. Nothing is carried from one frame to the next, so the
 * source and the designer cannot disagree about a frame, a scrub shows exactly what its position
 * implies, and every rule below is tested directly by the harness.
 *
 * Roll time starts at zero when the roll starts moving -- after any start delay -- and stands still
 * while the roll is paused. It keeps running after the roll has finished, because a block on its way
 * out carries on out: a card frozen half off the top of the frame is the one thing worse than one
 * that never left.
 */

/* What the timeline needs to know about one sticky block. Copied off its placement. */
struct TimelineBlock {
	/* The slot's top edge and height in strip space. */
	double slotTop = 0.0;
	double slotHeight = 0.0;
	/* How far the block's picture reaches past the slot at each end; see StickyBlockPlacement. */
	double margin = 0.0;
	/* Where the slot's top edge sits in canvas space once pinned. */
	double pinnedTop = 0.0;

	StickyEntrance entrance = StickyEntrance::WithRoll;
	double fadeIn = 0.0;
	double hold = 0.0;
	bool holdForever = false;
	StickyRelease release = StickyRelease::EndAtHold;
};

/* What the timeline needs to know about one animated logo. */
struct TimelineLogo {
	/* The logo's top edge in strip space -- or in its block's slot space, when `block` is set. */
	double top = 0.0;
	/* The block it is drawn inside, or -1 for a logo in an ordinary section. */
	int block = -1;
	bool startOnEnter = false;
	double speed = 1.0;
};

/* What the timeline needs to know about one Action section's marker. */
struct TimelineCue {
	/* The marker in strip space -- or in its block's slot space, when `block` is set. */
	double top = 0.0;
	/* The block it travels with, or -1 for a marker in the roll. */
	int block = -1;
	/* The line it fires on, in canvas pixels down from the top edge. */
	double line = 0.0;
};

/* One top-level section's extent in the strip, or a Page Break: what pages are cut from. */
struct TimelineSpan {
	double top = 0.0;
	double height = 0.0;
	bool pageBreak = false;
};

struct TimelineInputs {
	int canvasHeight = 1080;
	int stripHeight = 0;
	int leadOut = 0;
	double scrollSpeed = 90.0;
	bool loop = false;
	QVector<TimelineBlock> blocks;
	QVector<TimelineLogo> logos;
	QVector<TimelineCue> cues;

	PlaybackMode mode = PlaybackMode::Scroll;
	/* Play the roll in this many seconds rather than at `scrollSpeed` or `pageHold`; 0 is off. */
	double fitSeconds = 0.0;

	/* Played as pages. See Document::pageHold and the settings beside it. */
	QVector<TimelineSpan> spans;
	double pageHold = 4.0;
	PageTransition pageTransition = PageTransition::Fade;
	double pageTransitionTime = 0.6;
	/* Where a page that does not fill the frame sits in what the margins leave: 0 top, 1 bottom. */
	double pageAnchor = 0.5;
	int pageMargin = 48;
};

/*
 * Copies the document's playback settings -- speed, looping, the mode, the fit, the page settings --
 * onto timeline inputs built from a layout. They are playback settings: changing one lays nothing
 * out again, so they are taken from the document as it is now rather than as it was laid out.
 */
void applyPlayback(TimelineInputs *inputs, const Document &document);

/* One page of a roll played as pages, as it is drawn at one moment. */
struct PageFrame {
	/* The page's extent in the strip. Everything laid out in it, and only that, is drawn. */
	double top = 0.0;
	double bottom = 0.0;
	/* Where the strip's top edge lands in canvas space for this page, so its content lands where it should. */
	double stripTop = 0.0;
	double alpha = 1.0;
};

/* One sticky block at one moment. */
struct BlockFrame {
	/* False while a block that waits for the roll has not come up yet. */
	bool showing = false;
	/* Where the slot's top edge is in canvas space. Meaningless while not showing. */
	double top = 0.0;
	/* 1 everywhere but part-way through a fade. */
	double alpha = 1.0;
	/* Set once the block has been let go and is climbing off. */
	bool released = false;
	/*
	 * Set while the block stands apart from the roll: pinned, or fading up where it will pin. A block
	 * on its way up with the roll or climbing off after it is part of the roll's movement, and is
	 * drawn the way the roll is -- edge fade and all.
	 */
	bool held = false;
};

/* Everything that moves, at one moment. */
struct RollFrame {
	/* How far the strip has traveled: 0 with its top edge on the bottom of the canvas. */
	double offset = 0.0;
	QVector<BlockFrame> blocks;
	/* Milliseconds into each animated logo's own playback, before its loop or hold is applied. */
	QVector<double> logoElapsedMs;
	/* Set once the roll is over. Never set for a loop. */
	bool finished = false;
	/* Which pass of a looping roll this moment is in; 0 for a roll that does not loop. */
	int pass = 0;

	/*
	 * Set for a roll played as pages, whose frame is the pages on show rather than a strip at an
	 * offset: `offset` is then meaningless, and sticky blocks are drawn in their slots on their page.
	 */
	bool paged = false;
	QVector<PageFrame> pages;
};

/*
 * Which moments a stretch of roll time passed over; see RollTimeline::sweep.
 *
 * Indices into the list of times that was swept. For a loop that went round in the stretch, the
 * moments are split by the wrap: those left in the pass that ended, and those in the pass that began,
 * so that whoever acts on them can do what a new pass calls for in between.
 */
struct CueSweep {
	QVector<int> before;
	bool wrapped = false;
	QVector<int> after;
};

class RollTimeline {
public:
	static constexpr double kNever = std::numeric_limits<double>::infinity();

	RollTimeline() = default;
	explicit RollTimeline(const TimelineInputs &inputs);

	const TimelineInputs &inputs() const { return in; }

	/* Seconds for the strip to travel from entering the bottom of the frame to leaving the top. */
	double travelTime() const { return travel; }

	/*
	 * When the roll is over: the strip clearing the frame with nothing left holding it open, or a
	 * block whose release says that it ends the roll. kNever for a loop, and for a roll held open by
	 * a block that holds forever.
	 */
	double finishTime() const { return finish; }

	/* The frame at roll time `t`. */
	RollFrame evaluate(double t) const;

	bool paged() const { return in.mode == PlaybackMode::Pages; }

	/* The scroll speed really played at: the document's, or the one a fit worked out. */
	double speedUsed() const { return speed; }
	/* How long each page really holds: the document's, or what a fit worked out. */
	double pageHoldUsed() const { return hold; }
	/* False when a fit was asked for and could not be met; the roll then plays as near to it as it can. */
	bool fitMet() const { return fitReached; }

	/* One page's extent and schedule, for the designer and the harness. */
	struct Page {
		double top = 0.0;
		double bottom = 0.0;
		/* Where the page's top edge rests on the frame while it holds. */
		double rest = 0.0;
		double enter = 0.0;
		double holdStart = 0.0;
		double holdEnd = 0.0;
		double exit = 0.0;
		/* Slide only: how far the page travels coming in, and going out. */
		double enterDistance = 0.0;
		double exitDistance = 0.0;
	};

	const QVector<Page> &pages() const { return pageList; }

	/*
	 * The frame for a roll parked at `offset` under manual scroll.
	 *
	 * There is no clock at all, so holds never run out and fades are complete: a block is pinned
	 * where the position puts it, a block that waits for the roll is up if the roll has gone by and
	 * nothing else is in the way, and an animated logo shows the frame the position corresponds to.
	 */
	RollFrame evaluateScrubbed(double offset) const;

	/*
	 * Roll time on a timeline at another scroll speed that puts the strip where it is at `t` on this
	 * one. What keeps a roll in place when its speed is changed under it.
	 */
	double rebase(double t, const RollTimeline &next) const;

	/* A block's schedule, for the harness. kNever for anything that does not happen. */
	struct BlockSchedule {
		/* Reaches its pin and starts its hold. A block that waits for the roll pins when its fade ends. */
		double pin = kNever;
		/* Comes up, for a block that waits for the roll. 0 for one that travels with it. */
		double enter = 0.0;
		/* Its hold runs out. */
		double holdEnd = kNever;
		/* Starts climbing off, which is the hold running out under a release that resumes. */
		double release = kNever;
		/* The last of its picture leaves the top of the frame. */
		double gone = kNever;
	};

	const QVector<BlockSchedule> &schedules() const { return schedule; }

	/*
	 * When each Action section's marker first crosses its line, in roll time -- within a pass, for a
	 * loop. kNever for a marker that never gets there: one below the line when the strip stops, one
	 * on a card that never lets go.
	 *
	 * A marker already past its line when it comes into view crosses it the moment it does, which is
	 * t = 0 for a line at the bottom edge and the moment a card comes up for one that waited.
	 */
	const QVector<double> &cueTimes() const { return cueTime; }

	/*
	 * Which of `times` -- roll times within a pass, such as cueTimes() plus a delay -- the roll passed
	 * going from `from` to `to`: those after `from` and no later than `to`. A negative `from` means
	 * the roll is just starting, so a time of exactly zero counts.
	 *
	 * For a loop, a time past the end of the pass is reached at the wrap, as the pass runs out, so an
	 * action delayed beyond the end of the roll still happens once a pass. kNever is never reached.
	 */
	CueSweep sweep(const QVector<double> &times, double from, double to) const;

	/* The longest a fitted roll is allowed to scroll, in pixels a second. */
	static constexpr double kMaxFitSpeed = 5000.0;
	static constexpr double kMinFitSpeed = 1.0;

private:
	void simulate();
	void fitSpeed();
	void buildPages();
	PageFrame pageAt(int index, double t) const;
	int pageHolding(double stripY) const;
	double stripOffsetAt(double t) const;
	double blockTopAt(int index, double t) const;
	BlockFrame blockAt(int index, double t) const;
	double logoStart(int index) const;
	double cueCrossing(int index) const;

	TimelineInputs in;
	double speed = 0.0;
	double hold = 0.0;
	bool fitReached = true;
	QVector<Page> pageList;
	double travel = kNever;
	double finish = kNever;
	QVector<BlockSchedule> schedule;
	QVector<double> logoStarts;
	QVector<double> cueTime;
};

} // namespace closingtime
