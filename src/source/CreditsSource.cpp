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

#include "source/CreditsSource.hpp"

#include <graphics/vec4.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <plugin-support.h>

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "model/CreditsModel.hpp"
#include "model/StyleLibrary.hpp"
#include "render/ChunkPipeline.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "render/RollCompositor.hpp"
#include "render/RollRenderer.hpp"
#include "render/RollTimeline.hpp"
#include "source/RollControl.hpp"
#include "ui/DesignerDialog.hpp"

namespace closingtime {

namespace {

/*
 * How often the style library is asked to look at its file, in seconds.
 *
 * The look is a `stat` and it happens on the render thread; this is only how often it is asked
 * for. A second is what makes a preset edited in another OBS window land in the roll while the
 * person editing it is still looking at it, which is the whole reason the poll exists.
 */
constexpr double kLibraryPollSeconds = 1.0;

/*
 * How much rasterized picture a source keeps on the render thread's side, in bytes.
 *
 * What a loop coming back round, a scrub, or a rebuild that changed one section draws from instead
 * of rasterizing again. A screenful at 1920 wide is about eight megabytes, so this is thirty or so
 * screens -- a long roll's worth of the pictures most likely to be wanted again -- and nothing like
 * the whole of a long roll, which is the point.
 */
constexpr qint64 kRasterCacheBytes = 256LL * 1024 * 1024;

/*
 * How far ahead of the frame pictures are asked for, in seconds of travel. Rasterizing a section
 * is a few milliseconds and the render thread runs a source's requests ahead of everything else,
 * so this is a wide margin -- the cost of being wrong in the other direction is a section that
 * arrives on screen a few frames after its own top edge did.
 */
constexpr double kLookaheadSeconds = 4.0;

/*
 * How long a texture may go unwanted before it is let go, in nanoseconds of video time. Long
 * enough that a scrub back and forth over the same few sections does not upload them on every
 * pass; short enough that a ten-minute roll holds only the part of itself near the frame.
 */
constexpr uint64_t kTextureGraceNs = 2'000'000'000;

/*
 * How long a rebuilt roll may wait for its pictures before it is put on screen anyway; see
 * stageLayout. In video time, because a source can be rendered several times a frame.
 */
constexpr uint64_t kMaxStagingNs = 500'000'000;

/* Where the roll is in its lifecycle. */
enum class Phase {
	/* Parked at the start position, waiting to be armed. */
	Idle,
	/* Armed, counting down the configured start delay. */
	Delaying,
	/* Under way: roll time runs, unless paused. What "finished" means is the timeline's to say. */
	Running,
};

/* One chunk of one layer, on the GPU. */
struct ChunkTexture {
	gs_texture_t *texture = nullptr;
	/* The video frame it was last wanted in; see kTextureGraceNs. */
	uint64_t lastWanted = 0;
};

/*
 * One animated logo's textures: one for its frames and one for its shadow, each rewritten when the
 * frame changes rather than one per frame -- a thirty-second animation is thousands of frames and no
 * GPU wants thousands of small textures for one logo.
 */
struct LogoTextures {
	gs_texture_t *frame = nullptr;
	gs_texture_t *shadow = nullptr;
	int frameShown = -1;
	int shadowShown = -1;
};

/* A layout that has arrived from the render thread but is not on screen yet; see stageLayout. */
struct StagedLayout {
	RollLayoutPtr layout;
	RollTimeline timeline;
	/* The video frame it arrived in, for kMaxStagingNs. */
	uint64_t arrived = 0;
};

/*
 * Threading:
 *
 *   - `document` is written only by update(), which libobs defers to the graphics thread for video
 *     sources, and by create() before the source is visible to anyone else. The graphics thread can
 *     therefore read it without a lock.
 *   - Playback state is mutated from hotkey, proc-handler and websocket callbacks on other threads
 *     as well as from video_tick, so it lives behind `stateMutex`.
 *   - A finished layout crosses from the render thread through `pendingLayout` under
 *     `handoffMutex`, and rasterized chunks through the ChunkPipeline. Everything the GPU
 *     holds is only ever touched by the graphics thread.
 */
struct CreditsSourceData {
	obs_source_t *source = nullptr;
	/* Copied into every job posted to the render thread, which upgrades it before touching anything. */
	OBSWeakSource weakSelf;

	Document document;
	/* What the last rebuild was laid out from; see renderKey(). Graphics thread only. */
	QByteArray renderedFrom;

	/*
	 * Used only on the render thread: only jobs read or write them, and jobs run one at a time, so
	 * neither needs a lock. Shared with the picture pipeline, which draws from them too.
	 */
	std::shared_ptr<LogoCache> logos = std::make_shared<LogoCache>();
	std::shared_ptr<AnimatedLogoCache> animations = std::make_shared<AnimatedLogoCache>();
	/* What draws this source's chunks; see planPictures. */
	std::shared_ptr<ChunkPipeline> pictures;
	/* Families already reported, so a rebuild per keystroke does not spam the log. Render thread. */
	QSet<QString> warnedFonts;

	std::mutex handoffMutex;
	RollLayoutPtr pendingLayout;
	/* Asked once per render without the lock; written under it, beside the layout it describes. */
	std::atomic<bool> hasPendingLayout{false};
	/* Set while a rebuild is in flight so a burst of edits collapses into one layout. */
	bool rebuildInFlight = false;
	bool rebuildAgain = false;
	/*
	 * Bumped on every edit worth a rebuild. A layout in flight compares it against the number it was
	 * started for between sections, and abandons an edit that has already been overtaken.
	 */
	std::atomic<uint64_t> generation{0};

	/*
	 * Set when loading brought the document up to date against the library -- a preset renamed
	 * there, or a linked style edited -- and the settings the document came from are now behind
	 * it. The write-back happens on the next tick rather than inside update(), because writing
	 * settings from inside update() is how a source calls itself in a circle.
	 */
	bool settingsNeedWriteBack = false;

	/*
	 * The library as this roll last saw it, and how long it has been since anybody was asked to
	 * look at the file. A serial per source rather than the poll's own answer, because the poll has
	 * exactly one: whichever source's turn came up would consume the change and every other roll on
	 * the machine would go on rendering the style the library no longer holds. Graphics thread.
	 */
	quint64 librarySerial = 0;
	double libraryPollElapsed = 0.0;

	/* Graphics-thread state from here to the playback state. */
	RollLayoutPtr live;
	RollTimeline timeline;
	std::optional<StagedLayout> staged;

	/* Chunks on the GPU, by chunk key -- shared between layouts, which is what makes a rebuild cheap. */
	QHash<quint64, ChunkTexture> textures;
	/* Chunks rasterized and waiting for their upload, by chunk key. */
	struct ReadyChunk {
		QImage image;
		uint64_t lastWanted = 0;
	};
	QHash<quint64, ReadyChunk> ready;
	/* Chunks the renderer could not draw at all, which are not asked for again. */
	QSet<quint64> failed;
	/* The live layout's animated logos, in the layout's order. */
	QVector<LogoTextures> logoTextures;

	/*
	 * The video frame the last large upload was made in. One a frame is the budget, per video frame
	 * rather than per call to video_render: a source in both panes of Studio Mode, a projector and
	 * the multiview is rendered several times a frame.
	 */
	uint64_t lastUploadFrame = UINT64_MAX;
	/* Set once a frame has gone out without a picture it needed, so it is logged once per layout. */
	bool fellBehind = false;
	/* The video frame the source was last drawn in, which is how a plan knows it is being looked at. */
	uint64_t lastRendered = 0;

	/*
	 * The one shader the source draws with, compiled the first time it is needed. Null when the
	 * device would not compile it, which costs the linear blending and the fades and nothing else.
	 */
	gs_effect_t *effect = nullptr;
	bool effectTried = false;

