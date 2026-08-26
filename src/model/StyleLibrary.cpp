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

QVector<BackgroundPreset> backgroundsFromData(obs_data_t *data)
{
	QVector<BackgroundPreset> presets;

	OBSDataArrayAutoRelease array = obs_data_get_array(data, "background_presets");
	if (!array)
		return presets;

	const size_t count = obs_data_array_count(array);
	presets.reserve(static_cast<int>(count));

	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		BackgroundPreset preset;
		preset.load(item);

		if (preset.name.isEmpty())
			continue;
		if (std::any_of(presets.cbegin(), presets.cend(),
				[&preset](const BackgroundPreset &existing) { return existing.name == preset.name; }))
			continue;

		/* The library's own presets are never "linked"; see presetsFromData. */
		preset.linked = false;
		presets.append(preset);
	}

	return presets;
}

/*
 * The rename trail's mechanics, written once and used by both collections.
 *
 * Kept as free functions over a trail rather than duplicated per kind, because the three rules that
 * make a trail honest -- collapse the chains, drop the entry when a name is created again, never
 * follow a cycle -- are exactly the rules whether the name belonged to a typeface or to a panel,
 * and two copies of them is two places for one of the three to be forgotten.
 */
void recordRename(QVector<QPair<QString, QString>> &trail, const QString &from, const QString &to)
{
	/*
	 * Chains are collapsed rather than stacked: after A->B and then B->C, a document still
	 * carrying A has to reach C in one lookup, and B->C has to keep working for one that
	 * stopped at B. So every entry pointing at the old name is re-pointed at the new one,
	 * and the hop just made is added to them.
	 */
	for (QPair<QString, QString> &rename : trail) {
		if (rename.second == from)
			rename.second = to;
	}

	trail.append({from, to});

	/* A rename back to a name already in the trail leaves an entry pointing at itself. */
	trail.erase(std::remove_if(trail.begin(), trail.end(),
				   [](const QPair<QString, QString> &rename) { return rename.first == rename.second; }),
		    trail.end());
}

/*
 * A name that exists again is nobody's old name. Without this, a preset renamed away and then
 * re-created under the original name would send every roll bound to it off to the renamed one --
 * the trail would be describing a preset that is no longer gone.
 */
void dropRenameFrom(QVector<QPair<QString, QString>> &trail, const QString &name)
{
	trail.erase(std::remove_if(trail.begin(), trail.end(),
				   [&name](const QPair<QString, QString> &rename) { return rename.first == name; }),
		    trail.end());
}

bool followRename(const QVector<QPair<QString, QString>> &trail, const QString &from, QString *to)
{
	QString current = from;

	/*
	 * One hop is all a collapsed trail should ever need. The loop is here because the file can be
	 * hand-edited, and a cycle in it must not become a hang inside a render path's caller.
	 */
	for (int hops = 0; hops < trail.size() + 1; ++hops) {
		bool moved = false;

		for (const QPair<QString, QString> &rename : trail) {
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

QVector<QPair<QString, QString>> renamesFromData(obs_data_t *data, const char *key)
{
	QVector<QPair<QString, QString>> renames;

	OBSDataArrayAutoRelease array = obs_data_get_array(data, key);
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

void saveRenames(obs_data_t *data, const char *key, const QVector<QPair<QString, QString>> &renames)
{
	OBSDataArrayAutoRelease renameArray = obs_data_array_create();
	for (const QPair<QString, QString> &rename : renames) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "from", rename.first.toUtf8().constData());
		obs_data_set_string(item, "to", rename.second.toUtf8().constData());
		obs_data_array_push_back(renameArray, item);
	}

	obs_data_set_array(data, key, renameArray);
}

QString presetsToJson(const QVector<StylePreset> &presets, const QVector<BackgroundPreset> &backgrounds,
		      const QVector<QPair<QString, QString>> &renames,
		      const QVector<QPair<QString, QString>> &backgroundRenames, bool alwaysEditLinked)
{
	OBSDataAutoRelease data = obs_data_create();
	OBSDataArrayAutoRelease array = obs_data_array_create();

	for (const StylePreset &preset : presets) {
		OBSDataAutoRelease item = obs_data_create();
		preset.save(item);
		obs_data_array_push_back(array, item);
	}

	obs_data_set_array(data, "style_presets", array);

	OBSDataArrayAutoRelease backgroundArray = obs_data_array_create();
	for (const BackgroundPreset &preset : backgrounds) {
		OBSDataAutoRelease item = obs_data_create();
		preset.save(item);
		obs_data_array_push_back(backgroundArray, item);
	}

	obs_data_set_array(data, "background_presets", backgroundArray);

	saveRenames(data, "renames", renames);
	saveRenames(data, "background_renames", backgroundRenames);

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
		backgroundEntries.clear();
		renameTrail.clear();
		backgroundRenameTrail.clear();
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
	backgroundEntries = backgroundsFromData(data);
	renameTrail = renamesFromData(data, "renames");
	backgroundRenameTrail = renamesFromData(data, "background_renames");
	editLinkedInPlace = obs_data_get_bool(data, "always_edit_linked");
	bumpLocked();

	obs_log(LOG_INFO, "style library: %d preset(s), %d background(s)", entries.size(), backgroundEntries.size());
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
		json = presetsToJson(entries, backgroundEntries, renameTrail, backgroundRenameTrail, editLinkedInPlace);
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

		dropRenameFrom(renameTrail, name);

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

		recordRename(renameTrail, from, to);

		bumpLocked();
	}

	save();
}

bool StyleLibrary::renamedTo(const QString &from, QString *to) const
{
	if (from.isEmpty())
		return false;

	std::lock_guard<std::mutex> lock(libraryMutex());
	return followRename(renameTrail, from, to);
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

QVector<BackgroundPreset> StyleLibrary::backgrounds() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return backgroundEntries;
}

bool StyleLibrary::findBackground(const QString &name, BackgroundPanel *panel) const
{
	if (name.isEmpty())
		return false;

	std::lock_guard<std::mutex> lock(libraryMutex());
	for (const BackgroundPreset &preset : backgroundEntries) {
		if (preset.name != name)
			continue;

		if (panel)
			*panel = preset.panel;
		return true;
	}

	return false;
}

bool StyleLibrary::containsBackground(const QString &name) const
{
	return findBackground(name, nullptr);
}

void StyleLibrary::setBackground(const QString &name, const BackgroundPanel &panel)
{
	if (name.isEmpty())
		return;

	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		bool replaced = false;
		for (BackgroundPreset &preset : backgroundEntries) {
			if (preset.name != name)
				continue;
			preset.panel = panel;
			replaced = true;
			break;
		}

		if (!replaced)
			backgroundEntries.append(BackgroundPreset{name, panel, false});

		dropRenameFrom(backgroundRenameTrail, name);

		bumpLocked();
	}

	save();
}

