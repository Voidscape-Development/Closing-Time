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

#include <QImage>
#include <QRandomGenerator>

#include <algorithm>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/ImageEffects.hpp"
#include "render/RollRenderer.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/*
 * The box blur as it was first written: a copy of the image per pass, a division per channel. Kept
 * here as what the fast one is held to -- a shadow must not move by a single level for having been
 * made cheaper.
 */
void referenceBoxBlur(QImage &image, int radius, bool vertical)
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

void referenceBlur(QImage &image, int radius)
{
	for (int pass = 0; pass < 3; ++pass) {
		referenceBoxBlur(image, radius, false);
		referenceBoxBlur(image, radius, true);
	}
}

/* Premultiplied noise with hard edges and empty stretches, which is what a silhouette is made of. */
QImage noise(int width, int height, quint32 seed)
{
	QRandomGenerator random(seed);
	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	for (int y = 0; y < height; ++y) {
		auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
		for (int x = 0; x < width; ++x) {
			const int kind = random.bounded(4);
			const int alpha = kind == 0 ? 0 : kind == 1 ? 255 : random.bounded(256);
			row[x] = qPremultiply(
				qRgba(random.bounded(256), random.bounded(256), random.bounded(256), alpha));
		}
	}
	return image;
}

} // namespace

CT_SUITE(blur_exact, "The shadow blur gives the pixels it always gave")
{
	const QVector<QSize> sizes = {{1, 1}, {1, 9}, {9, 1}, {2, 2}, {7, 5}, {64, 33}, {301, 47}, {40, 260}};
	const QVector<int> radii = {1, 2, 3, 4, 7, 13, 50, 100};

	quint32 seed = 1;
	for (const QSize &size : sizes) {
		for (int radius : radii) {
			const Context context(
				QStringLiteral("%1x%2 radius %3").arg(size.width()).arg(size.height()).arg(radius));

			const QImage original = noise(size.width(), size.height(), seed++);

			QImage expected = original;
			referenceBlur(expected, radius);
			QImage blurred = original;
			blurImage(blurred, radius);
			check(blurred == expected, "three passes each way, to the last bit");

			for (bool vertical : {false, true}) {
				QImage one = original;
				referenceBoxBlur(one, radius, vertical);
				QImage fast = original;
				boxBlur(fast, radius, vertical);
				check(fast == one, vertical ? "one pass down" : "one pass across");
			}
		}
	}

	/* Every sum a pass can reach divides exactly: full white is the largest there is. */
	QImage white(90, 90, QImage::Format_ARGB32_Premultiplied);
	white.fill(Qt::white);
	QImage blurred = white;
	blurImage(blurred, 100);
	check(blurred == white, "a flat image stays flat at the largest radius");
}

CT_SUITE(logo_cache_generations, "A logo cache lets go of the decodes a roll no longer uses")
{
	LogoCache stills;
	AnimatedLogoCache moving;
	const RollRenderer renderer(&stills, &moving);

	/* What a rebuild does: lay the roll out, and tell the caches a layout has finished. */
	const auto rebuild = [&](const Document &document) {
		const RollLayoutPtr layout = renderer.layout(document);
		stills.endGeneration();
		moving.endGeneration();
		return layout;
	};

	/* A logo's height dragged through forty values, each of them a layout of its own. */
	for (int height = 40; height < 80; ++height) {
		Section heading = unpadded(SectionType::LogoTitle);
		withLogo(heading, height);
		rebuild(documentWith(heading));
	}
	check(stills.size() >= 1, "the still the roll uses is held");
	check(stills.size() <= 2, "and the heights it was dragged through are not");
	check(moving.size() <= 2, "nor the answers that each of them was a still");

	for (int height = 40; height < 80; ++height) {
		Section heading = unpadded(SectionType::LogoTitle);
		withAnimatedLogo(heading, height);
		rebuild(documentWith(heading));
	}
	check(moving.size() >= 1, "the animation the roll uses is held");
	check(moving.size() <= 2, "and the heights it was dragged through are not");

	/* What is in use is never decoded twice, however many layouts go by. */
	Section heading = unpadded(SectionType::LogoTitle);
	withLogo(heading, 96);
	const Document document = documentWith(heading);
	rebuild(document);
	const qint64 decoded = stills.get(testLogoPath(), 96).cacheKey();
	for (int pass = 0; pass < 5; ++pass)
		rebuild(document);
	checkEq(stills.get(testLogoPath(), 96).cacheKey(), decoded, "a logo still in the roll keeps its decode");

	/*
	 * A picture only painting asks for -- a panel's image -- is not asked for by the layout that
	 * follows, and has to outlive it: it is kept through two layouts without a use, and let go of at
	 * the third.
	 */
	const qint64 painted = stills.get(testLogoPath(), 33).cacheKey();
	rebuild(document);
	rebuild(document);
	checkEq(stills.get(testLogoPath(), 33).cacheKey(), painted, "a picture painted two layouts ago is kept");
	for (int pass = 0; pass < 3; ++pass)
		rebuild(document);
	check(stills.get(testLogoPath(), 33).cacheKey() != painted, "and let go of once three have gone by without it");
}

CT_SUITE(roll_truncated, "A roll cut off at the height limit says that it was")
{
	LogoCache stills;
	const RollRenderer renderer(&stills);

	Section title = Section::makeDefault(SectionType::Title);
	title.text = QStringLiteral("The last name in the roll");

	Document document = documentWith(title);
	check(!renderer.layout(document)->truncated, "an ordinary roll is whole");

	/* Lead-in is part of the strip, so it is the cheap way to a roll as tall as the limit. */
	document.leadIn = 199990;
	document.sections.append(title);
	const RollLayoutPtr layout = renderer.layout(document);
	check(layout->truncated, "a roll past the limit is marked as cut");
	check(layout->layers.size() < 2, "and what is past the limit is not in it");
}
