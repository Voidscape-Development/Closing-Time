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

#include <obs.h>

#include <QColor>
#include <QDate>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QVector>

#include "model/Background.hpp"
#include "model/CreditsModel.hpp"
#include "model/FontBundle.hpp"
#include "model/TagGlyph.hpp"

namespace closingtime {

/*
 * The calendar half of the plugin.
 *
 * A credit roll is a column of content that scrolls; a schedule board is a grid of content that
 * holds. They share everything below the layout -- typefaces, panels, gradients, logos, the font
 * bundle, the machine-wide style library -- and nothing above it, which is why this is a second
 * document and a second source rather than a twentieth section type.
 *
 * Everything here persists by string id, never by ordinal, exactly as the roll's model does: the
 * enums may be reordered freely and only the id strings are contractual.
 */

/* --- layout ------------------------------------------------------------------------------- */

/*
 * The shape the board is laid out in.
 *
 *   UpNext  - a list of rows, each a time and what happens at it. No axis, no lanes: the board
 *             answers "what is next" rather than "what is the day". The one a stream runs every
 *             week, and the only layout that reads as finished with three events on it.
 *   Grid    - blocks positioned on a time axis against lanes. Both the day-column board and the
 *             channel timeline are this, differing only in which way round the axes go; see
 *             `GridOrientation`.
 *   Stacked - lanes as columns of blocks in the order they happen, each block printing its own
 *             time. No axis to read against, so an hour of nothing costs no space and a schedule
 *             with long gaps stays dense.
 *
 * Three rather than the five presets the designer offers, because a wave grid is a grid whose axis
 * is slotted (see `TimeAxisMode`) and a channel timeline is a grid turned on its side. A preset is
 * a set of answers to these questions; it is not a layout of its own, and making it one would mean
 * a setting improved in one of them staying broken in the other four.
 */
enum class CalendarLayout { UpNext, Grid, Stacked };

const char *calendarLayoutId(CalendarLayout layout);
CalendarLayout calendarLayoutFromId(const char *id, CalendarLayout fallback = CalendarLayout::Grid);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *calendarLayoutName(CalendarLayout layout);

/* Every layout in the order the designer's picker should list them. */
const QVector<CalendarLayout> &allCalendarLayouts();

/*
 * What the time axis measures.
 *
 *   Clock - real time, proportionally: a four-hour block is four times the height of a one-hour
 *           block, and an hour with nothing in it still takes an hour's worth of room.
 *   Slots - named bands of equal size -- WAVE C, row D -- in the order they are listed. A slot may
 *           carry a real time as well (see `CalendarSlot`), which is what lets the gutter print
 *           both and the now-marker work on a board whose axis is not a clock.
 *
 * Both, rather than one, because the schedules this is for are published both ways and neither can
 * be expressed as the other. A wave is not a duration -- it is over when it is over -- and a board
 * built from waves that quietly sized them by their nominal length would misdraw every schedule it
 * was handed.
 */
enum class TimeAxisMode { Clock, Slots };

const char *timeAxisModeId(TimeAxisMode mode);
TimeAxisMode timeAxisModeFromId(const char *id, TimeAxisMode fallback = TimeAxisMode::Clock);

/*
 * Which way round a grid's two axes go.
 *
 *   TimeDown   - time runs down the rows, lanes across the columns. The day-column board.
 *   TimeAcross - lanes run down the rows, time across the columns. The channel timeline.
 *
 * A switch rather than two layouts because the two produce the same blocks from the same data and
 * differ only in which coordinate each measurement lands on. Everything the layout does -- overlap,
 * bands, continuation, the now-marker -- has to work both ways round, and it does that by being
 * written once against a major and a minor axis rather than twice against x and y.
 */
enum class GridOrientation { TimeDown, TimeAcross };

const char *gridOrientationId(GridOrientation orientation);
GridOrientation gridOrientationFromId(const char *id, GridOrientation fallback = GridOrientation::TimeDown);

/*
 * What the board does when its content does not fit the canvas.
 *
 *   Fit    - scale the whole board down until it lands. A still board's answer, and the one that
 *            keeps every event visible at once.
 *   Page   - cut the board into canvas-sized pages and cycle through them on a dwell timer.
 *   Scroll - lay the board out at full size and move it slowly past the canvas.
 *
 * One setting rather than three switches: they are three answers to one question, and a board that
 * was scaling *and* paging would be answering it twice.
 */
enum class OverflowMode { Fit, Page, Scroll };

const char *overflowModeId(OverflowMode mode);
OverflowMode overflowModeFromId(const char *id, OverflowMode fallback = OverflowMode::Fit);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *overflowModeName(OverflowMode mode);
const QVector<OverflowMode> &allOverflowModes();

/*
 * What a lane does when two of its events want the same time.
 *
 *   Split - the lane is divided across its minor axis and the colliding events sit side by side,
 *           each taking its share. What a printed schedule does when two games share a stream.
 *   Stack - the later event is drawn over the earlier one, offset by a few pixels, so the collision
 *           is visible rather than hidden.
 *
 * A choice rather than a rule because both are real: a lane that genuinely runs two things at once
 * is split, and a lane where a collision means somebody mistyped a time wants to be shown that.
 * Either way the designer says a collision happened -- see `CalendarDocument::overlaps` -- because
 * the one outcome nobody wants is a board that silently drew one event on top of another.
 */
enum class LaneOverlap { Split, Stack };

const char *laneOverlapId(LaneOverlap overlap);
LaneOverlap laneOverlapFromId(const char *id, LaneOverlap fallback = LaneOverlap::Split);

/*
 * How long an event with no end time lasts.
 *
 *   Fixed     - the lane's `openEndedMinutes`, whatever else is around it.
 *   UntilNext - until the next event in the same lane begins, falling back to `openEndedMinutes`
 *               when it is the last one. A published schedule that prints only start times means
 *               exactly this, which is why it is the default.
 *   Minimal   - just tall enough to hold its own text, so a lane of short announcements does not
 *               become a column of identical hour-tall boxes.
 */
enum class OpenEndedRule { Fixed, UntilNext, Minimal };

const char *openEndedRuleId(OpenEndedRule rule);
OpenEndedRule openEndedRuleFromId(const char *id, OpenEndedRule fallback = OpenEndedRule::UntilNext);

/* Which side of a grid carries the time gutter. Both is what a wide poster does. */
enum class GutterSide { Leading, Trailing, Both, None };

const char *gutterSideId(GutterSide side);
GutterSide gutterSideFromId(const char *id, GutterSide fallback = GutterSide::Leading);

/*
 * Where an event is in its life.
 *
 * `Auto` is the ordinary case and the only one that moves on its own: the clock decides, so a board
 * left running all day works its way through Upcoming, Live and Finished without anybody touching
 * it. The other four are stated, for the board that is not being driven by the clock at all, and
 * for `Cancelled`, which no clock can ever infer.
 */
enum class EventStatus { Auto, Upcoming, Live, Finished, Cancelled };

const char *eventStatusId(EventStatus status);
EventStatus eventStatusFromId(const char *id, EventStatus fallback = EventStatus::Auto);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *eventStatusName(EventStatus status);
const QVector<EventStatus> &allEventStatuses();

/*
 * Whether the document's times are read in this machine's zone or in one it names.
 *
 *   Automatic - whatever zone OBS is running in. A schedule authored on one machine is then correct
 *               on another, which is what a touring production wants.
 *   Fixed     - the zone named in `timeZone`. What every printed schedule in the world does: the
 *               board says ALL TIMES EST and means it wherever it is played out.
 */
enum class ClockZoneMode { Automatic, Fixed };

const char *clockZoneModeId(ClockZoneMode mode);
ClockZoneMode clockZoneModeFromId(const char *id, ClockZoneMode fallback = ClockZoneMode::Automatic);

/* --- block styling ------------------------------------------------------------------------ */

/*
 * A pattern laid over a block, on top of everything else it is filled with.
 *
 * What it is for is continuation: a block that is the same event carried across a break, a page or
 * the end of a wave, drawn so a reader can see at a glance that it is not a new thing starting. The
 * printed schedules this is modeled on all hatch it, and none of them re-title it.
 *
 * It is a property of a block style rather than of a continued event, so it is available to say
 * anything else -- a provisional slot, a room being held -- with the same marks.
 */
enum class HatchPattern { None, Diagonal, BackDiagonal, Cross, Horizontal, Vertical };

const char *hatchPatternId(HatchPattern pattern);
HatchPattern hatchPatternFromId(const char *id, HatchPattern fallback = HatchPattern::None);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *hatchPatternName(HatchPattern pattern);
const QVector<HatchPattern> &allHatchPatterns();

/*
 * What a block does when its words are wider than it is.
 *
 *   Shrink    - set the text smaller, down to a floor, until it lands. The default, because a dense
 *               grid is mostly blocks that are slightly too small for their titles and shrinking is
 *               what a person laying one out by hand does.
 *   Wrap      - break onto more lines, and let the text run out of the block if there is no room.
 *   Ellipsize - cut it off with an ellipsis, keeping every block's type at exactly one size.
 *
 * A block style rather than a board setting, so a lane of long game names can shrink while the
 * banner across the top of the same board keeps its size and gets cut.
 */
enum class TextFit { Shrink, Wrap, Ellipsize };

const char *textFitId(TextFit fit);
TextFit textFitFromId(const char *id, TextFit fallback = TextFit::Shrink);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *textFitName(TextFit fit);
const QVector<TextFit> &allTextFits();

struct HatchSpec {
	HatchPattern pattern = HatchPattern::None;
	QColor color = QColor(255, 255, 255, 70);
	/* Line thickness and the distance between consecutive lines, both in pixels. */
	double width = 3.0;
	double spacing = 12.0;

