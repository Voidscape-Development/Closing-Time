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
#include <QSet>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "model/CreditsModel.hpp"
#include "model/StyleLibrary.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "render/StripRenderer.hpp"
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

/* Where the roll is in its lifecycle. */
enum class Phase {
	/* Parked at the start position, waiting to be armed. */
	Idle,
	/* Armed, counting down the configured start delay. */
	Delaying,
	/* Scrolling. */
	Rolling,
	/* Content has cleared the canvas; the ending action may still be pending. */
	Finished,
};

/*
 * One strip tile: where it sits in the roll, the picture it was rasterized as, and the texture
 * that picture has been uploaded into.
 *
 * The picture is kept rather than uploaded and dropped the moment the strip arrives, because a
 * roll is tiled into as many 2048 px slices as it is long and only ever a screenful of them is on
 * display. Creating every one of those textures in the frame a rebuild lands in means tens or
 * hundreds of megabytes of upload inside one `video_render` -- a frame that takes long enough to
 * be seen, which is exactly the hitch this splitting up exists to avoid. A tile is instead put on
 * the GPU when it is about to be drawn, and one further tile is uploaded per frame ahead of that,
 * so the cost is spread over the frames after a rebuild rather than landing in one of them.
 *
 * Nothing about what reaches the screen changes: a tile that is drawn is uploaded first, in the
 * same frame, before the pass that draws it is started. Nor does the memory this costs -- the
 * pictures were all in RAM at once anyway while the old code uploaded them, and here each one is
 * let go of as it is uploaded, so the two together never hold more than that same peak.
 *
 * Graphics-thread state, like the textures it holds.
 */
struct TileRuntime {
	/* Y offset of this tile's top edge within the strip, in pixels. */
	int top = 0;
	int height = 0;
	gs_texture_t *texture = nullptr;
	/*
	 * The rasterized tile, until it has been uploaded. Null afterwards -- and null with no
	 * texture beside it means the upload was tried and failed, which is what keeps a roll too
	 * big for the GPU from logging once a frame for as long as it is on screen.
	 */
	QImage image;
};

/*
 * One animated logo, mid-playback.
 *
 * The strip left a hole where this logo goes (see AnimatedLogoPlacement) and this is what fills
 * it: a single texture, re-uploaded when the frame changes, drawn as its own quad after the
 * tiles. One texture rather than one per frame because a thirty-second animation is thousands of
 * frames and no GPU wants thousands of small textures for one logo; re-uploading a logo-sized
 * image at ten to thirty frames a second is a rounding error beside the strip itself.
 *
 * Graphics-thread state, like the tile textures beside it.
 */
struct AnimatedLogoRuntime {
	AnimatedLogoPlacement placement;
	gs_texture_t *texture = nullptr;
	gs_texture_t *shadowTexture = nullptr;
	/* Which frame the textures currently hold, or -1 when they hold nothing yet. */
	int uploadedFrame = -1;
	int frame = 0;
	/* How far into the animation playback has run, in milliseconds of the animation's own time. */
	double elapsedMs = 0.0;
	/* False until the animation is running: what `startOnEnter` holds off. */
	bool started = false;
	/* The roll pass this playback belongs to; a new one restarts it. */
	uint64_t epoch = 0;
};

/*
 * One sticky block, mid-roll.
 *
 * The strip left a slot where this block goes (see StickyBlockPlacement) and this is what fills
 * it: one texture, drawn as its own quad after the tiles, at a position this decides rather than
 * at the one the slot has scrolled to. That is the whole feature -- the block detaches from the
 * roll it arrived on and the roll carries on past behind it.
 *
 * Graphics-thread state, like the tile textures beside it.
 */
struct StickyBlockRuntime {
	StickyBlockPlacement placement;
	gs_texture_t *texture = nullptr;

	enum class State {
		/*
		 * Waiting for the roll to go by before showing itself at all -- the entrance that comes
		 * after the credits rather than with them. Nothing of the block is drawn in this state.
		 */
		Hidden,
		/* Fading up in place, having waited. */
		Entering,
		/* Still traveling with the roll, on its way to the anchor. */
		Waiting,
		/* Detached and holding at the anchor. */
		Pinned,
		/* Let go and climbing off the top of the frame under its own steam. */
		Released,
	};

	State state = State::Waiting;
	/* Seconds left of the fade. Meaningless outside Entering. */
	double fadeRemaining = 0.0;
	/* Seconds left of the hold. Meaningless until the block has pinned. */
	double holdRemaining = 0.0;
	/* How far it has traveled since it was released, in pixels. */
	double releasedTravel = 0.0;
	/* Set once its hold has had whatever say it has in ending the roll. */
	bool spent = false;
	/* Set once its leaving the frame has had whatever say it has in ending the roll. */
	bool departed = false;
	/* The roll pass this belongs to; a new one starts it over. */
	uint64_t epoch = 0;

	/* What the block is drawn at this frame: 1 everywhere but part-way through a fade. */
	double alpha() const
	{
		if (state != State::Entering)
			return 1.0;
		if (placement.fadeIn <= 0.0)
			return 1.0;
		return std::clamp(1.0 - fadeRemaining / placement.fadeIn, 0.0, 1.0);
	}

	/* True while the block is somewhere a viewer could see it, whether or not it is moving. */
	bool showing() const { return state != State::Hidden; }
};

/*
 * How the roll is moving this tick, as far as its blocks are concerned.
 *
 *   Rolling  - playing. Blocks pin, fade, hold and leave on their own clock.
 *   Coasting - the roll has been called finished but has not been taken off the screen. Nothing new
 *              starts, but a block already on its way out goes the rest of the way out: a block
 *              frozen half off the top is the one thing worse than one that never left.
 *   Held     - paused. Everything stands exactly where it is.
 *   Scrubbed - parked under manual scroll. There is no clock at all, so each block shows the state
 *              the scroll position alone implies -- pinned where it would be, arrived if the roll
 *              it waits for has gone by, and never part-way through a fade.
 */
enum class StickyClock { Rolling, Coasting, Held, Scrubbed };

/*
 * Puts a block at the top of its own loop: what a freshly rasterized block starts at, and what a
 * roll going back to its beginning puts every block it carries back to.
 */
void startStickyBlock(StickyBlockRuntime &runtime)
{
	const StickyBlockPlacement &placement = runtime.placement;

	runtime.state = stickyEntranceWaitsForRoll(placement.entrance) ? StickyBlockRuntime::State::Hidden
								       : StickyBlockRuntime::State::Waiting;
	runtime.fadeRemaining = std::max(0.0, placement.fadeIn);
	runtime.holdRemaining = std::max(0.0, placement.hold);
	runtime.releasedTravel = 0.0;
	runtime.spent = false;
	runtime.departed = false;
}

/*
 * Where a sticky block's slot-top sits in canvas space this frame.
 *
 * Waiting and pinned are one expression rather than two branches: a block travels with the roll
 * until the roll has carried it up to the anchor and stays there afterwards, which is exactly the
 * lower of the two positions. Written that way so a roll parked in manual scroll -- where none of
 * the timing in advanceStickyBlocks runs -- still shows every block where it belongs.
 *
 * A block that waited for the roll to go by never travels: it belongs at the anchor from the moment
 * it shows itself, which is the whole difference between the two entrances.
 */
