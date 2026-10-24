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

#include "render/FileDependencies.hpp"

#include <QDateTime>
#include <QFileInfo>

#include <algorithm>

namespace closingtime {

namespace {

thread_local FileStamps *current = nullptr;

FileStamp stampOf(const QString &path)
{
	const QFileInfo info(path);
	FileStamp stamp;
	stamp.path = path;
	if (info.exists()) {
		stamp.size = info.size();
		stamp.modifiedMs = info.lastModified().toMSecsSinceEpoch();
	}
	return stamp;
}

} // namespace

FileDependencyScope::FileDependencyScope(FileStamps *into) : previous(current)
{
	current = into;
}

FileDependencyScope::~FileDependencyScope()
{
	current = previous;
}

void noteFileRead(const QString &path)
{
	if (!current || path.isEmpty())
		return;

	/* A logo used by every row of a list is one dependency, not forty. */
	const bool known = std::any_of(current->cbegin(), current->cend(),
				       [&path](const FileStamp &stamp) { return stamp.path == path; });
	if (!known)
		current->append(stampOf(path));
}

bool fileStampsCurrent(const FileStamps &stamps)
{
	return std::all_of(stamps.cbegin(), stamps.cend(),
			   [](const FileStamp &stamp) { return stampOf(stamp.path) == stamp; });
}

} // namespace closingtime
