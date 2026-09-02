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

#include "render/ImageEffects.hpp"

#include <QPainter>

#include <algorithm>

namespace closingtime {

void boxBlur(QImage &image, int radius, bool vertical)
{
	const int width = image.width();
	const int height = image.height();
	const int length = vertical ? height : width;
	if (radius < 1 || length < 2)
		return;

	const QImage source = image.copy();
	const int span = radius * 2 + 1;
	const int lines = vertical ? width : height;

	for (int line = 0; line < lines; ++line) {
		const auto sample = [&](int index) -> const uchar * {
			const int clamped = std::clamp(index, 0, length - 1);
			const int x = vertical ? line : clamped;
			const int y = vertical ? clamped : line;
			return source.constScanLine(y) + static_cast<qsizetype>(x) * 4;
		};

		int sum[4] = {0, 0, 0, 0};
		for (int i = -radius; i <= radius; ++i) {
			const uchar *pixel = sample(i);
			for (int channel = 0; channel < 4; ++channel)
				sum[channel] += pixel[channel];
		}

		for (int index = 0; index < length; ++index) {
			const int x = vertical ? line : index;
			const int y = vertical ? index : line;
			uchar *target = image.scanLine(y) + static_cast<qsizetype>(x) * 4;
			for (int channel = 0; channel < 4; ++channel)
				target[channel] = static_cast<uchar>(sum[channel] / span);

			const uchar *leaving = sample(index - radius);
			const uchar *arriving = sample(index + radius + 1);
			for (int channel = 0; channel < 4; ++channel)
				sum[channel] += arriving[channel] - leaving[channel];
		}
	}
}

void blurImage(QImage &image, int radius)
{
	for (int pass = 0; pass < 3; ++pass) {
		boxBlur(image, radius, false);
		boxBlur(image, radius, true);
	}
}

QImage tintedImage(const QImage &image, const QColor &color)
{
	QImage tinted = image;
	if (tinted.isNull())
		return tinted;

	QPainter painter(&tinted);
	/* Keeps the silhouette's alpha and replaces everything under it with the one color. */
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(tinted.rect(), color);
	painter.end();

	return tinted;
}

} // namespace closingtime
