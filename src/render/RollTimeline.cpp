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

#include "render/RollTimeline.hpp"

#include <algorithm>
#include <cmath>

namespace closingtime {

namespace {

/* Where a block is in the simulation. Only `simulate` sees these. */
enum class BlockState { Hidden, Entering, Waiting, Pinned, Released };

double pictureBottom(const TimelineBlock &block, double top)
{
	return top + block.slotHeight + block.margin;
}

bool offFrame(const TimelineBlock &block, double top, int canvasHeight)
{
	return pictureBottom(block, top) <= 0.0 || top - block.margin >= canvasHeight;
}

bool clearedFrame(const TimelineBlock &block, double top)
{
	return pictureBottom(block, top) <= 0.0;
}

} // namespace

void applyPlayback(TimelineInputs *inputs, const Document &document)
{
	inputs->scrollSpeed = document.scrollSpeed;
	inputs->loop = document.loop;
	inputs->mode = document.playbackMode;
	inputs->fitSeconds = document.fitDuration ? document.fitSeconds : 0.0;
	inputs->pageHold = document.pageHold;
	inputs->pageTransition = document.pageTransition;
	inputs->pageTransitionTime = document.pageTransitionTime;
	inputs->pageAnchor = stickyAnchorFraction(document.pageAnchor);
	inputs->pageMargin = document.pageMargin;
}

namespace {

/* Eases a transition in and out, so a slide starts and stops rather than lurching. */
double ease(double p)
{
	p = std::clamp(p, 0.0, 1.0);
	return p * p * (3.0 - 2.0 * p);
}

/*
 * How long a roll at these inputs takes: until it is over, or -- for one that never is, a loop or a
 * roll held open by a card that holds for ever -- one pass of it. What a fit is measured against.
 */
double fittableLength(const RollTimeline &timeline)
{
	const double finish = timeline.finishTime();
	return std::isfinite(finish) ? finish : timeline.travelTime();
}

} // namespace

RollTimeline::RollTimeline(const TimelineInputs &inputs) : in(inputs)
{
	in.canvasHeight = std::max(1, in.canvasHeight);
	in.stripHeight = std::max(0, in.stripHeight);
	in.leadOut = std::max(0, in.leadOut);
	in.pageHold = std::max(0.0, in.pageHold);
	in.pageTransitionTime = std::max(0.0, in.pageTransitionTime);
	in.pageAnchor = std::clamp(in.pageAnchor, 0.0, 1.0);
	in.pageMargin = std::max(0, in.pageMargin);

	speed = in.scrollSpeed > 0.0 ? in.scrollSpeed : 0.0;
	hold = in.pageHold;

	schedule.resize(in.blocks.size());

	if (paged()) {
		buildPages();
	} else {
		if (in.fitSeconds > 0.0)
			fitSpeed();
		travel = speed > 0.0 ? (in.canvasHeight + in.stripHeight) / speed : kNever;
		simulate();
	}

	logoStarts.resize(in.logos.size());
	for (int i = 0; i < in.logos.size(); ++i)
		logoStarts[i] = logoStart(i);

	cueTime.resize(in.cues.size());
	for (int i = 0; i < in.cues.size(); ++i)
		cueTime[i] = cueCrossing(i);
}

/*
 * The scroll speed that plays the roll in `fitSeconds`.
 *
 * Found by bisection rather than worked out, because a roll is not a distance over a speed: a card's
 * hold and its fade take the same time at any speed, a block waiting for the roll comes up when the
 * roll has gone, and a release decides which of those ends it. What is true at every speed is that a
 * faster roll is never longer, which is all bisection needs. Each step builds a timeline with no fit,
 * which is a handful of events, so forty of them cost nothing worth measuring.
 */
void RollTimeline::fitSpeed()
{
	TimelineInputs probe = in;
	probe.fitSeconds = 0.0;

	const auto lengthAt = [&probe](double candidate) {
		probe.scrollSpeed = candidate;
		return fittableLength(RollTimeline(probe));
	};

	const double target = in.fitSeconds;
	if (lengthAt(kMaxFitSpeed) > target) {
		/* Even the fastest roll is too long: the cards' own holds take more than the time given. */
		speed = kMaxFitSpeed;
		fitReached = false;
		return;
	}
	if (lengthAt(kMinFitSpeed) < target) {
		/* A roll so short that even a crawl is over too soon. */
		speed = kMinFitSpeed;
		fitReached = false;
		return;
	}

	/* On the logarithm of the speed, since the length goes roughly as its inverse. */
	double low = std::log(kMinFitSpeed);
	double high = std::log(kMaxFitSpeed);
	for (int step = 0; step < 48; ++step) {
		const double middle = (low + high) / 2.0;
		if (lengthAt(std::exp(middle)) > target)
			low = middle;
		else
			high = middle;
	}
	speed = std::exp(high);
}

/*
 * Cuts the strip into pages and schedules them.
 *
 * A page is a run of whole sections that fits the frame between its margins. A new one starts at a
 * Page Break, and when the next section would not fit; a section taller than the frame is a page of
 * its own and is cut by the frame's edges, since there is nowhere else to put it. Nothing is laid out
 * again for any of this -- a page is a range of the strip the layout already made.
 *
 * Each page comes in, holds and goes out. Under Fade a page goes all the way out before the next one
 * comes in; under Crossfade and Slide the two happen together, so the frame is never empty between
 * pages. A Slide moves both pages the same distance at once, far enough that the one leaving clears
 * the top and the one arriving starts from below the bottom edge, so the two never overlap -- it is a
 * scroll that stops at every page.
 */
void RollTimeline::buildPages()
{
	const double canvas = in.canvasHeight;
	const double margin = std::min<double>(in.pageMargin, canvas / 2.0 - 1.0);
	const double room = std::max(1.0, canvas - 2.0 * std::max(0.0, margin));

	pageList.clear();
	Page current;
	bool open = false;
	const auto close = [&] {
		if (open)
			pageList.append(current);
		open = false;
	};

	for (const TimelineSpan &span : in.spans) {
		if (span.pageBreak) {
			close();
			continue;
		}

		const double bottom = span.top + span.height;
		if (open && bottom - current.top > room)
			close();

		if (!open) {
			current = Page();
			current.top = span.top;
			open = true;
		}
		current.bottom = bottom;
	}
	close();

	const int count = pageList.size();
	const double fade = in.pageTransition == PageTransition::Cut ? 0.0 : in.pageTransitionTime;
	const bool overlapping = in.pageTransition == PageTransition::Crossfade ||
				 in.pageTransition == PageTransition::Slide;

	/*
	 * A fitted roll is fitted by its hold: the transitions are a look, and stretching them to fill
	 * time would turn a crossfade into a slow dissolve nobody asked for.
	 */
	if (in.fitSeconds > 0.0 && count > 0) {
		const double fitted = overlapping ? (in.fitSeconds - fade) / count - fade
						  : in.fitSeconds / count - 2.0 * fade;
		hold = std::max(0.0, fitted);
		fitReached = fitted >= 0.0;
	}

	for (int i = 0; i < count; ++i) {
		Page &page = pageList[i];
		const double height = page.bottom - page.top;
		page.rest = std::max(0.0, margin) + (room - height) * in.pageAnchor;

		page.enter = overlapping ? i * (hold + fade) : i * (hold + 2.0 * fade);
		page.holdStart = page.enter + fade;
		page.holdEnd = page.holdStart + hold;
		page.exit = page.holdEnd + fade;
	}

	/* The slide distances: shared by each pair, so the two pages move as one strip. */
	for (int i = 0; i < count; ++i) {
		Page &page = pageList[i];
		if (i == 0)
			page.enterDistance = canvas - page.rest;
		if (i + 1 < count) {
			Page &next = pageList[i + 1];
			const double clearTop = page.rest + (page.bottom - page.top);
			const double fromBelow = std::max(canvas, clearTop + std::max(0.0, margin)) - next.rest;
			const double distance = std::max(clearTop, fromBelow);
			page.exitDistance = distance;
			next.enterDistance = distance;
		} else {
			page.exitDistance = page.rest + (page.bottom - page.top);
		}
	}

	const double total = count > 0 ? pageList.last().exit : 0.0;
	travel = total;
	finish = in.loop ? kNever : total;

	/* Nothing in a paged roll pins: a card is shown in its slot, on its page. */
	for (BlockSchedule &times : schedule)
		times = BlockSchedule();

	logoStarts.resize(in.logos.size());
	cueTime.resize(in.cues.size());
}

/* The page that shows strip row `stripY`: the one it is on, or the next one after it. -1 for none. */
int RollTimeline::pageHolding(double stripY) const
{
	for (int i = 0; i < pageList.size(); ++i) {
		if (stripY < pageList.at(i).bottom)
			return i;
	}
	return -1;
}

PageFrame RollTimeline::pageAt(int index, double t) const
{
	const Page &page = pageList.at(index);
	PageFrame frame;
	frame.top = page.top;
	frame.bottom = page.bottom;

	double y = page.rest;
	const double fade = page.holdStart - page.enter;
	const bool slide = in.pageTransition == PageTransition::Slide;

	if (t < page.holdStart && fade > 0.0) {
		const double p = (t - page.enter) / fade;
		if (slide)
			y = page.rest + page.enterDistance * (1.0 - ease(p));
		else
			frame.alpha = std::clamp(p, 0.0, 1.0);
	} else if (t >= page.holdEnd && fade > 0.0) {
		const double p = (t - page.holdEnd) / fade;
		if (slide)
			y = page.rest - page.exitDistance * ease(p);
		else
			frame.alpha = std::clamp(1.0 - p, 0.0, 1.0);
	}

	frame.stripTop = y - page.top;
	return frame;
}

double RollTimeline::stripOffsetAt(double t) const
{
	if (speed <= 0.0)
		return 0.0;

	/* The strip stops where the roll finished, or where it ran out of travel. */
	return speed * std::clamp(t, 0.0, std::min(travel, finish));
}

double RollTimeline::blockTopAt(int index, double t) const
{
	const TimelineBlock &block = in.blocks.at(index);
	const BlockSchedule &times = schedule.at(index);

	if (t >= times.release)
		return block.pinnedTop - speed * (t - times.release);

	/* A block that waited for the roll has no slot to travel from: it belongs at the pin. */
	if (stickyEntranceWaitsForRoll(block.entrance))
		return block.pinnedTop;

	/*
	 * Traveling with the roll until the roll has carried it up to the pin, and held there after:
	 * exactly the lower of the two positions, which is what lets one expression serve a block on its
	 * way up, one holding, and a roll parked anywhere under manual scroll.
	 */
	return std::max(block.pinnedTop, in.canvasHeight + block.slotTop - stripOffsetAt(t));
}

/*
 * Works out when everything happens.
 *
 * One event at a time, earliest first, and every candidate recomputed after each one, because the
 * blocks are not independent of each other: a block that waits for the roll also waits for every
 * other block to be off the frame, and the roll finishing -- which any of them may cause -- stops
 * every hold that has not yet run out. Taking the events one at a time is also what makes two
 * blocks that both wait for the roll come up one after the other rather than together: the first
 * one up is on the frame by the time the second is asked.
 *
 * The rules are the ones the roll has always played by:
 *
 *   - A block traveling with the roll pins when its slot reaches the pin, or when the strip has
 *     traveled as far as it goes, whichever is first -- the second catches a pin the roll would
 *     never reach.
 *   - A block that waits for the roll comes up once the strip's last drawn pixel has left the top
 *     of the frame and no other block is on the frame, fades up, and pins when the fade is done.
 *   - A hold counts down only while the roll is running. Once the roll has finished, a block still
 *     holding holds for good, and one still waiting for the roll never comes up.
 *   - A fade or a climb that has already started carries on after the roll has finished.
 *   - The roll finishes when a block's release says its hold or its exit is the end of it, or once
 *     the strip has traveled all the way with no block still holding the roll open. A loop never
 *     finishes.
 */
void RollTimeline::simulate()
{
	const int count = in.blocks.size();
	const int canvasHeight = in.canvasHeight;

	QVector<BlockState> state(count);
	QVector<bool> spent(count, false);
	QVector<bool> departed(count, false);

	for (int i = 0; i < count; ++i) {
		const TimelineBlock &block = in.blocks.at(i);
		BlockSchedule &times = schedule[i];

		if (stickyEntranceWaitsForRoll(block.entrance)) {
			state[i] = BlockState::Hidden;
			times.enter = kNever;
		} else {
			state[i] = BlockState::Waiting;
			times.enter = 0.0;
		}
	}

	/* The moment the strip's last drawn pixel leaves the top of the frame. Lead-out is blank. */
	const double rollCleared = speed > 0.0 ? (canvasHeight + std::max(0, in.stripHeight - in.leadOut)) / speed
					       : kNever;

	const auto showing = [&](int i) {
		return state.at(i) != BlockState::Hidden;
	};

	/* Whether anything a viewer could see of another block is on the frame at `t`. */
	const auto otherOnFrame = [&](int except, double t) {
		for (int j = 0; j < count; ++j) {
			if (j == except || !showing(j))
				continue;
			if (!offFrame(in.blocks.at(j), blockTopAt(j, t), canvasHeight))
				return true;
		}
		return false;
	};

	/* Whether some block still has a say in the roll being over. */
	const auto pending = [&](double t) {
		for (int i = 0; i < count; ++i) {
			const TimelineBlock &block = in.blocks.at(i);
			if (state.at(i) == BlockState::Hidden || state.at(i) == BlockState::Entering)
				return true;
			if (stickyReleaseEndsAtHold(block.release)) {
				if (!spent.at(i))
					return true;
			} else if (stickyReleaseEndsAtExit(block.release)) {
				if (!departed.at(i))
					return true;
			} else if (!clearedFrame(block, blockTopAt(i, t))) {
				/* A block that ends nothing holds the roll open while it is on screen. */
				return true;
			}
		}
		return false;
	};

	enum class Event { None, Pin, FadeEnd, HoldEnd, Gone, Enter };

	double now = 0.0;
	bool finished = false;

	/* Every block has a handful of transitions; the bound only guards against a bug looping forever. */
	const int limit = 16 + count * 16;

	for (int step = 0; step < limit; ++step) {
		double soonest = kNever;
		Event event = Event::None;
		int who = -1;

		const auto consider = [&](double at, Event kind, int index) {
			if (at < soonest) {
				soonest = at;
				event = kind;
				who = index;
			}
		};

		for (int i = 0; i < count; ++i) {
			const TimelineBlock &block = in.blocks.at(i);
			const BlockSchedule &times = schedule.at(i);

			switch (state.at(i)) {
			case BlockState::Hidden:
				/* Considered below, once everything that could happen before it is known. */
				break;

			case BlockState::Waiting:
				if (!finished && speed > 0.0) {
					const double reaches =
						std::max(0.0, (canvasHeight + block.slotTop - block.pinnedTop) / speed);
					consider(std::max(now, std::min(reaches, travel)), Event::Pin, i);
				}
				break;

			case BlockState::Entering:
				consider(std::max(now, times.enter + std::max(0.0, block.fadeIn)), Event::FadeEnd, i);
				break;

			case BlockState::Pinned:
				if (!finished && !block.holdForever && !spent.at(i))
					consider(std::max(now, times.pin + std::max(0.0, block.hold)), Event::HoldEnd,
						 i);
				break;

			case BlockState::Released:
				if (!departed.at(i) && speed > 0.0) {
					const double distance = block.pinnedTop + block.slotHeight + block.margin;
					consider(std::max(now, times.release + std::max(0.0, distance) / speed),
						 Event::Gone, i);
				}
				break;
			}
		}

		/*
		 * A block waiting for the roll can only be judged at the moment it would come up, and only
		 * once nothing else is due before then -- whether another block is in its way depends on
		 * what that block does in the meantime. When the way is not clear it waits for the next
		 * event, which is the only thing that could clear it.
		 */
		if (!finished) {
			for (int i = 0; i < count; ++i) {
				if (state.at(i) != BlockState::Hidden)
					continue;

				const double at = std::max(now, rollCleared);
				if (at > soonest || at == kNever)
					continue;
				if (otherOnFrame(i, at))
					continue;

				consider(at, Event::Enter, i);
				/* The earliest one up is the one that goes; the rest are asked again after it. */
				break;
			}
		}

		/* The strip finishing the roll itself, when nothing is holding it open. */
		if (!finished && !in.loop) {
			const double at = std::max(now, travel);
			if (at < soonest && at != kNever && !pending(at)) {
				finish = at;
				finished = true;
				now = at;
				continue;
			}
		}

		if (event == Event::None) {
			/* Nothing more will ever happen. Whatever is still pending holds the roll open for good. */
			break;
		}

		now = soonest;
		const TimelineBlock &block = in.blocks.at(who);
		BlockSchedule &times = schedule[who];
		bool endsRoll = false;

		switch (event) {
		case Event::Pin:
			state[who] = BlockState::Pinned;
			times.pin = now;
			break;

		case Event::Enter:
			times.enter = now;
			if (block.fadeIn > 0.0) {
				state[who] = BlockState::Entering;
				break;
			}
			/* No fade: up and pinned in the same moment. */
			state[who] = BlockState::Pinned;
			times.pin = now;
			break;

		case Event::FadeEnd:
			/* The hold starts once the card is all the way up, not while it is arriving. */
			state[who] = BlockState::Pinned;
			times.pin = now;
			break;

		case Event::HoldEnd:
			times.holdEnd = now;
			if (stickyReleaseEndsAtHold(block.release))
				endsRoll = true;
			spent[who] = true;
			if (stickyReleaseResumes(block.release)) {
				state[who] = BlockState::Released;
				times.release = now;
			}
			break;

		case Event::Gone:
			times.gone = now;
			departed[who] = true;
			if (stickyReleaseEndsAtExit(block.release))
				endsRoll = true;
			break;

		case Event::None:
			break;
		}

		if (endsRoll && !finished && !in.loop) {
			finish = now;
			finished = true;
		}
	}
}

double RollTimeline::logoStart(int index) const
{
	const TimelineLogo &logo = in.logos.at(index);

	/* Played as pages, a logo set to start on entering starts as its page comes in. */
	if (paged()) {
		if (!logo.startOnEnter)
			return 0.0;
		double y = logo.top;
		if (logo.block >= 0 && logo.block < in.blocks.size())
			y += in.blocks.at(logo.block).slotTop;
		const int page = pageHolding(y);
		return page >= 0 ? pageList.at(page).enter : kNever;
	}

	if (logo.block >= 0 && logo.block < in.blocks.size()) {
		const TimelineBlock &block = in.blocks.at(logo.block);
		const BlockSchedule &times = schedule.at(logo.block);

		if (!logo.startOnEnter)
			return 0.0;

		/* A card that comes up after the roll is entered the moment it comes up. */
		if (stickyEntranceWaitsForRoll(block.entrance))
			return times.enter;

		/* One that travels with the roll is entered when the logo's own top reaches the frame. */
		if (block.pinnedTop + logo.top >= in.canvasHeight || speed <= 0.0)
			return kNever;
		return std::max(0.0, (block.slotTop + logo.top) / speed);
	}

	if (!logo.startOnEnter)
		return 0.0;

	/*
	 * A play-once sting bound to a logo two thirds of the way down a long roll would otherwise have
	 * finished several minutes before anyone could see it.
	 */
	if (speed <= 0.0)
		return kNever;
	return std::max(0.0, logo.top / speed);
}

/*
 * When a marker first reaches its line, coming up from below.
 *
 * In the roll that is a straight line in time: the marker comes in at the bottom of the frame and
 * climbs at the scroll speed until the strip stops. In a block it climbs with the block until the
 * block pins, sits there for the hold, and climbs again once the block is let go -- so a marker the
 * pin leaves short of its line crosses it on the way out, or never, for a card that never leaves.
 */
double RollTimeline::cueCrossing(int index) const
{
	const TimelineCue &cue = in.cues.at(index);

	/*
	 * Played as pages, a marker fires as its page starts to come in -- a marker between two pages
	 * belongs to the one after it, which is where it was dropped to come before. Its line has nothing
	 * to measure against: a page does not travel past it.
	 */
	if (paged()) {
		double y = cue.top;
		if (cue.block >= 0 && cue.block < in.blocks.size())
			y += in.blocks.at(cue.block).slotTop;
		const int page = pageHolding(y);
		return page >= 0 ? pageList.at(page).enter : kNever;
	}

	if (speed <= 0.0)
		return kNever;

	const double stops = std::min(travel, finish);

	if (cue.block < 0 || cue.block >= in.blocks.size()) {
		const double at = std::max(0.0, (in.canvasHeight + cue.top - cue.line) / speed);
		return at <= stops ? at : kNever;
	}

	const TimelineBlock &block = in.blocks.at(cue.block);
	const BlockSchedule &times = schedule.at(cue.block);
	const double pinned = block.pinnedTop + cue.top;

	if (!stickyEntranceWaitsForRoll(block.entrance)) {
		/* Traveling with the roll, it crosses on the way up if the pin is past the line. */
		const double rising = std::max(0.0, (in.canvasHeight + block.slotTop + cue.top - cue.line) / speed);
		if (pinned <= cue.line && rising <= stops)
			return rising;
	} else if (times.enter != kNever && pinned <= cue.line) {
		/* A card that comes up already past the line crosses it as it comes up. */
		return times.enter;
	}

	if (times.release == kNever)
		return kNever;

	/* Let go: it climbs from wherever it was at the release. */
	const double from = blockTopAt(cue.block, times.release) + cue.top;
	return times.release + std::max(0.0, from - cue.line) / speed;
}

BlockFrame RollTimeline::blockAt(int index, double t) const
{
	const TimelineBlock &block = in.blocks.at(index);
	const BlockSchedule &times = schedule.at(index);

	BlockFrame frame;
	if (t < times.enter)
		return frame;

	frame.showing = true;
	frame.top = blockTopAt(index, t);
	frame.released = t >= times.release;
	frame.held = !frame.released && (stickyEntranceWaitsForRoll(block.entrance) || t >= times.pin);

	if (stickyEntranceWaitsForRoll(block.entrance) && t < times.pin && block.fadeIn > 0.0)
		frame.alpha = std::clamp((t - times.enter) / block.fadeIn, 0.0, 1.0);

	return frame;
}

RollFrame RollTimeline::evaluate(double t) const
{
	RollFrame frame;
	t = std::max(0.0, t);

	if (paged()) {
		frame.paged = true;
		if (in.loop && travel > 0.0 && std::isfinite(travel)) {
			frame.pass = static_cast<int>(std::floor(t / travel));
			t -= frame.pass * travel;
		}
		frame.finished = !in.loop && t >= finish;

		for (int i = 0; i < pageList.size(); ++i) {
			const Page &page = pageList.at(i);
			if (t >= page.enter && t < page.exit)
				frame.pages.append(pageAt(i, t));
		}

		frame.logoElapsedMs.reserve(in.logos.size());
		for (int i = 0; i < in.logos.size(); ++i) {
			const double until = std::min(t, finish);
			const double elapsed = std::max(0.0, until - logoStarts.at(i));
			const double rate = std::clamp(in.logos.at(i).speed, kMinLogoSpeed, kMaxLogoSpeed);
			frame.logoElapsedMs.append(std::isfinite(elapsed) ? elapsed * 1000.0 * rate : 0.0);
		}
		return frame;
	}

	/* A loop is the same pass over and over: every block and every animation starts again with it. */
	if (in.loop && travel > 0.0 && travel != kNever) {
		frame.pass = static_cast<int>(std::floor(t / travel));
		t -= frame.pass * travel;
	}

	frame.offset = stripOffsetAt(t);
	frame.finished = !in.loop && t >= finish;

	frame.blocks.reserve(in.blocks.size());
	for (int i = 0; i < in.blocks.size(); ++i)
		frame.blocks.append(blockAt(i, t));

	frame.logoElapsedMs.reserve(in.logos.size());
	for (int i = 0; i < in.logos.size(); ++i) {
		const TimelineLogo &logo = in.logos.at(i);
		/*
		 * A logo in the roll stands still once the roll has finished, as the roll does. One on a
		 * card goes on playing for as long as the card is up: a closing card that holds is exactly
		 * where a moving logo is meant to be seen.
		 */
		const double until = logo.block >= 0 ? t : std::min(t, finish);
		const double elapsed = std::max(0.0, until - logoStarts.at(i));
		const double rate = std::clamp(logo.speed, kMinLogoSpeed, kMaxLogoSpeed);
		frame.logoElapsedMs.append(std::isfinite(elapsed) ? elapsed * 1000.0 * rate : 0.0);
	}

	return frame;
}

RollFrame RollTimeline::evaluateScrubbed(double offset) const
{
	/*
	 * Played as pages there is no position, only a moment: the share of the travel the scrub is at
	 * is taken as the same share of the pages' running time, and never quite the end of it -- a scrub
	 * to the far end shows the last page rather than the empty frame after it.
	 */
	if (paged()) {
		const double share =
			std::clamp(offset / std::max(1.0, double(in.canvasHeight + in.stripHeight)), 0.0, 1.0);
		const double total = travel;
		RollFrame frame = evaluate(std::min(share * total, std::max(0.0, total - 1e-6)));
		frame.finished = false;
		return frame;
	}

	RollFrame frame;
	const int canvasHeight = in.canvasHeight;
	frame.offset = std::clamp(offset, 0.0, static_cast<double>(canvasHeight + in.stripHeight));

	const double stripTop = canvasHeight - frame.offset;
	const bool rollCleared = stripTop + std::max(0, in.stripHeight - in.leadOut) <= 0.0;

	frame.blocks.resize(in.blocks.size());

	/* Blocks that travel with the roll first: they are where the position puts them, pinned or not. */
	for (int i = 0; i < in.blocks.size(); ++i) {
		const TimelineBlock &block = in.blocks.at(i);
		if (stickyEntranceWaitsForRoll(block.entrance))
			continue;

		BlockFrame &out = frame.blocks[i];
		out.showing = true;
		out.top = std::max(block.pinnedTop, stripTop + block.slotTop);
		out.held = stripTop + block.slotTop <= block.pinnedTop;
	}

	/*
	 * Then the ones that wait, in order: up once the roll has gone by, if nothing already on the frame
	 * is in the way. Never part-way through a fade -- a position is not a moment in one.
	 */
	for (int i = 0; i < in.blocks.size(); ++i) {
		const TimelineBlock &block = in.blocks.at(i);
		if (!stickyEntranceWaitsForRoll(block.entrance) || !rollCleared)
			continue;

		bool blocked = false;
		for (int j = 0; j < in.blocks.size(); ++j) {
			if (j != i && frame.blocks.at(j).showing &&
			    !offFrame(in.blocks.at(j), frame.blocks.at(j).top, canvasHeight))
				blocked = true;
		}
		if (blocked)
			continue;

		BlockFrame &out = frame.blocks[i];
		out.showing = true;
		out.top = block.pinnedTop;
		out.held = true;
	}

	const double t = speed > 0.0 ? frame.offset / speed : 0.0;
	frame.logoElapsedMs.reserve(in.logos.size());
	for (int i = 0; i < in.logos.size(); ++i) {
		const TimelineLogo &logo = in.logos.at(i);
		/* A card has no clock while the roll is parked: it shows its logo's first frame. */
		const double elapsed = logo.block >= 0 ? 0.0 : std::max(0.0, t - logoStarts.at(i));
		const double rate = std::clamp(logo.speed, kMinLogoSpeed, kMaxLogoSpeed);
		frame.logoElapsedMs.append(std::isfinite(elapsed) ? elapsed * 1000.0 * rate : 0.0);
	}

	return frame;
}

CueSweep RollTimeline::sweep(const QVector<double> &times, double from, double to) const
{
	CueSweep result;
	if (to < from)
		return result;

	const auto collect = [&times](QVector<int> *into, double after, double upTo) {
		for (int i = 0; i < times.size(); ++i) {
			const double at = times.at(i);
			if (at == kNever || !std::isfinite(at))
				continue;
			if (at > after && at <= upTo)
				into->append(i);
		}
	};

	const bool looping = in.loop && travel > 0.0 && travel != kNever;
	if (!looping) {
		collect(&result.before, from < 0.0 ? -kNever : from, to);
		return result;
	}

	const double fromPass = from < 0.0 ? 0.0 : std::floor(from / travel);
	const double toPass = std::floor(std::max(0.0, to) / travel);
	const double fromLocal = from < 0.0 ? -kNever : from - fromPass * travel;
	const double toLocal = std::max(0.0, to) - toPass * travel;

	if (toPass == fromPass) {
		collect(&result.before, fromLocal, toLocal);
		return result;
	}

	/*
	 * Gone round. Everything left in the pass that ended -- past the end of it too, for a delay that
	 * outlasts the roll -- and then the new pass from its start. A stall long enough to skip whole
	 * passes is counted as one wrap: nobody watched the ones in between.
	 */
	collect(&result.before, fromLocal, kNever);
	result.wrapped = true;
	collect(&result.after, -kNever, toLocal);
	return result;
}

double RollTimeline::rebase(double t, const RollTimeline &next) const
{
	/* Pages have no position to keep; a change to them goes on from the same moment. */
	if (paged() || next.paged() || speed <= 0.0 || next.speed <= 0.0)
		return t;

	if (in.loop && travel > 0.0 && travel != kNever)
		t = std::fmod(std::max(0.0, t), travel);

	/*
	 * While the strip is moving, the same position is the same distance: time scales with the
	 * speed. Once it has stopped, the time it has stood still carries over as it is.
	 */
	const double moving = std::min(travel, finish);
	if (t <= moving)
		return t * speed / next.speed;

	return moving * speed / next.speed + (t - moving);
}

} // namespace closingtime
