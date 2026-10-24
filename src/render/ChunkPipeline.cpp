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

#include "render/ChunkPipeline.hpp"

#include <QSet>

#include <algorithm>

namespace closingtime {

std::shared_ptr<ChunkPipeline> ChunkPipeline::create(std::shared_ptr<LogoCache> logos,
						     std::shared_ptr<AnimatedLogoCache> animations, qint64 cacheBytes,
						     RenderPriority priority, std::function<void()> arrived)
{
	std::shared_ptr<ChunkPipeline> pipeline(new ChunkPipeline());
	pipeline->logos = std::move(logos);
	pipeline->animations = std::move(animations);
	pipeline->rasters = std::make_unique<RasterCache>(cacheBytes);
	pipeline->priority = priority;
	pipeline->arrived = std::move(arrived);
	return pipeline;
}

void ChunkPipeline::want(std::vector<ChunkRequest> requests)
{
	std::stable_sort(requests.begin(), requests.end(),
			 [](const ChunkRequest &a, const ChunkRequest &b) { return a.urgency < b.urgency; });

	std::lock_guard<std::mutex> lock(mutex);
	wanted.clear();

	/* Sorted first, so the first time a key is met is its most urgent asking. */
	QSet<quint64> seen;
	for (ChunkRequest &request : requests) {
		if (request.key == drawing || seen.contains(request.key))
			continue;
		seen.insert(request.key);
		wanted.push_back(std::move(request));
	}

	if (!wanted.empty() && !posted) {
		posted = true;
		post();
	}
}

QVector<QPair<quint64, QImage>> ChunkPipeline::takeArrived()
{
	std::lock_guard<std::mutex> lock(mutex);
	QVector<QPair<quint64, QImage>> taken;
	taken.swap(done);
	return taken;
}

bool ChunkPipeline::hasArrived() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return !done.isEmpty();
}

int ChunkPipeline::waiting() const
{
	std::lock_guard<std::mutex> lock(mutex);
	return static_cast<int>(wanted.size());
}

/* Callers hold the mutex, which is what keeps `posted` honest. */
void ChunkPipeline::post()
{
	postRenderJob(
		[weak = weak_from_this()] {
			if (const std::shared_ptr<ChunkPipeline> self = weak.lock())
				self->drainOne();
		},
		priority);
}

void ChunkPipeline::drainOne()
{
	ChunkRequest next;
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (wanted.empty()) {
			posted = false;
			return;
		}
		next = std::move(wanted.front());
		wanted.pop_front();
		drawing = next.key;
	}

	const RollRenderer renderer(logos.get(), animations.get());
	QImage image = rasters->chunk(renderer, *next.layout, next.layer, next.chunk, next.scale);

	{
		std::lock_guard<std::mutex> lock(mutex);
		done.append({next.key, std::move(image)});
		drawing = 0;
		/* Posted again under the lock, so a `want` that empties and refills the list cannot slip between. */
		if (wanted.empty())
			posted = false;
		else
			post();
	}

	if (arrived)
		arrived();
}

} // namespace closingtime
