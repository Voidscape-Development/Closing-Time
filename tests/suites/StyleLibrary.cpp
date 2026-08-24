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
#include <QFile>
#include <QTemporaryDir>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"
#include "model/StyleLibrary.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/*
 * Points the process-wide library at a file of this run's own, and puts it back afterwards.
 *
 * The library is a singleton because a machine has one of it, which makes it exactly the kind of
 * thing a test has to be careful with: every suite here shares it, so each one that touches it
 * owns it only for as long as this is in scope, and leaves it empty and unpointed on the way out.
 */
class ScopedLibrary {
public:
	ScopedLibrary()
	{
		StyleLibrary::instance().setFilePath(dir.filePath(QStringLiteral("style-presets.json")));
		StyleLibrary::instance().load();
	}

	~ScopedLibrary()
	{
		StyleLibrary::instance().replaceAll({});
		StyleLibrary::instance().setFilePath(QString());
	}

	QString path() const { return dir.filePath(QStringLiteral("style-presets.json")); }

	bool isValid() const { return dir.isValid(); }

private:
	QTemporaryDir dir;
};

TextStyle styleAt(int pixelSize, const QColor &color)
{
	TextStyle style;
	style.family = QStringLiteral("Sans Serif");
	style.pixelSize = pixelSize;
	style.color = color;
	return style;
}

/* A document with one heading bound to a preset of the given name. */
Document boundDocument(const QString &presetName, const TextStyle &style, bool linked)
{
	Section section = unpadded(SectionType::Title);
	section.text = QStringLiteral("Closing Time");
	section.stylePresetName = presetName;

	Document document = documentWith(section);
	document.stylePresets = {StylePreset{presetName, style, linked}};
	return document;
}

/*
 * A document that binds the same preset from all three slots a section has -- its own style, its
 * secondary style and its bridge's -- so a migration that rewrites one and forgets the others is
 * a failure rather than a coin toss about which slot the check happened to look at.
 */
Document boundEverywhere(const QString &presetName, bool linked)
{
	Section bridged = unpadded(SectionType::Bridged);
	bridged.entries = {Entry{QStringLiteral("Director"), QStringLiteral("Jane Doe"), {}, {}, LogoRef()}};
	bridged.stylePresetName = presetName;
	bridged.secondaryStylePresetName = presetName;
	bridged.bridgeStylePresetName = presetName;

	Document document = documentWith(bridged);
	document.stylePresets = {StylePreset{presetName, styleAt(28, Qt::white), linked}};
	return document;
}

} // namespace

CT_SUITE(style_library_storage, "Adding, finding, renaming and removing library presets")
{
	ScopedLibrary scoped;
	check(scoped.isValid(), "a temporary library directory was made");
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();

	check(library.presets().isEmpty(), "a library with no file behind it is empty rather than broken");
	check(!library.contains(QStringLiteral("House")), "and holds nothing");

	library.set(QStringLiteral("House"), styleAt(48, QColor(255, 210, 90)));
	check(library.contains(QStringLiteral("House")), "a published style is in the library");
	checkEq(library.presets().size(), 1, "and is the only one there");
	check(QFile::exists(scoped.path()), "publishing wrote the file");

	TextStyle found;
	check(library.find(QStringLiteral("House"), &found), "the style comes back by name");
	checkEq(found.pixelSize, 48, "with the values it was published with");

	/* Publishing over a name replaces rather than duplicating: a name is what a section binds to. */
	library.set(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)));
	checkEq(library.presets().size(), 1, "publishing an existing name replaces it");
	library.find(QStringLiteral("House"), &found);
	checkEq(found.pixelSize, 64, "with the new values");

	library.set(QStringLiteral("Roles"), styleAt(24, QColor(220, 220, 220)));
	checkEq(library.presets().size(), 2, "a second style sits beside the first");

	library.rename(QStringLiteral("House"), QStringLiteral("Titles"));
	check(library.contains(QStringLiteral("Titles")), "a rename takes");
	check(!library.contains(QStringLiteral("House")), "and the old name is gone");

	/* Renaming onto a name in use would leave two presets nothing could tell apart. */
	library.rename(QStringLiteral("Titles"), QStringLiteral("Roles"));
	check(library.contains(QStringLiteral("Titles")), "renaming onto an existing name is refused");
	checkEq(library.presets().size(), 2, "and changes nothing");

	library.remove(QStringLiteral("Roles"));
	checkEq(library.presets().size(), 1, "removing takes one out");
	check(!library.contains(QStringLiteral("Roles")), "the right one");

	/* An unnamed preset can never be bound to, so it is not allowed in. */
	library.set(QString(), styleAt(12, Qt::white));
	checkEq(library.presets().size(), 1, "an unnamed style is refused");
}

