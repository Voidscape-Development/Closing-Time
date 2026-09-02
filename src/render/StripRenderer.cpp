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

#include "render/BackgroundPainter.hpp"
#include "render/BridgeArtRenderer.hpp"
#include "render/DividerArtRenderer.hpp"
#include "render/FontResolution.hpp"
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

	/*
	 * The exact face, but only on a machine that has it. Naming one switches off the synthetic
	 * bold and slant Qt applies to the flags above, so asking for a face that is not installed
	 * is worse than not asking: the roll would come out in the family's plain face rather than
	 * in the nearest weight to what was chosen. Asked first, so the flags are still standing
	 * when the answer is no.
	 */
	if (!style.styleName.isEmpty() && fontStyleAvailable(style.family, style.styleName))
		font.setStyleName(style.styleName);

	font.setUnderline(style.underline);
	font.setStrikeOut(style.strikeOut);
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
 * Only styles that need more than a pen color go through this: an outline has to be stroked
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

/*
 * The underline and the strike-out of a laid-out run, as rectangles positioned at `origin`.
 *
 * QTextLayout::draw() rules these itself from the font's own flags, and the plain path lets it.
 * The effects path never calls it -- it works from QRawFont::pathForGlyph, and a rule is not a
 * glyph -- so a struck-out heading drawn there would come out with an outlined, gradient-filled,
 * shadow-casting set of letters and a rule that had none of it, which reads as the outline being
 * broken rather than as a rule somebody forgot to draw.
 *
 * Measured off the style's own font rather than off each run's, so the rule stays one straight
 * line at one thickness across a row a fallback font supplied part of.
 */
