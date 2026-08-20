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

#include <QSet>
#include <QString>

#include <algorithm>
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
	/* What the last rebuild was rasterised from; see renderKey(). Graphics thread only. */
	QString renderedFrom;
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
	bool hasPendingStrip = false;
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

	/* Graphics-thread state. */
	std::vector<gs_texture_t *> tileTextures;
	std::vector<int> tileTops;
	std::vector<AnimatedLogoRuntime> animatedLogos;
	int stripHeight = 0;

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
 * Everything the strip is rasterised from, as a string two documents can be compared by.
 *
 * Playback settings reach the source through the same update() every content edit does, and
 * scrubbing moves a slider that fires one per frame of the drag. Rasterising a long roll on each
 * of those would keep the render thread busy for the whole gesture and hand back a stream of
 * strips identical to the one already on the GPU, so a rebuild is queued only when this changes.
 *
 * Built by blanking the fields that move the finished strip rather than by listing the ones that
 * make it: a field added later counts towards the key by default, which costs an unnecessary
 * rebuild if it turns out to be a playback setting and never a stale one if it is not. Lead-in
 * and lead-out are deliberately not blanked -- they are baked into the strip as blank space.
 */
QString renderKey(const Document &document)
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
	 * megabytes: serialising them here would cost more than the rebuild it exists to avoid.
	 * What is left still names every family and every file, which is what a bundle changing
	 * actually looks like.
	 */
	for (BundledFont &font : content.bundledFonts)
		font.data = QByteArray::number(font.data.size());

	return content.toJson();
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
	 * Rasterisation is long enough to be seen if it happens on the thread drawing OBS's
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
		data->hasPendingStrip = true;
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
	for (gs_texture_t *texture : data->tileTextures)
		gs_texture_destroy(texture);

	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		if (runtime.texture)
			gs_texture_destroy(runtime.texture);
		if (runtime.shadowTexture)
			gs_texture_destroy(runtime.shadowTexture);
	}

	data->tileTextures.clear();
	data->tileTops.clear();
	data->animatedLogos.clear();
	data->stripHeight = 0;
}

/* Graphics thread only; requires an active graphics context. */
gs_texture_t *createTexture(const QImage &image)
{
	if (image.isNull())
		return nullptr;

	const uint8_t *bits = image.constBits();
	return gs_texture_create(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()), GS_BGRA,
				 1, &bits, GS_DYNAMIC);
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
		runtime.texture = createTexture(image);
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
			runtime.shadowTexture = createTexture(shadow);
		else
			gs_texture_set_image(runtime.shadowTexture, shadow.constBits(),
					     static_cast<uint32_t>(shadow.bytesPerLine()), false);
	}

	runtime.uploadedFrame = runtime.frame;
}

/* Graphics thread only; requires an active graphics context. */
void uploadPendingStrip(CreditsSourceData *data)
{
	Strip strip;
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (!data->hasPendingStrip)
			return;

		strip = std::move(data->pendingStrip);
		data->pendingStrip = Strip();
		data->hasPendingStrip = false;
	}

	releaseTextures(data);
	data->stripHeight = strip.height;

	data->animatedLogos.reserve(strip.animatedLogos.size());
	for (AnimatedLogoPlacement &placement : strip.animatedLogos) {
		AnimatedLogoRuntime runtime;
		runtime.placement = std::move(placement);
		/*
		 * Textures are left unallocated until the logo is first drawn. A roll may place more
		 * animated logos than are ever on screen at once, and the ones the viewer scrolls past
		 * in the last minute of a ten-minute roll have no business holding VRAM from the start.
		 */
		data->animatedLogos.push_back(std::move(runtime));
	}

	for (const StripTile &tile : strip.tiles) {
		const uint8_t *bits = tile.image.constBits();
		gs_texture_t *texture = gs_texture_create(static_cast<uint32_t>(tile.image.width()),
							  static_cast<uint32_t>(tile.image.height()), GS_BGRA, 1, &bits,
							  0);
		if (!texture) {
			obs_log(LOG_ERROR, "failed to allocate a %dx%d credit strip texture", tile.image.width(),
				tile.image.height());
			continue;
		}

		data->tileTextures.push_back(texture);
		data->tileTops.push_back(tile.top);
	}
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
	 * Only a change to what the strip is made of is worth rasterising again -- see renderKey().
	 * Everything else here arrives through the same call: a scrub sends one per frame of the
	 * drag, and rebuilding for those would keep the render thread busy producing strips
	 * identical to the one already uploaded.
	 */
	const QString key = renderKey(data->document);
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
	 * parked for editing is exactly the one somebody is restyling. The poll costs one stat a
	 * second at most, and a rebuild is only queued when a style bound to this document moved.
	 */
	if (StyleLibrary::instance().pollForChanges() && data->document.refreshLinkedPresets()) {
		queueRebuild(data);
		/*
		 * Saved as well as redrawn. The refreshed copy is this roll's fallback on a machine
		 * without the library, and a rename it just followed has to survive a restart -- both
		 * of which mean the settings, not just the strip.
		 */
		writeDocumentBack(data);
	}

	/*
	 * Re-applied every tick rather than once when the setting changes, because the position it
	 * resolves to depends on the strip: a rebuild finishing, or a canvas resize, changes the
	 * travel underneath it, and a roll parked halfway through should stay halfway through.
	 */
	if (data->document.manualScroll) {
		scrubTo(data, data->document.scrollPosition);
		advanceAnimatedLogos(data, 0.0, false);
		return;
	}

	const double delta = tickSeconds(seconds);
	advance(data, delta);

	bool rolling = false;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		rolling = data->phase == Phase::Rolling && !data->paused;
	}

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
 * Draws the animated logos over the strip.
 *
 * Each one goes where the layout put it, offset by however far the roll has travelled, into the
 * hole the strip left for it -- so an animated logo scrolls with the text beside it rather than
 * following it a frame later, because both are placed from the same offset in the same frame.
 *
 * Drawn after every tile rather than interleaved with them, which is what keeps a logo whose box
 * straddles a tile seam from being cut in half by the tile that comes after it.
 */
void drawAnimatedLogos(CreditsSourceData *data, gs_eparam_t *imageParam, double stripTop, int canvasHeight)
{
	for (AnimatedLogoRuntime &runtime : data->animatedLogos) {
		if (!runtime.placement.animation)
			continue;

		const QRectF &rect = runtime.placement.rect;
		const double top = stripTop + rect.top();

		/* Nothing is uploaded for a logo that is not on screen; see uploadPendingStrip. */
		if (top >= canvasHeight || top + rect.height() <= 0.0)
			continue;

		uploadFrame(runtime);
		if (!runtime.texture)
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

void videoRender(void *raw, gs_effect_t *)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	uploadPendingStrip(data);

	const int canvasWidth = std::max(1, data->document.width);
	const int canvasHeight = std::max(1, data->document.height);

	drawBackground(data->document.background, canvasWidth, canvasHeight);

	if (data->tileTextures.empty())
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

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw")) {
		for (size_t i = 0; i < data->tileTextures.size(); ++i) {
			gs_texture_t *texture = data->tileTextures[i];

			/*
			 * Rather than clipping with a scissor rect, which lives in screen space and
			 * would fight the scene item's transform, each tile is drawn as the part of
			 * itself that actually falls inside the canvas.
			 */
			gs_effect_set_texture(imageParam, texture);
			drawTile(texture, stripTop + data->tileTops[i], canvasHeight);
		}

		drawAnimatedLogos(data, imageParam, stripTop, canvasHeight);
	}

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