	std::mutex stateMutex;
	Phase phase = Phase::Idle;
	/* Seconds of roll time: zero when the roll starts moving, standing still while it is paused. */
	double rollTime = 0.0;
	double delayRemaining = 0.0;
	bool paused = false;
	/* Set once the roll has finished and said so; reset when it is armed again. */
	bool finished = false;
	bool actionPending = false;
	double actionRemaining = 0.0;
	/*
	 * The live timeline's finish time, copied here whenever the timeline changes so that a thread
	 * other than the graphics thread can report it. Infinite for a loop or a hold that never ends.
	 */
	double duration = RollTimeline::kNever;

	obs_hotkey_id startHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id pauseHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id restartHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id designerHotkey = OBS_INVALID_HOTKEY_ID;
};

/* ------------------------------------------------------------------ timelines */

/*
 * The timeline for a layout, played at the document's current playback settings.
 *
 * The layout carries the document it was laid out from, but scroll speed and looping are playback
 * settings: changing them does not lay the roll out again, so they are taken from the document as
 * it is now rather than as it was then.
 */
RollTimeline timelineFor(const RollLayout &layout, const Document &current)
{
	TimelineInputs inputs = layout.timelineInputs();
	inputs.scrollSpeed = current.scrollSpeed;
	inputs.loop = current.loop;
	return RollTimeline(inputs);
}

/*
 * The frame to draw from a timeline right now.
 *
 * Under manual scroll it is the frame the scroll position implies, which is a share of the full
 * travel rather than a pixel offset or a number of seconds -- so it means the same thing after the
 * content is edited or the scroll speed is changed. Otherwise it is the frame at the roll's current
 * time, which is zero for a roll that is parked or counting down its delay.
 *
 * Graphics thread only.
 */
RollFrame currentFrame(CreditsSourceData *data, const RollTimeline &timeline)
{
	if (data->document.manualScroll) {
		const TimelineInputs &inputs = timeline.inputs();
		const double travel = static_cast<double>(inputs.canvasHeight + inputs.stripHeight);
		return timeline.evaluateScrubbed(travel * std::clamp(data->document.scrollPosition, 0.0, 100.0) /
						 100.0);
	}

	double t = 0.0;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		t = data->rollTime;
	}
	return timeline.evaluate(t);
}

/* ------------------------------------------------------------------ strip rebuilding */

/*
 * Everything the roll is laid out from, as a string two documents can be compared by.
 *
 * Playback settings reach the source through the same update() every content edit does, and
 * scrubbing moves a slider that fires one per frame of the drag. Laying out a long roll on each of
 * those would keep the render thread busy for the whole gesture, so a rebuild is queued only when
 * this changes.
 *
 * Built by blanking the fields that move the finished roll rather than by listing the ones that
 * make it: a field added later counts towards the key by default, which costs an unnecessary
 * rebuild if it turns out to be a playback setting and never a stale one if it is not. Lead-in and
 * lead-out are deliberately not blanked -- they are part of the strip the roll scrolls.
 */
QByteArray renderKey(const Document &document)
{
	Document content = document;

	content.scrollSpeed = 0.0;
	content.loop = false;
	content.startOnShow = false;
	content.startDelay = 0.0;
	content.manualScroll = false;
	content.scrollPosition = 0.0;
	content.endingAction = EndingActionConfig();

	/*
	 * The bundled font files are reduced to their sizes rather than carried: a font runs to
	 * megabytes, and serializing them here would cost more than the rebuild this exists to avoid.
	 * What is left still names every family and every file.
	 */
	for (BundledFont &font : content.bundledFonts)
		font.data = QByteArray::number(font.data.size());

	OBSDataAutoRelease data = obs_data_create();
	content.save(data);
	return QByteArray(obs_data_get_json(data));
}

void runRebuild(CreditsSourceData *data, const Document &document, const QString &sourceName, uint64_t generation);

void queueRebuild(CreditsSourceData *data)
{
	const uint64_t generation = ++data->generation;

	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (data->rebuildInFlight) {
			/* The layout in flight sees the new generation and gives up; this one follows it. */
			data->rebuildAgain = true;
			return;
		}
		data->rebuildInFlight = true;
	}

	/* Copied rather than read back later, so the render thread never touches the source's state. */
	postRenderJob([weak = data->weakSelf, data, document = data->document,
		       sourceName = QString::fromUtf8(obs_source_get_name(data->source)), generation] {
		OBSSourceAutoRelease source = obs_weak_source_get_source(weak);
		if (!source)
			return;
		runRebuild(data, document, sourceName, generation);
	});
}

void runRebuild(CreditsSourceData *data, const Document &document, const QString &sourceName, uint64_t generation)
{
	/*
	 * The roll's own font files, registered before anything is measured against them. Doing it here
	 * rather than at load means it happens on the render thread, which is the one thread these
	 * documents are laid out on.
	 */
	for (const QString &family : installDocumentFonts(document)) {
		obs_log(LOG_INFO, "font '%s' is not installed; '%s' is rendering it from its own bundle",
			family.toUtf8().constData(), sourceName.toUtf8().constData());
	}

	/*
	 * A missing font is not fatal -- Qt substitutes one -- but it silently changes what goes to
	 * air, so each family is called out once per source in the OBS log. A family with a stand-in
	 * recorded for it is called out too, at a lower level: what it renders as is the designer's
	 * choice rather than Qt's, which makes it worth saying but not worth warning about.
	 */
	for (const QString &family : missingFontFamilies(document)) {
		if (data->warnedFonts.contains(family))
			continue;

		data->warnedFonts.insert(family);

		const QString substitute = document.fontSubstitute(family);
		if (substitute.isEmpty())
			obs_log(LOG_WARNING, "font '%s' is not installed; '%s' will render with a substitute",
				family.toUtf8().constData(), sourceName.toUtf8().constData());
		else
			obs_log(LOG_INFO, "font '%s' is not installed; '%s' will render it as '%s'",
				family.toUtf8().constData(), sourceName.toUtf8().constData(),
				substitute.toUtf8().constData());
	}

	const RollRenderer renderer(data->logos.get(), data->animations.get());
	RollLayoutPtr layout =
		renderer.layout(document, false, [data, generation] { return data->generation.load() != generation; });

	/*
	 * An abandoned layout hands nothing over. Either way an edit that arrived meanwhile is left in
	 * `rebuildAgain` for the next tick to queue, rather than queued from here: queueing copies the
	 * document, and the document belongs to the graphics thread.
	 */
	std::lock_guard<std::mutex> lock(data->handoffMutex);
	if (layout) {
		data->pendingLayout = std::move(layout);
		data->hasPendingLayout.store(true, std::memory_order_release);
	}
	data->rebuildInFlight = false;
}

/* ------------------------------------------------------------------ pictures */

/*
 * Whether this video frame's large upload has already been made, and the call that spends it.
 *
 * A chunk that is being drawn this frame is uploaded whatever this says, because what reaches the
 * frame does not get to depend on the spreading. Everything done ahead of need asks first.
 *
 * Graphics thread only.
 */
bool uploadSpent(const CreditsSourceData *data)
{
	return data->lastUploadFrame == obs_get_video_frame_time();
}

void spendUpload(CreditsSourceData *data)
{
	data->lastUploadFrame = obs_get_video_frame_time();
}

/*
 * Graphics thread only; requires an active graphics context.
 *
 * `flags` is GS_DYNAMIC for a texture that will be written again -- an animation's frames go
 * through one texture rather than one apiece -- and nothing at all for a picture that goes up once
 * and is only ever drawn. Asking for a dynamic texture that is never rewritten buys the driver's
 * upload path for nothing.
 */
