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

#include "render/RenderThread.hpp"

#include <QString>
#include <QThread>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace closingtime {

namespace {

/*
 * Puts the calling thread a step below the threads OBS composites and encodes on.
 *
 * Rasterizing a long roll keeps a core busy for seconds, and nothing waits on it frame by frame:
 * the strip it produces is picked up whenever it lands. The graphics and encoder threads are the
 * opposite -- each has a deadline every frame -- and on a machine with few cores to spare, a rebuild
 * competing with them as an equal is how editing a roll on air drops frames that have nothing to do
 * with the roll. So the scheduler is told which one can wait.
 *
 * Done natively rather than through QThread::setPriority, which on Linux does nothing for a thread
 * under the default scheduling policy -- which is every thread OBS starts.
 */
void lowerThreadPriority()
{
#if defined(_WIN32)
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
	/* Linux keeps a nice value per thread, and a thread is always allowed to lower its own. */
	setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 5);
#endif
}

struct RenderQueue {
	std::mutex mutex;
	std::condition_variable wake;
	std::deque<std::function<void()>> live;
	std::deque<std::function<void()>> jobs;
	bool stopping = false;
	/*
	 * A plain thread rather than a QThread: the first job can be posted from the graphics
	 * thread, and a QThread created there would then be a QObject belonging to a thread
	 * other than the one that joins and destroys it. Qt adopts this thread on first use
	 * and tears its per-thread state down when it exits, which is all the renderer needs.
	 */
	std::thread thread;
};

RenderQueue &renderQueue()
{
	static RenderQueue queue;
	return queue;
}

void runQueuedJobs()
{
	RenderQueue &queue = renderQueue();

	/* Adopts the thread into Qt and gives it a name worth seeing in a debugger. */
	QThread::currentThread()->setObjectName(QStringLiteral("closing-time-render"));
	lowerThreadPriority();

	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(queue.mutex);
			queue.wake.wait(lock, [&queue] {
				return queue.stopping || !queue.live.empty() || !queue.jobs.empty();
			});

			if (queue.stopping)
				return;

			std::deque<std::function<void()>> &from = queue.live.empty() ? queue.jobs : queue.live;
			job = std::move(from.front());
			from.pop_front();
		}

		/* Run outside the lock: a job is allowed to queue its own follow-up. */
		job();
	}
}

} // namespace

void postRenderJob(std::function<void()> job, RenderPriority priority)
{
	RenderQueue &queue = renderQueue();

	std::lock_guard<std::mutex> lock(queue.mutex);
	if (queue.stopping)
		return;

	if (!queue.thread.joinable())
		queue.thread = std::thread(runQueuedJobs);

	(priority == RenderPriority::Live ? queue.live : queue.jobs).push_back(std::move(job));
	queue.wake.notify_one();
}

void stopRenderThread()
{
	RenderQueue &queue = renderQueue();

	{
		std::lock_guard<std::mutex> lock(queue.mutex);
		if (!queue.thread.joinable())
			return;

		queue.stopping = true;
		/*
		 * Queued jobs are dropped rather than drained: by the time the module unloads,
		 * their results have nowhere left to go.
		 */
		queue.live.clear();
		queue.jobs.clear();
	}
	queue.wake.notify_all();

	/* Whatever was already running still finishes, so nothing is torn out from under it. */
	queue.thread.join();
}

} // namespace closingtime
