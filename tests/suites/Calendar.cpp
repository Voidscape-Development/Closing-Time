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

#include <obs.h>
#include <obs.hpp>

#include <QDate>
#include <QPainter>
#include <QDateTime>

#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

#include "model/CalendarModel.hpp"
#include "model/CalendarPresets.hpp"
#include "render/CalendarRenderer.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* One cache for the whole run, for the same reason the strip probe keeps one. */
LogoCache &logoCache()
{
	static LogoCache cache;
	return cache;
}

/* A fixed instant, so every check about the clock says the same thing on every machine and day. */
QDateTime fixedNow()
{
	return QDateTime(QDate(2026, 6, 27), QTime(14, 30), QTimeZone::systemTimeZone());
}

/*
 * The smallest board that can be asked a question: one day, two lanes, three events on a clock axis.
 * Everything else in this suite starts from it and pushes one thing off its default.
 */
CalendarDocument basicBoard()
{
	CalendarDocument document;
	document.width = 1920;
	document.height = 1080;
	document.layout = CalendarLayout::Grid;
	document.orientation = GridOrientation::TimeDown;
	document.axisMode = TimeAxisMode::Clock;

	CalendarDay day;
	day.id = QStringLiteral("d1");
	day.date = QDate(2026, 6, 27);
	document.days.append(day);

	CalendarLane a;
	a.id = QStringLiteral("l1");
	a.name = QStringLiteral("Lane A");
	document.lanes.append(a);

	CalendarLane b;
	b.id = QStringLiteral("l2");
	b.name = QStringLiteral("Lane B");
	document.lanes.append(b);

	CalendarCategory pools;
	pools.id = QStringLiteral("c1");
	pools.name = QStringLiteral("Pools");
	pools.style.usePanel = true;
	pools.style.panel.fill = BackgroundFill::Color;
	pools.style.panel.color = QColor(40, 80, 160);
	document.categories.append(pools);

	const auto add = [&document](const char *id, const char *title, const char *lane, int start, int end) {
		CalendarEvent event;
		event.id = QString::fromLatin1(id);
		event.title = QString::fromLatin1(title);
		event.dayId = QStringLiteral("d1");
		event.laneId = QString::fromLatin1(lane);
		event.categoryId = QStringLiteral("c1");
		event.startMinutes = start;
		event.endMinutes = end;
		document.events.append(event);
	};

	add("e1", "Morning Pools", "l1", 10 * 60, 12 * 60);
	add("e2", "Afternoon Pools", "l1", 13 * 60, 16 * 60);
	add("e3", "Side Bracket", "l2", 11 * 60, 14 * 60);

	return document;
}

QVector<CalendarHit> hitsOf(const CalendarDocument &document)
{
	const CalendarRenderer renderer(&logoCache());
	return renderer.hitBoxes(document, fixedNow());
}

QRectF hitFor(const CalendarDocument &document, const QString &eventId)
{
	const int index = [&] {
		for (int i = 0; i < document.events.size(); ++i) {
			if (document.events[i].id == eventId)
				return i;
		}
		return -1;
	}();

	for (const CalendarHit &hit : hitsOf(document)) {
		if (hit.event == index)
			return hit.rect;
	}
	return QRectF();
}

} // namespace

CT_SUITE(calendar_axis, "How a time turns into a position on the board")
{
	CalendarDocument document = basicBoard();

	/* The axis is taken from the events when it has not been pinned, rounded out to whole hours. */
	int start = 0;
	int end = 0;
	document.axisRange(&start, &end);
	checkEq(start, 10 * 60, "axis starts at the first event's hour");
	checkEq(end, 16 * 60, "axis ends at the last event's hour");

	/* Two hours of event at 90 px an hour is 180 px of block, whatever else is on the board. */
	document.grid.pixelsPerHour = 90.0;
	document.grid.blockInset = 0.0;

	const QRectF first = hitFor(document, QStringLiteral("e1"));
	checkNear(first.height(), 180.0, 1.0, "a two-hour block is two hours tall");

	const QRectF second = hitFor(document, QStringLiteral("e2"));
	checkNear(second.height(), 270.0, 1.0, "a three-hour block is three hours tall");

	/* And the gap between them is the hour nothing happens in, not a gap the layout invented. */
	checkNear(second.top() - first.bottom(), 90.0, 1.0, "an empty hour takes an hour's room");

	/* Pinning the axis widens the board rather than moving the blocks off it. */
	document.grid.axisStart = 8 * 60;
	document.grid.axisEnd = 20 * 60;
	const QRectF pinned = hitFor(document, QStringLiteral("e1"));
	checkNear(pinned.top() - first.top(), 180.0, 1.0, "pinning the axis two hours earlier moves a block down");
}

