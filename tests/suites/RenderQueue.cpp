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

#include "harness/Harness.hpp"
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