double stickyBlockTop(const StickyBlockRuntime &runtime, double stripTop, int canvasHeight)
{
	const StickyBlockPlacement &placement = runtime.placement;
	const double pinnedTop = placement.pinnedTop(canvasHeight);

	if (runtime.state == StickyBlockRuntime::State::Released)
		return pinnedTop - runtime.releasedTravel;

	if (stickyEntranceWaitsForRoll(placement.entrance))
		return pinnedTop;

	return std::max(pinnedTop, stripTop + placement.rect.top());
}

/*
 * Threading:
 *
 *   - `document` is written only by update(), which libobs defers to the graphics thread
 *     for video sources, and by create() before the source is visible to anyone else. The
 *     graphics thread can therefore read it without a lock.
 *   - Playback state is mutated from hotkey and proc-handler callbacks on the UI thread as
 *     well as from video_tick, so it lives behind `stateMutex`.
 *   - The rendered strip crosses from the render thread to the graphics thread through
 *     `pendingStrip` under `handoffMutex`; the GPU textures themselves are only ever
 *     touched by the graphics thread.
 */
struct CreditsSourceData {
	obs_source_t *source = nullptr;

	Document document;
	/* What the last rebuild was rasterized from; see renderKey(). Graphics thread only. */
	QByteArray renderedFrom;
	/*
	 * Owned by the render thread: only the rebuild job reads or writes it, and jobs run
	 * one at a time, so neither of these needs a lock.
	 */
	LogoCache logos;
	AnimatedLogoCache animations;
	/* Families already reported, so a rebuild per keystroke does not spam the log. */
	QSet<QString> warnedFonts;

	std::mutex handoffMutex;
	Strip pendingStrip;
	/*
	 * Atomic so the graphics thread can find out there is nothing waiting without taking the
	 * lock: this is asked once per render of the source, and a strip arrives once per rebuild.
	 * Written under the mutex all the same, beside the strip it describes.
	 */
	std::atomic<bool> hasPendingStrip{false};
	/* Set while a rebuild is in flight so a burst of edits collapses into one re-render. */
	bool rebuildInFlight = false;
	bool rebuildAgain = false;

	/*
	 * Set when loading brought the document up to date against the library -- a preset renamed
	 * there, or a linked style edited -- and the settings the document came from are now behind
	 * it. The write-back happens on the next tick rather than inside update(), because writing
	 * settings from inside update() is how a source calls itself in a circle.
	 */
	bool settingsNeedWriteBack = false;

	/*
	 * The library as this roll last saw it, and how long it has been since anybody was asked to
	 * look at the file.
	 *
	 * The look itself is a `stat`, and a `stat` is a blocking call on whatever thread makes it.
	 * Made from `video_tick` it is one on the thread compositing the whole program -- rare
	 * enough to be invisible on a warm cache and, on a cold one or behind a filter driver, the
	 * kind of stall that reads as the roll catching. So the poll is handed to the render thread
	 * and the tick is left comparing two numbers.
	 *
	 * A serial per source rather than the poll's own answer, because the poll has exactly one:
	 * whichever source's turn came up would consume the change and every other roll on the
	 * machine would go on rendering the style the library no longer holds.
	 *
	 * Graphics-thread state.
	 */
	quint64 librarySerial = 0;
	double libraryPollElapsed = 0.0;

	/* Graphics-thread state. */
	std::vector<TileRuntime> tiles;
	/*
	 * How many of them still hold a picture that has not been to the GPU. Counted rather than
	 * looked for, so the frames after the last tile has gone up -- which is every frame of a roll
	 * that has been running for a few seconds -- have nothing to look through.
	 */
	size_t tilesAwaitingUpload = 0;
	std::vector<AnimatedLogoRuntime> animatedLogos;
	std::vector<StickyBlockRuntime> stickyBlocks;
	/*
	 * The shader a sticky block fades up through, compiled the first time one is drawn and kept for
	 * the life of the source. Null when the device would not compile it, which is a fade lost
	 * rather than a block lost -- see drawStickyBlocks.
	 *
	 * Graphics-thread state, like the textures above.
	 */
	gs_effect_t *fadeEffect = nullptr;
	bool fadeEffectTried = false;
	int stripHeight = 0;
	/*
	 * True while a sticky block still has something to do before the roll can be called
	 * finished -- a hold that has not run out, or a released block still on screen. Written on
	 * the graphics thread each tick, before `advance` reads it, and both run only there.
	 */
	bool stickyPending = false;

	std::mutex stateMutex;
	Phase phase = Phase::Idle;
	double offset = 0.0;
	double delayRemaining = 0.0;
	double actionRemaining = 0.0;
	bool actionPending = false;
	bool paused = false;
	/*
	 * Bumped whenever the roll goes back to its beginning -- armed, reset, or wrapped by a loop.
	 * An animation that started with the roll has to start again when the roll does, and the
	 * offset alone cannot say that happened: it is a number that went down, which is also what a
	 * scrub does.
	 */
	uint64_t rollEpoch = 0;

	obs_hotkey_id startHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id pauseHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id restartHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id designerHotkey = OBS_INVALID_HOTKEY_ID;
};

/* ------------------------------------------------------------------ strip rebuilding */

struct RebuildTask {
	/*
	 * The weak reference proves the source is still alive when the task runs; `data` is
	 * valid for as long as that strong reference is held, because destroy() cannot run
	 * until the last reference goes away.
	 */
	OBSWeakSourceAutoRelease weak;
	CreditsSourceData *data = nullptr;
	Document document;
	/* Copied rather than read back later, so the render thread never touches the source. */
	QString sourceName;
};

/*
 * Everything the strip is rasterized from, as a string two documents can be compared by.
 *
 * Playback settings reach the source through the same update() every content edit does, and
 * scrubbing moves a slider that fires one per frame of the drag. Rasterizing a long roll on each
 * of those would keep the render thread busy for the whole gesture and hand back a stream of
 * strips identical to the one already on the GPU, so a rebuild is queued only when this changes.
 *
 * Built by blanking the fields that move the finished strip rather than by listing the ones that
 * make it: a field added later counts towards the key by default, which costs an unnecessary
 * rebuild if it turns out to be a playback setting and never a stale one if it is not. Lead-in
 * and lead-out are deliberately not blanked -- they are baked into the strip as blank space.
 *
 * Compact JSON, as bytes, rather than the pretty string `toJson` hands out. This is built on every
 * update -- which is once per frame of a slider drag -- and neither the indentation nor the
 * conversion of a document-sized string into UTF-16 is any part of telling two documents apart.
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
	 * The bundled font files are reduced to their sizes rather than carried. This key is built
	 * on every update -- which is once per frame of a slider drag -- and a font runs to
	 * megabytes: serializing them here would cost more than the rebuild it exists to avoid.
	 * What is left still names every family and every file, which is what a bundle changing
	 * actually looks like.
	 */
	for (BundledFont &font : content.bundledFonts)
		font.data = QByteArray::number(font.data.size());

	OBSDataAutoRelease data = obs_data_create();
	content.save(data);
	return QByteArray(obs_data_get_json(data));
}

void runRebuild(const std::shared_ptr<RebuildTask> &task);