gs_texture_t *createTexture(const QImage &image, uint32_t flags)
{
	if (image.isNull())
		return nullptr;

	const uint8_t *bits = image.constBits();
	return gs_texture_create(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()), GS_BGRA,
				 1, &bits, flags);
}

/* Takes whatever the render thread has finished off its hands. Graphics thread only. */
void receiveChunks(CreditsSourceData *data)
{
	if (!data->pictures->hasArrived())
		return;

	for (auto &[key, image] : data->pictures->takeArrived()) {
		if (image.isNull())
			data->failed.insert(key);
		else if (!data->textures.contains(key))
			data->ready.insert(key,
					   CreditsSourceData::ReadyChunk{std::move(image), obs_get_video_frame_time()});
	}
}

/*
 * Puts a chunk that has come back from the render thread on the GPU. A chunk wanted ahead of need
 * waits for a frame whose upload has not been spent; one being drawn now goes up regardless.
 *
 * Returns true when the chunk is on the GPU afterwards. Graphics thread only; requires an active
 * graphics context, and must not be called from inside a technique's pass.
 */
bool uploadChunk(CreditsSourceData *data, quint64 key, bool now)
{
	if (data->textures.contains(key))
		return true;

	const auto it = data->ready.find(key);
	if (it == data->ready.end())
		return false;
	if (!now && uploadSpent(data))
		return false;

	gs_texture_t *texture = createTexture(it->image, 0);
	if (!texture) {
		obs_log(LOG_ERROR, "failed to allocate a %dx%d credit roll texture", it->image.width(),
			it->image.height());
		/* Not asked for again: a roll too big for the GPU says so once, not once a frame. */
		data->failed.insert(key);
	}

	data->ready.erase(it);
	spendUpload(data);

	if (!texture)
		return false;

	data->textures.insert(key, ChunkTexture{texture, obs_get_video_frame_time()});
	return true;
}

/* The chunk key a draw refers to, or 0 for a draw that is not of a chunk. */
quint64 keyOf(const RollLayout &layout, const DrawSource &source)
{
	if (source.kind != DrawSource::Kind::Chunk || source.layer < 0 || source.layer >= layout.layers.size())
		return 0;
	return chunkKey(layout.layers.at(source.layer), source.chunk, 1.0);
}

/*
 * Puts on the GPU whatever a draw list uses that has come back -- every one of them when `now` says
 * the list is about to be drawn, one at most otherwise -- and returns true when every chunk the list
 * uses is on the GPU. Asking for what is missing is planPictures' job.
 *
 * Graphics thread only; requires an active graphics context.
 */
bool residentFor(CreditsSourceData *data, const RollLayout &layout, const DrawList &list, bool now)
{
	const uint64_t frameTime = obs_get_video_frame_time();
	bool resident = true;

	for (const DrawSource &chunk : chunksIn(list)) {
		const quint64 key = keyOf(layout, chunk);
		if (!key)
			continue;

		if (uploadChunk(data, key, now))
			data->textures[key].lastWanted = frameTime;
		else
			resident = false;
	}

	return resident;
}

/*
 * Works out which chunks this source wants and does not have, most urgent first, and hands the list
 * to the render thread.
 *
 * In order: what is on the canvas now, for the layout on screen and any layout waiting to replace
 * it; the strip from a little above the frame to several seconds of travel below it, each chunk as
 * urgent as it is near; the frame as it will be one, two and four seconds from now, which is what
 * catches a card about to come up or a block about to be let go; and, for a loop nearing the end of
 * its pass, the start of the next one.
 *
 * Only what is on the canvas now is asked for while nothing is looking at the source -- enough that
 * the frame it is shown in has its pictures, and no more than that drawn for a scene nobody is in.
 *
 * Graphics thread only. Draws nothing and touches no texture beyond marking it wanted, so it is safe
 * from video_tick.
 */
void planPictures(CreditsSourceData *data)
{
	if (!data->live && !data->staged)
		return;

	const uint64_t frameTime = obs_get_video_frame_time();
	const int canvasHeight = std::max(1, data->document.height);
	const double speed = std::max(1.0, data->document.scrollSpeed);
	const bool looking = obs_source_showing(data->source) ||
			     (data->lastRendered != 0 && frameTime - data->lastRendered < kTextureGraceNs);

	std::vector<ChunkRequest> wanted;
	QSet<quint64> seen;

	/* Lists are considered most urgent first, so the first to name a chunk decides how urgent it is. */
	const auto consider = [&](const RollLayoutPtr &layout, const DrawList &list, double urgency, bool byPosition) {
		for (const DrawItem &item : list) {
			const quint64 key = keyOf(*layout, item.source);
			if (!key || seen.contains(key))
				continue;
			seen.insert(key);

			if (auto it = data->textures.find(key); it != data->textures.end()) {
				it->lastWanted = frameTime;
				continue;
			}
			if (auto it = data->ready.find(key); it != data->ready.end()) {
				it->lastWanted = frameTime;
				continue;
			}
			if (data->failed.contains(key))
				continue;

			double soon = urgency;
			if (byPosition)
				soon = item.target.top() > canvasHeight ? (item.target.top() - canvasHeight) / speed
									: 0.0;

			wanted.push_back(ChunkRequest{key, layout, item.source.layer, item.source.chunk, 1.0, soon});
		}
	};

	if (data->staged) {
		const RollFrame frame = currentFrame(data, data->staged->timeline);
		consider(data->staged->layout,
			 composeFrame(*data->staged->layout, frame, ComposeView::canvas(frame, canvasHeight)), 0.0,
			 false);
	}

	if (data->live) {
		const RollLayoutPtr &layout = data->live;
		const RollFrame frame = currentFrame(data, data->timeline);
		consider(layout, composeFrame(*layout, frame, ComposeView::canvas(frame, canvasHeight)), 0.0, false);

		if (looking) {
			ComposeView ahead = ComposeView::canvas(frame, canvasHeight);
			ahead.clipTop = -canvasHeight * 0.5;
			ahead.clipBottom = canvasHeight + std::max<double>(canvasHeight, speed * kLookaheadSeconds);
			consider(layout, composeFrame(*layout, frame, ahead), 0.0, true);

			double t = 0.0;
			bool running = false;
			{
				std::lock_guard<std::mutex> lock(data->stateMutex);
				t = data->rollTime;
				running = data->phase != Phase::Idle && !data->paused;
			}

			if (running && !data->document.manualScroll) {
				for (double seconds : {1.0, 2.0, kLookaheadSeconds}) {
					const RollFrame later = data->timeline.evaluate(t + seconds);
					consider(layout,
						 composeFrame(*layout, later, ComposeView::canvas(later, canvasHeight)),
						 seconds, false);
				}

				const double travel = data->timeline.travelTime();
				if (data->document.loop && std::isfinite(travel) && travel > 0.0) {
					const double left = travel - std::fmod(t, travel);
					if (left < kLookaheadSeconds) {
						const RollFrame next = data->timeline.evaluate(t + left);
						ComposeView opening = ComposeView::canvas(next, canvasHeight);
						opening.clipBottom =
							canvasHeight + std::max<double>(canvasHeight, speed);
						consider(layout, composeFrame(*layout, next, opening), left, false);
					}
				}
			}
		}
	}

	data->pictures->want(std::move(wanted));
}

/*
 * Puts one chunk that has come back, and is not on the canvas yet, on the GPU -- if this frame's
 * upload has not been spent on something else.
 *
 * Graphics thread only; requires an active graphics context.
 */
void uploadAhead(CreditsSourceData *data)
{
	if (data->ready.isEmpty() || uploadSpent(data))
		return;

	uploadChunk(data, data->ready.constBegin().key(), false);
}

