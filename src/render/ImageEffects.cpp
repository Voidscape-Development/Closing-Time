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
#include <cstring>
#include <vector>

namespace closingtime {

namespace {

/*
 * A box blur divides every sum by the same span, and a division per channel per pixel is most of
 * what a blur costs. So it is done as a multiplication by the span's reciprocal instead: for any
 * span up to kMaxBlurRadius either side, the product shifted down by 32 bits is exactly the quotient
 * rounded down -- the same number the division gave, for every sum there can be.
 */
constexpr int kReciprocalShift = 32;
constexpr int kMaxBlurRadius = 2000;

quint64 reciprocalOf(int span)
{
	return ((quint64(1) << kReciprocalShift) + span - 1) / span;
}

inline uchar divided(int sum, quint64 reciprocal)
{
	return static_cast<uchar>((static_cast<quint64>(sum) * reciprocal) >> kReciprocalShift);
}

/* One pass along the rows, from `source` into `target`: two buffers of the same shape. */
void blurRows(const uchar *source, uchar *target, int width, int height, qsizetype stride, int radius)
{
	const int span = radius * 2 + 1;
	const quint64 reciprocal = reciprocalOf(span);

	for (int y = 0; y < height; ++y) {
		const uchar *in = source + y * stride;
		uchar *out = target + y * stride;

		/* Past either end the edge pixel repeats, which for a shadow is the transparent margin. */
		int sum[4] = {0, 0, 0, 0};
		for (int i = -radius; i <= radius; ++i) {
			const uchar *pixel = in + static_cast<qsizetype>(std::clamp(i, 0, width - 1)) * 4;
			for (int channel = 0; channel < 4; ++channel)
				sum[channel] += pixel[channel];
		}

		for (int x = 0; x < width; ++x) {
			for (int channel = 0; channel < 4; ++channel)
				out[x * 4 + channel] = divided(sum[channel], reciprocal);

			const uchar *leaving = in + static_cast<qsizetype>(std::clamp(x - radius, 0, width - 1)) * 4;
			const uchar *arriving =
				in + static_cast<qsizetype>(std::clamp(x + radius + 1, 0, width - 1)) * 4;
			for (int channel = 0; channel < 4; ++channel)
				sum[channel] += arriving[channel] - leaving[channel];
		}
	}
}

/*
 * One pass down the columns. Walked a row at a time with a running sum per column, rather than a
 * column at a time: the arithmetic is the same and the memory is read in the order it is laid out,
 * which on a wide shadow is the difference between a pass that is a stream and one that misses the
 * cache on every pixel.
 */
void blurColumns(const uchar *source, uchar *target, int width, int height, qsizetype stride, int radius,
		 std::vector<int> &sums)
{
	const int span = radius * 2 + 1;
	const quint64 reciprocal = reciprocalOf(span);
	const int samples = width * 4;

	const auto row = [&](int y) {
		return source + std::clamp(y, 0, height - 1) * stride;
	};

	sums.assign(static_cast<size_t>(samples), 0);
	for (int i = -radius; i <= radius; ++i) {
		const uchar *in = row(i);
		for (int sample = 0; sample < samples; ++sample)
			sums[sample] += in[sample];
	}

	for (int y = 0; y < height; ++y) {
		uchar *out = target + y * stride;
		for (int sample = 0; sample < samples; ++sample)
			out[sample] = divided(sums[sample], reciprocal);

		const uchar *leaving = row(y - radius);
		const uchar *arriving = row(y + radius + 1);
		for (int sample = 0; sample < samples; ++sample)
			sums[sample] += arriving[sample] - leaving[sample];
	}
}

/*
 * `passes` box blurs each way, across and then down, in place.
 *
 * The passes go back and forth between the image and one scratch buffer rather than each pass
 * copying the image to read from, which is what softening a shadow used to spend most of its time
 * doing. A pass along an axis shorter than two pixels changes nothing and is skipped.
 */
void blurPasses(QImage &image, int radius, int passes, bool across, bool down)
{
	const int width = image.width();
	const int height = image.height();
	if (radius < 1 || width < 1 || height < 1 || image.depth() != 32)
		return;
	/* Far past anything a shadow asks for (callers stop at 100), and as far as the reciprocal is exact. */
	radius = std::min(radius, kMaxBlurRadius);

	across = across && width >= 2;
	down = down && height >= 2;
	if (!across && !down)
		return;

	const qsizetype stride = image.bytesPerLine();
	uchar *pixels = image.bits();
	std::vector<uchar> scratch(static_cast<size_t>(stride) * height);
	std::vector<int> sums;

	uchar *current = pixels;
	uchar *other = scratch.data();
	for (int pass = 0; pass < passes; ++pass) {
		if (across) {
			blurRows(current, other, width, height, stride, radius);
			std::swap(current, other);
		}
		if (down) {
			blurColumns(current, other, width, height, stride, radius, sums);
			std::swap(current, other);
		}
	}

	if (current != pixels)
		std::memcpy(pixels, current, scratch.size());
}

} // namespace

void boxBlur(QImage &image, int radius, bool vertical)
{
	blurPasses(image, radius, 1, !vertical, vertical);
}

void blurImage(QImage &image, int radius)
{
	blurPasses(image, radius, 3, true, true);
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