void StyleLibrary::removeBackground(const QString &name)
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		const auto before = backgroundEntries.size();
		backgroundEntries.erase(
			std::remove_if(backgroundEntries.begin(), backgroundEntries.end(),
				       [&name](const BackgroundPreset &preset) { return preset.name == name; }),
			backgroundEntries.end());
		if (backgroundEntries.size() == before)
			return;

		bumpLocked();
	}

	save();
}

void StyleLibrary::renameBackground(const QString &from, const QString &to)
{
	if (from.isEmpty() || to.isEmpty() || from == to)
		return;

	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		/* Renaming onto an existing name would leave two presets nothing could tell apart. */
		if (std::any_of(backgroundEntries.cbegin(), backgroundEntries.cend(),
				[&to](const BackgroundPreset &preset) { return preset.name == to; }))
			return;

		bool renamed = false;
		for (BackgroundPreset &preset : backgroundEntries) {
			if (preset.name != from)
				continue;
			preset.name = to;
			renamed = true;
			break;
		}

		if (!renamed)
			return;

		recordRename(backgroundRenameTrail, from, to);

		bumpLocked();
	}

	save();
}

bool StyleLibrary::backgroundRenamedTo(const QString &from, QString *to) const
{
	if (from.isEmpty())
		return false;

	std::lock_guard<std::mutex> lock(libraryMutex());
	return followRename(backgroundRenameTrail, from, to);
}

QVector<QPair<QString, QString>> StyleLibrary::backgroundRenames() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return backgroundRenameTrail;
}

void StyleLibrary::replaceAllBackgrounds(const QVector<BackgroundPreset> &presets)
{
	{
		std::lock_guard<std::mutex> lock(libraryMutex());

		backgroundEntries.clear();
		backgroundEntries.reserve(presets.size());

		for (const BackgroundPreset &preset : presets) {
			if (preset.name.isEmpty())
				continue;
			if (std::any_of(backgroundEntries.cbegin(), backgroundEntries.cend(),
					[&preset](const BackgroundPreset &existing) {
						return existing.name == preset.name;
					}))
				continue;

			backgroundEntries.append(BackgroundPreset{preset.name, preset.panel, false});
		}

		bumpLocked();
	}

	save();
}

QString StyleLibrary::toJson() const
{
	std::lock_guard<std::mutex> lock(libraryMutex());
	return presetsToJson(entries, backgroundEntries, renameTrail, backgroundRenameTrail, editLinkedInPlace);
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

bool StyleLibrary::parseJson(const QString &json, QVector<StylePreset> *presets, QVector<BackgroundPreset> *backgrounds,
			     QString *error)
{
	OBSDataAutoRelease data = obs_data_create_from_json(json.toUtf8().constData());
	if (!data) {
		if (error)
			*error = QStringLiteral("The file is not valid Closing Time JSON.");
		return false;
	}

	QVector<StylePreset> parsed = presetsFromData(data);
	QVector<BackgroundPreset> parsedBackgrounds = backgroundsFromData(data);

	/*
	 * A whole exported document is a reasonable thing to point the importer at -- its presets
	 * are under the same keys -- but a file with neither styles nor panels in it is a mistake
	 * worth naming rather than an import that silently does nothing.
	 *
	 * Either collection on its own is enough. A library of nothing but cards is a perfectly good
	 * thing to publish, and refusing it because it named no typeface would make the panels the
	 * lesser half of a feature that is meant to stand beside the styles.
	 */
	if (parsed.isEmpty() && parsedBackgrounds.isEmpty()) {
		if (error)
			*error = QStringLiteral("That file holds no style presets or backgrounds.");
		return false;
	}

	if (presets)
		*presets = parsed;
	if (backgrounds)
		*backgrounds = parsedBackgrounds;

	return true;
}

void StyleLibrary::bumpLocked()
{
	++librarySerial;
}

} // namespace closingtime
