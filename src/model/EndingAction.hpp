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

#include <obs.h>

#include <QString>
#include <QVector>

#include <memory>
#include <mutex>
#include <vector>

namespace closingtime {

/*
 * What happens once the last pixel of the credit strip clears the top of the canvas.
 *
 * Whichever action is selected, the source additionally emits its "credits_finished"
 * signal and an equivalent frontend event, so scripts and other plugins can react even
 * when the action is None. Ending actions never fire while Loop is enabled -- a looping
 * roll has no end.
 */
enum class EndingActionType {
	None,
	SwitchScene,
	StopRecording,
	StopStreaming,
	StopVirtualCam,
	HideSelf,
	RestartRoll,
	FireHotkey,
	SetFilterEnabled,
	/*
	 * Shows, hides or toggles a source's scene items -- in one scene, or in every scene that holds
	 * it. What "a background source becomes active" is, said in OBS's own terms.
	 */
	SetSourceVisible,
};

const char *endingActionId(EndingActionType type);
EndingActionType endingActionFromId(const char *id, EndingActionType fallback = EndingActionType::None);
const char *endingActionName(EndingActionType type);

/* Every action type, in the order the properties combo box should list them. */
const QVector<EndingActionType> &allEndingActionTypes();

enum class FilterToggleMode { Enable, Disable, Toggle };

const char *filterToggleModeId(FilterToggleMode mode);
FilterToggleMode filterToggleModeFromId(const char *id, FilterToggleMode fallback = FilterToggleMode::Enable);

class ActionJournal;

/*
 * One action: what to do, and the settings each kind of action reads. The same thing serves the
 * source's ending action and every action an Action section carries -- a roll can do mid-way through
 * anything it can do at the end, and the other way round.
 */
struct EndingActionConfig {
	EndingActionType type = EndingActionType::None;

	/* Seconds to wait after the roll finishes before the action fires. */
	double delay = 0.0;

	/* SwitchScene */
	QString sceneName;

	/* SetFilterEnabled */
	QString filterSourceName;
	QString filterName;
	FilterToggleMode filterMode = FilterToggleMode::Enable;

	/*
	 * FireHotkey. `hotkeyName` is the internal registration name, which is stable across
	 * locales; `hotkeyPartner` is the name of the source that registered it, used to pick
	 * the right one when several sources register the same hotkey name.
	 */
	QString hotkeyName;
	QString hotkeyPartner;

	/*
	 * SetSourceVisible. `itemSceneName` empty means every scene that holds the source, groups and
	 * nested scenes included, the way HideSelf finds the roll itself.
	 */
	QString itemSceneName;
	QString itemSourceName;
	FilterToggleMode itemMode = FilterToggleMode::Enable;

	/*
	 * Ending-action fields live as "ea_"-prefixed keys on the source settings rather than
	 * in a nested object, so that obs_properties can bind straight to them instead of the
	 * source having to marshal a sub-object back and forth on every properties edit.
	 */
	void save(obs_data_t *data) const { save(data, "ea_"); }
	void load(obs_data_t *data) { load(data, "ea_"); }
	static void defaults(obs_data_t *data);

	/*
	 * The same fields under keys with another prefix -- none at all for an action stored as an
	 * object of its own in an Action section's list.
	 */
	void save(obs_data_t *data, const char *prefix) const;
	void load(obs_data_t *data, const char *prefix);

	/*
	 * The hotkey picker is a single combo box, so the registration name and the owning
	 * source name travel together as one newline-separated value. Hotkey names never
	 * contain newlines, which makes the split unambiguous.
	 */
	static QString encodeHotkey(const QString &name, const QString &partner);
	static void decodeHotkey(const QString &encoded, QString *name, QString *partner);

	/*
	 * Runs the configured action immediately. Safe to call from the graphics thread:
	 * anything that touches the frontend or the scene graph is deferred onto the UI task
	 * queue. `self` is the credits source, needed by HideSelf and RestartRoll.
	 *
	 * `delay` is not applied here -- the source counts it down and calls this once it has
	 * elapsed, so that a delayed action still cancels cleanly if the roll is reset first.
	 */
	void execute(obs_source_t *self) const { execute(self, nullptr); }

	/*
	 * Runs the action and, when `journal` is given, notes down what it changed that can be put back
	 * -- a filter's enabled state, a scene item's visibility -- so that the journal can undo it
	 * later. See ActionJournal.
	 */
	void execute(obs_source_t *self, const std::shared_ptr<ActionJournal> &journal) const;

	/* True for the kinds of action an ActionJournal can put back. */
	static bool isRevertible(EndingActionType type);

	/* A one-line description for the designer's section list, e.g. "Show Background". */
	QString summary() const;

	bool operator==(const EndingActionConfig &other) const = default;
};

/*
 * What an Action section's actions changed, so that a restart can put it back.
 *
 * Filled on the UI thread, by the actions themselves as they run -- the state worth restoring is the
 * state just before the action, and only the action knows when that is. Reverted on the UI thread
 * too, through the same queue the actions are run from, so a revert asked for after an action was
 * queued always sees what that action did: the queue runs in order.
 *
 * Shared, because the tasks that write to it can outlive the source that asked for them.
 */
class ActionJournal {
public:
	/* A filter's enabled state, or a scene item's visibility, as it was before an action changed it. */
	struct Entry {
		enum class Kind { Filter, SceneItem };
		Kind kind = Kind::Filter;
		/* The filter itself, or the scene the item sits in (a group's own scene for an item in one). */
		obs_weak_source_t *target = nullptr;
		int64_t itemId = 0;
		bool previous = false;
	};

	ActionJournal() = default;
	~ActionJournal();
	ActionJournal(const ActionJournal &) = delete;
	ActionJournal &operator=(const ActionJournal &) = delete;

	/* Takes its own reference to `target`. UI thread. */
	void record(Entry::Kind kind, obs_source_t *target, int64_t itemId, bool previous);

	/*
	 * Puts back everything recorded, newest first -- so a thing changed twice ends up as it was
	 * before the first change -- and forgets it. Safe from any thread: the work is queued onto the
	 * UI thread behind any action still waiting there.
	 */
	void revert(const std::shared_ptr<ActionJournal> &self);

	/* Does the reverting. UI thread. */
	void revertNow();

	bool empty() const;

private:
	mutable std::mutex mutex;
	std::vector<Entry> entries;
};

/* Emits the "credits_finished" signal on `self` and the matching frontend event. */
void emitCreditsFinished(obs_source_t *self);

} // namespace closingtime
