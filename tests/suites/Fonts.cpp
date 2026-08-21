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

#include <obs.hpp>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QTemporaryDir>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"
#include "model/FontBundle.hpp"
#include "render/FontResolution.hpp"
#include "util/FontFiles.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* A family no machine has, so every check about "missing" is about a family that really is. */
const QString kAbsentFamily = QStringLiteral("Closing Time Absent Face");

QString sample()
{
	return QStringLiteral("Closing Time");
}

/* A heading set in `family`, with nothing around it to answer for its ink. */
Document headingIn(const QString &family)
{
	Section section = unpadded(SectionType::Title);
	section.text = sample();
	section.style.family = family;
	section.style.pixelSize = 48;

	return documentWith(section);
}

/*
 * Points the font-file index and the extraction cache at directories of this run's own, and puts
 * both back afterwards.
 *
 * Both are process-wide, for the same reason the style library is: a machine has one set of font
 * directories and the plugin has one cache. A suite that touches either owns it for as long as
 * this is in scope.
 */
class ScopedFontPaths {
public:
	ScopedFontPaths()
	{
		previousCache = fontCacheDirectory();
		setFontCacheDirectory(dir.filePath(QStringLiteral("cache")));
	}

	~ScopedFontPaths()
	{
		setFontSearchPaths(QStringList());
		setFontCacheDirectory(previousCache);
	}

	void searchHere() { setFontSearchPaths({fontDir()}); }

	QString fontDir() const { return dir.filePath(QStringLiteral("fonts")); }
	QString cacheDir() const { return dir.filePath(QStringLiteral("cache")); }

	bool isValid() const { return dir.isValid(); }

private:
	QTemporaryDir dir;
	QString previousCache;
};

/*
 * A font file off this machine, and the family it declares.
 *
 * Real rather than generated: writing a font by hand would prove that the reader agrees with the
 * writer and nothing else, and the thing under test is whether it agrees with the fonts a machine
 * actually has. The one Qt would fall back to is deliberately avoided, since a check that installs
 * a copy of the default font is a check that cannot tell a registration from a no-op.
 */
struct SystemFont {
	QString path;
	QString family;
	/* Everything the file declares, so a check can ask whether Qt agrees with any of it. */
	QStringList families;

	bool isValid() const { return !path.isEmpty() && !family.isEmpty(); }
};

SystemFont findSystemFont()
{
	const QString fallback = QFontInfo(QFont(kAbsentFamily)).family();

	static const QStringList filters = {QStringLiteral("*.ttf"), QStringLiteral("*.otf")};

	for (const QString &directory : fontSearchPaths()) {
		QDirIterator walk(directory, filters, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
		while (walk.hasNext()) {
			const QString path = walk.next();
			if (QFileInfo(path).size() > kMaxBundledFontBytes)
				continue;

			const QStringList families = fontFamiliesInFile(path);
			if (families.isEmpty() || families.contains(fallback, Qt::CaseInsensitive))
				continue;

			return SystemFont{path, families.first(), families};
		}
	}

	return SystemFont();
}

/*
 * An installed family whose metrics differ from what an unknown family falls back to, or empty
 * when this machine has nothing that qualifies.
 *
 * This is what makes the substitution check able to fail. A stand-in that happens to measure the
 * same as the fallback would make "the substitution was applied" and "nothing happened" the same
 * picture, and the check would pass either way.
 */
QString distinctFamily()
{
	QFont absent(kAbsentFamily);
	absent.setPixelSize(48);
	const qreal fallbackWidth = QFontMetricsF(absent).horizontalAdvance(sample());

	for (const QString &family : QFontDatabase::families()) {
		QFont candidate(family);
		candidate.setPixelSize(48);
		if (std::abs(QFontMetricsF(candidate).horizontalAdvance(sample()) - fallbackWidth) > 2.0)
			return family;
	}

	return QString();
}

/*
 * An installed family that ships a bold face of its own, and whose bold measures differently
 * from its regular, or empty when this machine has none.
 *
 * Both halves matter. A family whose bold is synthesised would make "the named face was used" and
 * "the bold flag was used" the same picture, and a family whose two faces happen to measure alike
 * would make the check pass whichever of them was drawn.
 */
QString facedFamily()
{
	for (const QString &family : QFontDatabase::families()) {
		bool hasBold = false;
		for (const QString &face : QFontDatabase::styles(family))
			hasBold = hasBold ||
				  (QFontDatabase::bold(family, face) && !QFontDatabase::italic(family, face));

		if (!hasBold)
			continue;

		/*
		 * Measured off the rendered strip rather than off QFontMetricsF, because the strip is
		 * what the checks compare. A family whose bold is a wider advance but the same run of
		 * inked pixels would have the premise pass and every check under it be untestable.
		 *
		 * A title is bold by default, so the regular is asked for rather than assumed.
		 */
		Document regular = headingIn(family);
		regular.sections[0].style.bold = false;

		Document bold = headingIn(family);
		bold.sections[0].style.bold = true;

		if (inkOf(renderImage(regular)).width() != inkOf(renderImage(bold)).width())
			return family;
	}

	return QString();
}

int filesIn(const QString &directory)
{
	return QDir(directory).entryList(QDir::Files).size();
}

} // namespace

