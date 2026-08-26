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

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * The machine-wide style library.
 *
 * Presets on a document belong to that document, which means a house style has to be rebuilt in
 * every source that wants it and re-edited in every source when it changes. The library is the
 * other half: one file under the plugin's config directory, shared by every source, every scene
 * collection and every profile on the machine, holding the styles worth having in more than one
 * roll.
 *
 * A document does not copy from it and then forget where the style came from. A section binds to
 * a preset by name exactly as it always has; the document's own preset entry is marked `linked`,
 * and `Document::refreshLinkedPresets()` pulls the library's current values into those entries
 * whenever the library changes. Two things fall out of doing it that way rather than resolving
 * through the library at paint time. The renderer never learns the library exists -- it reads the
 * document's presets, as it always did, off the render thread with nothing to lock -- and the
 * copy left in the document is exactly the fallback a scene collection needs when it is opened on
 * a machine that has no library file.
 *
 * The file path is injected rather than read from obs_module_config_path() here, because this
 * class is compiled into the test harness, which has no OBS module to ask. plugin-main sets it at
 * load; a suite points it at a temporary file, or leaves it unset and gets an empty library that
 * touches no disk.
 */
class StyleLibrary {
public:
	/*
	 * The process-wide instance. Not owned by any window or source: a designer opening is not
	 * what makes a library exist, and the source needs it whether or not one is open.
	 */
	static StyleLibrary &instance();

	/* Where the library is read from and written to. */
	void setFilePath(const QString &path);
	QString filePath() const;

	/* Reads the file, replacing what is held. Missing file is not an error: it is an empty library. */
	void load();

	/* Writes the file, creating its directory. Returns false and logs when it cannot be written. */
	bool save();

	/*
	 * Re-reads the file if it changed on disk since it was last read, and returns true when it
	 * did.
	 *
	 * A second OBS window, a hand edit or a library imported from elsewhere all change the file
	 * underneath a running source. A stat per call is cheap for a watcher but not for a
	 * per-frame tick, so calls are rate-limited inside: asking every frame costs one stat a
	 * second and nothing else.
	 */
	bool pollForChanges();

	/* Moves whenever the contents do, from an edit here or a reload from disk. */
	quint64 serial() const;

	QVector<StylePreset> presets() const;
	/* Writes the named style into `style` and returns true, or returns false and leaves it alone. */
	bool find(const QString &name, TextStyle *style) const;
	bool contains(const QString &name) const;

	/* Adds `name` or replaces the style already under it, then writes the file. */
	void set(const QString &name, const TextStyle &style);
	void remove(const QString &name);

	/*
	 * Renames a preset, and records that it happened.
	 *
	 * A binding is a name, so a rename on its own would leave every roll in every scene
	 * collection on the machine pointing at a name that is no longer there. They would keep
	 * rendering -- each carries its own copy -- but they would have quietly stopped following the
	 * library, which is the one thing linking them was for.
	 *
	 * So the rename is kept: `renamedTo()` reports it, and a document brought up to date against
	 * the library rewrites its own preset and its sections' bindings to match (see
	 * `Document::applyLibraryRenames`). A collection that is open migrates within the second; one
	 * that is not migrates when it is next loaded, whenever that is. Nothing reaches into another
	 * collection's file behind OBS's back to do it.
	 */
	void rename(const QString &from, const QString &to);

	/*
	 * Follows the recorded renames from `from` to the name that preset carries now, and returns
	 * true when there was one. Chains are collapsed as they are recorded, so this is a lookup
	 * rather than a walk; the loop guard is there for a file that was hand-edited into a cycle.
	 */
	bool renamedTo(const QString &from, QString *to) const;

	/* Every recorded rename, oldest first, as {from, to}. For the tests and the file. */
	QVector<QPair<QString, QString>> renames() const;
	/* Replaces the whole library in one write, for the manager dialog's import. */
	void replaceAll(const QVector<StylePreset> &presets);

	/* --- backgrounds ------------------------------------------------------------------------ */

	/*
	 * The same library, one collection over: named panels sections bind a background slot to.
	 *
	 * A second collection in the same file rather than a second file, because a house style is one
	 * thing to publish, back up and carry between machines whether it is a typeface or the card
	 * behind it. A second *collection* rather than a field on StylePreset, because the two are
	 * genuinely different decisions -- see Document::backgroundPresets -- and because a shared
	 * namespace would make a background called "Card" and a text style called "Card" collide over
	 * a name neither of them chose.
	 *
	 * Everything below means exactly what its text-style twin above means, down to the rename
	 * trail, which is kept separately for the same reason the collection is.
	 */
	QVector<BackgroundPreset> backgrounds() const;
	bool findBackground(const QString &name, BackgroundPanel *panel) const;
	bool containsBackground(const QString &name) const;

	void setBackground(const QString &name, const BackgroundPanel &panel);
	void removeBackground(const QString &name);
	void renameBackground(const QString &from, const QString &to);
	bool backgroundRenamedTo(const QString &from, QString *to) const;
	QVector<QPair<QString, QString>> backgroundRenames() const;
	void replaceAllBackgrounds(const QVector<BackgroundPreset> &presets);

	/*
	 * Whether editing a style bound to a linked preset edits the library rather than forking a
	 * copy into the document.
	 *
	 * The designer asks the first time it matters, because a machine-wide restyle is not
	 * something to do by accident while nudging one roll's title. This is where the "don't ask
	 * again" answer is kept: it is a preference about the library, so it lives with the library
	 * rather than in a settings file of its own.
	 */
	bool alwaysEditLinked() const;
	void setAlwaysEditLinked(bool always);

	/*
	 * Serialises to a standalone JSON string, and back, for the manager's export and import.
	 *
	 * `parseJson` fills in whichever of the two collections the caller asks for, and succeeds when
	 * the file held *either* -- a library of nothing but panels is as importable as one of nothing
	 * but styles, and an exported document carrying both comes in whole.
	 */
	QString toJson() const;
	static bool parseJson(const QString &json, QVector<StylePreset> *presets,
			      QVector<BackgroundPreset> *backgrounds = nullptr, QString *error = nullptr);

private:
	StyleLibrary() = default;

	/* Callers must hold the mutex. */
	void bumpLocked();

	QString path;
	QVector<StylePreset> entries;
	QVector<BackgroundPreset> backgroundEntries;
	/*
	 * Old name -> current name, for presets that have been renamed. Kept for as long as the
	 * library is: a scene collection that has not been opened in a year is exactly the one that
	 * still needs the trail when it finally is. Chains are collapsed on the way in, so this never
	 * grows past one entry per name that has ever been used and abandoned.
	 */
	QVector<QPair<QString, QString>> renameTrail;
	/* The same, for the backgrounds. Separate because the two names are separate. */
	QVector<QPair<QString, QString>> backgroundRenameTrail;
	bool editLinkedInPlace = false;
	quint64 librarySerial = 0;
	/* Modification time and size of the file as last read, for pollForChanges(). */
	qint64 fileModifiedMs = -1;
	qint64 fileSize = -1;
	qint64 lastPollMs = 0;
};

} // namespace closingtime