	bool isVisible() const { return pattern != HatchPattern::None && color.alpha() > 0 && width > 0.0; }

	bool operator==(const HatchSpec &other) const
	{
		return pattern == other.pattern && color == other.color &&
		       qFuzzyCompare(width + 1.0, other.width + 1.0) &&
		       qFuzzyCompare(spacing + 1.0, other.spacing + 1.0);
	}
	bool operator!=(const HatchSpec &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * Everything a block can be drawn with, with every part optional.
 *
 * This is a *layer*, not a style: a block's real appearance is four of these merged in a fixed
 * order -- the document's default, then the lane's, then the category's, then the status's, then
 * the event's own -- with each layer contributing only the parts it has switched on. See
 * `CalendarDocument::resolveBlockStyle`.
 *
 * Optional-per-part rather than all-or-nothing because that is what makes the cascade worth having.
 * A category that owns a color should not have to restate the typeface; a status that dims a block
 * should not have to restate the color it is dimming. Either would mean a change to the typeface
 * being made in every category, which is exactly the maintenance a cascade exists to remove.
 *
 * `presetName` on each part binds to the document's shared preset collections -- the same ones the
 * credit roll binds to -- and falls back to the copy held here when the name no longer resolves,
 * the way every binding in this plugin does.
 */
struct BlockStyle {
	bool usePanel = false;
	BackgroundPanel panel;
	QString panelPresetName;

	bool useTitleStyle = false;
	TextStyle titleStyle;
	QString titlePresetName;

	bool useSubtitleStyle = false;
	TextStyle subtitleStyle;
	QString subtitlePresetName;

	/*
	 * The small print inside a block: its time, its channel tags, its location. One style for the
	 * three of them because they are one line's worth of supporting text however many of them a
	 * block happens to carry, and three styles would be three places to set the same 14px grey.
	 */
	bool useMetaStyle = false;
	TextStyle metaStyle;
	QString metaPresetName;

	bool useHatch = false;
	HatchSpec hatch;

	/*
	 * Multiplied into everything the block paints, 0.0 to 1.0. This is what "dim the finished
	 * events" is: a status layer carrying nothing but an opacity, which leaves every color and
	 * typeface below it exactly as the schedule set them.
	 */
	bool useOpacity = false;
	double opacity = 1.0;

	bool useFit = false;
	TextFit fit = TextFit::Shrink;
	/* Shrink only: how small the text may be set before it is allowed to overflow, in pixels. */
	int minPixelSize = 11;

	/* True when the layer would contribute anything at all. */
	bool isEmpty() const;

	/*
	 * Copies whatever `over` has switched on into this. The caller starts from the base layer and
	 * applies the others in cascade order, so the last layer to speak about a part wins.
	 */
	void applyOver(const BlockStyle &over);

	bool operator==(const BlockStyle &other) const;
	bool operator!=(const BlockStyle &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * A block style with every part answered, which is what the renderer draws from.
 *
 * Separate from `BlockStyle` so that nothing on the paint path can be handed a half-specified
 * style and have to decide for itself what an unset part means. Deciding that is the cascade's
 * job, and it happens exactly once per block.
 */
struct ResolvedBlockStyle {
	BackgroundPanel panel;
	TextStyle titleStyle;
	TextStyle subtitleStyle;
	TextStyle metaStyle;
	HatchSpec hatch;
	double opacity = 1.0;
	TextFit fit = TextFit::Shrink;
	int minPixelSize = 11;
};

/* --- the pieces of a schedule ------------------------------------------------------------- */

/*
 * A stream, a station or a platform an event is watched on.
 *
 * A label with an optional mark beside it. The marks are the built-in glyph table (see
 * model/TagGlyph.hpp) or a file the user points at, which between them cover the boards this is
 * modeled on and anything they did not think of. The glyph is painted through the meta style's own
 * ink, exactly as a bridge tile is painted through a section's, so a tag picks up the color of the
 * text it sits beside rather than needing one of its own.
 */
struct ChannelTag {
	QString label;
	TagGlyph glyph = TagGlyph::None;
	/* Custom glyphs only: absolute path to an image or SVG. */
	QString imagePath;

	bool isEmpty() const { return label.isEmpty() && glyph == TagGlyph::None && imagePath.isEmpty(); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One day of the schedule.
 *
 * A day is both a date and an object of its own, because both are true of it: the date is what
 * orders the board and drives the clock, and the object is what carries the heading, the column and
 * the styling a printed schedule gives each day. An event names a day rather than carrying a date,
 * so retiming a whole day is one edit here rather than one per event -- and so a board of waves
 * that never mentions a date still has days to arrange itself by.
 *
 * The heading is built from `date` unless one is typed here, which is what lets FRIDAY be called
 * DAY ONE without the schedule losing track of when Friday is.
 */
struct CalendarDay {
	QString id;
	QDate date;
	/*
	 * The heading, or empty to build one from `date` through the document's `dayFormat`. Empty by
	 * default so a day renamed by the calendar does not have to be renamed here too.
	 */
	QString label;
	/* A second line under it: 2/18, or anything else. Empty builds one from `dateFormat`. */
	QString subLabel;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One row or column of a grid: a stream channel, a game, a station, a stage.
 *
 * The overlap and open-ended rules live here rather than on the document because they are
 * properties of what the lane *is*. A lane that is one screen in one room can never run two things
 * at once and wants a collision shown; a lane that stands for a whole platform genuinely does, and
 * wants them side by side. A document-wide setting would be right for one of them and wrong for
 * the other in the same board.
 */
struct CalendarLane {
	QString id;
	QString name;
	/* A second line under the name in the lane header -- a channel, a room. */
	QString subLabel;
	LogoRef logo;

	LaneOverlap overlap = LaneOverlap::Split;
	OpenEndedRule openEnded = OpenEndedRule::UntilNext;
	/* How long an open-ended event in this lane lasts, in minutes, when the rule needs a number. */
	int openEndedMinutes = 60;

	/* This lane's layer of the block-style cascade, and the styling of its own header. */
	BlockStyle style;
	BlockStyle headerStyle;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One band of a slotted time axis: WAVE C, row D, "Block 2".
 *
 * `startMinutes`/`endMinutes` are optional -- -1 for unset -- and that is the whole of what makes a
 * slotted board able to do everything a clocked one can. Without them a slot is a name and an
 * order, the axis is a row of equal boxes, and the now-marker has nothing to point at; with them
 * the gutter can print WAVE C and 1:00 PM together, an event typed in with a clock time can be
 * dropped into the right slot, and every live feature works on a board with no clock axis at all.
 */
struct CalendarSlot {
	QString id;
	QString name;
	/* Minutes from midnight, or -1 when the slot carries no real time. */
	int startMinutes = -1;
	int endMinutes = -1;

	/* Slots of different weights, for a wave that is deliberately longer than its neighbors. */
	double weight = 1.0;

	bool visible = true;

	bool hasTime() const { return startMinutes >= 0; }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * A kind of thing that happens: POOLS, TOP 24, TOP 8, SPECIAL.
 *
 * The layer of the cascade a legend can be built from, which is why a category is a named object
 * rather than a color typed onto each event. A legend generated from the colors actually used on
 * the board would have no names to print beside them, and one maintained by hand goes stale the
 * first time a category is recolored.
 */
struct CalendarCategory {
	QString id;
	QString name;
	BlockStyle style;
	/* Kept out of the legend without being deleted, for an internal category nobody watching cares about. */
	bool inLegend = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One thing that happens.
 *
 * Where it sits is said twice over, deliberately: `startMinutes`/`endMinutes` place it on a clock
 * axis and `slotId`/`endSlotId` place it on a slotted one. An event usually carries only the pair
 * its board reads, and carrying both is what lets the same schedule be shown either way -- see
 * `CalendarDocument::eventSpan`, which resolves whichever the axis is asking for, deriving one from
 * the other where the slots carry times.
 *
 * Nothing here is thrown away when a board's layout changes, exactly as nothing is thrown away when
 * a roll's section changes type: the fields the layout does not read simply stop being read.
 */
struct CalendarEvent {
	QString id;

	QString title;
	QString subtitle;

	QString dayId;
	QString laneId;
	QString categoryId;

	/* Clock placement: minutes from midnight within the day, or -1 for unset. */
	int startMinutes = -1;
	int endMinutes = -1;

	/* Slot placement: the slot it starts in, and the last slot it runs through. */
	QString slotId;
	QString endSlotId;

	/*
	 * This event's own zone, or empty to read it in the document's. An override rather than a
	 * conversion recorded on the way in, so a schedule stitched from several regions' feeds keeps
	 * saying what each feed said and the board does the arithmetic.
	 */
	QString timeZone;

	LogoRef logo;
	QVector<ChannelTag> tags;
	QString location;
	/* Never drawn on the board: the designer's own note, for the row that needs explaining. */
	QString notes;

	EventStatus status = EventStatus::Auto;

	/*
	 * Drawn across every lane rather than inside one: DOORS OPEN, MAIN HALL CLOSE.
	 *
	 * First-class rather than a lane called "everything", because a band is not a lane's content --
	 * it has no lane, it takes no lane's room, and the lanes go on being drawn behind or beside it.
	 * A schedule with three of them would otherwise need a fourth column nobody wants to see.
	 */
	bool band = false;

	/*
	 * The id of the event this one continues, or empty.
	 *
	 * Two things at once, which is what makes it worth a field rather than a checkbox. The layout
	 * knows the pair are one event, so an Up Next list counts them once and the second block need
	 * not repeat the title; and the style cascade can mark it, through whatever hatch the
	 * continuation layer carries. A link that names an event no longer in the schedule degrades to
	 * an ordinary block rather than failing.
	 */
	QString continuationOf;

	/* The event's own layer of the cascade: the escape hatch for the one block that differs. */
	BlockStyle style;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	/* What the designer's event table shows in its first column. */
	QString displayLabel() const;
};

/* --- the free layer ----------------------------------------------------------------------- */

/*
 * What a free-layer element draws.
 *
 * The layout owns the furniture that is structurally part of a board -- day headings, lane headers,
 * the time gutter -- because all three are generated from the schedule and have to move when it
 * moves. Everything else a board carries is placed by hand, because every board carries a different
 * set of it and no amount of anticipating covers the next one: a clock, a ticker, a row of chips, a
 * sponsor strip, a QR code somebody exported as a PNG.
 */
enum class ElementType { Text, Image, Panel, Clock, Ticker, ChipRow, Legend };

const char *elementTypeId(ElementType type);
ElementType elementTypeFromId(const char *id, ElementType fallback = ElementType::Text);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *elementTypeName(ElementType type);
const QVector<ElementType> &allElementTypes();

/*
 * Which point of the canvas an element's position is measured from.
 *
 * An anchor rather than a plain x/y because a canvas gets resized under a board that was designed
 * at another size, and "24 px in from the bottom right" survives that where "at 1720, 1016" does
 * not. The nine points are the corners, the edge midpoints and the middle.
 */
enum class ElementAnchor {
	TopLeft,
	TopCenter,
	TopRight,
	CenterLeft,
	Center,
	CenterRight,
	BottomLeft,
	BottomCenter,
	BottomRight,
};

const char *elementAnchorId(ElementAnchor anchor);
ElementAnchor elementAnchorFromId(const char *id, ElementAnchor fallback = ElementAnchor::TopLeft);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *elementAnchorName(ElementAnchor anchor);
const QVector<ElementAnchor> &allElementAnchors();

/* Where the anchor sits on the canvas and on the element, 0 at the leading edge and 1 at the far one. */
double elementAnchorX(ElementAnchor anchor);
double elementAnchorY(ElementAnchor anchor);

/* What a legend element lists. */
enum class LegendSource { Categories, Lanes, Statuses };

const char *legendSourceId(LegendSource source);
LegendSource legendSourceFromId(const char *id, LegendSource fallback = LegendSource::Categories);

/* One hand-written entry of a chip row. */
struct Chip {
	QString label;
	TagGlyph glyph = TagGlyph::None;
	QString imagePath;
	/* Overrides the element's own chip panel for this one chip. Off by default. */
	bool useColor = false;
	QColor color = QColor(255, 255, 255, 40);

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One thing placed on the canvas over the board.
 *
 * A single struct covering all seven types rather than a hierarchy, for exactly the reason the
 * roll's `Section` is one struct: it keeps persistence trivial, and it makes changing an element's
 * type in the designer non-destructive -- nothing is thrown away, the fields the new type does not
 * read simply stop being read.
 */
struct CalendarElement {
	QString id;
	ElementType type = ElementType::Text;
	/* Shown in the designer's element list only; never drawn. */
	QString label;

	ElementAnchor anchor = ElementAnchor::TopLeft;
	/* Offset from the anchor point, in pixels. Positive is inward from the edge it names. */
	double x = 0.0;
	double y = 0.0;
	/* The box, in pixels. A height of 0 means "as tall as the content turns out to be". */
	double width = 400.0;
	double height = 0.0;

	/* Text, Ticker: the words. Clock: an optional label drawn above the time. */
	QString text;
	TextStyle textStyle;
	QString textPresetName;

	/* The panel behind the element, which for a Panel element is the whole of what it draws. */
	BackgroundPanel panel;
	QString panelPresetName;
	/* Space between the panel's edge and the content inside it, in pixels. */
	double paddingX = 16.0;
	double paddingY = 10.0;

	/* Image: the artwork. Also the Clock's optional mark, and a Ticker's leading badge. */
	LogoRef image;

	/*
	 * Clock: how the time is written, in QDateTime::toString's format language, and whether it is
	 * shown in the board's zone or in this machine's. A board that says ALL TIMES EST usually also
	 * carries a LOCAL TIME clock, which is that second reading of the same instant.
	 */
	QString clockFormat = QStringLiteral("h:mm AP");
	bool clockUsesLocalZone = false;
	/* Clock: a second line under the time, for the zone's own name. */
	QString clockZoneLabel;

	/* Ticker: several messages shown in turn, or one when the list holds one. */
	QStringList messages;
	/* Ticker: seconds each message holds before the next. Ignored while the board is still. */
	double messageDwell = 8.0;

	/* ChipRow: the chips, and how they are drawn. */
	QVector<Chip> chips;
	double chipGap = 12.0;
	double chipPaddingX = 14.0;
	double chipPaddingY = 6.0;
	double chipRadius = 14.0;

	/* Legend: what it lists, and how it is arranged. */
	LegendSource legendSource = LegendSource::Categories;
	int legendColumns = 1;
	double legendSwatchSize = 18.0;
	double legendGap = 10.0;
	/*
	 * Ids the legend leaves out, and labels it prints instead of the object's own name. Between
	 * them these are what make a generated legend editable without making it hand-maintained: the
	 * list follows the schedule, and the two or three deliberate departures from it are recorded
	 * rather than re-entered.
	 */
	QStringList legendHidden;
	QStringList legendLabels;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	/* Builds an element of `type` with defaults appropriate to it. */
	static CalendarElement makeDefault(ElementType type);

	/* What the designer's element list shows. */
	QString displayLabel() const;
};

/* --- board settings ----------------------------------------------------------------------- */

/*
 * How the grid itself is drawn: its gutter, its headings, its rules and its sizing.
 *
 * Grouped rather than spread over the document because they are one subject -- the furniture the
 * layout owns -- and because a preset is mostly a value for each of these.
 */
struct GridSettings {
	/* The time gutter. */
	GutterSide gutter = GutterSide::Leading;
	double gutterWidth = 110.0;
	bool gutter24Hour = false;
	/* Minutes between gutter labels on a clock axis. 60 is an hour, 30 a half. */
	int gutterStep = 60;
	/* Slotted axes only: print each slot's real time under its name, when it carries one. */
	bool gutterShowSlotTimes = true;
	TextStyle gutterStyle;
	QString gutterPresetName;
	BackgroundPanel gutterPanel;
	QString gutterPanelPresetName;

	/* Lane headers. */
	bool showLaneHeaders = true;
	double laneHeaderSize = 64.0;
	bool laneHeaderLogos = true;
	TextStyle laneStyle;
	QString lanePresetName;
	BackgroundPanel lanePanel;
	QString lanePanelPresetName;

	/* Day headings. */
	bool showDayHeaders = true;
	double dayHeaderHeight = 88.0;
	/* QDate::toString formats, used only for a day that has not been given a label of its own. */
	QString dayFormat = QStringLiteral("dddd");
	QString dateFormat = QStringLiteral("M/d");
	TextStyle dayStyle;
	QString dayPresetName;
	BackgroundPanel dayPanel;
	QString dayPanelPresetName;

	/*
	 * How the days are arranged on a grid.
	 *
	 * Columns puts each day beside the last, sharing one time axis -- the three-day board. Rows
	 * stacks a whole grid per day, each with its own axis and lane headers -- the poster. The
	 * difference is not cosmetic: one has a single axis and one has several, so it decides how
	 * much of the board a lane even means.
	 */
	bool daysAsColumns = true;

	/* Sizing. */
	/* Clock axes: pixels per hour along the time axis. Slotted axes read `slotSize` instead. */
	double pixelsPerHour = 90.0;
	double slotSize = 120.0;
	/* Lane thickness across the minor axis, in pixels. */
	double laneSize = 220.0;
	/* Space between one lane and the next, and between a block and the edges of its cell. */
	double laneGap = 6.0;
	double blockInset = 2.0;
	double dayGap = 24.0;

	/* Grid rules, drawn under the blocks. */
	bool showTimeLines = true;
	QColor timeLineColor = QColor(255, 255, 255, 40);
	double timeLineWidth = 1.0;
	bool showLaneLines = true;
	QColor laneLineColor = QColor(255, 255, 255, 25);
	double laneLineWidth = 1.0;

	/*
	 * The window the time axis covers, in minutes from midnight, or -1 to take it from the events.
	 * A board of an evening's schedule that still drew midnight to midnight would spend four fifths
	 * of itself on nothing, and one pinned to 11AM-midnight is what a printed schedule does.
	 */
	int axisStart = -1;
	int axisEnd = -1;

	/* What a block prints inside itself, beyond its title. */
	bool showBlockTimes = true;
	bool showBlockSubtitles = true;
	bool showBlockTags = true;
	bool showBlockLocation = false;
	bool showBlockLogos = true;
	double blockLogoHeight = 28.0;
	double blockPaddingX = 10.0;
	double blockPaddingY = 6.0;
	double blockGap = 4.0;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * What the board does about the current time.
 *
 * Five independent switches rather than one "live mode", because they are five separate wants and
 * boards ask for different subsets of them: a poster on a wall wants none, a between-matches holding
 * screen wants the finished ones gone, a control-room board wants the line and the highlight and
 * nothing removed. A single mode would force the ones nobody asked for.
 *
 * A board with all of them off never re-renders on a timer and costs nothing per frame, which is
 * what makes a still board still.
 */
struct LiveSettings {
	bool nowLine = false;
	QColor nowLineColor = QColor(255, 80, 80);
	double nowLineWidth = 3.0;
	/* A label riding on the line -- NOW -- or empty for a bare rule. */
	QString nowLineLabel;

	bool dimFinished = false;
	double finishedOpacity = 0.35;

	bool highlightCurrent = false;
	/* The layer merged over a block that is running now. */
	BlockStyle currentStyle;

	bool dropFinished = false;
	/*
	 * Keep an event on the board for this many minutes after it ends, so a result that has just
	 * finished does not vanish while people are still looking at it.
	 */
	int dropGraceMinutes = 0;

	/*
	 * How often the board is re-rendered while any of the above is on, in seconds. A minute is
	 * enough for a schedule measured in hours; a clock element showing seconds is what makes this
	 * worth turning down, and re-rasterizing a board is not free.
	 */
	int refreshSeconds = 30;

	/* True when anything here needs the board rebuilt as time passes. */
	bool needsClock() const;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/* How a board too big for its canvas is dealt with. */
struct OverflowSettings {
	OverflowMode mode = OverflowMode::Fit;

	/* Fit: how far the board may be scaled down before it is allowed to overflow instead. */
	double minScale = 0.35;

	/* Page: seconds each page holds, and whether paging follows the days rather than the pixels. */
	double pageDwell = 12.0;
	/*
	 * Cut the pages at day boundaries rather than wherever the canvas runs out. A three-day board
	 * paged by pixels puts the end of Friday and the start of Saturday on one page, which is not a
	 * page anybody meant to make.
	 */
	bool pageByDay = true;

	/* Scroll: pixels per second, and the pause at each end before it turns around. */
	double scrollSpeed = 40.0;
	double scrollPause = 3.0;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/* --- the document ------------------------------------------------------------------------- */

/*
 * One event's resolved position on the board's time axis.
 *
 * Minutes from the start of its day for a clock axis, slot indices for a slotted one; `valid` is
 * false for an event the axis cannot place at all, which the designer reports and the layout skips
 * rather than drawing at a position it made up.
 */
struct EventSpan {
	bool valid = false;
	/* Clock axes: minutes from midnight, end exclusive. */
	int startMinutes = 0;
	int endMinutes = 0;
	/* Slotted axes: inclusive range of slot indices. */
	int startSlot = 0;
	int endSlot = 0;
	/* True when the end was inferred rather than stated -- see `OpenEndedRule`. */
	bool openEnded = false;
};

/* Two events in one lane that want the same time. Reported to the designer, never hidden. */
struct OverlapReport {
	int firstEvent = -1;
	int secondEvent = -1;
	QString laneId;
};

struct CalendarDocument {
	/* Canvas geometry in pixels, and the source's reported size. */
	int width = 1920;
	int height = 1080;
	QColor background = QColor(0, 0, 0, 0);

	CalendarLayout layout = CalendarLayout::Grid;
	TimeAxisMode axisMode = TimeAxisMode::Clock;
	GridOrientation orientation = GridOrientation::TimeDown;

	/* Space between the board and the edges of the canvas, in pixels. */
	double marginX = 40.0;
	double marginY = 40.0;

	QVector<CalendarDay> days;
	QVector<CalendarLane> lanes;
	QVector<CalendarSlot> timeSlots;
	QVector<CalendarCategory> categories;
	QVector<CalendarEvent> events;
	QVector<CalendarElement> elements;

	GridSettings grid;
	LiveSettings live;
	OverflowSettings overflow;

	/* The bottom layer of the block-style cascade: what a block with nothing said about it is. */
	BlockStyle blockStyle;
	/* The layers merged for each stated status, over everything else. */
	BlockStyle upcomingStyle;
	BlockStyle liveStyle;
	BlockStyle finishedStyle;
	BlockStyle cancelledStyle;
	/* The layer merged over a block that continues another. */
	BlockStyle continuationStyle;

	/* UpNext lists only. */
	int upNextCount = 5;
	double upNextRowHeight = 92.0;
	double upNextRowGap = 10.0;
	/* Width of the time column, in pixels. */
	double upNextTimeWidth = 220.0;
	/* Show events that have already finished, rather than only what is still to come. */
	bool upNextIncludesPast = false;

	/* Zone the schedule's times are read in. */
	ClockZoneMode zoneMode = ClockZoneMode::Automatic;
	QString timeZone;

	/*
	 * Shared with the credit roll: the same named text styles and panels, so one house style
	 * dresses both sources. Calendar-specific presets -- whole boards -- live in the machine-wide
	 * library rather than here; see StyleLibrary.
	 */
	QVector<StylePreset> stylePresets;
	QVector<BackgroundPreset> backgroundPresets;

	/* Fonts carried inside the document, exactly as a roll carries them. */
	bool bundleFonts = true;
	QVector<BundledFont> bundledFonts;
	QVector<FontSubstitution> fontSubstitutions;

	/* --- lookups ---------------------------------------------------------------------- */

	const CalendarDay *findDay(const QString &id) const;
	const CalendarLane *findLane(const QString &id) const;
	const CalendarSlot *findSlot(const QString &id) const;
	const CalendarCategory *findCategory(const QString &id) const;
	const CalendarEvent *findEvent(const QString &id) const;

	int dayIndex(const QString &id) const;
	int laneIndex(const QString &id) const;
	int slotIndex(const QString &id) const;

	/* An id nothing in `existing` is using, built from `prefix`. */
	static QString makeId(const QString &prefix, const QStringList &existing);
	QString makeEventId() const;
	QString makeLaneId() const;
	QString makeDayId() const;
	QString makeSlotId() const;
	QString makeCategoryId() const;
	QString makeElementId() const;

	/* --- time ------------------------------------------------------------------------- */

	/* The zone the schedule's own times are read in. */
	QTimeZone displayZone() const;

	/*
	 * When `event` starts and ends, as instants. Invalid when the event carries no placement the
	 * clock can read -- a slotted event whose slots carry no times, for instance -- which every
	 * live feature treats as "no opinion" rather than as a time of zero.
	 */
	QDateTime eventStart(const CalendarEvent &event) const;
	QDateTime eventEnd(const CalendarEvent &event) const;

	/*
	 * Where `event` sits on the axis, with an open end resolved through its lane's rule. The whole
	 * of the layout's placement arithmetic, in one place, so a clock board and a slotted board
	 * cannot disagree about where an event is.
	 */
	EventSpan eventSpan(const CalendarEvent &event) const;

	/*
	 * How long an event with no stated end runs, in minutes, under its lane's rule.
	 *
	 * Its own function because `UntilNext` is the only part of placing an event that has to look at
	 * the other events, and burying that inside `eventSpan` would hide a whole-schedule scan inside
	 * something every caller treats as a field read.
	 */
	int resolvedOpenEndedLength(const CalendarEvent &event, int startMinutes) const;

	/* The status the board should draw `event` in at `now`, with Auto resolved against the clock. */
	EventStatus statusAt(const CalendarEvent &event, const QDateTime &now) const;

	/* The window the time axis covers, in minutes from midnight, taken from the events when unset. */
	void axisRange(int *start, int *end) const;

	/*
	 * True when anything on the board would be wrong a minute from now: a live feature, or a clock
	 * element. What the source asks to decide whether this board needs redrawing on a timer at all
	 * -- and a board that answers no costs nothing per frame for the rest of its life.
	 */
	bool needsClock() const;

	/* --- styling ---------------------------------------------------------------------- */

	/* Null when no preset carries that name, including for an empty name. */
	const TextStyle *findStylePreset(const QString &name) const;
	const BackgroundPanel *findBackgroundPreset(const QString &name) const;

	/*
	 * The whole cascade, resolved: document, then lane, then category, then status, then
	 * continuation, then the event's own layer, with preset bindings resolved at each step.
	 *
	 * Everything that paints a block goes through this rather than reading a `BlockStyle` directly,
	 * so a layer cannot be skipped by forgetting it at one paint site -- the same guarantee
	 * `Document::effectiveStyle` gives a roll's text.
	 */
	ResolvedBlockStyle resolveBlockStyle(const CalendarEvent &event, EventStatus status) const;

	/* The same for a piece of furniture, which reads only the document's own layers. */
	ResolvedBlockStyle resolveStyle(const BlockStyle &layer) const;

	/* The text style an element draws with, once its preset binding is resolved. */
	const TextStyle &effectiveElementStyle(const CalendarElement &element) const;
	const BackgroundPanel &effectiveElementPanel(const CalendarElement &element) const;

	void setStylePreset(const QString &name, const TextStyle &style);
	void removeStylePreset(const QString &name);
	void setBackgroundPreset(const QString &name, const BackgroundPanel &panel);
	void removeBackgroundPreset(const QString &name);
	bool linkStylePreset(const QString &name);
	bool linkBackgroundPreset(const QString &name);
	bool refreshLinkedPresets();
	bool refreshLinkedBackgroundPresets();
	bool applyLibraryRenames();

	/* --- housekeeping ----------------------------------------------------------------- */

	/*
	 * Makes sure the board has a day and that every event names one that exists, moving the
	 * orphans onto the first. Returns true when anything moved.
	 *
	 * It never removes a day: a day emptied while its events are being retyped is not one anybody
	 * meant to delete. And it is called from the designer rather than from the render path, because
	 * a board should not quietly grow a column while it is being drawn -- the one place the shape
	 * of a schedule can change is an edit.
	 */
	bool syncDays();

	/* Every pair of events in one lane that want the same time, for the designer to report. */
	QVector<OverlapReport> overlaps() const;

	/* Events in the order the board draws them: by day, then lane, then time. */
	QVector<int> orderedEvents() const;

	/* --- fonts ------------------------------------------------------------------------ */

	QStringList usedFontFamilies() const;
	QVector<FontUse> usedFonts() const;
	QString fontSubstitute(const QString &family) const;
	void setFontSubstitute(const QString &from, const QString &to);
	bool applyFontSubstitutions(const QStringList &families);
	bool refreshFontBundle(QStringList *skipped = nullptr, bool recollect = false);

	/* --- persistence ------------------------------------------------------------------ */

	void save(obs_data_t *data) const;
	void load(obs_data_t *data, bool *migrated = nullptr);
	static void defaults(obs_data_t *data);

	QString toJson() const;
	bool fromJson(const QString &json, QString *error = nullptr);
};

} // namespace closingtime