CT_SUITE(calendar_orientation, "The same schedule laid out both ways round")
{
	CalendarDocument down = basicBoard();
	down.orientation = GridOrientation::TimeDown;
	down.grid.blockInset = 0.0;

	CalendarDocument across = basicBoard();
	across.orientation = GridOrientation::TimeAcross;
	across.grid.blockInset = 0.0;

	const QRectF a = hitFor(down, QStringLiteral("e1"));
	const QRectF b = hitFor(across, QStringLiteral("e1"));

	check(a.height() > a.width() || a.height() > 100.0, "time down the side makes a block tall");
	checkNear(b.width(), a.height(), 1.0, "the block's length along the time axis is the same either way");
	checkNear(b.height(), a.width(), 1.0, "and its thickness across the lane is too");

	/* The whole board is the transpose, so what was wide is now tall. */
	const CalendarRenderer renderer(&logoCache());
	const QSizeF downSize = renderer.measure(down, fixedNow());
	const QSizeF acrossSize = renderer.measure(across, fixedNow());
	check(acrossSize.width() > downSize.width(), "a timeline is wider than a day column board");
	check(acrossSize.height() < downSize.height(), "and shorter");
}

CT_SUITE(calendar_overlap, "Two events wanting one lane at one time")
{
	CalendarDocument document = basicBoard();
	document.grid.blockInset = 0.0;

	const QRectF alone = hitFor(document, QStringLiteral("e1"));

	/* A second event over the first, in the same lane. */
	CalendarEvent clash;
	clash.id = QStringLiteral("e4");
	clash.title = QStringLiteral("Clash");
	clash.dayId = QStringLiteral("d1");
	clash.laneId = QStringLiteral("l1");
	clash.startMinutes = 11 * 60;
	clash.endMinutes = 12 * 60;
	document.events.append(clash);

	/* The designer has to be told, whichever way the lane resolves it. */
	const QVector<OverlapReport> reports = document.overlaps();
	checkEq(reports.size(), 1, "the collision is reported once");

	document.lanes[0].overlap = LaneOverlap::Split;
	const QRectF split = hitFor(document, QStringLiteral("e1"));
	checkNear(split.width(), alone.width() / 2.0, 1.0, "splitting halves the lane between them");

	document.lanes[0].overlap = LaneOverlap::Stack;
	const QRectF stacked = hitFor(document, QStringLiteral("e1"));
	check(stacked.width() > split.width(), "stacking keeps them wider than a split would");
	check(stacked.left() >= alone.left(), "and the one underneath stays where it was");
}

CT_SUITE(calendar_open_ended, "An event that never says when it ends")
{
	CalendarDocument document = basicBoard();
	document.grid.blockInset = 0.0;
	document.grid.pixelsPerHour = 60.0;

	/* Take the end off the first event; the next one in the lane starts at 13:00. */
	document.events[0].endMinutes = -1;

	document.lanes[0].openEnded = OpenEndedRule::UntilNext;
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 180.0, 1.0,
		  "until-next runs to the next event in the lane");

	document.lanes[0].openEnded = OpenEndedRule::Fixed;
	document.lanes[0].openEndedMinutes = 45;
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 45.0, 1.0, "fixed uses the lane's own length");

	document.lanes[0].openEnded = OpenEndedRule::Minimal;
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 15.0, 1.0, "minimal is the smallest real span");

	/* And the last event in a lane has nothing after it, so it falls back to the lane's length. */
	document.events[1].endMinutes = -1;
	document.lanes[0].openEnded = OpenEndedRule::UntilNext;
	document.lanes[0].openEndedMinutes = 90;
	checkNear(hitFor(document, QStringLiteral("e2")).height(), 90.0, 1.0,
		  "the last event falls back rather than running to midnight");
}