CT_SUITE(style_library_file, "The library survives a round trip through its file")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();

	TextStyle fancy = styleAt(72, QColor(255, 210, 90));
	fancy.bold = true;
	fancy.fill = TextFill::LinearGradient;
	fancy.outline.enabled = true;
	fancy.outline.width = 3.0;
	fancy.shadow.enabled = true;
	fancy.shadow.blur = 12.0;

	library.set(QStringLiteral("House"), fancy);

	const quint64 before = library.serial();

	/* Read back from disk exactly as another OBS window would read it. */
	library.load();
	check(library.serial() != before, "a reload moves the serial, which is what a watcher acts on");

	TextStyle loaded;
	check(library.find(QStringLiteral("House"), &loaded), "the style came back");
	check(loaded == fancy, "field for field, gradients and effects included");

	/* The export/import format is the same one, so a library file is a valid import. */
	QVector<StylePreset> parsed;
	QString error;
	check(StyleLibrary::parseJson(library.toJson(), &parsed, &error), "the library's own JSON parses");
	checkEq(parsed.size(), 1, "with the preset in it");

	check(!StyleLibrary::parseJson(QStringLiteral("not json at all"), &parsed, &error), "nonsense is refused");
	check(!error.isEmpty(), "with something to show the user");

	check(!StyleLibrary::parseJson(QStringLiteral("{}"), &parsed, &error), "a file with no presets is refused");

	/* A preset in the library is the library's; the flag is a property of a document's copy. */
	check(!library.presets().first().linked, "library entries are never marked linked");
}

CT_SUITE(style_library_linking, "How a linked preset resolves, and what happens when it cannot")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	library.set(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)));

	/* A document whose linked copy is out of date: refreshing is what brings it up to the library. */
	Document document = boundDocument(QStringLiteral("House"), styleAt(20, Qt::white), true);
	check(document.refreshLinkedPresets(), "a stale linked preset reports that it moved");
	checkEq(document.stylePresets.first().style.pixelSize, 64, "and now carries the library's style");
	checkEq(document.effectiveStyle(document.sections.first()).pixelSize, 64,
		"which is what the section is drawn with");

	check(!document.refreshLinkedPresets(), "refreshing again changes nothing and says so");

	/* A local preset of the same name is the document's own and is left alone. */
	Document local = boundDocument(QStringLiteral("House"), styleAt(20, Qt::white), false);
	check(!local.refreshLinkedPresets(), "an unlinked preset does not follow the library");
	checkEq(local.stylePresets.first().style.pixelSize, 20, "and keeps its own values");

	/*
	 * The fallback that makes a scene collection portable: with no library at all -- another
	 * machine, a fresh install -- a linked preset keeps the copy it was saved with rather than
	 * dropping the section back to its own untouched style.
	 */
	library.remove(QStringLiteral("House"));
	Document orphan = boundDocument(QStringLiteral("House"), styleAt(48, QColor(200, 40, 40)), true);
	check(!orphan.refreshLinkedPresets(), "a linked preset the library has lost does not move");
	checkEq(orphan.stylePresets.first().style.pixelSize, 48, "it keeps the style it is carrying");
	checkEq(orphan.effectiveStyle(orphan.sections.first()).pixelSize, 48, "and the section still draws with it");

	/* Linking asks the library for the style, so it cannot link to something that is not there. */
	Document linking = documentWith(unpadded(SectionType::Title));
	check(!linking.linkStylePreset(QStringLiteral("House")), "linking to an absent preset fails");
	check(linking.stylePresets.isEmpty(), "and adds nothing");

	library.set(QStringLiteral("House"), styleAt(30, Qt::white));
	check(linking.linkStylePreset(QStringLiteral("House")), "linking to one that exists works");
	checkEq(linking.stylePresets.size(), 1, "the document gains the preset");
	check(linking.stylePresets.first().linked, "marked as following the library");
	checkEq(linking.stylePresets.first().style.pixelSize, 30, "with the library's current values");
}

