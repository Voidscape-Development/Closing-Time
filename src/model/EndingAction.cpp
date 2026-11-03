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
#include <string>

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
	{EndingActionType::SetSourceVisible, "set_source_visible", "Show/Hide A Source"},
};

/*
 * Deferred payload for the UI task queue. The credits source may be destroyed between the
 * roll ending and the task running, so the source is held by a strong reference.
 */
struct DeferredAction {
	EndingActionConfig config;
	OBSSourceAutoRelease self;
	/* Null for an ending action, which nothing ever puts back. */
	std::shared_ptr<ActionJournal> journal;
};

bool applyMode(FilterToggleMode mode, bool current)
{
	switch (mode) {
	case FilterToggleMode::Enable:
		return true;
	case FilterToggleMode::Disable:
		return false;
	case FilterToggleMode::Toggle:
	default:
		return !current;
	}
}

bool hideMatchingItems(obs_scene_t * /*scene*/, obs_sceneitem_t *item, void *param)
{
	auto *target = static_cast<obs_source_t *>(param);

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, hideMatchingItems, param);

	if (obs_sceneitem_get_source(item) == target)
		obs_sceneitem_set_visible(item, false);

	return true;
}

/*
 * Hides every scene item backed by `self`, wherever it sits.
 *
 * Walking all scenes rather than just the current one is what makes this work for the
 * arrangements people actually build: a credit roll parked in a nested scene, or the same
 * source placed in several scenes at once. Nesting is covered for free -- a nested scene is
 * itself a scene here -- and groups recurse through the item callback.
 */
void hideSelf(obs_source_t *self)
{
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			if (obs_scene_t *scene = obs_scene_from_source(source))
				obs_scene_enum_items(scene, hideMatchingItems, param);
			return true;
		},
		self);
}

void switchScene(const QString &sceneName)
{
	if (sceneName.isEmpty())
		return;

	OBSSourceAutoRelease scene = obs_get_source_by_name(sceneName.toUtf8().constData());
	if (!scene) {
		obs_log(LOG_WARNING, "action: scene '%s' not found", sceneName.toUtf8().constData());
		return;
	}

	obs_frontend_set_current_scene(scene);
}

void setFilterEnabled(const EndingActionConfig &config, ActionJournal *journal)
{
	if (config.filterSourceName.isEmpty() || config.filterName.isEmpty())
		return;

	OBSSourceAutoRelease target = obs_get_source_by_name(config.filterSourceName.toUtf8().constData());
	if (!target) {
		obs_log(LOG_WARNING, "action: source '%s' not found", config.filterSourceName.toUtf8().constData());
		return;
	}

	OBSSourceAutoRelease filter = obs_source_get_filter_by_name(target, config.filterName.toUtf8().constData());
	if (!filter) {
		obs_log(LOG_WARNING, "action: filter '%s' not found on '%s'", config.filterName.toUtf8().constData(),
			config.filterSourceName.toUtf8().constData());
		return;
	}

	const bool before = obs_source_enabled(filter);
	const bool after = applyMode(config.filterMode, before);
	if (after == before)
		return;

	if (journal)
		journal->record(ActionJournal::Entry::Kind::Filter, filter, 0, before);
	obs_source_set_enabled(filter, after);
}

struct VisibilityChange {
	obs_source_t *target;
	FilterToggleMode mode;
	ActionJournal *journal;
	int matched;
};

bool setMatchingItemsVisible(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	auto *change = static_cast<VisibilityChange *>(param);

	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, setMatchingItemsVisible, param);

	if (obs_sceneitem_get_source(item) != change->target)
		return true;

	++change->matched;
	const bool before = obs_sceneitem_visible(item);
	const bool after = applyMode(change->mode, before);
	if (after == before)
		return true;

	/*
	 * Recorded against the scene the item actually sits in -- a group's own scene for an item inside
	 * one -- because an item id is only unique within that scene.
	 */
	if (change->journal)
		change->journal->record(ActionJournal::Entry::Kind::SceneItem, obs_scene_get_source(scene),
					obs_sceneitem_get_id(item), before);
	obs_sceneitem_set_visible(item, after);
	return true;
}

/*
 * Shows, hides or toggles every scene item backed by the named source: in the named scene, or in
 * every scene when none is named. Groups are walked into either way, since a source inside a group
 * is still in the scene as far as anyone looking at it is concerned.
 */
