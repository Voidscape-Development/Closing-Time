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

#include "model/StyleLibrary.hpp"

#include <obs.hpp>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <mutex>

#include "plugin-support.h"

namespace closingtime {

namespace {

/* How often pollForChanges() is allowed to stat the file, in milliseconds. */
constexpr qint64 kPollIntervalMs = 1000;

/*
 * The one lock the library takes.
 *
 * It is a file-static rather than a member because the instance is a singleton with a private
 * constructor and the mutex has to be usable from the const accessors; keeping it out of the
 * class also keeps <mutex> out of a header that model/, render/, source/ and ui/ all include.
 */
std::mutex &libraryMutex()
{
	static std::mutex mutex;
	return mutex;
}

QVector<StylePreset> presetsFromData(obs_data_t *data)
{
	QVector<StylePreset> presets;

	OBSDataArrayAutoRelease array = obs_data_get_array(data, "style_presets");
	if (!array)
		return presets;

	const size_t count = obs_data_array_count(array);
	presets.reserve(static_cast<int>(count));

	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		StylePreset preset;
		preset.load(item);

		/* An unnamed preset can never be bound to, and a duplicate name can never be told apart. */
		if (preset.name.isEmpty())
			continue;
		if (std::any_of(presets.cbegin(), presets.cend(),
				[&preset](const StylePreset &existing) { return existing.name == preset.name; }))
			continue;

		/*
		 * Every preset in the library is by definition the library's own. The flag is a
		 * property of a *document's* copy -- it says "this one follows the library" -- so it is
		 * cleared here rather than round-tripped, and a library file hand-edited to carry it
		 * means nothing different.
		 */
		preset.linked = false;
		presets.append(preset);
	}

	return presets;
}

QVector<QPair<QString, QString>> renamesFromData(obs_data_t *data)
{
	QVector<QPair<QString, QString>> renames;

	OBSDataArrayAutoRelease array = obs_data_get_array(data, "renames");
	if (!array)
		return renames;

	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		const QString from = QString::fromUtf8(obs_data_get_string(item, "from"));
		const QString to = QString::fromUtf8(obs_data_get_string(item, "to"));

		if (from.isEmpty() || to.isEmpty() || from == to)
			continue;

		renames.append({from, to});
	}

	return renames;
}

QString presetsToJson(const QVector<StylePreset> &presets, const QVector<QPair<QString, QString>> &renames,
		      bool alwaysEditLinked)
{
	OBSDataAutoRelease data = obs_data_create();
	OBSDataArrayAutoRelease array = obs_data_array_create();

	for (const StylePreset &preset : presets) {
		OBSDataAutoRelease item = obs_data_create();
		preset.save(item);
		obs_data_array_push_back(array, item);
	}

	obs_data_set_array(data, "style_presets", array);

	OBSDataArrayAutoRelease renameArray = obs_data_array_create();
	for (const QPair<QString, QString> &rename : renames) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "from", rename.first.toUtf8().constData());
		obs_data_set_string(item, "to", rename.second.toUtf8().constData());
		obs_data_array_push_back(renameArray, item);
	}

	obs_data_set_array(data, "renames", renameArray);
	obs_data_set_bool(data, "always_edit_linked", alwaysEditLinked);
	return QString::fromUtf8(obs_data_get_json_pretty(data));
}

} // namespace

StyleLibrary &StyleLibrary::instance()
{
	static StyleLibrary library;
	return library;
}

void StyleLibrary::setFilePath(const QString &newPath)
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	path = newPath;
	/* A new file has not been read yet, whatever was read from the old one. */
	fileModifiedMs = -1;
	fileSize = -1;
	lastPollMs = 0;
}

QString StyleLibrary::filePath() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return path;
}

