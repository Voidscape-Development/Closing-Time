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

#include <QImage>
#include <QPair>
#include <QVector>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "render/RenderThread.hpp"
#include "render/RollCompositor.hpp"
#include "render/RollRenderer.hpp"

namespace closingtime {

/* One chunk somebody wants drawn, and how soon they will draw it. */
struct ChunkRequest {
	quint64 key = 0;
	RollLayoutPtr layout;
	int layer = -1;
	int chunk = -1;
	qreal scale = 1.0;
	/* Seconds until it is drawn: 0 for one that is wanted on screen now. */
	double urgency = 0.0;
};

/*
 * Rasterizes the chunks one consumer wants, most urgent first, on the render thread.
 *
 * The consumer -- a source, a designer's preview -- says what it wants as often as it likes, and each
 * time the whole list is replaced. The render thread takes the most urgent chunk off the front one
 * job at a time and posts itself again after each, so the list is consulted afresh for every chunk:
 * something scrubbed onto the screen goes to the front at once rather than waiting behind whatever
 * was asked for before it, a chunk nobody wants any more simply drops out, and every consumer on the
 * machine gets a turn rather than one long roll's whole lookahead going first.
 *
 * Every chunk goes through the consumer's own raster cache, so one drawn before -- a loop coming
 * round, a scrub back, a section a rebuild did not change -- costs a lookup.
 *
 * Held by shared_ptr and reached from jobs through a weak one, so a consumer that goes away with
 * work still queued simply stops getting answers. `arrived` is called on the render thread after
 * each chunk lands, for a consumer that would rather be told than ask.
 */
class ChunkPipeline : public std::enable_shared_from_this<ChunkPipeline> {
public:
	static std::shared_ptr<ChunkPipeline> create(std::shared_ptr<LogoCache> logos,
						     std::shared_ptr<AnimatedLogoCache> animations, qint64 cacheBytes,
						     RenderPriority priority, std::function<void()> arrived = {});

	/*
	 * Replaces what is wanted. Sorted here, most urgent first; a chunk asked for twice keeps the
	 * more urgent of the two. The chunk being drawn at this moment is left out, since it is on its
	 * way already. Safe from any thread.
	 */
	void want(std::vector<ChunkRequest> requests);

	/*
	 * The chunks drawn since the last call, by key. A null image is a chunk that could not be drawn
	 * at all, which a consumer should stop asking for. Safe from any thread.
	 */
	QVector<QPair<quint64, QImage>> takeArrived();
	bool hasArrived() const;

	/* How many chunks are waiting to be drawn, for the harness. */
	int waiting() const;

private:
	ChunkPipeline() = default;

	void post();
	void drainOne();

	std::shared_ptr<LogoCache> logos;
	std::shared_ptr<AnimatedLogoCache> animations;
	/* Render thread only. */
	std::unique_ptr<RasterCache> rasters;
	RenderPriority priority = RenderPriority::Normal;
	std::function<void()> arrived;

	mutable std::mutex mutex;
	std::deque<ChunkRequest> wanted;
	/* Set while a drain job is queued or running, so there is never more than one. */
	bool posted = false;
	/* The chunk being drawn at this moment, which a new list leaves out. */
	quint64 drawing = 0;
	QVector<QPair<quint64, QImage>> done;
};

} // namespace closingtime
