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

#include "util/FontFiles.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QHash>
#include <QStandardPaths>
#include <QStringDecoder>

#include <mutex>

namespace closingtime {

namespace {

/* Enough to hold every font on a well-stocked machine, and a stop for a directory loop. */
constexpr int kMaxIndexedFiles = 20000;
/* A font collection with more faces than this is a corrupt header rather than a font. */
constexpr quint32 kMaxFacesPerFile = 256;
/* A `name` table is a few kilobytes; anything claiming more is not one. */
constexpr quint32 kMaxNameTableBytes = 1024 * 1024;

quint16 readU16(const QByteArray &bytes, int offset)
{
	if (offset < 0 || offset + 2 > bytes.size())
		return 0;

	const auto *data = reinterpret_cast<const quint8 *>(bytes.constData()) + offset;
	return static_cast<quint16>((data[0] << 8) | data[1]);
}

quint32 readU32(const QByteArray &bytes, int offset)
{
	if (offset < 0 || offset + 4 > bytes.size())
		return 0;

	const auto *data = reinterpret_cast<const quint8 *>(bytes.constData()) + offset;
	return (static_cast<quint32>(data[0]) << 24) | (static_cast<quint32>(data[1]) << 16) |
	       (static_cast<quint32>(data[2]) << 8) | static_cast<quint32>(data[3]);
}

/*
 * A name record's bytes as text.
 *
 * Platform 3 (Windows) and platform 0 (Unicode) store UTF-16BE; platform 1 (Macintosh) stores a
 * legacy single-byte encoding, of which the only one that matters here is Roman -- a family name
 * outside it will also be recorded in one of the Unicode platforms, since a Windows record is
 * mandatory for a font to install at all.
 */
QString decodeNameRecord(quint16 platformId, const QByteArray &bytes)
{
	if (bytes.isEmpty())
		return QString();

	if (platformId == 1) {
		/* Latin-1 rather than Mac Roman: they agree over ASCII, which family names are. */
		return QString::fromLatin1(bytes).trimmed();
	}

	QStringDecoder decoder(QStringDecoder::Utf16BE);
	QString text = decoder(bytes);
	if (decoder.hasError())
		return QString();

	return text.trimmed();
}

void appendUnique(QStringList &list, const QString &value)
{
	if (value.isEmpty())
		return;

	for (const QString &existing : list) {
		if (existing.compare(value, Qt::CaseInsensitive) == 0)
			return;
	}

	list.append(value);
}

/* Families declared by the face whose table directory starts at `faceOffset`. */
void collectFaceFamilies(QFile &file, quint32 faceOffset, QStringList &families)
{
	if (!file.seek(faceOffset))
		return;

	const QByteArray header = file.read(12);
	if (header.size() < 12)
		return;

	const quint16 tableCount = readU16(header, 4);
	if (tableCount == 0)
		return;

	const QByteArray directory = file.read(static_cast<qint64>(tableCount) * 16);

	quint32 nameOffset = 0;
	quint32 nameLength = 0;
	for (int i = 0; i + 16 <= directory.size(); i += 16) {
		if (directory.mid(i, 4) != QByteArrayLiteral("name"))
			continue;

		nameOffset = readU32(directory, i + 8);
		nameLength = readU32(directory, i + 12);
		break;
	}

	if (nameLength < 6 || nameLength > kMaxNameTableBytes || !file.seek(nameOffset))
		return;

	const QByteArray table = file.read(nameLength);
	if (table.size() < 6)
		return;

	const quint16 recordCount = readU16(table, 2);
	const quint16 stringBase = readU16(table, 4);

	for (quint16 i = 0; i < recordCount; ++i) {
		const int record = 6 + i * 12;
		if (record + 12 > table.size())
			break;

		const quint16 nameId = readU16(table, record + 6);
		/* 1 is the family a font installs under, 16 the typographic family it belongs to. */
		if (nameId != 1 && nameId != 16)
			continue;

		const quint16 platformId = readU16(table, record);
		const quint16 length = readU16(table, record + 8);
		const quint16 offset = readU16(table, record + 10);

		appendUnique(families, decodeNameRecord(platformId, table.mid(stringBase + offset, length)));
	}
}

struct FontIndex {
	std::mutex mutex;
	bool built = false;
	QStringList searchPathOverride;
	/* Lower-cased family -> the files declaring it, in the order they were walked. */
	QHash<QString, QStringList> files;
};

FontIndex &index()
{
	static FontIndex instance;
	return instance;
}

QStringList searchPathsLocked(const FontIndex &state)
{
	if (!state.searchPathOverride.isEmpty())
		return state.searchPathOverride;

	QStringList paths = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);

#if defined(Q_OS_WIN)
	/*
	 * Qt reports the machine-wide directory but not the per-user one a font installed without
	 * administrator rights lands in, which is where a font a streamer installed for themselves
	 * usually is.
	 */
	const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
	if (!localAppData.isEmpty())
		paths.append(localAppData + QStringLiteral("/Microsoft/Windows/Fonts"));
#elif defined(Q_OS_MACOS)
	paths.append(QStringLiteral("/System/Library/Fonts"));
	paths.append(QStringLiteral("/Library/Fonts"));
	paths.append(QDir::homePath() + QStringLiteral("/Library/Fonts"));
#else
	paths.append(QStringLiteral("/usr/share/fonts"));
	paths.append(QStringLiteral("/usr/local/share/fonts"));
	paths.append(QDir::homePath() + QStringLiteral("/.fonts"));
	paths.append(QDir::homePath() + QStringLiteral("/.local/share/fonts"));
#endif