CT_SUITE(fonts_used_families, "Which families a roll is actually set in")
{
	Section heading = unpadded(SectionType::Title);
	heading.text = sample();
	heading.style.family = QStringLiteral("Zeta Face");

	Section pair = unpadded(SectionType::TitleWithSubtitle);
	pair.text = sample();
	pair.secondaryText = QStringLiteral("and after");
	pair.style.family = QStringLiteral("Alpha Face");
	pair.secondaryStyle.family = QStringLiteral("Zeta Face");

	Document document = documentWith({heading, pair});

	QStringList families = document.usedFontFamilies();
	checkEq(families.size(), 2, "a family used twice is reported once");
	checkEq(families.value(0), QStringLiteral("Alpha Face"), "and the list is sorted");
	checkEq(families.value(1), QStringLiteral("Zeta Face"), "both ends of it");

	/* A hidden section is not going to air, so its font is nothing to go and install. */
	document.sections[1].visible = false;
	families = document.usedFontFamilies();
	checkEq(families.size(), 1, "a hidden section contributes no family");
	checkEq(families.value(0), QStringLiteral("Zeta Face"), "leaving only the one still on show");

	/* A binding is what is drawn with, so it is the preset's family that counts. */
	Section bound = unpadded(SectionType::Title);
	bound.text = sample();
	bound.style.family = QStringLiteral("Ignored Face");
	bound.stylePresetName = QStringLiteral("House");

	TextStyle preset;
	preset.family = QStringLiteral("Preset Face");

	Document boundDocument = documentWith(bound);
	boundDocument.stylePresets = {StylePreset{QStringLiteral("House"), preset, false}};

	families = boundDocument.usedFontFamilies();
	checkEq(families.size(), 1, "a bound section reports one family");
	checkEq(families.value(0), QStringLiteral("Preset Face"), "and it is the preset's, not the section's");

	/* A logo heading draws no text at all, whatever family its style happens to carry. */
	Section logo = unpadded(SectionType::LogoTitle);
	withLogo(logo);
	logo.style.family = QStringLiteral("Unused Face");
	check(documentWith(logo).usedFontFamilies().isEmpty(), "a type that draws no text reports no family");

	/* A divider is asked rather than assumed: artwork alone is not a font this roll needs. */
	Section divider = unpadded(SectionType::SectionDivider);
	divider.style.family = QStringLiteral("Divider Face");
	check(documentWith(divider).usedFontFamilies().isEmpty(), "a divider with no text reports no family");

	DividerPiece word;
	word.kind = DividerPiece::Kind::Text;
	word.text = QStringLiteral("PART II");
	divider.dividerCentre = {word};
	checkEq(documentWith(divider).usedFontFamilies().size(), 1,
		"a divider that does set text reports the family it sets it in");

	/*
	 * A bridge takes only ink from its own style and keeps the row's font, so the family named
	 * there is never drawn with -- reporting it would send the user after a font nothing uses.
	 */
	Section bridged = unpadded(SectionType::Bridged);
	bridged.entries = {Entry{QStringLiteral("Director"), QStringLiteral("Jane Doe"), LogoRef()}};
	bridged.style.family = QStringLiteral("Row Face");
	bridged.secondaryStyle.family = QStringLiteral("Row Face");
	bridged.bridgeStyle.family = QStringLiteral("Bridge Face");

	families = documentWith(bridged).usedFontFamilies();
	checkEq(families.size(), 1, "a bridged row reports the family it is set in");
	checkEq(families.value(0), QStringLiteral("Row Face"), "and not the one its bridge style carries");
}