void StyleLibrary::load()
{
	std::lock_guard<std::mutex> lock(libraryMutex());

	if (path.isEmpty())
		return;

	const QFileInfo info(path);
	fileModifiedMs = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
	fileSize = info.exists() ? info.size() : -1;
	lastPollMs = QDateTime::currentMSecsSinceEpoch();

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		/* No file is the ordinary state of a machine that has never published a style. */
		if (info.exists())
			obs_log(LOG_WARNING, "could not read the style library at '%s'", path.toUtf8().constData());

		/*
		 * Everything the file holds, not just the presets: the rename trail and the ask-again
		 * answer are as much a part of a library as its styles are, and leaving either behind
		 * would carry one library's history into whatever is read next.
		 */
		entries.clear();
		renameTrail.clear();
		editLinkedInPlace = false;
		bumpLocked();
		return;
	}

	const QByteArray contents = file.readAll();
	file.close();

	OBSDataAutoRelease data = obs_data_create_from_json(contents.constData());
	if (!data) {
		obs_log(LOG_WARNING, "the style library at '%s' is not valid JSON; leaving it alone",
			path.toUtf8().constData());
		return;
	}

	entries = presetsFromData(data);
	renameTrail = renamesFromData(data);
	editLinkedInPlace = obs_data_get_bool(data, "always_edit_linked");
	bumpLocked();

	obs_log(LOG_INFO, "style library: %d preset(s)", entries.size());
}

bool StyleLibrary::save()
{
	QString target;
	QString json;

	{
		std::lock_guard<std::mutex> lock(libraryMutex());
		if (path.isEmpty())
			return false;

		target = path;
		json = presetsToJson(entries, renameTrail, editLinkedInPlace);
	}

	QDir().mkpath(QFileInfo(target).absolutePath());

	/*
	 * Written through QSaveFile so a crash or a full disk halfway through leaves the previous
	 * library in place rather than a truncated one: this file is the only copy of styles that
	 * may be bound from every scene collection on the machine.
	 */
	QSaveFile file(target);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		obs_log(LOG_WARNING, "could not open the style library at '%s' for writing",
			target.toUtf8().constData());
		return false;
	}

	file.write(json.toUtf8());
	if (!file.commit()) {
		obs_log(LOG_WARNING, "could not write the style library at '%s'", target.toUtf8().constData());
		return false;
	}

	/* Our own write must not read back as somebody else's change. */
	const QFileInfo info(target);
	std::lock_guard<std::mutex> lock(libraryMutex());
	fileModifiedMs = info.lastModified().toMSecsSinceEpoch();
	fileSize = info.size();

	return true;
}

bool StyleLibrary::pollForChanges()
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());
		if (path.isEmpty())
			return false;

		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		if (now - lastPollMs < kPollIntervalMs)
			return false;
		lastPollMs = now;

		const QFileInfo info(path);
		const qint64 modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() : -1;
		const qint64 size = info.exists() ? info.size() : -1;
		if (modified == fileModifiedMs && size == fileSize)
			return false;
	}

	const quint64 before = serial();
	load();
	return serial() != before;
}

quint64 StyleLibrary::serial() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return librarySerial;
}

QVector<StylePreset> StyleLibrary::presets() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return entries;
}

bool StyleLibrary::find(const QString &name, TextStyle *style) const
{
	if (name.isEmpty())
		return false;

	std::lock_guard<std::mutex> lock(libraryMutex());
	for (const StylePreset &preset : entries) {
		if (preset.name != name)
			continue;

		if (style)
			*style = preset.style;
		return true;
	}

	return false;
}

bool StyleLibrary::contains(const QString &name) const
{
	return find(name, nullptr);
}

void StyleLibrary::set(const QString &name, const TextStyle &style)
{
	if (name.isEmpty())
		return;

	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		bool replaced = false;
		for (StylePreset &preset : entries) {
			if (preset.name != name)
				continue;
			preset.style = style;
			replaced = true;
			break;
		}

		if (!replaced)
			entries.append(StylePreset{name, style, false});

		/*
		 * A name that exists again is nobody's old name. Without this, a preset renamed away
		 * and then re-created under the original name would send every roll bound to it off to
		 * the renamed one -- the trail would be describing a preset that is no longer gone.
		 */
		renameTrail.erase(
			std::remove_if(renameTrail.begin(), renameTrail.end(),
				       [&name](const QPair<QString, QString> &rename) { return rename.first == name; }),
			renameTrail.end());

		bumpLocked();
	}

	save();
}