void queueRebuild(CreditsSourceData *data)
{
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (data->rebuildInFlight) {
			data->rebuildAgain = true;
			return;
		}
		data->rebuildInFlight = true;
	}

	auto task = std::make_shared<RebuildTask>();
	task->weak = obs_source_get_weak_source(data->source);
	task->data = data;
	task->document = data->document;
	task->sourceName = QString::fromUtf8(obs_source_get_name(data->source));

	/*
	 * Rasterization is long enough to be seen if it happens on the thread drawing OBS's
	 * own window, so it goes to the shared render thread instead. The handoff below is
	 * unchanged: the graphics thread still picks the finished tiles up in video_render.
	 */
	postRenderJob([task] { runRebuild(task); });
}

void runRebuild(const std::shared_ptr<RebuildTask> &task)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(task->weak);
	if (!source)
		return;

	CreditsSourceData *data = task->data;

	/*
	 * The roll's own font files, registered before anything is measured against them. Doing it
	 * here rather than at load means it happens on the render thread, which is the one thread
	 * these documents are laid out on, and it costs a hash lookup on every rebuild after the
	 * first. The renderer asks for them again itself -- see documentWithFontsResolved -- so this
	 * is only what makes them reportable in the log.
	 */
	for (const QString &family : installDocumentFonts(task->document)) {
		obs_log(LOG_INFO, "font '%s' is not installed; '%s' is rendering it from its own bundle",
			family.toUtf8().constData(), task->sourceName.toUtf8().constData());
	}

	/*
	 * A missing font is not fatal -- Qt substitutes one -- but it silently changes what goes to
	 * air, so each family is called out once per source in the OBS log. A family with a stand-in
	 * recorded for it is called out too, at a lower level: what it renders as is the designer's
	 * choice rather than Qt's, which makes it worth saying but not worth warning about.
	 */
	for (const QString &family : missingFontFamilies(task->document)) {
		if (data->warnedFonts.contains(family))
			continue;

		data->warnedFonts.insert(family);

		const QString substitute = task->document.fontSubstitute(family);
		if (substitute.isEmpty())
			obs_log(LOG_WARNING, "font '%s' is not installed; '%s' will render with a substitute",
				family.toUtf8().constData(), task->sourceName.toUtf8().constData());
		else
			obs_log(LOG_INFO, "font '%s' is not installed; '%s' will render it as '%s'",
				family.toUtf8().constData(), task->sourceName.toUtf8().constData(),
				substitute.toUtf8().constData());
	}

	StripRenderer renderer(&data->logos, &data->animations);
	Strip strip = renderer.render(task->document);

	bool again = false;
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		data->pendingStrip = std::move(strip);
		data->hasPendingStrip.store(true, std::memory_order_release);
		data->rebuildInFlight = false;
		again = data->rebuildAgain;
		data->rebuildAgain = false;
	}

	if (again)
		queueRebuild(data);
}

/* Graphics thread only. */
void releaseTextures(CreditsSourceData *data)
{
	for (TileRuntime &tile : data->tiles) {
		if (tile.texture)
			gs_texture_destroy(tile.texture);
	}

	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		if (runtime.texture)
			gs_texture_destroy(runtime.texture);
		if (runtime.shadowTexture)
			gs_texture_destroy(runtime.shadowTexture);
	}

	for (StickyBlockRuntime &runtime : data->stickyBlocks) {
		if (runtime.texture)
			gs_texture_destroy(runtime.texture);
	}

	data->tiles.clear();
	data->tilesAwaitingUpload = 0;
	data->animatedLogos.clear();
	data->stickyBlocks.clear();
	data->stripHeight = 0;
	data->stickyPending = false;
}

/*
 * Graphics thread only; requires an active graphics context.
 *
 * `flags` is GS_DYNAMIC for a texture that will be written again -- an animation's frames go
 * through one texture rather than one apiece -- and nothing at all for a picture that goes up once
 * and is only ever drawn, which is what a strip tile and a sticky block are. Asking for a dynamic
 * texture that is never rewritten buys the driver's upload path for nothing.
 */
gs_texture_t *createTexture(const QImage &image, uint32_t flags)
{
	if (image.isNull())
		return nullptr;

	const uint8_t *bits = image.constBits();
	return gs_texture_create(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()), GS_BGRA,
				 1, &bits, flags);
}

/*
 * Puts the current frame on the GPU, allocating the texture the first time.
 *
 * The frames of one animation are all the same size -- the decoder guarantees it -- so the
 * texture is allocated once and written over from then on.
 */
void uploadFrame(AnimatedLogoRuntime &runtime)
{
	if (runtime.uploadedFrame == runtime.frame)
		return;

	const LogoAnimation &animation = *runtime.placement.animation;
	if (runtime.frame < 0 || runtime.frame >= animation.frames.size())
		return;

	const QImage &image = animation.frames.at(runtime.frame).image;

	if (!runtime.texture) {
		runtime.texture = createTexture(image, GS_DYNAMIC);
		if (!runtime.texture) {
			obs_log(LOG_ERROR, "failed to allocate a %dx%d animated logo texture", image.width(),
				image.height());
			/* Marked as uploaded so the failure is not retried once a frame for the whole roll. */
			runtime.uploadedFrame = runtime.frame;
			return;
		}
	} else {
		gs_texture_set_image(runtime.texture, image.constBits(), static_cast<uint32_t>(image.bytesPerLine()),
				     false);
	}

	const QVector<QImage> &shadows = runtime.placement.shadowFrames;
	if (!shadows.isEmpty() && runtime.frame < shadows.size()) {
		const QImage &shadow = shadows.at(runtime.frame);
		if (!runtime.shadowTexture)
			runtime.shadowTexture = createTexture(shadow, GS_DYNAMIC);
		else
			gs_texture_set_image(runtime.shadowTexture, shadow.constBits(),
					     static_cast<uint32_t>(shadow.bytesPerLine()), false);
	}

	runtime.uploadedFrame = runtime.frame;
}

/*
 * Puts one tile's picture on the GPU and lets go of the picture.
 *
 * Returns false for a tile there is nothing to draw from: one whose texture could not be
 * allocated, which is remembered by the picture having been dropped along with it, so a roll too
 * large for the GPU says so once rather than once a frame for as long as it is on screen.
 *
 * Graphics thread only; requires an active graphics context, and must not be called from inside a
 * technique's pass -- see the ordering in videoRender.
 */
bool uploadTile(CreditsSourceData *data, size_t index)
{
	TileRuntime &tile = data->tiles[index];

	if (tile.texture)
		return true;
	if (tile.image.isNull())
		return false;

	tile.texture = createTexture(tile.image, 0);
	if (!tile.texture)
		obs_log(LOG_ERROR, "failed to allocate a %dx%d credit strip texture", tile.image.width(),
			tile.image.height());

	/*
	 * Dropped either way. It has served its purpose on a success, and on a failure it is what
	 * says the failure has already been reported.
	 */
	tile.image = QImage();
	--data->tilesAwaitingUpload;
	return tile.texture != nullptr;
}

/*
 * The run of tiles that falls inside the canvas this frame, as a half-open [first, last).
 *
 * Tiles are held in strip order and each is a slice of the same strip, so the visible ones are a
 * single run and the search for it stops at the first tile past the bottom of the frame. A long
 * roll is a hundred tiles of which two are ever on display, and walking all hundred of them to set
 * a texture parameter and work out that there is nothing to draw is a cost that grows with the
 * length of the roll for no picture.
 */
