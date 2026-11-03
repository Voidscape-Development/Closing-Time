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

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

#include "model/CreditsModel.hpp"
#include "render/RollRenderer.hpp"
#include "render/RollTimeline.hpp"

#include <obs.hpp>

using namespace closingtime;
using namespace closingtime::test;

namespace {

/*
 * The same round numbers the timeline suite uses: a 1000 px canvas, a 2000 px strip and 100 px/s,
 * so the full travel is thirty seconds and every expected time can be checked by eye.
 */
TimelineInputs roll()
{
	TimelineInputs in;
	in.canvasHeight = 1000;
	in.stripHeight = 2000;
	in.scrollSpeed = 100.0;
	return in;
}

TimelineCue cue(double top, double line, int block = -1)
{
	TimelineCue c;
	c.top = top;
	c.line = line;
	c.block = block;
	return c;
}

Section action(double line = 1.0)
{
	Section section = Section::makeDefault(SectionType::Action);
	section.actionLine = line;
	return section;
}

RollLayoutPtr layOut(const Document &document)
{
	LogoCache logos;
	RollRenderer renderer(&logos);
	return renderer.layout(document);
}

} // namespace

CT_SUITE(action_defaults, "A new Action section")
{
	const Section section = Section::makeDefault(SectionType::Action);
	checkEq(section.actions.size(), 1, "starts with one action to fill in");
	check(section.actions.front().type == EndingActionType::SetSourceVisible, "which shows a source");
	checkNear(section.actionLine, 1.0, 1e-9, "and fires as it comes into frame");
	checkEq(section.paddingTop + section.paddingBottom, 0, "and carries no padding");
	check(!sectionUsesText(section.type) && !sectionUsesLogos(section.type) && !sectionUsesEntries(section.type),
	      "it has no content of its own");
	check(section.displayLabel().contains(QStringLiteral("Action")), "the list names it as an action");
}

CT_SUITE(action_takes_no_room, "A marker moves nothing above or below it")
{
	Section title = unpadded(SectionType::Title);
	title.text = QStringLiteral("Above");
	Section other = unpadded(SectionType::Title);
	other.text = QStringLiteral("Below");

	const Document without = documentWith(QVector<Section>{title, other});
	Section marker = action();
	marker.paddingTop = 40;
	marker.paddingBottom = 40;
	const Document with = documentWith(QVector<Section>{title, marker, other});

	checkEq(measure(with), measure(without), "the roll is exactly as long with a marker in it");
	check(renderImage(with) == renderImage(without), "and draws exactly the same pixels");

	const RollLayoutPtr layout = layOut(with);
	check(layout != nullptr, "the roll lays out");
	if (!layout)
		return;

	checkEq(layout->cues.size(), 1, "the marker is reported");
	checkEq(layout->layers.size(), 2, "and is no picture of its own");
	if (layout->cues.isEmpty())
		return;

	const QRectF above = boxOf(with, LayoutBox::Kind::Section, 0);
	checkNear(layout->cues.front().top, above.bottom(), 0.5, "it sits where the section above it ends");
	checkEq(layout->cues.front().block, -1, "and travels with the roll");
	checkEq(layout->cues.front().section, 1, "naming its own section");
	checkEq(layout->cues.front().actions.size(), 1, "carrying its actions");

	const QRectF line = boxOf(with, LayoutBox::Kind::Action);
	checkNear(line.top(), above.bottom(), 0.5, "the overlay draws it as a line in the same place");
	checkNear(line.height(), 0.0, 1e-9, "with no height");

	/* A hidden marker is no marker. */
	Section hidden = marker;
	hidden.visible = false;
	const RollLayoutPtr none = layOut(documentWith(QVector<Section>{title, hidden, other}));
	if (none)
		checkEq(none->cues.size(), 0, "a hidden marker is not reported");

	/* The line the marker fires on reaches the timeline in canvas pixels. */
	marker.actionLine = 0.25;
	const RollLayoutPtr quarter = layOut(documentWith(QVector<Section>{title, marker, other}));
	if (quarter) {
		const TimelineInputs in = quarter->timelineInputs();
		checkEq(in.cues.size(), 1, "the timeline is told about it");
		if (!in.cues.isEmpty())
			checkNear(in.cues.front().line, 0.25 * in.canvasHeight, 1e-6, "as a line a quarter down");
	}
}

