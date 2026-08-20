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

#ifdef CLOSING_TIME_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace closingtime {

namespace {

/*
 * A frame delay of zero is not an instruction to draw as fast as the machine can: it is what
 * authoring tools write when they mean "default", and every browser has read it as a tenth of a
 * second for thirty years. Anything under this is treated the same way.
 */
constexpr int kMinHonouredDelayMs = 20;
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

		const int durationMs = delay >= kMinHonouredDelayMs ? delay : kDefaultDelayMs;
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

#ifdef CLOSING_TIME_HAVE_FFMPEG

/* Frees the decode's FFmpeg objects however the decode leaves off. */
struct VideoDecodeContext {
	AVFormatContext *format = nullptr;
	AVCodecContext *codec = nullptr;
	SwsContext *scaler = nullptr;
	AVFrame *frame = nullptr;
	AVPacket *packet = nullptr;

	~VideoDecodeContext()
	{
		if (scaler)
			sws_freeContext(scaler);
		if (frame)
			av_frame_free(&frame);
		if (packet)
			av_packet_free(&packet);
		if (codec)
			avcodec_free_context(&codec);
		if (format)
			avformat_close_input(&format);
	}
};

/*
 * The size a video's frames are decoded to: its own, scaled down to `maxHeight`.
 *
 * Scaling in the decoder rather than at draw time is the whole reason a 1080p sting costs what a
 * 96-pixel logo costs. Sample aspect is applied here too, so anamorphic footage is not left to be
 * un-squeezed by a quad that knows nothing about it.
 */
QSize videoFrameSize(const AVCodecContext *codec, const AVStream *stream, int maxHeight)
{
	double width = codec->width;
	const double height = codec->height;
	if (width <= 0.0 || height <= 0.0)
		return QSize();

	AVRational sar = codec->sample_aspect_ratio;
	if (stream->sample_aspect_ratio.num > 0)
		sar = stream->sample_aspect_ratio;
	if (sar.num > 0 && sar.den > 0)
		width = width * sar.num / sar.den;

	QSize size(qRound(width), qRound(height));
	if (size.height() > maxHeight)
		size.scale(QSize(std::max(1, qRound(width)), maxHeight), Qt::KeepAspectRatio);

	return QSize(std::max(1, size.width()), std::max(1, size.height()));
}

/* Converts one decoded frame into the QImage the rest of the plugin works in. */
QImage scaleFrame(VideoDecodeContext &context, const AVFrame *frame, const QSize &size)
{
	context.scaler = sws_getCachedContext(context.scaler, frame->width, frame->height,
					      static_cast<AVPixelFormat>(frame->format), size.width(), size.height(),
					      AV_PIX_FMT_RGB32, SWS_BILINEAR, nullptr, nullptr, nullptr);
	if (!context.scaler)
		return QImage();

	QImage image(size, QImage::Format_ARGB32);
	if (image.isNull())
		return QImage();

	uint8_t *planes[4] = {image.bits(), nullptr, nullptr, nullptr};
	int strides[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};

	sws_scale(context.scaler, frame->data, frame->linesize, 0, frame->height, planes, strides);
	return image;
}

/*
 * How long a frame is held, in milliseconds.
 *
 * Taken from the frame's own duration where the container carries one, and from the stream's
 * frame rate where it does not. Both can be absent or nonsense in a file that has been through
 * enough tools, so the fallback is the same default a frame with no delay gets.
 */
int videoFrameDuration(const AVStream *stream, const AVFrame *frame)
{
	if (frame->duration > 0) {
		const double seconds = frame->duration * av_q2d(stream->time_base);
		if (seconds > 0.0)
			return std::max(1, qRound(seconds * 1000.0));
	}

	const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
	if (rate.num > 0 && rate.den > 0)
		return std::max(1, qRound(1000.0 * rate.den / rate.num));

	return kDefaultDelayMs;
}

LogoAnimationPtr decodeWithFfmpeg(const QString &path, int maxHeight)
{
	VideoDecodeContext context;

	const QByteArray utf8 = path.toUtf8();
	if (avformat_open_input(&context.format, utf8.constData(), nullptr, nullptr) < 0) {
		obs_log(LOG_WARNING, "could not open video logo '%s'", utf8.constData());
		return nullptr;
	}

	if (avformat_find_stream_info(context.format, nullptr) < 0) {
		obs_log(LOG_WARNING, "no stream information in video logo '%s'", utf8.constData());
		return nullptr;
	}

	const AVCodec *decoder = nullptr;
	const int streamIndex = av_find_best_stream(context.format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (streamIndex < 0 || !decoder) {
		obs_log(LOG_WARNING, "no video stream in logo '%s'", utf8.constData());
		return nullptr;
	}

	AVStream *stream = context.format->streams[streamIndex];

	context.codec = avcodec_alloc_context3(decoder);
	if (!context.codec || avcodec_parameters_to_context(context.codec, stream->codecpar) < 0)
		return nullptr;

	/*
	 * Decoding runs on the render thread, which is already off the compositor's path, but a
	 * 30-second file is thousands of frames and a single-threaded decode of it is a visible
	 * stall in the designer. The decoder picks its own thread count.
	 */
	context.codec->thread_count = 0;

	if (avcodec_open2(context.codec, decoder, nullptr) < 0) {
		obs_log(LOG_WARNING, "could not open a decoder for video logo '%s'", utf8.constData());
		return nullptr;
	}

	context.frame = av_frame_alloc();
	context.packet = av_packet_alloc();
	if (!context.frame || !context.packet)
		return nullptr;

	const QSize size = videoFrameSize(context.codec, stream, maxHeight);
	if (!size.isValid())
		return nullptr;

	auto animation = std::make_shared<LogoAnimation>();
	animation->size = size;
	qint64 bytes = 0;
	bool capped = false;

	/*
	 * Frames are taken in decode order and kept in that order, which is presentation order for
	 * every container that reorders -- av_read_frame hands packets over in stream order and the
	 * decoder emits frames in presentation order. Nothing here seeks, so there is no B-frame
	 * ordering left to undo.
	 */
	const auto drain = [&] {
		while (!capped) {
			const int result = avcodec_receive_frame(context.codec, context.frame);
			if (result < 0)
				return;

			const QImage image = scaleFrame(context, context.frame, size);
			/* Read while the frame still holds it: unref resets every field on it. */
			const int durationMs = videoFrameDuration(stream, context.frame);
			av_frame_unref(context.frame);

			if (image.isNull())
				return;

			animation->frames.append(LogoFrame{image, durationMs});
			animation->totalMs += durationMs;
			bytes += frameBytes(image);

			if (atCap(*animation, bytes)) {
				capped = true;
				animation->truncated = true;
				return;
			}
		}
	};

	while (!capped && av_read_frame(context.format, context.packet) >= 0) {
		if (context.packet->stream_index == streamIndex) {
			if (avcodec_send_packet(context.codec, context.packet) >= 0)
				drain();
		}
		av_packet_unref(context.packet);
	}

	if (!capped) {
		/* Flush what the decoder is still holding, or a short file loses its tail. */
		avcodec_send_packet(context.codec, nullptr);
		drain();
	}

	if (!animation->isValid()) {
		obs_log(LOG_WARNING, "video logo '%s' decoded to nothing playable", utf8.constData());
		return nullptr;
	}

	if (animation->truncated)
		obs_log(LOG_WARNING, "video logo '%s' is longer than the %d-second limit; playing the first part",
			utf8.constData(), kMaxLogoDurationMs / 1000);

	return animation;
}

#endif /* CLOSING_TIME_HAVE_FFMPEG */

} // namespace

bool animatedLogosSupportVideo()
{
#ifdef CLOSING_TIME_HAVE_FFMPEG
	return true;
#else
	return false;
#endif
}

const QStringList &videoLogoExtensions()
{
	static const QStringList extensions = {
		QStringLiteral("webm"), QStringLiteral("mp4"), QStringLiteral("m4v"),
		QStringLiteral("mov"),  QStringLiteral("mkv"), QStringLiteral("avi"),
		QStringLiteral("wmv"),  QStringLiteral("mpg"), QStringLiteral("mpeg"),
	};
	return extensions;
}

bool isVideoLogoPath(const QString &path)
{
	return videoLogoExtensions().contains(QFileInfo(path).suffix().toLower());
}

QString imageLogoPatterns()
{
	return QStringLiteral("*.png *.apng *.jpg *.jpeg *.gif *.webp *.bmp *.svg");
}

QString videoLogoPatterns()
{
	if (!animatedLogosSupportVideo())
		return QString();

	QStringList patterns;
	for (const QString &extension : videoLogoExtensions())
		patterns.append(QStringLiteral("*.") + extension);

	return patterns.join(QLatin1Char(' '));
}

bool logoPathLooksAnimated(const QString &path)
{
	if (path.isEmpty())
		return false;

	/*
	 * A video is known by its name rather than by its contents, so the file has to be there for
	 * the answer to mean anything -- otherwise a mistyped path would offer playback settings for
	 * artwork that does not exist. Images answer through QImageReader, which fails on its own.
	 */
	if (isVideoLogoPath(path))
		return animatedLogosSupportVideo() && QFileInfo::exists(path);

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

	LogoAnimationPtr animation;

	if (isVideoLogoPath(path)) {
#ifdef CLOSING_TIME_HAVE_FFMPEG
		animation = decodeWithFfmpeg(path, height);
#else
		/*
		 * Logged rather than silently ignored: the file was picked deliberately, and a build
		 * without FFmpeg drawing a placeholder box with no explanation is the kind of thing
		 * that gets reported as the logo being broken.
		 */
		obs_log(LOG_WARNING, "video logo '%s' needs a build with FFmpeg; drawing nothing",
			path.toUtf8().constData());
#endif
	} else {
		animation = decodeWithQt(path, height);
	}

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
