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

#include <QByteArray>
#include <QPainterPath>
#include <QRect>
#include <QString>

#include <vector>

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * The squares of one QR code, row by row, true where the square is dark.
 *
 * Kept apart from any picture of it: the layout needs only how many squares wide the code is, the
 * paint pass needs where the dark ones are, and neither needs a decoded image -- the code is drawn
 * as filled squares at whatever size the section asks for, which is what keeps it sharp.
 */
struct QrMatrix {
	/* Squares along one side, quiet zone not included. Zero when there is nothing to encode. */
	int size = 0;
	std::vector<bool> modules;

	bool isEmpty() const { return size <= 0; }
	bool dark(int x, int y) const { return modules[static_cast<size_t>(y) * size + x]; }
};

/*
 * The bytes a code at `level` will actually carry for `text`: its UTF-8, cut back at a character
 * boundary to the most the largest code can hold. Cutting is what lets an over-long text still draw
 * a code -- of the part that fits -- rather than nothing, and doing it here rather than leaving the
 * encoder to refuse means the encoder is never asked for something it cannot do.
 */
QByteArray qrPayload(const QString &text, QrErrorCorrection level);

/* Encodes `text` at `level`, picking the smallest code that holds it. Empty for empty text. */
QrMatrix encodeQr(const QString &text, QrErrorCorrection level);

/*
 * Where a code's plate and squares land when it is drawn at most `maxSize` pixels square, with a
 * quiet zone `quietZone` squares deep, its left edge at `x` and its top at `y`.
 *
 * Every square is a whole number of pixels, so its edges fall on pixel boundaries and a reader sees
 * clean squares rather than a blur between them; the plate shrinks to the largest whole multiple
 * that fits, which can leave it a few pixels short of `maxSize`.
 */
struct QrPlacement {
	QRect plate;
	int module = 0;
	/* The dark squares, merged into runs along each row so neighbors share no edge to seam along. */
	QPainterPath ink;
};

/* The side of the square plate a code comes to, without laying it out. */
int qrPlateSize(const QrMatrix &matrix, int maxSize, int quietZone);

QrPlacement placeQr(const QrMatrix &matrix, int maxSize, int quietZone, int x, int y);

} // namespace closingtime
