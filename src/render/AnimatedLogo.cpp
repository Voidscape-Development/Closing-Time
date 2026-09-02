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

#include "render/AnimatedLogo.hpp"

#include <obs.h>

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <cmath>

#include "plugin-support.h"

namespace closingtime {

namespace {

/*
 * A frame delay of zero is not an instruction to draw as fast as the machine can: it is what
 * authoring tools write when they mean "default", and every browser has read it as a tenth of a
 * second for thirty years. Anything under this is treated the same way.
 */
constexpr int kMinHonoredDelayMs = 20;
constexpr int kDefaultDelayMs = 100;

/* Scales `image` down to `maxHeight`, converts it to the format the whole path works in. */
QImage prepareFrame(const QImage &source, int maxHeight, const QSize &fixed)
{
	if (source.isNull())
		return QImage();

	QImage image = source;

	if (fixed.isValid()) {
		/*
		 * Every frame is forced to the first frame's size. A container may hand back frames
		 * of different sizes -- a GIF whose later frames cover only the part that changed,
		 * a video that changes resolution mid-stream -- and the drawn quad is one texture
		 * of one size, so a frame that disagrees is fitted rather than allowed to shift the
		 * logo's box halfway through the roll.
		 */
		if (image.size() != fixed)
			image = image.scaled(fixed, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	} else if (image.height() > maxHeight) {
		image = image.scaledToHeight(maxHeight, Qt::SmoothTransformation);
	}

	/*
	 * Straight alpha, matching the strip tiles: these frames are uploaded as GPU textures for
	 * the source and composited by QPainter for the preview, and OBS composites with straight
	 * alpha. Converting once here is cheaper than correcting per frame in either consumer.
	 */
	return image.convertToFormat(QImage::Format_ARGB32);
}

/* True once the decode has taken all it is allowed to. */
bool atCap(const LogoAnimation &animation, qint64 bytes)
{
	return animation.frames.size() >= kMaxLogoFrames || animation.totalMs >= kMaxLogoDurationMs ||
	       bytes >= kMaxLogoAnimationBytes;
}

qint64 frameBytes(const QImage &image)
{
	return static_cast<qint64>(image.sizeInBytes());
}

/* GIF, APNG, animated WebP -- anything QImageReader will step through. */
LogoAnimationPtr decodeWithQt(const QString &path, int maxHeight)
{
	QImageReader reader(path);
	reader.setAutoTransform(true);

	if (!reader.supportsAnimation())
		return nullptr;

	/*
	 * imageCount() is 1 for a single-frame file in an animated container -- a one-frame GIF, an
	 * ordinary PNG read by the APNG-capable handler -- which is a still and belongs on the
	 * static path. It can also be 0 when the handler does not know the count up front, and that
	 * is not an answer either way, so the frames are counted by reading them.
	 */
	if (reader.imageCount() == 1)
		return nullptr;

	auto animation = std::make_shared<LogoAnimation>();
	qint64 bytes = 0;

	while (reader.canRead()) {
		const int delay = reader.nextImageDelay();
		const QImage raw = reader.read();
		if (raw.isNull())
			break;

		const QImage frame = prepareFrame(raw, maxHeight, animation->size);
		if (frame.isNull())
			break;

		if (!animation->size.isValid())
			animation->size = frame.size();

		const int durationMs = delay >= kMinHonoredDelayMs ? delay : kDefaultDelayMs;
		animation->frames.append(LogoFrame{frame, durationMs});
		animation->totalMs += durationMs;
		bytes += frameBytes(frame);

		if (atCap(*animation, bytes)) {
			animation->truncated = reader.canRead();
			break;
		}
	}

	if (!animation->isValid())
		return nullptr;

	if (animation->truncated)
		obs_log(LOG_WARNING, "animated logo '%s' is longer than the %d-second limit; playing the first part",
			path.toUtf8().constData(), kMaxLogoDurationMs / 1000);

	return animation;
}

} // namespace

QString imageLogoPatterns()
{
	return QStringLiteral("*.png *.apng *.jpg *.jpeg *.gif *.webp *.bmp *.svg");
}

bool logoPathLooksAnimated(const QString &path)
{
	if (path.isEmpty())
		return false;

	QImageReader reader(path);
	if (!reader.supportsAnimation())
		return false;

	/* Exactly one image is a still that happens to be in a container that could hold more. */
	return reader.imageCount() != 1;
}

int logoFrameAt(const LogoAnimation &animation, double elapsedMs, bool loop)
{
	const int count = animation.frames.size();
	if (count <= 0 || animation.totalMs <= 0)
		return 0;

	double position = std::max(0.0, elapsedMs);

	if (position >= animation.totalMs) {
		if (!loop)
			return count - 1;

		position = std::fmod(position, static_cast<double>(animation.totalMs));
	}

	/*
	 * Walked rather than divided, because frames do not share a duration: a GIF that holds its
	 * first frame for a second and then runs twelve in a tenth of one is ordinary, and dividing
	 * by an average would show the wrong frame for most of it.
	 */
	double consumed = 0.0;
	for (int index = 0; index < count; ++index) {
		consumed += animation.frames.at(index).durationMs;
		if (position < consumed)
			return index;
	}

	return count - 1;
}

LogoAnimationPtr AnimatedLogoCache::get(const QString &path, int maxHeight)
{
	if (path.isEmpty())
		return nullptr;

	const int height = std::max(1, maxHeight);
	const QString key = QStringLiteral("%1|%2").arg(path).arg(height);

	const QFileInfo info(path);
	const qint64 fileSize = info.size();
	const qint64 modifiedMs = info.lastModified().toMSecsSinceEpoch();

	const auto it = cache.constFind(key);
	if (it != cache.constEnd() && it->fileSize == fileSize && it->modifiedMs == modifiedMs)
		return it->animation;

	/*
	 * Anything QImageReader cannot step through -- a still, a video, a file that is not a
	 * picture at all -- comes back as no animation, and the still path reports what it made of
	 * it. Only what Qt reads animates here.
	 */
	const LogoAnimationPtr animation = decodeWithQt(path, height);

	cache.insert(key, CacheEntry{animation, fileSize, modifiedMs});
	return animation;
}

void AnimatedLogoCache::clear()
{
	cache.clear();
}

void AnimatedLogoCache::invalidate(const QString &path)
{
	const QString prefix = path + QLatin1Char('|');
	for (auto it = cache.begin(); it != cache.end();) {
		if (it.key().startsWith(prefix))
			it = cache.erase(it);
		else
			++it;
	}
}

} // namespace closingtime