	QStringList unique;
	for (const QString &path : paths) {
		const QString clean = QDir::cleanPath(path);
		if (!clean.isEmpty() && !unique.contains(clean))
			unique.append(clean);
	}

	return unique;
}

void buildLocked(FontIndex &state)
{
	if (state.built)
		return;

	state.built = true;
	state.files.clear();

	static const QStringList filters = {
		QStringLiteral("*.ttf"),
		QStringLiteral("*.otf"),
		QStringLiteral("*.ttc"),
		QStringLiteral("*.otc"),
	};

	int seen = 0;
	for (const QString &directory : searchPathsLocked(state)) {
		QDirIterator walk(directory, filters, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
		while (walk.hasNext() && seen < kMaxIndexedFiles) {
			const QString path = walk.next();
			++seen;

			for (const QString &family : fontFamiliesInFile(path)) {
				QStringList &paths = state.files[family.toLower()];
				if (!paths.contains(path))
					paths.append(path);
			}
		}
	}
}

} // namespace

QStringList fontSearchPaths()
{
	FontIndex &state = index();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return searchPathsLocked(state);
}

QStringList fontFamiliesInFile(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return QStringList();

	const QByteArray header = file.read(12);
	if (header.size() < 12)
		return QStringList();

	QVector<quint32> faces;
	const quint32 tag = readU32(header, 0);

	if (tag == 0x74746366u /* 'ttcf' */) {
		const quint32 count = qMin(readU32(header, 8), kMaxFacesPerFile);
		const QByteArray offsets = file.read(static_cast<qint64>(count) * 4);
		for (quint32 i = 0; i < count && static_cast<int>(i * 4 + 4) <= offsets.size(); ++i)
			faces.append(readU32(offsets, static_cast<int>(i * 4)));
	} else if (tag == 0x00010000u || tag == 0x74727565u /* 'true' */ || tag == 0x4F54544Fu /* 'OTTO' */) {
		faces.append(0);
	} else {
		/* WOFF, Type 1, bitmap fonts: real fonts, but not ones this reader claims to know. */
		return QStringList();
	}

	QStringList families;
	for (quint32 face : faces)
		collectFaceFamilies(file, face, families);

	return families;
}

QStringList fontFilesForFamily(const QString &family)
{
	if (family.isEmpty())
		return QStringList();

	FontIndex &state = index();
	const std::lock_guard<std::mutex> lock(state.mutex);
	buildLocked(state);

	return state.files.value(family.toLower());
}

void clearFontFileIndex()
{
	FontIndex &state = index();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.built = false;
	state.files.clear();
}

void setFontSearchPaths(const QStringList &paths)
{
	FontIndex &state = index();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.searchPathOverride = paths;
	state.built = false;
	state.files.clear();
}

} // namespace closingtime
