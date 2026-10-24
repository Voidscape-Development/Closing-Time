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

#include <QString>

#include <chrono>
#include <future>
#include <mutex>
#include <vector>

#include <thread>

#include "harness/Harness.hpp"
#include "render/ChunkPipeline.hpp"
#include "render/RenderThread.hpp"

using namespace closingtime;
using namespace closingtime::test;

CT_SUITE(render_queue_priority, "A live job runs ahead of everything already waiting")
{
	/*
	 * The thread is held on a gate while the queue is filled, so the order the jobs run in is the
	 * queue's decision and not a race against the thread picking them up.
	 */
	std::promise<void> gate;
	std::shared_future<void> opened = gate.get_future().share();
	std::promise<void> done;

	std::mutex mutex;
	std::vector<QString> order;
	const auto record = [&](const char *name) {
		std::lock_guard<std::mutex> lock(mutex);
		order.push_back(QString::fromLatin1(name));
	};

	postRenderJob([opened] { opened.wait(); });
	postRenderJob([&] { record("first"); });
	postRenderJob([&] { record("second"); });
	postRenderJob([&] { record("live"); }, RenderPriority::Live);
	postRenderJob([&] {
		record("last");
		done.set_value();
	});

	gate.set_value();
	const bool finished = done.get_future().wait_for(std::chrono::seconds(10)) == std::future_status::ready;
	check(finished, "every job runs");
	if (!finished)
		return;

	std::lock_guard<std::mutex> lock(mutex);
	checkEq(static_cast<long long>(order.size()), 4, "four jobs ran");
	if (order.size() == 4) {
		checkEq(order.at(0), QStringLiteral("live"), "the live job first, though it was posted third");
		checkEq(order.at(1), QStringLiteral("first"), "then the rest in the order they were posted");
		checkEq(order.at(2), QStringLiteral("second"), "in order");
		checkEq(order.at(3), QStringLiteral("last"), "to the last");
	}
}

CT_SUITE(chunk_pipeline, "A consumer's chunks are drawn most urgent first, and only while wanted")
{
	Document document;
	document.width = 640;
	for (const char *text : {"Alpha", "Bravo", "Charlie"}) {
		Section section = Section::makeDefault(SectionType::Title);
		section.text = QString::fromLatin1(text);
		document.sections.append(section);
	}

	auto logos = std::make_shared<LogoCache>();
	const RollLayoutPtr layout = RollRenderer(logos.get()).layout(document);
	if (layout->layers.size() < 3) {
		fail(QStringLiteral("three layers to ask for"));
		return;
	}

	const auto request = [&](int layer, double urgency) {
		return ChunkRequest{chunkKey(layout->layers.at(layer), 0, 1.0), layout, layer, 0, 1.0, urgency};
	};

	std::promise<void> gate;
	std::shared_future<void> opened = gate.get_future().share();
	postRenderJob([opened] { opened.wait(); });

	const auto pipeline = ChunkPipeline::create(logos, nullptr, 64LL * 1024 * 1024, RenderPriority::Normal);
	/* Asked for in one order, then changed its mind before anything was drawn. */
	pipeline->want({request(0, 2.0), request(1, 0.0), request(2, 1.0)});
	pipeline->want({request(2, 1.0), request(1, 0.0)});
	checkEq(pipeline->waiting(), 2, "the second list replaces the first");

	gate.set_value();

	QVector<QPair<quint64, QImage>> arrived;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (arrived.size() < 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		arrived += pipeline->takeArrived();
	}

	/* Give a stray third chunk the chance to turn up, if it was ever going to. */
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	arrived += pipeline->takeArrived();

	checkEq(static_cast<long long>(arrived.size()), 2, "exactly the two still wanted are drawn");
	if (arrived.size() == 2) {
		check(arrived.at(0).first == request(1, 0).key, "the most urgent first");
		check(arrived.at(1).first == request(2, 0).key, "then the next");
		check(!arrived.at(0).second.isNull() && !arrived.at(1).second.isNull(), "and both are pictures");
	}
}