CT_SUITE(fonts_persistence, "A bundle and its stand-ins survive the settings round trip")
{
	BundledFont font;
	font.family = QStringLiteral("Carried Face");
	font.fileName = QStringLiteral("carried.ttf");
	/* Not a font: nothing here registers it, and every byte of it has to come back. */
	font.data = QByteArray("\x00\x01\x02\xFF\xFE", 5) + QByteArray("bytes");

	Document document = headingIn(QStringLiteral("Carried Face"));
	document.bundledFonts = {font};
	document.setFontSubstitute(kAbsentFamily, QStringLiteral("Stand In"));
	document.bundleFonts = true;

	OBSDataAutoRelease data = obs_data_create();
	document.save(data);

	Document loaded;
	loaded.load(data);

	check(loaded.bundleFonts, "the bundling switch comes back");
	checkEq(loaded.bundledFonts.size(), 1, "the bundle comes back");
	checkEq(loaded.bundledFonts.value(0).family, font.family, "with the family it was collected for");
	checkEq(loaded.bundledFonts.value(0).fileName, font.fileName, "and the file it came from");
	check(loaded.bundledFonts.value(0).data == font.data, "and the file's bytes, byte for byte");
	checkEq(loaded.fontSubstitute(kAbsentFamily), QStringLiteral("Stand In"), "the stand-in comes back");

	/* Off is a choice, and a choice that survives is the only kind worth offering. */
	document.bundleFonts = false;
	OBSDataAutoRelease off = obs_data_create();
	document.save(off);
	Document loadedOff;
	loadedOff.load(off);
	check(!loadedOff.bundleFonts, "and so does having switched it off");

	/*
	 * A roll written before any of this existed has made no choice, which is not the same as
	 * having chosen not to carry anything.
	 */
	OBSDataAutoRelease old = obs_data_create();
	Document older;
	older.load(old);
	check(older.bundleFonts, "a document from before bundling existed carries fonts by default");
	check(older.bundledFonts.isEmpty(), "and carries none yet");
	check(older.fontSubstitutions.isEmpty(), "and stands nothing in");

	/* A hand-edited collection is a broken bundle, not a broken document. */
	OBSDataAutoRelease damaged = obs_data_create();
	document.bundleFonts = true;
	document.save(damaged);
	OBSDataArrayAutoRelease array = obs_data_get_array(damaged, "bundled_fonts");
	OBSDataAutoRelease first = obs_data_array_item(array, 0);
	obs_data_set_string(first, "data", "not base64 at all !!!");

	Document salvaged;
	salvaged.load(damaged);
	check(salvaged.bundledFonts.isEmpty(), "an unreadable bundle entry is dropped rather than carried");
	checkEq(salvaged.fontSubstitute(kAbsentFamily), QStringLiteral("Stand In"),
		"and the rest of the document loads around it");
}

