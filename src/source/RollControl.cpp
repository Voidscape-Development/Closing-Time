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

#include "source/RollControl.hpp"

#include <obs.hpp>
#include <plugin-support.h>

/* The API header uses both without including them. */
#include <cassert>
#include <cmath>
#include <cstring>

#include "source/CreditsSource.hpp"
#include "third-party/obs-websocket-api.h"

namespace closingtime {

namespace {

obs_websocket_vendor vendor = nullptr;

/*
 * The credit roll a request names, or null with `response` already saying why. A source of any
 * other type is refused rather than poked: its proc handler may have a "restart" that means
 * something else entirely.
 */
OBSSourceAutoRelease rollFor(obs_data_t *request, obs_data_t *response)
{
	const char *name = obs_data_get_string(request, "sourceName");
	if (!name || !*name) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "sourceName is required");
		return nullptr;
	}

	OBSSourceAutoRelease source = obs_get_source_by_name(name);
	if (!source) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "no source by that name");
		return nullptr;
	}

	if (std::strcmp(obs_source_get_unversioned_id(source), kCreditsSourceId) != 0) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "error", "that source is not a credit roll");
		return nullptr;
	}

	return source;
}

/* A request that is one call to the roll's own proc handler: start, pause, resume, restart. */
void callProc(obs_data_t *request, obs_data_t *response, const char *proc)
{
	OBSSourceAutoRelease source = rollFor(request, response);
	if (!source)
		return;

	calldata_t call = {};
	proc_handler_call(obs_source_get_proc_handler(source), proc, &call);
	calldata_free(&call);
	obs_data_set_bool(response, "ok", true);
}

void getState(obs_data_t *request, obs_data_t *response, void *)
{
	OBSSourceAutoRelease source = rollFor(request, response);
	if (!source)
		return;

	calldata_t call = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &call);

	const char *phase = calldata_string(&call, "phase");
	const double duration = calldata_float(&call, "duration");

	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "phase", phase ? phase : "idle");
	obs_data_set_double(response, "time", calldata_float(&call, "time"));
	/* JSON has no infinity: a roll that never ends says so with -1. */
	obs_data_set_double(response, "duration", std::isfinite(duration) ? duration : -1.0);
	obs_data_set_bool(response, "finished", calldata_bool(&call, "finished"));
	obs_data_set_bool(response, "paused", calldata_bool(&call, "paused"));

	calldata_free(&call);
}

} // namespace

void registerRollControl()
{
	vendor = obs_websocket_register_vendor("closing-time");
	if (!vendor) {
		obs_log(LOG_INFO, "obs-websocket is not available; credit rolls cannot be driven over it");
		return;
	}

	obs_websocket_vendor_register_request(
		vendor, "Start",
		[](obs_data_t *request, obs_data_t *response, void *) { callProc(request, response, "start"); },
		nullptr);
	obs_websocket_vendor_register_request(
		vendor, "Pause",
		[](obs_data_t *request, obs_data_t *response, void *) { callProc(request, response, "pause"); },
		nullptr);
	obs_websocket_vendor_register_request(
		vendor, "Resume",
		[](obs_data_t *request, obs_data_t *response, void *) { callProc(request, response, "resume"); },
		nullptr);
	obs_websocket_vendor_register_request(
		vendor, "Restart",
		[](obs_data_t *request, obs_data_t *response, void *) { callProc(request, response, "restart"); },
		nullptr);
	obs_websocket_vendor_register_request(vendor, "GetState", getState, nullptr);

	obs_log(LOG_INFO, "credit rolls can be driven over obs-websocket as vendor 'closing-time'");
}

void notifyRollFinished(obs_source_t *source)
{
	if (!vendor || !source)
		return;

	OBSDataAutoRelease event = obs_data_create();
	obs_data_set_string(event, "sourceName", obs_source_get_name(source));
	obs_websocket_vendor_emit_event(vendor, "RollFinished", event);
}

} // namespace closingtime