struct TileRange {
	size_t first = 0;
	size_t last = 0;
};

TileRange visibleTiles(const CreditsSourceData *data, double stripTop, int canvasHeight)
{
	TileRange range;
	bool started = false;

	for (size_t i = 0; i < data->tiles.size(); ++i) {
		const TileRuntime &tile = data->tiles[i];
		const double top = stripTop + tile.top;

		if (top + tile.height <= 0.0)
			continue;
		if (top >= canvasHeight)
			break;

		if (!started) {
			range.first = i;
			started = true;
		}
		range.last = i + 1;
	}

	return range;
}

/*
 * Uploads one tile that is not on screen yet.
 *
 * A tile is put on the GPU when it is about to be drawn, which on its own is enough to keep the
 * picture right: at a couple of tiles a screenful and 2048 pixels a tile, a roll meets a new one
 * every several seconds. This is what keeps the pictures from sitting in RAM in the meantime, and
 * what puts the upload a frame or more before the tile is needed rather than in the frame that
 * needs it. One per frame, because a tile is fifteen megabytes and the whole point is not to do
 * several of them at once.
 *
 * Started from the tile after the visible run and wrapping round, so the roll uploads in the
 * direction it is travelling.
 *
 * Graphics thread only; requires an active graphics context.
 */
void uploadTileAhead(CreditsSourceData *data, size_t from)
{
	if (data->tilesAwaitingUpload == 0)
		return;

	const size_t count = data->tiles.size();

	for (size_t step = 0; step < count; ++step) {
		const size_t index = (from + step) % count;
		if (data->tiles[index].image.isNull())
			continue;

		uploadTile(data, index);
		return;
	}
}

/*
 * Takes the finished strip off the render thread.
 *
 * Nothing is uploaded here: the tiles are adopted with their pictures still in hand and go to the
 * GPU as they are reached -- see TileRuntime, which is where the reason for that lives.
 *
 * Graphics thread only.
 */
void adoptPendingStrip(CreditsSourceData *data)
{
	/* Asked once per render, answered without the lock on every frame but the one that matters. */
	if (!data->hasPendingStrip.load(std::memory_order_acquire))
		return;

	Strip strip;
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (!data->hasPendingStrip)
			return;

		strip = std::move(data->pendingStrip);
		data->pendingStrip = Strip();
		data->hasPendingStrip.store(false, std::memory_order_relaxed);
	}

	releaseTextures(data);
	data->stripHeight = strip.height;

	data->animatedLogos.reserve(strip.animatedLogos.size());
	for (AnimatedLogoPlacement &placement : strip.animatedLogos) {
		AnimatedLogoRuntime runtime;
		runtime.placement = std::move(placement);
		/*
		 * Textures are left unallocated until the logo reaches the frame; see
		 * prepareAnimatedLogos. A roll may place more animated logos than are ever on screen at
		 * once, and the ones the viewer scrolls past in the last minute of a ten-minute roll
		 * have no business holding VRAM from the start.
		 */
		data->animatedLogos.push_back(std::move(runtime));
	}

	data->stickyBlocks.reserve(strip.stickyBlocks.size());
	for (StickyBlockPlacement &placement : strip.stickyBlocks) {
		StickyBlockRuntime runtime;
		runtime.placement = std::move(placement);
		startStickyBlock(runtime);
		/*
		 * The texture is left unallocated until the block is nearly in frame -- see
		 * prepareStickyBlocks -- for the reason the animated logos' are: a roll may carry a
		 * block the viewer does not reach for minutes, and a canvas-sized picture is not a small
		 * thing to hold VRAM for in the meantime.
		 */
		data->stickyBlocks.push_back(std::move(runtime));
	}

	data->tiles.reserve(strip.tiles.size());
	for (StripTile &tile : strip.tiles) {
		TileRuntime runtime;
		runtime.top = tile.top;
		runtime.height = tile.image.height();
		runtime.image = std::move(tile.image);
		data->tiles.push_back(std::move(runtime));
	}

	data->tilesAwaitingUpload = data->tiles.size();
}

/* ------------------------------------------------------------------ playback control */

/* Callers must hold stateMutex. */
void resetRollLocked(CreditsSourceData *data)
{
	++data->rollEpoch;
	data->phase = Phase::Idle;
	data->offset = 0.0;
	data->delayRemaining = 0.0;
	data->actionRemaining = 0.0;
	data->actionPending = false;
	data->paused = false;
}

