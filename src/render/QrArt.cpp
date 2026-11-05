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

QrPlacement placeQr(const QrMatrix &matrix, int maxSize, int quietZone, int x, int y)
{
	QrPlacement placement;
	const int side = qrPlateSize(matrix, maxSize, quietZone);
	placement.plate = QRect(x, y, side, side);

	if (matrix.isEmpty())
		return placement;

	const int quiet = std::max(0, quietZone);
	placement.module = side / (matrix.size + 2 * quiet);

	const int originX = x + quiet * placement.module;
	const int originY = y + quiet * placement.module;

	placement.ink.setFillRule(Qt::WindingFill);
	for (int row = 0; row < matrix.size; ++row) {
		int column = 0;
		while (column < matrix.size) {
			if (!matrix.dark(column, row)) {
				++column;
				continue;
			}

			const int start = column;
			while (column < matrix.size && matrix.dark(column, row))
				++column;

			placement.ink.addRect(originX + start * placement.module, originY + row * placement.module,
					      (column - start) * placement.module, placement.module);
		}
	}

	return placement;
}

} // namespace closingtime