/*
 * Lets go of every texture nobody has wanted for a while, and every picture that came back for a
 * chunk nobody wants any more.
 *
 * Graphics thread only; requires an active graphics context.
 */
void evict(CreditsSourceData *data)
{
	const uint64_t now = obs_get_video_frame_time();

	for (auto it = data->textures.begin(); it != data->textures.end();) {
		if (now - it->lastWanted > kTextureGraceNs) {
			gs_texture_destroy(it->texture);
			it = data->textures.erase(it);
		} else {
			++it;
		}
	}

	/* A picture still waiting for its upload once the roll has moved past it was wanted too late. */
	for (auto it = data->ready.begin(); it != data->ready.end();) {
		if (now - it->lastWanted > kTextureGraceNs)
			it = data->ready.erase(it);
		else
			++it;
	}
}

/* Graphics thread only; requires an active graphics context. */
void releaseLogoTextures(CreditsSourceData *data)
{
	for (LogoTextures &textures : data->logoTextures) {
		if (textures.frame)
			gs_texture_destroy(textures.frame);
		if (textures.shadow)
			gs_texture_destroy(textures.shadow);
	}
	data->logoTextures.clear();
}

/* Graphics thread only; requires an active graphics context. */
void releaseTextures(CreditsSourceData *data)
{
	for (ChunkTexture &chunk : data->textures)
		gs_texture_destroy(chunk.texture);
	data->textures.clear();
	data->ready.clear();
	releaseLogoTextures(data);
}

/*
 * Puts one frame of an animated logo -- or of its shadow -- on the GPU, allocating the texture the
 * first time. The frames of one animation are all the same size -- the decoder guarantees it -- so
 * the texture is allocated once and written over from then on.
 *
 * Graphics thread only; requires an active graphics context.
 */
void uploadLogo(CreditsSourceData *data, const DrawSource &source)
{
	if (source.logo < 0 || source.logo >= data->logoTextures.size())
		return;

	LogoTextures &textures = data->logoTextures[source.logo];
	const bool shadow = source.kind == DrawSource::Kind::LogoShadow;
	gs_texture_t *&texture = shadow ? textures.shadow : textures.frame;
	int &shown = shadow ? textures.shadowShown : textures.frameShown;

	if (shown == source.frame)
		return;

	const QImage image = logoImage(*data->live, source);
	if (image.isNull())
		return;

	if (!texture) {
		texture = createTexture(image, GS_DYNAMIC);
		if (!texture)
			obs_log(LOG_ERROR, "failed to allocate a %dx%d animated logo texture", image.width(),
				image.height());
	} else {
		gs_texture_set_image(texture, image.constBits(), static_cast<uint32_t>(image.bytesPerLine()), false);
	}

	/* Marked as shown even on a failure, so it is not retried once a frame for the whole roll. */
	shown = source.frame;
}

/*
 * Takes a finished layout off the render thread and stages it beside the one on screen.
 *
 * Graphics thread only.
 */
void adoptPendingLayout(CreditsSourceData *data)
{
	/* Asked once per render, answered without the lock on every frame but the one that matters. */
	if (data->hasPendingLayout.load(std::memory_order_acquire)) {
		RollLayoutPtr layout;
		{
			std::lock_guard<std::mutex> lock(data->handoffMutex);
			layout = std::move(data->pendingLayout);
			data->pendingLayout.reset();
			data->hasPendingLayout.store(false, std::memory_order_relaxed);
		}

		if (layout) {
			/* A layout arriving while another is still staged replaces it; that one never showed. */
			StagedLayout staged;
			staged.timeline = timelineFor(*layout, data->document);
			staged.layout = std::move(layout);
			staged.arrived = obs_get_video_frame_time();
			data->staged = std::move(staged);
		}
	}
}

/* Puts the staged layout on screen. Graphics thread only; requires an active graphics context. */
void commitStaged(CreditsSourceData *data)
{
	StagedLayout staged = std::move(*data->staged);
	data->staged.reset();

	/*
	 * The chunk textures stay: they are keyed by what they show, so every section the rebuild did
	 * not change is already on the GPU for the new layout, and the rest age out as nothing wants
	 * them. The logo textures belong to the old layout's list of logos and go with it.
	 */
	releaseLogoTextures(data);

	data->live = std::move(staged.layout);
	data->timeline = std::move(staged.timeline);
	data->logoTextures.resize(data->live->animatedLogos.size());
	data->fellBehind = false;

	std::lock_guard<std::mutex> lock(data->stateMutex);
	data->duration = data->timeline.finishTime();
}

/*
 * Moves a staged layout one frame closer to the screen, and puts it there once it is ready.
 *
 * Ready means every chunk it would show at the roll's current position is on the GPU. Until then it
 * spends frames' uploads on them and the old layout goes on being drawn, so the frame the new one
 * takes over in has nothing to upload. Since chunk textures are keyed by content, a rebuild that
 * changed one section usually finds everything but that section's chunks already there.
 *
 * Graphics thread only; requires an active graphics context, and must not be called from inside a
 * technique's pass.
 */
void stageLayout(CreditsSourceData *data)
{
	if (!data->staged)
		return;

	const StagedLayout &staged = *data->staged;
	const RollFrame frame = currentFrame(data, staged.timeline);
	const DrawList list =
		composeFrame(*staged.layout, frame, ComposeView::canvas(frame, std::max(1, data->document.height)));

	const bool resident = residentFor(data, *staged.layout, list, false);

	/* With nothing on screen there is nothing for the wait to be smoother than. */
	if (resident || !data->live || obs_get_video_frame_time() - staged.arrived >= kMaxStagingNs)
		commitStaged(data);
}

/* ------------------------------------------------------------------ playback control */

/* Callers must hold stateMutex. */
void resetRollLocked(CreditsSourceData *data)
{
	data->phase = Phase::Idle;
	data->rollTime = 0.0;
	data->delayRemaining = 0.0;
	data->paused = false;
	data->finished = false;
	data->actionPending = false;
	data->actionRemaining = 0.0;
}

/* Callers must hold stateMutex. */
void armRollLocked(CreditsSourceData *data, double startDelay)
{
	resetRollLocked(data);
	data->delayRemaining = startDelay;
	data->phase = startDelay > 0.0 ? Phase::Delaying : Phase::Running;
}

void armRoll(CreditsSourceData *data)
{
	const double startDelay = data->document.startDelay;
	std::lock_guard<std::mutex> lock(data->stateMutex);
	armRollLocked(data, startDelay);
}

void resetRoll(CreditsSourceData *data)
{
	std::lock_guard<std::mutex> lock(data->stateMutex);
	resetRollLocked(data);
}

/*
 * How far to advance the roll for one tick, in seconds.
 *
 * `video_tick` reports the wall-clock gap since the last tick, and that gap jitters: a percent or
 * two either side of the frame interval on an idle machine, more whenever anything else on the
 * system takes a moment. Frames are composited on a fixed cadence regardless, so feeding the
 * measured gap straight into the scroll position moves the roll a slightly different distance in
 * each equally-spaced frame. On a page of type that reads as the roll catching -- appearing to
 * stall for an instant and then carry on -- because the eye tracks the text and sees the spacing
 * between successive positions change, not the clock the positions were derived from.
 *
 * A gap close enough to the video's own frame interval to be that jitter is therefore taken as
 * the interval, which lands equal distances on equally-spaced frames and is what makes the
 * movement read as smooth. A gap well outside that band is a real stall -- a dropped frame, a
 * scene collection loading -- and is used as measured, so the roll keeps its timing over anything
 * long enough to be worth keeping it over.
 */
