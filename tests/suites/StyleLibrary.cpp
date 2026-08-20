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