/* Callers must hold stateMutex. */
void armRollLocked(CreditsSourceData *data, double startDelay)
{
	resetRollLocked(data);
	data->delayRemaining = startDelay;
	data->phase = startDelay > 0.0 ? Phase::Delaying : Phase::Rolling;
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
 * The distance the roll travels in full: the strip's own height plus the canvas it enters from
 * below and leaves through the top.
 */
double rollTravel(const CreditsSourceData *data)
{
	return static_cast<double>(std::max(1, data->document.height)) + data->stripHeight;
}

/*
 * Parks the roll at the scrub position instead of advancing it.
 *
 * The position is a share of the full travel rather than a pixel offset or a number of seconds,
 * so it means the same thing after the content is edited or the scroll speed is changed -- which
 * is the whole point of a control used while the roll is still being written.
 *
 * The phase is deliberately left alone. Nothing advances while this is on, so the roll cannot
 * reach the finished phase and the ending action cannot fire; leaving the phase as playback set
 * it means switching manual scrolling back off resumes from a state update() already knows how
 * to re-arm rather than from one invented here.
 */
void scrubTo(CreditsSourceData *data, double percent)
{
	const double offset = rollTravel(data) * std::clamp(percent, 0.0, 100.0) / 100.0;

	std::lock_guard<std::mutex> lock(data->stateMutex);
	data->offset = offset;
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

void advance(CreditsSourceData *data, double seconds)
{
	const Document &document = data->document;

	/*
	 * The strip starts one canvas-height below the top of the frame, so the distance it
	 * has to travel before the last pixel clears the top is canvas + strip.
	 */
	const double travel = rollTravel(data);

	bool fireAction = false;
	bool finished = false;

	{
		std::lock_guard<std::mutex> lock(data->stateMutex);

		switch (data->phase) {
		case Phase::Idle:
		case Phase::Finished:
			break;

		case Phase::Delaying:
			data->delayRemaining -= seconds;
			if (data->delayRemaining <= 0.0) {
				data->delayRemaining = 0.0;
				data->phase = Phase::Rolling;
			}
			break;

		case Phase::Rolling:
			if (data->paused)
				break;

			data->offset += document.scrollSpeed * seconds;
			if (data->offset < travel)
				break;

			if (document.loop) {
				/*
				 * Wrapping by subtraction rather than snapping to zero keeps the
				 * loop seamless: this frame's overshoot carries into the next pass.
				 */
				data->offset -= travel;
				++data->rollEpoch;
				break;
			}

			data->offset = travel;

			/*
			 * The strip has cleared the frame, but a sticky block may not have: one still
			 * holding at its anchor, or climbing off after being let go, is content that is
			 * on screen, and a roll is not over while something of it still is. The block
			 * is what ends the roll in that case -- see advanceStickyBlocks -- so the phase
			 * is left as it is and the roll simply stops moving.
			 */
			if (data->stickyPending)
				break;

			data->phase = Phase::Finished;
			finished = true;

			if (document.endingAction.type != EndingActionType::None) {
				data->actionPending = true;
				data->actionRemaining = document.endingAction.delay;
			}
			break;
		}

		if (data->actionPending && !finished) {
			data->actionRemaining -= seconds;
			if (data->actionRemaining <= 0.0) {
				data->actionPending = false;
				data->actionRemaining = 0.0;
				fireAction = true;
			}
		}
	}

	/* Signals and actions run outside the lock so their handlers can call back in. */
	if (finished) {
		emitCreditsFinished(data->source);

		if (document.endingAction.type != EndingActionType::None && document.endingAction.delay <= 0.0) {
			std::lock_guard<std::mutex> lock(data->stateMutex);
			data->actionPending = false;
			fireAction = true;
		}
	}

	if (fireAction)
		document.endingAction.execute(data->source);
}

/*
 * Ends the roll from outside `advance` -- which today means a sticky block whose hold has run out
 * and whose release says that is the end of it.
 *
 * Everything the natural end does, in the same order: the phase, the ending action's countdown,
 * the signal, and the action itself when there is no delay to wait through. Doing it here rather
 * than teaching `advance` about blocks keeps the scroll loop about scrolling.
 *
 * Graphics thread only, and does nothing for a roll that has already finished or is looping.
 */
void finishRoll(CreditsSourceData *data)
{
	const Document &document = data->document;
	if (document.loop)
		return;

	bool fireAction = false;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		if (data->phase == Phase::Finished || data->phase == Phase::Idle)
			return;

		data->phase = Phase::Finished;

		if (document.endingAction.type != EndingActionType::None) {
			data->actionPending = true;
			data->actionRemaining = document.endingAction.delay;

			if (document.endingAction.delay <= 0.0) {
				data->actionPending = false;
				fireAction = true;
			}
		}
	}

	/* Outside the lock, so a handler can call straight back in. */
	emitCreditsFinished(data->source);

	if (fireAction)
		document.endingAction.execute(data->source);
}

/*
 * Advances every sticky block by one tick, and reports whether any of them is still to have its
 * say about the roll being over.
 *
 * A block either travels with the roll until its slot reaches the anchor, or waits off screen for
 * the roll to go by and then fades up in place. Either way it holds at the anchor while whatever is
 * left of the roll scrolls on behind it. What happens when the hold runs out is the block's own
 * setting: it stays where it is and ends the roll, it carries on up and off the top, it does both,
 * or it leaves and ends nothing so that another block can follow it.
 *
 * Where the block is *drawn* is not decided here -- see stickyBlockTop, which reads the state this
 * leaves behind. That split is what lets a roll parked in manual scroll show its blocks pinned
 * without any of the timing below running at all.
 *
 * Graphics thread only.
 */
void advanceStickyBlocks(CreditsSourceData *data, double seconds, StickyClock clock)
{
	/* A roll with no blocks in it has nothing here to hold it open, and nothing to advance. */
	if (data->stickyBlocks.empty()) {
		data->stickyPending = false;
		return;
	}

	const Document &document = data->document;
	const int canvasHeight = std::max(1, document.height);

	double offset = 0.0;
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		offset = data->offset;
		epoch = data->rollEpoch;
	}

	const bool rolling = clock == StickyClock::Rolling;
	const bool scrubbed = clock == StickyClock::Scrubbed;
	/*
	 * Rolling and coasting both carry a block that is already on its way out the rest of the way
	 * out; paused and scrubbed stand still. A block frozen half off the top of the frame is what
	 * the coasting case exists to prevent -- the roll being called finished is not a reason for the
	 * last thing on screen to stop where it is.
	 */
	const bool moving = rolling || clock == StickyClock::Coasting;

	/* The roll has gone as far as it goes; nothing below can still be on its way to an anchor. */
	const bool traveled = offset >= rollTravel(data) - 0.5;
	const double stripTop = canvasHeight - offset;

	/*
	 * Where the last pixel the strip actually draws is this frame. Everything past it is lead-out:
	 * blank, and no part of what anybody watching would call the roll, so a block waiting for the
	 * roll to go by has no business waiting through it.
	 */
	const double rollBottom = stripTop + std::max(0, data->stripHeight - document.leadOut);
	const bool rollCleared = rollBottom <= 0.0;

	/* A new pass of the roll is a new pass for its blocks: back to the top of the loop. */
	for (StickyBlockRuntime &runtime : data->stickyBlocks) {
		if (runtime.epoch == epoch)
			continue;

		runtime.epoch = epoch;
		startStickyBlock(runtime);
	}

	/*
	 * Whether anything a viewer can see is on the frame, counted before any of it is advanced.
	 * A block whose entrance comes after the roll waits for the other blocks as well as for the
	 * credits: back-to-back cards are the reason that entrance exists, and one fading up through
	 * another still on its way out is not that.
	 */
	bool blockOnFrame = false;
	for (const StickyBlockRuntime &runtime : data->stickyBlocks) {
		if (runtime.showing() &&
		    !runtime.placement.offFrame(stickyBlockTop(runtime, stripTop, canvasHeight), canvasHeight))
			blockOnFrame = true;
	}

	bool pending = false;
	bool endNow = false;

	for (StickyBlockRuntime &runtime : data->stickyBlocks) {
		const StickyBlockPlacement &placement = runtime.placement;
		const double pinnedTop = placement.pinnedTop(canvasHeight);

		switch (runtime.state) {
		case StickyBlockRuntime::State::Hidden:
			/*
			 * Nothing of the roll left to see, and no other block in the way: the card can
			 * come up. Scrubbing arrives here too, which is what makes the entrance something
			 * the designer's scroll position can be dragged into rather than only played into.
			 */
			if (!rollCleared || blockOnFrame)
				break;
			if (!rolling && !scrubbed)
				break;

			runtime.state = StickyBlockRuntime::State::Entering;
			runtime.fadeRemaining = scrubbed ? 0.0 : std::max(0.0, placement.fadeIn);
			[[fallthrough]];

		case StickyBlockRuntime::State::Entering:
			/* Parked rather than played: a scrubbed block is shown arrived, never mid-fade. */
			if (scrubbed)
				runtime.fadeRemaining = 0.0;
			else if (moving)
				runtime.fadeRemaining -= seconds;

			if (runtime.fadeRemaining > 0.0)
				break;

			/* The hold starts once the card is all the way up, not while it is arriving. */
			runtime.fadeRemaining = 0.0;
			runtime.state = StickyBlockRuntime::State::Pinned;
			break;

		case StickyBlockRuntime::State::Waiting: {
			/*
			 * Pinned once the slot has carried it up to the anchor -- or once the roll has
			 * finished traveling, which catches a pin the roll would never otherwise reach
			 * (a negative offset on a short roll) rather than leaving the roll waiting on a
			 * block that can never arrive.
			 */
			const double naturalTop = stripTop + placement.rect.top();
			if (rolling && (naturalTop <= pinnedTop || traveled))
				runtime.state = StickyBlockRuntime::State::Pinned;
			break;
		}

		case StickyBlockRuntime::State::Pinned:
			if (!rolling || placement.holdForever)
				break;

			runtime.holdRemaining -= seconds;
			if (runtime.holdRemaining > 0.0)
				break;

			runtime.holdRemaining = 0.0;
			if (stickyReleaseEndsAtHold(placement.release) && !runtime.spent)
				endNow = true;

			runtime.spent = true;
			if (stickyReleaseResumes(placement.release))
				runtime.state = StickyBlockRuntime::State::Released;
			break;

		case StickyBlockRuntime::State::Released:
			if (moving)
				runtime.releasedTravel += document.scrollSpeed * seconds;
			break;
		}

		/*
		 * "Off screen" is the picture, not the slot: the block's backdrop and whatever its
		 * children paint outside their own boxes reach past the slot at both ends, and a release
		 * that waits for the block to leave has to wait for the last of that to go too.
		 */
		const bool onFrame = runtime.showing() &&
				     !placement.clearedFrame(stickyBlockTop(runtime, stripTop, canvasHeight));

		/* A block that has left, under a release that says its leaving is the end of the roll. */
		if (runtime.state == StickyBlockRuntime::State::Released && !onFrame && !runtime.departed) {
			runtime.departed = true;
			if (stickyReleaseEndsAtExit(placement.release))
				endNow = true;
		}

		/*
		 * What still has to happen before the roll is over. A block yet to arrive holds it open
		 * until it has; one that ends the roll itself holds it open until it has; one that only
		 * leaves holds it open until it has left. A block that ends nothing holds it open while it
		 * is on screen and then stands aside, so that the rest of the roll decides.
		 */
		if (!runtime.showing() || runtime.state == StickyBlockRuntime::State::Entering) {
			pending = true;
		} else if (stickyReleaseEndsAtHold(placement.release)) {
			pending = pending || !runtime.spent;
		} else if (stickyReleaseEndsAtExit(placement.release)) {
			pending = pending || !runtime.departed;
		} else {
			pending = pending || onFrame;
		}
	}

	data->stickyPending = pending;

	if (endNow)
		finishRoll(data);
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
	 * Only a change to what the strip is made of is worth rasterizing again -- see renderKey().
	 * Everything else here arrives through the same call: a scrub sends one per frame of the
	 * drag, and rebuilding for those would keep the render thread busy producing strips
	 * identical to the one already uploaded.
	 */
	const QByteArray key = renderKey(data->document);
	const bool contentChanged = key != data->renderedFrom;
	if (contentChanged) {
		data->renderedFrom = key;
		queueRebuild(data);
	}

	/*
	 * Geometry and content edits invalidate the current scroll position, so a roll that is
	 * already running restarts instead of jumping to a stale offset in new content. A playback
	 * setting changing is not that: a roll keeps its position when the scroll speed is adjusted
	 * under it, and dragging the scrub slider does not re-arm the roll once per frame.
	 */
	std::lock_guard<std::mutex> lock(data->stateMutex);
	if (data->phase == Phase::Idle) {
		/*
		 * An idle roll draws at whatever offset it is holding, and scrubbing leaves one
		 * there. Without this, turning manual scrolling off on a source that is hidden -- or
		 * that does not start on show -- would leave the roll frozen wherever the slider was
		 * rather than parked at its start, waiting to run from a position nothing chose.
		 */
		if (!data->document.manualScroll)
			data->offset = 0.0;
	} else if (contentChanged) {
		armRollLocked(data, data->document.startDelay);
	}
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new CreditsSourceData();
	data->source = source;

	signal_handler_add(obs_source_get_signal_handler(source), "void credits_finished(ptr source)");

	proc_handler_t *procs = obs_source_get_proc_handler(source);
	proc_handler_add(
		procs, "void restart()",
		[](void *param, calldata_t *) { armRoll(static_cast<CreditsSourceData *>(param)); }, data);
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

	data->startHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Start", obs_module_text("Hotkey.Start"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (!pressed)
				return;

			auto *self = static_cast<CreditsSourceData *>(param);
			std::unique_lock<std::mutex> lock(self->stateMutex);
			if (self->phase == Phase::Idle || self->phase == Phase::Finished)
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
	if (data->fadeEffect)
		gs_effect_destroy(data->fadeEffect);
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

/*
 * Advances every animated logo by one tick.
 *
 * Animations move with the roll rather than with the wall clock: a paused roll is a still frame,
 * and a roll parked in manual scroll shows the frame it was parked on. A credit roll is a
 * composed picture, and having its logos carry on jigging about while the thing they belong to is
 * held still is the kind of motion that reads as a bug.
 *
 * Graphics thread only.
 */
void advanceAnimatedLogos(CreditsSourceData *data, double seconds, bool rolling)
{
	if (data->animatedLogos.empty())
		return;

	double offset = 0.0;
	uint64_t epoch = 0;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		offset = data->offset;
		epoch = data->rollEpoch;
	}

	const double canvasHeight = std::max(1, data->document.height);
	const double stripTop = canvasHeight - offset;

	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		if (!runtime.placement.animation)
			continue;

		/* A roll that went back to its beginning plays its animations from theirs. */
		if (runtime.epoch != epoch) {
			runtime.epoch = epoch;
			runtime.started = false;
			runtime.frame = 0;
			runtime.elapsedMs = 0.0;
		}

		const double top = stripTop + runtime.placement.rect.top();
		const bool entered = top < canvasHeight;

		if (!runtime.started) {
			/*
			 * A play-once sting bound to a logo two thirds of the way down a long roll
			 * would otherwise have finished several minutes before anyone could see it.
			 */
			if (runtime.placement.playback.startOnEnter && !entered)
				continue;

			runtime.started = true;
		}

		if (!rolling)
			continue;

		const double speed = std::clamp(runtime.placement.playback.speed, kMinLogoSpeed, kMaxLogoSpeed);
		runtime.elapsedMs += seconds * 1000.0 * speed;
		runtime.frame =
			logoFrameAt(*runtime.placement.animation, runtime.elapsedMs, runtime.placement.playback.loop);
	}
}