CT_SUITE(calendar_slots, "A board whose axis is named waves rather than a clock")
{
	CalendarDocument document = basicBoard();
	document.axisMode = TimeAxisMode::Slots;
	document.grid.slotSize = 100.0;
	document.grid.blockInset = 0.0;

	for (int i = 0; i < 4; ++i) {
		CalendarSlot slot;
		slot.id = QStringLiteral("s%1").arg(i + 1);
		slot.name = QStringLiteral("WAVE %1").arg(QChar('A' + i));
		slot.startMinutes = (10 + i * 2) * 60;
		slot.endMinutes = (12 + i * 2) * 60;
		document.timeSlots.append(slot);
	}

	/* An event with no slot named but a clock time falls into the slot holding that time. */
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 100.0, 1.0, "a block takes one slot's room");

	/* A stated span of slots takes all of them, whatever the clock says. */
	document.events[0].slotId = QStringLiteral("s1");
	document.events[0].endSlotId = QStringLiteral("s3");
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 300.0, 1.0, "three slots is three slots tall");

	/* Slots of different weights size differently, which is what an over-running wave needs. */
	document.timeSlots[0].weight = 2.0;
	checkNear(hitFor(document, QStringLiteral("e1")).height(), 400.0, 1.0, "a double-weight slot is twice as long");
}

CT_SUITE(calendar_cascade, "Lane, then category, then status, then the event itself")
{
	CalendarDocument document = basicBoard();

	document.blockStyle.usePanel = true;
	document.blockStyle.panel.fill = BackgroundFill::Color;
	document.blockStyle.panel.color = QColor(10, 10, 10);

	/* Start with the event bound to nothing, so each layer can be added and seen in turn. */
	document.events[0].categoryId.clear();

	const auto colorOf = [&document] {
		return document.resolveBlockStyle(document.events[0], EventStatus::Upcoming).panel.color.name();
	};

	checkEq(colorOf(), QColor(10, 10, 10).name(), "with nothing else said, a block is the document's layer");

	/* The lane speaks over the document. */
	document.lanes[0].style.usePanel = true;
	document.lanes[0].style.panel.fill = BackgroundFill::Color;
	document.lanes[0].style.panel.color = QColor(20, 20, 20);
	checkEq(colorOf(), QColor(20, 20, 20).name(), "the lane speaks over the document");

	/* The category over the lane. */
	document.events[0].categoryId = QStringLiteral("c1");
	checkEq(colorOf(), QColor(40, 80, 160).name(), "the category speaks over the lane");

	/* The event's own layer over everything. */
	document.events[0].style.usePanel = true;
	document.events[0].style.panel.fill = BackgroundFill::Color;
	document.events[0].style.panel.color = QColor(90, 90, 90);
	checkEq(colorOf(), QColor(90, 90, 90).name(), "the event speaks over everything");

	/* Opacity multiplies down the cascade rather than replacing, so dimming compounds. */
	document.lanes[0].style.useOpacity = true;
	document.lanes[0].style.opacity = 0.5;
	document.finishedStyle.useOpacity = true;
	document.finishedStyle.opacity = 0.5;
	const ResolvedBlockStyle finished = document.resolveBlockStyle(document.events[0], EventStatus::Finished);
	checkNear(finished.opacity, 0.25, 0.001, "a half-present lane's finished event is a quarter present");
}

