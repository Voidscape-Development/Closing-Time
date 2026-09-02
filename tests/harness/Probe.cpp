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

#include "harness/Probe.hpp"

#include "harness/Harness.hpp"

#include <QDir>
#include <QHash>
#include <QPainter>

#include <algorithm>

namespace closingtime::test {

namespace {

/*
 * One cache for the whole run. The renderer takes it by pointer and never shares it across
 * threads; here everything is one thread, and reusing it means a sweep of several thousand
 * documents decodes each logo once rather than once per document.
 */
LogoCache &logoCache()
{
	static LogoCache cache;
	return cache;
}

AnimatedLogoCache &animationCache()
{
	static AnimatedLogoCache cache;
	return cache;
}

} // namespace

int measure(const Document &document)
{
	StripRenderer renderer(&logoCache());
	return renderer.measure(document);
}

LayoutBoxes layout(const Document &document)
{
	StripRenderer renderer(&logoCache());
	LayoutBoxes boxes;
	renderer.render(document, &boxes);
	return boxes;
}

QVector<QRectF> boxesOf(const Document &document, LayoutBox::Kind kind)
{
	QVector<QRectF> result;
	for (const LayoutBox &box : layout(document)) {
		if (box.kind == kind)
			result.append(box.rect);
	}
	return result;
}

QRectF boxOf(const Document &document, LayoutBox::Kind kind, int index)
{
	const QVector<QRectF> all = boxesOf(document, kind);
	return index >= 0 && index < all.size() ? all.at(index) : QRectF();
}

Strip renderStrip(const Document &document)
{
	StripRenderer renderer(&logoCache());
	return renderer.render(document);
}

Strip renderAnimatedStrip(const Document &document)
{
	StripRenderer renderer(&logoCache(), &animationCache());
	return renderer.render(document);
}

QImage renderImage(const Document &document)
{
	return flatten(renderStrip(document));
}

QImage flatten(const Strip &strip)
{
	if (strip.isEmpty())
		return QImage();

	QImage flat(strip.width, strip.height, QImage::Format_ARGB32_Premultiplied);
	flat.fill(Qt::transparent);

	QPainter painter(&flat);
	/* Source, not SourceOver: the tiles partition the strip, so this is assembly and not
	 * compositing, and blending would quietly hide a tile that overlapped its neighbor. */
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	for (const StripTile &tile : strip.tiles)
		painter.drawImage(0, tile.top, tile.image);

	return flat;
}

Ink inkOf(const QImage &image)
{
	return inkOf(image, image.rect());
}

Ink inkOf(const QImage &image, const QRect &within)
{
	const QRect area = within.intersected(image.rect());

	Ink ink;
	ink.left = area.right() + 1;
	ink.right = area.left() - 1;
	ink.top = area.bottom() + 1;
	ink.bottom = area.top() - 1;

	if (image.isNull() || area.isEmpty())
		return Ink();

	for (int y = area.top(); y <= area.bottom(); ++y) {
		const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = area.left(); x <= area.right(); ++x) {
			if (qAlpha(row[x]) == 0)
				continue;
			ink.left = std::min(ink.left, x);
			ink.right = std::max(ink.right, x);
			ink.top = std::min(ink.top, y);
			ink.bottom = std::max(ink.bottom, y);
		}
	}

	return ink.isEmpty() ? Ink() : ink;
}

Ink inkOf(const QImage &image, const QRectF &box)
{
	/*
	 * Rounded outward, so a box landing on a fractional pixel still covers the pixel its own
	 * edge is drawn into. Rounding inward would drop the very column a test about an edge is
	 * asking after.
	 */
	return inkOf(image, QRect(QPoint(qFloor(box.left()), qFloor(box.top())),
				  QPoint(qCeil(box.right()) - 1, qCeil(box.bottom()) - 1)));
}

bool inksColumn(const QImage &image, int x)
{
	if (image.isNull() || x < 0 || x >= image.width())
		return false;

	for (int y = 0; y < image.height(); ++y) {
		if (qAlpha(image.pixel(x, y)) != 0)
			return true;
	}
	return false;
}