double tickSeconds(float seconds)
{
	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi) || ovi.fps_num == 0 || ovi.fps_den == 0)
		return seconds;

	const double interval = static_cast<double>(ovi.fps_den) / static_cast<double>(ovi.fps_num);
	const bool withinJitter = seconds > interval * 0.5 && seconds < interval * 1.5;
	return withinJitter ? interval : static_cast<double>(seconds);
}

/*
 * Moves the roll on by one tick and does whatever the roll finishing calls for.
 *
 * Where everything *is* is the timeline's business; this only keeps the clock. The roll finishing
 * is the moment roll time passes the timeline's finish -- the strip clearing with nothing holding
 * it open, or a sticky block whose release says so -- and from there the signal goes out and the
 * ending action counts down its delay. That countdown is in real time and runs through a pause, as
 * it always has: pausing a roll that has already ended is not a reason to hold its ending back.
 *
 * Graphics thread only.
 */
void advance(CreditsSourceData *data, double seconds)
{
	const Document &document = data->document;
	const double finishTime = data->timeline.finishTime();

	bool finishedNow = false;
	bool fireAction = false;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);

		if (data->phase == Phase::Delaying) {
			data->delayRemaining -= seconds;
			if (data->delayRemaining <= 0.0) {
				data->delayRemaining = 0.0;
				data->phase = Phase::Running;
			}
		} else if (data->phase == Phase::Running && !data->paused) {
			data->rollTime += seconds;
		}

		/* Only a roll that has a layout can be over: before the first one lands there is no roll. */
		if (data->phase == Phase::Running && !data->finished && data->live && data->rollTime >= finishTime) {
			data->finished = true;
			finishedNow = true;

			if (document.endingAction.type != EndingActionType::None) {
				if (document.endingAction.delay <= 0.0) {
					fireAction = true;
				} else {
					data->actionPending = true;
					data->actionRemaining = document.endingAction.delay;
				}
			}
		} else if (data->actionPending) {
			data->actionRemaining -= seconds;
			if (data->actionRemaining <= 0.0) {
				data->actionPending = false;
				data->actionRemaining = 0.0;
				fireAction = true;
			}
		}
	}

	/* Signals and actions run outside the lock so their handlers can call back in. */
	if (finishedNow) {
		emitCreditsFinished(data->source);
		notifyRollFinished(data->source);
	}

	if (fireAction)
		document.endingAction.execute(data->source);
}

/* ------------------------------------------------------------------------- callbacks */

const char *getName(void *)
{
	return obs_module_text("CreditsMarquee");
}

void getDefaults(obs_data_t *settings)
{
	Document::defaults(settings);
}

/*
 * Writes the in-memory document back to the source's settings.
 *
 * For migrations the source performs on its own: a style preset renamed in the library, whose new
 * name this roll's sections have just been re-pointed at. Without this the rename would be redone
 * from the old settings on every load, and would be lost the moment the library forgot it.
 *
 * Graphics thread only.
 */
void writeDocumentBack(CreditsSourceData *data)
{
	OBSDataAutoRelease settings = obs_source_get_settings(data->source);
	if (!settings)
		return;

	data->document.save(settings);
	/* Kept in step first: the update() this schedules must not read as a content change. */
	data->renderedFrom = renderKey(data->document);
	obs_source_update(data->source, settings);
}

void update(void *raw, obs_data_t *settings)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	const double previousSpeed = data->document.scrollSpeed;
	const bool previousLoop = data->document.loop;

	/*
	 * `load` brings the document up to date against the library, which can rename a preset and
	 * every binding that named it. When it does, what is in `settings` is now the old shape of
	 * this document and has to be replaced -- on the next tick, since we are inside update() and
	 * writing settings from in here is how a source calls itself in a circle.
	 */
	bool migrated = false;
	data->document.load(settings, &migrated);
	if (migrated)
		data->settingsNeedWriteBack = true;

	/*
	 * Only a change to what the roll is made of is worth laying out again -- see renderKey().
	 * Everything else here arrives through the same call: a scrub sends one per frame of the drag.
	 */
	const QByteArray key = renderKey(data->document);
	const bool contentChanged = key != data->renderedFrom;
	if (contentChanged) {
		data->renderedFrom = key;
		queueRebuild(data);
	}

	/*
	 * A new speed or a loop switched is a new timeline for the same layout, not a new layout. The
	 * roll's time is carried across so the strip stays where it is -- a roll keeps its place when
	 * its speed is adjusted under it.
	 */
	if (!contentChanged && data->live &&
	    (data->document.scrollSpeed != previousSpeed || data->document.loop != previousLoop)) {
		RollTimeline next = timelineFor(*data->live, data->document);
		{
			std::lock_guard<std::mutex> lock(data->stateMutex);
			data->rollTime = data->timeline.rebase(data->rollTime, next);
			data->duration = next.finishTime();
		}
		data->timeline = std::move(next);
		if (data->staged)
			data->staged->timeline = timelineFor(*data->staged->layout, data->document);
	}

	/*
	 * Content edits invalidate the current position, so a roll that is already running restarts
	 * instead of jumping to a stale time in new content. A playback setting changing is not that,
	 * and dragging the scrub slider does not re-arm the roll once per frame.
	 */
	std::lock_guard<std::mutex> lock(data->stateMutex);
	if (data->phase != Phase::Idle && contentChanged)
		armRollLocked(data, data->document.startDelay);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new CreditsSourceData();
	data->source = source;
	data->pictures = ChunkPipeline::create(data->logos, data->animations, kRasterCacheBytes, RenderPriority::Live);
	{
		OBSWeakSourceAutoRelease weak = obs_source_get_weak_source(source);
		data->weakSelf = weak.Get();
	}

	signal_handler_add(obs_source_get_signal_handler(source), "void credits_finished(ptr source)");

	proc_handler_t *procs = obs_source_get_proc_handler(source);
	proc_handler_add(
		procs, "void restart()",
		[](void *param, calldata_t *) { armRoll(static_cast<CreditsSourceData *>(param)); }, data);
	/* What the Start hotkey does: arm a roll that is parked or over, carry on with one that is paused. */
	proc_handler_add(
		procs, "void start()",
		[](void *param, calldata_t *) {
			auto *self = static_cast<CreditsSourceData *>(param);
			std::lock_guard<std::mutex> lock(self->stateMutex);
			if (self->phase == Phase::Idle || self->finished)
				armRollLocked(self, self->document.startDelay);
			else
				self->paused = false;
		},
		data);
	proc_handler_add(
		procs, "void pause()",
		[](void *param, calldata_t *) {
			auto *self = static_cast<CreditsSourceData *>(param);
			std::lock_guard<std::mutex> lock(self->stateMutex);
			self->paused = true;
		},
		data);
	proc_handler_add(
		procs, "void resume()",
		[](void *param, calldata_t *) {
			auto *self = static_cast<CreditsSourceData *>(param);
			std::lock_guard<std::mutex> lock(self->stateMutex);
			self->paused = false;
		},
		data);
	/* What the websocket vendor's GetState reports; see RollControl. */
	proc_handler_add(
		procs,
		"void get_state(out string phase, out float time, out float duration, out bool finished, out bool paused)",
		[](void *param, calldata_t *call) {
			auto *self = static_cast<CreditsSourceData *>(param);
			std::lock_guard<std::mutex> lock(self->stateMutex);
			const char *phase = self->phase == Phase::Idle       ? "idle"
					    : self->phase == Phase::Delaying ? "delaying"
					    : self->finished                 ? "finished"
									     : "running";
			calldata_set_string(call, "phase", phase);
			calldata_set_float(call, "time", self->rollTime);
			calldata_set_float(call, "duration", self->duration);
			calldata_set_bool(call, "finished", self->finished);
			calldata_set_bool(call, "paused", self->paused);
		},
		data);

	data->startHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Start", obs_module_text("Hotkey.Start"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;

			auto *self = static_cast<CreditsSourceData *>(param);
			std::unique_lock<std::mutex> lock(self->stateMutex);
			if (self->phase == Phase::Idle || self->finished)
				armRollLocked(self, self->document.startDelay);
			else
				self->paused = false;
		},
		data);

	data->pauseHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Pause", obs_module_text("Hotkey.Pause"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;

			auto *self = static_cast<CreditsSourceData *>(param);
			std::lock_guard<std::mutex> lock(self->stateMutex);
			self->paused = !self->paused;
		},
		data);

	data->restartHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Restart", obs_module_text("Hotkey.Restart"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				armRoll(static_cast<CreditsSourceData *>(param));
		},
		data);

	/*
	 * Opening the designer through the properties window means opening, and then closing, a
	 * window that has nothing to do with what is being edited. This is one of two ways round
	 * that: a hotkey per source, and the Tools menu entry registered below.
	 */
	data->designerHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Designer", obs_module_text("Hotkey.Designer"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				openDesignerForAsync(static_cast<CreditsSourceData *>(param)->source);
		},
		data);

	update(data, settings);

	/*
	 * Where the library stood when this roll was read out of its settings, which the load inside
	 * update() has just brought it up to date against. Anything that moves it from here is a
	 * change this roll has not seen yet -- see the poll in videoTick.
	 */
	data->librarySerial = StyleLibrary::instance().serial();
	return data;
}