CT_SUITE(style_library_persistence, "A document remembers which of its presets follow the library")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary::instance().set(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)));

	Document document = boundDocument(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)), true);
	document.stylePresets.append(StylePreset{QStringLiteral("Local"), styleAt(18, Qt::white), false});

	Document loaded;
	check(loaded.fromJson(document.toJson()), "the document round-trips");
	checkEq(loaded.stylePresets.size(), 2, "with both presets");
	if (loaded.stylePresets.size() != 2)
		return;

	check(loaded.stylePresets.at(0).linked, "the linked one is still linked");
	check(!loaded.stylePresets.at(1).linked, "and the local one is still local");

	/*
	 * A document written before the library existed has no flag to read, and every preset in it
	 * belongs to that document alone. Loading one has to leave it that way.
	 */
	OBSDataAutoRelease legacy = obs_data_create();
	obs_data_set_string(legacy, "name", "House");
	StylePreset preset;
	preset.load(legacy);
	check(!preset.linked, "a preset from an older document is local");
}

CT_SUITE(style_library_preference, "The 'don't ask again' answer is kept with the library")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();

	check(!library.alwaysEditLinked(), "the designer asks by default");

	library.setAlwaysEditLinked(true);
	library.load();
	check(library.alwaysEditLinked(), "the answer survives a reload, which is to say it is on disk");

	library.setAlwaysEditLinked(false);
	library.load();
	check(!library.alwaysEditLinked(), "and can be turned back off");
}

CT_SUITE(style_library_rename_migration, "A renamed preset takes the rolls bound to it with it")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	library.set(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)));

	Document document = boundEverywhere(QStringLiteral("House"), true);
	library.rename(QStringLiteral("House"), QStringLiteral("Titles"));

	QString renamed;
	check(library.renamedTo(QStringLiteral("House"), &renamed), "the library remembers the rename");
	checkEq(renamed, QStringLiteral("Titles"), "and where the preset went");

	check(document.applyLibraryRenames(), "a bound document follows it");
	checkEq(document.stylePresets.first().name, QStringLiteral("Titles"), "the preset is renamed here too");
	check(document.stylePresets.first().linked, "and is still linked");

	const Section &section = document.sections.first();
	checkEq(section.stylePresetName, QStringLiteral("Titles"), "the section's own binding followed");
	checkEq(section.secondaryStylePresetName, QStringLiteral("Titles"), "so did the secondary binding");
	checkEq(section.bridgeStylePresetName, QStringLiteral("Titles"), "so did the bridge's");

	/* Which is the point of all of it: the roll is still drawn by the library's style. */
	library.set(QStringLiteral("Titles"), styleAt(90, QColor(255, 210, 90)));
	check(document.refreshLinkedPresets(), "and the roll still follows edits to it");
	checkEq(document.effectiveStyle(section).pixelSize, 90, "with the library's current values");

	check(!document.applyLibraryRenames(), "a document already migrated does not move again");
}

CT_SUITE(style_library_rename_chains, "Renaming twice, renaming back, and reusing an abandoned name")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	library.set(QStringLiteral("A"), styleAt(40, Qt::white));

	library.rename(QStringLiteral("A"), QStringLiteral("B"));
	library.rename(QStringLiteral("B"), QStringLiteral("C"));

	QString renamed;
	check(library.renamedTo(QStringLiteral("A"), &renamed), "a document that never saw B still follows");
	checkEq(renamed, QStringLiteral("C"), "straight to where the preset is now");
	check(library.renamedTo(QStringLiteral("B"), &renamed), "and one that stopped at B follows too");
	checkEq(renamed, QStringLiteral("C"), "to the same place");

	/* Two documents, each left behind at a different point in the chain. */
	Document old = boundDocument(QStringLiteral("A"), styleAt(40, Qt::white), true);
	Document halfway = boundDocument(QStringLiteral("B"), styleAt(40, Qt::white), true);
	check(old.applyLibraryRenames(), "the older document migrates");
	check(halfway.applyLibraryRenames(), "so does the newer one");
	checkEq(old.stylePresets.first().name, QStringLiteral("C"), "both land on the current name");
	checkEq(halfway.sections.first().stylePresetName, QStringLiteral("C"), "bindings included");

	/* A name that comes back is nobody's old name: the trail for it has to be dropped. */
	library.set(QStringLiteral("A"), styleAt(12, Qt::white));
	check(!library.renamedTo(QStringLiteral("A"), &renamed), "re-creating a name clears its trail");

	Document fresh = boundDocument(QStringLiteral("A"), styleAt(12, Qt::white), true);
	check(!fresh.applyLibraryRenames(), "and a roll bound to it is left where it is");

	/* Renaming back onto a name leaves nothing pointing at itself. */
	library.rename(QStringLiteral("C"), QStringLiteral("D"));
	library.rename(QStringLiteral("D"), QStringLiteral("C"));
	for (const QPair<QString, QString> &rename : library.renames())
		check(rename.first != rename.second, "no rename points at itself");
	check(!library.renamedTo(QStringLiteral("C"), &renamed), "and a round trip leaves the name alone");
}

