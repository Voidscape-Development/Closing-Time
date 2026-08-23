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

#include <QHash>
#include <QImage>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

namespace closingtime {

/*
 * Animated logo artwork, decoded whole.
 *
 * Every frame is decoded once, scaled to the size it will be drawn at, and kept. A credit-roll
 * logo is a small picture -- a bug, a sting, a sponsor loop, bounded by LogoRef::maxHeight --
 * so the whole of one is a few tens of megabytes at worst and playback becomes an index into an
 * array rather than a decoder running beside the compositor. It is the wrong trade for an hour
 * of footage and the right one for the artwork this actually holds, which is why the caps below
 * are part of the design rather than a safety net bolted on: a file that will not fit inside
 * them is not a logo, and is refused as one.
 */

/* One decoded frame and how long it is held before the next. */
struct LogoFrame {
	QImage image;
	int durationMs = 100;
};

struct LogoAnimation {
	QVector<LogoFrame> frames;
	/* Length of one pass, in milliseconds. */
	int totalMs = 0;
	/* The frames are all this size; the drawn rectangle may scale them. */
	QSize size;
	/*
	 * Set when decoding stopped at a cap rather than at the end of the file, so the designer
	 * can say that what plays is the first part of the artwork rather than the whole of it.
	 */
	bool truncated = false;

	/* One frame is a still picture that happens to have arrived through an animated container. */
	bool isValid() const { return frames.size() > 1 && totalMs > 0; }
};

/*
 * Shared because a decoded animation outlives the render that produced it: the strip carries its
 * frames from the render thread to the graphics thread, and the cache may hand the same artwork
 * to the next rebuild while the last one is still on screen. Const because nothing may edit a
 * decode two threads are reading.
 */
using LogoAnimationPtr = std::shared_ptr<const LogoAnimation>;

/* Longest animation that will be decoded. See the note above. */
constexpr int kMaxLogoFrames = 1800;
constexpr int kMaxLogoDurationMs = 30000;
/*
 * A ceiling on the decoded bytes of a single animation, as a backstop to the two above.
 *
 * Frame count and duration bound a logo-sized picture; they do not bound a large one. A 30-second
 * animation at a `maxHeight` of 720 and a wide aspect is inside both caps and still several
 * gigabytes of frames, which is a way for a setting nobody thought of as dangerous to take the
 * machine down. Decoding stops here and the animation is marked truncated.
 */
constexpr qint64 kMaxLogoAnimationBytes = 256LL * 1024 * 1024;

/*
 * Filename patterns for a logo file dialog, e.g. "*.png *.gif".
 *
 * Patterns rather than a finished filter string, because the words around them -- "Images and
 * animations" -- are translated, and `obs_module_text` needs a module this file is not always
 * compiled into.
 */
QString imageLogoPatterns();

/*
 * A quick answer to "would this file animate?", read from its header rather than its frames.
 *
 * For the designer, which has to decide whether to offer playback settings for a file it has no
 * reason to decode yet.
 */
bool logoPathLooksAnimated(const QString &path);

/*
 * The frame showing `elapsedMs` into an animation.
 *
 * Shared by the source and the designer's preview so that the two cannot disagree about what a
 * speed multiplier or a play-once means -- the preview showing something the roll will not do is
 * the failure mode this whole path exists to avoid. Past the end, a looping animation wraps and a
 * play-once one holds its last frame.
 */
int logoFrameAt(const LogoAnimation &animation, double elapsedMs, bool loop);

/*
 * Decoded animations, keyed by "path|maxHeight" exactly as LogoCache keys its stills.
 *
 * A null pointer is a cached answer too: it means "this file is a still picture", which is the
 * common case and the one worth not asking the disk about twice.
 *
 * Deliberately not shared between the source and a designer window, for the same reason LogoCache
 * is not: each is touched only from its owner's render job, and render jobs run one at a time.
 */
class AnimatedLogoCache {
public:
	/*
	 * The animation in `path`, scaled so its frames are at most `maxHeight` tall, or null when
	 * the file is a still or cannot be decoded.
	 */
	LogoAnimationPtr get(const QString &path, int maxHeight);

	void clear();

	/* Drops entries whose backing file changed on disk since it was decoded. */
	void invalidate(const QString &path);

private:
	struct CacheEntry {
		LogoAnimationPtr animation;
		qint64 fileSize = 0;
		qint64 modifiedMs = 0;
	};

	QHash<QString, CacheEntry> cache;
};

} // namespace closingtime