CT_SUITE(fonts_file_index, "Finding the file a family came out of")
{
	const SystemFont installed = findSystemFont();
	check(installed.isValid(), "this machine has a font file to read");
	if (!installed.isValid())
		return;

	/*
	 * Tied to what Qt calls the font, not just to what this reader called it. A style names a
	 * family the way Qt reports it, so a reader that agreed only with itself -- reading the
	 * full name, say, or the subfamily -- would index every file under a name no lookup ever
	 * asks for, and every check here would still pass.
	 */
	bool qtKnowsOne = false;
	for (const QString &family : installed.families)
		qtKnowsOne = qtKnowsOne || QFontDatabase::hasFamily(family);

	check(qtKnowsOne, "a family read out of an installed file is a name Qt knows that font by");

	const QStringList files = fontFilesForFamily(installed.family);
	check(files.contains(installed.path), "the index finds the file the family was read out of");

	/* A lookup is by name, and a name is not a password. */
	check(fontFilesForFamily(installed.family.toUpper()).contains(installed.path),
	      "and finds it whatever case the family is asked for in");

	check(fontFilesForFamily(kAbsentFamily).isEmpty(), "a family nothing declares has no file");
	check(fontFilesForFamily(QString()).isEmpty(), "and neither has no family at all");

	/* Read from the tables rather than guessed from the file name, which is the whole point. */
	ScopedFontPaths scoped;
	check(scoped.isValid(), "a temporary font directory was made");
	if (!scoped.isValid())
		return;

	check(QDir().mkpath(scoped.fontDir()), "the temporary font directory was created");
	const QString renamed = scoped.fontDir() + QStringLiteral("/00000000.ttf");
	check(QFile::copy(installed.path, renamed), "a font copied under a name that says nothing");

	scoped.searchHere();
	checkEq(fontFilesForFamily(installed.family).value(0), renamed,
		"the family is found in a file whose name does not mention it");

	QFile notAFont(scoped.fontDir() + QStringLiteral("/broken.ttf"));
	check(notAFont.open(QIODevice::WriteOnly), "a file that is not a font was written");
	notAFont.write("this is not a font, it is a sentence");
	notAFont.close();

	check(fontFamiliesInFile(notAFont.fileName()).isEmpty(), "a file that is not a font declares nothing");
	check(fontFamiliesInFile(scoped.fontDir() + QStringLiteral("/nothing.ttf")).isEmpty(),
	      "and neither does a file that is not there");
}