CT_SUITE(style_library_rename_limits, "What a rename deliberately does not touch")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	library.set(QStringLiteral("House"), styleAt(64, QColor(255, 210, 90)));
	library.rename(QStringLiteral("House"), QStringLiteral("Titles"));

	/* A preset the document owns has nothing to do with a library preset of the same name. */
	Document local = boundDocument(QStringLiteral("House"), styleAt(20, Qt::white), false);
	check(!local.applyLibraryRenames(), "an unlinked preset is not renamed");
	checkEq(local.stylePresets.first().name, QStringLiteral("House"), "it keeps its name");
	checkEq(local.sections.first().stylePresetName, QStringLiteral("House"), "and its binding");

	/* Merging two presets is not a rename: the clash is left alone rather than forced. */
	Document clashing = boundDocument(QStringLiteral("House"), styleAt(20, Qt::white), true);
	clashing.stylePresets.append(StylePreset{QStringLiteral("Titles"), styleAt(80, Qt::red), false});
	check(!clashing.applyLibraryRenames(), "a rename onto a name this document already uses is skipped");
	checkEq(clashing.stylePresets.first().name, QStringLiteral("House"), "the link stays under the old name");
	checkEq(clashing.effectiveStyle(clashing.sections.first()).pixelSize, 20, "and the roll still renders");

	/* Resolve the clash and the migration happens by itself, with nothing to re-run by hand. */
	clashing.stylePresets.removeLast();
	check(clashing.applyLibraryRenames(), "once the clash is gone the link follows");
	checkEq(clashing.stylePresets.first().name, QStringLiteral("Titles"), "to the current name");

	/* A rename to a preset that has since been deleted has nothing to point a roll at. */
	library.remove(QStringLiteral("Titles"));
	Document orphan = boundDocument(QStringLiteral("House"), styleAt(48, Qt::white), true);
	check(!orphan.applyLibraryRenames(), "a rename into an empty slot is not followed");
	checkEq(orphan.effectiveStyle(orphan.sections.first()).pixelSize, 48, "and the roll renders from its copy");
}

CT_SUITE(style_library_rename_file, "The rename trail is part of the library file")
{
	ScopedLibrary scoped;
	if (!scoped.isValid())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	library.set(QStringLiteral("House"), styleAt(64, Qt::white));
	library.rename(QStringLiteral("House"), QStringLiteral("Titles"));

	/* Reloaded as another OBS window would, or as the next run of OBS will. */
	library.load();

	QString renamed;
	check(library.renamedTo(QStringLiteral("House"), &renamed), "the trail survived the file");
	checkEq(renamed, QStringLiteral("Titles"), "intact");
	checkEq(library.renames().size(), 1, "with no duplicates picked up on the way");

	/*
	 * The whole point of persisting it: a scene collection that was not open when the rename
	 * happened -- one loading now, from settings written before any of it -- still follows.
	 */
	Document saved = boundDocument(QStringLiteral("House"), styleAt(64, Qt::white), true);
	OBSDataAutoRelease settings = obs_data_create();
	saved.save(settings);

	Document reloaded;
	bool migrated = false;
	reloaded.load(settings, &migrated);
	check(migrated, "loading reports that the document was brought up to date");
	checkEq(reloaded.stylePresets.first().name, QStringLiteral("Titles"), "under the new name");
	checkEq(reloaded.sections.first().stylePresetName, QStringLiteral("Titles"), "with its binding moved");
}
