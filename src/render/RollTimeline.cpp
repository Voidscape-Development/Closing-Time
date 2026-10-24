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

RollTimeline::RollTimeline(const TimelineInputs &inputs) : in(inputs)
{
	in.canvasHeight = std::max(1, in.canvasHeight);
	in.stripHeight = std::max(0, in.stripHeight);
	in.leadOut = std::max(0, in.leadOut);

	speed = in.scrollSpeed > 0.0 ? in.scrollSpeed : 0.0;
	travel = speed > 0.0 ? (in.canvasHeight + in.stripHeight) / speed : kNever;

	schedule.resize(in.blocks.size());
	simulate();

	logoStarts.resize(in.logos.size());
	for (int i = 0; i < in.logos.size(); ++i)
		logoStarts[i] = logoStart(i);
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

	if (stickyEntranceWaitsForRoll(block.entrance) && t < times.pin && block.fadeIn > 0.0)
		frame.alpha = std::clamp((t - times.enter) / block.fadeIn, 0.0, 1.0);

	return frame;
}

RollFrame RollTimeline::evaluate(double t) const
{
	RollFrame frame;
	t = std::max(0.0, t);

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

double RollTimeline::rebase(double t, const RollTimeline &next) const
{
	if (speed <= 0.0 || next.speed <= 0.0)
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
