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
 * Lays `text` out into `width` pixels, optionally painting it at (x, y), and returns the
 * height it occupies. Passing a null painter measures without rasterising, which is how
 * the two-pass layout keeps measurement and painting from drifting apart.
 */
int layoutText(QPainter *painter, const QString &text, const TextStyle &style, qreal x, qreal y, qreal width)
{
	if (text.isEmpty() || width <= 0.0)
		return 0;

	const QFont font = makeFont(style);
	const QFontMetricsF metrics(font);

	/* QTextLayout only breaks on explicit line separators, not on plain newlines. */
	QString content = text;
	content.replace(QLatin1Char('\n'), QChar::LineSeparator);

	QTextLayout layout(content, font);
	QTextOption option;
	option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
	/*
	 * Alignment is deliberately left off the text option and applied per line below.
	 * Setting both makes QTextLayout offset the line and then get offset again here,
	 * which pushes centred text a full half-width to the right.
	 */
	layout.setTextOption(option);

	const qreal step = metrics.lineSpacing() * style.lineSpacing;

	qreal cursor = 0.0;
	layout.beginLayout();
	for (;;) {
		QTextLine line = layout.createLine();
		if (!line.isValid())
			break;

		line.setLineWidth(width);
		line.setPosition(QPointF(alignOffset(style.align, width, line.naturalTextWidth()), cursor));
		cursor += step;
	}
	layout.endLayout();

	if (painter) {
		painter->setPen(style.color);
		layout.draw(painter, QPointF(x, y));
	}

	return qCeil(cursor);
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

	int y = top + section.paddingTop;
	const int contentTop = y;

	switch (section.type) {
	case SectionType::Spacer:
		y += section.spacerHeight;
		break;

	case SectionType::Title:
	case SectionType::Header:
		y += layoutText(painter, section.text, section.style, contentX, y, contentWidth);
		break;

	case SectionType::LogoTitle:
	case SectionType::LogoHeader: {
		const QImage image = logos->get(section.logo.path, section.logo.maxHeight);
		const QSize size = logoDrawSize(image, section.logo, contentWidth);
		const int x = contentX + qRound(alignOffset(section.style.align, contentWidth, size.width()));
		paintLogo(painter, image, QRect(QPoint(x, y), size));
		y += size.height();
		break;
	}

	case SectionType::TitleWithLogo:
	case SectionType::HeaderWithLogo: {
		const QImage image = logos->get(section.logo.path, section.logo.maxHeight);
		const int logoBudget = std::max(1, contentWidth / 3);
		const QSize logoSize = logoDrawSize(image, section.logo, logoBudget);

		const int textWidth = std::max(1, contentWidth - logoSize.width() - section.logoGap);
		const int textHeight = layoutText(nullptr, section.text, section.style, 0, 0, textWidth);
		const int rowHeight = std::max(logoSize.height(), textHeight);

		const int logoX = section.logoSide == LogoSide::Left ? contentX
								     : contentX + contentWidth - logoSize.width();
		const int textX = section.logoSide == LogoSide::Left ? contentX + logoSize.width() + section.logoGap
								     : contentX;

		/* The logo and the text are centred against each other within the row. */
		paintLogo(painter, image, QRect(QPoint(logoX, y + (rowHeight - logoSize.height()) / 2), logoSize));
		layoutText(painter, section.text, section.style, textX, y + (rowHeight - textHeight) / 2.0, textWidth);

		y += rowHeight;
		break;
	}

	case SectionType::Bridged: {
		const TextStyle &rightStyle = section.useSecondaryStyle ? section.secondaryStyle : section.style;

		const QFontMetricsF bridgeMetrics(makeFont(section.style));
		const int bridgeWidth = qCeil(bridgeMetrics.horizontalAdvance(section.bridge));
		const int columnWidth = std::max(1, (contentWidth - bridgeWidth) / 2);
		const int rightX = contentX + columnWidth + bridgeWidth;

		for (const Entry &entry : section.entries) {
			const int leftHeight = layoutText(nullptr, entry.text, section.style, 0, 0, columnWidth);
			const int rightHeight = layoutText(nullptr, entry.secondaryText, rightStyle, 0, 0, columnWidth);
			const int rowHeight = std::max({leftHeight, rightHeight, 1});

			layoutText(painter, entry.text, section.style, contentX, y, columnWidth);
			layoutText(painter, entry.secondaryText, rightStyle, rightX, y, columnWidth);

			if (!section.bridge.isEmpty()) {
				TextStyle bridgeStyle = section.style;
				bridgeStyle.align = HAlign::Center;
				layoutText(painter, section.bridge, bridgeStyle, contentX + columnWidth, y,
					   bridgeWidth);
			}

			y += rowHeight + section.entryGap;
		}

		if (!section.entries.isEmpty())
			y -= section.entryGap;
		break;
	}

	case SectionType::TextList: {
		for (const Entry &entry : section.entries) {
			y += layoutText(painter, entry.text, section.style, contentX, y, contentWidth);
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
			const int x = contentX + qRound(alignOffset(section.style.align, contentWidth, size.width()));
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
						x + qRound(alignOffset(section.style.align, columnWidth, size.width()));
					paintLogo(painter, image, QRect(QPoint(logoX, y), size));
					rowHeight = std::max(rowHeight, size.height());
				} else {
					const int height =
						layoutText(painter, entry.text, section.style, x, y, columnWidth);
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
