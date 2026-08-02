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

#include "model/EndingAction.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <plugin-support.h>

#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

struct IdName {
	EndingActionType type;
	const char *id;
	const char *name;
};

const IdName kEndingActions[] = {
	{EndingActionType::None, "none", "Do Nothing"},
	{EndingActionType::SwitchScene, "switch_scene", "Switch Scene"},
	{EndingActionType::StopRecording, "stop_recording", "Stop Recording"},
	{EndingActionType::StopStreaming, "stop_streaming", "Stop Streaming"},
	{EndingActionType::StopVirtualCam, "stop_virtualcam", "Stop Virtual Camera"},
	{EndingActionType::HideSelf, "hide_self", "Hide This Source"},
	{EndingActionType::RestartRoll, "restart", "Restart The Roll"},
	{EndingActionType::FireHotkey, "fire_hotkey", "Trigger A Hotkey"},
	{EndingActionType::SetFilterEnabled, "set_filter_enabled", "Toggle A Filter"},
};

/*
 * Deferred payload for the UI task queue. The credits source may be destroyed between the
 * roll ending and the task running, so the source is held by a strong reference.
 */
struct DeferredAction {
	EndingActionConfig config;
	OBSSourceAutoRelease self;
};

bool hideMatchingItems(obs_scene_t * /*scene*/, obs_sceneitem_t *item, void *param)
{
	auto *target = static_cast<obs_source_t *>(param);

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, hideMatchingItems, param);

	if (obs_sceneitem_get_source(item) == target)
		obs_sceneitem_set_visible(item, false);

	return true;
}

void hideSelf(obs_source_t *self)
{
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	if (!current)
		return;

	obs_scene_t *scene = obs_scene_from_source(current);
	if (!scene)
		return;

	obs_scene_enum_items(scene, hideMatchingItems, self);
}

void switchScene(const QString &sceneName)
{
	if (sceneName.isEmpty())
		return;

	OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.toUtf8().constData());
	if (!scene) {
		obs_log(LOG_WARNING, "ending action: scene '%s' not found", sceneName.toUtf8().constData());
		return;
	}

	obs_frontend_set_current_scene(scene);
}

void setFilterEnabled(const EndingActionConfig &config)
{
	if (config.filterSourceName.isEmpty() || config.filterName.isEmpty())
		return;

	OBSSourceAutoRelease target = obs_get_source_by_name(config.filterSourceName.toUtf8().constData());
	if (!target) {
		obs_log(LOG_WARNING, "ending action: source '%s' not found",
			config.filterSourceName.toUtf8().constData());
		return;
	}

	OBSSourceAutoRelease filter = obs_source_get_filter_by_name(target, config.filterName.toUtf8().constData());
	if (!filter) {
		obs_log(LOG_WARNING, "ending action: filter '%s' not found on '%s'",
			config.filterName.toUtf8().constData(), config.filterSourceName.toUtf8().constData());
		return;
	}

	switch (config.filterMode) {
	case FilterToggleMode::Enable:
		obs_source_set_enabled(filter, true);
		break;
	case FilterToggleMode::Disable:
		obs_source_set_enabled(filter, false);
		break;
	case FilterToggleMode::Toggle:
		obs_source_set_enabled(filter, !obs_source_enabled(filter));
		break;
	}
}

struct HotkeySearch {
	const char *name;
	const char *partner;
	obs_hotkey_id id;
};

bool matchHotkey(void *param, obs_hotkey_id id, obs_hotkey_t *hotkey)
{
	auto *search = static_cast<HotkeySearch *>(param);

	if (strcmp(obs_hotkey_get_name(hotkey), search->name) != 0)
		return true;

	/*
	 * Source-registered hotkeys share names across sources ("Show"/"Hide" on every scene
	 * item, for instance), so when a partner was recorded it has to match too.
	 */
	if (search->partner && *search->partner) {
		if (obs_hotkey_get_registerer_type(hotkey) != OBS_HOTKEY_REGISTERER_SOURCE)
			return true;

		auto *registerer = static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(hotkey));
		OBSSourceAutoRelease owner = obs_weak_source_get_source(registerer);
		if (!owner || strcmp(obs_source_get_name(owner), search->partner) != 0)
			return true;
	}

	search->id = id;
	return false;
}

void fireHotkey(const EndingActionConfig &config)
{
	if (config.hotkeyName.isEmpty())
		return;

	const QByteArray name = config.hotkeyName.toUtf8();
	const QByteArray partner = config.hotkeyPartner.toUtf8();

	HotkeySearch search{name.constData(), partner.constData(), OBS_INVALID_HOTKEY_ID};
	obs_enum_hotkeys(matchHotkey, &search);

	if (search.id == OBS_INVALID_HOTKEY_ID) {
		obs_log(LOG_WARNING, "ending action: hotkey '%s' not found", name.constData());
		return;
	}

	/*
	 * The OBS frontend enables callback rerouting, so a hotkey's own callback only runs
	 * when something feeds it back in through this entry point. Press and release are
	 * both delivered because toggle-style hotkeys act on one edge and reset on the other.
	 */
	obs_hotkey_trigger_routed_callback(search.id, true);
	obs_hotkey_trigger_routed_callback(search.id, false);
}

void restartRoll(obs_source_t *self)
{
	proc_handler_t *handler = obs_source_get_proc_handler(self);
	if (!handler)
		return;

	calldata_t data{};
	calldata_init(&data);
	proc_handler_call(handler, "restart", &data);
	calldata_free(&data);
}

