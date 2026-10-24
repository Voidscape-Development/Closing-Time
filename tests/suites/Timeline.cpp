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

#include "harness/Harness.hpp"

#include "render/RollTimeline.hpp"

#include <cmath>

using namespace closingtime;
using namespace closingtime::test;

namespace {

/*
 * Round numbers throughout, so every expected time is arithmetic that can be checked by eye: a
 * 1000 px canvas, a 2000 px strip and 100 px/s make the full travel thirty seconds, and a block
 * whose slot starts 1000 px down the strip and pins with its top 400 px down the frame reaches the
 * pin at sixteen.
 */
constexpr double kEpsilon = 1e-6;

TimelineInputs roll()
{
	TimelineInputs in;
	in.canvasHeight = 1000;
	in.stripHeight = 2000;
	in.scrollSpeed = 100.0;
	return in;
}

TimelineBlock block(StickyRelease release, double hold = 5.0)
{
	TimelineBlock b;
	b.slotTop = 1000.0;
	b.slotHeight = 200.0;
	b.pinnedTop = 400.0;
	b.hold = hold;
	b.release = release;
	return b;
}

TimelineBlock waitingBlock(StickyRelease release, double hold = 5.0, double fadeIn = 1.0)
{
	TimelineBlock b = block(release, hold);
	b.entrance = StickyEntrance::AfterRoll;
	b.fadeIn = fadeIn;
	return b;
}

bool never(double t)
{
	return std::isinf(t);
}

} // namespace

CT_SUITE(timeline_plain, "A roll with nothing in it but the strip")
{
	const RollTimeline timeline(roll());

	checkNear(timeline.travelTime(), 30.0, kEpsilon, "the strip travels canvas plus strip at the scroll speed");
	checkNear(timeline.finishTime(), 30.0, kEpsilon, "and with nothing holding it open, that is the end");

	checkNear(timeline.evaluate(0.0).offset, 0.0, kEpsilon, "it starts below the frame");
	checkNear(timeline.evaluate(12.5).offset, 1250.0, kEpsilon, "and moves at the scroll speed");
	check(!timeline.evaluate(29.9).finished, "it is not over before the strip has gone");
	check(timeline.evaluate(30.0).finished, "and it is once it has");
	checkNear(timeline.evaluate(45.0).offset, 3000.0, kEpsilon, "after which the strip stays where it stopped");
	checkEq(timeline.evaluate(45.0).pass, 0, "a roll that does not loop is always on its first pass");
}

CT_SUITE(timeline_loop, "A looping roll is one pass over and over")
{
	TimelineInputs in = roll();
	in.loop = true;
	in.blocks = {block(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	check(never(timeline.finishTime()), "a loop never finishes");
	check(!timeline.evaluate(1000.0).finished, "however long it runs");

	const RollFrame second = timeline.evaluate(42.0);
	checkEq(second.pass, 1, "forty-two seconds in is the second pass");
	checkNear(second.offset, 1200.0, kEpsilon, "twelve seconds into it");

	/* The block was holding at the end of the first pass; the second starts it over. */
	const RollFrame first = timeline.evaluate(29.0);
	const RollFrame restarted = timeline.evaluate(31.0);
	checkNear(first.blocks.at(0).top, 400.0, kEpsilon, "the block is pinned near the end of a pass");
	check(restarted.blocks.at(0).top > 1000.0, "and back below the frame at the start of the next");
}

CT_SUITE(timeline_end_at_hold, "A block that pins, holds, and ends the roll where it is")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);
	const RollTimeline::BlockSchedule &times = timeline.schedules().at(0);

	checkNear(times.pin, 16.0, kEpsilon, "it pins when its slot reaches the pin");
	checkNear(times.holdEnd, 21.0, kEpsilon, "and holds for its hold");
	checkNear(timeline.finishTime(), 21.0, kEpsilon, "which is the end of the roll");
	check(never(times.release), "it never leaves");

	checkNear(timeline.evaluate(10.0).blocks.at(0).top, 1000.0, kEpsilon, "on its way up it rides the roll");
	checkNear(timeline.evaluate(18.0).blocks.at(0).top, 400.0, kEpsilon, "holding, it is at the pin");
	checkNear(timeline.evaluate(60.0).blocks.at(0).top, 400.0, kEpsilon, "and it is still there afterwards");
	checkNear(timeline.evaluate(60.0).offset, 2100.0, kEpsilon, "with the strip stopped where the roll ended");
}