void StyleLibrary::remove(const QString &name)
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		const auto before = entries.size();
		entries.erase(std::remove_if(entries.begin(), entries.end(),
					     [&name](const StylePreset &preset) { return preset.name == name; }),
			      entries.end());
		if (entries.size() == before)
			return;

		bumpLocked();
	}

	save();
}

void StyleLibrary::rename(const QString &from, const QString &to)
{
	if (from.isEmpty() || to.isEmpty() || from == to)
		return;

	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		/* Renaming onto an existing name would leave two presets nothing could tell apart. */
		if (std::any_of(entries.cbegin(), entries.cend(),
				[&to](const StylePreset &preset) { return preset.name == to; }))
			return;

		bool renamed = false;
		for (StylePreset &preset : entries) {
			if (preset.name != from)
				continue;
			preset.name = to;
			renamed = true;
			break;
		}

		if (!renamed)
			return;

		/*
		 * Chains are collapsed rather than stacked: after A->B and then B->C, a document still
		 * carrying A has to reach C in one lookup, and B->C has to keep working for one that
		 * stopped at B. So every entry pointing at the old name is re-pointed at the new one,
		 * and the hop just made is added to them.
		 */
		for (QPair<QString, QString> &rename : renameTrail) {
			if (rename.second == from)
				rename.second = to;
		}

		renameTrail.append({from, to});

		/* A rename back to a name already in the trail leaves an entry pointing at itself. */
		renameTrail.erase(std::remove_if(renameTrail.begin(), renameTrail.end(),
						 [](const QPair<QString, QString> &rename) {
							 return rename.first == rename.second;
						 }),
				  renameTrail.end());

		bumpLocked();
	}

	save();
}

bool StyleLibrary::renamedTo(const QString &from, QString *to) const
{
	if (from.isEmpty())
		return false;

	std::lock_guard<std::mutex> lock(libraryMutex());

	QString current = from;

	/*
	 * One hop is all a collapsed trail should ever need. The loop is here because the file can be
	 * hand-edited, and a cycle in it must not become a hang inside a render path's caller.
	 */
	for (int hops = 0; hops < renameTrail.size() + 1; ++hops) {
		bool moved = false;

		for (const QPair<QString, QString> &rename : renameTrail) {
			if (rename.first != current)
				continue;

			current = rename.second;
			moved = true;
			break;
		}

		if (!moved)
			break;
	}

	if (current == from)
		return false;

	if (to)
		*to = current;

	return true;
}

QVector<QPair<QString, QString>> StyleLibrary::renames() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return renameTrail;
}

void StyleLibrary::replaceAll(const QVector<StylePreset> &presets)
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		entries.clear();
		entries.reserve(presets.size());

		for (const StylePreset &preset : presets) {
			if (preset.name.isEmpty())
				continue;
			if (std::any_of(entries.cbegin(), entries.cend(), [&preset](const StylePreset &existing) {
				    return existing.name == preset.name;
			    }))
				continue;

			entries.append(StylePreset{preset.name, preset.style, false});
		}

		bumpLocked();
	}

	save();
}

QString StyleLibrary::toJson() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return presetsToJson(entries, renameTrail, editLinkedInPlace);
}

bool StyleLibrary::alwaysEditLinked() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return editLinkedInPlace;
}

void StyleLibrary::setAlwaysEditLinked(bool always)
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());
		if (editLinkedInPlace == always)
			return;
		editLinkedInPlace = always;
	}

	save();
}

bool StyleLibrary::parseJson(const QString &json, QVector<StylePreset> *presets, QString *error)
{
	OBSDataAutoRelease data = obs_data_create_from_json(json.toUtf8().constData());
	if (!data) {
		if (error)
			*error = QStringLiteral("The file is not valid Closing Time JSON.");
		return false;
	}

	QVector<StylePreset> parsed = presetsFromData(data);

	/*
	 * A whole exported document is a reasonable thing to point the importer at -- its presets
	 * are under the same key -- but a file with no presets at all is a mistake worth naming
	 * rather than an import that silently does nothing.
	 */
	if (parsed.isEmpty()) {
		if (error)
			*error = QStringLiteral("That file holds no style presets.");
		return false;
	}

	if (presets)
		*presets = parsed;

	return true;
}

void StyleLibrary::bumpLocked()
{
	++librarySerial;
}

} // namespace closingtime