CT_SUITE(fonts_collection, "Which files a roll carries with it")
{
	const SystemFont installed = findSystemFont();
	if (!installed.isValid())
		return;

	ScopedFontPaths scoped;
	if (!scoped.isValid())
		return;

	check(QDir().mkpath(scoped.fontDir()), "the temporary font directory was created");

	const QString regular = scoped.fontDir() + QStringLiteral("/regular.ttf");
	const QString second = scoped.fontDir() + QStringLiteral("/second.ttf");
	check(QFile::copy(installed.path, regular), "a font file was placed where the index will find it");
	check(QFile::copy(installed.path, second), "and a second file declaring the same family");
	scoped.searchHere();

	const QVector<BundledFont> bundle = collectBundledFonts({installed.family});
	checkEq(bundle.size(), 2, "every file declaring the family is carried, not just one of them");
	check(!bundle.value(0).isEmpty(), "with the file's bytes in it");
	checkEq(bundle.value(0).family, installed.family, "recorded under the family it was collected for");
	checkEq(static_cast<long long>(fontBundleBytes(bundle)), 2 * QFileInfo(regular).size(),
		"and the bundle is as big as the files in it");

	check(collectBundledFonts({kAbsentFamily}).isEmpty(), "a family with no file contributes nothing");

	QStringList skipped;
	collectBundledFonts({kAbsentFamily}, &skipped);
	check(skipped.isEmpty(), "a family with no file is not reported as skipped: there was nothing to refuse");

	/* A document collects for itself, through the same switch the designer offers. */
	Document document = headingIn(installed.family);
	check(document.refreshFontBundle(), "a roll with an uncarried family collects its files");
	checkEq(document.bundledFonts.size(), 2, "carrying every file the family is in");
	check(!document.refreshFontBundle(), "and asking again with nothing changed collects nothing");

	document.bundleFonts = false;
	check(document.refreshFontBundle(), "switching bundling off is a change");
	check(document.bundledFonts.isEmpty(), "and empties the bundle rather than leaving it unused");

	/*
	 * The case the whole bundle exists for: a roll opened on a machine that does not have its
	 * fonts, and then edited. Re-collecting there finds nothing -- there is nothing to find --
	 * and a refresh that took that for an answer would throw the roll's fonts away on the first
	 * edit made anywhere but the machine it was designed on.
	 */
	BundledFont foreign;
	foreign.family = kAbsentFamily;
	foreign.fileName = QStringLiteral("absent.ttf");
	foreign.data = QByteArray("pretend this is a font");

	Document travelled = headingIn(kAbsentFamily);
	travelled.bundledFonts = {foreign};

	check(!travelled.refreshFontBundle(), "a roll carrying a font this machine lacks is unchanged by a refresh");
	checkEq(travelled.bundledFonts.size(), 1, "and still carries it");

	check(!travelled.refreshFontBundle(nullptr, true), "even when the files are deliberately re-read");
	checkEq(travelled.bundledFonts.size(), 1, "which is the machine the bundle exists for");

	/* A family the roll has stopped using is a file it has stopped needing to carry. */
	travelled.sections[0].style.family = installed.family;
	check(travelled.refreshFontBundle(), "changing the family the roll is set in changes the bundle");
	check(!travelled.bundledFonts.isEmpty(), "which now carries the family it is set in");
	for (const BundledFont &font : travelled.bundledFonts)
		check(font.family != kAbsentFamily, "and no longer the one it is not");

	/* A family that has been answered with a stand-in is not one to go looking for a file for. */
	Document standing = headingIn(installed.family);
	standing.setFontSubstitute(installed.family, QStringLiteral("Sans Serif"));
	standing.refreshFontBundle();
	check(standing.bundledFonts.isEmpty(), "a family with a stand-in recorded is not collected");
}

CT_SUITE(fonts_install, "Registering the files a roll carries")
{
	const SystemFont installed = findSystemFont();
	if (!installed.isValid())
		return;

	ScopedFontPaths scoped;
	if (!scoped.isValid())
		return;

	QFile file(installed.path);
	check(file.open(QIODevice::ReadOnly), "the font file was readable");
	if (!file.isOpen())
		return;

	BundledFont carried;
	/*
	 * Filed under a family this machine does not have, so the registration is a registration:
	 * a bundle whose family is already installed is skipped by design, which would make a check
	 * about registering it a check about nothing.
	 */
	carried.family = kAbsentFamily;
	carried.fileName = QFileInfo(installed.path).fileName();
	carried.data = file.readAll();

	checkEq(filesIn(scoped.cacheDir()), 0, "the cache starts empty");

	const QStringList registered = installBundledFonts({carried});
	check(!registered.isEmpty(), "a carried file is registered, and says which families it brought");
	checkEq(filesIn(scoped.cacheDir()), 1, "and is extracted into the cache exactly once");
	check(fontFamilyAvailable(registered.value(0)), "the family it brought is available to Qt");
	check(fontFamilySuppliedByBundle(registered.value(0)), "and is known to have come from a bundle");

	check(installBundledFonts({carried}).isEmpty(), "registering the same file again does nothing");
	checkEq(filesIn(scoped.cacheDir()), 1, "and writes nothing more");

	/*
	 * The cache is named by content, so a second roll carrying the same font shares the file
	 * rather than writing its own copy of it.
	 */
	BundledFont sameFileOtherRoll = carried;
	sameFileOtherRoll.family = QStringLiteral("Closing Time Absent Face II");
	installBundledFonts({sameFileOtherRoll});
	checkEq(filesIn(scoped.cacheDir()), 1, "two rolls carrying one font share one cached file");

	BundledFont rubbish;
	rubbish.family = QStringLiteral("Closing Time Rubbish Face");
	rubbish.fileName = QStringLiteral("rubbish.ttf");
	rubbish.data = QByteArray("this is not a font either");

	check(installBundledFonts({rubbish}).isEmpty(), "a bundle that is not a font registers nothing");
	check(!fontFamilyAvailable(QStringLiteral("Closing Time Rubbish Face")),
	      "and the family it claimed stays missing rather than half-arriving");

	/* An installed family is left to the installed file: two families under one name says nothing. */
	BundledFont overriding;
	overriding.family = installed.family;
	overriding.fileName = carried.fileName;
	overriding.data = carried.data;
	check(installBundledFonts({overriding}).isEmpty(),
	      "a bundle for a family this machine already has is skipped rather than overriding it");
}

