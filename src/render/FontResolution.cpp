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

#include "render/FontResolution.hpp"

#include <plugin-support.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <mutex>

namespace closingtime {

namespace {

struct FontRegistry {
	std::mutex mutex;
	QString cacheDirectory;
	/*
	 * Content hashes already dealt with, whether they were registered, skipped because the
	 * machine already had the family, or refused by Qt. One entry per file for the life of the
	 * process, so a render that happens sixty times a minute asks a hash set rather than the
	 * font database.
	 */
	QSet<QString> handled;
	/* Lower-cased families Qt has only because something here handed it a file. */
	QSet<QString> supplied;
	/*
	 * The same, one entry deeper: "family|face", lower-cased. A machine can have the family
	 * installed and be missing the one face the roll is set in, in which case the bundle is
	 * supplying that face and nothing else, and the family is not "from the bundle" at all.
	 */
	QSet<QString> suppliedFaces;
};

/* The key a family and one of its faces are remembered under. */
QString faceKey(const QString &family, const QString &styleName)
{
	return family.toLower() + QLatin1Char('|') + styleName.toLower();
}

/*
 * True when every face this file declares is one the machine can already draw, so registering it
 * would add nothing.
 *
 * A file that declares no faces at all -- a bundle written before they were recorded -- says
 * nothing either way, and is treated as adding nothing, which is the behavior it already had.
 */
bool suppliesNothingMissing(const BundledFont &font)
{
	for (const QString &styleName : font.styleNames) {
		if (!styleName.isEmpty() && !fontStyleAvailable(font.family, styleName))
			return false;
	}

	return true;
}

FontRegistry &registry()
{
	static FontRegistry instance;
	return instance;
}

/*
 * Where one bundled file is extracted to. Named by content hash rather than by family, so two
 * rolls carrying the same font share the file, a font that has been updated does not overwrite
 * the version an older collection was designed against, and a family name with a slash or a
 * colon in it cannot name a path outside the cache.
 */
QString cachePathFor(const QString &directory, const BundledFont &font)
{
	QString suffix = QFileInfo(font.fileName).suffix().toLower();
	if (suffix.isEmpty() ||
	    !std::all_of(suffix.cbegin(), suffix.cend(), [](QChar c) { return c.isLetterOrNumber(); }))
		suffix = QStringLiteral("ttf");

	return QStringLiteral("%1/%2.%3").arg(directory, font.hash(), suffix);
}

/* Writes the file unless it is already there with the right size. Returns the path, or empty. */
QString extract(const QString &directory, const BundledFont &font)
{
	if (!QDir().mkpath(directory))
		return QString();

	const QString path = cachePathFor(directory, font);

	const QFileInfo existing(path);
	if (existing.isFile() && existing.size() == font.data.size())
		return path;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return QString();

	const qint64 written = file.write(font.data);
	file.close();

	if (written != font.data.size()) {
		file.remove();
		return QString();
	}

	return path;
}

} // namespace

bool fontFamilyAvailable(const QString &family)
{
	if (family.isEmpty())
		return true;

	/*
	 * Qt maps these onto whatever the platform's default is for the category, so they are
	 * never a substitution there is anything to be done about.
	 */
	static const QStringList generics = {
		QStringLiteral("Sans Serif"), QStringLiteral("Serif"),   QStringLiteral("Monospace"),
		QStringLiteral("Cursive"),    QStringLiteral("Fantasy"), QStringLiteral("System"),
	};

	for (const QString &generic : generics) {
		if (family.compare(generic, Qt::CaseInsensitive) == 0)
			return true;
	}

	return QFontDatabase::hasFamily(family);
}

QStringList fontStyleNames(const QString &family)
{
	if (family.isEmpty())
		return QStringList();

	return QFontDatabase::styles(family);
}

bool fontStyleAvailable(const QString &family, const QString &styleName)
{
	if (styleName.isEmpty())
		return true;

	/*
	 * Compared case-insensitively because the name travels in the document as text and comes
	 * back on a machine whose copy of the family may spell it differently -- "SemiBold" against
	 * "Semibold" is the same face, and refusing it would drop the roll to Regular for a capital.
	 */
	for (const QString &candidate : fontStyleNames(family)) {
		if (candidate.compare(styleName, Qt::CaseInsensitive) == 0)
			return true;
	}

	return false;
}

QStringList missingFontFamilies(const Document &document)
{
	QStringList missing;

	for (const QString &family : document.usedFontFamilies()) {
		if (!fontFamilyAvailable(family))
			missing.append(family);
	}

	return missing;
}

QStringList unresolvedFontFamilies(const Document &document)
{
	QStringList unresolved;

	for (const QString &family : missingFontFamilies(document)) {
		if (document.fontSubstitute(family).isEmpty())
			unresolved.append(family);
	}

	return unresolved;
}

QVector<FontStatus> fontStatus(const Document &document)
{
	QHash<QString, qint64> bundled;
	for (const BundledFont &font : document.bundledFonts)
		bundled[font.family] += font.data.size();

	QVector<FontStatus> statuses;
	for (const FontUse &use : document.usedFonts()) {
		FontStatus status;
		status.family = use.family;
		status.available = fontFamilyAvailable(use.family);
		status.fromBundle = fontFamilySuppliedByBundle(use.family);
		status.bundled = bundled.contains(use.family);
		status.bundledBytes = bundled.value(use.family);
		status.substitute = document.fontSubstitute(use.family);

		for (const QString &styleName : use.styleNames) {
			/*
			 * The family's default face is not a face anybody chose, and the family row
			 * already answers for it. A row saying "Regular: installed" under every family
			 * would be noise on every roll that has never opened the picker.
			 */
			if (styleName.isEmpty())
				continue;

			FontFaceStatus face;
			face.styleName = styleName;
			face.available = fontStyleAvailable(use.family, styleName);
			face.fromBundle = fontFaceSuppliedByBundle(use.family, styleName);

			for (const BundledFont &font : document.bundledFonts)
				face.bundled = face.bundled || font.supplies(use.family, styleName);

			status.faces.append(face);
		}

		statuses.append(status);
	}

	return statuses;
}

void setFontCacheDirectory(const QString &directory)
{
	FontRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	state.cacheDirectory = directory;
}

QString fontCacheDirectory()
{
	FontRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.cacheDirectory;
}

QStringList installBundledFonts(const QVector<BundledFont> &fonts)
{
	if (fonts.isEmpty())
		return QStringList();

	FontRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);