void videoTick(void *raw, float seconds)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	if (data->settingsNeedWriteBack) {
		data->settingsNeedWriteBack = false;
		writeDocumentBack(data);
	}

	/*
	 * A style the library changed underneath this roll -- edited in another OBS window, imported,
	 * or hand-edited -- is pulled in here. Before the manual-scroll branch below, because a roll
	 * parked for editing is exactly the one somebody is restyling.
	 *
	 * Asking the file whether it moved is a `stat`, so it is asked for on the render thread and
	 * this tick only compares the library's serial against the one this roll last saw. What that
	 * costs here is an atomic read and an addition; what it saves is a blocking call on the thread
	 * compositing the program, once a second, for every roll on the machine.
	 */
	data->libraryPollElapsed += seconds;
	if (data->libraryPollElapsed >= kLibraryPollSeconds) {
		data->libraryPollElapsed = 0.0;
		postRenderJob([] { StyleLibrary::instance().pollForChanges(); });
	}

	if (const quint64 serial = StyleLibrary::instance().serial(); serial != data->librarySerial) {
		data->librarySerial = serial;

		/* A rebuild only when a style bound to *this* document actually moved. */
		if (data->document.refreshLinkedPresets()) {
			queueRebuild(data);
			/*
			 * Saved as well as redrawn. The refreshed copy is this roll's fallback on a
			 * machine without the library, and a rename it just followed has to survive a
			 * restart -- both of which mean the settings, not just the strip.
			 */
			writeDocumentBack(data);
		}
	}

	/*
	 * Re-applied every tick rather than once when the setting changes, because the position it
	 * resolves to depends on the strip: a rebuild finishing, or a canvas resize, changes the
	 * travel underneath it, and a roll parked halfway through should stay halfway through.
	 */
	if (data->document.manualScroll) {
		scrubTo(data, data->document.scrollPosition);
		/*
		 * The blocks are still asked where they are, with no time to advance: parked is a state
		 * they have an answer for, and it is the one that puts a card whose entrance waits for
		 * the roll on screen when the scroll position is dragged past the end of it.
		 */
		advanceStickyBlocks(data, 0.0, StickyClock::Scrubbed);
		advanceAnimatedLogos(data, 0.0, false);
		return;
	}

	const double delta = tickSeconds(seconds);
	advance(data, delta);

	bool rolling = false;
	StickyClock clock = StickyClock::Held;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		rolling = data->phase == Phase::Rolling && !data->paused;

		if (rolling)
			clock = StickyClock::Rolling;
		else if (!data->paused && data->phase != Phase::Idle)
			clock = StickyClock::Coasting;
	}

	/*
	 * Before the animated logos and after the scroll, because a block's own timing is measured
	 * against the offset this tick just left behind.
	 */
	advanceStickyBlocks(data, delta, clock);
	advanceAnimatedLogos(data, delta, rolling);
}