QPainterPath textDecorations(const QTextLayout &layout, const QPointF &origin, const TextStyle &style)
{
	QPainterPath path;
	if (!style.underline && !style.strikeOut)
		return path;

	const QFontMetricsF metrics(layout.font());
	const qreal thickness = std::max(1.0, metrics.lineWidth());

	for (int index = 0; index < layout.lineCount(); ++index) {
		const QTextLine line = layout.lineAt(index);
		const qreal width = line.naturalTextWidth();
		if (width <= 0.0)
			continue;

		const qreal left = origin.x() + line.x();
		const qreal baseline = origin.y() + line.y() + line.ascent();

		/* Both are measured from the baseline: the underline below it, the strike-out above. */
		if (style.underline)
			path.addRect(QRectF(left, baseline + metrics.underlinePos(), width, thickness));
		if (style.strikeOut)
			path.addRect(QRectF(left, baseline - metrics.strikeOutPos(), width, thickness));
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

	QPainterPath path = glyphPath(layout, origin);

	/*
	 * United rather than added, and this is the one place in the renderer that pays for a
	 * boolean op. A rule added to the same path crosses the descenders it passes through, and
	 * two overlapping contours wound opposite ways cancel under any fill rule -- which would
	 * punch the shape of the rule out of the tail of every 'g' it crossed. Only a run that is
	 * both decorated and effected comes through here at all.
	 */
	const QPainterPath decorations = textDecorations(layout, origin, style);
	if (!decorations.isEmpty())
		path = path.united(decorations);

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
 * rasterizing, which is how the two-pass layout keeps measurement and painting from
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
	 * which pushes centered text a full half-width to the right.
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
 * The width a title/subtitle pair wants before any wrapping is imposed on it: whichever of its
 * two lines is wider, since the two are laid out into the same column.
 */
qreal naturalPairWidth(const QString &title, const QString &subtitle, const TextStyle &titleStyle,
		       const TextStyle &subtitleStyle)
{
	return std::max(naturalTextWidth(title, titleStyle), naturalTextWidth(subtitle, subtitleStyle));
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
 * here, only the image recolored to the shadow's ink at the size it is about to be drawn. Like
 * every other effect in the renderer this paints outside the section's box without growing it,
 * which is why `effectBleed` has to count the sections that place logos as well as the ones that
 * set text.
 *
 * Returned as an image rather than painted, because a shadow is wanted in two places that cannot
 * share a painter: baked into the strip behind a still logo, and handed to the source as a quad
 * of its own for an animated one whose shadow follows its frames. `offset` comes back as the
 * shadow's top-left relative to the artwork's own, blur margin and all.
 */
QImage logoShadowImage(const QImage &image, const QSize &size, const TextShadow &shadow, QPointF *offset)
{
	if (size.isEmpty() || image.isNull())
		return QImage();

	const QImage ink = tintedImage(image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
					       .convertToFormat(QImage::Format_ARGB32_Premultiplied),
				       shadow.color);
	if (ink.isNull())
		return QImage();

	const QPointF at(shadow.offsetX, shadow.offsetY);
	const int radius = std::clamp(qRound(shadow.blur / 2.0), 0, 100);

	if (radius < 1) {
		*offset = at;
		return ink;
	}

	/* Three passes of box radius r reach 3r, so that is the margin the blur needs. */
	const int margin = radius * 3 + 1;
	const QSize buffered(ink.width() + margin * 2, ink.height() + margin * 2);

	if (static_cast<qint64>(buffered.width()) * buffered.height() > kMaxLogoShadowPixels) {
		obs_log(LOG_WARNING, "logo shadow blur too large to buffer; drawing it hard instead");
		*offset = at;
		return ink;
	}

	QImage buffer(buffered, QImage::Format_ARGB32_Premultiplied);
	if (buffer.isNull())
		return QImage();

	buffer.fill(Qt::transparent);

	QPainter bufferPainter(&buffer);
	bufferPainter.drawImage(QPoint(margin, margin), ink);
	bufferPainter.end();

	blurImage(buffer, radius);

	*offset = at - QPointF(margin, margin);
	return buffer;
}

void paintLogoShadow(QPainter *painter, const QImage &image, const QRect &rect, const TextShadow &shadow)
{
	QPointF offset;
	const QImage buffer = logoShadowImage(image, rect.size(), shadow, &offset);
	if (buffer.isNull())
		return;

	painter->drawImage(QPointF(rect.topLeft()) + offset, buffer);
}

/*
 * A logo's artwork, however it is stored.
 *
 * `poster` is the frame the layout measures and, for a still, the frame it draws: an animation's
 * first frame is a still picture of exactly the size the animation will occupy, so every
 * measurement in the layout carries on being taken from one image and none of them has to learn
 * what a frame is. `animation` is non-null only when there is something to play.
 */
struct LogoArt {
	QImage poster;
	LogoAnimationPtr animation;

	bool isAnimated() const { return animation && animation->isValid(); }
};

LogoArt resolveLogoArt(LogoCache *stills, AnimatedLogoCache *animations, const LogoRef &ref)
{
	LogoArt art;

	if (ref.isEmpty())
		return art;

	if (animations) {
		art.animation = animations->get(ref.path, ref.maxHeight);
		if (art.animation && art.animation->isValid()) {
			art.poster = art.animation->frames.first().image;
			return art;
		}
		art.animation.reset();
	}

	if (stills)
		art.poster = stills->get(ref.path, ref.maxHeight);

	return art;
}

/*
 * Paints a logo into the strip.
 *
 * An animated logo paints its shadow and stops. The artwork itself is deliberately left out --
 * that hole is what the overlay quad is drawn into -- and the shadow is kept because the shadow
 * for artwork that holds its silhouette is the same shadow in every frame, so baking it costs one
 * blur at rebuild rather than one per frame for the length of the roll. A logo that asked for a
 * shadow that follows its frames paints nothing at all here: its shadow arrives with the
 * placement, frame by frame.
 */
void paintLogo(QPainter *painter, const LogoArt &art, const QRect &rect, const TextStyle &style,
	       const LogoPlayback &playback)
{
	if (!painter)
		return;

	if (art.poster.isNull()) {
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

	const bool animated = art.isAnimated();

	if (style.shadow.enabled && !(animated && playback.animatedShadow))
		paintLogoShadow(painter, art.poster, rect, style.shadow);

	if (!animated)
		painter->drawImage(rect, art.poster);
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

/*
 * Gathers the animated logos the layout placed, for whoever draws over the strip.
 *
 * Unlike the box collector this is not an optional extra the designer alone pays for: an animated
 * logo that is not reported is a hole in the roll with nothing drawn into it. It is filled during
 * the measure pass for the same reason the boxes are -- that pass visits each section once,
 * whatever tiles the section straddles.
 */
/*
 * Collects the sticky blocks the layout placed, the way AnimatedCollector collects animated logos.
 *
 * Only a caller that can actually draw a block over the strip asks for these -- the source and the
 * designer's preview. `measure()` passes nothing and rasterizes nothing, so asking a roll how tall
 * it is stays as cheap as it was.
 */
struct StickyCollector {
	QVector<StickyBlockPlacement> *placements = nullptr;
	int section = -1;

	void add(StickyBlockPlacement placement)
	{
		if (!placements || placement.rect.isEmpty())
			return;

		placement.section = section;
		placements->append(std::move(placement));
	}
};

struct AnimatedCollector {
	QVector<AnimatedLogoPlacement> *placements = nullptr;
	int section = -1;

	void add(const LogoArt &art, const LogoRef &ref, const QRect &rect, const TextStyle &style)
	{
		if (!placements || !art.isAnimated() || rect.isEmpty())
			return;

		AnimatedLogoPlacement placement;
		placement.rect = QRectF(rect);
		placement.animation = art.animation;
		placement.playback = ref.playback;
		placement.section = section;

		if (style.shadow.enabled && ref.playback.animatedShadow) {
			/*
			 * One blur per frame, taken here so the compositor never has to. The frames
			 * are all the same size, so the offset the first one comes back with is the
			 * offset for all of them.
			 */
			placement.shadowFrames.reserve(art.animation->frames.size());
			for (const LogoFrame &frame : art.animation->frames) {
				QPointF offset;
				QImage shadow = logoShadowImage(frame.image, rect.size(), style.shadow, &offset);
				if (shadow.isNull()) {
					placement.shadowFrames.clear();
					break;
				}

				placement.shadowOffset = offset;
				placement.shadowFrames.append(shadow.convertToFormat(QImage::Format_ARGB32));
			}
		}

		placements->append(placement);
	}
};

/*
 * Where a logo's artwork comes from: stills, animations, or neither.
 *
 * Carried as one thing rather than as two more parameters through a layout that already threads
 * five, and null-tolerant in both halves so a caller that has no animation cache -- the measure
 * that only wants a height, the test harness -- reads exactly like one that does.
 */
struct LogoSource {
	LogoCache *stills = nullptr;
	AnimatedLogoCache *animations = nullptr;

	LogoArt resolve(const LogoRef &ref) const { return resolveLogoArt(stills, animations, ref); }
};

/*
 * The panels one section draws, resolved once and carried to every place that puts content down.
 *
 * It exists because a panel goes *under* something whose rectangle is only known once that thing
 * has been measured, and measuring text twice is the most expensive thing this renderer could
 * casually start doing. So `wants` is asked first: a slot with nothing in it -- which is every slot
 * on every section of every roll that has not been given a panel -- costs one comparison and the
 * content is laid out exactly once, as it always was. Only a slot that will really paint pays for
 * the extra measure.
 *
 * It also holds the one rule that cannot live on the model: which of `Entry` and `EntryAlt` an
 * entry falls to. A list with no alternate draws `Entry` behind every row; a list that has one
 * draws it behind the odd-numbered rows, whether or not that alternate is set to draw anything --
 * which is how every other row is left bare.
 */
class SectionPanels {
public:
	SectionPanels(QPainter *painter, const Document &document, const Section &section, LogoCache *images)
		: painter(painter),
		  document(&document),
		  section(&section),
		  images(images),
		  alternating(section.hasBackground(BackgroundSlot::EntryAlt))
	{
	}

	/* True when this slot would paint, and therefore worth measuring a box for. */
	bool wants(BackgroundSlot slot) const
	{
		return painter && document->effectiveBackground(*section, slot).isVisible();
	}

	void paint(BackgroundSlot slot, const QRectF &box) const
	{
		paintBackgroundPanel(painter, document->effectiveBackground(*section, slot), box, images);
	}

	/* Which slot the entry at `index` in this section's list is paneled from. */
	BackgroundSlot entrySlot(int index) const
	{
		return alternating && (index % 2) == 1 ? BackgroundSlot::EntryAlt : BackgroundSlot::Entry;
	}

private:
	QPainter *painter;
	const Document *document;
	const Section *section;
	LogoCache *images;
	bool alternating;
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
 *
 * What goes in the text column is the caller's business: `naturalWidth` is the width that column
 * would like and `hasText` whether there is anything in it to be held off the logo. A heading with
 * a subtitle under it hands over the wider of its two lines and is divided from its logo exactly
 * as a single line is, which is the whole of the difference between the two shapes.
 */
LogoRow placeLogoRow(const Section &section, qreal contentX, qreal contentWidth, qreal logoWidth, qreal naturalWidth,
		     bool hasText)
{
	const bool onLeft = section.logoSide == LogoSide::Left;
	/* Nothing to separate the logo from when there is no text. */
	const qreal gap = hasText ? section.logoGap : 0.0;

	LogoRow row;

	switch (section.logoPlacement) {
	case LogoPlacement::Edge:
		row.textWidth = std::max(0.0, contentWidth - logoWidth - gap);
		row.logoX = onLeft ? contentX : contentX + contentWidth - logoWidth;
		row.textX = onLeft ? contentX + logoWidth + gap : contentX;
		break;

	case LogoPlacement::Hug: {
		row.textWidth = std::min(naturalWidth, std::max(0.0, contentWidth - logoWidth - gap));

		const qreal groupWidth = logoWidth + gap + row.textWidth;
		const qreal groupX = contentX + alignOffset(section.sectionAlign, contentWidth, groupWidth);

		row.logoX = onLeft ? groupX : groupX + row.textWidth + gap;
		row.textX = onLeft ? groupX + logoWidth + gap : groupX;
		break;
	}

	case LogoPlacement::Bridged: {
		row.textWidth = std::min(naturalWidth, std::max(0.0, contentWidth - logoWidth));

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

/*
 * One side of a bridged row: the line, whatever is stacked under it, and the style of each.
 *
 * A side is a pair rather than a string because `rowSubtitles` lets either of them carry a second
 * line, and every question the row asks of a side -- how wide does it want to be, where is its top
 * line's baseline, is there anything in it at all -- has to be asked of both lines or of neither.
 * Threading four more parameters through the placement instead would leave each of those questions
 * free to answer for only half of the side, which is exactly the bug this shape cannot have.
 *
 * Pointers rather than copies because everything here is already owned by the entry or the section
 * being laid out, and a style is the larger half of what would otherwise be copied once per row.
 */
struct BridgedSide {
	const QString *text;
	const QString *subtitle;
	const TextStyle *style;
	const TextStyle *subtitleStyle;

	bool isEmpty() const { return text->isEmpty() && subtitle->isEmpty(); }

	/* The width the side wants: whichever of its two lines is wider, since they share a column. */
	qreal naturalWidth() const { return naturalPairWidth(*text, *subtitle, *style, *subtitleStyle); }
};

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
BridgedRow placeBridgedRow(const Section &section, const BridgedSide &left, const BridgedSide &right, qreal contentX,
			   qreal contentWidth, qreal naturalBridge)
{
	qreal leftWidth = 0.0;
	qreal rightWidth = 0.0;

	/* An empty bridge is laid out as a Fixed one whatever the setting says; see the model. */
	const BridgeFill fill = effectiveBridgeFill(section);

	if (section.bridgeSizing == BridgeSizing::Split) {
		/*
		 * The ratio divides the space the two texts share rather than the whole width,
		 * so a Fixed bridge at the default 0.5 lands exactly where Bridged sections
		 * have always drawn it. With a filling bridge there is nothing to reserve, and
		 * the ratio becomes a plain tab stop for the leader to start at.
		 */
		const qreal reserved = fill == BridgeFill::Fixed ? naturalBridge : 0.0;
		leftWidth = section.bridgeSplit * std::max(0.0, contentWidth - reserved);

		if (fill == BridgeFill::Fixed) {
			rightWidth = std::max(0.0, contentWidth - leftWidth - naturalBridge);
		} else {
			rightWidth = std::min(right.naturalWidth(), std::max(0.0, contentWidth - leftWidth));

			/*
			 * With a filling bridge the split is a tab stop rather than a cap: there is
			 * nothing reserved on the other side of it, so a left text that overruns it
			 * takes what it needs and pushes the bridge along instead of wrapping inside
			 * its column with the gap beside it left empty. Rows that do fit still start
			 * their bridge at the same x, which is the whole point of the setting.
			 */
			leftWidth = std::clamp(left.naturalWidth(), leftWidth,
					       std::max(leftWidth, contentWidth - rightWidth));
		}
	} else {
		leftWidth = left.naturalWidth();
		rightWidth = right.naturalWidth();

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
	if (fill != BridgeFill::Fixed) {
		if (section.bridgeSpanEmpty && left.isEmpty())
			leftWidth = 0.0;
		if (section.bridgeSpanEmpty && right.isEmpty())
			rightWidth = 0.0;
	}

	const qreal slack = std::max(0.0, contentWidth - leftWidth - rightWidth);

	BridgedRow row;
	row.leftWidth = leftWidth;
	row.rightWidth = rightWidth;
	row.bridgeWidth = fill == BridgeFill::Fixed ? std::min(naturalBridge, slack) : slack;

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
		 * Whole copies only, centered in the gap by the caller. A partial copy would cut
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
	if (bridgeTypeIsEmpty(section.bridgeType)) {
		/*
		 * Nothing to draw, and deliberately nothing to measure either: an empty bridge
		 * reports no ascent and no height, so the row is exactly as tall as its own texts
		 * and its baseline is theirs. The space it occupies is still real -- the columns
		 * were placed around `bridgeMinGap` before this was ever called.
		 */
		PreparedBridge prepared;
		prepared.font = makeFont(style);
		return prepared;
	}

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
	/*
	 * An empty bridge is nothing but its own width, which is the whole reason `bridgeMinGap`
	 * exists: it is what keeps the two texts of a naturally sized row off each other when
	 * there is no leader between them to do it.
	 */
	if (bridgeTypeIsEmpty(section.bridgeType))
		return std::max(0.0, section.bridgeMinGap);

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
 * One line of a stacked pair, as the pair's own drawing order leaves it.
 *
 * A pointer pair rather than a copy because both of them are already owned by the section or
 * the entry being laid out, and a style is the larger half of what would otherwise be copied
 * once per row of a long list.
 */
struct StackedLine {
	const QString *text;
	const TextStyle *style;
};

/*
 * The line of a title/subtitle pair that really is drawn on top.
 *
 * Ordinarily `subtitleFirst` decides it outright, but a pair with one of its two texts left
 * blank draws the other at the top of the block -- so anything anchoring on the top of a pair
 * from the outside, which is to say the leader of a bridged logo row, hangs off the line that is
 * there rather than off one that was skipped.
 *
 * A pair with nothing in it at all reports the line the order would have put second, which is
 * the title in the ordinary case: an empty run has no baseline to offer and the caller falls
 * back to the font's own ascent, so what matters is only which font it falls back to.
 */
StackedLine topStackedLine(const Section &section, const QString &title, const QString &subtitle,
			   const TextStyle &titleStyle, const TextStyle &subtitleStyle)
{
	const StackedLine first{section.subtitleFirst ? &subtitle : &title,
				section.subtitleFirst ? &subtitleStyle : &titleStyle};
	if (!first.text->isEmpty())
		return first;

	return StackedLine{section.subtitleFirst ? &title : &subtitle,
			   section.subtitleFirst ? &titleStyle : &subtitleStyle};
}

/*
 * One title/subtitle pair: two lines stacked inside the column they are given, separated by
 * `subtitleGap`, and returning the height the pair occupies.
 *
 * Takes the two texts rather than the thing holding them, because the same stack serves a list
 * entry and a section's own heading, and the two keep them in different places. Everything that
 * shapes the stack still comes off the section, which is where all of it lives.
 *
 * Each line is aligned by its own style within the full column rather than the two being
 * measured and placed as one group, which is what lets a title sit left with its subtitle
 * right, or either of them be centered against a run of names of differing lengths.
 *
 * `subtitleFirst` swaps only the placement. The title is still `title` drawn in `titleStyle`
 * and the subtitle still `subtitle` drawn in `subtitleStyle`, so flipping the order never
 * moves content between the two fields.
 */
template<typename Record>
int layoutTitleSubtitle(QPainter *painter, const Section &section, const TextStyle &titleStyle,
			const TextStyle &subtitleStyle, const QString &title, const QString &subtitle, qreal x, qreal y,
			qreal width, const Record &record, const SectionPanels *panels = nullptr)
{
	const QString &first = section.subtitleFirst ? subtitle : title;
	const QString &second = section.subtitleFirst ? title : subtitle;
	const TextStyle &firstStyle = section.subtitleFirst ? subtitleStyle : titleStyle;
	const TextStyle &secondStyle = section.subtitleFirst ? titleStyle : subtitleStyle;

	/*
	 * The slots follow the fields, not the placement. `subtitleFirst` swaps which line is drawn on
	 * top and nothing else, so the subtitle's panel goes behind the subtitle wherever the flip has
	 * put it -- exactly as its style does.
	 */
	const BackgroundSlot firstSlot = section.subtitleFirst ? BackgroundSlot::Subtitle : BackgroundSlot::Title;
	const BackgroundSlot secondSlot = section.subtitleFirst ? BackgroundSlot::Title : BackgroundSlot::Subtitle;

	/*
	 * A panel goes under a line whose height is not known until the line has been measured, so a
	 * slot that will paint measures first and then draws. A slot with nothing in it -- which is
	 * every slot until somebody sets one -- lays the line out exactly once, as it always did.
	 */
	const auto panelUnder = [&](BackgroundSlot slot, const QString &text, const TextStyle &style, qreal top) {
		if (!panels || !panels->wants(slot))
			return;

		const int height = layoutText(nullptr, text, style, x, top, width);
		if (height > 0)
			panels->paint(slot, QRectF(x, top, width, height));
	};

	qreal cursor = y;

	/*
	 * A line with no text has no height and takes no gap with it, so a pair carrying only
	 * one of its two texts occupies exactly what that one line does -- a heading row in an
	 * otherwise paired list sits where a single line would rather than reserving space for
	 * the line that is not there.
	 */
	panelUnder(firstSlot, first, firstStyle, cursor);
	const int firstHeight = layoutText(painter, first, firstStyle, x, cursor, width);
	if (firstHeight > 0) {
		record(LayoutBox::Kind::Text, QRectF(x, cursor, width, firstHeight));
		cursor += firstHeight + section.subtitleGap;
	}

	panelUnder(secondSlot, second, secondStyle, cursor);
	const int secondHeight = layoutText(painter, second, secondStyle, x, cursor, width);
	if (secondHeight > 0) {
		record(LayoutBox::Kind::Text, QRectF(x, cursor, width, secondHeight));
		cursor += secondHeight;
	} else if (firstHeight > 0) {
		cursor -= section.subtitleGap;
	}

	return qCeil(cursor - y);
}

/* Collects nothing, for the measure pass a caller makes before it knows where to place a pair. */
const auto kIgnoreBoxes = [](LayoutBox::Kind, const QRectF &) { /* deliberately nothing */ };

/*
 * Where one entry's own column starts, once its indent is counted in.
 *
 * The column is translated rather than narrowed, so every alignment steps by the same amount and
 * an indent means one thing whatever the list is set to -- see Entry::indent. A list with no
 * indents anywhere is arithmetic on a zero and lands exactly where it always did.
 */
qreal indentedX(const Section &section, const Entry &entry, qreal x)
{
	return x + entry.indent * section.indentStep;
}

/*
 * Turns the painter about a rectangle's center for as long as it is in scope, so a divider piece
 * drawn by the very helpers that draw a section's own title and its own logo comes out at the
 * angle its row asked for without either of them learning what an angle is.
 *
 * Nothing at all at zero degrees, which is what every piece in most rolls is set to.
 */
class TurnedPainter {
public:
	TurnedPainter(QPainter *painter, const QRectF &rect, qreal degrees)
		: target(painter && degrees != 0.0 ? painter : nullptr)
	{
		if (!target)
			return;

		target->save();
		target->translate(rect.center());
		target->rotate(degrees);
		target->translate(-rect.center());
	}

	~TurnedPainter()
	{
		if (target)
			target->restore();
	}

	TurnedPainter(const TurnedPainter &) = delete;
	TurnedPainter &operator=(const TurnedPainter &) = delete;

private:
	QPainter *target = nullptr;
};

/*
 * Where every part of one Section Divider goes, in strip space.
 *
 * The artwork is separated from the text and the logos because the three are painted by
 * different machinery: art goes through the divider renderer's stencil, while a word and a mark
 * in the middle of a divider are drawn by exactly the helpers that draw a section's own title
 * and its own logo -- which is the point of letting the center hold them at all.
 */
struct DividerLayout {
	struct TextPiece {
		QString text;
		QRectF rect;
		/* Degrees clockwise about the rect's center; see DividerPiece::rotation. */
		qreal rotation = 0.0;
	};
	struct LogoPiece {
		LogoArt art;
		/*
		 * The piece's own reference, for the playback settings the placement needs. It points
		 * into the section, which outlives every use of the layout this call returns.
		 */
		const LogoRef *ref = nullptr;
		QRect rect;
		/* Degrees clockwise about the rect's center; see DividerPiece::rotation. */
		qreal rotation = 0.0;
	};

	QVector<DividerArtPlacement> art;
	QVector<TextPiece> texts;
	QVector<LogoPiece> logos;

	/* Total height the divider occupies. Zero when it draws nothing at all. */
	qreal height = 0.0;
};

/* One piece of a divider -- an end or a centerpiece -- measured but not yet placed. */
struct MeasuredPiece {
	const DividerPiece *piece;
	QSizeF size;
	/* Logo pieces only, resolved once here so the paint pass does not look it up again. */
	LogoArt art;
};

/* A measured stack of them: what one end or the middle of a divider comes to. */
struct MeasuredStack {
	QVector<MeasuredPiece> pieces;
	/*
	 * The join before each piece, already clamped -- `gaps[i]` sits between piece `i - 1` and
	 * piece `i`, and `gaps[0]` is unused. Carried rather than recomputed at placement time
	 * because a negative gap is bounded by the two pieces it sits between, and a measure pass
	 * and a place pass that worked that bound out separately would be two chances to disagree
	 * about a width the section under this one is positioned from.
	 */
	QVector<qreal> gaps;
	qreal width = 0.0;
	qreal height = 0.0;

	bool isEmpty() const { return pieces.isEmpty(); }

	/*
	 * Half the outermost piece, which is where a connected arm stops.
	 *
	 * The outermost piece is always the first written: a cap's list runs outermost-first, and
	 * the right-hand end is the same list placed in reverse, which puts the same piece at the
	 * far edge either way.
	 */
	qreal outerHalfWidth() const { return isEmpty() ? 0.0 : pieces.first().size.width() / 2.0; }
};

/*
 * How tall a measured piece reaches once it is turned: the height of its own box while it stands
 * square, and the height that box sweeps out once it does not.
 *
 * Height only. A turn moves nothing along the rule -- the piece keeps the width its untilted shape
 * asked for, so its neighbors and the arms stay where they were while an angle is dialed in (see
 * DividerPiece::rotation) -- but the divider still has to be tall enough to hold what it draws, or
 * a square set on its corner would have the corners cut off by the section's own box.
 */
qreal turnedHeight(const MeasuredPiece &piece)
{
	const qreal degrees = piece.piece->rotation;
	if (degrees == 0.0)
		return piece.size.height();

	const qreal radians = qDegreesToRadians(degrees);
	return std::abs(piece.size.width() * std::sin(radians)) + std::abs(piece.size.height() * std::cos(radians));
}

/*
 * Measures a run of divider pieces laid side by side, `pieceGap` apart.
 *
 * One helper for all three stacks rather than one for the middle and a separate path for the two
 * ends, because an end *is* a stack: that is the whole of what makes a shape offered in one place
 * offered in the other, and a word or a mark able to cap a rule as well as break one.
 */
MeasuredStack measureDividerStack(const QVector<DividerPiece> &pieces, const TextStyle &style, const LogoSource &logos,
				  DividerArtCache *dividers, qreal thickness, qreal pieceGap, qreal available)
{
	MeasuredStack stack;
	stack.pieces.reserve(pieces.size());

	for (const DividerPiece &piece : pieces) {
		MeasuredPiece measured{&piece, QSizeF(), LogoArt()};

		switch (piece.kind) {
		case DividerPiece::Kind::Ornament:
			measured.size = dividerShapeSize(piece.shape, piece.svgPath, dividers, thickness, piece.scale);
			break;

		case DividerPiece::Kind::Text: {
			if (piece.text.isEmpty())
				break;
			/*
			 * Measured at its natural width and then laid out into exactly that, so the
			 * word occupies the room it needs and never wraps inside a divider. A divider
			 * piece has no column to wrap into -- the arms are what the space around it
			 * is for.
			 */
			const qreal textWidth = naturalTextWidth(piece.text, style);
			const qreal textHeight = layoutText(nullptr, piece.text, style, 0, 0, textWidth);
			measured.size = QSizeF(textWidth, textHeight);
			break;
		}

		case DividerPiece::Kind::Logo: {
			measured.art = logos.resolve(piece.logo);
			measured.size = QSizeF(logoDrawSize(measured.art.poster, piece.logo, qRound(available)));
			break;
		}
		}

		/*
		 * A piece that measures to nothing -- an empty word, a logo that would not decode, a
		 * shape whose file is missing -- is dropped along with the gap that would have sat
		 * beside it, rather than left as a hole in the rule.
		 */
		if (measured.size.width() > 0.0 && measured.size.height() > 0.0)
			stack.pieces.append(measured);
	}

	stack.gaps.resize(stack.pieces.size());

	for (int i = 0; i < stack.pieces.size(); ++i) {
		const qreal pieceWidth = stack.pieces.at(i).size.width();

		if (i > 0) {
			/*
			 * A negative gap pushes two pieces into each other, which is how a stack is
			 * made to read as one ornament rather than as a row of them. Bounded at half
			 * the narrower of the pair, so they may be pushed together but never through
			 * each other: past that the run reorders itself, and a stack whose width
			 * counts down as the setting goes up is not a design anybody asked for.
			 */
			const qreal limit = std::min(stack.pieces.at(i - 1).size.width(), pieceWidth) / 2.0;
			stack.gaps[i] = std::max(pieceGap, -limit);
			stack.width += stack.gaps.at(i);
		}

		stack.width += pieceWidth;
		stack.height = std::max(stack.height, turnedHeight(stack.pieces.at(i)));
	}

	return stack;
}

/*
 * Places a measured stack, left to right from `left`, centered on `midY`.
 *
 * `mirrored` is what an end at the right-hand side of the divider is drawn with: the pieces run in
 * reverse, so the figure is a mirror image of the same list, and each piece's *artwork* is flipped
 * along x -- the caps in the shape library are authored pointing outward along -x, so an arrowhead
 * at the right-hand end has to be. A word and a picture are deliberately left unflipped: mirrored
 * type is not a design, it is a mistake, and neither is authored pointing anywhere.
 *
 * The joins come from the stack rather than from the section, already bounded by the pieces they
 * sit between -- see MeasuredStack::gaps.
 */
void placeDividerStack(DividerLayout *layout, const MeasuredStack &stack, qreal left, qreal midY, bool mirrored)
{
	const int count = stack.pieces.size();
	qreal cursor = left;

	for (int i = 0; i < count; ++i) {
		const int index = mirrored ? count - 1 - i : i;
		const MeasuredPiece &measured = stack.pieces.at(index);
		const DividerPiece &piece = *measured.piece;

		/*
		 * The join before the piece about to be placed. Reversing the run reverses the joins
		 * with it -- the gap that was written after a piece is drawn before it -- so the two
		 * ends of a divider are the same figure whichever way round it is read.
		 */
		if (i > 0)
			cursor += stack.gaps.at(mirrored ? index + 1 : index);

		const QRectF box(cursor, midY - measured.size.height() / 2.0, measured.size.width(),
				 measured.size.height());

		/*
		 * A word and a mark are not flipped, so the lean they were given is reflected instead:
		 * that is what keeps a tilted year or badge at one end the answering figure to the one
		 * at the other without setting any of it backwards. Artwork needs no such arrangement --
		 * its flip is a true mirror, and reflects whatever angle it carries with it.
		 */
		const qreal turn = mirrored ? -piece.rotation : piece.rotation;

		switch (piece.kind) {
		case DividerPiece::Kind::Ornament:
			layout->art.append(
				DividerArtPlacement{box, piece.shape, piece.svgPath, mirrored, piece.rotation});
			break;

		case DividerPiece::Kind::Text:
			layout->texts.append(DividerLayout::TextPiece{piece.text, box, turn});
			break;

		case DividerPiece::Kind::Logo:
			layout->logos.append(DividerLayout::LogoPiece{measured.art, &piece.logo, box.toRect(), turn});
			break;
		}

		cursor += measured.size.width();
	}
}

/*
 * Composes a divider across `width`, starting at `x`, with its content beginning at `top`.
 *
 * Nothing is painted here: the whole figure is measured and placed, and the caller paints from
 * the result. That is what keeps the measure pass and the render pass in agreement -- they are
 * the same call, and a divider whose height is reported as one thing and drawn as another would
 * shift every section under it by the difference.
 */
DividerLayout layoutDivider(const Section &section, const TextStyle &style, const LogoSource &logos,
			    DividerArtCache *dividers, qreal x, qreal top, qreal width)
{
	DividerLayout layout;

	const qreal thickness = section.dividerThickness;
	if (!dividers || thickness <= 0.0 || width <= 0.0)
		return layout;

	/* --- Measure the three stacks ---------------------------------------------------- */

	const qreal pieceGap = section.dividerPieceGap;
	const auto measure = [&](const QVector<DividerPiece> &pieces) {
		return measureDividerStack(pieces, style, logos, dividers, thickness, pieceGap, width);
	};

	const MeasuredStack center = measure(section.dividerCenter);
	const MeasuredStack leftEnd = measure(section.dividerCap);
	/*
	 * Mirroring says the two ends are the same list, not that the right-hand one is drawn
	 * unflipped: an end is always drawn as a mirror image of the way it is written, whichever
	 * list it came from, so the divider cannot come out pointing the same way at both ends.
	 */
	const MeasuredStack rightEnd = section.dividerMirrorEnds ? leftEnd : measure(section.dividerEndCap);

	const int rules = std::clamp(section.dividerRules, 1, 16);
	const qreal stackHeight = rules * thickness + (rules - 1) * section.dividerRuleGap;

	layout.height = std::max({stackHeight, leftEnd.height, rightEnd.height, center.height});
	if (layout.height <= 0.0)
		return layout;

	const qreal midY = top + layout.height / 2.0;

	/* --- Work out where the arms run ------------------------------------------------- */

	/*
	 * Centered on the section's own box rather than between the two arms, so a divider whose
	 * ends differ still has its ornament on the middle of the line the eye follows.
	 */
	const qreal centerLeft = x + (width - center.width) / 2.0;

	const bool connect = section.dividerConnect;

	/*
	 * Connected, the rule runs to the middle of the outermost piece of each end rather than
	 * stopping outside the stack: that point is inside every cap's silhouette whatever the cap
	 * is, where running to the stack's outer edge would poke a blunt nose out of a tapering
	 * arrowhead. Otherwise the arm keeps clear by the section's gap -- which may itself be
	 * negative, and is then an overlap by exactly the same reasoning, only asked for by hand.
	 *
	 * Either way the span is held inside the section's own box. A join deep enough to push an
	 * arm out of it would have the rule painting over whatever the section above or below drew.
	 */
	qreal armLeft = x;
	qreal armRight = x + width;

	if (connect) {
		armLeft = x + leftEnd.outerHalfWidth();
		armRight = x + width - rightEnd.outerHalfWidth();
	} else {
		/* Nothing to keep clear of, nor to run under, at an end with nothing on it. */
		if (leftEnd.width > 0.0)
			armLeft = std::max(x, x + leftEnd.width + section.dividerGap);
		if (rightEnd.width > 0.0)
			armRight = std::min(x + width, x + width - rightEnd.width - section.dividerGap);
	}

	/* --- Place the arms -------------------------------------------------------------- */

	/*
	 * Before the stacks rather than after them, so a piece is drawn over the rule it sits on
	 * rather than under it. It makes no difference to tinted artwork -- every tinted part goes
	 * into one silhouette, which is what lets an overlap union seamlessly in the first place --
	 * but a custom picture left in its own colors is painted straight to the strip, and a rule
	 * running across the front of it is not the ornament anybody placed.
	 */
	const qreal stackTop = midY - stackHeight / 2.0;

	for (int rule = 0; rule < rules; ++rule) {
		/*
		 * Distance from the middle of the stack in rule steps, so each rule is shorter than
		 * the one nearer the midline by exactly the inset. An even stack has no middle rule
		 * to measure from and its two innermost sit half a step out, which keeps the wedge
		 * symmetric either way.
		 */
		const qreal steps = std::abs(rule - (rules - 1) / 2.0);
		const qreal inset = steps * section.dividerRuleInset;

		const qreal ruleTop = stackTop + rule * (thickness + section.dividerRuleGap);
		const qreal left = armLeft + inset;
		const qreal right = armRight - inset;

		/*
		 * The right-hand arm is mirrored for the same reason the right-hand cap is: an arm
		 * that is not symmetric in itself -- a taper running from a hairline up to full
		 * thickness, a rule ticked off at one edge of each tile -- would otherwise point the
		 * same way on both sides and leave the divider lopsided. Mirroring a tile that *is*
		 * symmetric costs a transform and changes nothing, which is why it is unconditional
		 * rather than a property some shapes declare and others forget to.
		 */
		const auto placeArm = [&](qreal from, qreal to, bool mirrored) {
			if (to <= from)
				return;
			const QVector<QRectF> tiles = layoutDividerArm(section.dividerArm, section.dividerArmSvg,
								       dividers,
								       QRectF(from, ruleTop, to - from, thickness));
			for (const QRectF &tile : tiles) {
				layout.art.append(
					DividerArtPlacement{tile, section.dividerArm, section.dividerArmSvg, mirrored});
			}
		};

		if (center.isEmpty() || connect) {
			/*
			 * Nothing in the way -- or nothing the rule is asked to stop for -- so it
			 * runs from one cap straight to the other, as one arm rather than two
			 * touching: a scaling shape is stretched once across the whole span and a
			 * spreading one is not made to meet in the middle. A shape with a direction
			 * to it points one way along the whole rule, which for an unbroken line is
			 * the only reading there is.
			 *
			 * Connected, the center is then drawn on top of this, which is how an
			 * ornamental rule is actually composed: the diamond sits *on* the line
			 * rather than between two lines that stop short of it.
			 */
			placeArm(left, right, false);
		} else {
			/*
			 * Each arm still stops at its own end of the span. A join negative enough
			 * to run one arm past the center would otherwise carry it out through the
			 * far cap and off the section's box.
			 */
			placeArm(left, std::min(right, centerLeft - section.dividerGap), false);
			placeArm(std::max(left, centerLeft + center.width + section.dividerGap), right, true);
		}
	}

	/* --- Place the ends and the center ------------------------------------------------ */

	/*
	 * The ends sit at the divider's full extent and on its midline, and are drawn once however
	 * many rules run between them: three lines meeting one arrowhead is the figure the deco
	 * rules draw, where three arrowheads stacked on top of each other is not.
	 */
	placeDividerStack(&layout, leftEnd, x, midY, false);
	placeDividerStack(&layout, rightEnd, x + width - rightEnd.width, midY, true);
	placeDividerStack(&layout, center, centerLeft, midY, false);

	return layout;
}

/* How far outside its own box one section can paint. Zero for a section that draws nothing. */
double sectionBleed(const Document &document, const Section &section)
{
	if (!section.visible)
		return 0.0;

	/*
	 * A panel's outset reaches outside the box it was given exactly as a shadow does, and is
	 * counted first because it is the one thing here a section of *any* type can carry -- a
	 * Spacer with a band of color across it draws nothing else at all, and a bleed taken only
	 * from the styles would cut that band off at the nearest tile seam.
	 */
	double bleed = 0.0;
	for (const SectionBackground &entry : section.backgrounds)
		bleed = std::max(bleed, document.effectiveBackground(section, entry.slot).bleed());

	/*
	 * Logos cast the style's shadow as well, so a section that only places art counts
	 * too -- and a divider counts whatever its center holds, since its rule is inked by
	 * the same style and can carry the same shadow with nothing but artwork in it.
	 */
	if (!(sectionUsesText(section.type) || sectionUsesLogos(section.type) ||
	      section.type == SectionType::SectionDivider))
		return bleed;

	bleed = std::max({bleed, document.effectiveStyle(section).effectBleed(),
			  document.effectiveSecondaryStyle(section).effectBleed()});
	/*
	 * A bridge inked separately can carry a heavier shadow than either text beside it, and
	 * it is counted for every section rather than only the bridged shapes: the override
	 * costs nothing to resolve for a section that never draws a bridge, and the alternative
	 * is this predicate and the layout switch's having to agree on which types those are.
	 */
	return std::max(bleed, document.effectiveBridgeStyle(section).effectBleed());
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
int layoutSection(QPainter *painter, const Section &section, const Document &document, const LogoSource &logos,
		  BridgeArtCache *bridges, DividerArtCache *dividers, int top, BoxCollector *boxes = nullptr,
		  AnimatedCollector *animated = nullptr, StickyCollector *sticky = nullptr);

/*
 * Lays a sticky block's children out, one under the next, and returns the height they come to.
 *
 * The children go through the very same layout call every other section does -- they *are* sections
 * -- so a block holds whatever the roll holds and nothing here has to know what any of it is. What
 * they do not get is the sticky collector: a block cannot hold a block, and the loader drops any
 * that tries.
 */
int layoutStickyChildren(QPainter *painter, const Section &block, const Document &document, const LogoSource &logos,
			 BridgeArtCache *bridges, DividerArtCache *dividers, int top, BoxCollector *boxes,
			 AnimatedCollector *animated)
{
	int y = top;
	for (const Section &child : block.children) {
		if (!child.visible)
			continue;

		y += layoutSection(painter, child, document, logos, bridges, dividers, y, boxes, animated);
	}

	return y - top;
}

int layoutSection(QPainter *painter, const Section &section, const Document &document, const LogoSource &logos,
		  BridgeArtCache *bridges, DividerArtCache *dividers, int top, BoxCollector *boxes,
		  AnimatedCollector *animated, StickyCollector *sticky)
{
	/* Reads as one call at every site whether or not anyone is collecting. */
	const auto record = [boxes](LayoutBox::Kind kind, const QRectF &rect) {
		if (boxes)
			boxes->add(kind, rect);
	};

	/*
	 * Every logo goes through here: it is what keeps the overlay's box and the animated logo's
	 * hole in agreement, since neither can be reported without the other.
	 */
	const auto recordLogo = [&record, animated](const LogoArt &art, const LogoRef &ref, const QRect &box,
						    const TextStyle &ink) {
		record(LayoutBox::Kind::Logo, QRectF(box));
		if (animated)
			animated->add(art, ref, box, ink);
	};

	/*
	 * The section's box: a share of the canvas width, placed within it by `sectionAlign`, with
	 * `marginX` taken off each of the box's own edges. A margin alone can only ever center the
	 * content, since it insets both sides equally; the box is what lets a section sit against
	 * one edge of the canvas with the margin still holding it clear of that edge.
	 *
	 * At the defaults -- the full width, centered -- the box is the canvas and this is exactly
	 * the inset from both edges that it has always been.
	 */
	/*
	 * A sticky block is the exception: it spans the canvas whatever these say, because what it
	 * holds is whole sections and each of them carries a box of its own. Two nested shares of the
	 * width would be two settings for one thing, and the inner one is the one that can already
	 * say everything the outer one could.
	 */
	const bool spansCanvas = section.type == SectionType::StickyBlock;

	const qreal boxWidth = spansCanvas ? document.width
					   : std::clamp(section.sectionWidth, 0.0, 1.0) * document.width;
	const int boxX = spansCanvas ? 0 : qRound(alignOffset(section.sectionAlign, document.width, boxWidth));

	/*
	 * The nudge is added to the content's left edge and taken out of nothing: the whole of what
	 * the section draws slides with it, keeping the arrangement inside the box rather than
	 * reflowing into a narrower one. A block spans the canvas and its children carry offsets of
	 * their own, so it takes none of its own -- see Section::contentOffsetX.
	 */
	const int contentX = spansCanvas ? 0 : boxX + section.marginX + section.contentOffsetX;
	const int contentWidth = spansCanvas ? document.width : std::max(1, qRound(boxWidth) - section.marginX * 2);

	/* Resolved once here so nothing below can accidentally bypass a preset binding. */
	const TextStyle &style = document.effectiveStyle(section);

	const SectionPanels panels(painter, document, section, logos.stills);

	/*
	 * The section's own panel, which sits behind everything below and therefore has to go down
	 * before any of it -- and needs the section's finished height, which is not known until all of
	 * it has been laid out. So the section measures itself once and then draws.
	 *
	 * The recursion terminates on the painter: the measuring call passes none, and a call with no
	 * painter paints no panel and so never measures again. It costs a second pass over one
	 * section, and only over a section that has actually been given a panel.
	 *
	 * A sticky block is the exception. It leaves a hole in the strip rather than drawing into it,
	 * so a panel painted here would be a card hanging in the roll where the block used to be; its
	 * panel goes into the block's own picture instead, down in the branch below.
	 */
	if (section.type != SectionType::StickyBlock && panels.wants(BackgroundSlot::Section)) {
		const int measured = layoutSection(nullptr, section, document, logos, bridges, dividers, top);
		panels.paint(BackgroundSlot::Section, QRectF(boxX, top, boxWidth, measured));
	}

	int y = top + section.paddingTop;
	const int contentTop = y;

	switch (section.type) {
	case SectionType::Spacer:
		y += section.spacerHeight;
		break;

	case SectionType::StickyBlock: {
		/*
		 * The block takes up its slot in the roll exactly as its content would have inline, so
		 * everything above and below it sits where it always did and the slot goes on scrolling
		 * after the block has detached from it. What it does *not* do is draw into the strip:
		 * with a painter this leaves the slot empty, and the block itself is handed to the
		 * compositor as a picture of its own.
		 *
		 * Its children are laid out without the animation cache, which is the documented way to
		 * ask for every logo as a still: the strip's hole-and-overlay trick has nowhere to put a
		 * second hole inside a picture that is itself drawn as one quad, and a logo left as a
		 * hole with nothing over it would simply not be drawn at all.
		 */
		const LogoSource stills{logos.stills, nullptr};

		const int height =
			layoutStickyChildren(nullptr, section, document, stills, bridges, dividers, y, boxes, nullptr);
		record(LayoutBox::Kind::Sticky, QRectF(0, y, document.width, height));

		if (sticky && height > 0) {
			/*
			 * Rasterized at the strip's full width, because a child places itself across
			 * the canvas exactly as a top-level section does -- its own `sectionWidth` and
			 * placement are what narrow it, and they would mean something different if the
			 * block handed them a narrower canvas to sit in.
			 *
			 * The picture is grown by the roll's bleed at top and bottom: a shadow cast by
			 * the first or last child reaches outside the slot, and unlike the strip -- where
			 * the neighboring tile catches it -- there is nothing outside this picture to
			 * catch it in.
			 */
			const int bleed = qCeil(sectionBleed(document, section));
			int blockBleed = bleed;
			for (const Section &child : section.children)
				blockBleed = std::max(blockBleed, qCeil(sectionBleed(document, child)));

			StickyBlockPlacement placement;
			placement.rect = QRectF(0, y, document.width, height);
			placement.anchor = section.stickyAnchor;
			placement.canvasPosition = section.stickyCanvasPosition;
			placement.offset = section.stickyOffset;
			placement.hold = section.stickyHold;
			placement.holdForever = section.stickyHoldForever;
			placement.release = section.stickyRelease;
			/*
			 * A panel's outset reaches outside the slot as surely as a shadow does, so the
			 * picture is grown by whichever of the two asks for more.
			 */
			const BackgroundPanel &blockPanel =
				document.effectiveBackground(section, BackgroundSlot::Section);
			const int margin = std::max(blockBleed, qCeil(blockPanel.bleed()));
			placement.margin = margin;

			QImage image(std::max(1, document.width), std::max(1, height + margin * 2),
				     QImage::Format_ARGB32_Premultiplied);
			image.fill(Qt::transparent);

			QPainter blockPainter(&image);
			blockPainter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
						    QPainter::SmoothPixmapTransform);
			/* The children are laid out in strip space; shift the picture under them. */
			blockPainter.translate(0, -(y - margin));

			/*
			 * The panel goes down first, under everything the block holds: keeping the roll
			 * running past behind the block from reading through its lettering is a job it
			 * can only do from underneath. It is painted into the picture rather than carried
			 * out as a color to be drawn behind it, because a flat quad would mean a second
			 * effect started inside the pass that is drawing the strip, and the panel is a
			 * shape the rasterizer can fill for nothing at rebuild time.
			 */
			paintBackgroundPanel(&blockPainter, blockPanel, QRectF(0, y, document.width, height),
					     stills.stills);

			layoutStickyChildren(&blockPainter, section, document, stills, bridges, dividers, y, nullptr,
					     nullptr);
			blockPainter.end();

			/* Straight alpha, for the same reason the tiles are. */
			placement.image = image.convertToFormat(QImage::Format_ARGB32);
			sticky->add(std::move(placement));
		}

		y += height;
		break;
	}

	case SectionType::SectionDivider: {
		const DividerLayout divider = layoutDivider(section, style, logos, dividers, contentX, y, contentWidth);

		if (painter && divider.height > 0.0) {
			const QRectF fillBox(contentX, y, contentWidth, divider.height);

			/*
			 * The divider's own panel, over the box its artwork occupies rather than the
			 * section's -- which is the narrower and shorter of the two, since a divider set
			 * narrower than its section sits centered in it. Down before the art, so a rule
			 * drawn on a band reads as being on it.
			 */
			panels.paint(BackgroundSlot::Divider, fillBox);

			/*
			 * The artwork takes the bridge's ink override, because "color the art
			 * apart from the words" is one want with two names: a divider whose rule
			 * carries the title's gold sweep while its own label stays white is the
			 * same edit as yellow leader dots under white names.
			 */
			paintDividerArt(painter, divider.art, section, document.effectiveBridgeStyle(section), dividers,
					fillBox);

			/*
			 * A word and a mark in the middle of a divider go through the very helpers
			 * that draw a section's own title and its own logo, so they pick up the
			 * style's gradient, outline and shadow without a second implementation of
			 * any of it.
			 */
			for (const DividerLayout::TextPiece &piece : divider.texts) {
				const TurnedPainter turned(painter, piece.rect, piece.rotation);
				layoutText(painter, piece.text, style, piece.rect.left(), piece.rect.top(),
					   piece.rect.width());
			}

			for (const DividerLayout::LogoPiece &piece : divider.logos) {
				const TurnedPainter turned(painter, piece.rect, piece.rotation);
				paintLogo(painter, piece.art, piece.rect, style, piece.ref->playback);
			}
		}

		for (const DividerLayout::TextPiece &piece : divider.texts)
			record(LayoutBox::Kind::Text, piece.rect);
		for (const DividerLayout::LogoPiece &piece : divider.logos)
			recordLogo(piece.art, *piece.ref, piece.rect, style);

		record(LayoutBox::Kind::Divider, QRectF(contentX, y, contentWidth, divider.height));
		y += qRound(divider.height);
		break;
	}

	case SectionType::Title:
	case SectionType::Header: {
		/* Measured before it is drawn only when there is a panel to go under it; see SectionPanels. */
		if (panels.wants(BackgroundSlot::Title)) {
			const int measured = layoutText(nullptr, section.text, style, contentX, y, contentWidth);
			if (measured > 0)
				panels.paint(BackgroundSlot::Title, QRectF(contentX, y, contentWidth, measured));
		}

		const int height = layoutText(painter, section.text, style, contentX, y, contentWidth);
		record(LayoutBox::Kind::Text, QRectF(contentX, y, contentWidth, height));
		y += height;
		break;
	}

	case SectionType::TitleWithSubtitle:
	case SectionType::HeaderWithSubtitle: {
		/*
		 * The section's own pair, through the very stack a title/subtitle list lays each of its
		 * entries out with. A heading with a line under it and a list of one pair are the same
		 * geometry, so `subtitleGap`, `subtitleFirst` and the empty-line courtesy come across
		 * whole rather than being written a second time and drifting from it.
		 */
		y += layoutTitleSubtitle(painter, section, style, document.effectiveSecondaryStyle(section),
					 section.text, section.secondaryText, contentX, y, contentWidth, record,
					 &panels);
		break;
	}

	case SectionType::LogoTitle:
	case SectionType::LogoHeader: {
		const LogoArt art = logos.resolve(section.logo);
		const QSize size = logoDrawSize(art.poster, section.logo, contentWidth);
		const int x = contentX + qRound(alignOffset(style.align, contentWidth, size.width()));
		const QRect box(QPoint(x, y), size);
		/*
		 * Behind the artwork's own box rather than the column it was placed in, so a card sits
		 * under the wordmark rather than running the width of the section. The outset is what
		 * takes it wider when that is what is wanted.
		 */
		panels.paint(BackgroundSlot::Logo, QRectF(box));
		paintLogo(painter, art, box, style, section.logo.playback);
		recordLogo(art, section.logo, box, style);
		y += size.height();
		break;
	}

	case SectionType::TitleWithLogo:
	case SectionType::HeaderWithLogo:
	case SectionType::TitleWithSubtitleAndLogo:
	case SectionType::HeaderWithSubtitleAndLogo: {
		/*
		 * The four shapes differ in what goes in the text column and in nothing else: the column
		 * is measured, divided from the logo and centered against it the same way whether it holds
		 * one line or a pair. That is why they share a branch rather than a second copy of the
		 * placement -- the alternative is two logo rows that agree until one of them is edited.
		 *
		 * A single-line type is the pair with an empty subtitle, which the stack already draws as
		 * the one line at the one height, taking no gap with it. So the plain shapes go through
		 * the same call and come out where they always did.
		 */
		const TextStyle &subtitleStyle = document.effectiveSecondaryStyle(section);
		const QString subtitle = sectionUsesSubtitles(section.type) ? section.secondaryText : QString();
		const bool hasText = !section.text.isEmpty() || !subtitle.isEmpty();

		const LogoArt art = logos.resolve(section.logo);
		const int logoBudget = std::max(1, contentWidth / 3);
		const QSize logoSize = logoDrawSize(art.poster, section.logo, logoBudget);

		const LogoRow row = placeLogoRow(section, contentX, contentWidth, logoSize.width(),
						 naturalPairWidth(section.text, subtitle, style, subtitleStyle),
						 hasText);

		/*
		 * Measured with nothing collected, because where the block goes is not known until its
		 * height is: the boxes are recorded by the placing pass below, at the top the row
		 * actually gives it.
		 */
		const int textHeight = layoutTitleSubtitle(nullptr, section, style, subtitleStyle, section.text,
							   subtitle, 0, 0, row.textWidth, kIgnoreBoxes);
		const int rowHeight = std::max(logoSize.height(), textHeight);
		const qreal textTop = y + (rowHeight - textHeight) / 2.0;

		/* The logo and the text block are centered against each other within the row. */
		const QRect logoBox(QPoint(qRound(row.logoX), y + (rowHeight - logoSize.height()) / 2), logoSize);
		panels.paint(BackgroundSlot::Logo, QRectF(logoBox));
		paintLogo(painter, art, logoBox, style, section.logo.playback);
		recordLogo(art, section.logo, logoBox, style);

		/* Records the box of each line it actually draws, so the overlay is the stack's own. */
		layoutTitleSubtitle(painter, section, style, subtitleStyle, section.text, subtitle, row.textX, textTop,
				    row.textWidth, record, &panels);

		if (section.logoPlacement == LogoPlacement::Bridged) {
			/*
			 * The bridge's own ink over the row's own font, so recoloring a leader cannot
			 * move the row it runs through. Resolved here rather than beside `style`
			 * because it is a merge and costs a copy, which only the two bridged shapes
			 * have any use for.
			 */
			const TextStyle bridgeStyle = document.effectiveBridgeStyle(section);
			const PreparedBridge bridge = prepareBridge(section, bridgeStyle, bridges, row.bridgeWidth);
			/*
			 * Hung off the text's own baseline, so a leader lands on it whatever the
			 * bridge is made of: text in the same font needs no offset at all, and art
			 * sits on the same line rather than on the top of the text's box. A row with
			 * no text at all still has the font's own ascent to hang it from.
			 *
			 * With a subtitle it is the baseline of whichever line is drawn on top, so
			 * adding a second line under a bridged heading leaves the leader exactly where
			 * it was -- and flipping the stack moves it to the line that took the top.
			 */
			const StackedLine top = topStackedLine(section, section.text, subtitle, style, subtitleStyle);
			const qreal textAscent = top.text->isEmpty()
							 ? QFontMetricsF(makeFont(*top.style)).ascent()
							 : firstBaseline(*top.text, *top.style, row.textWidth);
			const qreal baseline = textTop + textAscent;
			const qreal bridgeTop = baseline - bridge.ascent(section);

			/*
			 * The leader's own panel. Measured first for the same reason a line of text is:
			 * how tall a bridge draws depends on what it is made of.
			 */
			if (panels.wants(BackgroundSlot::Bridge)) {
				const int measured = paintBridge(nullptr, bridge, section, bridgeStyle, bridges,
								 row.bridgeX, bridgeTop, row.bridgeWidth);
				if (measured > 0) {
					panels.paint(BackgroundSlot::Bridge,
						     QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, measured));
				}
			}

			const int bridgeHeight = paintBridge(painter, bridge, section, bridgeStyle, bridges,
							     row.bridgeX, bridgeTop, row.bridgeWidth);
			record(LayoutBox::Kind::Bridge, QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, bridgeHeight));
		}

		y += rowHeight;
		break;
	}

	case SectionType::Bridged: {
		const TextStyle &rightStyle = document.effectiveSecondaryStyle(section);
		/*
		 * The two subtitles, and the empty string that stands in for them when the section
		 * does not draw any. Standing one in rather than testing the flag at each use is what
		 * makes a row without subtitles go through the very same code as one with them: an
		 * empty line takes no height and no gap, so the pair collapses to exactly the single
		 * line a bridged row has always been, measured and placed identically.
		 */
		const TextStyle &leftSubtitleStyle = document.effectiveRowSubtitleStyle(section);
		const TextStyle &rightSubtitleStyle = document.effectiveRowSecondarySubtitleStyle(section);
		const QString noSubtitle;
		/*
		 * The bridge's own ink over the row's own font. Everything below measures from the
		 * fields the merge leaves alone -- the string is set in the row's face at the row's
		 * size -- so a leader given a color of its own reserves exactly the width it did.
		 */
		const TextStyle bridgeStyle = document.effectiveBridgeStyle(section);
		const qreal naturalBridge = naturalBridgeWidth(section, bridgeStyle, bridges);

		for (int index = 0; index < section.entries.size(); ++index) {
			const Entry &entry = section.entries.at(index);
			const BridgedSide left{&entry.text, section.rowSubtitles ? &entry.subtitle : &noSubtitle,
					       &style, &leftSubtitleStyle};
			const BridgedSide right{&entry.secondaryText,
						section.rowSubtitles ? &entry.secondarySubtitle : &noSubtitle,
						&rightStyle, &rightSubtitleStyle};

			const BridgedRow row = placeBridgedRow(
				section, left, right, indentedX(section, entry, contentX), contentWidth, naturalBridge);
			const PreparedBridge bridge = prepareBridge(section, bridgeStyle, bridges, row.bridgeWidth);

			/*
			 * The three parts share a baseline rather than a top edge, which is what
			 * keeps a leader running through the middle of the text when the two
			 * sides are set at different sizes. It is anchored on whichever part
			 * reaches lowest, so nothing climbs above the row into the one before it.
			 *
			 * A side carrying a subtitle is anchored on the line that ends up on *top* of
			 * its stack -- the same rule a bridged logo row follows, and for the same reason:
			 * adding a subtitle under a row should leave the leader exactly where it was
			 * rather than dragging it down to the middle of a two-line block.
			 */
			const StackedLine leftTopLine =
				topStackedLine(section, *left.text, *left.subtitle, *left.style, *left.subtitleStyle);
			const StackedLine rightTopLine = topStackedLine(section, *right.text, *right.subtitle,
									*right.style, *right.subtitleStyle);

			const qreal leftAscent = firstBaseline(*leftTopLine.text, *leftTopLine.style, row.leftWidth);
			const qreal rightAscent =
				firstBaseline(*rightTopLine.text, *rightTopLine.style, row.rightWidth);
			const qreal bridgeAscent = bridge.ascent(section);
			const qreal baseline = std::max({leftAscent, rightAscent, bridgeAscent});

			const qreal leftTop = y + baseline - leftAscent;
			const qreal rightTop = y + baseline - rightAscent;
			const qreal bridgeTop = y + baseline - bridgeAscent;

			/*
			 * How tall the row comes to, given what its three parts came to. Written once
			 * because the row's own panel needs the answer *before* anything is drawn, and a
			 * second copy of it would be a panel that fits the row until one of the three
			 * grows.
			 */
			const auto rowExtent = [&](int leftHeight, int rightHeight, int bridgeHeight) {
				int rowHeight = 1;
				const auto extend = [&rowHeight, y](qreal partTop, int height) {
					/* An empty part has no height and must not push the row down. */
					if (height > 0)
						rowHeight = std::max(rowHeight, qCeil(partTop - y) + height);
				};

				extend(leftTop, leftHeight);
				extend(rightTop, rightHeight);
				extend(bridgeTop, bridgeHeight);
				return rowHeight;
			};

			/*
			 * The row's panel spans the section's full width rather than hugging the two
			 * texts: what a striped list is striping is the row, and a band that stopped at
			 * the ink would leave a ragged edge down the middle of the leader.
			 */
			const BackgroundSlot entrySlot = panels.entrySlot(index);
			if (panels.wants(entrySlot)) {
				const int measured = rowExtent(
					layoutTitleSubtitle(nullptr, section, *left.style, *left.subtitleStyle,
							    *left.text, *left.subtitle, row.leftX, leftTop,
							    row.leftWidth, kIgnoreBoxes),
					layoutTitleSubtitle(nullptr, section, *right.style, *right.subtitleStyle,
							    *right.text, *right.subtitle, row.rightX, rightTop,
							    row.rightWidth, kIgnoreBoxes),
					paintBridge(nullptr, bridge, section, bridgeStyle, bridges, row.bridgeX,
						    bridgeTop, row.bridgeWidth));

				panels.paint(entrySlot,
					     QRectF(indentedX(section, entry, contentX), y, contentWidth, measured));
			}

			if (panels.wants(BackgroundSlot::Bridge)) {
				const int measured = paintBridge(nullptr, bridge, section, bridgeStyle, bridges,
								 row.bridgeX, bridgeTop, row.bridgeWidth);
				if (measured > 0) {
					panels.paint(BackgroundSlot::Bridge,
						     QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, measured));
				}
			}

			/*
			 * Here the two text slots are the row's two *columns*, not the two lines of one
			 * of them -- which is exactly how the two styles map, `style` being the left side
			 * and `secondaryStyle` the right. So each side's panel goes behind that side's
			 * whole block, subtitle and all, and the pair helper is handed no panels of its
			 * own: it would otherwise paint the Title slot behind the left column's top line
			 * and again behind the right column's, which is one slot claiming two places.
			 */
			const auto panelBehindSide = [&](BackgroundSlot slot, const BridgedSide &side, qreal sideX,
							 qreal sideTop, qreal sideWidth) {
				if (!panels.wants(slot))
					return;

				const int measured = layoutTitleSubtitle(nullptr, section, *side.style,
									 *side.subtitleStyle, *side.text,
									 *side.subtitle, sideX, sideTop, sideWidth,
									 kIgnoreBoxes);
				if (measured > 0)
					panels.paint(slot, QRectF(sideX, sideTop, sideWidth, measured));
			};

			panelBehindSide(BackgroundSlot::Title, left, row.leftX, leftTop, row.leftWidth);
			panelBehindSide(BackgroundSlot::Subtitle, right, row.rightX, rightTop, row.rightWidth);

			/*
			 * Both sides go through the pair helper whether or not they carry a subtitle,
			 * which is what records their text boxes as well as drawing them.
			 */
			const int leftHeight = layoutTitleSubtitle(painter, section, *left.style, *left.subtitleStyle,
								   *left.text, *left.subtitle, row.leftX, leftTop,
								   row.leftWidth, record);
			const int rightHeight = layoutTitleSubtitle(painter, section, *right.style,
								    *right.subtitleStyle, *right.text, *right.subtitle,
								    row.rightX, rightTop, row.rightWidth, record);
			const int bridgeHeight = paintBridge(painter, bridge, section, bridgeStyle, bridges,
							     row.bridgeX, bridgeTop, row.bridgeWidth);

			record(LayoutBox::Kind::Bridge, QRectF(row.bridgeX, bridgeTop, row.bridgeWidth, bridgeHeight));

			y += rowExtent(leftHeight, rightHeight, bridgeHeight) + section.entryGap;
		}

		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::TextList: {
		for (int index = 0; index < section.entries.size(); ++index) {
			const Entry &entry = section.entries.at(index);
			const qreal x = indentedX(section, entry, contentX);

			const BackgroundSlot entrySlot = panels.entrySlot(index);
			if (panels.wants(entrySlot)) {
				const int measured = layoutText(nullptr, entry.text, style, x, y, contentWidth);
				if (measured > 0)
					panels.paint(entrySlot, QRectF(x, y, contentWidth, measured));
			}

			const int height = layoutText(painter, entry.text, style, x, y, contentWidth);
			record(LayoutBox::Kind::Text, QRectF(x, y, contentWidth, height));
			y += height;
			y += section.entryGap;
		}
		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::TitleSubtitleList: {
		const TextStyle &subtitleStyle = document.effectiveSecondaryStyle(section);

		for (int index = 0; index < section.entries.size(); ++index) {
			const Entry &entry = section.entries.at(index);
			const qreal x = indentedX(section, entry, contentX);

			/*
			 * The entry's panel goes behind the whole pair; the Title and Subtitle panels go
			 * behind its two lines, which the pair helper puts down as it places each of them.
			 */
			const BackgroundSlot entrySlot = panels.entrySlot(index);
			if (panels.wants(entrySlot)) {
				const int measured = layoutTitleSubtitle(nullptr, section, style, subtitleStyle,
									 entry.text, entry.secondaryText, x, y,
									 contentWidth, kIgnoreBoxes);
				if (measured > 0)
					panels.paint(entrySlot, QRectF(x, y, contentWidth, measured));
			}

			y += layoutTitleSubtitle(painter, section, style, subtitleStyle, entry.text,
						 entry.secondaryText, x, y, contentWidth, record, &panels);
			y += section.entryGap;
		}
		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::LogoList: {
		for (int index = 0; index < section.entries.size(); ++index) {
			const Entry &entry = section.entries.at(index);
			const LogoArt art = logos.resolve(entry.logo);
			const QSize size = logoDrawSize(art.poster, entry.logo, contentWidth);
			const int x = qRound(indentedX(section, entry, contentX) +
					     alignOffset(style.align, contentWidth, size.width()));
			const QRect box(QPoint(x, y), size);

			/*
			 * Two panels that mean two different things: the entry's runs the width of the
			 * list, so a striped run of sponsors reads as rows, while the logo's hugs the
			 * artwork, which is the card behind one mark.
			 */
			panels.paint(panels.entrySlot(index),
				     QRectF(indentedX(section, entry, contentX), y, contentWidth, size.height()));
			panels.paint(BackgroundSlot::Logo, QRectF(box));

			paintLogo(painter, art, box, style, entry.logo.playback);
			recordLogo(art, entry.logo, box, style);
			y += size.height() + section.entryGap;
		}
		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::MultiTextList:
	case SectionType::MultiTitleSubtitleList:
	case SectionType::MultiLogoList: {
		const int columns = std::max(1, section.columns);
		const int count = section.entries.size();
		if (count == 0)
			break;

		const int columnWidth = std::max(1, (contentWidth - (columns - 1) * section.columnGap) / columns);
		const int rows = (count + columns - 1) / columns;
		const bool logoMode = section.type == SectionType::MultiLogoList;
		const bool subtitleMode = sectionUsesSubtitles(section.type);
		const TextStyle &subtitleStyle = document.effectiveSecondaryStyle(section);

		/*
		 * Across-fill walks the row before wrapping; the default down-fill completes each column
		 * before moving right, which is what reads naturally in a long alphabetized list.
		 */
		const auto indexAt = [&](int row, int column) {
			return section.fillAcross ? row * columns + column : column * rows + row;
		};

		const auto cellX = [&](const Entry &entry, int column) {
			return qRound(indentedX(section, entry, contentX + column * (columnWidth + section.columnGap)));
		};

		/* What one cell comes to, drawing nothing. Only a paneled row ever asks. */
		const auto measureCell = [&](const Entry &entry, int x, int top) {
			if (logoMode) {
				const LogoArt art = logos.resolve(entry.logo);
				return logoDrawSize(art.poster, entry.logo, columnWidth).height();
			}
			if (subtitleMode) {
				return layoutTitleSubtitle(nullptr, section, style, subtitleStyle, entry.text,
							   entry.secondaryText, x, top, columnWidth, kIgnoreBoxes);
			}
			return layoutText(nullptr, entry.text, style, x, top, columnWidth);
		};

		for (int row = 0; row < rows; ++row) {
			int rowHeight = 0;

			/*
			 * A cell's panel is the height of its *row* rather than of its own content, so the
			 * cards across a row line up instead of stepping with whichever name happened to
			 * wrap. That means the row has to be measured before any of it is drawn -- but only
			 * when something in it will actually paint, which is what keeps an unpaneled grid
			 * laying its text out exactly once.
			 */
			bool rowPaneled = false;
			for (int column = 0; column < columns && !rowPaneled; ++column) {
				const int index = indexAt(row, column);
				if (index < count && panels.wants(panels.entrySlot(index)))
					rowPaneled = true;
			}

			if (rowPaneled) {
				int measured = 0;
				for (int column = 0; column < columns; ++column) {
					const int index = indexAt(row, column);
					if (index >= count)
						continue;

					const Entry &entry = section.entries.at(index);
					measured = std::max(measured, measureCell(entry, cellX(entry, column), y));
				}

				for (int column = 0; column < columns; ++column) {
					const int index = indexAt(row, column);
					if (index >= count)
						continue;

					const Entry &entry = section.entries.at(index);
					panels.paint(panels.entrySlot(index),
						     QRectF(cellX(entry, column), y, columnWidth, measured));
				}
			}

			for (int column = 0; column < columns; ++column) {
				const int index = indexAt(row, column);
				if (index >= count)
					continue;

				const Entry &entry = section.entries.at(index);
				const int x = cellX(entry, column);

				if (logoMode) {
					const LogoArt art = logos.resolve(entry.logo);
					const QSize size = logoDrawSize(art.poster, entry.logo, columnWidth);
					const int logoX =
						x + qRound(alignOffset(style.align, columnWidth, size.width()));
					const QRect box(QPoint(logoX, y), size);
					panels.paint(BackgroundSlot::Logo, QRectF(box));
					paintLogo(painter, art, box, style, entry.logo.playback);
					recordLogo(art, entry.logo, box, style);
					rowHeight = std::max(rowHeight, size.height());
				} else if (subtitleMode) {
					/*
					 * A row is as tall as the tallest pair in it, so a wrapped title
					 * in one column pushes the next row down rather than overlapping
					 * the entry beneath it.
					 */
					const int height = layoutTitleSubtitle(painter, section, style, subtitleStyle,
									       entry.text, entry.secondaryText, x, y,
									       columnWidth, record, &panels);
					rowHeight = std::max(rowHeight, height);
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

	/*
	 * A sticky block's own children are counted too. They are not drawn into the strip, so they
	 * cannot bleed across a tile seam -- but the block is rasterized into a picture of its own,
	 * and that picture has to be big enough to hold what they paint outside their boxes.
	 */
	visitSections(document.sections,
		      [&](const Section &section) { bleed = std::max(bleed, sectionBleed(document, section)); });

	return qCeil(bleed);
}

QVector<PlacedSection> placeSections(const Document &document, const LogoSource &logos, BridgeArtCache *bridges,
				     DividerArtCache *dividers, int *totalHeight, LayoutBoxes *boxes = nullptr,
				     QVector<AnimatedLogoPlacement> *animated = nullptr,
				     QVector<StickyBlockPlacement> *sticky = nullptr)
{
	QVector<PlacedSection> placed;
	placed.reserve(document.sections.size());

	BoxCollector collector{boxes, -1};
	AnimatedCollector animatedCollector{animated, -1};
	StickyCollector stickyCollector{sticky, -1};

	int y = document.leadIn;
	for (int index = 0; index < document.sections.size(); ++index) {
		const Section &section = document.sections.at(index);
		if (!section.visible)
			continue;

		collector.section = index;
		animatedCollector.section = index;
		stickyCollector.section = index;
		const int height = layoutSection(nullptr, section, document, logos, bridges, dividers, y,
						 boxes ? &collector : nullptr, animated ? &animatedCollector : nullptr,
						 sticky ? &stickyCollector : nullptr);
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

	/*
	 * The mapping itself lives with the panel painter, which sweeps a gradient over a rectangle
	 * for exactly the same reason this does. One copy means a panel and the heading in front of it
	 * carry the same sweep at the same angle rather than two that agree until one is edited.
	 */
	return gradientBrush(style.gradient, style.fill == TextFill::RadialGradient, box);
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

int StripRenderer::measure(const Document &input) const
{
	/* Measured against the fonts the roll will really be drawn in; see render() below. */
	Document resolved;
	const Document &document = documentWithFontsResolved(input, resolved);

	/*
	 * The bridge tiles are parsed for the length of the call and dropped again. Unlike a
	 * decoded logo there is nothing here worth holding on to between renders -- a tile is a
	 * few hundred bytes of markup -- and building it fresh is what makes a custom SVG edited
	 * on disk show up in the next rebuild without anything having to watch the file.
	 */
	BridgeArtCache bridges;
	DividerArtCache dividers;

	int total = 0;
	/*
	 * Measured without the animation cache. An animated logo occupies its first frame's box,
	 * which is the box a still of the same artwork occupies, so the height comes out the same --
	 * and a duration shown while the user is still typing is not worth decoding a video for.
	 */
	placeSections(document, LogoSource{logos, nullptr}, &bridges, &dividers, &total);
	return total - document.leadIn - document.leadOut;
}

Strip StripRenderer::render(const Document &input, LayoutBoxes *boxes) const
{
	/*
	 * Fonts are resolved here rather than by each caller, so nothing can render a roll in a
	 * font the document did not ask for by forgetting to. The common case -- every family
	 * present -- hands `input` straight back and copies nothing.
	 */
	Document resolved;
	const Document &document = documentWithFontsResolved(input, resolved);

	Strip strip;
	strip.width = std::max(1, document.width);

	BridgeArtCache bridges;
	DividerArtCache dividers;

	if (boxes)
		boxes->clear();

	int total = 0;
	const LogoSource art{logos, animations};
	const QVector<PlacedSection> placed = placeSections(document, art, &bridges, &dividers, &total, boxes,
							    &strip.animatedLogos, &strip.stickyBlocks);

	strip.height = std::min(std::max(total, 0), kMaxStripHeight);
	if (strip.height <= 0 || placed.isEmpty()) {
		/* Nothing is drawn over a strip that is not drawn. */
		strip.animatedLogos.clear();
		strip.stickyBlocks.clear();
		return strip;
	}

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

			layoutSection(&painter, *entry.section, document, art, &bridges, &dividers, entry.top);
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
