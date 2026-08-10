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

#include <obs-module.h>
#include <plugin-support.h>

#include "render/RenderThread.hpp"
#include "source/CreditsSource.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Plugin.Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Closing Time";
}

bool obs_module_load(void)
{
	/*
	 * A single global signal gives scripts one place to listen for any credit roll
	 * finishing, without having to connect to each source individually.
	 */
	signal_handler_add(obs_get_signal_handler(), "void closing_time_finished(ptr source)");

	closingtime::registerCreditsSource();
	closingtime::registerDesignerToolsMenu();

	obs_log(LOG_INFO, "Closing Time loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	/* Joins the rasterisation thread before this module's code is unmapped. */
	closingtime::stopRenderThread();

	obs_log(LOG_INFO, "Closing Time unloaded");
}