void drawBackground(const QColor &color, int width, int height)
{
	if (color.alpha() <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	struct vec4 value;
	vec4_set(&value, static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
		 static_cast<float>(color.blueF()), static_cast<float>(color.alphaF()));
	gs_effect_set_vec4(colorParam, &value);

	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

/*
 * Draws one strip tile, clipped to the canvas.
 *
 * `top` is where the tile's own top edge lands in canvas space, and it is fractional: the roll
 * advances by whatever part of a pixel the scroll speed and the frame rate work out to, and that
 * fraction is what makes the movement read as smooth rather than as a step every few frames.
 *
 * Clipping through gs_draw_sprite_subregion cannot carry it. That call takes whole texels, so
 * the visible height has to be rounded down to the previous whole pixel, and the quad ends up
 * short of the canvas edge by the fraction that was dropped -- a sliver along the bottom that
 * never gets painted, which content entering from below appears to pop through rather than slide
 * into. Building the quad here puts the fraction into both the geometry and the texture
 * coordinates instead, so the tile meets the edge it is clipped against exactly.
 */
void drawClipped(double left, double top, double width, double height, int canvasHeight)
{
	if (width <= 0.0 || height <= 0.0)
		return;

	const double visibleTop = std::max(top, 0.0);
	const double visibleBottom = std::min(top + height, static_cast<double>(canvasHeight));
	if (visibleBottom <= visibleTop)
		return;

	const auto x0 = static_cast<float>(left);
	const auto x1 = static_cast<float>(left + width);
	const auto y0 = static_cast<float>(visibleTop);
	const auto y1 = static_cast<float>(visibleBottom);
	const auto v0 = static_cast<float>((visibleTop - top) / height);
	const auto v1 = static_cast<float>((visibleBottom - top) / height);

	/* Vertices in the order libobs builds its own sprites for a triangle strip: tl, tr, bl, br. */
	gs_render_start(false);
	gs_texcoord(0.0f, v0, 0);
	gs_vertex2f(x0, y0);
	gs_texcoord(1.0f, v0, 0);
	gs_vertex2f(x1, y0);
	gs_texcoord(0.0f, v1, 0);
	gs_vertex2f(x0, y1);
	gs_texcoord(1.0f, v1, 0);
	gs_vertex2f(x1, y1);
	gs_render_stop(GS_TRISTRIP);
}

/*
 * Draws one strip tile at its own size, clipped to the canvas.
 *
 * The tile is the full width of the strip and starts at its left edge; everything interesting
 * about the clipping is in drawClipped.
 */
void drawTile(gs_texture_t *texture, double top, int canvasHeight)
{
	drawClipped(0.0, top, static_cast<double>(gs_texture_get_width(texture)),
		    static_cast<double>(gs_texture_get_height(texture)), canvasHeight);
}

/*
 * Puts this frame of every animated logo that is on screen on the GPU.
 *
 * Nothing is uploaded for a logo that is not: a roll may place more animated logos than are ever
 * visible at once, and the ones the viewer reaches in the last minute of a ten-minute roll have no
 * business holding VRAM from the start.
 *
 * Graphics thread only; requires an active graphics context, and must not be called from inside a
 * technique's pass -- see the ordering in videoRender.
 */
void prepareAnimatedLogos(CreditsSourceData *data, double stripTop, int canvasHeight)
{
	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		if (!runtime.placement.animation)
			continue;

		const QRectF &rect = runtime.placement.rect;
		const double top = stripTop + rect.top();
		if (top >= canvasHeight || top + rect.height() <= 0.0)
			continue;

		uploadFrame(runtime);
	}
}

/*
 * Draws the animated logos over the strip.
 *
 * Each one goes where the layout put it, offset by however far the roll has traveled, into the
 * hole the strip left for it -- so an animated logo scrolls with the text beside it rather than
 * following it a frame later, because both are placed from the same offset in the same frame.
 *
 * Drawn after every tile rather than interleaved with them, which is what keeps a logo whose box
 * straddles a tile seam from being cut in half by the tile that comes after it.
 */
void drawAnimatedLogos(CreditsSourceData *data, gs_eparam_t *imageParam, double stripTop, int canvasHeight)
{
	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		/* Uploaded before the pass this is drawn in; see prepareAnimatedLogos. */
		if (!runtime.texture)
			continue;

		const QRectF &rect = runtime.placement.rect;
		const double top = stripTop + rect.top();
		if (top >= canvasHeight || top + rect.height() <= 0.0)
			continue;

		if (runtime.shadowTexture) {
			const QPointF at = rect.topLeft() + runtime.placement.shadowOffset;
			gs_effect_set_texture(imageParam, runtime.shadowTexture);
			drawClipped(at.x(), stripTop + at.y(),
				    static_cast<double>(gs_texture_get_width(runtime.shadowTexture)),
				    static_cast<double>(gs_texture_get_height(runtime.shadowTexture)), canvasHeight);
		}

		gs_effect_set_texture(imageParam, runtime.texture);
		drawClipped(rect.left(), top, rect.width(), rect.height(), canvasHeight);
	}
}

/*
 * The one shader in the plugin: libobs' own textured quad with an opacity on it.
 *
 * A sticky block whose entrance waits for the roll fades up in place, and the base effect draws a
 * texture exactly as it is. Multiplying the alpha rather than the whole color is what makes the
 * fade a fade rather than a fade to black: the pictures are straight alpha, blended the same way
 * the tiles are, so scaling the channel the blend reads is the whole of the change.
 *
 * Written out here rather than shipped as a .effect file because it is thirty lines and because a
 * data file is one more thing that has to be found at runtime on three platforms.
 */
const char *const kFadeEffectSource = R"(
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

float4 PSFade(VertData vert_in) : TARGET
{
	float4 rgba = image.Sample(textureSampler, vert_in.uv);
	rgba.a *= alpha;
	return rgba;
}