void runOnUiThread(void *param)
{
	auto *deferred = static_cast<DeferredAction *>(param);
	const EndingActionConfig &config = deferred->config;

	switch (config.type) {
	case EndingActionType::None:
		break;
	case EndingActionType::SwitchScene:
		switchScene(config.sceneName);
		break;
	case EndingActionType::StopRecording:
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
		break;
	case EndingActionType::StopStreaming:
		if (obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
		break;
	case EndingActionType::StopVirtualCam:
		if (obs_frontend_virtualcam_active())
			obs_frontend_stop_virtualcam();
		break;
	case EndingActionType::HideSelf:
		if (deferred->self)
			hideSelf(deferred->self);
		break;
	case EndingActionType::RestartRoll:
		if (deferred->self)
			restartRoll(deferred->self);
		break;
	case EndingActionType::FireHotkey:
		fireHotkey(config);
		break;
	case EndingActionType::SetFilterEnabled:
		setFilterEnabled(config);
		break;
	}

	delete deferred;
}

} // namespace

const char *endingActionId(EndingActionType type)
{
	for (const auto &entry : kEndingActions) {
		if (entry.type == type)
			return entry.id;
	}
	return "none";
}

EndingActionType endingActionFromId(const char *id, EndingActionType fallback)
{
	if (!id)
		return fallback;

	for (const auto &entry : kEndingActions) {
		if (strcmp(entry.id, id) == 0)
			return entry.type;
	}
	return fallback;
}

const QVector<EndingActionType> &allEndingActionTypes()
{
	static const QVector<EndingActionType> types = [] {
		QVector<EndingActionType> result;
		result.reserve(static_cast<int>(std::size(kEndingActions)));
		for (const auto &entry : kEndingActions)
			result.append(entry.type);
		return result;
	}();
	return types;
}

const char *endingActionName(EndingActionType type)
{
	for (const auto &entry : kEndingActions) {
		if (entry.type == type)
			return entry.name;
	}
	return "Do Nothing";
}

const char *filterToggleModeId(FilterToggleMode mode)
{
	switch (mode) {
	case FilterToggleMode::Disable:
		return "disable";
	case FilterToggleMode::Toggle:
		return "toggle";
	case FilterToggleMode::Enable:
	default:
		return "enable";
	}
}

FilterToggleMode filterToggleModeFromId(const char *id, FilterToggleMode fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "disable") == 0)
		return FilterToggleMode::Disable;
	if (strcmp(id, "toggle") == 0)
		return FilterToggleMode::Toggle;
	if (strcmp(id, "enable") == 0)
		return FilterToggleMode::Enable;
	return fallback;
}

QString EndingActionConfig::encodeHotkey(const QString &name, const QString &partner)
{
	if (name.isEmpty())
		return QString();
	if (partner.isEmpty())
		return name;
	return name + QLatin1Char('\n') + partner;
}

void EndingActionConfig::decodeHotkey(const QString &encoded, QString *name, QString *partner)
{
	const int split = encoded.indexOf(QLatin1Char('\n'));
	if (split < 0) {
		*name = encoded;
		partner->clear();
		return;
	}

	*name = encoded.left(split);
	*partner = encoded.mid(split + 1);
}

void EndingActionConfig::save(obs_data_t *data) const
{
	obs_data_set_string(data, "ea_type", endingActionId(type));
	obs_data_set_double(data, "ea_delay", delay);
	obs_data_set_string(data, "ea_scene", sceneName.toUtf8().constData());
	obs_data_set_string(data, "ea_filter_source", filterSourceName.toUtf8().constData());
	obs_data_set_string(data, "ea_filter_name", filterName.toUtf8().constData());
	obs_data_set_string(data, "ea_filter_mode", filterToggleModeId(filterMode));
	obs_data_set_string(data, "ea_hotkey", encodeHotkey(hotkeyName, hotkeyPartner).toUtf8().constData());
}

void EndingActionConfig::load(obs_data_t *data)
{
	type = endingActionFromId(obs_data_get_string(data, "ea_type"), EndingActionType::None);
	delay = obs_data_get_double(data, "ea_delay");
	sceneName = QString::fromUtf8(obs_data_get_string(data, "ea_scene"));
	filterSourceName = QString::fromUtf8(obs_data_get_string(data, "ea_filter_source"));
	filterName = QString::fromUtf8(obs_data_get_string(data, "ea_filter_name"));
	filterMode = filterToggleModeFromId(obs_data_get_string(data, "ea_filter_mode"), FilterToggleMode::Enable);

	decodeHotkey(QString::fromUtf8(obs_data_get_string(data, "ea_hotkey")), &hotkeyName, &hotkeyPartner);

	if (delay < 0.0)
		delay = 0.0;
}

void EndingActionConfig::defaults(obs_data_t *data)
{
	obs_data_set_default_string(data, "ea_type", "none");
	obs_data_set_default_double(data, "ea_delay", 0.0);
	obs_data_set_default_string(data, "ea_filter_mode", "enable");
}

void EndingActionConfig::execute(obs_source_t *self) const
{
	auto *deferred = new DeferredAction();
	deferred->config = *this;
	deferred->self = obs_source_get_ref(self);

	/*
	 * Ending actions are decided on the graphics thread but touch the frontend and the
	 * scene graph, so they are always handed to the UI queue rather than run in place.
	 */
	obs_queue_task(OBS_TASK_UI, runOnUiThread, deferred, false);
}

void emitCreditsFinished(obs_source_t *self)
{
	if (!self)
		return;

	calldata_t data{};
	calldata_init(&data);
	calldata_set_ptr(&data, "source", self);

	if (signal_handler_t *sourceSignals = obs_source_get_signal_handler(self))
		signal_handler_signal(sourceSignals, "credits_finished", &data);

	if (signal_handler_t *globalSignals = obs_get_signal_handler())
		signal_handler_signal(globalSignals, "closing_time_finished", &data);

	calldata_free(&data);
}

} // namespace closingtime