CT_SUITE(calendar_live, "What the clock does to a board")
{
	CalendarDocument document = basicBoard();
	const QDateTime now = fixedNow();

	/* 14:30 on the day: the first event is over, the second is running, nothing is upcoming in lane A. */
	checkEq(static_cast<int>(document.statusAt(document.events[0], now)), static_cast<int>(EventStatus::Finished),
		"an event that has ended reads as finished");
	checkEq(static_cast<int>(document.statusAt(document.events[1], now)), static_cast<int>(EventStatus::Live),
		"an event running now reads as live");

	/* A stated status is never overruled by the clock. */
	document.events[0].status = EventStatus::Cancelled;
	checkEq(static_cast<int>(document.statusAt(document.events[0], now)), static_cast<int>(EventStatus::Cancelled),
		"a stated status wins over the clock");
	document.events[0].status = EventStatus::Auto;

	/*
	 * Dropping the finished ones removes them from the board rather than restyling them. Two of the
	 * three are over by half past two -- the morning pool and the side bracket that ended at two.
	 */
	const int before = hitsOf(document).size();
	document.live.dropFinished = true;
	const int after = hitsOf(document).size();
	checkEq(after, before - 2, "dropping finished events removes exactly the finished ones");

	/* And the grace period keeps one that has only just ended. */
	document.live.dropGraceMinutes = 24 * 60;
	checkEq(hitsOf(document).size(), before, "a grace period keeps a recently finished event");
}

CT_SUITE(calendar_overflow, "A board that does not fit its canvas")
{
	CalendarDocument document = basicBoard();
	document.grid.pixelsPerHour = 400.0;
	document.height = 600;

	const CalendarRenderer renderer(&logoCache());

	document.overflow.mode = OverflowMode::Fit;
	CalendarBoard board = renderer.render(document, fixedNow());
	check(board.overflowed, "a board taller than its canvas says so");
	check(board.scale < 1.0, "fitting scales it down");
	checkEq(board.pages.size(), 1, "a fitted board is one page");
	checkEq(board.pages.first().tiles.first().image.width(), document.width, "and is canvas-sized");

	document.overflow.mode = OverflowMode::Scroll;
	board = renderer.render(document, fixedNow());
	checkEq(board.pages.size(), 1, "a scrolling board is one page");
	check(board.pages.first().height > document.height, "taller than the canvas, for the source to move");
	checkNear(board.scale, 1.0, 0.001, "and not scaled");

	/* Tiles have to be contiguous and cover the page exactly, as the strip's do. */
	const CalendarPage &page = board.pages.first();
	int expected = 0;
	for (const StripTile &tile : page.tiles) {
		checkEq(tile.top, expected, "tiles are contiguous");
		expected += tile.image.height();
	}
	checkEq(expected, page.height, "and cover the page exactly");

	document.overflow.mode = OverflowMode::Page;
	document.overflow.pageByDay = false;
	board = renderer.render(document, fixedNow());
	check(board.pages.size() > 1, "a paged board is several pages");
	for (const CalendarPage &each : board.pages)
		checkEq(each.height, document.height, "each of them canvas-sized");
}

CT_SUITE(calendar_up_next, "The list that answers what is next")
{
	CalendarDocument document = basicBoard();
	document.layout = CalendarLayout::UpNext;
	document.upNextCount = 5;

	/* At 14:30 only the afternoon pool is still running; the other two have been and gone. */
	checkEq(hitsOf(document).size(), 1, "a finished event is not what is next");

	document.upNextIncludesPast = true;
	checkEq(hitsOf(document).size(), 3, "unless the list is asked to show the past too");

	/* The count is a cap rather than a target. */
	document.upNextCount = 1;
	checkEq(hitsOf(document).size(), 1, "the count caps the list");

	/*
	 * A continuation is the same event carried over, so a list of what is next must not offer it
	 * twice -- the one place the link does more than draw a hatch.
	 */
	document.upNextCount = 10;
	document.upNextIncludesPast = true;
	CalendarEvent second = document.events[1];
	second.id = QStringLiteral("e2b");
	second.continuationOf = QStringLiteral("e2");
	second.startMinutes = 16 * 60;
	second.endMinutes = 17 * 60;
	document.events.append(second);
	checkEq(hitsOf(document).size(), 3, "a continuation is not a second entry in the list");
}