CT_SUITE(action_in_block, "A marker on a card travels with the card")
{
	Section block = unpadded(SectionType::StickyBlock);
	block.children.clear();
	Section title = unpadded(SectionType::Title);
	title.text = QStringLiteral("Card");
	block.children.push_back(title);
	block.children.push_back(action());

	Section before = unpadded(SectionType::Spacer);
	before.spacerHeight = 300;

	const Document document = documentWith(QVector<Section>{before, block});
	const RollLayoutPtr layout = layOut(document);
	check(layout != nullptr, "the roll lays out");
	if (!layout || layout->cues.isEmpty() || layout->blocks.isEmpty()) {
		check(false, "the block and its marker are both reported");
		return;
	}

	const ActionCuePlacement &marker = layout->cues.front();
	checkEq(marker.block, 0, "the marker belongs to the block");
	checkNear(marker.top, layout->blocks.front().rect.height(), 0.5,
		  "placed from the block's slot, under the title it follows");
	checkEq(marker.section, 1, "naming the block that holds it");

	/* A block holding nothing but markers has no slot, so its markers go with the roll. */
	Section empty = unpadded(SectionType::StickyBlock);
	empty.children.clear();
	empty.children.push_back(action());
	const RollLayoutPtr bare = layOut(documentWith(QVector<Section>{before, empty}));
	if (bare && !bare->cues.isEmpty()) {
		checkEq(bare->blocks.size(), 0, "an empty block is not placed");
		checkEq(bare->cues.front().block, -1, "so its marker travels with the roll");
		checkNear(bare->cues.front().top, 300.0, 0.5, "from where the block would have been");
	} else {
		check(false, "a marker in an empty block is still reported");
	}
}

CT_SUITE(action_timeline_roll, "When a marker in the roll crosses its line")
{
	TimelineInputs in = roll();
	in.cues = {cue(0.0, 1000.0), cue(500.0, 1000.0), cue(500.0, 500.0), cue(500.0, 0.0), cue(2000.0, 0.0)};
	const RollTimeline timeline(in);
	const QVector<double> &times = timeline.cueTimes();
	checkEq(times.size(), 5, "every marker has a time");
	if (times.size() != 5)
		return;

	checkNear(times[0], 0.0, 1e-6, "a marker at the top of the strip enters the frame as the roll starts");
	checkNear(times[1], 5.0, 1e-6, "one 500 px down enters five seconds later");
	checkNear(times[2], 10.0, 1e-6, "and reaches the middle of the frame five seconds after that");
	checkNear(times[3], 15.0, 1e-6, "and the top edge five more after that");
	checkNear(times[4], 30.0, 1e-6, "the strip's last pixel reaches the top as the travel runs out");

	/* A marker the strip stops short of never fires. */
	TimelineInputs stops = roll();
	TimelineBlock card;
	card.slotTop = 1000.0;
	card.slotHeight = 200.0;
	card.pinnedTop = 400.0;
	card.hold = 2.0;
	card.release = StickyRelease::EndAtHold;
	stops.blocks = {card};
	stops.cues = {cue(1500.0, 0.0), cue(100.0, 0.0)};
	const RollTimeline ended(stops);
	if (ended.cueTimes().size() == 2) {
		/* The card pins at 16 s and ends the roll at 18 s, with the strip 1800 px along. */
		check(ended.cueTimes()[0] == RollTimeline::kNever,
		      "a marker below the line when the roll ends never fires");
		checkNear(ended.cueTimes()[1], 11.0, 1e-6, "one that got there first did");
	}

	/* A roll that does not move reaches nothing. */
	TimelineInputs still = roll();
	still.scrollSpeed = 0.0;
	still.cues = {cue(0.0, 1000.0)};
	const RollTimeline parked(still);
	check(parked.cueTimes().value(0) == RollTimeline::kNever, "a roll that does not move reaches no line");
}

CT_SUITE(action_timeline_blocks, "When a marker on a card crosses its line")
{
	/* A card pinning with its slot 400 px down, its slot starting 1000 px into the strip: pins at 16 s. */
	TimelineBlock card;
	card.slotTop = 1000.0;
	card.slotHeight = 200.0;
	card.pinnedTop = 400.0;
	card.hold = 5.0;
	card.release = StickyRelease::ResumeThenEnd;

	TimelineInputs in = roll();
	in.blocks = {card};
	/* 100 px into the card: 500 px down once pinned. */
	in.cues = {cue(100.0, 1000.0, 0), cue(100.0, 500.0, 0), cue(100.0, 300.0, 0)};
	const RollTimeline timeline(in);
	const QVector<double> &times = timeline.cueTimes();
	if (times.size() != 3) {
		check(false, "every marker has a time");
		return;
	}

	checkNear(times[0], 11.0, 1e-6, "it comes into frame with the card");
	checkNear(times[1], 16.0, 1e-6, "reaches a line at the pin as the card pins");
	/* Released at 21 s, 200 px short of the line, climbing at 100 px/s. */
	checkNear(times[2], 23.0, 1e-6, "and a line above the pin only once the card is let go");

	/* A card that never lets go never carries it past the pin. */
	TimelineInputs holdsForever = in;
	holdsForever.blocks[0].holdForever = true;
	const RollTimeline held(holdsForever);
	check(held.cueTimes().value(2) == RollTimeline::kNever, "a card that holds forever never gets it there");
	checkNear(held.cueTimes().value(1), 16.0, 1e-6, "though one at the pin still fires");

	/* A card that waits for the roll comes up already past a low line. */
	TimelineInputs waits = in;
	waits.blocks[0].entrance = StickyEntrance::AfterRoll;
	waits.blocks[0].fadeIn = 1.0;
	const RollTimeline after(waits);
	const double enter = after.schedules().value(0).enter;
	check(enter != RollTimeline::kNever, "the card comes up");
	checkNear(after.cueTimes().value(0), enter, 1e-6, "a marker past its line fires as the card comes up");
	checkNear(after.cueTimes().value(1), enter, 1e-6, "one exactly on it too");
	check(after.cueTimes().value(2) > enter, "one short of it waits for the card to leave");
}