CT_SUITE(fonts_substitution, "What a roll renders as when a font cannot be had")
{
	Document document = headingIn(kAbsentFamily);
	document.sections[0].secondaryStyle.family = kAbsentFamily;

	TextStyle preset;
	preset.family = kAbsentFamily;
	document.stylePresets = {StylePreset{QStringLiteral("House"), preset, false}};

	checkEq(missingFontFamilies(document).size(), 1, "a family nothing has is reported missing");
	checkEq(unresolvedFontFamilies(document).size(), 1, "and unresolved, with nothing said about it");

	document.setFontSubstitute(kAbsentFamily, QStringLiteral("Sans Serif"));
	checkEq(missingFontFamilies(document).size(), 1, "a stand-in does not make the family present");
	check(unresolvedFontFamilies(document).isEmpty(), "but it does answer for it");

	Document rewritten = document;
	check(rewritten.applyFontSubstitutions({kAbsentFamily}), "applying the stand-in changes the document");
	checkEq(rewritten.sections.value(0).style.family, QStringLiteral("Sans Serif"),
		"the section's own style is rewritten");
	checkEq(rewritten.sections.value(0).secondaryStyle.family, QStringLiteral("Sans Serif"),
		"and so is its secondary");
	checkEq(rewritten.stylePresets.value(0).style.family, QStringLiteral("Sans Serif"),
		"and the preset a bound section would be drawn from");

	/* Only the families it was asked about: a stand-in is a fallback, not an override. */
	Document narrowed = document;
	check(!narrowed.applyFontSubstitutions({QStringLiteral("Some Other Face")}),
	      "a family that is not missing is left alone");
	checkEq(narrowed.sections.value(0).style.family, kAbsentFamily, "with its style untouched");

	document.setFontSubstitute(kAbsentFamily, QString());
	check(document.fontSubstitute(kAbsentFamily).isEmpty(), "a stand-in can be taken back off");
	checkEq(unresolvedFontFamilies(document).size(), 1, "and the family goes back to unresolved");
}

CT_SUITE(fonts_substitution_render, "A stand-in renders as the family it stands in for")
{
	const QString stand = distinctFamily();
	check(!stand.isEmpty(), "this machine has a family that measures differently from the fallback");
	if (stand.isEmpty())
		return;

	Document fallback = headingIn(kAbsentFamily);

	Document substituted = headingIn(kAbsentFamily);
	substituted.setFontSubstitute(kAbsentFamily, stand);

	const Document named = headingIn(stand);

	const Ink standingInk = inkOf(renderImage(substituted));
	const Ink namedInk = inkOf(renderImage(named));
	const Ink fallbackInk = inkOf(renderImage(fallback));

	check(!standingInk.isEmpty(), "the roll with a stand-in draws something");

	/*
	 * Written as a relation rather than as pixel counts: what a stand-in means is "render as
	 * though the style had named this family", and that holds whatever either family measures.
	 */
	checkEq(standingInk.width(), namedInk.width(), "a stand-in renders as wide as naming the family does");
	checkEq(standingInk.height(), namedInk.height(), "and as tall");
	checkEq(measure(substituted), measure(named), "and lays the roll out to the same height");

	/*
	 * And it is really doing something: without the stand-in the same document renders in
	 * whatever Qt falls back to, which `distinctFamily` chose the stand-in to differ from.
	 */
	check(standingInk.width() != fallbackInk.width(),
	      "and is not what the same roll renders as with no stand-in recorded");
}