void destroy(void *raw)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	obs_hotkey_unregister(data->startHotkey);
	obs_hotkey_unregister(data->pauseHotkey);
	obs_hotkey_unregister(data->restartHotkey);
	obs_hotkey_unregister(data->designerHotkey);

	closeDesignerFor(data->source);

	obs_enter_graphics();
	releaseTextures(data);
	if (data->effect)
		gs_effect_destroy(data->effect);
	obs_leave_graphics();

	delete data;
}

uint32_t getWidth(void *raw)
{
	return static_cast<uint32_t>(std::max(1, static_cast<CreditsSourceData *>(raw)->document.width));
}

uint32_t getHeight(void *raw)
{
	return static_cast<uint32_t>(std::max(1, static_cast<CreditsSourceData *>(raw)->document.height));
}

void onShow(void *raw)
{
	auto *data = static_cast<CreditsSourceData *>(raw);
	if (data->document.startOnShow)
		armRoll(data);
}

void onHide(void *raw)
{
	auto *data = static_cast<CreditsSourceData *>(raw);
	if (data->document.startOnShow)
		resetRoll(data);
}

void videoTick(void *raw, float seconds)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	if (data->settingsNeedWriteBack) {
		data->settingsNeedWriteBack = false;
		writeDocumentBack(data);
	}

	/* An edit that arrived while a layout was in flight; see runRebuild. */
	{
		bool again = false;
		{
			std::lock_guard<std::mutex> lock(data->handoffMutex);
			again = data->rebuildAgain && !data->rebuildInFlight;
			if (again)
				data->rebuildAgain = false;
		}
		if (again)
			queueRebuild(data);
	}

	/*
	 * A style the library changed underneath this roll -- edited in another OBS window, imported,
	 * or hand-edited -- is pulled in here. Before the manual-scroll branch below, because a roll
	 * parked for editing is exactly the one somebody is restyling.
	 *
	 * Asking the file whether it moved is a `stat`, so it is asked for on the render thread and
	 * this tick only compares the library's serial against the one this roll last saw.
	 */
	data->libraryPollElapsed += seconds;
	if (data->libraryPollElapsed >= kLibraryPollSeconds) {
		data->libraryPollElapsed = 0.0;

		/*
		 * One poll in the queue at a time for the whole machine. Every roll asks once a second,
		 * and the render thread can be busy for a while: without this the asks pile up behind it
		 * and are answered back to back, all of them the same answer.
		 */
		static std::atomic<bool> pollQueued{false};
		if (!pollQueued.exchange(true, std::memory_order_acq_rel)) {
			postRenderJob([] {
				pollQueued.store(false, std::memory_order_release);
				StyleLibrary::instance().pollForChanges();
			});
		}
	}

	if (const quint64 serial = StyleLibrary::instance().serial(); serial != data->librarySerial) {
		data->librarySerial = serial;

		/* A rebuild only when a style bound to *this* document actually moved. */
		if (data->document.refreshLinkedPresets()) {
			queueRebuild(data);
			/*
			 * Saved as well as redrawn. The refreshed copy is this roll's fallback on a
			 * machine without the library, and a rename it just followed has to survive a
			 * restart -- both of which mean the settings, not just the pictures.
			 */
			writeDocumentBack(data);
		}
	}

	/*
	 * Nothing moves while the roll is parked under manual scroll: the roll cannot finish and the
	 * ending action cannot fire. The phase is left exactly as playback set it, so switching the
	 * mode back off resumes from a state update() already knows how to re-arm.
	 */
	if (!data->document.manualScroll)
		advance(data, tickSeconds(seconds));

	/* After the clock has moved, so the plan is for the frame about to be drawn. */
	planPictures(data);
}

/* ------------------------------------------------------------------------- drawing */

/*
 * The one shader the source draws with: a premultiplied texture, blended in linear light, at an
 * opacity.
 *
 * The pictures are drawn by QPainter, premultiplied, in sRGB -- the gamma-encoded values a screen
 * shows. The source is sRGB-aware, so OBS composites what it draws in linear light the way it does
 * its own sources, and onto an HDR canvas correctly. So each texel is unpremultiplied, decoded to
 * linear, premultiplied again and scaled by the draw's opacity; the framebuffer encodes it back on
 * the way out. The opacity scales all four channels because the colors are premultiplied: scaling
 * the alpha alone would leave the color at full strength on top of a background the blend no longer
 * takes away, which glows rather than fades.
 *
 * Written out here rather than shipped as a .effect file because it is forty lines and because a
 * data file is one more thing that has to be found at runtime on three platforms.
 */
const char *const kRollEffectSource = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float alpha;

sampler_state textureSampler {
	Filter   = Linear;
	AddressU = Clamp;
	AddressV = Clamp;
};

struct VertData {
	float4 pos : POSITION;
	float2 uv  : TEXCOORD0;
};

VertData VSDefault(VertData vert_in)
{
	VertData vert_out;
	vert_out.pos = mul(float4(vert_in.pos.xyz, 1.0), ViewProj);
	vert_out.uv  = vert_in.uv;
	return vert_out;
}

float srgb_to_linear_channel(float u)
{
	return (u <= 0.04045) ? (u / 12.92) : pow((u + 0.055) / 1.055, 2.4);
}

float4 PSDraw(VertData vert_in) : TARGET
{
	float4 rgba = image.Sample(textureSampler, vert_in.uv);
	float3 straight = rgba.rgb / max(rgba.a, 0.0001);
	float3 linear_rgb = float3(srgb_to_linear_channel(straight.r), srgb_to_linear_channel(straight.g),
				   srgb_to_linear_channel(straight.b));
	return float4(linear_rgb * rgba.a, rgba.a) * alpha;
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(vert_in);
		pixel_shader  = PSDraw(vert_in);
	}
}
)";

/*
 * The shader, compiled on first use. Returns null when the device would not compile it, and only
 * tries once: a shader that failed to compile will not compile on the next frame either, and a
 * compiler error logged sixty times a second is a log nobody can read.
 *
 * Graphics thread only; requires an active graphics context.
 */
gs_effect_t *rollEffect(CreditsSourceData *data)
{
	if (data->effectTried)
		return data->effect;

	data->effectTried = true;

	char *errors = nullptr;
	data->effect = gs_effect_create(kRollEffectSource, "closing-time-roll.effect", &errors);
	if (!data->effect)
		obs_log(LOG_ERROR, "could not compile the credit roll shader: %s",
			errors ? errors : "no error reported");

	bfree(errors);
	return data->effect;
}

