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

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * What happens to a roll whose fonts this machine does not have.
 *
 * Styles name a font by family, so a scene collection carried to another machine can end up
 * rendering in whatever Qt substitutes -- every line a different width than the one it was
 * designed at, and nothing said about it. Three things happen here, in this order:
 *
 *  1. The font files the document carries are registered, which makes their families available
 *     to Qt exactly as an installed font would be.
 *  2. A family still missing is rewritten to the stand-in the document records for it, if it
 *     records one.
 *  3. Whatever is left is reported -- named in the designer, logged once by the source.
 *
 * The order is what makes each of the three a fallback for the one before it, and it is why a
 * substitution never applies on a machine that has the real font: by the time step 2 is asked,
 * the family it names is either present or it is not.
 */

/*
 * True when `family` will actually be used rather than substituted. The generic families Qt
 * resolves against the platform default are always considered available, since there is nothing
 * for the user to install.
 *
 * A family a bundle has already registered answers true here, which is the whole point of
 * registering it.
 */
bool fontFamilyAvailable(const QString &family);

/*
 * Families a document's visible text asks for that this machine cannot supply, deduplicated and
 * sorted. Preset bindings are resolved first, so only fonts that really get drawn count.
 */
QStringList missingFontFamilies(const Document &document);

/*
 * The families still missing once the document's own stand-ins are taken into account: the ones
 * nothing can be done about without installing a font. This is what the designer warns about and
 * what the source logs.
 */
QStringList unresolvedFontFamilies(const Document &document);

/* Where each family in a document stands, for the designer's font window and its warnings. */
struct FontStatus {
	QString family;
	/* Qt has it -- installed on this machine, or registered from this roll's own bundle. */
	bool available = false;
	/* The document carries the file, whether or not this machine needed it. */
	bool bundled = false;
	/*
	 * Qt has it because a bundle gave it to Qt. Told apart from `available` because on the
	 * machine the roll was designed on the two look identical from the outside, and which of
	 * them is true is exactly what the designer is being asked to check.
	 */
	bool fromBundle = false;
	/* Size of the files carried for it, in bytes. */
	qint64 bundledBytes = 0;
	/* The family standing in for it, empty when none is recorded. */
	QString substitute;

	/* Missing, with nothing recorded to put in its place. */
	bool isUnresolved() const { return !available && substitute.isEmpty(); }
};

/* One entry per family the roll's visible text is set in, in the order `usedFontFamilies` gives. */
QVector<FontStatus> fontStatus(const Document &document);

/*
 * Where extracted bundle files are kept.
 *
 * Injected rather than read from obs_module_config_path() here, for the same reason the style
 * library's path is: this code is compiled into the test harness, which has no OBS module to ask.
 * With no directory set the bundle is registered from memory instead, so a caller that has
 * nowhere to write still renders in the right font -- it just re-reads the bytes each process.
 */
void setFontCacheDirectory(const QString &directory);
QString fontCacheDirectory();

/*
 * Registers the files in `fonts` for families this machine does not already have, and returns the
 * families that this call made available. Empty on every call after the first for a given file,
 * which is what lets it sit in front of a render.
 *
 * A family the machine already has is deliberately skipped rather than overridden. Registering a
 * second file under a name Qt already knows leaves two families under one name and no way to say
 * which a style meant, and the installed one is at least the font the user chose to install.
 *
 * Nothing is ever unregistered. Removing an application font invalidates font engines that other
 * threads may be laying text out with, and the saving -- a few hundred kilobytes for the life of
 * the process -- is not worth that.
 */
QStringList installBundledFonts(const QVector<BundledFont> &fonts);

/* True when `family` is available on this machine only because a bundle registered it. */
bool fontFamilySuppliedByBundle(const QString &family);

/* `installBundledFonts(document.bundledFonts)`. */
QStringList installDocumentFonts(const Document &document);

/*
 * The document as it should actually be rendered: bundles registered, and every style set in a
 * family this machine still lacks rewritten to the stand-in recorded for it.
 *
 * `storage` is filled and returned only when something has to change. A roll whose fonts are all
 * present -- which is every roll on the machine it was designed on -- comes straight back and
 * copies nothing.
 */
const Document &documentWithFontsResolved(const Document &document, Document &storage);

} // namespace closingtime
