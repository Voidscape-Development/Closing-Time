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

#include "model/CalendarPresets.hpp"

#include <QDate>

namespace closingtime {

namespace {

/*
 * The palette every preset is dressed in.
 *
 * One set of colors rather than five, because a board is meant to be recolored to whatever it is
 * announcing and the presets exist to show its *shape*. Five palettes would mean four of them being
 * thrown away on first use, and a house style that had to be built five times to cover them.
 */
const QColor kInk = QColor(244, 246, 251);
const QColor kMutedInk = QColor(160, 172, 195);
const QColor kSurface = QColor(22, 26, 38, 235);
const QColor kRule = QColor(255, 255, 255, 34);

const QColor kAccentA = QColor(58, 110, 200);
const QColor kAccentB = QColor(154, 82, 196);
const QColor kAccentC = QColor(196, 106, 62);
const QColor kAccentD = QColor(58, 150, 128);

TextStyle textStyle(int size, bool bold, const QColor &color, HAlign align = HAlign::Left)
{
	TextStyle style;
	style.pixelSize = size;
	style.bold = bold;
	style.color = color;
	style.align = align;
	return style;
}

BackgroundPanel panel(const QColor &color, double radius = 6.0)
{
	BackgroundPanel result;
	result.fill = BackgroundFill::Color;
	result.color = color;
	result.setRadius(radius);
	return result;
}

/* A layer that says nothing but a color, which is what a category is. */
BlockStyle colorLayer(const QColor &color, double radius = 6.0)
{
	BlockStyle layer;
	layer.usePanel = true;
	layer.panel = panel(color, radius);
	return layer;
}

/* The one place the shared look is set: the base layer every preset starts a block from. */
void applyCommonStyling(CalendarDocument *document)
{
	document->background = QColor(0, 0, 0, 0);

	BlockStyle base;
	base.usePanel = true;
	base.panel = panel(kSurface);
	base.useTitleStyle = true;
	base.titleStyle = textStyle(22, true, kInk);
	base.useSubtitleStyle = true;
	base.subtitleStyle = textStyle(17, false, kInk.darker(115));
	base.useMetaStyle = true;
	base.metaStyle = textStyle(14, false, kMutedInk);
	base.useFit = true;
	base.fit = TextFit::Shrink;
	base.minPixelSize = 11;
	document->blockStyle = base;

	/*
	 * The status layers are deliberately thin: an opacity for what is over, a border for what is
	 * running. A preset that recolored each status would be overriding the category colors that are
	 * the whole point of the board.
	 */
	BlockStyle finished;
	finished.useOpacity = true;
	finished.opacity = 0.45;
	document->finishedStyle = finished;

	BlockStyle cancelled;
	cancelled.useOpacity = true;
	cancelled.opacity = 0.35;
	cancelled.useHatch = true;
	cancelled.hatch.pattern = HatchPattern::Diagonal;
	cancelled.hatch.color = QColor(255, 255, 255, 60);
	document->cancelledStyle = cancelled;

	/* Continuation: the same block, hatched, so a reader sees it is not a new thing starting. */
	BlockStyle continuation;
	continuation.useHatch = true;
	continuation.hatch.pattern = HatchPattern::Diagonal;
	continuation.hatch.color = QColor(255, 255, 255, 45);
	continuation.hatch.spacing = 14.0;
	document->continuationStyle = continuation;

	BlockStyle current;
	current.usePanel = true;
	current.panel = panel(kSurface);
	current.panel.border.enabled = true;
	current.panel.border.width = 3.0;
	current.panel.border.color = QColor(255, 214, 102);
	document->live.currentStyle = current;

	document->grid.gutterStyle = textStyle(20, true, kInk, HAlign::Center);
	document->grid.gutterPanel = panel(QColor(255, 255, 255, 16), 4.0);
	document->grid.laneStyle = textStyle(20, true, kInk, HAlign::Center);
	document->grid.lanePanel = panel(QColor(255, 255, 255, 22), 6.0);
	document->grid.dayStyle = textStyle(34, true, kInk, HAlign::Center);
	document->grid.dayPanel = panel(QColor(0, 0, 0, 150), 8.0);

	document->grid.timeLineColor = kRule;
	document->grid.laneLineColor = QColor(255, 255, 255, 20);
}

CalendarElement clockElement()
{
	CalendarElement element = CalendarElement::makeDefault(ElementType::Clock);
	element.label = QStringLiteral("Local clock");
	element.anchor = ElementAnchor::TopRight;
	element.x = 40.0;
	element.y = 24.0;
	element.width = 220.0;
	element.text.clear();
	element.clockZoneLabel = QStringLiteral("LOCAL TIME");
	element.textStyle = textStyle(40, true, kInk, HAlign::Right);
	return element;
}

CalendarElement legendElement()
{
	CalendarElement element = CalendarElement::makeDefault(ElementType::Legend);
	element.label = QStringLiteral("Category legend");
	element.anchor = ElementAnchor::BottomLeft;
	element.x = 40.0;
	element.y = 24.0;
	element.width = 720.0;
	element.legendColumns = 4;
	element.textStyle = textStyle(17, true, kInk);
	return element;
}

CalendarElement tickerElement(const QString &message)
{
	CalendarElement element = CalendarElement::makeDefault(ElementType::Ticker);
	element.label = QStringLiteral("Ticker");
	element.anchor = ElementAnchor::BottomCenter;
	element.y = 24.0;
	element.width = 1400.0;
	element.height = 56.0;
	element.messages = QStringList{message};
	element.textStyle = textStyle(26, true, kInk, HAlign::Center);
	element.panel = panel(QColor(0, 0, 0, 170), 8.0);
	return element;
}

/* --- sample content ----------------------------------------------------------------------- */

CalendarCategory category(const QString &id, const QString &name, const QColor &color)
{
	CalendarCategory result;
	result.id = id;
	result.name = name;
	result.style = colorLayer(color);
	return result;
}

CalendarLane lane(const QString &id, const QString &name, const QString &sub = QString())
{
	CalendarLane result;
	result.id = id;
	result.name = name;
	result.subLabel = sub;
	return result;
}

CalendarDay day(const QString &id, int dayOffset)
{
	CalendarDay result;
	result.id = id;
	result.date = QDate::currentDate().addDays(dayOffset);
	return result;
}

CalendarEvent event(const QString &id, const QString &title, const QString &dayId, const QString &laneId,
		    const QString &categoryId, int start, int end)
{
	CalendarEvent result;
	result.id = id;
	result.title = title;
	result.dayId = dayId;
	result.laneId = laneId;
	result.categoryId = categoryId;
	result.startMinutes = start;
	result.endMinutes = end;
	return result;
}

void clearSchedule(CalendarDocument *document)
{
	document->days.clear();
	document->lanes.clear();
	document->timeSlots.clear();
	document->categories.clear();
	document->events.clear();
}

/* The three categories every sample board is colored by. */
void sampleCategories(CalendarDocument *document)
{
	document->categories = {
		category(QStringLiteral("cat_pools"), QStringLiteral("Pools"), kAccentA),
		category(QStringLiteral("cat_top"), QStringLiteral("Top Cut"), kAccentB),
		category(QStringLiteral("cat_finals"), QStringLiteral("Finals"), kAccentC),
		category(QStringLiteral("cat_special"), QStringLiteral("Special"), kAccentD),
	};
}

/* --- the presets -------------------------------------------------------------------------- */

void presetUpNext(CalendarDocument *document, bool sample)
{
	document->layout = CalendarLayout::UpNext;
	document->axisMode = TimeAxisMode::Clock;
	document->marginX = 120.0;
	document->marginY = 140.0;

	document->upNextCount = 5;
	document->upNextRowHeight = 108.0;
	document->upNextRowGap = 14.0;
	document->upNextTimeWidth = 240.0;
	document->upNextIncludesPast = false;

	/* The one board that is *about* the clock, so it starts with the clock features already on. */
	document->live.dropFinished = true;
	document->live.highlightCurrent = true;
	document->live.refreshSeconds = 30;

	document->blockStyle.panel = panel(kAccentA, 6.0);
	document->blockStyle.titleStyle = textStyle(38, true, kInk);
	document->blockStyle.subtitleStyle = textStyle(24, false, kInk.darker(110));

	document->overflow.mode = OverflowMode::Fit;

	document->elements = {clockElement()};

	if (!sample)
		return;

	clearSchedule(document);
	sampleCategories(document);
	document->days = {day(QStringLiteral("day1"), 0)};
	document->lanes = {lane(QStringLiteral("main"), QStringLiteral("Main Stage"))};
	document->events = {
		event(QStringLiteral("e1"), QStringLiteral("Round 1 Pools"), QStringLiteral("day1"),
		      QStringLiteral("main"), QStringLiteral("cat_pools"), 11 * 60, 13 * 60),
		event(QStringLiteral("e2"), QStringLiteral("Round 2 Pools"), QStringLiteral("day1"),
		      QStringLiteral("main"), QStringLiteral("cat_pools"), 13 * 60, 15 * 60),
		event(QStringLiteral("e3"), QStringLiteral("Top 24"), QStringLiteral("day1"), QStringLiteral("main"),
		      QStringLiteral("cat_top"), 15 * 60, 17 * 60),
		event(QStringLiteral("e4"), QStringLiteral("Top 8"), QStringLiteral("day1"), QStringLiteral("main"),
		      QStringLiteral("cat_finals"), 17 * 60, 19 * 60),
		event(QStringLiteral("e5"), QStringLiteral("Grand Finals"), QStringLiteral("day1"),
		      QStringLiteral("main"), QStringLiteral("cat_finals"), 19 * 60, 20 * 60),
	};
}

void presetDayColumns(CalendarDocument *document, bool sample)
{
	document->layout = CalendarLayout::Grid;
	document->axisMode = TimeAxisMode::Clock;
	document->orientation = GridOrientation::TimeDown;
	document->marginX = 30.0;
	document->marginY = 30.0;

	document->grid.daysAsColumns = true;
	document->grid.gutter = GutterSide::Leading;
	document->grid.gutterWidth = 96.0;
	document->grid.gutterStep = 60;
	document->grid.pixelsPerHour = 72.0;
	document->grid.laneSize = 260.0;
	document->grid.laneGap = 4.0;
	document->grid.dayGap = 20.0;
	document->grid.dayHeaderHeight = 76.0;
	document->grid.laneHeaderSize = 0.0;
	document->grid.showLaneHeaders = false;
	document->grid.showBlockTimes = false;

	document->overflow.mode = OverflowMode::Fit;

	/*
	 * The clock goes to the bottom corner on this board rather than the top: the day headings run
	 * the whole width across the top, and a clock in that corner would be sitting on the last one.
	 */
	CalendarElement clock = clockElement();
	clock.anchor = ElementAnchor::BottomRight;
	clock.y = 96.0;
	document->elements = {tickerElement(QStringLiteral("Welcome in")), clock};

	if (!sample)
		return;

	clearSchedule(document);
	sampleCategories(document);

	document->days = {day(QStringLiteral("day1"), 0), day(QStringLiteral("day2"), 1),
			  day(QStringLiteral("day3"), 2)};
	document->lanes = {lane(QStringLiteral("stream_a"), QStringLiteral("Stream A")),
			   lane(QStringLiteral("stream_b"), QStringLiteral("Stream B"))};

	document->events = {
		event(QStringLiteral("e1"), QStringLiteral("Doors Open"), QStringLiteral("day1"), QString(),
		      QStringLiteral("cat_special"), 10 * 60, 11 * 60),
		event(QStringLiteral("e2"), QStringLiteral("Pools"), QStringLiteral("day1"), QStringLiteral("stream_a"),
		      QStringLiteral("cat_pools"), 11 * 60, 15 * 60),
		event(QStringLiteral("e3"), QStringLiteral("Side Bracket"), QStringLiteral("day1"),
		      QStringLiteral("stream_b"), QStringLiteral("cat_pools"), 12 * 60, 16 * 60),
		event(QStringLiteral("e4"), QStringLiteral("Top 24"), QStringLiteral("day2"),
		      QStringLiteral("stream_a"), QStringLiteral("cat_top"), 11 * 60, 15 * 60),
		event(QStringLiteral("e5"), QStringLiteral("Doubles Top 8"), QStringLiteral("day2"),
		      QStringLiteral("stream_b"), QStringLiteral("cat_top"), 13 * 60, 17 * 60),
		event(QStringLiteral("e6"), QStringLiteral("Top 8"), QStringLiteral("day3"), QStringLiteral("stream_a"),
		      QStringLiteral("cat_finals"), 12 * 60, 16 * 60),
	};

	/* The first one is a band, which is what a full-width DOORS OPEN really is. */
	document->events[0].band = true;
}

void presetChannelTimeline(CalendarDocument *document, bool sample)
{
	document->layout = CalendarLayout::Grid;
	document->axisMode = TimeAxisMode::Clock;
	document->orientation = GridOrientation::TimeAcross;
	document->marginX = 30.0;
	document->marginY = 30.0;

	document->grid.daysAsColumns = false;
	document->grid.gutter = GutterSide::Leading;
	document->grid.gutterWidth = 44.0;
	document->grid.gutterStep = 60;
	document->grid.pixelsPerHour = 130.0;
	document->grid.laneSize = 62.0;
	document->grid.laneGap = 4.0;
	document->grid.laneHeaderSize = 220.0;
	document->grid.showLaneHeaders = true;
	document->grid.laneStyle.align = HAlign::Left;
	document->grid.dayHeaderHeight = 62.0;
	document->grid.showBlockTimes = false;
	document->grid.blockPaddingX = 12.0;
	document->grid.blockPaddingY = 6.0;

	document->blockStyle.titleStyle = textStyle(19, true, kInk);
	document->blockStyle.subtitleStyle = textStyle(15, false, kInk.darker(115));

	document->overflow.mode = OverflowMode::Fit;

	document->elements = {legendElement()};

	if (!sample)
		return;

	clearSchedule(document);
	sampleCategories(document);

	document->days = {day(QStringLiteral("day1"), 0), day(QStringLiteral("day2"), 1)};
	document->lanes = {
		lane(QStringLiteral("ch1"), QStringLiteral("Channel One"), QStringLiteral("Main")),
		lane(QStringLiteral("ch2"), QStringLiteral("Channel Two"), QStringLiteral("Secondary")),
		lane(QStringLiteral("ch3"), QStringLiteral("Channel Three"), QStringLiteral("Side events")),
		lane(QStringLiteral("ch4"), QStringLiteral("Channel Four"), QStringLiteral("Overflow")),
	};

	document->events = {
		event(QStringLiteral("e1"), QStringLiteral("Pools Round 1"), QStringLiteral("day1"),
		      QStringLiteral("ch1"), QStringLiteral("cat_pools"), 10 * 60, 14 * 60),
		event(QStringLiteral("e2"), QStringLiteral("Pools Round 1"), QStringLiteral("day1"),
		      QStringLiteral("ch2"), QStringLiteral("cat_pools"), 10 * 60, 13 * 60),
		event(QStringLiteral("e3"), QStringLiteral("Top 24"), QStringLiteral("day1"), QStringLiteral("ch2"),
		      QStringLiteral("cat_top"), 13 * 60, 16 * 60),
		event(QStringLiteral("e4"), QStringLiteral("Showcase"), QStringLiteral("day1"), QStringLiteral("ch3"),
		      QStringLiteral("cat_special"), 11 * 60, 13 * 60),
		event(QStringLiteral("e5"), QStringLiteral("Pools Round 2"), QStringLiteral("day1"),
		      QStringLiteral("ch4"), QStringLiteral("cat_pools"), 12 * 60, 17 * 60),
		event(QStringLiteral("e6"), QStringLiteral("Top 8"), QStringLiteral("day2"), QStringLiteral("ch1"),
		      QStringLiteral("cat_finals"), 11 * 60, 15 * 60),
	};
}

void presetWaveGrid(CalendarDocument *document, bool sample)
{
	document->layout = CalendarLayout::Grid;
	document->axisMode = TimeAxisMode::Slots;
	document->orientation = GridOrientation::TimeDown;
	document->marginX = 24.0;
	document->marginY = 24.0;

	document->grid.daysAsColumns = false;
	document->grid.gutter = GutterSide::Both;
	document->grid.gutterWidth = 92.0;
	document->grid.gutterShowSlotTimes = true;
	document->grid.slotSize = 92.0;
	document->grid.laneSize = 128.0;
	document->grid.laneGap = 3.0;
	document->grid.laneHeaderSize = 70.0;
	document->grid.showLaneHeaders = true;
	document->grid.dayHeaderHeight = 70.0;
	document->grid.showBlockTimes = false;
	document->grid.showBlockSubtitles = false;
	document->grid.blockPaddingX = 6.0;
	document->grid.blockPaddingY = 4.0;

	document->blockStyle.titleStyle = textStyle(16, true, kInk, HAlign::Center);
	document->blockStyle.panel = panel(kSurface, 3.0);

	document->overflow.mode = OverflowMode::Fit;

	document->elements = {legendElement()};

	if (!sample)
		return;

	clearSchedule(document);
	sampleCategories(document);

	document->days = {day(QStringLiteral("day1"), 0)};

	/* Named bands that also carry a time, which is what lets the gutter print both. */
	const char *names[] = {"WAVE A", "WAVE B", "WAVE C", "WAVE D", "WAVE E", "WAVE F"};
	for (int i = 0; i < 6; ++i) {
		CalendarSlot slot;
		slot.id = QStringLiteral("slot%1").arg(i + 1);
		slot.name = QString::fromLatin1(names[i]);
		slot.startMinutes = (10 + i * 2) * 60;
		slot.endMinutes = (12 + i * 2) * 60;
		document->timeSlots.append(slot);
	}

	document->lanes = {
		lane(QStringLiteral("g1"), QStringLiteral("Game One")),
		lane(QStringLiteral("g2"), QStringLiteral("Game Two")),
		lane(QStringLiteral("g3"), QStringLiteral("Game Three")),
		lane(QStringLiteral("g4"), QStringLiteral("Game Four")),
		lane(QStringLiteral("g5"), QStringLiteral("Game Five")),
		lane(QStringLiteral("g6"), QStringLiteral("Game Six")),
	};

	const struct {
		const char *id;
		const char *title;
		const char *lane;
		const char *category;
		int startSlot;
		int endSlot;
	} rows[] = {
		{"e1", "Pools", "g1", "cat_pools", 0, 1},  {"e2", "Pools", "g2", "cat_pools", 0, 0},
		{"e3", "Top 24", "g2", "cat_top", 1, 2},   {"e4", "Pools", "g3", "cat_pools", 1, 2},
		{"e5", "Top 8", "g1", "cat_finals", 3, 4}, {"e6", "Pools", "g4", "cat_pools", 2, 3},
		{"e7", "Top 12", "g5", "cat_top", 3, 4},   {"e8", "Bracket", "g6", "cat_pools", 0, 2},
	};

	for (const auto &row : rows) {
		CalendarEvent item;
		item.id = QString::fromLatin1(row.id);
		item.title = QString::fromLatin1(row.title);
		item.dayId = QStringLiteral("day1");
		item.laneId = QString::fromLatin1(row.lane);
		item.categoryId = QString::fromLatin1(row.category);
		item.slotId = QStringLiteral("slot%1").arg(row.startSlot + 1);
		item.endSlotId = QStringLiteral("slot%1").arg(row.endSlot + 1);
		document->events.append(item);
	}
}

void presetStacked(CalendarDocument *document, bool sample)
{
	document->layout = CalendarLayout::Stacked;
	document->axisMode = TimeAxisMode::Clock;
	document->marginX = 40.0;
	document->marginY = 40.0;

	document->grid.laneSize = 300.0;
	document->grid.laneGap = 16.0;
	document->grid.laneHeaderSize = 70.0;
	document->grid.showLaneHeaders = true;
	document->grid.dayHeaderHeight = 76.0;
	document->grid.pixelsPerHour = 110.0;
	document->grid.showBlockTimes = true;
	document->grid.blockPaddingX = 14.0;
	document->grid.blockPaddingY = 10.0;

	/* The time is the block's own heading here, since there is no axis to read it off. */
	document->blockStyle.metaStyle = textStyle(20, true, QColor(255, 214, 102));
	document->blockStyle.titleStyle = textStyle(22, true, kInk);

	document->overflow.mode = OverflowMode::Fit;

	document->elements = {clockElement()};

	if (!sample)
		return;

	clearSchedule(document);
	sampleCategories(document);

	document->days = {day(QStringLiteral("day1"), 0)};
	document->lanes = {
		lane(QStringLiteral("s1"), QStringLiteral("Stage One")),
		lane(QStringLiteral("s2"), QStringLiteral("Stage Two")),
		lane(QStringLiteral("s3"), QStringLiteral("Stage Three")),
	};

	document->events = {
		event(QStringLiteral("e1"), QStringLiteral("Opening"), QStringLiteral("day1"), QStringLiteral("s1"),
		      QStringLiteral("cat_special"), 11 * 60, 12 * 60),
		event(QStringLiteral("e2"), QStringLiteral("Pools"), QStringLiteral("day1"), QStringLiteral("s1"),
		      QStringLiteral("cat_pools"), 13 * 60, 16 * 60),
		event(QStringLiteral("e3"), QStringLiteral("Top 8"), QStringLiteral("day1"), QStringLiteral("s1"),
		      QStringLiteral("cat_finals"), 18 * 60, 20 * 60),
		event(QStringLiteral("e4"), QStringLiteral("Side Bracket"), QStringLiteral("day1"),
		      QStringLiteral("s2"), QStringLiteral("cat_pools"), 12 * 60, 15 * 60),
		event(QStringLiteral("e5"), QStringLiteral("Exhibition"), QStringLiteral("day1"), QStringLiteral("s2"),
		      QStringLiteral("cat_special"), 17 * 60, 18 * 60),
		event(QStringLiteral("e6"), QStringLiteral("Workshop"), QStringLiteral("day1"), QStringLiteral("s3"),
		      QStringLiteral("cat_special"), 14 * 60, 16 * 60),
	};
}

const CalendarPresetInfo kPresets[] = {
	{"up_next_panel", "Up Next Panel", "A short list of what is coming, with a clock. Fills a holding screen."},
	{"day_column_board", "Day Column Board",
	 "Hours down the side, a column per day, blocks spanning the hours they run."},
	{"channel_timeline", "Channel Timeline",
	 "A row per channel or stage with time running across, colored by category."},
	{"wave_grid", "Wave Grid", "Named waves down the side against a column per game. The dense poster board."},
	{"stacked_blocks", "Stacked Blocks",
	 "Columns of blocks in the order they happen, each printing its own start time."},
};

} // namespace

const QVector<CalendarPresetInfo> &allCalendarPresets()
{
	static const QVector<CalendarPresetInfo> presets = [] {
		QVector<CalendarPresetInfo> result;
		for (const auto &preset : kPresets)
			result.append(preset);
		return result;
	}();
	return presets;
}

bool applyCalendarPreset(const QString &id, CalendarDocument *document, bool includeSample)
{
	if (!document)
		return false;

	/*
	 * The shared look is applied first and the preset then departs from it, rather than each preset
	 * setting every field. That is what keeps the five of them recognizably one family, and what
	 * means a change to the family is one edit.
	 */
	applyCommonStyling(document);

	if (id == QLatin1String("up_next_panel"))
		presetUpNext(document, includeSample);
	else if (id == QLatin1String("day_column_board"))
		presetDayColumns(document, includeSample);
	else if (id == QLatin1String("channel_timeline"))
		presetChannelTimeline(document, includeSample);
	else if (id == QLatin1String("wave_grid"))
		presetWaveGrid(document, includeSample);
	else if (id == QLatin1String("stacked_blocks"))
		presetStacked(document, includeSample);
	else
		return false;

	document->syncDays();
	return true;
}

} // namespace closingtime