double srgbToLinear(double u)
{
	return u <= 0.04045 ? u / 12.92 : std::pow((u + 0.055) / 1.055, 2.4);
}

void drawBackground(const QColor &color, int width, int height, bool linear)
{
	if (color.alpha() <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	/* The color is chosen in sRGB; drawn into a linear framebuffer, it has to arrive linear. */
	const auto channel = [linear](double value) {
		return static_cast<float>(linear ? srgbToLinear(value) : value);
	};

	struct vec4 value;
	vec4_set(&value, channel(color.redF()), channel(color.greenF()), channel(color.blueF()),
		 static_cast<float>(color.alphaF()));
	gs_effect_set_vec4(colorParam, &value);

	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

/*
 * Draws one textured quad: `target` in canvas pixels, `source` in the texture's own.
 *
 * Built vertex by vertex rather than through gs_draw_sprite_subregion, because the scroll position
 * is fractional and that call is not: it takes whole texels, so a picture cut at the canvas's edge
 * would end short of it by the fraction that was dropped -- a sliver along the bottom that content
 * entering from below appears to pop through rather than slide into. Four vertices carrying the
 * fraction in both the positions and the texture coordinates cost nothing per frame and land the
 * edge exactly.
 */
void drawQuad(gs_texture_t *texture, const QRectF &target, const QRectF &source)
{
	const double width = gs_texture_get_width(texture);
	const double height = gs_texture_get_height(texture);
	if (width <= 0.0 || height <= 0.0 || target.isEmpty())
		return;

	const auto x0 = static_cast<float>(target.left());
	const auto x1 = static_cast<float>(target.right());
	const auto y0 = static_cast<float>(target.top());
	const auto y1 = static_cast<float>(target.bottom());
	const auto u0 = static_cast<float>(source.left() / width);
	const auto u1 = static_cast<float>(source.right() / width);
	const auto v0 = static_cast<float>(source.top() / height);
	const auto v1 = static_cast<float>(source.bottom() / height);

	/* Vertices in the order libobs builds its own sprites for a triangle strip: tl, tr, bl, br. */
	gs_render_start(false);
	gs_texcoord(u0, v0, 0);
	gs_vertex2f(x0, y0);
	gs_texcoord(u1, v0, 0);
	gs_vertex2f(x1, y0);
	gs_texcoord(u0, v1, 0);
	gs_vertex2f(x0, y1);
	gs_texcoord(u1, v1, 0);
	gs_vertex2f(x1, y1);
	gs_render_stop(GS_TRISTRIP);
}

/* The texture a draw takes its pixels from, or null when it is not on the GPU. */
gs_texture_t *textureFor(CreditsSourceData *data, const DrawSource &source)
{
	if (source.kind == DrawSource::Kind::Chunk) {
		const auto it = data->textures.constFind(keyOf(*data->live, source));
		return it == data->textures.constEnd() ? nullptr : it->texture;
	}

	if (source.logo < 0 || source.logo >= data->logoTextures.size())
		return nullptr;

	const LogoTextures &textures = data->logoTextures.at(source.logo);
	if (source.kind == DrawSource::Kind::LogoShadow)
		return textures.shadowShown == source.frame ? textures.shadow : nullptr;
	return textures.frameShown == source.frame ? textures.frame : nullptr;
}

void videoRender(void *raw, gs_effect_t *)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	adoptPendingLayout(data);
	receiveChunks(data);

	/*
	 * Everything this frame draws goes on the GPU before the pass that draws it is started, rather
	 * than being allocated part-way through one: a technique's pass is a poor place to create a
	 * resource, and gs_effect_loop may run its body more than once. A rebuilt roll is staged first,
	 * so that the frame it takes over in draws it rather than the one it replaced.
	 */
	stageLayout(data);

	const int canvasWidth = std::max(1, data->document.width);
	const int canvasHeight = std::max(1, data->document.height);

	gs_effect_t *effect = rollEffect(data);
	/* Without the shader there is no linear decode, so the framebuffer is left as it was. */
	const bool linear = effect != nullptr;
	const bool previousSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);

	drawBackground(data->document.background, canvasWidth, canvasHeight, linear);

	if (!data->live || data->live->isEmpty()) {
		gs_enable_framebuffer_srgb(previousSrgb);
		return;
	}

	const RollFrame frame = currentFrame(data, data->timeline);
	const DrawList list = composeFrame(*data->live, frame, ComposeView::canvas(frame, canvasHeight));

	/*
	 * What is on the canvas now goes up now, whatever the budget says. A picture that is not back
	 * yet is simply not drawn this frame -- which, for a roll that is playing, is a section arriving
	 * a few frames after its own top edge, and worth a line in the log. A roll parked or scrubbed
	 * to somewhere new is expected to wait for its pictures, and says nothing.
	 */
	data->lastRendered = obs_get_video_frame_time();
	const bool resident = residentFor(data, *data->live, list, true);
	/* The plan from this frame's tick may predate a layout committed just now; make sure it is asked for. */
	if (!resident)
		planPictures(data);
	if (!resident && !data->fellBehind) {
		bool playing = false;
		{
			std::lock_guard<std::mutex> lock(data->stateMutex);
			playing = data->phase == Phase::Running && !data->paused && !data->document.manualScroll;
		}
		if (playing) {
			data->fellBehind = true;
			obs_log(LOG_WARNING,
				"'%s' drew a frame before all of its pictures were ready; rasterizing is behind",
				obs_source_get_name(data->source));
		}
	}

	uploadAhead(data);
	evict(data);

	for (const DrawItem &item : list) {
		if (item.source.kind != DrawSource::Kind::Chunk)
			uploadLogo(data, item.source);
	}

	if (!effect)
		effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *alphaParam = gs_effect_get_param_by_name(effect, "alpha");

	/*
	 * Premultiplied, because the pictures are -- see RollRenderer::rasterize. It is also what OBS
	 * itself expects of a source: a straight-alpha blend applied to the alpha channel as well writes
	 * alpha squared into any texture the source is first drawn into -- a filter, a nested scene, a
	 * transition -- and every antialiased edge comes out thinner there.
	 */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw")) {
		for (const DrawItem &item : list) {
			gs_texture_t *texture = textureFor(data, item.source);
			if (!texture)
				continue;

			gs_effect_set_texture(imageParam, texture);
			if (alphaParam)
				gs_effect_set_float(alphaParam, static_cast<float>(item.alpha));
			drawQuad(texture, item.target, item.sourceRect);
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previousSrgb);
}

/* ------------------------------------------------------------------------ properties */

bool onOpenDesigner(obs_properties_t *, obs_property_t *, void *raw)
{
	openDesignerFor(static_cast<CreditsSourceData *>(raw)->source);
	return false;
}

/*
 * The scrub position and its warning are only worth showing while the roll is actually parked;
 * with playback running they describe nothing.
 */
bool onManualScrollChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = obs_data_get_bool(settings, "manual_scroll");

	for (const char *name : {"scroll_position", "manual_scroll_warning"}) {
		if (obs_property_t *property = obs_properties_get(props, name))
			obs_property_set_visible(property, manual);
	}

	return true;
}

/* Shows only the fields the selected ending action actually uses. */
bool onEndingActionChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const EndingActionType type =
		endingActionFromId(obs_data_get_string(settings, "ea_type"), EndingActionType::None);

	const auto setVisible = [props](const char *name, bool visible) {
		if (obs_property_t *property = obs_properties_get(props, name))
			obs_property_set_visible(property, visible);
	};

	setVisible("ea_scene", type == EndingActionType::SwitchScene);
	setVisible("ea_filter_source", type == EndingActionType::SetFilterEnabled);
	setVisible("ea_filter_name", type == EndingActionType::SetFilterEnabled);
	setVisible("ea_filter_mode", type == EndingActionType::SetFilterEnabled);
	setVisible("ea_hotkey", type == EndingActionType::FireHotkey);
	setVisible("ea_delay", type != EndingActionType::None);

	return true;
}

