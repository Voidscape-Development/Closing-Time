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

namespace closingtime {

/*
 * Driving a roll from outside OBS, through obs-websocket.
 *
 * Stream-control tools -- a Stream Deck, a show-control system, a script on another machine -- talk
 * to OBS over obs-websocket, and a roll is otherwise only reachable through hotkeys, which those
 * tools can fire but cannot ask anything of. So the plugin registers a vendor, `closing-time`, with:
 *
 *   Start, Pause, Resume, Restart   { "sourceName": "..." }
 *       What the hotkeys do. Start arms a roll that is parked or over and carries on with one that
 *       is paused.
 *   GetState                        { "sourceName": "..." }
 *       { "phase": "idle" | "delaying" | "running" | "finished", "time": seconds of roll time,
 *         "duration": seconds to the finish (-1 for a loop or a hold that never ends),
 *         "finished": bool, "paused": bool }
 *
 * and a vendor event, RollFinished { "sourceName": "..." }, beside the credits_finished signal.
 *
 * Every response carries "ok", and "error" when it is false -- a source that is not there, or not a
 * credit roll. obs-websocket is optional: without it none of this is registered and nothing else
 * changes.
 */

/* Registers the vendor. obs-websocket only accepts this from obs_module_post_load. */
void registerRollControl();

/* Tells anyone listening over obs-websocket that `source` has finished. */
void notifyRollFinished(obs_source_t *source);

} // namespace closingtime
