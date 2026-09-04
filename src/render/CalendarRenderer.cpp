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

#include "render/CalendarRenderer.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <QTime>

#include <algorithm>
#include <cmath>

#include "render/BackgroundPainter.hpp"
#include "render/FontResolution.hpp"
#include "render/SvgArt.hpp"

namespace closingtime {

namespace {

/* Total board height is capped, as a backstop against a runaway import. */
constexpr double kMaxBoardExtent = 60000.0;

/* --- fonts and text ----------------------------------------------------------------------- */

QFont makeFont(const TextStyle &style)
{
	QFont font(style.family);
	/* Pixel sizing, for the same reason the roll uses it: a board must not move with screen DPI. */
	font.setPixelSize(std::max(1, style.pixelSize));
	font.setBold(style.bold);
	font.setItalic(style.italic);

	/* Only ever a face this machine has -- naming one it lacks switches off Qt's synthetic bold. */
	if (!style.styleName.isEmpty() && fontStyleAvailable(style.family, style.styleName))
		font.setStyleName(style.styleName);

	font.setUnderline(style.underline);
	font.setStrikeOut(style.strikeOut);
	return font;
}

/* Word-wraps one paragraph to `width`, never breaking a word that fits on a line of its own. */
QStringList wrapParagraph(const QString &text, const QFontMetricsF &metrics, double width)
{
	QStringList lines;
	if (width <= 0.0) {
		lines.append(text);
		return lines;
	}

	QString line;
	const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	for (const QString &word : words) {
		const QString candidate = line.isEmpty() ? word : line + QLatin1Char(' ') + word;
		if (!line.isEmpty() && metrics.horizontalAdvance(candidate) > width) {
			lines.append(line);
			line = word;
		} else {
			line = candidate;
		}
	}
	if (!line.isEmpty())
		lines.append(line);
	if (lines.isEmpty())
		lines.append(QString());

	return lines;
}

/*
 * The lines a run of text is set as, and the font it is set in, once the fit rule has had its say.
 *
 * One place rather than three, because the three rules are three answers to the same question and a
 * board that measured a block one way and drew it another would misplace everything under it. The
 * font comes back out because Shrink changes it: the caller has to draw with the size that fitted,
 * not with the size that was asked for.
 */
struct FittedText {
	QStringList lines;
	QFont font;
	double lineHeight = 0.0;
	double height = 0.0;
	double width = 0.0;
};

FittedText fitText(const QString &text, const TextStyle &style, double width, TextFit fit, int minPixelSize)
{
	FittedText fitted;

	const QStringList paragraphs = text.split(QLatin1Char('\n'));
	int pixelSize = std::max(1, style.pixelSize);

	const auto measure = [&](const QFont &font) {
		const QFontMetricsF metrics(font);
		QStringList lines;
		double widest = 0.0;

		for (const QString &paragraph : paragraphs) {
			if (fit == TextFit::Wrap) {
				lines.append(wrapParagraph(paragraph, metrics, width));
			} else if (fit == TextFit::Ellipsize) {
				lines.append(metrics.elidedText(paragraph, Qt::ElideRight, width));
			} else {
				lines.append(paragraph);
			}
		}

		for (const QString &line : lines)
			widest = std::max(widest, metrics.horizontalAdvance(line));

		fitted.lines = lines;
		fitted.width = widest;
		fitted.lineHeight = metrics.height() * std::max(0.1, style.lineSpacing);
		fitted.height = fitted.lineHeight * lines.size();
	};

	QFont font = makeFont(style);
	measure(font);

	/*
	 * Shrinking steps the size down rather than solving for it: the relationship between a pixel
	 * size and an advance width is not linear once hinting is involved, so the arithmetic answer
	 * would have to be measured anyway. A block is a handful of steps from fitting or it is far too
	 * small to hold the words at all, which the floor catches.
	 */
	if (fit == TextFit::Shrink && width > 0.0) {
		const int floorSize = std::clamp(minPixelSize, 4, pixelSize);
		while (fitted.width > width && pixelSize > floorSize) {
			--pixelSize;
			font.setPixelSize(pixelSize);
			measure(font);
		}
	}

	fitted.font = font;
	return fitted;
}

/*
 * Draws already-fitted lines inside `rect`.
 *
 * Text with effects goes through `paintInkedArt` as a silhouette, exactly as a bridge tile does, so
 * a gradient, an outline and a shadow mean the same thing on a schedule block as they do in a credit
 * roll -- and so there is one implementation of each rather than two.
 */
void paintLines(QPainter *painter, const QRectF &rect, const FittedText &fitted, const TextStyle &style)
{
	if (!painter || fitted.lines.isEmpty())
		return;

	const QFontMetricsF metrics(fitted.font);
	const double ascent = metrics.ascent();

	const auto lineX = [&](const QString &line) {
		const double advance = metrics.horizontalAdvance(line);
		switch (style.align) {
		case HAlign::Left:
			return rect.left();
		case HAlign::Right:
			return rect.right() - advance;
		default:
			return rect.left() + (rect.width() - advance) / 2.0;
		}
	};

	if (!style.hasEffects()) {
		painter->setFont(fitted.font);
		painter->setPen(style.color);

		double y = rect.top();
		for (const QString &line : fitted.lines) {
			painter->drawText(QPointF(lineX(line), y + ascent), line);
			y += fitted.lineHeight;
		}
		return;
	}

	const double bleed = style.effectBleed();
	const QRectF bounds = rect.adjusted(-bleed, -bleed, bleed, bleed);
	const QRectF fillBox(rect.left(), rect.top(), rect.width(), std::max(1.0, fitted.height));

	paintInkedArt(painter, bounds, style, fillBox, [&](QPainter *target) {
		QPainterPath path;
		double y = rect.top();
		for (const QString &line : fitted.lines) {
			path.addText(QPointF(lineX(line), y + ascent), fitted.font, line);
			y += fitted.lineHeight;
		}
		target->fillPath(path, Qt::white);
	});
}

/* --- plan --------------------------------------------------------------------------------- */

/*
 * One thing the layout decided to draw, in board coordinates.
 *
 * A single tagged struct rather than a list per kind, because the order things are drawn in *is* the
 * plan: a block's panel goes over the grid rules and under its own text, and keeping the items in
 * one list in the order they were planned is what says so. Separate lists would need a z-order
 * invented alongside them and kept in step with every new kind of item.
 */
struct PlanItem {
	enum class Kind { Panel, Fill, Text, Image, Glyph, Hatch };

	Kind kind = Kind::Fill;
	QRectF rect;
	double opacity = 1.0;

	/* Panel. */
	BackgroundPanel panel;

	/* Fill: a flat rectangle, which is what every grid rule and the now-line are. */
	QColor color;

	/* Text. */
	QString text;
	TextStyle style;
	TextFit fit = TextFit::Shrink;
	int minPixelSize = 11;

	/* Image, and the file behind a custom glyph. */
	QString imagePath;
	int imageMaxHeight = 0;
	/* Image only: how the artwork is timed, if it turns out to be animated. */
	LogoPlayback playback;
	/*
	 * Image only: set by the animation pass when the file turns out to animate. The paint pass then
	 * leaves the space empty and the compositor draws the artwork over it -- see CalendarAnimation.
	 */
	bool hole = false;

	/* Glyph: a built-in mark, inked through `style`. */
	TagGlyph glyph = TagGlyph::None;

	/* Hatch, drawn over the block whose corners `radius` names. */
	HatchSpec hatch;
	double radius = 0.0;
};

/*
 * Everything the layout decided, in board coordinates, plus where a page may be cut.
 *
 * `breaks` are the offsets a paged board is allowed to start a page at -- the top of each day, when
 * the board is paged by day. Without them a three-day board paged by pixels puts the end of Friday
 * and the start of Saturday on one page, which is not a page anybody meant to make.
 */
struct Plan {
	QVector<PlanItem> items;
	double width = 0.0;
	double height = 0.0;
	QVector<CalendarHit> hits;
	QVector<double> breaks;
	int placedEvents = 0;
};

/* --- the time axis ------------------------------------------------------------------------ */

/*
 * The one mapping from a time to a distance along the board.
 *
 * Both axis modes come through here, so nothing downstream has to ask which kind of board it is on:
 * a block, a grid rule, a gutter label and the now-line all place themselves by asking the axis
 * where a time is. That is what makes the same layout code draw a clock board and a wave board.
 */
struct TimeAxis {
	bool slotted = false;
	int startMinutes = 0;
	int endMinutes = 0;
	double pixelsPerMinute = 1.5;
	/* Slotted axes: the offset of each slot's leading edge, with the total on the end. */
	QVector<double> offsets;