CT_SUITE(fonts_face_persistence, "The face a style names surviving the round trip")
{
	TextStyle chosen;
	chosen.family = QStringLiteral("Some Family");
	chosen.styleName = QStringLiteral("Semibold Italic");
	chosen.bold = true;
	chosen.italic = true;
	chosen.underline = true;
	chosen.strikeOut = true;

	OBSDataAutoRelease data = obs_data_create();
	chosen.save(data);

	TextStyle loaded;
	loaded.load(data);

	checkEq(loaded.styleName, chosen.styleName, "the face's own name comes back");
	check(loaded.bold && loaded.italic, "and so do the flags that stand in for it on a machine without that face");
	check(loaded.underline, "underline comes back");
	check(loaded.strikeOut, "and so does strikeout");

	/*
	 * A style written before the picker existed names no face, and must not be given one: the
	 * family's default face is what that roll has always rendered in.
	 */
	OBSDataAutoRelease older = obs_data_create();
	obs_data_set_string(older, "family", "Some Family");
	obs_data_set_bool(older, "bold", true);

	TextStyle legacy;
	legacy.load(older);

	check(legacy.styleName.isEmpty(), "an old style names no face");
	check(legacy.bold, "and the bold flag it does carry still carries it");
	check(!legacy.underline && !legacy.strikeOut, "effects it never had are off");

	/*
	 * Two styles differing only in the face are different styles. Equality is what decides
	 * whether a strip is rebuilt, so a face-only edit that compared equal would be an edit the
	 * preview never showed.
	 */
	TextStyle plain;
	TextStyle faced = plain;
	faced.styleName = QStringLiteral("Semibold");
	check(plain != faced, "a face-only change counts as a change");

	TextStyle ruled = plain;
	ruled.underline = true;
	check(plain != ruled, "and so does an underline");
}

CT_SUITE(fonts_face_render, "A named face rendering, and falling back when it cannot be had")
{
	const QString family = facedFamily();
	check(!family.isEmpty(), "this machine has a family with a bold face of its own");
	if (family.isEmpty())
		return;

	/* A title is bold by default, so the regular has to be asked for rather than assumed. */
	Document regular = headingIn(family);
	regular.sections[0].style.bold = false;

	Document bold = headingIn(family);
	bold.sections[0].style.bold = true;

	const int regularWidth = inkOf(renderImage(regular)).width();
	const int boldWidth = inkOf(renderImage(bold)).width();
	check(regularWidth != boldWidth, "the family's bold really is a different width from its regular");

	/*
	 * The face this machine does not have. Naming one must leave the roll rendering in the
	 * nearest weight it can reach rather than dropping it to the regular: QFont::setStyleName()
	 * switches off Qt's synthetic bold, so a renderer that named the face regardless would send
	 * a roll designed in a semibold to air in the plain face with nothing said about it.
	 */
	Document absentFace = headingIn(family);
	absentFace.sections[0].style.bold = true;
	absentFace.sections[0].style.styleName = QStringLiteral("Closing Time Absent Face Name");

	checkEq(inkOf(renderImage(absentFace)).width(), boldWidth,
		"a face this machine lacks falls back to the weight the style also carries");

	/* And the face it does have is used, rather than being quietly ignored for the flags. */
	const QStringList faces = fontStyleNames(family);
	check(!faces.isEmpty(), "the family reports the faces it ships");

	QString boldFace;
	for (const QString &face : faces) {
		if (QFontDatabase::bold(family, face) && !QFontDatabase::italic(family, face)) {
			boldFace = face;
			break;
		}
	}

	check(!boldFace.isEmpty(), "one of them is its bold");
	if (boldFace.isEmpty())
		return;

	/*
	 * Named with the bold flag deliberately off, so the width can only have come from the face.
	 * With the flag left on, a renderer that ignored the name entirely would draw the same
	 * picture and the check would prove nothing.
	 */
	Document namedFace = headingIn(family);
	namedFace.sections[0].style.bold = false;
	namedFace.sections[0].style.styleName = boldFace;

	checkEq(inkOf(renderImage(namedFace)).width(), boldWidth, "naming the bold face renders bold without the flag");
	check(inkOf(renderImage(namedFace)).width() != regularWidth, "and not as the family's regular");

	check(fontStyleAvailable(family, boldFace.toUpper()),
	      "a face is recognised whatever the document spelled its capitals as");
	check(!fontStyleAvailable(family, QStringLiteral("Closing Time Absent Face Name")),
	      "and a face the family does not ship is not");
}

