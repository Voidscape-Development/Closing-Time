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
};

const char *endingActionId(EndingActionType type);
EndingActionType endingActionFromId(const char *id, EndingActionType fallback = EndingActionType::None);
const char *endingActionName(EndingActionType type);

/* Every action type, in the order the properties combo box should list them. */
const QVector<EndingActionType> &allEndingActionTypes();

enum class FilterToggleMode { Enable, Disable, Toggle };

const char *filterToggleModeId(FilterToggleMode mode);
FilterToggleMode filterToggleModeFromId(const char *id, FilterToggleMode fallback = FilterToggleMode::Enable);

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
	 * Ending-action fields live as "ea_"-prefixed keys on the source settings rather than
	 * in a nested object, so that obs_properties can bind straight to them instead of the
	 * source having to marshal a sub-object back and forth on every properties edit.
	 */
	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
	static void defaults(obs_data_t *data);

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
	void execute(obs_source_t *self) const;
};

/* Emits the "credits_finished" signal on `self` and the matching frontend event. */
void emitCreditsFinished(obs_source_t *self);

} // namespace closingtime
