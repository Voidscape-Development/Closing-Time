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
#include <QStringList>
#include <QVector>

namespace closingtime {

/*
 * One face a font file holds: the family it belongs to and the name that family gives it.
 *
 * A file is not a family. `DejaVuSans-Bold.ttf` holds the Bold of DejaVu Sans and nothing else,
 * and a roll set in the Bold needs that file rather than any of the family's others -- which is
 * the question a bundle has to be able to answer before it can be trusted to carry a face.
 *
 * `styleName` is empty for a file that declares no subfamily at all, which means the family's
 * default face.
 */
struct FontFace {
	QString family;
	QString styleName;
};

/*
 * Which file on this machine a font family came out of.
 *
 * Qt will name every installed family but never says where any of them lives, and a family name
 * is not something that can be carried to another machine -- which is the whole of the problem
 * bundling exists to solve. So the files are found here instead, by reading each one's own `name`
 * table rather than by trusting its file name: `DejaVuSans-BoldOblique.ttf` is a guess, and a
 * foundry that names its files by serial number defeats it entirely.
 *
 * Reading the tables directly is deliberate. The alternative -- handing each candidate to
 * QFontDatabase::addApplicationFont() to ask what families it holds -- mutates a process-wide
 * database that the render thread is laying text out against, for every file in every font
 * directory on the machine. Parsing a few hundred bytes per file touches nothing.
 */

/* The directories walked when the index is built, deepest first as the platform lists them. */
QStringList fontSearchPaths();

/*
 * Every family `path` declares, read from its sfnt `name` table. Empty when the file is not a
 * TrueType/OpenType font or is too damaged to read.
 *
 * Both the legacy family (name id 1) and the typographic family (name id 16) are reported, and
 * every language they are recorded in, because which of them Qt reports as *the* family name
 * varies by platform: a lookup has to be able to find the file by whichever one it was given.
 */
QStringList fontFamiliesInFile(const QString &path);

/*
 * Every family/face pair `path` declares, read from the same `name` table as the families above.
 *
 * Both spellings of each face are reported, for the same reason both spellings of the family are.
 * A file holding Inter's semibold records the legacy pair ("Inter SemiBold", "Regular") -- which
 * is what fits the four-face model Windows was built around -- and the typographic pair
 * ("Inter", "SemiBold"), which is what the family really is. Which of the two Qt reports varies
 * by platform, so a lookup has to be able to match on either.
 */
QVector<FontFace> fontFacesInFile(const QString &path);

/*
 * The files declaring `family`, in the order the index found them. Usually more than one -- the
 * regular, the bold, the italic and the bold italic of a family are separate files, and a roll
 * that sets one word bold needs the file that has the bold in it.
 *
 * The index is built on the first call and kept for the life of the process. Building it walks
 * every font directory on the machine, which is a second or so; it is done from the designer,
 * never from the render path.
 */
QStringList fontFilesForFamily(const QString &family);

/* Drops the index, so the next lookup walks the directories again. */
void clearFontFileIndex();

/*
 * Points the index at `paths` instead of the machine's font directories, and drops what it holds.
 * An empty list restores the real ones. For the harness, which needs a directory it controls.
 */
void setFontSearchPaths(const QStringList &paths);

} // namespace closingtime
