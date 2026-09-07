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

#include <QString>

#include "model/StyleLibrary.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "source/CreditsSource.hpp"
#include "ui/DesignerDialog.hpp"

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

	/*
	 * The style library is machine-wide, so it belongs to the module rather than to a source or
	 * a window: a source loading a scene collection has to be able to resolve a linked preset
	 * before any designer has been opened. The path is handed in from here because the library
	 * itself is compiled into the test harness, which has no OBS module to ask for one.
	 */
	if (char *path = obs_module_config_path("style-presets.json")) {
		closingtime::StyleLibrary::instance().setFilePath(QString::fromUtf8(path));
		bfree(path);
		closingtime::StyleLibrary::instance().load();
	}

	/*
	 * Fonts a roll carries with it are extracted here before they are registered with Qt. The
	 * directory is handed in for the same reason the library's path is -- this code compiles into
	 * the test harness, which has no OBS module to ask -- and a build without one registers the
	 * bytes straight from the document instead.
	 */
	if (char *path = obs_module_config_path("fonts")) {
		closingtime::setFontCacheDirectory(QString::fromUtf8(path));
		bfree(path);
	}

	closingtime::registerCreditsSource();
	/*
	 * The menu lists sources by type and the designer opens them, so neither half of that
	 * belongs to the other: the module entry point is where the two are introduced.
	 */
	closingtime::registerDesignerToolsMenu(closingtime::kCreditsSourceId);
	/* The library is the machine's rather than any roll's, so it gets an entry of its own. */
	closingtime::registerStyleLibraryToolsMenu();

	obs_log(LOG_INFO, "Closing Time loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	/* Joins the rasterization thread before this module's code is unmapped. */
	closingtime::stopRenderThread();

	obs_log(LOG_INFO, "Closing Time unloaded");
}
