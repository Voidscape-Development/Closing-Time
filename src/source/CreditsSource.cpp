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

#include <QString>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "model/CreditsModel.hpp"
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
 * Threading:
 *
 *   - `document` is written only by update(), which libobs defers to the graphics thread
 *     for video sources, and by create() before the source is visible to anyone else. The
 *     graphics thread can therefore read it without a lock.
 *   - Playback state is mutated from hotkey and proc-handler callbacks on the UI thread as
 *     well as from video_tick, so it lives behind `stateMutex`.
 *   - The rendered strip crosses from the UI thread to the graphics thread through
 *     `pendingStrip` under `handoffMutex`; the GPU textures themselves are only ever
 *     touched by the graphics thread.
 */
struct CreditsSourceData {
	obs_source_t *source = nullptr;

	Document document;
	/* Owned by the UI thread: only the rebuild task reads or writes it. */
	LogoCache logos;

	std::mutex handoffMutex;
	Strip pendingStrip;
	bool hasPendingStrip = false;
	/* Set while a rebuild is in flight so a burst of edits collapses into one re-render. */
	bool rebuildInFlight = false;
	bool rebuildAgain = false;

	/* Graphics-thread state. */
	std::vector<gs_texture_t *> tileTextures;
	std::vector<int> tileTops;
	int stripHeight = 0;

	std::mutex stateMutex;
	Phase phase = Phase::Idle;
	double offset = 0.0;
	double delayRemaining = 0.0;
	double actionRemaining = 0.0;
	bool actionPending = false;
	bool paused = false;

	obs_hotkey_id startHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id pauseHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id restartHotkey = OBS_INVALID_HOTKEY_ID;
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
};

void rebuildOnUiThread(void *param);

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

	auto *task = new RebuildTask();
	task->weak = obs_source_get_weak_source(data->source);
	task->data = data;
	task->document = data->document;

	/*
	 * QPainter and QFont want a thread with a Qt event loop behind them, and the designer
	 * preview shares the same code path, so all rasterisation happens on the UI thread.
	 */
	obs_queue_task(OBS_TASK_UI, rebuildOnUiThread, task, false);
}

void rebuildOnUiThread(void *param)
{
	const std::unique_ptr<RebuildTask> task(static_cast<RebuildTask *>(param));

	OBSSourceAutoRelease source = obs_weak_source_get_source(task->weak);
	if (!source)
		return;

	CreditsSourceData *data = task->data;

	StripRenderer renderer(&data->logos);
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

	data->tileTextures.clear();
	data->tileTops.clear();
	data->stripHeight = 0;
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

void advance(CreditsSourceData *data, float seconds)
{
	const Document &document = data->document;

	/*
	 * The strip starts one canvas-height below the top of the frame, so the distance it
	 * has to travel before the last pixel clears the top is canvas + strip.
	 */
	const double travel = static_cast<double>(std::max(1, document.height)) + data->stripHeight;

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

void update(void *raw, obs_data_t *settings)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	data->document.load(settings);
	queueRebuild(data);

	/*
	 * Geometry and content edits invalidate the current scroll position, so a roll that is
	 * already running restarts instead of jumping to a stale offset in new content.
	 */
	std::lock_guard<std::mutex> lock(data->stateMutex);
	if (data->phase != Phase::Idle)
		armRollLocked(data, data->document.startDelay);
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

	update(data, settings);
	return data;
}

void destroy(void *raw)
{
	auto *data = static_cast<CreditsSourceData *>(raw);

	obs_hotkey_unregister(data->startHotkey);
	obs_hotkey_unregister(data->pauseHotkey);
	obs_hotkey_unregister(data->restartHotkey);

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

void videoTick(void *raw, float seconds)
{
	advance(static_cast<CreditsSourceData *>(raw), seconds);
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
			const int tileWidth = static_cast<int>(gs_texture_get_width(texture));
			const int tileHeight = static_cast<int>(gs_texture_get_height(texture));
			const double tileTop = stripTop + data->tileTops[i];

			/*
			 * Rather than clipping with a scissor rect, which lives in screen space and
			 * would fight the scene item's transform, each tile is drawn as the
			 * sub-rectangle that actually falls inside the canvas.
			 */
			const int skipTop = static_cast<int>(std::max(0.0, -tileTop));
			const double drawTop = tileTop + skipTop;
			const int visibleHeight = static_cast<int>(
				std::min(static_cast<double>(tileHeight - skipTop), canvasHeight - drawTop));

			if (visibleHeight <= 0 || drawTop >= canvasHeight)
				continue;

			gs_matrix_push();
			gs_matrix_translate3f(0.0f, static_cast<float>(drawTop), 0.0f);
			gs_effect_set_texture(imageParam, texture);
			gs_draw_sprite_subregion(texture, 0, 0, static_cast<uint32_t>(skipTop),
						 static_cast<uint32_t>(tileWidth),
						 static_cast<uint32_t>(visibleHeight));
			gs_matrix_pop();
		}
	}

	gs_blend_state_pop();
}

/* ------------------------------------------------------------------------ properties */

bool onOpenDesigner(obs_properties_t *, obs_property_t *, void *raw)
{
	openDesignerFor(static_cast<CreditsSourceData *>(raw)->source);
	return false;
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