CT_SUITE(timeline_resume_then_end, "A block that leaves, and ends the roll once it has gone")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::ResumeThenEnd)};
	const RollTimeline timeline(in);
	const RollTimeline::BlockSchedule &times = timeline.schedules().at(0);

	checkNear(times.release, 21.0, kEpsilon, "it lets go when its hold runs out");
	/* From a 400 px pin, a 200 px block is gone after 600 px of climbing: six seconds. */
	checkNear(times.gone, 27.0, kEpsilon, "and is gone once the last of it is off the top");
	/* Its exit is the end of the roll, wherever the strip has got to. */
	checkNear(timeline.finishTime(), 27.0, kEpsilon, "the roll ends the moment it has gone");
	checkNear(timeline.evaluate(40.0).offset, 2700.0, kEpsilon, "with the strip stopped where it was then");
	checkNear(timeline.evaluate(23.0).blocks.at(0).top, 200.0, kEpsilon, "climbing at the scroll speed");
	check(timeline.evaluate(23.0).blocks.at(0).released, "and says so");
}

CT_SUITE(timeline_resume_then_end_late, "A block still leaving when the strip runs out keeps the roll open")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::ResumeThenEnd, 12.0)};
	const RollTimeline timeline(in);
	const RollTimeline::BlockSchedule &times = timeline.schedules().at(0);

	checkNear(times.release, 28.0, kEpsilon, "a long hold lets go late");
	checkNear(times.gone, 34.0, kEpsilon, "and the block leaves after the strip has");
	checkNear(timeline.finishTime(), 34.0, kEpsilon, "so the roll ends when the block is gone");
}

CT_SUITE(timeline_resume_end_at_hold, "A block that ends the roll at its hold and leaves anyway")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::ResumeEndAtHold)};
	const RollTimeline timeline(in);

	checkNear(timeline.finishTime(), 21.0, kEpsilon, "the roll ends the moment the hold does");
	checkNear(timeline.evaluate(21.0).offset, 2100.0, kEpsilon, "and the strip stops there");
	/*
	 * Coasting: the roll is over, but a card frozen half off the top is worse than one that never
	 * left, so it carries on out.
	 */
	checkNear(timeline.evaluate(23.0).blocks.at(0).top, 200.0, kEpsilon,
		  "while the block carries on out after the end");
	check(!never(timeline.schedules().at(0).gone), "all the way out");
}

CT_SUITE(timeline_resume_only, "A block that leaves and ends nothing")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::ResumeOnly)};
	const RollTimeline timeline(in);

	checkNear(timeline.schedules().at(0).gone, 27.0, kEpsilon, "it leaves");
	checkNear(timeline.finishTime(), 30.0, kEpsilon, "and the strip decides when the roll is over");

	in.blocks = {block(StickyRelease::ResumeOnly, 12.0)};
	const RollTimeline late(in);
	checkNear(late.finishTime(), 34.0, kEpsilon, "unless it is still on screen when the strip has gone");
}

CT_SUITE(timeline_hold_forever, "A hold that never runs out")
{
	for (StickyRelease release : {StickyRelease::EndAtHold, StickyRelease::ResumeThenEnd,
				      StickyRelease::ResumeEndAtHold, StickyRelease::ResumeOnly}) {
		const Context context(QString::fromLatin1(stickyReleaseId(release)));

		TimelineInputs in = roll();
		TimelineBlock endless = block(release);
		endless.holdForever = true;
		in.blocks = {endless};
		const RollTimeline timeline(in);

		check(never(timeline.finishTime()), "holds the roll open for good");
		checkNear(timeline.evaluate(500.0).blocks.at(0).top, 400.0, kEpsilon, "at the pin");
	}
}

CT_SUITE(timeline_after_roll, "A block that waits for the roll and fades up into the empty frame")
{
	TimelineInputs in = roll();
	in.blocks = {waitingBlock(StickyRelease::EndAtHold, 5.0, 2.0)};
	const RollTimeline timeline(in);
	const RollTimeline::BlockSchedule &times = timeline.schedules().at(0);

	checkNear(times.enter, 30.0, kEpsilon, "it comes up once the strip has left the frame");
	checkNear(times.pin, 32.0, kEpsilon, "is all the way up when its fade is done");
	checkNear(timeline.finishTime(), 37.0, kEpsilon, "and its hold starts from there");

	check(!timeline.evaluate(20.0).blocks.at(0).showing, "nothing of it is drawn before then");
	checkNear(timeline.evaluate(31.0).blocks.at(0).alpha, 0.5, kEpsilon, "half way up, half faded");
	checkNear(timeline.evaluate(31.0).blocks.at(0).top, 400.0, kEpsilon, "at the pin from the start");
	checkNear(timeline.evaluate(33.0).blocks.at(0).alpha, 1.0, kEpsilon, "and solid once it is up");
}