technique Draw
{
	pass
	{
		vertex_shader = VSDefault(vert_in);
		pixel_shader  = PSFade(vert_in);
	}
}
)";

/*
 * The fade shader, compiled on first use. Returns null when the device would not compile it, and
 * only tries once: a shader that failed to compile will not compile on the next frame either, and a
 * compiler error logged sixty times a second is a log nobody can read.
 *
 * Graphics thread only; requires an active graphics context.
 */
gs_effect_t *stickyFadeEffect(CreditsSourceData *data)
{
	if (data->fadeEffectTried)
		return data->fadeEffect;

	data->fadeEffectTried = true;

	char *errors = nullptr;
	data->fadeEffect = gs_effect_create(kFadeEffectSource, "closing-time-sticky-fade.effect", &errors);
	if (!data->fadeEffect)
		obs_log(LOG_ERROR, "could not compile the sticky block fade shader: %s",
			errors ? errors : "no error reported");

	bfree(errors);
	return data->fadeEffect;
}

/*
 * Puts a sticky block on the GPU before the frame it is wanted in.
 *
 * A block's picture is the canvas over, and allocating it in the frame it first becomes visible
 * puts several megabytes of upload into exactly the moment a closing card is arriving on screen --
 * the one place in the roll where a dropped frame is most likely to be noticed. So the upload
 * happens while the block is still a screenful below the frame, and the frame it arrives in has
 * nothing to do but draw it.
 *
 * A block is still uploaded on the spot if it turns up already on screen, which is what a scrub or
 * a canvas resize does; the lookahead is an optimization rather than a precondition of drawing.
 *
 * Graphics thread only; requires an active graphics context, and must not be called from inside a
 * technique's pass -- see the ordering in videoRender.
 */
void prepareStickyBlocks(CreditsSourceData *data, double stripTop, int canvasHeight)
{
	for (StickyBlockRuntime &runtime : data->stickyBlocks) {
		if (runtime.texture || runtime.placement.image.isNull())
			continue;

		const double top = stickyBlockTop(runtime, stripTop, canvasHeight);
		if (runtime.placement.pictureTop(top) >= canvasHeight * 2.0 || runtime.placement.clearedFrame(top))
			continue;

		runtime.texture = createTexture(runtime.placement.image, 0);
		if (!runtime.texture)
			obs_log(LOG_ERROR, "failed to allocate a %dx%d sticky block texture",
				runtime.placement.image.width(), runtime.placement.image.height());

		/*
		 * Dropped either way, for the reason a tile's is: it is on the GPU, or it is what says
		 * the failure has already been reported.
		 */
		runtime.placement.image = QImage();
	}
}

/*
 * Draws one sticky block's picture where the block currently is.
 *
 * The picture reaches past the slot by its margin at each end -- the backdrop's padding and whatever
 * the children paint outside their own boxes -- so it is drawn from there rather than from the slot.
 */
void drawStickyBlock(const StickyBlockRuntime &runtime, double top, int canvasHeight)
{
	drawClipped(0.0, top - runtime.placement.margin, static_cast<double>(gs_texture_get_width(runtime.texture)),
		    static_cast<double>(gs_texture_get_height(runtime.texture)), canvasHeight);
}

/*
 * Draws the sticky blocks over the strip.
 *
 * After the tiles and after the animated logos, because a pinned block is the thing the roll is
 * running behind: it is on top of everything else by construction, which is what makes a closing
 * card readable while the last of the credits goes past underneath it.
 *
 * In a pass of its own rather than in the one the tiles are drawn from, because a block can be
 * part-way through fading up and the base effect has no opacity to set. The fade shader is asked
 * for once here; if the graphics device would not compile it the base effect is used instead and
 * every block is drawn solid, which loses the fade and nothing else.
 */
void drawStickyBlocks(CreditsSourceData *data, double stripTop, int canvasHeight)
{
	if (data->stickyBlocks.empty())
		return;

	gs_effect_t *fade = stickyFadeEffect(data);
	gs_effect_t *effect = fade ? fade : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");
	gs_eparam_t *alphaParam = fade ? gs_effect_get_param_by_name(effect, "alpha") : nullptr;

	while (gs_effect_loop(effect, "Draw")) {
		for (const StickyBlockRuntime &runtime : data->stickyBlocks) {
			/* Uploaded before the pass this is drawn in; see prepareStickyBlocks. */
			if (!runtime.texture || !runtime.showing())
				continue;

			const double top = stickyBlockTop(runtime, stripTop, canvasHeight);
			if (runtime.placement.offFrame(top, canvasHeight))
				continue;

			const double alpha = runtime.alpha();
			/* Nothing to draw and nothing to blend: the first frame of a fade from nothing. */
			if (alpha <= 0.0)
				continue;

			gs_effect_set_texture(imageParam, runtime.texture);
			if (alphaParam)
				gs_effect_set_float(alphaParam, static_cast<float>(alpha));

			drawStickyBlock(runtime, top, canvasHeight);
		}
	}
}

void videoRender(void *raw, gs_effect_t *)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	adoptPendingStrip(data);

	const int canvasWidth = std::max(1, data->document.width);
	const int canvasHeight = std::max(1, data->document.height);

	drawBackground(data->document.background, canvasWidth, canvasHeight);

	if (data->tiles.empty())
		return;

	double offset = 0.0;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		offset = data->offset;
	}

	/*
	 * The strip's top edge sits one canvas-height below the top of the frame at offset 0
	 * and travels upward, so the start of the roll enters from the bottom of the canvas.
	 */
	const double stripTop = canvasHeight - offset;

	/*
	 * Everything this frame draws goes on the GPU before the pass that draws it is started,
	 * rather than being allocated part-way through one: a technique's pass is a poor place to
	 * create a resource, and gs_effect_loop may run its body more than once.
	 */
	const TileRange visible = visibleTiles(data, stripTop, canvasHeight);
	bool uploaded = false;
	for (size_t i = visible.first; i < visible.last; ++i) {
		uploaded = uploaded || !data->tiles[i].image.isNull();
		uploadTile(data, i);
	}

	/*
	 * One tile a frame between the two of them. A tile that had to go up this frame because it is
	 * being drawn has already had the frame's share of the upload, and putting a second one up
	 * behind it would be doing exactly what spreading them out is for avoiding.
	 */
	if (!uploaded)
		uploadTileAhead(data, visible.last);

	prepareAnimatedLogos(data, stripTop, canvasHeight);
	prepareStickyBlocks(data, stripTop, canvasHeight);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw")) {
		for (size_t i = visible.first; i < visible.last; ++i) {
			gs_texture_t *texture = data->tiles[i].texture;
			if (!texture)
				continue;

			/*
			 * Rather than clipping with a scissor rect, which lives in screen space and
			 * would fight the scene item's transform, each tile is drawn as the part of
			 * itself that actually falls inside the canvas.
			 */
			gs_effect_set_texture(imageParam, texture);
			drawTile(texture, stripTop + data->tiles[i].top, canvasHeight);
		}

		drawAnimatedLogos(data, imageParam, stripTop, canvasHeight);
	}

	/* After the loop above rather than inside it: the blocks are drawn through a pass of their own. */
	drawStickyBlocks(data, stripTop, canvasHeight);

	gs_blend_state_pop();
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
	creditsSourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
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
