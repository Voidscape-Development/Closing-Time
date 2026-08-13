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

#include "render/StripRenderer.hpp"

#include <plugin-support.h>

#include <QDateTime>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRawFont>
#include <QTextLayout>
#include <QTextOption>
#include <QtMath>

#include <algorithm>
#include <cmath>

#include "render/BridgeArtRenderer.hpp"
#include "render/ImageEffects.hpp"

namespace closingtime {

namespace {

/*
 * Hard ceiling on the height of a rendered strip. A roll this long already runs for the
 * better part of an hour at the default speed; the cap exists so a runaway import cannot
 * try to allocate an unbounded pile of tiles.
 */
constexpr int kMaxStripHeight = 200000;

/* Drawn in place of a logo whose file is missing or undecodable. */
constexpr int kPlaceholderAspectNumerator = 2;

/*
 * A line width wide enough that nothing ever breaks against it, for the passes that want a
 * run's natural width rather than a wrapped one.
 */
constexpr qreal kUnboundedWidth = 1.0e7;

/* Largest buffer a logo's shadow is softened in, in pixels. A blur past this is drawn hard. */
constexpr qint64 kMaxLogoShadowPixels = 64 * 1024 * 1024;

QFont makeFont(const TextStyle &style)
{
	QFont font(style.family);
	/*
	 * Pixel sizing, not point sizing: the strip is laid out in video pixels and must come
	 * out identical regardless of the DPI of whatever screen OBS happens to be on.
	 */
	font.setPixelSize(std::max(1, style.pixelSize));
	font.setBold(style.bold);
	font.setItalic(style.italic);
	return font;
}

/*
 * The paragraph as QTextLayout wants it. QTextLayout only breaks on explicit line separators,
 * not on the plain newlines the user types, so everything that lays a run out -- measuring or
 * painting -- has to go through the same substitution or the two disagree about line count.
 */
QString layoutContent(const QString &text)
{
	QString content = text;
	content.replace(QLatin1Char('\n'), QChar::LineSeparator);
	return content;
}

qreal alignOffset(HAlign align, qreal available, qreal used)
{
	switch (align) {
	case HAlign::Left:
		return 0.0;
	case HAlign::Right:
		return available - used;
	case HAlign::Center:
	default:
		return (available - used) / 2.0;
	}
}

/*
 * The glyphs of a laid-out run as one filled path, positioned at `origin`.
 *
 * Only styles that need more than a pen colour go through this: an outline has to be stroked
 * around the letterforms and a shadow has to be cast by their silhouette, neither of which
 * QTextLayout::draw() can be asked for. Going by way of QRawFont::pathForGlyph keeps the
 * shaping QTextLayout already did -- ligatures, marks and bidi runs all stay as laid out --
 * which reconstructing the path from the source string would not.
 */
QPainterPath glyphPath(const QTextLayout &layout, const QPointF &origin)
{
	QPainterPath path;
	path.setFillRule(Qt::WindingFill);

	const QList<QGlyphRun> runs = layout.glyphRuns();
	for (const QGlyphRun &run : runs) {
		const QRawFont font = run.rawFont();
		const QList<quint32> indexes = run.glyphIndexes();
		const QList<QPointF> positions = run.positions();

		const int count = std::min(indexes.size(), positions.size());
		for (int i = 0; i < count; ++i) {
			QPainterPath glyph = font.pathForGlyph(indexes.at(i));
			if (glyph.isEmpty())
				continue;

			glyph.translate(origin + positions.at(i));
			path.addPath(glyph);
		}
	}

	return path;
}

/* The pen an outline is stroked with, or a null pen when the style has no outline. */
QPen outlinePen(const TextStyle &style)
{
	if (!style.outline.enabled || style.outline.width <= 0.0)
		return QPen(Qt::NoPen);

	/*
	 * Twice the width the user asked for, because a stroke straddles the path it follows.
	 * The fill goes over the top afterwards and covers the inner half back up, which
	 * leaves exactly `width` pixels of outline outside the letterform.
	 */
	QPen pen(style.outline.color, style.outline.width * 2.0);
	pen.setJoinStyle(Qt::RoundJoin);
	pen.setCapStyle(Qt::RoundCap);
	return pen;
}

/* Largest shadow buffer we will allocate, in pixels. A blur past this is drawn hard instead. */
constexpr qint64 kMaxShadowPixels = 64 * 1024 * 1024;

void paintShadow(QPainter *painter, const QPainterPath &path, const TextStyle &style)
{
	const TextShadow &shadow = style.shadow;
	const QPen pen = outlinePen(style);
	/* The shadow is cast by the outline too, so it has to be grown by the same stroke. */
	const qreal grow = pen.style() == Qt::NoPen ? 0.0 : pen.widthF() / 2.0;

	const QPainterPath offset = path.translated(shadow.offsetX, shadow.offsetY);
	const QRectF shape = offset.boundingRect().adjusted(-grow, -grow, grow, grow);

	const int radius = std::clamp(qRound(shadow.blur / 2.0), 0, 100);
	if (radius < 1) {
		painter->save();
		painter->setPen(pen.style() == Qt::NoPen
					? QPen(Qt::NoPen)
					: QPen(shadow.color, pen.widthF(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		painter->setBrush(shadow.color);
		painter->drawPath(offset);
		painter->restore();
		return;
	}

	/* Three passes of box radius r reach 3r, so that is the margin the blur needs. */
	const int margin = radius * 3 + 1;
	const QRect bounds = shape.toAlignedRect().adjusted(-margin, -margin, margin, margin);
	if (bounds.isEmpty())
		return;

	if (static_cast<qint64>(bounds.width()) * bounds.height() > kMaxShadowPixels) {
		obs_log(LOG_WARNING, "text shadow blur too large to buffer; drawing it hard instead");
		painter->save();
		painter->setBrush(shadow.color);
		painter->setPen(Qt::NoPen);
		painter->drawPath(offset);
		painter->restore();
		return;
	}

	QImage buffer(bounds.size(), QImage::Format_ARGB32_Premultiplied);
	buffer.fill(Qt::transparent);

	QPainter shadowPainter(&buffer);
	shadowPainter.setRenderHint(QPainter::Antialiasing);
	shadowPainter.translate(-bounds.topLeft());
	shadowPainter.setBrush(shadow.color);
	shadowPainter.setPen(pen.style() == Qt::NoPen
				     ? QPen(Qt::NoPen)
				     : QPen(shadow.color, pen.widthF(), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	shadowPainter.drawPath(offset);
	shadowPainter.end();

	blurImage(buffer, radius);

	painter->drawImage(bounds.topLeft(), buffer);
}

/*
 * Draws one laid-out run with everything its style asks for. `box` is the run's block, used
 * to map a gradient; see fillBrush.
 *
 * None of this changes where anything sits: an outline and a shadow paint outside the block
 * without growing it, the way a CSS text-shadow does. StripRenderer::render widens which
 * sections it visits per tile by the same amount, so a shadow cast across a tile boundary is
 * still drawn rather than cut off at the seam.
 */
void paintStyledLayout(QPainter *painter, const QTextLayout &layout, const TextStyle &style, const QPointF &origin,
		       const QRectF &box)
{
	if (!style.hasEffects()) {
		painter->setPen(style.color);
		layout.draw(painter, origin);
		return;
	}

	const QPainterPath path = glyphPath(layout, origin);
	if (path.isEmpty())
		return;

	painter->save();

	if (style.shadow.enabled)
		paintShadow(painter, path, style);

	const QPen pen = outlinePen(style);
	if (pen.style() != Qt::NoPen) {
		painter->setPen(pen);
		painter->setBrush(Qt::NoBrush);
		painter->drawPath(path);
	}

	painter->setPen(Qt::NoPen);
	painter->fillPath(path, textFillBrush(style, box));

	painter->restore();
}

/*
 * Lays `text` out into `width` pixels with an already-prepared font, optionally painting it
 * at (x, y), and returns the height it occupies. Passing a null painter measures without
 * rasterising, which is how the two-pass layout keeps measurement and painting from
 * drifting apart.
 *
 * `font` and `align` are passed separately from `style` because the bridge of a Bridged
 * section is drawn in the section's style but with a stretched font and its own alignment.
 *
 * `wrap` is off for that bridge: a leader sized to the gap can round a fraction of a pixel
 * over it, and overflowing by that hair reads far better than silently becoming two rows of
 * dots.
 */
int layoutPreparedText(QPainter *painter, const QString &text, const TextStyle &style, const QFont &font, HAlign align,
		       qreal x, qreal y, qreal width, bool wrap = true)
{
	if (text.isEmpty() || width <= 0.0)
		return 0;

	const QFontMetricsF metrics(font);

	QTextLayout layout(layoutContent(text), font);
	QTextOption option;
	option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
	/*
	 * Alignment is deliberately left off the text option and applied per line below.
	 * Setting both makes QTextLayout offset the line and then get offset again here,
	 * which pushes centred text a full half-width to the right.
	 */
	layout.setTextOption(option);

	const qreal step = metrics.lineSpacing() * style.lineSpacing;

	/* Tracked while laying out so a gradient spans what the lines cover, not the column. */
	qreal inkedLeft = width;
	qreal inkedRight = 0.0;

	qreal cursor = 0.0;
	layout.beginLayout();
	for (;;) {
		QTextLine line = layout.createLine();
		if (!line.isValid())
			break;

		line.setLineWidth(width);

		const qreal lineWidth = line.naturalTextWidth();
		const qreal lineX = alignOffset(align, width, lineWidth);
		line.setPosition(QPointF(lineX, cursor));

		inkedLeft = std::min(inkedLeft, lineX);
		inkedRight = std::max(inkedRight, lineX + lineWidth);

		cursor += step;
	}
	layout.endLayout();

	if (painter) {
		const QRectF box(x + inkedLeft, y, std::max(0.0, inkedRight - inkedLeft), cursor);
		paintStyledLayout(painter, layout, style, QPointF(x, y), box);
	}

	return qCeil(cursor);
}

int layoutText(QPainter *painter, const QString &text, const TextStyle &style, qreal x, qreal y, qreal width)
{
	return layoutPreparedText(painter, text, style, makeFont(style), style.align, x, y, width);
}

/*
 * Width the text wants on its widest line, before any wrapping is imposed on it.
 *
 * Measured through the same QTextLayout the paint pass lays the run out with, rather than
 * through QFontMetricsF, and rounded up to a whole pixel. A column sized from this is handed
 * straight back to that layout as its line width, so a natural width landing a fraction of a
 * pixel under what the layout then asks for is enough to break the line -- text wrapping
 * inside a column measured to fit it, with the space it could have used sitting empty beside
 * it. Rounding up costs at most a pixel of column and cannot round the other way.
 */
qreal naturalTextWidth(const QString &text, const TextStyle &style)
{
	if (text.isEmpty())
		return 0.0;

	QTextLayout layout(layoutContent(text), makeFont(style));
	QTextOption option;
	option.setWrapMode(QTextOption::NoWrap);
	layout.setTextOption(option);

	qreal widest = 0.0;
	layout.beginLayout();
	for (;;) {
		QTextLine line = layout.createLine();
		if (!line.isValid())
			break;

		line.setLineWidth(kUnboundedWidth);
		widest = std::max(widest, line.naturalTextWidth());
	}
	layout.endLayout();

	return std::ceil(widest);
}

/*
 * Distance from the top of a run of this text down to the baseline of its first line, when laid
 * out into `width` pixels.
 *
 * Taken from the laid-out line rather than from QFontMetricsF::ascent(), because the line is
 * what the paint pass positions the glyphs against. The two agree for a run set entirely in the
 * family's own engine and part company as soon as anything else supplies a glyph -- a fallback
 * font for one character, a different engine picked for the run's script -- and a row whose
 * parts are measured against one ascent and drawn against another sits its sides a pixel or two
 * apart for no reason the document can explain.
 *
 * Empty text has no baseline to share and reports none, so a bridged row with one side blank is
 * anchored on the side that is actually there.
 */
qreal firstBaseline(const QString &text, const TextStyle &style, qreal width)
{
	if (text.isEmpty())
		return 0.0;

	const QFont font = makeFont(style);
	QTextLayout layout(layoutContent(text), font);
	QTextOption option;
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	layout.setTextOption(option);

	layout.beginLayout();
	QTextLine line = layout.createLine();
	if (line.isValid())
		line.setLineWidth(std::max(0.0, width));
	layout.endLayout();

	/* A width too small to lay anything out into still has the font's own ascent to offer. */
	return line.isValid() ? line.ascent() : QFontMetricsF(font).ascent();
}

/* The size a logo will be drawn at once it is fitted into `available` pixels of width. */
QSize logoDrawSize(const QImage &image, const LogoRef &ref, int available)
{
	const int maxHeight = std::max(1, ref.maxHeight);

	if (image.isNull()) {
		/* Placeholder keeps the layout stable while artwork is still being chosen. */
		const int width = std::min(available, maxHeight * kPlaceholderAspectNumerator);
		return QSize(std::max(1, width), maxHeight);
	}

	QSize size = image.size();
	size.scale(QSize(std::max(1, available), maxHeight), Qt::KeepAspectRatio);
	return size;
}

/*
 * The shadow a logo casts, softened exactly the way the text's and the bridge art's are.
 *
 * The artwork is its own silhouette -- its alpha is the shape -- so there is no path to offset
 * here, only the image recoloured to the shadow's ink at the size it is about to be drawn. Like
 * every other effect in the renderer this paints outside the section's box without growing it,
 * which is why `effectBleed` has to count the sections that place logos as well as the ones that
 * set text.
 */
void paintLogoShadow(QPainter *painter, const QImage &image, const QRect &rect, const TextShadow &shadow)
{
	if (rect.isEmpty())
		return;

	const QImage ink = tintedImage(image.scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
					       .convertToFormat(QImage::Format_ARGB32_Premultiplied),
				       shadow.color);
	if (ink.isNull())
		return;

	const QPointF at = QPointF(rect.topLeft()) + QPointF(shadow.offsetX, shadow.offsetY);
	const int radius = std::clamp(qRound(shadow.blur / 2.0), 0, 100);

	if (radius < 1) {
		painter->drawImage(at, ink);
		return;
	}

	/* Three passes of box radius r reach 3r, so that is the margin the blur needs. */
	const int margin = radius * 3 + 1;
	const QSize size(ink.width() + margin * 2, ink.height() + margin * 2);

	if (static_cast<qint64>(size.width()) * size.height() > kMaxLogoShadowPixels) {
		obs_log(LOG_WARNING, "logo shadow blur too large to buffer; drawing it hard instead");
		painter->drawImage(at, ink);
		return;
	}

	QImage buffer(size, QImage::Format_ARGB32_Premultiplied);
	if (buffer.isNull())
		return;

	buffer.fill(Qt::transparent);

	QPainter bufferPainter(&buffer);
	bufferPainter.drawImage(QPoint(margin, margin), ink);
	bufferPainter.end();

	blurImage(buffer, radius);

	painter->drawImage(at - QPointF(margin, margin), buffer);
}

void paintLogo(QPainter *painter, const QImage &image, const QRect &rect, const TextStyle &style)
{
	if (!painter)
		return;

	if (image.isNull()) {
		painter->save();
		QPen pen(QColor(160, 160, 160, 180));
		pen.setStyle(Qt::DashLine);
		pen.setWidth(2);
		painter->setPen(pen);
		painter->setBrush(Qt::NoBrush);
		painter->drawRect(rect.adjusted(1, 1, -1, -1));
		painter->restore();
		return;
	}

	if (style.shadow.enabled)
		paintLogoShadow(painter, image, rect, style.shadow);

	painter->drawImage(rect, image);
}

/*
 * Gathers the boxes the layout puts things in, for the designer's overlay.
 *
 * Null everywhere except the one measure pass that is asked for them, so the layout code can
 * report a rectangle without checking first whether anyone is listening, and the source's own
 * renders carry no cost for a feature only the designer uses.
 */
struct BoxCollector {
	LayoutBoxes *boxes = nullptr;
	int section = -1;

	void add(LayoutBox::Kind kind, const QRectF &rect)
	{
		/* Nothing was placed, so there is nothing to outline: an empty entry, a missing logo. */
		if (rect.width() <= 0.0 || rect.height() <= 0.0)
			return;

		boxes->append(LayoutBox{kind, section, rect});
	}
};

/* Where the logo and text of a "... w/ Logo" row sit horizontally. */
struct LogoRow {
	qreal logoX = 0.0;
	qreal textX = 0.0;
	qreal textWidth = 0.0;
	qreal bridgeX = 0.0;
	qreal bridgeWidth = 0.0;
};

/*
 * Divides a "... w/ Logo" row between its logo and its text.
 *
 * The three placements differ in what the text is allowed to be wide: everything left over
 * (Edge), only what it needs (Hug), or only what it needs while parked against the far edge
 * (Bridged). Hug is the one that makes `logoGap` mean what it looks like it means, because
 * the pair is measured together and then aligned as a unit -- under Edge the text aligns
 * inside the whole remaining column instead, and drifts away from the logo.
 *
 * A Hug group is placed by the section's own placement, not by the text's alignment. Edge and
 * Bridged both consume the whole section box, so the box -- and therefore `sectionAlign` -- is
 * what says where they land; a Hug group is narrower than its box, and aligning it by the text
 * instead left the one setting named after placing a section unable to move it. The text's
 * alignment still does its own job inside the column, which is where a wrapped or multi-line
 * title needs it.
 */
LogoRow placeLogoRow(const Section &section, const TextStyle &style, qreal contentX, qreal contentWidth,
		     qreal logoWidth)
{
	const bool onLeft = section.logoSide == LogoSide::Left;
	/* Nothing to separate the logo from when there is no text. */
	const qreal gap = section.text.isEmpty() ? 0.0 : section.logoGap;

	LogoRow row;

	switch (section.logoPlacement) {
	case LogoPlacement::Edge:
		row.textWidth = std::max(0.0, contentWidth - logoWidth - gap);
		row.logoX = onLeft ? contentX : contentX + contentWidth - logoWidth;
		row.textX = onLeft ? contentX + logoWidth + gap : contentX;
		break;

	case LogoPlacement::Hug: {
		row.textWidth =
			std::min(naturalTextWidth(section.text, style), std::max(0.0, contentWidth - logoWidth - gap));

		const qreal groupWidth = logoWidth + gap + row.textWidth;
		const qreal groupX = contentX + alignOffset(section.sectionAlign, contentWidth, groupWidth);

		row.logoX = onLeft ? groupX : groupX + row.textWidth + gap;
		row.textX = onLeft ? groupX + logoWidth + gap : groupX;
		break;
	}

	case LogoPlacement::Bridged: {
		row.textWidth =
			std::min(naturalTextWidth(section.text, style), std::max(0.0, contentWidth - logoWidth));

		row.logoX = onLeft ? contentX : contentX + contentWidth - logoWidth;
		row.textX = onLeft ? contentX + contentWidth - row.textWidth : contentX;

		/* The gap becomes padding at each end, so the leader touches neither cap. */
		const qreal span = std::max(0.0, contentWidth - logoWidth - row.textWidth);
		row.bridgeWidth = std::max(0.0, span - gap * 2.0);
		row.bridgeX = (onLeft ? contentX + logoWidth : contentX + row.textWidth) + gap;
		break;
	}
	}

	return row;
}

/* Where the three parts of one bridged row sit horizontally. */
struct BridgedRow {
	qreal leftX = 0.0;
	qreal leftWidth = 0.0;
	qreal bridgeX = 0.0;
	qreal bridgeWidth = 0.0;
	qreal rightX = 0.0;
	qreal rightWidth = 0.0;
};

/*
 * Divides one row's width between the two texts and the bridge.
 *
 * The two sizing modes differ only in how the text columns are measured; from there a
 * single placement path covers every combination. Whatever the columns leave over becomes
 * the bridge, and the row is then aligned within the section -- which only moves anything
 * for a Fixed bridge with Natural sizing, since every other combination has already
 * consumed the full width.
 */
BridgedRow placeBridgedRow(const Section &section, const TextStyle &leftStyle, const TextStyle &rightStyle,
			   const Entry &entry, qreal contentX, qreal contentWidth, qreal naturalBridge)
{
	qreal leftWidth = 0.0;
	qreal rightWidth = 0.0;

	if (section.bridgeSizing == BridgeSizing::Split) {
		/*
		 * The ratio divides the space the two texts share rather than the whole width,
		 * so a Fixed bridge at the default 0.5 lands exactly where Bridged sections
		 * have always drawn it. With a filling bridge there is nothing to reserve, and
		 * the ratio becomes a plain tab stop for the leader to start at.
		 */
		const qreal reserved = section.bridgeFill == BridgeFill::Fixed ? naturalBridge : 0.0;
		leftWidth = section.bridgeSplit * std::max(0.0, contentWidth - reserved);

		if (section.bridgeFill == BridgeFill::Fixed) {
			rightWidth = std::max(0.0, contentWidth - leftWidth - naturalBridge);
		} else {
			rightWidth = std::min(naturalTextWidth(entry.secondaryText, rightStyle),
					      std::max(0.0, contentWidth - leftWidth));

			/*
			 * With a filling bridge the split is a tab stop rather than a cap: there is
			 * nothing reserved on the other side of it, so a left text that overruns it
			 * takes what it needs and pushes the bridge along instead of wrapping inside
			 * its column with the gap beside it left empty. Rows that do fit still start
			 * their bridge at the same x, which is the whole point of the setting.
			 */
			leftWidth = std::clamp(naturalTextWidth(entry.text, leftStyle), leftWidth,
					       std::max(leftWidth, contentWidth - rightWidth));
		}
	} else {
		leftWidth = naturalTextWidth(entry.text, leftStyle);
		rightWidth = naturalTextWidth(entry.secondaryText, rightStyle);

		/*
		 * Overlong rows shrink both sides in proportion rather than letting whichever
		 * text comes first swallow the row and wrap the other one out of existence.
		 */
		const qreal wanted = leftWidth + rightWidth;
		if (wanted > contentWidth && wanted > 0.0) {
			leftWidth = contentWidth * (leftWidth / wanted);
			rightWidth = contentWidth - leftWidth;
		}
	}

	/*
	 * A side with nothing on it hands its column to the bridge, when asked to. Confined to
	 * a filling bridge: a fixed one has nothing to cover the freed space with, so all
	 * collapsing the column would do there is shorten the row. Keeping it out of that case
	 * is also what leaves Natural sizing with a Fixed bridge as the only way to get a row
	 * narrower than the section -- which is exactly where the row placement control shows.
	 */
	if (section.bridgeFill != BridgeFill::Fixed) {
		if (section.bridgeSpanEmpty && entry.text.isEmpty())
			leftWidth = 0.0;
		if (section.bridgeSpanEmpty && entry.secondaryText.isEmpty())
			rightWidth = 0.0;
	}

	const qreal slack = std::max(0.0, contentWidth - leftWidth - rightWidth);

	BridgedRow row;
	row.leftWidth = leftWidth;
	row.rightWidth = rightWidth;
	row.bridgeWidth = section.bridgeFill == BridgeFill::Fixed ? std::min(naturalBridge, slack) : slack;

	const qreal rowWidth = leftWidth + row.bridgeWidth + rightWidth;
	row.leftX = contentX + alignOffset(section.bridgeRowAlign, contentWidth, rowWidth);
	row.bridgeX = row.leftX + leftWidth;
	row.rightX = row.bridgeX + row.bridgeWidth;

	return row;
}

/*
 * The bridge as it will actually be drawn across a given gap.
 *
 * Both kinds of bridge end up here: a text bridge as the string and the font that makes it
 * span the gap, an art bridge as the tiles laid across it. Everything downstream -- where the
 * row's baseline lands, how tall the row is, what gets painted -- goes through this rather
 * than asking which kind it is, so the two stay interchangeable at every call site.
 */
struct PreparedBridge {
	QString text;
	QFont font;

	bool usesArt = false;
	BridgeArtLayout art;

	bool isEmpty() const { return usesArt ? art.isEmpty() : text.isEmpty(); }

	/*
	 * How far the bridge reaches above the row's baseline. Art rests on that baseline the way
	 * a run of leader dots does, raised by `bridgeOffset` when what the user wants is a rule
	 * running through the middle of the text instead; a text bridge shares the baseline with
	 * the words either side of it, as any other run of glyphs would.
	 */
	qreal ascent(const Section &section) const
	{
		if (isEmpty())
			return 0.0;

		return usesArt ? art.height + section.bridgeOffset : QFontMetricsF(font).ascent();
	}
};

PreparedBridge prepareTextBridge(const Section &section, const TextStyle &style, qreal width)
{
	PreparedBridge prepared;
	prepared.font = makeFont(style);

	if (section.bridge.isEmpty() || width <= 0.0)
		return prepared;

	const qreal unit = QFontMetricsF(prepared.font).horizontalAdvance(section.bridge);
	if (unit <= 0.0)
		return prepared;

	switch (section.bridgeFill) {
	case BridgeFill::Fixed:
		prepared.text = section.bridge;
		break;

	case BridgeFill::Repeat: {
		/*
		 * Whole copies only, centred in the gap by the caller. A partial copy would cut
		 * a leader mid-glyph, which reads as damage rather than design once the bridge
		 * is a word instead of a run of dots. The leftover is at most one copy wide, so
		 * a short bridge unit is what makes this look tight.
		 */
		const int copies = static_cast<int>(width / unit);
		if (copies > 0)
			prepared.text = section.bridge.repeated(copies);
		break;
	}

	case BridgeFill::Stretch:
		if (width < unit)
			break;

		prepared.text = section.bridge;
		/*
		 * Qt adds the spacing after every character including the last, so dividing by
		 * the full length lands the run's advance on `width` exactly instead of
		 * overshooting it by one gap.
		 */
		prepared.font.setLetterSpacing(QFont::AbsoluteSpacing,
					       (width - unit) / static_cast<qreal>(section.bridge.size()));
		break;
	}

	return prepared;
}

PreparedBridge prepareBridge(const Section &section, const TextStyle &style, BridgeArtCache *bridges, qreal width)
{
	if (!bridgeTypeUsesArt(section.bridgeType))
		return prepareTextBridge(section, style, width);

	PreparedBridge prepared;
	prepared.font = makeFont(style);
	prepared.usesArt = true;
	prepared.art = layoutBridgeArt(section, bridges, width);
	return prepared;
}

/* The width one copy of the bridge wants, which is what a Fixed bridge reserves from a row. */
qreal naturalBridgeWidth(const Section &section, const TextStyle &style, BridgeArtCache *bridges)
{
	if (!bridgeTypeUsesArt(section.bridgeType))
		return naturalTextWidth(section.bridge, style);

	/* The gaps either end are part of what the art has to be given room for. */
	const qreal tile = bridgeTileWidth(section, bridges);
	return tile > 0.0 ? tile + std::max(0.0, section.bridgeGap) * 2.0 : 0.0;
}

/*
 * Draws the bridge with its top edge at `top` and returns the height it occupies. With a null
 * painter it only measures, like the rest of the layout.
 */
int paintBridge(QPainter *painter, const PreparedBridge &bridge, const Section &section, const TextStyle &style,
		BridgeArtCache *bridges, qreal x, qreal top, qreal width)
{
	if (bridge.isEmpty())
		return 0;

	if (!bridge.usesArt)
		return layoutPreparedText(painter, bridge.text, style, bridge.font, HAlign::Center, x, top, width,
					  false);

	if (painter) {
		/*
		 * A gradient is mapped over a line of the section's text rather than over the art's
		 * own few pixels of height, so a leader shows the same slice of the sweep as the
		 * words either side of it instead of the whole of it crammed into a run of dots.
		 */
		const QFontMetricsF metrics(bridge.font);
		const qreal baseline = top + bridge.ascent(section);
		const QRectF fillBox(x, baseline - metrics.ascent(), width, metrics.height());

		paintBridgeArt(painter, bridge.art, section, style, bridges, QPointF(x, top), fillBox);
	}

	return qCeil(bridge.art.height);
}

/*
 * Both passes of the layout run through this one function. With `painter` set it draws
 * into the current tile; with `painter` null it only reports the height. `top` is the
 * section's Y position in strip space, which is also painter space -- callers translate
 * the painter by the tile offset before drawing.
 *
 * `boxes`, when given, collects the rectangles things were placed in for the designer's layout
 * overlay. Only the measure pass is ever asked for them -- see BoxCollector.
 */
int layoutSection(QPainter *painter, const Section &section, const Document &document, LogoCache *logos,
		  BridgeArtCache *bridges, int top, BoxCollector *boxes = nullptr)
{
	/* Reads as one call at every site whether or not anyone is collecting. */
	const auto record = [boxes](LayoutBox::Kind kind, const QRectF &rect) {
		if (boxes)
			boxes->add(kind, rect);
	};

	/*
	 * The section's box: a share of the canvas width, placed within it by `sectionAlign`, with
	 * `marginX` taken off each of the box's own edges. A margin alone can only ever centre the
	 * content, since it insets both sides equally; the box is what lets a section sit against
	 * one edge of the canvas with the margin still holding it clear of that edge.
	 *
	 * At the defaults -- the full width, centred -- the box is the canvas and this is exactly
	 * the inset from both edges that it has always been.
	 */
	const qreal boxWidth = std::clamp(section.sectionWidth, 0.0, 1.0) * document.width;
	const int boxX = qRound(alignOffset(section.sectionAlign, document.width, boxWidth));

	const int contentX = boxX + section.marginX;
	const int contentWidth = std::max(1, qRound(boxWidth) - section.marginX * 2);

	/* Resolved once here so nothing below can accidentally bypass a preset binding. */
	const TextStyle &style = document.effectiveStyle(section);

	int y = top + section.paddingTop;
	const int contentTop = y;

	switch (section.type) {
	case SectionType::Spacer:
		y += section.spacerHeight;
		break;

	case SectionType::Title:
	case SectionType::Header: {
		const int height = layoutText(painter, section.text, style, contentX, y, contentWidth);
		record(LayoutBox::Kind::Text, QRectF(contentX, y, contentWidth, height));
		y += height;
		break;
	}

	case SectionType::LogoTitle:
	case SectionType::LogoHeader: {
		const QImage image = logos->get(section.logo.path, section.logo.maxHeight);
		const QSize size = logoDrawSize(image, section.logo, contentWidth);
		const int x = contentX + qRound(alignOffset(style.align, contentWidth, size.width()));
		const QRect box(QPoint(x, y), size);
		paintLogo(painter, image, box, style);
		record(LayoutBox::Kind::Logo, QRectF(box));
		y += size.height();
		break;
	}

	case SectionType::TitleWithLogo:
	case SectionType::HeaderWithLogo: {
		const QImage image = logos->get(section.logo.path, section.logo.maxHeight);
		const int logoBudget = std::max(1, contentWidth / 3);
		const QSize logoSize = logoDrawSize(image, section.logo, logoBudget);

		const LogoRow row = placeLogoRow(section, style, contentX, contentWidth, logoSize.width());

		const int textHeight = layoutText(nullptr, section.text, style, 0, 0, row.textWidth);
		const int rowHeight = std::max(logoSize.height(), textHeight);
		const qreal textTop = y + (rowHeight - textHeight) / 2.0;

		/* The logo and the text are centred against each other within the row. */
		const QRect logoBox(QPoint(qRound(row.logoX), y + (rowHeight - logoSize.height()) / 2), logoSize);
		paintLogo(painter, image, logoBox, style);
		layoutText(painter, section.text, style, row.textX, textTop, row.textWidth);

		record(LayoutBox::Kind::Logo, QRectF(logoBox));
		record(LayoutBox::Kind::Text, QRectF(row.textX, textTop, row.textWidth, textHeight));

		if (section.logoPlacement == LogoPlacement::Bridged) {
			const PreparedBridge bridge = prepareBridge(section, style, bridges, row.bridgeWidth);
			/*
			 * Hung off the text's own baseline, so a leader lands on it whatever the
			 * bridge is made of: text in the same font needs no offset at all, and art
			 * sits on the same line rather than on the top of the text's box. A row with
			 * no text at all still has the font's own ascent to hang it from.
			 */
			const qreal textAscent = section.text.isEmpty()
							 ? QFontMetricsF(makeFont(style)).ascent()
							 : firstBaseline(section.text, style, row.textWidth);
			const qreal baseline = textTop + textAscent;
			const qreal bridgeTop = baseline - bridge.ascent(section);
			const int bridgeHeight = paintBridge(painter, bridge, section, style, bridges, row.bridgeX,
							     bridgeTop, row.bridgeWidth);
			record(LayoutBox::Kind::Bridge, QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, bridgeHeight));
		}

		y += rowHeight;
		break;
	}

	case SectionType::Bridged: {
		const TextStyle &rightStyle = document.effectiveSecondaryStyle(section);
		const qreal naturalBridge = naturalBridgeWidth(section, style, bridges);

		for (const Entry &entry : section.entries) {
			const BridgedRow row = placeBridgedRow(section, style, rightStyle, entry, contentX,
							       contentWidth, naturalBridge);
			const PreparedBridge bridge = prepareBridge(section, style, bridges, row.bridgeWidth);

			/*
			 * The three parts share a baseline rather than a top edge, which is what
			 * keeps a leader running through the middle of the text when the two
			 * sides are set at different sizes. It is anchored on whichever part
			 * reaches lowest, so nothing climbs above the row into the one before it.
			 */
			const qreal leftAscent = firstBaseline(entry.text, style, row.leftWidth);
			const qreal rightAscent = firstBaseline(entry.secondaryText, rightStyle, row.rightWidth);
			const qreal bridgeAscent = bridge.ascent(section);
			const qreal baseline = std::max({leftAscent, rightAscent, bridgeAscent});

			const qreal leftTop = y + baseline - leftAscent;
			const qreal rightTop = y + baseline - rightAscent;
			const qreal bridgeTop = y + baseline - bridgeAscent;

			const int leftHeight =
				layoutText(painter, entry.text, style, row.leftX, leftTop, row.leftWidth);
			const int rightHeight = layoutText(painter, entry.secondaryText, rightStyle, row.rightX,
							   rightTop, row.rightWidth);
			const int bridgeHeight = paintBridge(painter, bridge, section, style, bridges, row.bridgeX,
							     bridgeTop, row.bridgeWidth);

			record(LayoutBox::Kind::Text, QRectF(row.leftX, leftTop, row.leftWidth, leftHeight));
			record(LayoutBox::Kind::Text, QRectF(row.rightX, rightTop, row.rightWidth, rightHeight));
			record(LayoutBox::Kind::Bridge, QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, bridgeHeight));

			int rowHeight = 1;
			const auto extend = [&rowHeight, y](qreal top, int height) {
				/* An empty part has no height and must not push the row down. */
				if (height > 0)
					rowHeight = std::max(rowHeight, qCeil(top - y) + height);
			};

			extend(leftTop, leftHeight);
			extend(rightTop, rightHeight);
			extend(bridgeTop, bridgeHeight);

			y += rowHeight + section.entryGap;
		}

		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::TextList: {
		for (const Entry &entry : section.entries) {
			const int height = layoutText(painter, entry.text, style, contentX, y, contentWidth);
			record(LayoutBox::Kind::Text, QRectF(contentX, y, contentWidth, height));
			y += height;
			y += section.entryGap;
		}
		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::LogoList: {
		for (const Entry &entry : section.entries) {
			const QImage image = logos->get(entry.logo.path, entry.logo.maxHeight);
			const QSize size = logoDrawSize(image, entry.logo, contentWidth);
			const int x = contentX + qRound(alignOffset(style.align, contentWidth, size.width()));
			const QRect box(QPoint(x, y), size);
			paintLogo(painter, image, box, style);
			record(LayoutBox::Kind::Logo, QRectF(box));
			y += size.height() + section.entryGap;
		}
		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::MultiTextList:
	case SectionType::MultiLogoList: {
		const int columns = std::max(1, section.columns);
		const int count = section.entries.size();
		if (count == 0)
			break;

		const int columnWidth = std::max(1, (contentWidth - (columns - 1) * section.columnGap) / columns);
		const int rows = (count + columns - 1) / columns;
		const bool logoMode = section.type == SectionType::MultiLogoList;

		for (int row = 0; row < rows; ++row) {
			int rowHeight = 0;

			for (int column = 0; column < columns; ++column) {
				/*
				 * Across-fill walks the row before wrapping; the default down-fill
				 * completes each column before moving right, which is what reads
				 * naturally in a long alphabetised list.
				 */
				const int index = section.fillAcross ? row * columns + column : column * rows + row;
				if (index >= count)
					continue;

				const Entry &entry = section.entries.at(index);
				const int x = contentX + column * (columnWidth + section.columnGap);

				if (logoMode) {
					const QImage image = logos->get(entry.logo.path, entry.logo.maxHeight);
					const QSize size = logoDrawSize(image, entry.logo, columnWidth);
					const int logoX =
						x + qRound(alignOffset(style.align, columnWidth, size.width()));
					const QRect box(QPoint(logoX, y), size);
					paintLogo(painter, image, box, style);
					record(LayoutBox::Kind::Logo, QRectF(box));
					rowHeight = std::max(rowHeight, size.height());
				} else {
					const int height = layoutText(painter, entry.text, style, x, y, columnWidth);
					record(LayoutBox::Kind::Text, QRectF(x, y, columnWidth, height));
					rowHeight = std::max(rowHeight, height);
				}
			}

			y += rowHeight + section.entryGap;
		}

		y -= section.entryGap;
		break;
	}
	}

	/* A section never collapses below its own padding, even with nothing in it. */
	y = std::max(y, contentTop);

	const int height = (y - top) + section.paddingBottom;

	/*
	 * Recorded last, because both are only known once the content has been laid out: the box
	 * spans the padding as well as the content, the content area only what sits between the
	 * margins and inside the padding. The difference between the two is what a section's
	 * spacing settings actually bought.
	 */
	record(LayoutBox::Kind::Section, QRectF(boxX, top, boxWidth, height));
	record(LayoutBox::Kind::Content, QRectF(contentX, contentTop, contentWidth, y - contentTop));

	return height;
}

struct PlacedSection {
	const Section *section;
	int top;
	int height;
};

/*
 * How far outside their own boxes the document's sections can paint, in pixels. Outlines and
 * shadows do not take part in the layout, so a section drawn near a tile's edge can reach
 * into the next one; the tile loop widens what it visits by this so the seam stays invisible.
 */
int effectBleed(const Document &document)
{
	double bleed = 0.0;

	for (const Section &section : document.sections) {
		/* Logos cast the style's shadow as well, so a section that only places art counts too. */
		if (!section.visible || !(sectionUsesText(section.type) || sectionUsesLogos(section.type)))
			continue;

		bleed = std::max(bleed, document.effectiveStyle(section).effectBleed());
		bleed = std::max(bleed, document.effectiveSecondaryStyle(section).effectBleed());
	}

	return qCeil(bleed);
}

QVector<PlacedSection> placeSections(const Document &document, LogoCache *logos, BridgeArtCache *bridges,
				     int *totalHeight, LayoutBoxes *boxes = nullptr)
{
	QVector<PlacedSection> placed;
	placed.reserve(document.sections.size());

	BoxCollector collector{boxes, -1};

	int y = document.leadIn;
	for (int index = 0; index < document.sections.size(); ++index) {
		const Section &section = document.sections.at(index);
		if (!section.visible)
			continue;

		collector.section = index;
		const int height =
			layoutSection(nullptr, section, document, logos, bridges, y, boxes ? &collector : nullptr);
		placed.append(PlacedSection{&section, y, height});
		y += height;

		if (y > kMaxStripHeight) {
			obs_log(LOG_WARNING, "credit strip exceeded %d px; remaining sections were dropped",
				kMaxStripHeight);
			break;
		}
	}

	*totalHeight = y + document.leadOut;
	return placed;
}

} // namespace

QBrush textFillBrush(const TextStyle &style, const QRectF &box)
{
	/*
	 * `box` is the block the gradient is mapped over: the laid-out height of the run and
	 * the horizontal extent its lines actually ink. Using the laid-out height rather than
	 * the ink bounds is what keeps a bridged row coherent -- a run of leader dots inks
	 * perhaps four pixels of height, so mapping the sweep onto its ink would cram the whole
	 * gradient into the dots while the text beside them showed only the middle of it.
	 */
	if (style.fill == TextFill::Solid || box.isEmpty())
		return QBrush(style.color);

	QGradientStops stops;
	for (const QPair<qreal, QColor> &stop : style.gradient.resolvedStops())
		stops.append(QGradientStop(stop.first, stop.second));

	if (style.fill == TextFill::RadialGradient) {
		/* Half the diagonal, so the last stop lands on the corners rather than inside them. */
		const qreal radius = std::hypot(box.width(), box.height()) / 2.0;
		QRadialGradient gradient(box.center(), std::max(radius, 1.0));
		gradient.setStops(stops);
		return QBrush(gradient);
	}

	/* Clockwise from straight down: 0 runs top to bottom, 90 left to right. */
	const qreal radians = qDegreesToRadians(style.gradient.angle);
	const QPointF axis(std::sin(radians), std::cos(radians));
	const QPointF centre = box.center();
	/* The box's own extent along that axis, so the stops span exactly the block. */
	const qreal half = (std::abs(axis.x()) * box.width() + std::abs(axis.y()) * box.height()) / 2.0;

	QLinearGradient gradient(centre - axis * half, centre + axis * half);
	gradient.setStops(stops);
	return QBrush(gradient);
}

bool fontFamilyAvailable(const QString &family)
{
	if (family.isEmpty())
		return true;

	/*
	 * Qt maps these onto whatever the platform's default is for the category, so they are
	 * never a substitution there is anything to be done about.
	 */
	static const QStringList generics = {
		QStringLiteral("Sans Serif"), QStringLiteral("Serif"),   QStringLiteral("Monospace"),
		QStringLiteral("Cursive"),    QStringLiteral("Fantasy"), QStringLiteral("System"),
	};

	for (const QString &generic : generics) {
		if (family.compare(generic, Qt::CaseInsensitive) == 0)
			return true;
	}

	return QFontDatabase::hasFamily(family);
}

QStringList missingFontFamilies(const Document &document)
{
	QStringList missing;

	const auto consider = [&missing](const QString &family) {
		if (family.isEmpty() || missing.contains(family) || fontFamilyAvailable(family))
			return;
		missing.append(family);
	};

	for (const Section &section : document.sections) {
		if (!section.visible || !sectionUsesText(section.type))
			continue;

		consider(document.effectiveStyle(section).family);
		consider(document.effectiveSecondaryStyle(section).family);
	}

	missing.sort(Qt::CaseInsensitive);
	return missing;
}

QImage LogoCache::get(const QString &path, int maxHeight)
{
	if (path.isEmpty())
		return QImage();

	const int height = std::max(1, maxHeight);
	const QString key = QStringLiteral("%1|%2").arg(path).arg(height);

	const QFileInfo info(path);
	const qint64 fileSize = info.size();
	const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();

	const auto it = cache.constFind(key);
	if (it != cache.constEnd() && it->fileSize == fileSize && it->modifiedMs == modifiedMs)
		return it->image;

	QImageReader reader(path);
	reader.setAutoTransform(true);

	QImage image = reader.read();
	if (image.isNull()) {
		obs_log(LOG_WARNING, "could not decode logo '%s': %s", path.toUtf8().constData(),
			reader.errorString().toUtf8().constData());
	} else {
		if (image.height() > height)
			image = image.scaledToHeight(height, Qt::SmoothTransformation);
		image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	}

	cache.insert(key, CacheEntry{image, fileSize, modifiedMs});
	return image;
}

void LogoCache::clear()
{
	cache.clear();
}

void LogoCache::invalidate(const QString &path)
{
	const QString prefix = path + QLatin1Char('|');
	for (auto it = cache.begin(); it != cache.end();) {
		if (it.key().startsWith(prefix))
			it = cache.erase(it);
		else
			++it;
	}
}

int StripRenderer::measure(const Document &document) const
{
	/*
	 * The bridge tiles are parsed for the length of the call and dropped again. Unlike a
	 * decoded logo there is nothing here worth holding on to between renders -- a tile is a
	 * few hundred bytes of markup -- and building it fresh is what makes a custom SVG edited
	 * on disk show up in the next rebuild without anything having to watch the file.
	 */
	BridgeArtCache bridges;

	int total = 0;
	placeSections(document, logos, &bridges, &total);
	return total - document.leadIn - document.leadOut;
}

Strip StripRenderer::render(const Document &document, LayoutBoxes *boxes) const
{
	Strip strip;
	strip.width = std::max(1, document.width);

	BridgeArtCache bridges;

	if (boxes)
		boxes->clear();

	int total = 0;
	const QVector<PlacedSection> placed = placeSections(document, logos, &bridges, &total, boxes);

	strip.height = std::min(std::max(total, 0), kMaxStripHeight);
	if (strip.height <= 0 || placed.isEmpty())
		return strip;

	const int bleed = effectBleed(document);

	for (int top = 0; top < strip.height; top += kTileHeight) {
		const int tileHeight = std::min(kTileHeight, strip.height - top);
		const int bottom = top + tileHeight;

		QImage tile(strip.width, tileHeight, QImage::Format_ARGB32_Premultiplied);
		tile.fill(Qt::transparent);

		QPainter painter(&tile);
		painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
				       QPainter::SmoothPixmapTransform);
		/* Sections are laid out in strip space, so shift the tile under them. */
		painter.translate(0, -top);

		for (const PlacedSection &entry : placed) {
			if (entry.top - bleed >= bottom || entry.top + entry.height + bleed <= top)
				continue;

			layoutSection(&painter, *entry.section, document, logos, &bridges, entry.top);
		}

		painter.end();

		/*
		 * OBS composites with straight alpha, so the premultiplied buffer QPainter needs
		 * is unpremultiplied once here rather than corrected per frame on the GPU.
		 */
		strip.tiles.append(StripTile{top, tile.convertToFormat(QImage::Format_ARGB32)});
	}

	return strip;
}

} // namespace closingtime