CT_SUITE(calendar_bands, "An event that belongs to no lane")
{
	CalendarDocument document = basicBoard();
	document.grid.blockInset = 0.0;

	CalendarEvent band;
	band.id = QStringLiteral("band");
	band.title = QStringLiteral("Doors Open");
	band.dayId = QStringLiteral("d1");
	band.band = true;
	band.startMinutes = 10 * 60;
	band.endMinutes = 10 * 60 + 30;
	document.events.append(band);

	const QRectF lane = hitFor(document, QStringLiteral("e1"));
	const QRectF across = hitFor(document, QStringLiteral("band"));

	check(across.width() > lane.width(), "a band is wider than any one lane");
	checkNear(across.width(),
		  document.lanes.size() * document.grid.laneSize + (document.lanes.size() - 1) * document.grid.laneGap,
		  1.0, "and exactly as wide as the lanes it crosses");

	/* A band takes no lane's room: the lane blocks are where they were before it existed. */
	CalendarDocument without = basicBoard();
	without.grid.blockInset = 0.0;
	checkNear(lane.left(), hitFor(without, QStringLiteral("e1")).left(), 0.5,
		  "a band does not move the lanes it crosses");
}

CT_SUITE(calendar_zones, "A schedule stitched from more than one clock")
{
	CalendarDocument document = basicBoard();
	document.zoneMode = ClockZoneMode::Fixed;
	document.timeZone = QStringLiteral("UTC");
	document.grid.pixelsPerHour = 60.0;
	document.grid.blockInset = 0.0;
	document.grid.axisStart = 0;
	document.grid.axisEnd = 24 * 60;

	const QRectF plain = hitFor(document, QStringLiteral("e1"));

	/* The same wall-clock time, stated in a zone three hours behind, lands three hours later. */
	document.events[0].timeZone = QStringLiteral("Etc/GMT+3");
	const QRectF shifted = hitFor(document, QStringLiteral("e1"));
	checkNear(shifted.top() - plain.top(), 180.0, 1.0,
		  "an event in a zone three hours behind lands three hours later");

	/* An unknown zone is ignored rather than throwing the event somewhere arbitrary. */
	document.events[0].timeZone = QStringLiteral("Not/AZone");
	checkNear(hitFor(document, QStringLiteral("e1")).top(), plain.top(), 1.0, "an unreadable zone is ignored");
}

CT_SUITE(calendar_presets, "Every preset lays out and draws")
{
	const CalendarRenderer renderer(&logoCache());

	for (const CalendarPresetInfo &info : allCalendarPresets()) {
		Context context(QString::fromLatin1(info.id));

		CalendarDocument document;
		check(applyCalendarPreset(QString::fromLatin1(info.id), &document, true),
		      "the preset is known and applied");

		check(!document.events.isEmpty(), "it comes with a sample schedule");
		check(!document.days.isEmpty(), "and a day to hang it on");

		const QSizeF size = renderer.measure(document, fixedNow());
		check(size.width() > 1.0 && size.height() > 1.0, "it measures to something");

		const CalendarBoard board = renderer.render(document, fixedNow());
		check(!board.isEmpty(), "it renders");
		checkEq(board.placedEvents, document.events.size(), "every sample event is placed");

		/* Composed the way the source composes it: the page, then the free layer over it. */
		QImage picture(board.width, board.height, QImage::Format_ARGB32);
		picture.fill(Qt::transparent);
		{
			QPainter painter(&picture);
			for (const StripTile &tile : board.pages.first().tiles)
				painter.drawImage(QPointF(0.0, tile.top), tile.image);
			if (!board.overlay.isNull())
				painter.drawImage(QPointF(0.0, 0.0), board.overlay);
		}
		saveArtifact(QStringLiteral("calendar-%1").arg(QString::fromLatin1(info.id)), picture);

		/* Applying a preset without the sample leaves whatever schedule is already there. */
		CalendarDocument restyled = document;
		check(applyCalendarPreset(QString::fromLatin1(info.id), &restyled, false),
		      "the preset applies again without its sample");
		checkEq(restyled.events.size(), document.events.size(), "and leaves the schedule alone");
	}
}