CT_SUITE(action_sweep, "Which actions a stretch of roll time passed over")
{
	TimelineInputs in = roll();
	const RollTimeline timeline(in);
	const QVector<double> times = {0.0, 5.0, 10.0, RollTimeline::kNever};

	check(timeline.sweep(times, -1.0, 0.0).before == QVector<int>{0}, "the very first moment counts on the way in");
	check(timeline.sweep(times, 0.0, 5.0).before == QVector<int>{1}, "a stretch includes its end");
	checkEq(timeline.sweep(times, 5.0, 9.9).before.size(), 0, "and not its start");
	checkEq(timeline.sweep(times, -1.0, 1e9).before.size(), 3, "never is never reached");
	check(!timeline.sweep(times, 0.0, 100.0).wrapped, "a roll that does not loop never wraps");

	/* Looping: thirty seconds a pass. */
	TimelineInputs looping = roll();
	looping.loop = true;
	const RollTimeline loop(looping);
	const QVector<double> loopTimes = {1.0, 29.0, 31.0};

	const CueSweep within = loop.sweep(loopTimes, 30.5, 31.5);
	check(within.before == QVector<int>{0}, "the second pass reaches what the first did");
	check(!within.wrapped, "without wrapping inside a pass");

	const CueSweep round = loop.sweep(loopTimes, 28.0, 31.5);
	check(round.wrapped, "going past the end of a pass wraps");
	check(round.before == (QVector<int>{1, 2}), "the end of the old pass runs first, a delay past it included");
	check(round.after == QVector<int>{0}, "then the start of the new one");

	checkEq(loop.sweep(loopTimes, -1.0, 0.5).before.size(), 0, "nothing is due before it is");
}

CT_SUITE(action_persistence, "Actions in a section and the ending action share one shape")
{
	EndingActionConfig original;
	original.type = EndingActionType::SetSourceVisible;
	original.delay = 2.5;
	original.itemSceneName = QStringLiteral("Scene");
	original.itemSourceName = QStringLiteral("Background");
	original.itemMode = FilterToggleMode::Disable;

	/* The ending action keeps the keys a scene collection already carries. */
	OBSDataAutoRelease settings = obs_data_create();
	original.save(settings);
	check(QString::fromUtf8(obs_data_get_string(settings, "ea_type")) == QStringLiteral("set_source_visible"),
	      "the ending action is stored under its prefixed keys");
	EndingActionConfig loaded;
	loaded.load(settings);
	check(loaded == original, "and reads them back");

	/* A section's action is an object of its own, with no prefix. */
	OBSDataAutoRelease item = obs_data_create();
	original.save(item, "");
	check(obs_data_has_user_value(item, "item_source"), "a section's action is stored without one");
	EndingActionConfig fromItem;
	fromItem.load(item, "");
	check(fromItem == original, "and reads back the same");

	/* A section written before Action sections existed. */
	OBSDataAutoRelease bare = obs_data_create();
	obs_data_set_string(bare, "type", "title");
	Section legacy;
	legacy.load(bare);
	check(legacy.actions.isEmpty(), "an old section runs nothing");
	checkNear(legacy.actionLine, 1.0, 1e-9, "and would fire as it enters, were it an action");

	/* Out-of-range lines are pulled back onto the canvas. */
	Section wild = action(3.0);
	OBSDataAutoRelease wildData = obs_data_create();
	wild.save(wildData);
	Section tamed;
	tamed.load(wildData);
	checkNear(tamed.actionLine, 1.0, 1e-9, "a line below the canvas is put on its bottom edge");

	check(EndingActionConfig::isRevertible(EndingActionType::SetSourceVisible), "visibility can be put back");
	check(EndingActionConfig::isRevertible(EndingActionType::SetFilterEnabled), "so can a filter");
	check(!EndingActionConfig::isRevertible(EndingActionType::HideSelf),
	      "hiding the roll itself is not, or a roll that starts on show would start itself again");
	check(!EndingActionConfig::isRevertible(EndingActionType::SwitchScene), "nor is a scene switch");
}
