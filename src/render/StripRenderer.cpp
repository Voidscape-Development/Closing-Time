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
#include <QImageReader>
#include <QPainter>
#include <QTextLayout>
#include <QTextOption>
#include <QtMath>

#include <algorithm>

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
 * Lays `text` out into `width` pixels with an already-prepared font, optionally painting it
 * at (x, y), and returns the height it occupies. Passing a null painter measures without
 * rasterising, which is how the two-pass layout keeps measurement and painting from
 * drifting apart.
 *
 * `wrap` is off for the bridge of a Bridged section: a leader sized to the gap can round a
 * fraction of a pixel over it, and overflowing by that hair reads far better than silently
 * becoming two rows of dots.
 */
int layoutPreparedText(QPainter *painter, const QString &text, const QFont &font, const QColor &color, HAlign align,
		       double lineSpacing, qreal x, qreal y, qreal width, bool wrap = true)
{
	if (text.isEmpty() || width <= 0.0)
		return 0;

	const QFontMetricsF metrics(font);

	/* QTextLayout only breaks on explicit line separators, not on plain newlines. */
	QString content = text;
	content.replace(QLatin1Char('\n'), QChar::LineSeparator);

	QTextLayout layout(content, font);
	QTextOption option;
	option.setWrapMode(wrap ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
	/*
	 * Alignment is deliberately left off the text option and applied per line below.
	 * Setting both makes QTextLayout offset the line and then get offset again here,
	 * which pushes centred text a full half-width to the right.
	 */
	layout.setTextOption(option);

	const qreal step = metrics.lineSpacing() * lineSpacing;

	qreal cursor = 0.0;
	layout.beginLayout();
	for (;;) {
		QTextLine line = layout.createLine();
		if (!line.isValid())
			break;

		line.setLineWidth(width);
		line.setPosition(QPointF(alignOffset(align, width, line.naturalTextWidth()), cursor));
		cursor += step;
	}
	layout.endLayout();

	if (painter) {
		painter->setPen(color);
		layout.draw(painter, QPointF(x, y));
	}

	return qCeil(cursor);
}

int layoutText(QPainter *painter, const QString &text, const TextStyle &style, qreal x, qreal y, qreal width)
{
	return layoutPreparedText(painter, text, makeFont(style), style.color, style.align, style.lineSpacing, x, y,
				  width);
}

/* Width the text wants on its widest line, before any wrapping is imposed on it. */
qreal naturalTextWidth(const QString &text, const TextStyle &style)
{
	if (text.isEmpty())
		return 0.0;

	const QFontMetricsF metrics(makeFont(style));

	qreal widest = 0.0;
	for (const QString &line : text.split(QLatin1Char('\n')))
		widest = std::max(widest, metrics.horizontalAdvance(line));

	return widest;
}

/* Distance from the top of a run of this text down to its first baseline. */
qreal firstBaseline(const QString &text, const TextStyle &style)
{
	if (text.isEmpty())
		return 0.0;

	return QFontMetricsF(makeFont(style)).ascent();
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

void paintLogo(QPainter *painter, const QImage &image, const QRect &rect)
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

	painter->drawImage(rect, image);
}

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
		const qreal groupX = contentX + alignOffset(style.align, contentWidth, groupWidth);

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

		if (section.bridgeFill == BridgeFill::Fixed)
			rightWidth = std::max(0.0, contentWidth - leftWidth - naturalBridge);
		else
			rightWidth = std::min(naturalTextWidth(entry.secondaryText, rightStyle),
					      std::max(0.0, contentWidth - leftWidth));
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

/* The bridge as it will actually be drawn, with the font that makes it span `width`. */
struct PreparedBridge {
	QString text;
	QFont font;
};

PreparedBridge prepareBridge(const Section &section, const TextStyle &style, qreal width)
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

/*
 * Both passes of the layout run through this one function. With `painter` set it draws
 * into the current tile; with `painter` null it only reports the height. `top` is the
 * section's Y position in strip space, which is also painter space -- callers translate
 * the painter by the tile offset before drawing.
 */
int layoutSection(QPainter *painter, const Section &section, const Document &document, LogoCache *logos, int top)
{
	const int contentX = section.marginX;
	const int contentWidth = std::max(1, document.width - section.marginX * 2);

	/* Resolved once here so nothing below can accidentally bypass a preset binding. */
	const TextStyle &style = document.effectiveStyle(section);

	int y = top + section.paddingTop;
	const int contentTop = y;

	switch (section.type) {
	case SectionType::Spacer:
		y += section.spacerHeight;
		break;

	case SectionType::Title:
	case SectionType::Header:
		y += layoutText(painter, section.text, style, contentX, y, contentWidth);
		break;

	case SectionType::LogoTitle:
	case SectionType::LogoHeader: {
		const QImage image = logos->get(section.logo.path, section.logo.maxHeight);
		const QSize size = logoDrawSize(image, section.logo, contentWidth);
		const int x = contentX + qRound(alignOffset(style.align, contentWidth, size.width()));
		paintLogo(painter, image, QRect(QPoint(x, y), size));
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
		paintLogo(painter, image,
			  QRect(QPoint(qRound(row.logoX), y + (rowHeight - logoSize.height()) / 2), logoSize));
		layoutText(painter, section.text, style, row.textX, textTop, row.textWidth);

		if (section.logoPlacement == LogoPlacement::Bridged) {
			const PreparedBridge bridge = prepareBridge(section, style, row.bridgeWidth);
			/*
			 * Drawn from the same top as the text and in the same font, so the leader
			 * lands on the text's baseline without needing an offset of its own.
			 */
			layoutPreparedText(painter, bridge.text, bridge.font, style.color, HAlign::Center,
					   style.lineSpacing, row.bridgeX, textTop, row.bridgeWidth, false);
		}

		y += rowHeight;
		break;
	}

	case SectionType::Bridged: {
		const TextStyle &rightStyle = document.effectiveSecondaryStyle(section);
		const qreal naturalBridge = naturalTextWidth(section.bridge, style);

		for (const Entry &entry : section.entries) {
			const BridgedRow row = placeBridgedRow(section, style, rightStyle, entry, contentX,
							       contentWidth, naturalBridge);
			const PreparedBridge bridge = prepareBridge(section, style, row.bridgeWidth);

			/*
			 * The three parts share a baseline rather than a top edge, which is what
			 * keeps a leader running through the middle of the text when the two
			 * sides are set at different sizes. It is anchored on whichever part
			 * reaches lowest, so nothing climbs above the row into the one before it.
			 */
			const qreal leftAscent = firstBaseline(entry.text, style);
			const qreal rightAscent = firstBaseline(entry.secondaryText, rightStyle);
			const qreal bridgeAscent = bridge.text.isEmpty() ? 0.0 : QFontMetricsF(bridge.font).ascent();
			const qreal baseline = std::max({leftAscent, rightAscent, bridgeAscent});

			const qreal leftTop = y + baseline - leftAscent;
			const qreal rightTop = y + baseline - rightAscent;
			const qreal bridgeTop = y + baseline - bridgeAscent;

			const int leftHeight =
				layoutText(painter, entry.text, style, row.leftX, leftTop, row.leftWidth);
			const int rightHeight = layoutText(painter, entry.secondaryText, rightStyle, row.rightX,
							   rightTop, row.rightWidth);
			const int bridgeHeight = layoutPreparedText(painter, bridge.text, bridge.font, style.color,
								    HAlign::Center, style.lineSpacing, row.bridgeX,
								    bridgeTop, row.bridgeWidth, false);

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
			y += layoutText(painter, entry.text, style, contentX, y, contentWidth);
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
			paintLogo(painter, image, QRect(QPoint(x, y), size));
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
					paintLogo(painter, image, QRect(QPoint(logoX, y), size));
					rowHeight = std::max(rowHeight, size.height());
				} else {
					const int height = layoutText(painter, entry.text, style, x, y, columnWidth);
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
	return (y - top) + section.paddingBottom;
}

struct PlacedSection {
	const Section *section;
	int top;
	int height;
};

QVector<PlacedSection> placeSections(const Document &document, LogoCache *logos, int *totalHeight)
{
	QVector<PlacedSection> placed;
	placed.reserve(document.sections.size());

	int y = document.leadIn;
	for (const Section &section : document.sections) {
		if (!section.visible)
			continue;

		const int height = layoutSection(nullptr, section, document, logos, y);
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
	int total = 0;
	placeSections(document, logos, &total);
	return total - document.leadIn - document.leadOut;
}

Strip StripRenderer::render(const Document &document) const
{
	Strip strip;
	strip.width = std::max(1, document.width);

	int total = 0;
	const QVector<PlacedSection> placed = placeSections(document, logos, &total);

	strip.height = std::min(std::max(total, 0), kMaxStripHeight);
	if (strip.height <= 0 || placed.isEmpty())
		return strip;

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
			if (entry.top >= bottom || entry.top + entry.height <= top)
				continue;

			layoutSection(&painter, *entry.section, document, logos, entry.top);
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