void fillSceneList(obs_property_t *list)
{
	char **names = obs_frontend_get_scene_names();
	if (!names)
		return;

	for (char **name = names; *name; ++name)
		obs_property_list_add_string(list, *name, *name);

	bfree(names);
}

void fillFilterList(obs_property_t *list, const char *sourceName)
{
	if (!sourceName || !*sourceName)
		return;

	OBSSourceAutoRelease target = obs_get_source_by_name(sourceName);
	if (!target)
		return;

	obs_source_enum_filters(
		target,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			if (const char *name = obs_source_get_name(filter))
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
		},
		list);
}

bool onFilterSourceChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *list = obs_properties_get(props, "ea_filter_name");
	if (!list)
		return false;

	obs_property_list_clear(list);
	obs_property_list_add_string(list, "", "");
	fillFilterList(list, obs_data_get_string(settings, "ea_filter_source"));
	return true;
}

void fillHotkeyList(obs_property_t *list)
{
	obs_enum_hotkeys(
		[](void *param, obs_hotkey_id, obs_hotkey_t *hotkey) {
			auto *target = static_cast<obs_property_t *>(param);

			const QString name = QString::fromUtf8(obs_hotkey_get_name(hotkey));
			QString partner;
			QString label = QString::fromUtf8(obs_hotkey_get_description(hotkey));

			if (obs_hotkey_get_registerer_type(hotkey) == OBS_HOTKEY_REGISTERER_SOURCE) {
				auto *weak = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(hotkey));
				OBSSourceAutoRelease owner = obs_weak_source_get_source(weak);
				if (owner) {
					partner = QString::fromUtf8(obs_source_get_name(owner));
					label = QStringLiteral("%1: %2").arg(partner, label);
				}
			}

			const QString value = EndingActionConfig::encodeHotkey(name, partner);
			obs_property_list_add_string(target, label.toUtf8().constData(), value.toUtf8().constData());
			return true;
		},
		list);
}

obs_properties_t *getProperties(void *raw)
{
	auto *data = static_cast<CreditsSourceData *>(raw);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_button2(props, "open_designer", obs_module_text("OpenDesigner"), onOpenDesigner, data);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 16, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 16, 8192, 1);
	obs_properties_add_color_alpha(props, "background", obs_module_text("BackgroundColor"));

	obs_property_t *speed =
		obs_properties_add_float(props, "scroll_speed", obs_module_text("ScrollSpeed"), 1.0, 2000.0, 1.0);
	obs_property_float_set_suffix(speed, " px/s");

	obs_properties_add_int(props, "lead_in", obs_module_text("LeadIn"), 0, 20000, 10);
	obs_properties_add_int(props, "lead_out", obs_module_text("LeadOut"), 0, 20000, 10);

	obs_properties_add_bool(props, "start_on_show", obs_module_text("StartOnShow"));
	obs_properties_add_float(props, "start_delay", obs_module_text("StartDelay"), 0.0, 600.0, 0.1);
	obs_properties_add_bool(props, "loop", obs_module_text("Loop"));

	/*
	 * Scrubbing by hand, for looking at a section in the middle of a long roll without waiting
	 * for the roll to scroll there. The slider is a share of the full travel rather than a pixel
	 * offset, so it keeps its meaning as the content underneath it is edited.
	 */
	obs_property_t *manual = obs_properties_add_bool(props, "manual_scroll", obs_module_text("ManualScroll"));
	obs_property_set_long_description(manual, obs_module_text("ManualScroll.Tip"));
	obs_property_set_modified_callback(manual, onManualScrollChanged);

	obs_property_t *position = obs_properties_add_float_slider(props, "scroll_position",
								   obs_module_text("ScrollPosition"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(position, " %");
	obs_property_set_long_description(position, obs_module_text("ScrollPosition.Tip"));

	/*
	 * The setting saves with the scene collection like every other one here, so it is perfectly
	 * possible to leave it on and go live with a roll that never moves. Saying so is the whole
	 * of the guard: silently turning it off at some later moment would be its own surprise.
	 */
	obs_property_t *warning = obs_properties_add_text(props, "manual_scroll_warning",
							  obs_module_text("ManualScroll.Warning"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(warning, OBS_TEXT_INFO_WARNING);

	obs_properties_t *ending = obs_properties_create();

	obs_property_t *actionList = obs_properties_add_list(ending, "ea_type", obs_module_text("EndingAction"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (EndingActionType type : allEndingActionTypes())
		obs_property_list_add_string(actionList, endingActionName(type), endingActionId(type));
	obs_property_set_modified_callback(actionList, onEndingActionChanged);

	obs_property_t *sceneList = obs_properties_add_list(ending, "ea_scene", obs_module_text("EndingAction.Scene"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fillSceneList(sceneList);

	obs_property_t *filterSource = obs_properties_add_list(ending, "ea_filter_source",
							       obs_module_text("EndingAction.FilterSource"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(filterSource, "", "");
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (const char *name = obs_source_get_name(source))
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		filterSource);
	obs_property_set_modified_callback(filterSource, onFilterSourceChanged);

	obs_properties_add_list(ending, "ea_filter_name", obs_module_text("EndingAction.Filter"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);

	obs_property_t *filterMode = obs_properties_add_list(ending, "ea_filter_mode",
							     obs_module_text("EndingAction.FilterMode"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(filterMode, obs_module_text("EndingAction.FilterMode.Enable"), "enable");
	obs_property_list_add_string(filterMode, obs_module_text("EndingAction.FilterMode.Disable"), "disable");
	obs_property_list_add_string(filterMode, obs_module_text("EndingAction.FilterMode.Toggle"), "toggle");

	obs_property_t *hotkeyList = obs_properties_add_list(ending, "ea_hotkey",
							     obs_module_text("EndingAction.Hotkey"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fillHotkeyList(hotkeyList);

	obs_property_t *delay =
		obs_properties_add_float(ending, "ea_delay", obs_module_text("EndingAction.Delay"), 0.0, 600.0, 0.1);
	obs_property_float_set_suffix(delay, " s");

	obs_properties_add_group(props, "ending_action_group", obs_module_text("EndingActionGroup"), OBS_GROUP_NORMAL,
				 ending);

	return props;
}

struct obs_source_info creditsSourceInfo = {};

} // namespace

void registerCreditsSource()
{
	creditsSourceInfo.id = kCreditsSourceId;
	creditsSourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	/*
	 * sRGB-aware: the source blends in linear light, the way OBS's own sources do, which is also what
	 * lets OBS put it on an HDR canvas correctly. See kRollEffectSource.
	 */
	creditsSourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	creditsSourceInfo.icon_type = OBS_ICON_TYPE_TEXT;
	creditsSourceInfo.get_name = getName;
	creditsSourceInfo.create = create;
	creditsSourceInfo.destroy = destroy;
	creditsSourceInfo.get_width = getWidth;
	creditsSourceInfo.get_height = getHeight;
	creditsSourceInfo.get_defaults = getDefaults;
	creditsSourceInfo.get_properties = getProperties;
	creditsSourceInfo.update = update;
	creditsSourceInfo.show = onShow;
	creditsSourceInfo.hide = onHide;
	creditsSourceInfo.video_tick = videoTick;
	creditsSourceInfo.video_render = videoRender;

	obs_register_source(&creditsSourceInfo);
}

} // namespace closingtime
