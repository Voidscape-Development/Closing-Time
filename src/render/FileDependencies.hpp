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

#include <QString>
#include <QVector>

namespace closingtime {

/*
 * The files a picture was drawn from, as they were when it was drawn.
 *
 * A cached picture of a section is only as good as the logos, panel images and SVG artwork it was
 * painted from, and those live on disk where they can change without the document changing at all.
 * Listing which fields of the model name a file would be a list to keep up to date by hand, and the
 * one field nobody remembered to add is a stale logo on air. So the loaders say what they read
 * instead: every one of them reports the path it opens, a scope collects the reports for the length
 * of a raster, and the cache holds the picture only while every file still has the size and
 * modification time it had.
 */
struct FileStamp {
	QString path;
	qint64 size = -1;
	qint64 modifiedMs = -1;

	bool operator==(const FileStamp &other) const = default;
};

using FileStamps = QVector<FileStamp>;

/*
 * Collects every file reported on this thread into `into` for as long as it is alive. Scopes nest;
 * the innermost one collects. Null collects nothing, which is what a caller that does not cache
 * passes.
 */
class FileDependencyScope {
public:
	explicit FileDependencyScope(FileStamps *into);
	~FileDependencyScope();

	FileDependencyScope(const FileDependencyScope &) = delete;
	FileDependencyScope &operator=(const FileDependencyScope &) = delete;

private:
	FileStamps *previous;
};

/* Reports that `path` is being read. Called by every loader; a no-op outside a scope. */
void noteFileRead(const QString &path);

/* True while every file still looks the way it did when it was stamped. */
bool fileStampsCurrent(const FileStamps &stamps);

} // namespace closingtime
