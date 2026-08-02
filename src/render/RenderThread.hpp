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

#include <functional>

namespace closingtime {

/*
 * The shared strip-rasterisation thread.
 *
 * Rasterising a long roll takes long enough to be seen, so it does not belong on the thread
 * that draws OBS's own window. QPainter, QFontMetrics and QImageReader are all usable off
 * the GUI thread as long as the paint device is a QImage, which is exactly what the strip
 * renderer paints into.
 *
 * One thread serves every credits source and every open designer window, and jobs run
 * strictly in the order they were posted. That is deliberate: it makes each LogoCache
 * single-threaded by construction without a lock, and a machine rendering several rolls at
 * once has nothing to gain from fighting over cores it also needs for encoding.
 */

/*
 * Queues `job` on the render thread, starting it on first use. Safe to call from any
 * thread, including from inside another job. Jobs posted after stopRenderThread() are
 * dropped.
 */
void postRenderJob(std::function<void()> job);

/*
 * Discards anything still queued, waits for the job in flight, and joins the thread. Called
 * once from obs_module_unload.
 */
void stopRenderThread();

} // namespace closingtime