CT_SUITE(calendar_persistence, "A board survives a round trip through obs_data")
{
	CalendarDocument original;
	applyCalendarPreset(QStringLiteral("wave_grid"), &original, true);

	/* Push a few things off their defaults, including one of each awkward kind. */
	original.zoneMode = ClockZoneMode::Fixed;
	original.timeZone = QStringLiteral("America/New_York");
	original.live.nowLine = true;
	original.live.nowLineLabel = QStringLiteral("NOW");
	original.overflow.mode = OverflowMode::Page;
	original.events[0].tags.append(ChannelTag{QStringLiteral("Main"), TagGlyph::Twitch, QString()});
	original.events[0].notes = QStringLiteral("a note with \" a quote and a \n newline");
	original.events[1].continuationOf = original.events[0].id;

	CalendarElement ticker = CalendarElement::makeDefault(ElementType::Ticker);
	ticker.messages = QStringList{QStringLiteral("one"), QStringLiteral("two\nwith a break")};
	original.elements.append(ticker);

	OBSDataAutoRelease data = obs_data_create();
	original.save(data);

	CalendarDocument loaded;
	loaded.load(data);

	checkEq(loaded.events.size(), original.events.size(), "every event came back");
	checkEq(loaded.timeSlots.size(), original.timeSlots.size(), "every slot came back");
	checkEq(loaded.lanes.size(), original.lanes.size(), "every lane came back");
	checkEq(loaded.elements.size(), original.elements.size(), "every element came back");
	checkEq(loaded.timeZone, original.timeZone, "the zone came back");
	checkEq(static_cast<int>(loaded.overflow.mode), static_cast<int>(original.overflow.mode),
		"the overflow mode came back");
	checkEq(loaded.live.nowLineLabel, original.live.nowLineLabel, "the now-line label came back");
	checkEq(loaded.events[0].notes, original.events[0].notes, "a note with punctuation came back intact");
	checkEq(loaded.events[1].continuationOf, original.events[1].continuationOf, "the continuation link came back");
	checkEq(loaded.elements.last().messages.size(), 2, "both ticker messages came back");
	checkEq(loaded.elements.last().messages.last(), ticker.messages.last(),
		"including the one with a line break in it");

	checkEq(static_cast<int>(loaded.events[0].tags.size()), 1, "the tag came back");
	checkEq(static_cast<int>(loaded.events[0].tags[0].glyph), static_cast<int>(TagGlyph::Twitch),
		"and its glyph with it");

	/* And the whole thing again through JSON, which is what the designer's export writes. */
	CalendarDocument fromJson;
	check(fromJson.fromJson(original.toJson()), "the JSON parses");
	checkEq(fromJson.events.size(), original.events.size(), "and holds the same schedule");
}

CT_SUITE(calendar_days, "Days, and what happens to an event without one")
{
	CalendarDocument document;
	check(document.days.isEmpty(), "a fresh document has no days");

	CalendarEvent orphan;
	orphan.id = QStringLiteral("e1");
	orphan.title = QStringLiteral("Orphan");
	document.events.append(orphan);

	check(document.syncDays(), "syncing makes a day and adopts the orphan");
	checkEq(document.days.size(), 1, "exactly one day is made");
	checkEq(document.events[0].dayId, document.days[0].id, "and the event names it");
	check(!document.syncDays(), "syncing again changes nothing");

	/* A day is never removed by syncing, even with nothing on it. */
	CalendarDay spare;
	spare.id = QStringLiteral("spare");
	document.days.append(spare);
	check(!document.syncDays(), "an empty day is left alone");
	checkEq(document.days.size(), 2, "and still there");
}
