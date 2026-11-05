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

#include "render/QrArt.hpp"

#include <algorithm>

#include <QPointF>

#include "third-party/qrcodegen/qrcodegen.hpp"

namespace closingtime {

namespace {

qrcodegen::QrCode::Ecc eccFor(QrErrorCorrection level)
{
	switch (level) {
	case QrErrorCorrection::Low:
		return qrcodegen::QrCode::Ecc::LOW;
	case QrErrorCorrection::Quartile:
		return qrcodegen::QrCode::Ecc::QUARTILE;
	case QrErrorCorrection::High:
		return qrcodegen::QrCode::Ecc::HIGH;
	case QrErrorCorrection::Medium:
	default:
		return qrcodegen::QrCode::Ecc::MEDIUM;
	}
}

} // namespace

QByteArray qrPayload(const QString &text, QrErrorCorrection level)
{
	QByteArray bytes = text.toUtf8();
	const int capacity = qrCapacityBytes(level);
	if (bytes.size() <= capacity)
		return bytes;

	/*
	 * Back off to the start of a character: a continuation byte is 10xxxxxx, so stepping back
	 * over those from the cut lands on the first byte of the character the cut fell inside.
	 */
	int end = capacity;
	while (end > 0 && (static_cast<unsigned char>(bytes.at(end)) & 0xC0) == 0x80)
		--end;
	bytes.truncate(end);
	return bytes;
}

QrMatrix encodeQr(const QString &text, QrErrorCorrection level)
{
	QrMatrix matrix;
	const QByteArray payload = qrPayload(text, level);
	if (payload.isEmpty())
		return matrix;

	/*
	 * Byte mode always, rather than letting the encoder pick numeric or alphanumeric for text that
	 * fits them. Those are denser, but only for upper-case text -- and the capacity `qrPayload`
	 * cuts to is the byte-mode capacity, which is what guarantees the encoder never refuses.
	 */
	const std::vector<std::uint8_t> data(payload.cbegin(), payload.cend());
	const std::vector<qrcodegen::QrSegment> segments{qrcodegen::QrSegment::makeBytes(data)};
	const qrcodegen::QrCode code = qrcodegen::QrCode::encodeSegments(segments, eccFor(level));

	matrix.size = code.getSize();
	matrix.modules.resize(static_cast<size_t>(matrix.size) * matrix.size);
	for (int y = 0; y < matrix.size; ++y) {
		for (int x = 0; x < matrix.size; ++x)
			matrix.modules[static_cast<size_t>(y) * matrix.size + x] = code.getModule(x, y);
	}
	return matrix;
}

int qrPlateSize(const QrMatrix &matrix, int maxSize, int quietZone)
{
	if (matrix.isEmpty())
		return std::max(1, maxSize);

	const int span = matrix.size + 2 * std::max(0, quietZone);
	/* Never under a pixel a square, which is the one way to keep the code a code. */
	const int module = std::max(1, maxSize / span);
	return module * span;
}

QrErrorCorrection effectiveQrLevel(const Section &section)
{
	return section.qrDesign.hasLogo() ? QrErrorCorrection::High : section.qrErrorCorrection;
}

bool qrInFinder(const QrMatrix &matrix, int x, int y)
{
	const int far = matrix.size - 7;
	return (x < 7 && y < 7) || (x >= far && y < 7) || (x < 7 && y >= far);
}

namespace {

/*
 * A rectangle with each corner rounded by a radius of its own: a connected module is a square
 * rounded only at the corners where its run ends.
 */
QPainterPath cornerPath(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl)
{
	QPainterPath path;
	path.moveTo(r.left() + tl, r.top());
	path.lineTo(r.right() - tr, r.top());
	if (tr > 0)
		path.arcTo(QRectF(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr), 90, -90);
	path.lineTo(r.right(), r.bottom() - br);
	if (br > 0)
		path.arcTo(QRectF(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
	path.lineTo(r.left() + bl, r.bottom());
	if (bl > 0)
		path.arcTo(QRectF(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
	path.lineTo(r.left(), r.top() + tl);
	if (tl > 0)
		path.arcTo(QRectF(r.left(), r.top(), 2 * tl, 2 * tl), 180, -90);
	path.closeSubpath();
	return path;
}

QPainterPath diamondPath(const QRectF &r)
{
	QPainterPath path;
	path.moveTo(r.center().x(), r.top());
	path.lineTo(r.right(), r.center().y());
	path.lineTo(r.center().x(), r.bottom());
	path.lineTo(r.left(), r.center().y());
	path.closeSubpath();
	return path;
}

/*
 * One part of an eye -- its outer edge, the hole in its frame, or its pupil -- in `shape`.
 *
 * Every shape is centered on the finder and spans exactly its width along the lines through its
 * middle, which is where a reader looks for the 1:1:3:1:1 run that says "finder". That is what
 * keeps a round or diamond eye readable: along those lines it is the square it replaces.
 */
QPainterPath eyePart(QrEyeShape shape, const QRectF &r, qreal shrink = 0.0)
{
	/*
	 * `shrink` is how far in from the frame's outer edge this part sits: a frame's hole passes one
	 * square, so its corners are rounded by the outer radius less that square. Rounding each part
	 * by a share of its own size instead thins the ring where it turns, and a thin corner is what
	 * a reader trips on.
	 */
	const qreal side = r.width() + 2 * shrink;
	switch (shape) {
	case QrEyeShape::Rounded: {
		const qreal radius = std::max(0.0, side * 0.28 - shrink);
		QPainterPath path;
		path.addRoundedRect(r, radius, radius);
		return path;
	}
	case QrEyeShape::Circle: {
		QPainterPath path;
		path.addEllipse(r);
		return path;
	}
	case QrEyeShape::Diamond:
		return diamondPath(r);
	case QrEyeShape::Square:
	default: {
		QPainterPath path;
		path.addRect(r);
		return path;
	}
	}
}

} // namespace

QrPlacement placeQr(const QrMatrix &matrix, int maxSize, int quietZone, int x, int y, const QrDesign &design)
{
	QrPlacement placement;
	const int side = qrPlateSize(matrix, maxSize, quietZone);
	placement.plate = QRect(x, y, side, side);

	if (matrix.isEmpty())
		return placement;

	const int quiet = std::max(0, quietZone);
	const int m = side / (matrix.size + 2 * quiet);
	placement.module = m;

	const int originX = x + quiet * m;
	const int originY = y + quiet * m;
	placement.code = QRect(originX, originY, matrix.size * m, matrix.size * m);

	/*
	 * The logo's square, centered, a whole number of pixels, and the squares it clears: every one
	 * it touches, plus a square's margin so the logo stands on clean plate rather than on ragged
	 * edges. Odd-sized, so it sits on the middle square rather than straddling two.
	 */
	QRect cleared;
	if (design.hasLogo()) {
		int span = std::max(1, qRound(matrix.size * std::clamp(design.logoSize, 0.05, 0.3)));
		if (span % 2 == 0)
			++span;
		const int first = (matrix.size - span) / 2;
		placement.logo = QRect(originX + first * m, originY + first * m, span * m, span * m);
		if (design.logoClear)
			cleared = QRect(first - 1, first - 1, span + 2, span + 2);
	}

	const auto dark = [&](int column, int row) {
		if (column < 0 || row < 0 || column >= matrix.size || row >= matrix.size)
			return false;
		if (cleared.contains(column, row))
			return false;
		return matrix.dark(column, row) && !qrInFinder(matrix, column, row);
	};

	const qreal scale = std::clamp(design.moduleScale, 0.5, 1.0);
	placement.ink.setFillRule(Qt::WindingFill);

	for (int row = 0; row < matrix.size; ++row) {
		for (int column = 0; column < matrix.size; ++column) {
			if (!dark(column, row))
				continue;

			const QRectF cell(originX + column * m, originY + row * m, m, m);
			const qreal inset = m * (1.0 - scale) / 2.0;
			const QRectF shrunk = cell.adjusted(inset, inset, -inset, -inset);

			switch (design.moduleShape) {
			case QrModuleShape::Rounded:
				placement.ink.addRoundedRect(shrunk, shrunk.width() * 0.32, shrunk.width() * 0.32);
				break;
			case QrModuleShape::Dots:
				placement.ink.addEllipse(shrunk);
				break;
			case QrModuleShape::Diamond:
				placement.ink.addPath(diamondPath(shrunk));
				break;
			case QrModuleShape::Connected: {
				/*
				 * Full squares, so neighbors run into each other, with a corner rounded only
				 * where both squares beside it are empty: the end of a run, the outside of a
				 * bend. The scale is not applied, since a gap would break the strokes apart.
				 */
				const bool up = dark(column, row - 1);
				const bool down = dark(column, row + 1);
				const bool left = dark(column - 1, row);
				const bool right = dark(column + 1, row);
				const qreal r = m / 2.0;
				placement.ink.addPath(cornerPath(cell, !up && !left ? r : 0, !up && !right ? r : 0,
								 !down && !right ? r : 0, !down && !left ? r : 0));
				break;
			}
			case QrModuleShape::Square:
			default:
				/* Merged along the row, so a run of squares has no edges inside it at all. */
				if (scale >= 1.0) {
					int end = column;
					while (dark(end + 1, row))
						++end;
					placement.ink.addRect(cell.left(), cell.top(), (end - column + 1) * m, m);
					column = end;
				} else {
					placement.ink.addRect(shrunk);
				}
				break;
			}
		}
	}

	/* The three eyes: a frame (outer edge less its hole) and a pupil each. */
	/* A diamond frame does not read reliably (see QrDesign::load); it is drawn square instead. */
	const QrEyeShape frame = design.eyeFrame == QrEyeShape::Diamond ? QrEyeShape::Square : design.eyeFrame;
	placement.eyeFrames.setFillRule(Qt::OddEvenFill);
	const int far = (matrix.size - 7) * m;
	const QPoint eyes[] = {QPoint(0, 0), QPoint(far, 0), QPoint(0, far)};

	for (const QPoint &eye : eyes) {
		const QRectF outer(originX + eye.x(), originY + eye.y(), 7 * m, 7 * m);
		placement.eyeFrames.addPath(eyePart(frame, outer));
		placement.eyeFrames.addPath(eyePart(frame, outer.adjusted(m, m, -m, -m), m));
		placement.eyePupils.addPath(eyePart(design.eyePupil, outer.adjusted(2 * m, 2 * m, -2 * m, -2 * m)));
	}

	return placement;
}

} // namespace closingtime