	double extent() const
	{
		if (slotted)
			return offsets.isEmpty() ? 0.0 : offsets.last();
		return (endMinutes - startMinutes) * pixelsPerMinute;
	}

	/* Clock axes: where a minute lands, clamped to the board rather than drawn off it. */
	double at(int minutes) const
	{
		const double raw = (minutes - startMinutes) * pixelsPerMinute;
		return std::clamp(raw, 0.0, extent());
	}

	double slotStart(int index) const
	{
		if (offsets.isEmpty())
			return 0.0;
		return offsets.at(std::clamp(index, 0, static_cast<int>(offsets.size()) - 1));
	}

	double slotEnd(int index) const { return slotStart(index + 1); }
};

TimeAxis buildAxis(const CalendarDocument &document)
{
	TimeAxis axis;
	axis.slotted = document.axisMode == TimeAxisMode::Slots;

	if (axis.slotted) {
		double offset = 0.0;
		axis.offsets.append(offset);
		for (const CalendarSlot &slot : document.timeSlots) {
			if (!slot.visible)
				continue;
			offset += document.grid.slotSize * std::max(0.05, slot.weight);
			axis.offsets.append(offset);
		}
		/* A slotted board with no slots still needs somewhere to draw, so it gets one band. */
		if (axis.offsets.size() < 2)
			axis.offsets.append(document.grid.slotSize);
		return axis;
	}

	document.axisRange(&axis.startMinutes, &axis.endMinutes);
	axis.pixelsPerMinute = document.grid.pixelsPerHour / 60.0;
	return axis;
}

/* --- small helpers ------------------------------------------------------------------------ */

QString formatMinutes(int minutes, bool h24)
{
	const QTime time = QTime(0, 0).addSecs(((minutes % 1440) + 1440) % 1440 * 60);
	return h24 ? time.toString(QStringLiteral("HH:mm")) : time.toString(QStringLiteral("h:mm AP"));
}

/* The heading a day shows, built from its date when it has not been given one. */
QString dayHeading(const CalendarDocument &document, const CalendarDay &day)
{
	if (!day.label.isEmpty())
		return day.label;
	if (day.date.isValid())
		return day.date.toString(document.grid.dayFormat).toUpper();
	return QString();
}

QString daySubheading(const CalendarDocument &document, const CalendarDay &day)
{
	if (!day.subLabel.isEmpty())
		return day.subLabel;
	if (day.date.isValid())
		return day.date.toString(document.grid.dateFormat);
	return QString();
}

/* The line of small print inside a block: its time, then whatever else was asked for. */
QString blockMeta(const CalendarDocument &document, const CalendarEvent &event, const EventSpan &span)
{
	QStringList parts;

	if (document.grid.showBlockTimes && span.valid) {
		const QString start = formatMinutes(span.startMinutes, document.grid.gutter24Hour);
		if (span.openEnded)
			parts.append(start);
		else
			parts.append(QStringLiteral("%1 - %2").arg(start, formatMinutes(span.endMinutes,
											document.grid.gutter24Hour)));
	}

	if (document.grid.showBlockLocation && !event.location.isEmpty())
		parts.append(event.location);

	return parts.join(QStringLiteral("  ·  "));
}

QVector<int> visibleLaneIndices(const CalendarDocument &document)
{
	QVector<int> indices;
	for (int i = 0; i < document.lanes.size(); ++i) {
		if (document.lanes[i].visible)
			indices.append(i);
	}
	return indices;
}

QVector<int> visibleDayIndices(const CalendarDocument &document)
{
	QVector<int> indices;
	for (int i = 0; i < document.days.size(); ++i) {
		if (document.days[i].visible)
			indices.append(i);
	}
	return indices;
}

/*
 * True when this event should be drawn at all.
 *
 * "Drop finished" lives here rather than in the layout because it is the one filter that removes an
 * event rather than restyling it, and having it in one place is what keeps an Up Next list and a
 * grid from disagreeing about whether the thing that just ended is still on the board.
 */
bool eventIsShown(const CalendarDocument &document, const CalendarEvent &event, const QDateTime &now)
{
	if (!event.visible)
		return false;

	if (!document.live.dropFinished)
		return true;

	if (document.statusAt(event, now) != EventStatus::Finished)
		return true;

	if (document.live.dropGraceMinutes <= 0)
		return false;

	const QDateTime end = document.eventEnd(event);
	if (!end.isValid())
		return true;

	return end.addSecs(document.live.dropGraceMinutes * 60) > now;
}

/* --- the block planner -------------------------------------------------------------------- */

/*
 * Lays the contents of one block out inside its own rectangle and adds them to the plan.
 *
 * Content is dropped from the bottom up when the block is too small to hold it -- the tag row, then
 * the small print, then the subtitle -- rather than being drawn over the block below. That order is
 * the order a person laying a schedule out by hand gives things up in: the title is what the block
 * is *for*, and a block reduced to nothing but its title is still a schedule.
 */
void planBlockContent(Plan *plan, const CalendarDocument &document, const CalendarEvent &event,
		      const ResolvedBlockStyle &style, const QRectF &rect, const EventSpan &span, bool compact)
{
	const GridSettings &grid = document.grid;

	QRectF inner = rect.adjusted(grid.blockPaddingX, grid.blockPaddingY, -grid.blockPaddingX, -grid.blockPaddingY);
	if (inner.width() <= 1.0 || inner.height() <= 1.0)
		return;

	/* A logo takes the leading edge of the block and the text takes what is left beside it. */
	if (grid.showBlockLogos && !event.logo.isEmpty()) {
		const double size = std::min(grid.blockLogoHeight, inner.height());
		PlanItem item;
		item.kind = PlanItem::Kind::Image;
		item.rect = QRectF(inner.left(), inner.top(), size, size);
		item.imagePath = event.logo.path;
		item.imageMaxHeight = static_cast<int>(std::ceil(size));
		item.playback = event.logo.playback;
		item.opacity = style.opacity;
		plan->items.append(item);

		inner.setLeft(inner.left() + size + grid.blockGap);
	}

	struct Run {
		QString text;
		const TextStyle *style;
	};

	QVector<Run> runs;
	if (!event.title.isEmpty())
		runs.append({event.title, &style.titleStyle});
	if (grid.showBlockSubtitles && !event.subtitle.isEmpty())
		runs.append({event.subtitle, &style.subtitleStyle});

	const QString meta = compact ? QString() : blockMeta(document, event, span);
	if (!meta.isEmpty())
		runs.append({meta, &style.metaStyle});

	QString tagLine;
	if (grid.showBlockTags && !compact) {
		QStringList labels;
		for (const ChannelTag &tag : event.tags) {
			if (!tag.label.isEmpty())
				labels.append(tag.label);
		}
		tagLine = labels.join(QStringLiteral("   "));
	}

	/* Measure everything, then give up the least important runs until what is left fits. */
	QVector<FittedText> fitted;
	fitted.reserve(runs.size());
	double total = 0.0;
	for (const Run &run : runs) {
		FittedText text = fitText(run.text, *run.style, inner.width(), style.fit, style.minPixelSize);
		total += text.height;
		fitted.append(text);
	}
	if (!runs.isEmpty())
		total += grid.blockGap * (runs.size() - 1);

	double tagHeight = 0.0;
	FittedText tagText;
	if (!tagLine.isEmpty()) {
		tagText = fitText(tagLine, style.metaStyle, inner.width(), style.fit, style.minPixelSize);
		tagHeight = tagText.height + grid.blockGap;
	}

	while (total + tagHeight > inner.height()) {
		if (tagHeight > 0.0) {
			tagHeight = 0.0;
			continue;
		}
		if (runs.size() <= 1)
			break;

		total -= fitted.last().height + grid.blockGap;
		runs.removeLast();
		fitted.removeLast();
	}

	double y = inner.top();
	for (int i = 0; i < runs.size(); ++i) {
		PlanItem item;
		item.kind = PlanItem::Kind::Text;
		item.rect = QRectF(inner.left(), y, inner.width(), fitted[i].height);
		item.text = runs[i].text;
		item.style = *runs[i].style;
		item.style.pixelSize = fitted[i].font.pixelSize();
		item.fit = style.fit;
		item.minPixelSize = style.minPixelSize;
		item.opacity = style.opacity;
		plan->items.append(item);

		y += fitted[i].height + grid.blockGap;
	}

	if (tagHeight <= 0.0)
		return;

	/*
	 * The tag row sits at the foot of the block, where a printed schedule puts it, rather than
	 * following the text: it is a label on the block rather than another line of it.
	 */
	const double tagY = std::max(y, inner.bottom() - tagText.height);
	double tagX = inner.left();

	for (const ChannelTag &tag : event.tags) {
		if (tag.glyph == TagGlyph::None && tag.imagePath.isEmpty())
			continue;

		const double size = tagText.lineHeight * 0.8;
		PlanItem item;
		item.rect =
			QRectF(tagX, tagY + (tagText.lineHeight - size) / 2.0, size * tagGlyphAspect(tag.glyph), size);
		item.opacity = style.opacity;
		item.style = style.metaStyle;

		if (tag.glyph == TagGlyph::Custom || !tag.imagePath.isEmpty()) {
			item.kind = PlanItem::Kind::Image;
			item.imagePath = tag.imagePath;
			item.imageMaxHeight = static_cast<int>(std::ceil(size));
		} else {
			item.kind = PlanItem::Kind::Glyph;
			item.glyph = tag.glyph;
		}

		plan->items.append(item);
		tagX += item.rect.width() + grid.blockGap;
	}

	PlanItem item;
	item.kind = PlanItem::Kind::Text;
	item.rect = QRectF(tagX, tagY, std::max(1.0, inner.right() - tagX), tagText.height);
	item.text = tagLine;
	item.style = style.metaStyle;
	item.style.pixelSize = tagText.font.pixelSize();
	item.style.align = HAlign::Left;
	item.fit = style.fit;
	item.minPixelSize = style.minPixelSize;
	item.opacity = style.opacity;
	plan->items.append(item);
}

/* Adds the panel, the hatch and the contents of one block. */
void planBlock(Plan *plan, const CalendarDocument &document, int eventIndex, const QRectF &rect, const QDateTime &now,
	       bool compact)
{
	const CalendarEvent &event = document.events[eventIndex];
	const EventStatus status = document.statusAt(event, now);
	const ResolvedBlockStyle style = document.resolveBlockStyle(event, status);

	PlanItem panel;
	panel.kind = PlanItem::Kind::Panel;
	panel.rect = rect;
	panel.panel = style.panel;
	panel.opacity = style.opacity;
	plan->items.append(panel);

	if (style.hatch.isVisible()) {
		PlanItem hatch;
		hatch.kind = PlanItem::Kind::Hatch;
		hatch.rect = rect;
		hatch.hatch = style.hatch;
		hatch.opacity = style.opacity;
		hatch.radius = style.panel.radiusTopLeft;
		plan->items.append(hatch);
	}

	planBlockContent(plan, document, event, style, rect, document.eventSpan(event), compact);

	CalendarHit hit;
	hit.rect = rect;
	hit.event = eventIndex;
	plan->hits.append(hit);
	plan->placedEvents++;
}

/* Furniture: a panel with a heading, and optionally a second line and a mark. */
void planHeader(Plan *plan, const QRectF &rect, const BackgroundPanel &panel, const QString &heading,
		const TextStyle &style, const QString &subHeading, const LogoRef &logo, bool logoLeading)
{
	if (rect.width() <= 0.0 || rect.height() <= 0.0)
		return;

	if (panel.isVisible()) {
		PlanItem item;
		item.kind = PlanItem::Kind::Panel;
		item.rect = rect;
		item.panel = panel;
		plan->items.append(item);
	}

	QRectF inner = rect.adjusted(8.0, 4.0, -8.0, -4.0);
	if (inner.width() <= 1.0)
		return;

	if (!logo.isEmpty() && logoLeading) {
		const double size = std::min(inner.height(), static_cast<double>(logo.maxHeight));
		PlanItem item;
		item.kind = PlanItem::Kind::Image;
		item.rect = QRectF(inner.left(), inner.top() + (inner.height() - size) / 2.0, size, size);
		item.imagePath = logo.path;
		item.imageMaxHeight = static_cast<int>(std::ceil(size));
		item.playback = logo.playback;
		plan->items.append(item);

		inner.setLeft(inner.left() + size + 8.0);
	}

	TextStyle subStyle = style;
	subStyle.pixelSize = std::max(9, static_cast<int>(style.pixelSize * 0.6));
	subStyle.bold = false;

	const FittedText head = fitText(heading, style, inner.width(), TextFit::Shrink, 9);
	const FittedText sub = subHeading.isEmpty() ? FittedText()
						    : fitText(subHeading, subStyle, inner.width(), TextFit::Shrink, 8);

	const double total = head.height + (subHeading.isEmpty() ? 0.0 : sub.height);
	double y = inner.top() + std::max(0.0, (inner.height() - total) / 2.0);

	if (!heading.isEmpty()) {
		PlanItem item;
		item.kind = PlanItem::Kind::Text;
		item.rect = QRectF(inner.left(), y, inner.width(), head.height);
		item.text = heading;
		item.style = style;
		item.style.pixelSize = head.font.pixelSize();
		plan->items.append(item);
		y += head.height;
	}

	if (!subHeading.isEmpty()) {
		PlanItem item;
		item.kind = PlanItem::Kind::Text;
		item.rect = QRectF(inner.left(), y, inner.width(), sub.height);
		item.text = subHeading;
		item.style = subStyle;
		item.style.pixelSize = sub.font.pixelSize();
		plan->items.append(item);
	}
}

/* --- the grid ----------------------------------------------------------------------------- */

/*
 * One day's grid, laid out at `origin`.
 *
 * Written against a major axis (time) and a minor one (lanes) rather than against x and y, which is
 * the whole of what lets the same code draw a board with time down the side and one with time across
 * the top. `place` is the only thing that knows which way round they are.
 */
QSizeF planDayGroup(Plan *plan, const CalendarDocument &document, const TimeAxis &axis, int dayIndex,
		    const QPointF &origin, const QDateTime &now, bool measureOnly)
{
	const GridSettings &grid = document.grid;
	const bool vertical = document.orientation == GridOrientation::TimeDown;
	const QVector<int> lanes = visibleLaneIndices(document);

	const double gutterLead =
		(grid.gutter == GutterSide::Leading || grid.gutter == GutterSide::Both) ? grid.gutterWidth : 0.0;
	const double gutterTrail =
		(grid.gutter == GutterSide::Trailing || grid.gutter == GutterSide::Both) ? grid.gutterWidth : 0.0;
	const double laneHeader = grid.showLaneHeaders ? grid.laneHeaderSize : 0.0;
	const double dayHeader = grid.showDayHeaders ? grid.dayHeaderHeight : 0.0;

	const double laneThickness = grid.laneSize;
	const double minorExtent = lanes.isEmpty() ? laneThickness
						   : lanes.size() * laneThickness + (lanes.size() - 1) * grid.laneGap;
	const double majorExtent = std::max(1.0, axis.extent());

	/*
	 * The two orientations differ only here. With time down the side the gutter is a column and the
	 * lane headers are a row above the columns; with time across the top the gutter is a row and the
	 * lane headers are a column beside the rows.
	 */
	const double width = vertical ? gutterLead + minorExtent + gutterTrail : laneHeader + majorExtent;
	const double height = vertical ? dayHeader + laneHeader + majorExtent
				       : dayHeader + gutterLead + minorExtent + gutterTrail;

	if (measureOnly)
		return QSizeF(width, height);

	const double plotMajor = vertical ? origin.y() + dayHeader + laneHeader : origin.x() + laneHeader;
	const double plotMinor = vertical ? origin.x() + gutterLead : origin.y() + dayHeader + gutterLead;

	/* Board coordinates for a span of the major axis across one lane's worth of the minor one. */
	const auto place = [&](double majorStart, double majorEnd, double minorStart, double minorSize) {
		if (vertical)
			return QRectF(minorStart, plotMajor + majorStart, minorSize, majorEnd - majorStart);
		return QRectF(plotMajor + majorStart, minorStart, majorEnd - majorStart, minorSize);
	};

	const CalendarDay &day = document.days[dayIndex];

	/* The day's own heading, across the whole group. */
	if (dayHeader > 0.0) {
		planHeader(plan, QRectF(origin.x(), origin.y(), width, dayHeader), grid.dayPanel,
			   dayHeading(document, day), grid.dayStyle, daySubheading(document, day), LogoRef(), false);
	}

	/* Lane headers. */
	if (laneHeader > 0.0) {
		for (int i = 0; i < lanes.size(); ++i) {
			const CalendarLane &lane = document.lanes[lanes[i]];
			const double minorStart = plotMinor + i * (laneThickness + grid.laneGap);

			const QRectF rect =
				vertical ? QRectF(minorStart, origin.y() + dayHeader, laneThickness, laneHeader)
					 : QRectF(origin.x(), minorStart, laneHeader, laneThickness);

			planHeader(plan, rect, grid.lanePanel, lane.name, grid.laneStyle, lane.subLabel,
				   grid.laneHeaderLogos ? lane.logo : LogoRef(), true);
		}
	}

	/* The gutter, and the rules that run with it across the plot. */
	const auto gutterEntry = [&](double majorStart, double majorEnd, const QString &label, const QString &sub) {
		const QRectF plot = place(majorStart, majorEnd, plotMinor, minorExtent);

		if (grid.showTimeLines && grid.timeLineWidth > 0.0 && majorStart > 0.0) {
			PlanItem rule;
			rule.kind = PlanItem::Kind::Fill;
			rule.rect = vertical ? QRectF(plot.left(), plot.top(), plot.width(), grid.timeLineWidth)
					     : QRectF(plot.left(), plot.top(), grid.timeLineWidth, plot.height());
			rule.color = grid.timeLineColor;
			plan->items.append(rule);
		}

		if (label.isEmpty())
			return;

		for (int side = 0; side < 2; ++side) {
			const bool leading = side == 0;
			if (leading && gutterLead <= 0.0)
				continue;
			if (!leading && gutterTrail <= 0.0)
				continue;

			QRectF rect;
			if (vertical) {
				const double x = leading ? origin.x() : origin.x() + gutterLead + minorExtent;
				rect = QRectF(x, plot.top(), leading ? gutterLead : gutterTrail,
					      std::min(plot.height(), grid.gutterWidth));
			} else {
				const double y = leading ? origin.y() + dayHeader
							 : origin.y() + dayHeader + gutterLead + minorExtent;
				rect = QRectF(plot.left(), y, plot.width(), leading ? gutterLead : gutterTrail);
			}

			planHeader(plan, rect, grid.gutterPanel, label, grid.gutterStyle, sub, LogoRef(), false);
		}
	};

	if (axis.slotted) {
		int visible = 0;
		for (const CalendarSlot &slot : document.timeSlots) {
			if (!slot.visible)
				continue;

			const QString sub = (grid.gutterShowSlotTimes && slot.hasTime())
						    ? formatMinutes(slot.startMinutes, grid.gutter24Hour)
						    : QString();
			gutterEntry(axis.slotStart(visible), axis.slotEnd(visible), slot.name, sub);
			++visible;
		}
	} else {
		const int step = std::max(5, grid.gutterStep);
		for (int minutes = axis.startMinutes; minutes < axis.endMinutes; minutes += step) {
			gutterEntry(axis.at(minutes), axis.at(std::min(minutes + step, axis.endMinutes)),
				    formatMinutes(minutes, grid.gutter24Hour), QString());
		}
	}

	/* Lane rules, running the length of the plot between one lane and the next. */
	if (grid.showLaneLines && grid.laneLineWidth > 0.0) {
		for (int i = 1; i < lanes.size(); ++i) {
			const double minorStart = plotMinor + i * (laneThickness + grid.laneGap) - grid.laneGap / 2.0;
			const QRectF rect = place(0.0, majorExtent, minorStart, grid.laneLineWidth);

			PlanItem rule;
			rule.kind = PlanItem::Kind::Fill;
			rule.rect = rect;
			rule.color = grid.laneLineColor;
			plan->items.append(rule);
		}
	}

	/* Blocks, lane by lane, with collisions resolved inside each lane. */
	for (int i = 0; i < lanes.size(); ++i) {
		const CalendarLane &lane = document.lanes[lanes[i]];
		const double minorStart = plotMinor + i * (laneThickness + grid.laneGap);

		struct Placed {
			int index;
			double start;
			double end;
		};

		QVector<Placed> placed;
		for (int e = 0; e < document.events.size(); ++e) {
			const CalendarEvent &event = document.events[e];
			if (event.band || event.dayId != day.id || event.laneId != lane.id)
				continue;
			if (!eventIsShown(document, event, now))
				continue;

			const EventSpan span = document.eventSpan(event);
			if (!span.valid)
				continue;

			const double start = axis.slotted ? axis.slotStart(span.startSlot) : axis.at(span.startMinutes);
			const double end = axis.slotted ? axis.slotEnd(span.endSlot) : axis.at(span.endMinutes);
			placed.append({e, start, std::max(end, start + 1.0)});
		}

		std::sort(placed.begin(), placed.end(),
			  [](const Placed &a, const Placed &b) { return a.start < b.start; });

		/*
		 * Collisions are resolved by counting how many events are live at each block's own start
		 * and dividing the lane between them. Simple enough to be predictable, which matters more
		 * here than packing tightly: a schedule with one accidental overlap should show two
		 * half-width blocks rather than rearranging the whole lane around it.
		 */
		for (int b = 0; b < placed.size(); ++b) {
			int share = 1;
			int slot = 0;
			for (int other = 0; other < placed.size(); ++other) {
				if (other == b)
					continue;
				if (placed[other].end <= placed[b].start || placed[other].start >= placed[b].end)
					continue;
				++share;
				if (other < b)
					++slot;
			}

			double blockMinor = minorStart;
			double blockSize = laneThickness;

			if (share > 1) {
				if (lane.overlap == LaneOverlap::Split) {
					blockSize = laneThickness / share;
					blockMinor = minorStart + slot * blockSize;
				} else {
					/* Stacked: offset each collision so the one underneath stays visible. */
					const double nudge = std::min(12.0, laneThickness / 6.0) * slot;
					blockMinor = minorStart + nudge;
					blockSize = laneThickness - nudge;
				}
			}

			const QRectF rect =
				place(placed[b].start, placed[b].end, blockMinor, blockSize)
					.adjusted(grid.blockInset, grid.blockInset, -grid.blockInset, -grid.blockInset);

			planBlock(plan, document, placed[b].index, rect, now, share > 1);
		}
	}

	/* Bands: drawn last inside the group so they sit over the lanes they cross. */
	for (int e = 0; e < document.events.size(); ++e) {
		const CalendarEvent &event = document.events[e];
		if (!event.band || event.dayId != day.id)
			continue;
		if (!eventIsShown(document, event, now))
			continue;

		const EventSpan span = document.eventSpan(event);
		if (!span.valid)
			continue;

		const double start = axis.slotted ? axis.slotStart(span.startSlot) : axis.at(span.startMinutes);
		const double end = axis.slotted ? axis.slotEnd(span.endSlot) : axis.at(span.endMinutes);

		const QRectF rect =
			place(start, std::max(end, start + 1.0), plotMinor, minorExtent)
				.adjusted(grid.blockInset, grid.blockInset, -grid.blockInset, -grid.blockInset);

		planBlock(plan, document, e, rect, now, false);
	}

	/*
	 * The now-line, drawn over everything in the group and only on the day it is actually on. A line
	 * across yesterday's column would be saying something false about yesterday.
	 */
	if (document.live.nowLine && !axis.slotted && now.isValid() &&
	    (!day.date.isValid() || day.date == now.date())) {
		const int minutes = now.time().hour() * 60 + now.time().minute();
		if (minutes >= axis.startMinutes && minutes <= axis.endMinutes) {
			const double at = axis.at(minutes);
			const QRectF rect =
				place(at, at + std::max(1.0, document.live.nowLineWidth), plotMinor, minorExtent);

			PlanItem line;
			line.kind = PlanItem::Kind::Fill;
			line.rect = rect;
			line.color = document.live.nowLineColor;
			plan->items.append(line);

			if (!document.live.nowLineLabel.isEmpty()) {
				TextStyle style = grid.gutterStyle;
				style.color = document.live.nowLineColor;
				style.bold = true;

				PlanItem label;
				label.kind = PlanItem::Kind::Text;
				label.style = style;
				label.text = document.live.nowLineLabel;
				label.rect = vertical ? QRectF(origin.x(), rect.top() - style.pixelSize - 2.0,
							       std::max(gutterLead, 60.0), style.pixelSize + 2.0)
						      : QRectF(rect.left() + 4.0, origin.y() + dayHeader, 120.0,
							       style.pixelSize + 2.0);
				plan->items.append(label);
			}
		}
	}

	return QSizeF(width, height);
}

void planGrid(Plan *plan, const CalendarDocument &document, const QDateTime &now)
{
	const TimeAxis axis = buildAxis(document);
	const QVector<int> days = visibleDayIndices(document);

	/*
	 * Days run side by side only when the board asks for it *and* time runs down the side. With
	 * time across the top every day needs its own axis along the same edge, so the days can only
	 * stack -- side-by-side days would mean two time axes on one row, which is not a board anyone
	 * draws.
	 */
	const bool columns = document.grid.daysAsColumns && document.orientation == GridOrientation::TimeDown;

	QPointF cursor(0.0, 0.0);
	double width = 0.0;
	double height = 0.0;

	for (int i = 0; i < days.size(); ++i) {
		if (!columns)
			plan->breaks.append(cursor.y());

		const QSizeF size = planDayGroup(plan, document, axis, days[i], cursor, now, false);

		if (columns) {
			cursor.setX(cursor.x() + size.width() + document.grid.dayGap);
			width = cursor.x() - document.grid.dayGap;
			height = std::max(height, size.height());
		} else {
			cursor.setY(cursor.y() + size.height() + document.grid.dayGap);
			height = cursor.y() - document.grid.dayGap;
			width = std::max(width, size.width());
		}
	}

	plan->width = std::max(width, 1.0);
	plan->height = std::max(height, 1.0);
}

/* --- stacked blocks ----------------------------------------------------------------------- */

/*
 * Lanes as columns of blocks in the order they happen, each printing its own time.
 *
 * The layout an evening's schedule wants when most of the day is nothing: with no axis to hold the
 * gaps open, four events three hours apart take four blocks' worth of room rather than a column of
 * mostly empty grid.
 */
void planStacked(Plan *plan, const CalendarDocument &document, const QDateTime &now)
{
	const GridSettings &grid = document.grid;
	const QVector<int> lanes = visibleLaneIndices(document);
	const QVector<int> days = visibleDayIndices(document);

	const double dayHeader = grid.showDayHeaders ? grid.dayHeaderHeight : 0.0;
	const double laneHeader = grid.showLaneHeaders ? grid.laneHeaderSize : 0.0;
	/* A block's height here is a setting rather than a duration, since nothing measures time. */
	const double blockHeight = std::max(32.0, grid.pixelsPerHour * 0.9);

	double top = 0.0;
	double width = 0.0;

	for (int d : days) {
		const CalendarDay &day = document.days[d];
		plan->breaks.append(top);

		const double groupWidth = lanes.isEmpty()
						  ? grid.laneSize
						  : lanes.size() * grid.laneSize + (lanes.size() - 1) * grid.laneGap;

		if (dayHeader > 0.0) {
			planHeader(plan, QRectF(0.0, top, groupWidth, dayHeader), grid.dayPanel,
				   dayHeading(document, day), grid.dayStyle, daySubheading(document, day), LogoRef(),
				   false);
		}

		double tallest = 0.0;

		for (int i = 0; i < lanes.size(); ++i) {
			const CalendarLane &lane = document.lanes[lanes[i]];
			const double x = i * (grid.laneSize + grid.laneGap);
			double y = top + dayHeader;

			if (laneHeader > 0.0) {
				planHeader(plan, QRectF(x, y, grid.laneSize, laneHeader), grid.lanePanel, lane.name,
					   grid.laneStyle, lane.subLabel, grid.laneHeaderLogos ? lane.logo : LogoRef(),
					   true);
				y += laneHeader;
			}

			QVector<int> ordered;
			for (int e : document.orderedEvents()) {
				const CalendarEvent &event = document.events[e];
				if (event.dayId != day.id || event.laneId != lane.id)
					continue;
				if (!eventIsShown(document, event, now))
					continue;
				ordered.append(e);
			}

			for (int e : ordered) {
				const QRectF rect(x + grid.blockInset, y + grid.blockInset,
						  grid.laneSize - grid.blockInset * 2.0,
						  blockHeight - grid.blockInset * 2.0);
				planBlock(plan, document, e, rect, now, false);
				y += blockHeight + grid.blockGap;
			}

			tallest = std::max(tallest, y - top);
		}

		top += tallest + grid.dayGap;
		width = std::max(width, groupWidth);
	}

	plan->width = std::max(width, 1.0);
	plan->height = std::max(top - grid.dayGap, 1.0);
}

/* --- the up-next list --------------------------------------------------------------------- */

void planUpNext(Plan *plan, const CalendarDocument &document, const QDateTime &now, double canvasWidth)
{
	QVector<int> candidates;
	for (int e : document.orderedEvents()) {
		const CalendarEvent &event = document.events[e];
		if (!event.visible)
			continue;

		/*
		 * A continuation is the same event carried across a break, so a list of what is next must
		 * not offer it twice. This is the one place the link is load-bearing rather than cosmetic.
		 */
		if (!event.continuationOf.isEmpty())
			continue;

		if (!document.upNextIncludesPast && document.statusAt(event, now) == EventStatus::Finished)
			continue;

		candidates.append(e);
		if (candidates.size() >= document.upNextCount)
			break;
	}

	const double width = std::max(canvasWidth - document.marginX * 2.0, 200.0);
	double y = 0.0;

	for (int e : candidates) {
		const CalendarEvent &event = document.events[e];
		const EventStatus status = document.statusAt(event, now);
		const ResolvedBlockStyle style = document.resolveBlockStyle(event, status);
		const EventSpan span = document.eventSpan(event);

		const QRectF rect(0.0, y, width, document.upNextRowHeight);

		PlanItem panel;
		panel.kind = PlanItem::Kind::Panel;
		panel.rect = rect;
		panel.panel = style.panel;
		panel.opacity = style.opacity;
		plan->items.append(panel);

		if (style.hatch.isVisible()) {
			PlanItem hatch;
			hatch.kind = PlanItem::Kind::Hatch;
			hatch.rect = rect;
			hatch.hatch = style.hatch;
			hatch.opacity = style.opacity;
			hatch.radius = style.panel.radiusTopLeft;
			plan->items.append(hatch);
		}

		const QRectF inner = rect.adjusted(document.grid.blockPaddingX, document.grid.blockPaddingY,
						   -document.grid.blockPaddingX, -document.grid.blockPaddingY);

		/* The time reads as the row's own heading, which is what the boards this copies do. */
		if (span.valid && document.upNextTimeWidth > 0.0) {
			const QString time = formatMinutes(span.startMinutes, document.grid.gutter24Hour);
			const FittedText fitted = fitText(time, style.titleStyle, document.upNextTimeWidth, style.fit,
							  style.minPixelSize);

			PlanItem item;
			item.kind = PlanItem::Kind::Text;
			item.rect = QRectF(inner.left(), inner.top() + (inner.height() - fitted.height) / 2.0,
					   document.upNextTimeWidth, fitted.height);
			item.text = time;
			item.style = style.titleStyle;
			item.style.pixelSize = fitted.font.pixelSize();
			item.opacity = style.opacity;
			plan->items.append(item);
		}

		const double textLeft = inner.left() + (span.valid ? document.upNextTimeWidth + 20.0 : 0.0);
		const double textWidth = std::max(20.0, inner.right() - textLeft);

		const FittedText title =
			fitText(event.title, style.titleStyle, textWidth, style.fit, style.minPixelSize);
		const FittedText subtitle = event.subtitle.isEmpty()
						    ? FittedText()
						    : fitText(event.subtitle, style.subtitleStyle, textWidth, style.fit,
							      style.minPixelSize);

		const double total = title.height + subtitle.height;
		double textY = inner.top() + std::max(0.0, (inner.height() - total) / 2.0);

		PlanItem titleItem;
		titleItem.kind = PlanItem::Kind::Text;
		titleItem.rect = QRectF(textLeft, textY, textWidth, title.height);
		titleItem.text = event.title;
		titleItem.style = style.titleStyle;
		titleItem.style.pixelSize = title.font.pixelSize();
		titleItem.opacity = style.opacity;
		plan->items.append(titleItem);
		textY += title.height;

		if (!event.subtitle.isEmpty()) {
			PlanItem item;
			item.kind = PlanItem::Kind::Text;
			item.rect = QRectF(textLeft, textY, textWidth, subtitle.height);
			item.text = event.subtitle;
			item.style = style.subtitleStyle;
			item.style.pixelSize = subtitle.font.pixelSize();
			item.opacity = style.opacity;
			plan->items.append(item);
		}

		CalendarHit hit;
		hit.rect = rect;
		hit.event = e;
		plan->hits.append(hit);
		plan->placedEvents++;

		y += document.upNextRowHeight + document.upNextRowGap;
	}

	plan->width = width;
	plan->height = std::max(y - document.upNextRowGap, 1.0);
}

/* --- the free layer ----------------------------------------------------------------------- */

/* An element's box on the canvas, resolved through its anchor. */
QRectF elementRect(const CalendarElement &element, const QSizeF &canvas, double height)
{
	const double ax = elementAnchorX(element.anchor);
	const double ay = elementAnchorY(element.anchor);

	const double width = element.width > 0.0 ? element.width : canvas.width();
	const double tall = element.height > 0.0 ? element.height : height;

	/*
	 * The offset is measured inward from the edge the anchor names, which is what makes "24 px in
	 * from the bottom right" mean the same thing on a canvas of any size. A centered anchor has no
	 * inward direction, so its offset is a plain nudge.
	 */
	const double originX = canvas.width() * ax - width * ax;
	const double originY = canvas.height() * ay - tall * ay;
	const double dx = ax > 0.5 ? -element.x : element.x;
	const double dy = ay > 0.5 ? -element.y : element.y;

	return QRectF(originX + dx, originY + dy, width, tall);
}

void planElements(Plan *plan, const CalendarDocument &document, const QDateTime &now, const QSizeF &canvas)
{
	for (const CalendarElement &element : document.elements) {
		if (!element.visible)
			continue;

		const TextStyle &style = document.effectiveElementStyle(element);
		const BackgroundPanel &panel = document.effectiveElementPanel(element);

		/* What the element writes, which for four of the seven types is not `text`. */
		QString body = element.text;
		QString second;

		switch (element.type) {
		case ElementType::Clock: {
			const QDateTime shown = element.clockUsesLocalZone ? now.toLocalTime()
									   : now.toTimeZone(document.displayZone());
			body = shown.toString(element.clockFormat);
			second = element.clockZoneLabel;
			break;
		}

		case ElementType::Ticker: {
			if (element.messages.isEmpty()) {
				body = element.text;
				break;
			}
			/*
			 * Which message is showing is a function of the clock rather than of a counter the
			 * source keeps, so a board rebuilt for any other reason comes back showing the same
			 * one rather than starting the rotation over.
			 */
			const qint64 seconds = now.isValid() ? now.toSecsSinceEpoch() : 0;
			const int index = static_cast<int>((seconds / std::max(1.0, element.messageDwell))) %
					  element.messages.size();
			body = element.messages.at(index);
			break;
		}

		default:
			break;
		}

		/* Height comes from the content when the element was not given one. */
		double contentHeight = element.height;
		FittedText fitted;
		if (contentHeight <= 0.0) {
			const double inner = std::max(20.0, element.width - element.paddingX * 2.0);
			fitted = fitText(body, style, inner, TextFit::Wrap, style.pixelSize);
			contentHeight = fitted.height + element.paddingY * 2.0;
			if (!second.isEmpty())
				contentHeight += style.pixelSize * 0.8;
			if (element.type == ElementType::Image)
				contentHeight = std::max(contentHeight, static_cast<double>(element.image.maxHeight));
		}

		const QRectF rect = elementRect(element, canvas, contentHeight);

		if (panel.isVisible()) {
			PlanItem item;
			item.kind = PlanItem::Kind::Panel;
			item.rect = rect;
			item.panel = panel;
			plan->items.append(item);
		}

		const QRectF inner =
			rect.adjusted(element.paddingX, element.paddingY, -element.paddingX, -element.paddingY);

		switch (element.type) {
		case ElementType::Panel:
			break;

		case ElementType::Image: {
			PlanItem item;
			item.kind = PlanItem::Kind::Image;
			item.rect = inner;
			item.imagePath = element.image.path;
			item.imageMaxHeight = std::max(1, static_cast<int>(std::ceil(inner.height())));
			item.playback = element.image.playback;
			plan->items.append(item);
			break;
		}

		case ElementType::ChipRow: {
			double x = inner.left();
			for (const Chip &chip : element.chips) {
				const FittedText label =
					fitText(chip.label, style, inner.width(), TextFit::Ellipsize, style.pixelSize);

				const double glyphWidth = chip.glyph == TagGlyph::None && chip.imagePath.isEmpty()
								  ? 0.0
								  : label.lineHeight * tagGlyphAspect(chip.glyph) + 6.0;
				const double chipWidth = label.width + glyphWidth + element.chipPaddingX * 2.0;
				const double chipHeight = label.height + element.chipPaddingY * 2.0;
				const QRectF chipRect(x, inner.top(), chipWidth, chipHeight);

				BackgroundPanel chipPanel;
				chipPanel.fill = BackgroundFill::Color;
				chipPanel.color = chip.useColor ? chip.color : QColor(255, 255, 255, 36);
				chipPanel.setRadius(element.chipRadius);

				PlanItem back;
				back.kind = PlanItem::Kind::Panel;
				back.rect = chipRect;
				back.panel = chipPanel;
				plan->items.append(back);

				double textX = chipRect.left() + element.chipPaddingX;

				if (glyphWidth > 0.0) {
					PlanItem mark;
					mark.rect = QRectF(textX, chipRect.top() + element.chipPaddingY,
							   label.lineHeight * tagGlyphAspect(chip.glyph),
							   label.lineHeight);
					mark.style = style;

					if (chip.glyph == TagGlyph::Custom || !chip.imagePath.isEmpty()) {
						mark.kind = PlanItem::Kind::Image;
						mark.imagePath = chip.imagePath;
						mark.imageMaxHeight = static_cast<int>(std::ceil(label.lineHeight));
					} else {
						mark.kind = PlanItem::Kind::Glyph;
						mark.glyph = chip.glyph;
					}

					plan->items.append(mark);
					textX += glyphWidth;
				}

				PlanItem text;
				text.kind = PlanItem::Kind::Text;
				text.rect =
					QRectF(textX, chipRect.top() + element.chipPaddingY, label.width, label.height);
				text.text = chip.label;
				text.style = style;
				text.style.align = HAlign::Left;
				plan->items.append(text);

				x += chipWidth + element.chipGap;
			}
			break;
		}

		case ElementType::Legend: {
			struct Entry {
				QString id;
				QString label;
				QColor color;
			};

			QVector<Entry> entries;

			/*
			 * Built from what the schedule actually holds, which is what keeps a legend correct
			 * as categories are added -- and edited through the hidden list and the label
			 * overrides, which is what keeps it from being a second thing to maintain.
			 */
			if (element.legendSource == LegendSource::Categories) {
				for (const CalendarCategory &category : document.categories) {
					if (!category.inLegend)
						continue;
					entries.append({category.id, category.name,
							document.resolveStyle(category.style).panel.color});
				}
			} else if (element.legendSource == LegendSource::Lanes) {
				for (const CalendarLane &lane : document.lanes) {
					if (!lane.visible)
						continue;
					entries.append(
						{lane.id, lane.name, document.resolveStyle(lane.style).panel.color});
				}
			} else {
				for (EventStatus status : allEventStatuses()) {
					if (status == EventStatus::Auto)
						continue;
					BlockStyle layer = document.blockStyle;
					switch (status) {
					case EventStatus::Upcoming:
						layer.applyOver(document.upcomingStyle);
						break;
					case EventStatus::Live:
						layer.applyOver(document.liveStyle);
						break;
					case EventStatus::Finished:
						layer.applyOver(document.finishedStyle);
						break;
					default:
						layer.applyOver(document.cancelledStyle);
						break;
					}
					entries.append({QString::fromUtf8(eventStatusId(status)),
							QString::fromUtf8(eventStatusName(status)),
							document.resolveStyle(layer).panel.color});
				}
			}

			const int columns = std::max(1, element.legendColumns);
			const double columnWidth = inner.width() / columns;
			int shown = 0;

			for (const Entry &entry : entries) {
				if (element.legendHidden.contains(entry.id))
					continue;

				QString label = entry.label;
				for (const QString &override : element.legendLabels) {
					const int split = override.indexOf(QLatin1Char('='));
					if (split > 0 && override.left(split) == entry.id)
						label = override.mid(split + 1);
				}

				const double x = inner.left() + (shown % columns) * columnWidth;
				const double y = inner.top() +
						 (shown / columns) * (element.legendSwatchSize + element.legendGap);

				BackgroundPanel swatch;
				swatch.fill = BackgroundFill::Color;
				swatch.color = entry.color;
				swatch.setRadius(3.0);

				PlanItem mark;
				mark.kind = PlanItem::Kind::Panel;
				mark.rect = QRectF(x, y, element.legendSwatchSize, element.legendSwatchSize);
				mark.panel = swatch;
				plan->items.append(mark);

				PlanItem text;
				text.kind = PlanItem::Kind::Text;
				text.rect = QRectF(x + element.legendSwatchSize + 8.0, y,
						   columnWidth - element.legendSwatchSize - 8.0,
						   element.legendSwatchSize);
				text.text = label;
				text.style = style;
				text.style.align = HAlign::Left;
				text.fit = TextFit::Ellipsize;
				plan->items.append(text);

				++shown;
			}
			break;
		}

		default: {
			if (body.isEmpty())
				break;

			const FittedText text = fitText(body, style, inner.width(), TextFit::Wrap, style.pixelSize);

			PlanItem item;
			item.kind = PlanItem::Kind::Text;
			item.rect = QRectF(inner.left(), inner.top(), inner.width(), text.height);
			item.text = body;
			item.style = style;
			item.style.pixelSize = text.font.pixelSize();
			plan->items.append(item);

			if (!second.isEmpty()) {
				TextStyle subStyle = style;
				subStyle.pixelSize = std::max(9, static_cast<int>(style.pixelSize * 0.4));

				PlanItem sub;
				sub.kind = PlanItem::Kind::Text;
				sub.rect = QRectF(inner.left(), inner.top() + text.height, inner.width(),
						  subStyle.pixelSize * 1.4);
				sub.text = second;
				sub.style = subStyle;
				plan->items.append(sub);
			}
			break;
		}
		}
	}
}

/* --- painting ----------------------------------------------------------------------------- */

void paintHatch(QPainter *painter, const PlanItem &item)
{
	const HatchSpec &spec = item.hatch;
	if (!spec.isVisible())
		return;

	painter->save();

	QPainterPath clip;
	clip.addRoundedRect(item.rect, item.radius, item.radius);
	painter->setClipPath(clip, Qt::IntersectClip);

	QPen pen(spec.color);
	pen.setWidthF(spec.width);
	painter->setPen(pen);

	const double spacing = std::max(2.0, spec.spacing);
	const QRectF r = item.rect;

	if (spec.pattern == HatchPattern::Horizontal || spec.pattern == HatchPattern::Cross) {
		for (double y = r.top(); y <= r.bottom(); y += spacing)
			painter->drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
	}
	if (spec.pattern == HatchPattern::Vertical || spec.pattern == HatchPattern::Cross) {
		for (double x = r.left(); x <= r.right(); x += spacing)
			painter->drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
	}
	if (spec.pattern == HatchPattern::Diagonal || spec.pattern == HatchPattern::Cross) {
		for (double x = r.left() - r.height(); x <= r.right(); x += spacing)
			painter->drawLine(QPointF(x, r.bottom()), QPointF(x + r.height(), r.top()));
	}
	if (spec.pattern == HatchPattern::BackDiagonal) {
		for (double x = r.left(); x <= r.right() + r.height(); x += spacing)
			painter->drawLine(QPointF(x, r.top()), QPointF(x - r.height(), r.bottom()));
	}

	painter->restore();
}

void paintItem(QPainter *painter, const PlanItem &item, LogoCache *logos, SvgArtCache *art)
{
	painter->save();
	painter->setOpacity(std::clamp(item.opacity, 0.0, 1.0));

	switch (item.kind) {
	case PlanItem::Kind::Panel:
		paintBackgroundPanel(painter, item.panel, item.rect, logos);
		break;

	case PlanItem::Kind::Fill:
		painter->fillRect(item.rect, item.color);
		break;

	case PlanItem::Kind::Hatch:
		paintHatch(painter, item);
		break;

	case PlanItem::Kind::Text: {
		/*
		 * Re-fitted at paint time from the size the layout settled on, rather than the lines being
		 * carried in the plan. The size is what the fit decided and is carried; re-breaking the
		 * same string at the same size with the same metrics gives the same lines, and carrying a
		 * string list per block would make the plan several times its size for a board of any
		 * length.
		 */
		const FittedText fitted =
			fitText(item.text, item.style, item.rect.width(), item.fit, item.minPixelSize);
		paintLines(painter, item.rect, fitted, item.style);
		break;
	}

	case PlanItem::Kind::Image: {
		/* An animated logo is drawn over the page rather than into it, so its space is left empty. */
		if (item.hole || !logos || item.imagePath.isEmpty())
			break;

		const QImage image = logos->get(item.imagePath, std::max(1, item.imageMaxHeight));
		if (image.isNull())
			break;

		/* Fitted inside its box and centered, so artwork of any proportion keeps its shape. */
		const double scale = std::min(item.rect.width() / image.width(), item.rect.height() / image.height());
		const QSizeF size(image.width() * scale, image.height() * scale);
		const QRectF target(item.rect.left() + (item.rect.width() - size.width()) / 2.0,
				    item.rect.top() + (item.rect.height() - size.height()) / 2.0, size.width(),
				    size.height());
		painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
		painter->drawImage(target, image);
		break;
	}

	case PlanItem::Kind::Glyph: {
		if (!art)
			break;

		QSvgRenderer *renderer = art->builtIn(tagGlyphId(item.glyph), tagGlyphSvg(item.glyph));
		if (!renderer)
			break;

		/* Inked through the style beside it, exactly as a bridge tile is inked through a row's. */
		paintInkedArt(painter, item.rect, item.style, item.rect,
			      [&](QPainter *target) { renderer->render(target, item.rect); });
		break;
	}
	}

	painter->restore();
}

/*
 * Paints the part of `plan` that falls inside `clip`, with the board transformed by `scale` and
 * `offset` first.
 *
 * The clip is what makes a page cheap: a paged board rasterizes each page by painting the same plan
 * with a different offset and skipping every item that is not on it.
 */
void paintPlan(QPainter *painter, const Plan &plan, const QRectF &clip, LogoCache *logos, SvgArtCache *art)
{
	for (const PlanItem &item : plan.items) {
		/* Grown by the panel's outsets and the style's bleed, so nothing is cut at a page seam. */
		const double bleed = item.kind == PlanItem::Kind::Panel ? item.panel.bleed() : item.style.effectBleed();
		if (!item.rect.adjusted(-bleed, -bleed, bleed, bleed).intersects(clip))
			continue;

		paintItem(painter, item, logos, art);
	}
}

/* --- assembling a board ------------------------------------------------------------------- */

Plan buildPlan(const CalendarDocument &document, const QDateTime &now)
{
	Plan plan;

	switch (document.layout) {
	case CalendarLayout::UpNext:
		planUpNext(&plan, document, now, document.width);
		break;
	case CalendarLayout::Stacked:
		planStacked(&plan, document, now);
		break;
	case CalendarLayout::Grid:
	default:
		planGrid(&plan, document, now);
		break;
	}

	plan.width = std::min(plan.width, kMaxBoardExtent);
	plan.height = std::min(plan.height, kMaxBoardExtent);
	return plan;
}

/*
 * Finds the animated artwork in a plan, marks it as a hole, and reports it.
 *
 * Run once per plan, after the fit scale is known: a logo is decoded at the size it is really drawn
 * at, and on a board scaled to fit that is not the size the layout asked for. Decoding at the wrong
 * one costs either a blurred bug or a pile of frames being scaled down every time they are drawn.
 *
 * `place` maps a rectangle from the plan's own coordinates onto the canvas -- board space through
 * the scale and the page offset, or the identity for the free layer, which is already there.
 */
void collectAnimations(Plan *plan, AnimatedLogoCache *cache, QVector<CalendarAnimation> *into, int page, bool scrolls,
		       const std::function<QRectF(const QRectF &)> &place, const QRectF *within)
{
	if (!cache)
		return;

	for (PlanItem &item : plan->items) {
		if (item.kind != PlanItem::Kind::Image || item.imagePath.isEmpty())
			continue;

		/* A page draws only what is on it, so nothing is decoded or uploaded for what is not. */
		if (within && !item.rect.intersects(*within))
			continue;

		const QRectF placed = place(item.rect);
		const int height = std::max(1, static_cast<int>(std::ceil(placed.height())));

		const LogoAnimationPtr animation = cache->get(item.imagePath, height);
		if (!animation)
			continue;

		item.hole = true;

		CalendarAnimation entry;
		entry.rect = placed;
		entry.animation = animation;
		entry.playback = item.playback;
		entry.page = page;
		entry.scrolls = scrolls;
		entry.key = QStringLiteral("%1|%2|%3|%4|%5|%6")
				    .arg(item.imagePath)
				    .arg(page)
				    .arg(std::lround(placed.x()))
				    .arg(std::lround(placed.y()))
				    .arg(std::lround(placed.width()))
				    .arg(std::lround(placed.height()));
		into->append(entry);
	}
}

/*
 * The document as it will really be drawn on this machine: its own font files registered, and its
 * recorded stand-ins applied to whatever families are still missing.
 *
 * The same bargain the roll strikes, and for the same reason: what goes to air should be the
 * substitution the designer approved rather than whatever this machine's font matching lands on.
 */
const CalendarDocument &documentWithFonts(const CalendarDocument &document, CalendarDocument &storage)
{
	installBundledFonts(document.bundledFonts);

	QStringList missing;
	for (const QString &family : document.usedFontFamilies()) {
		if (!fontFamilyAvailable(family) && !document.fontSubstitute(family).isEmpty())
			missing.append(family);
	}

	if (missing.isEmpty())
		return document;

	storage = document;
	storage.applyFontSubstitutions(missing);
	return storage;
}

} // namespace

/* --- public interface --------------------------------------------------------------------- */

QSizeF CalendarRenderer::measure(const CalendarDocument &document, const QDateTime &now) const
{
	CalendarDocument storage;
	const CalendarDocument &resolved = documentWithFonts(document, storage);

	const Plan plan = buildPlan(resolved, now);
	return QSizeF(plan.width, plan.height);
}

QVector<CalendarHit> CalendarRenderer::hitBoxes(const CalendarDocument &document, const QDateTime &now) const
{
	CalendarDocument storage;
	const CalendarDocument &resolved = documentWithFonts(document, storage);

	return buildPlan(resolved, now).hits;
}

CalendarBoard CalendarRenderer::render(const CalendarDocument &document, const QDateTime &now) const
{
	CalendarDocument storage;
	const CalendarDocument &resolved = documentWithFonts(document, storage);

	CalendarBoard board;
	board.width = std::max(1, resolved.width);
	board.height = std::max(1, resolved.height);

	Plan plan = buildPlan(resolved, now);
	board.boardWidth = plan.width;
	board.boardHeight = plan.height;
	board.placedEvents = plan.placedEvents;

	/* The room the board has once the canvas margins are taken off. */
	const double availableWidth = std::max(1.0, board.width - resolved.marginX * 2.0);
	const double availableHeight = std::max(1.0, board.height - resolved.marginY * 2.0);

	const bool tooWide = plan.width > availableWidth;
	const bool tooTall = plan.height > availableHeight;
	board.overflowed = tooWide || tooTall;

	/*
	 * The free layer is planned once, in canvas coordinates, and painted onto every page: a clock
	 * pinned to a corner belongs to the canvas rather than to the board, so it neither scales with
	 * a fitted board nor travels with a scrolling one.
	 */
	Plan overlay;
	planElements(&overlay, resolved, now, QSizeF(board.width, board.height));

	/*
	 * The free layer is already in canvas coordinates and belongs to every page, so its animations
	 * are collected once, unmapped, and marked as belonging to no page in particular.
	 */
	collectAnimations(
		&overlay, animations, &board.animations, -1, false, [](const QRectF &rect) { return rect; }, nullptr);

	double scale = 1.0;
	QVector<QRectF> windows;

	/*
	 * A board smaller than its canvas is centered in it rather than left in the top corner.
	 *
	 * The alternative would be to stretch it, which is worse: a schedule stretched to fill a canvas
	 * has hours of different heights depending on how many events happen to be on it that day. So
	 * the board keeps the size its settings ask for and the empty room is shared out around it.
	 */
	double offsetX = 0.0;
	double offsetY = 0.0;

	switch (resolved.overflow.mode) {
	case OverflowMode::Fit: {
		if (board.overflowed) {
			scale = std::min(availableWidth / plan.width, availableHeight / plan.height);
			if (scale < resolved.overflow.minScale) {
				scale = resolved.overflow.minScale;
				board.clipped = true;
			}
		}
		windows.append(QRectF(0.0, 0.0, availableWidth / scale, availableHeight / scale));
		offsetX = std::max(0.0, (availableWidth - plan.width * scale) / 2.0);
		offsetY = std::max(0.0, (availableHeight - plan.height * scale) / 2.0);
		break;
	}

	case OverflowMode::Page: {
		/*
		 * Pages are cut at the breaks the layout left -- the top of each day -- when the board asks
		 * for it, and at canvas boundaries otherwise. A three-day board paged by pixels puts the
		 * end of Friday and the start of Saturday on one page, which is not a page anybody meant.
		 */
		QVector<double> starts;
		if (resolved.overflow.pageByDay && !plan.breaks.isEmpty()) {
			starts = plan.breaks;
		} else {
			for (double top = 0.0; top < plan.height; top += availableHeight)
				starts.append(top);
		}
		if (starts.isEmpty())
			starts.append(0.0);

		for (double top : starts)
			windows.append(QRectF(0.0, top, availableWidth, availableHeight));

		/* Centered across, never down: down is the direction the pages are cut in. */
		offsetX = std::max(0.0, (availableWidth - plan.width) / 2.0);
		break;
	}

	case OverflowMode::Scroll:
	default:
		/*
		 * A scrolling board is one page as tall as the whole board: the source moves it past the
		 * canvas rather than the renderer cutting it up, exactly as a credit roll's strip works.
		 */
		windows.append(QRectF(0.0, 0.0, availableWidth, std::max(plan.height, availableHeight)));
		offsetX = std::max(0.0, (availableWidth - plan.width) / 2.0);
		break;
	}

	SvgArtCache art;

	for (int p = 0; p < windows.size(); ++p) {
		const QRectF &window = windows[p];

		/* Board coordinates onto the canvas: through the fit scale, then the page's own offset. */
		const auto toCanvas = [&](const QRectF &rect) {
			return QRectF(resolved.marginX + offsetX + (rect.left() - window.left()) * scale,
				      resolved.marginY + offsetY + (rect.top() - window.top()) * scale,
				      rect.width() * scale, rect.height() * scale);
		};

		/*
		 * Before the page is rasterized, because this is what decides which artwork the page leaves
		 * a hole for -- and it is after the scale is settled, so each animation is decoded at the
		 * size it is really drawn at rather than the size the layout asked for.
		 */
		collectAnimations(&plan, animations, &board.animations, p, true, toCanvas, &window);

		const int pageHeight = resolved.overflow.mode == OverflowMode::Scroll
					       ? static_cast<int>(std::ceil(window.height() + resolved.marginY * 2.0))
					       : board.height;

		CalendarPage page;
		page.height = pageHeight;

		for (int top = 0; top < pageHeight; top += kTileHeight) {
			const int height = std::min(kTileHeight, pageHeight - top);

			QImage image(board.width, height, QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::transparent);

			QPainter painter(&image);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setRenderHint(QPainter::TextAntialiasing, true);
			painter.translate(0.0, -top);

			painter.save();
			painter.translate(resolved.marginX + offsetX, resolved.marginY + offsetY);
			painter.scale(scale, scale);
			painter.translate(-window.left(), -window.top());

			const QRectF clip(window.left() - 1.0,
					  window.top() + (top - resolved.marginY - offsetY) / scale - 1.0,
					  window.width() + 2.0, height / scale + 2.0);
			paintPlan(&painter, plan, clip, logos, &art);
			painter.restore();

			painter.end();

			StripTile tile;
			tile.top = top;
			/* QPainter paints premultiplied; OBS composites straight alpha. */
			tile.image = image.convertToFormat(QImage::Format_ARGB32);
			page.tiles.append(tile);
		}

		board.pages.append(page);

		/* Hit boxes are reported in canvas space, page by page, which is where a click arrives. */
		for (const CalendarHit &hit : plan.hits) {
			/* A block on another page is not under the pointer while this one is showing. */
			if (!hit.rect.intersects(window))
				continue;

			CalendarHit mapped;
			mapped.rect = toCanvas(hit.rect);
			mapped.event = hit.event;
			mapped.page = p;
			board.hits.append(mapped);
		}
	}

	/*
	 * The free layer, painted once into a canvas-sized picture of its own. The source draws it over
	 * whichever page is showing, at the canvas's own coordinates -- so it neither scales with a
	 * fitted board nor travels with a scrolling one.
	 */
	if (!overlay.items.isEmpty()) {
		QImage image(board.width, board.height, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);

		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setRenderHint(QPainter::TextAntialiasing, true);
		paintPlan(&painter, overlay, QRectF(0.0, 0.0, board.width, board.height), logos, &art);
		painter.end();

		/* QPainter paints premultiplied; OBS composites straight alpha. */
		board.overlay = image.convertToFormat(QImage::Format_ARGB32);
	}

	board.scale = scale;
	return board;
}

} // namespace closingtime