int boxesOutsideContent(const Document &document)
{
	QHash<int, QRectF> contentBySection;
	const LayoutBoxes boxes = layout(document);

	for (const LayoutBox &box : boxes) {
		if (box.kind == LayoutBox::Kind::Content)
			contentBySection.insert(box.section, box.rect);
	}

	int escaped = 0;
	for (const LayoutBox &box : boxes) {
		if (box.kind == LayoutBox::Kind::Section || box.kind == LayoutBox::Kind::Content)
			continue;

		const auto it = contentBySection.constFind(box.section);
		if (it == contentBySection.constEnd())
			continue;

		/*
		 * Half a pixel of slack, because a content area is rounded to whole pixels while the
		 * things placed inside it are not, and a box sitting exactly on the edge is inside it.
		 */
		const QRectF &content = *it;
		if (box.rect.left() < content.left() - 0.5 || box.rect.right() > content.right() + 0.5 ||
		    box.rect.top() < content.top() - 0.5 || box.rect.bottom() > content.bottom() + 0.5)
			++escaped;
	}

	return escaped;
}

QString tilingProblem(const Strip &strip)
{
	int expected = 0;
	for (const StripTile &tile : strip.tiles) {
		if (tile.top != expected)
			return QStringLiteral("tile at %1 should start at %2").arg(tile.top).arg(expected);
		if (tile.image.width() != strip.width)
			return QStringLiteral("tile at %1 is %2 wide, strip is %3")
				.arg(tile.top)
				.arg(tile.image.width())
				.arg(strip.width);
		expected += tile.image.height();
	}

	if (expected != strip.height)
		return QStringLiteral("tiles cover %1 px of a %2 px strip").arg(expected).arg(strip.height);

	return QString();
}

void sweep(const Section &base, const QVector<Axis> &axes, const std::function<void(const Section &)> &body)
{
	/*
	 * Walked as an odometer rather than by recursion: the axis count is a runtime value, and an
	 * odometer keeps the whole walk in one frame so a failing combination's label is built from
	 * exactly the positions that produced it.
	 */
	QVector<int> position(axes.size(), 0);
	if (axes.isEmpty())
		return;

	for (const Axis &axis : axes) {
		if (axis.values.isEmpty())
			return;
	}

	for (;;) {
		Section section = base;
		QStringList label;

		for (int i = 0; i < axes.size(); ++i) {
			const auto &value = axes.at(i).values.at(position.at(i));
			value.second(section);
			label.append(QStringLiteral("%1=%2").arg(axes.at(i).name, value.first));
		}

		{
			const Context context(label.join(QStringLiteral(" ")));
			body(section);
		}

		int digit = axes.size() - 1;
		for (; digit >= 0; --digit) {
			if (++position[digit] < axes.at(digit).values.size())
				break;
			position[digit] = 0;
		}
		if (digit < 0)
			return;
	}
}

int sweepSize(const QVector<Axis> &axes)
{
	int total = 1;
	for (const Axis &axis : axes)
		total *= axis.values.size();
	return axes.isEmpty() ? 0 : total;
}

void saveArtifact(const QString &name, const QImage &image)
{
	const QString dir = artifactDir();
	if (dir.isEmpty() || image.isNull())
		return;

	/*
	 * Painted onto a dark ground rather than saved with its own alpha. The strip is transparent
	 * everywhere it has not drawn, and a transparent PNG of white text is a picture of nothing
	 * in most viewers -- which is the opposite of what an artifact is for.
	 */
	QImage sheet(image.size(), QImage::Format_ARGB32_Premultiplied);
	sheet.fill(QColor(18, 18, 24));
	QPainter painter(&sheet);
	painter.drawImage(0, 0, image);
	painter.end();

	sheet.save(QDir(dir).filePath(name + QStringLiteral(".png")));
}

void saveArtifact(const QString &name, const Document &document)
{
	if (!artifactDir().isEmpty())
		saveArtifact(name, renderImage(document));
}

} // namespace closingtime::test