void setSourceVisible(const EndingActionConfig &config, ActionJournal *journal)
{
	if (config.itemSourceName.isEmpty())
		return;

	OBSSourceAutoRelease target = obs_get_source_by_name(config.itemSourceName.toUtf8().constData());
	if (!target) {
		obs_log(LOG_WARNING, "action: source '%s' not found", config.itemSourceName.toUtf8().constData());
		return;
	}

	VisibilityChange change{target, config.itemMode, journal, 0};

	if (config.itemSceneName.isEmpty()) {
		obs_enum_scenes(
			[](void *param, obs_source_t *source) {
				if (obs_scene_t *scene = obs_scene_from_source(source))
					obs_scene_enum_items(scene, setMatchingItemsVisible, param);
				return true;
			},
			&change);
	} else {
		OBSSourceAutoRelease sceneSource = obs_get_source_by_name(config.itemSceneName.toUtf8().constData());
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
		if (!scene)
			scene = sceneSource ? obs_group_from_source(sceneSource) : nullptr;
		if (!scene) {
			obs_log(LOG_WARNING, "action: scene '%s' not found", config.itemSceneName.toUtf8().constData());
			return;
		}
		obs_scene_enum_items(scene, setMatchingItemsVisible, &change);
	}

	if (change.matched == 0)
		obs_log(LOG_WARNING, "action: source '%s' is not in %s", config.itemSourceName.toUtf8().constData(),
			config.itemSceneName.isEmpty()
				? "any scene"
				: qUtf8Printable(QStringLiteral("scene '%1'").arg(config.itemSceneName)));
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
		obs_log(LOG_WARNING, "action: hotkey '%s' not found", name.constData());
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
		setFilterEnabled(config, deferred->journal.get());
		break;
	case EndingActionType::SetSourceVisible:
		setSourceVisible(config, deferred->journal.get());
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

namespace {

/* `prefix` followed by `key`, for the keys an action is stored under. */
std::string keyOf(const char *prefix, const char *key)
{
	return std::string(prefix ? prefix : "") + key;
}

} // namespace

void EndingActionConfig::save(obs_data_t *data, const char *prefix) const
{
	const auto key = [prefix](const char *name) {
		return keyOf(prefix, name);
	};

	obs_data_set_string(data, key("type").c_str(), endingActionId(type));
	obs_data_set_double(data, key("delay").c_str(), delay);
	obs_data_set_string(data, key("scene").c_str(), sceneName.toUtf8().constData());
	obs_data_set_string(data, key("filter_source").c_str(), filterSourceName.toUtf8().constData());
	obs_data_set_string(data, key("filter_name").c_str(), filterName.toUtf8().constData());
	obs_data_set_string(data, key("filter_mode").c_str(), filterToggleModeId(filterMode));
	obs_data_set_string(data, key("hotkey").c_str(), encodeHotkey(hotkeyName, hotkeyPartner).toUtf8().constData());
	obs_data_set_string(data, key("item_scene").c_str(), itemSceneName.toUtf8().constData());
	obs_data_set_string(data, key("item_source").c_str(), itemSourceName.toUtf8().constData());
	obs_data_set_string(data, key("item_mode").c_str(), filterToggleModeId(itemMode));
}

void EndingActionConfig::load(obs_data_t *data, const char *prefix)
{
	const auto key = [prefix](const char *name) {
		return keyOf(prefix, name);
	};

	type = endingActionFromId(obs_data_get_string(data, key("type").c_str()), EndingActionType::None);
	delay = obs_data_get_double(data, key("delay").c_str());
	sceneName = QString::fromUtf8(obs_data_get_string(data, key("scene").c_str()));
	filterSourceName = QString::fromUtf8(obs_data_get_string(data, key("filter_source").c_str()));
	filterName = QString::fromUtf8(obs_data_get_string(data, key("filter_name").c_str()));
	filterMode =
		filterToggleModeFromId(obs_data_get_string(data, key("filter_mode").c_str()), FilterToggleMode::Enable);

	decodeHotkey(QString::fromUtf8(obs_data_get_string(data, key("hotkey").c_str())), &hotkeyName, &hotkeyPartner);

	itemSceneName = QString::fromUtf8(obs_data_get_string(data, key("item_scene").c_str()));
	itemSourceName = QString::fromUtf8(obs_data_get_string(data, key("item_source").c_str()));
	itemMode =
		filterToggleModeFromId(obs_data_get_string(data, key("item_mode").c_str()), FilterToggleMode::Enable);

	if (delay < 0.0)
		delay = 0.0;
}

void EndingActionConfig::defaults(obs_data_t *data)
{
	obs_data_set_default_string(data, "ea_type", "none");
	obs_data_set_default_double(data, "ea_delay", 0.0);
	obs_data_set_default_string(data, "ea_filter_mode", "enable");
	obs_data_set_default_string(data, "ea_item_mode", "enable");
}

void EndingActionConfig::execute(obs_source_t *self, const std::shared_ptr<ActionJournal> &journal) const
{
	if (type == EndingActionType::None)
		return;

	auto *deferred = new DeferredAction();
	deferred->config = *this;
	deferred->self = obs_source_get_ref(self);
	if (isRevertible(type))
		deferred->journal = journal;

	/*
	 * Actions are decided on the graphics thread but touch the frontend and the scene graph, so
	 * they are always handed to the UI queue rather than run in place.
	 */
	obs_queue_task(OBS_TASK_UI, runOnUiThread, deferred, false);
}

bool EndingActionConfig::isRevertible(EndingActionType type)
{
	/*
	 * HideSelf is deliberately not among them, though it is a visibility change: a roll that hides
	 * itself is reset by being hidden when it starts on show, and a revert that showed it again
	 * would start it again -- straight back into the action that hid it.
	 */
	return type == EndingActionType::SetFilterEnabled || type == EndingActionType::SetSourceVisible;
}

QString EndingActionConfig::summary() const
{
	const auto modeWord = [](FilterToggleMode mode, const char *on, const char *off) {
		switch (mode) {
		case FilterToggleMode::Enable:
			return QString::fromUtf8(on);
		case FilterToggleMode::Disable:
			return QString::fromUtf8(off);
		case FilterToggleMode::Toggle:
		default:
			return QStringLiteral("Toggle");
		}
	};

	switch (type) {
	case EndingActionType::SwitchScene:
		if (!sceneName.isEmpty())
			return QStringLiteral("Switch to %1").arg(sceneName);
		break;
	case EndingActionType::FireHotkey:
		if (!hotkeyName.isEmpty())
			return QStringLiteral("Hotkey %1").arg(hotkeyName);
		break;
	case EndingActionType::SetFilterEnabled:
		if (!filterName.isEmpty())
			return QStringLiteral("%1 %2").arg(modeWord(filterMode, "Enable", "Disable"), filterName);
		break;
	case EndingActionType::SetSourceVisible:
		if (!itemSourceName.isEmpty())
			return QStringLiteral("%1 %2").arg(modeWord(itemMode, "Show", "Hide"), itemSourceName);
		break;
	default:
		break;
	}

	return QString::fromUtf8(endingActionName(type));
}

ActionJournal::~ActionJournal()
{
	for (const Entry &entry : entries)
		obs_weak_source_release(entry.target);
}

void ActionJournal::record(Entry::Kind kind, obs_source_t *target, int64_t itemId, bool previous)
{
	if (!target)
		return;

	Entry entry;
	entry.kind = kind;
	entry.target = obs_source_get_weak_source(target);
	entry.itemId = itemId;
	entry.previous = previous;

	std::lock_guard<std::mutex> lock(mutex);
	entries.push_back(entry);
}

bool ActionJournal::empty() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return entries.empty();
}

void ActionJournal::revert(const std::shared_ptr<ActionJournal> &self)
{
	if (!self)
		return;

	auto *held = new std::shared_ptr<ActionJournal>(self);
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			auto *journal = static_cast<std::shared_ptr<ActionJournal> *>(param);
			(*journal)->revertNow();
			delete journal;
		},
		held, false);
}

void ActionJournal::revertNow()
{
	std::vector<Entry> taken;
	{
		std::lock_guard<std::mutex> lock(mutex);
		taken.swap(entries);
	}

	/* Newest first, so something changed twice ends up as it was before the first change. */
	for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
		OBSSourceAutoRelease target = obs_weak_source_get_source(it->target);
		obs_weak_source_release(it->target);
		if (!target)
			continue;

		switch (it->kind) {
		case Entry::Kind::Filter:
			obs_source_set_enabled(target, it->previous);
			break;
		case Entry::Kind::SceneItem: {
			obs_scene_t *scene = obs_scene_from_source(target);
			if (!scene)
				scene = obs_group_from_source(target);
			if (!scene)
				break;
			if (obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, it->itemId))
				obs_sceneitem_set_visible(item, it->previous);
			break;
		}
		}
	}
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