	QStringList registered;

	for (const BundledFont &font : fonts) {
		if (font.isEmpty() || font.family.isEmpty())
			continue;

		/*
		 * Asked before the hash, and per file rather than once per family. Before, because
		 * this call sits in front of every render and hashing a bundle nobody needs would be
		 * megabytes of work per keystroke; on the machine a roll was designed on, where every
		 * family and every face is installed, it means no hashing at all. Per file, because the
		 * four files of a family arrive together and the first of them is what makes the name
		 * available.
		 *
		 * A family this machine already has is left to the installed file -- two families under
		 * one name says nothing about which a style meant -- *unless* this file carries a face
		 * of it that the machine does not have. Having Inter installed and not its semibold is
		 * an ordinary state for a machine to be in, and it is the one case where the installed
		 * family and the bundled file are not answering the same question: the file is the only
		 * thing that can supply that face, and skipping it leaves the roll a weight off with
		 * nothing said about it.
		 */
		if (fontFamilyAvailable(font.family) && suppliesNothingMissing(font))
			continue;

		const QString hash = font.hash();
		if (state.handled.contains(hash))
			continue;

		state.handled.insert(hash);

		int id = -1;
		if (!state.cacheDirectory.isEmpty()) {
			const QString path = extract(state.cacheDirectory, font);
			if (!path.isEmpty())
				id = QFontDatabase::addApplicationFont(path);
		} else {
			/* Nowhere to write: the bytes are handed straight to Qt instead. */
			id = QFontDatabase::addApplicationFontFromData(font.data);
		}

		if (id < 0) {
			obs_log(LOG_WARNING, "the font bundled for '%s' ('%s') could not be read",
				font.family.toUtf8().constData(), font.fileName.toUtf8().constData());
			continue;
		}

		for (const QString &family : QFontDatabase::applicationFontFamilies(id)) {
			state.supplied.insert(family.toLower());
			if (!registered.contains(family))
				registered.append(family);

			/*
			 * Asked of Qt now that the file is in, rather than taken from what the file
			 * declared: the name table says what the designer of the font called a face,
			 * and this says what the roll will find when it goes looking for one.
			 */
			for (const QString &styleName : QFontDatabase::styles(family))
				state.suppliedFaces.insert(faceKey(family, styleName));
		}
	}

	return registered;
}

bool fontFamilySuppliedByBundle(const QString &family)
{
	if (family.isEmpty())
		return false;

	FontRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.supplied.contains(family.toLower());
}

bool fontFaceSuppliedByBundle(const QString &family, const QString &styleName)
{
	if (family.isEmpty())
		return false;

	/* The family's default face is the family's business, and is answered for by the family. */
	if (styleName.isEmpty())
		return fontFamilySuppliedByBundle(family);

	FontRegistry &state = registry();
	const std::lock_guard<std::mutex> lock(state.mutex);
	return state.suppliedFaces.contains(faceKey(family, styleName));
}

QStringList installDocumentFonts(const Document &document)
{
	return installBundledFonts(document.bundledFonts);
}

const Document &documentWithFontsResolved(const Document &document, Document &storage)
{
	installDocumentFonts(document);

	if (document.fontSubstitutions.isEmpty())
		return document;

	/*
	 * Asked after the bundle is registered, so a family the roll carries its own file for is
	 * not missing any more and never reaches a stand-in.
	 */
	const QStringList missing = missingFontFamilies(document);
	if (missing.isEmpty())
		return document;

	storage = document;
	if (!storage.applyFontSubstitutions(missing))
		return document;

	return storage;
}

} // namespace closingtime