CT_SUITE(timeline_after_roll_lead_out, "Lead-out is blank and does not count as the roll")
{
	TimelineInputs in = roll();
	in.leadOut = 500;
	in.blocks = {waitingBlock(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	/* The last drawn pixel is 1500 px down the strip: clear of the frame after 2500 px of travel. */
	checkNear(timeline.schedules().at(0).enter, 25.0, kEpsilon, "the card does not wait through the lead-out");
}

CT_SUITE(timeline_back_to_back, "Two cards that wait for the roll come up one after the other")
{
	TimelineInputs in = roll();
	in.blocks = {waitingBlock(StickyRelease::ResumeOnly, 3.0, 1.0),
		     waitingBlock(StickyRelease::EndAtHold, 3.0, 1.0)};
	const RollTimeline timeline(in);
	const RollTimeline::BlockSchedule &first = timeline.schedules().at(0);
	const RollTimeline::BlockSchedule &second = timeline.schedules().at(1);

	checkNear(first.enter, 30.0, kEpsilon, "the first comes up when the roll has gone");
	checkNear(first.release, 34.0, kEpsilon, "fades, holds and lets go");
	/* 400 px pin, 200 px tall: six seconds to climb off. */
	checkNear(first.gone, 40.0, kEpsilon, "and is gone six seconds later");
	checkNear(second.enter, 40.0, kEpsilon, "the second waits for it to go rather than fading up through it");
	checkNear(timeline.finishTime(), 44.0, kEpsilon, "and ends the roll at its own hold");

	const RollFrame between = timeline.evaluate(35.0);
	check(between.blocks.at(0).showing && !between.blocks.at(1).showing, "only one card is up at a time");
}

CT_SUITE(timeline_waits_behind_a_card, "A card that waits can be held off for good by one that never leaves")
{
	TimelineInputs in = roll();
	TimelineBlock endless = block(StickyRelease::EndAtHold);
	endless.holdForever = true;
	in.blocks = {endless, waitingBlock(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	check(never(timeline.schedules().at(1).enter), "the second card never comes up over the first");
	check(never(timeline.finishTime()), "and the roll is held open");
}

CT_SUITE(timeline_finish_freezes_holds, "The roll finishing stops every hold still running")
{
	TimelineInputs in = roll();
	TimelineBlock quick = block(StickyRelease::EndAtHold, 1.0);
	TimelineBlock slow = block(StickyRelease::ResumeThenEnd, 10.0);
	slow.slotTop = 900.0;
	in.blocks = {quick, slow};
	const RollTimeline timeline(in);

	checkNear(timeline.finishTime(), 17.0, kEpsilon, "the short hold ends the roll");
	check(never(timeline.schedules().at(1).release), "and the long one holds for good, rather than leaving later");
	checkNear(timeline.evaluate(100.0).blocks.at(1).top, 400.0, kEpsilon, "where it was pinned");
}

CT_SUITE(timeline_finish_stops_entrances, "A card still waiting when the roll ends never comes up")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::EndAtHold, 1.0), waitingBlock(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	checkNear(timeline.finishTime(), 17.0, kEpsilon, "the first block ends the roll before the strip has gone");
	check(never(timeline.schedules().at(1).enter), "so the card that waits for the strip never gets its turn");
}

CT_SUITE(timeline_unreachable_pin, "A pin the roll would never carry a block up to")
{
	TimelineInputs in = roll();
	TimelineBlock high = block(StickyRelease::EndAtHold);
	/* The slot stops 1000 px above the frame's top; a pin further up still can never be reached. */
	high.pinnedTop = -1500.0;
	in.blocks = {high};
	const RollTimeline timeline(in);

	checkNear(timeline.schedules().at(0).pin, 30.0, kEpsilon,
		  "it pins when the strip has gone as far as it goes, rather than holding the roll open forever");
	checkNear(timeline.finishTime(), 35.0, kEpsilon, "and its hold runs from there");
}

CT_SUITE(timeline_no_fade, "A card with no fade is up and pinned in the same moment")
{
	TimelineInputs in = roll();
	in.blocks = {waitingBlock(StickyRelease::EndAtHold, 5.0, 0.0)};
	const RollTimeline timeline(in);

	checkNear(timeline.schedules().at(0).pin, 30.0, kEpsilon, "pinned as it comes up");
	checkNear(timeline.evaluate(30.0).blocks.at(0).alpha, 1.0, kEpsilon, "and solid at once");
}

CT_SUITE(timeline_scrubbed, "A roll parked under manual scroll")
{
	TimelineInputs in = roll();
	in.blocks = {block(StickyRelease::ResumeThenEnd, 1.0), waitingBlock(StickyRelease::EndAtHold),
		     waitingBlock(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	const RollFrame early = timeline.evaluateScrubbed(500.0);
	checkNear(early.blocks.at(0).top, 1500.0, kEpsilon, "a block below its pin is where the roll puts it");
	check(!early.blocks.at(1).showing, "a card that waits is not up while the roll is on screen");

	const RollFrame late = timeline.evaluateScrubbed(2800.0);
	checkNear(late.blocks.at(0).top, 400.0, kEpsilon,
		  "a block past its pin is pinned, whatever its hold would have done by now");
	check(!late.blocks.at(1).showing, "and it keeps the cards behind it off the frame");

	/* With the traveling block out of the way, the first card that waits is up, and only it. */
	in.blocks[0].pinnedTop = -5000.0;
	const RollTimeline clear(in);
	const RollFrame end = clear.evaluateScrubbed(3000.0);
	check(end.blocks.at(1).showing, "once the roll has gone, the first card is up");
	checkNear(end.blocks.at(1).alpha, 1.0, kEpsilon, "never part-way through its fade");
	check(!end.blocks.at(2).showing, "and the one after it waits its turn");

	/* Parked is a position, not a history: the same position always shows the same frame. */
	const RollFrame again = clear.evaluateScrubbed(3000.0);
	check(again.blocks.at(1).showing == end.blocks.at(1).showing &&
		      again.blocks.at(2).showing == end.blocks.at(2).showing,
	      "the same position shows the same frame");
}

CT_SUITE(timeline_logos, "Animated logos play from roll time")
{
	TimelineInputs in = roll();
	TimelineLogo withRoll;
	withRoll.top = 0.0;
	TimelineLogo onEnter;
	onEnter.top = 1500.0;
	onEnter.startOnEnter = true;
	TimelineLogo doubled;
	doubled.speed = 2.0;
	in.logos = {withRoll, onEnter, doubled};
	const RollTimeline timeline(in);

	const RollFrame frame = timeline.evaluate(20.0);
	checkNear(frame.logoElapsedMs.at(0), 20000.0, kEpsilon,
		  "a logo that starts with the roll has run since it did");
	/* The logo's top reaches the bottom of the frame after 1500 px of travel. */
	checkNear(frame.logoElapsedMs.at(1), 5000.0, kEpsilon,
		  "one that waits for the frame has run since it got there");
	checkNear(frame.logoElapsedMs.at(2), 40000.0, kEpsilon, "and a speed multiplier runs it faster");
	checkNear(timeline.evaluate(10.0).logoElapsedMs.at(1), 0.0, kEpsilon, "nothing has played before it enters");
	checkNear(timeline.evaluate(90.0).logoElapsedMs.at(0), 30000.0, kEpsilon,
		  "a logo in the roll stands still once the roll has finished");
	checkNear(timeline.evaluateScrubbed(1700.0).logoElapsedMs.at(1), 2000.0, kEpsilon,
		  "and a parked roll shows the frame its position corresponds to");
}

CT_SUITE(timeline_card_logos, "A logo on a card plays while the card is up")
{
	TimelineInputs in = roll();
	in.blocks = {waitingBlock(StickyRelease::EndAtHold, 5.0, 0.0)};
	TimelineLogo logo;
	logo.block = 0;
	logo.startOnEnter = true;
	in.logos = {logo};
	const RollTimeline timeline(in);

	checkNear(timeline.evaluate(31.0).logoElapsedMs.at(0), 1000.0, kEpsilon, "it starts when the card comes up");
	checkNear(timeline.evaluate(60.0).logoElapsedMs.at(0), 30000.0, kEpsilon,
		  "and goes on playing after the roll has ended, for as long as the card holds");
}

CT_SUITE(timeline_still, "A roll that does not move")
{
	TimelineInputs in = roll();
	in.scrollSpeed = 0.0;
	in.blocks = {block(StickyRelease::EndAtHold), waitingBlock(StickyRelease::EndAtHold)};
	const RollTimeline timeline(in);

	check(never(timeline.travelTime()) && never(timeline.finishTime()), "never travels and never finishes");
	checkNear(timeline.evaluate(100.0).offset, 0.0, kEpsilon, "and stays below the frame");
	check(!timeline.evaluate(100.0).blocks.at(1).showing, "with nothing coming up");
}

CT_SUITE(timeline_rebase, "Changing the scroll speed keeps the strip where it is")
{
	TimelineInputs in = roll();
	const RollTimeline slow(in);
	in.scrollSpeed = 200.0;
	const RollTimeline fast(in);

	const double moved = slow.rebase(12.0, fast);
	checkNear(fast.evaluate(moved).offset, slow.evaluate(12.0).offset, kEpsilon, "the same distance up the frame");
	checkNear(slow.rebase(40.0, fast), 25.0, kEpsilon,
		  "and time already spent standing at the end carries over as it is");
}