CT_SUITE(fonts_decorations, "Underline and strikeout, on both of the renderer's paths")
{
	/*
	 * Two letters with a gap between them, so the question can be asked of a column that the
	 * letterforms themselves never ink: a rule is the only thing that can put a pixel there.
	 */
	Section base = unpadded(SectionType::Title);
	base.text = QStringLiteral("I    I");
	base.style.pixelSize = 64;

	const auto inksGap = [](const Document &document) {
		const QImage image = renderImage(document);
		const Ink ink = inkOf(image);
		if (ink.isEmpty())
			return false;

		return inksColumn(image, (ink.left + ink.right) / 2);
	};

	check(!inksGap(documentWith(base)), "the gap between the letters is empty to start with");

	Section underlined = base;
	underlined.style.underline = true;
	check(inksGap(documentWith(underlined)), "an underline rules across the gap");

	Section struck = base;
	struck.style.strikeOut = true;
	check(inksGap(documentWith(struck)), "and so does a strikeout");

	/* An underline is drawn below the letters rather than through them. */
	const Ink plainInk = inkOf(renderImage(documentWith(base)));
	const Ink underlinedInk = inkOf(renderImage(documentWith(underlined)));
	check(underlinedInk.bottom > plainInk.bottom, "the underline sits below the letters it is under");

	/*
	 * The same, on the path that draws the effects. That one never calls QTextLayout::draw() --
	 * it works from the glyph outlines, and a rule is not a glyph -- so this is a different piece
	 * of code answering the same question, and it is the one that would silently draw nothing.
	 */
	Section outlined = base;
	outlined.style.outline.enabled = true;
	outlined.style.outline.width = 2.0;

	check(outlined.style.hasEffects(), "the outlined heading takes the effects path");
	check(!inksGap(documentWith(outlined)), "which inks nothing in the gap on its own");

	Section outlinedStruck = outlined;
	outlinedStruck.style.strikeOut = true;
	check(inksGap(documentWith(outlinedStruck)), "a strikeout is drawn there too");

	Section outlinedUnderlined = outlined;
	outlinedUnderlined.style.underline = true;
	check(inksGap(documentWith(outlinedUnderlined)), "and so is an underline");

	/*
	 * And it is drawn as part of the letterforms rather than beside them: the outline grows the
	 * ink of a struck-out heading past what the same heading inks unstruck, which it could only
	 * do if the rule went into the path the outline is stroked around.
	 */
	const Ink outlinedInk = inkOf(renderImage(documentWith(outlined)));
	const Ink outlinedStruckInk = inkOf(renderImage(documentWith(outlinedStruck)));
	check(outlinedStruckInk.width() > outlinedInk.width(),
	      "the strikeout is outlined along with the letters, so it reaches past them");
}
