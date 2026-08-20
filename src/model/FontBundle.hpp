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

#include <obs.h>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace closingtime {

/*
 * A font file carried by the document.
 *
 * A style names a font by family, and a family name means nothing on a machine that does not have
 * that font: Qt substitutes something, the roll still renders, and every line is a different width
 * than the one it was designed at. Naming the missing family in the log says what happened but
 * does not fix it, so the file itself travels with the roll -- inside the source's settings, which
 * is what makes the scene collection the one thing that has to be copied.
 *
 * The bytes are the file's own, verbatim. Nothing is subset, re-encoded or repacked: a font is a
 * program as much as a table of outlines, and the only version of it certain to render what the
 * designer saw is the one they had.
 */
struct BundledFont {
	/* The family this file was collected for, as the document's styles name it. */
	QString family;
	/* The file's own base name. Carried for the log and for the cache file's extension. */
	QString fileName;
	QByteArray data;

	/*
	 * Content hash, hex. Names the extracted cache file and is how two rolls that carry the
	 * same font register it once between them rather than once each.
	 *
	 * Kept once it has been taken, because the registration in front of every render would
	 * otherwise hash the whole bundle again each time it is asked.
	 */
	QString hash() const;

	bool isEmpty() const { return data.isEmpty(); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

private:
	mutable QString cachedHash;
};

/*
 * A family this machine cannot supply, and the installed family standing in for it.
 *
 * The other half of the answer, for the fonts that cannot travel: most commercial licences
 * forbid passing the file on, and a foundry's webfont is often not a file at all. Choosing the
 * stand-in explicitly is still better than letting Qt pick, because the choice is recorded --
 * it renders the same on every machine that opens the collection, including the one it was
 * made on, so what goes to air is what was approved rather than whatever each machine's font
 * matching happened to land on.
 */
struct FontSubstitution {
	QString from;
	QString to;

	bool operator==(const FontSubstitution &other) const { return from == other.from && to == other.to; }
	bool operator!=(const FontSubstitution &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * Largest single file that will be carried, and largest a whole bundle may grow to.
 *
 * A scene collection is rewritten by OBS on every save and read back on every load, so the cost
 * of a bundle is paid over and over rather than once. A CJK family runs to 20 MB and a variable
 * font's whole design space to more; past these the roll keeps the family name, reports it as
 * unbundled, and the user is left to install the font or choose a stand-in -- which is a worse
 * outcome than bundling, and a much better one than a scene collection that takes a minute to
 * save.
 */
constexpr qint64 kMaxBundledFontBytes = 8 * 1024 * 1024;
constexpr qint64 kMaxFontBundleBytes = 32 * 1024 * 1024;

/*
 * Reads the files behind each family off this machine, ready to be carried with the document.
 *
 * Every file declaring the family comes along, not just the one Qt would pick: the regular, the
 * bold and the italic of a family are separate files, and a roll that sets one heading bold needs
 * the file the bold is in. Families with no file to be found -- a generic, a webfont, one this
 * machine does not have either -- contribute nothing and are simply absent from the result.
 *
 * `skipped`, when given, receives the families a file was found for but not carried, because that
 * one file or the bundle as a whole would have gone past the caps above.
 */
QVector<BundledFont> collectBundledFonts(const QStringList &families, QStringList *skipped = nullptr);

/* Total size of the files in a bundle, in bytes. */
qint64 fontBundleBytes(const QVector<BundledFont> &fonts);

} // namespace closingtime
