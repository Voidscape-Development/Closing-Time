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

#include "harness/Harness.hpp"

#include "model/CreditsModel.hpp"

using namespace closingtime;
using namespace closingtime::test;

CT_SUITE(section_types, "The type table, its ids and its predicates")
{
	const QVector<SectionType> types = allSectionTypes();
	check(!types.isEmpty(), "the designer has types to list");

	/*
	 * Ids are the contract: they are what a saved scene collection carries, and the enum may be
	 * reordered freely underneath them. A duplicate would silently alias two types onto one.
	 */
	QStringList seen;
	for (SectionType type : types) {
		const QString id = QString::fromUtf8(sectionTypeId(type));
		const Context context(id);

		check(sectionTypeFromId(id.toUtf8().constData()) == type, "the id round trips");
		check(!seen.contains(id), "the id is not already taken");
		check(!id.isEmpty(), "the type has an id at all");
		check(*sectionTypeName(type) != '\0', "the type has a display name");
		seen.append(id);
	}

	check(sectionTypeFromId("no_such_type", SectionType::Header) == SectionType::Header,
	      "an unknown id takes the caller's fallback rather than the first row of the table");
	check(sectionTypeFromId(nullptr, SectionType::Spacer) == SectionType::Spacer,
	      "a null id takes the caller's fallback");

	/* --- the predicates have to agree with each other ---------------------------------- */

	for (SectionType type : types) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));

		/* Derived rather than tabulated, so this is the derivation holding. */
		check(sectionUsesSecondaryText(type) == (type == SectionType::Bridged || sectionUsesSubtitles(type)),
		      "carrying two texts is being bridged or stacking a subtitle");

		/* Columns spread entries, so a type with columns and no entries has nothing to spread. */
		if (sectionUsesColumns(type))
			check(sectionUsesEntries(type), "a type with columns has entries to put in them");

		/* Every type that stacks a subtitle needs somewhere to draw both lines from. */
		if (sectionUsesSubtitles(type))
			check(sectionUsesText(type), "a type that stacks a subtitle sets text");
	}

	/* --- the ids of the types that exist today ----------------------------------------- */

	const QVector<QPair<SectionType, const char *>> contractual = {
		{SectionType::Title, "title"},
		{SectionType::TitleWithSubtitle, "title_with_subtitle"},
		{SectionType::TitleWithLogo, "title_with_logo"},
		{SectionType::TitleWithSubtitleAndLogo, "title_with_subtitle_logo"},
		{SectionType::LogoTitle, "logo_title"},
		{SectionType::Header, "header"},
		{SectionType::HeaderWithSubtitle, "header_with_subtitle"},
		{SectionType::HeaderWithLogo, "header_with_logo"},
		{SectionType::HeaderWithSubtitleAndLogo, "header_with_subtitle_logo"},
		{SectionType::LogoHeader, "logo_header"},
		{SectionType::Bridged, "bridged"},
		{SectionType::TextList, "text_list"},
		{SectionType::TitleSubtitleList, "title_subtitle_list"},
		{SectionType::LogoList, "logo_list"},
		{SectionType::MultiTextList, "multi_text_list"},
		{SectionType::MultiTitleSubtitleList, "multi_title_subtitle_list"},
		{SectionType::MultiLogoList, "multi_logo_list"},
		{SectionType::SectionDivider, "section_divider"},
		{SectionType::Spacer, "spacer"},
	};

	checkEq(types.size(), contractual.size(), "every type is listed here, so a new one has to be added");
	for (const auto &pair : contractual) {
		checkEq(QString::fromUtf8(sectionTypeId(pair.first)), QString::fromUtf8(pair.second),
			QStringLiteral("%1 keeps its stored id").arg(QString::fromUtf8(pair.second)));
	}
}

CT_SUITE(section_defaults, "What a freshly added section of each type carries")
{
	for (SectionType type : allSectionTypes()) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));
		const Section section = Section::makeDefault(type);

		check(section.type == type, "the default is of the type it was asked for");
		check(section.visible, "a new section is included in the roll");
		check(section.style.pixelSize > 0, "the text has a size to be drawn at");
		check(section.paddingTop >= 0 && section.paddingBottom >= 0, "padding is not negative");
		check(section.sectionWidth > 0.0 && section.sectionWidth <= 1.0, "the section box is a real share");
		check(section.columns >= 1, "there is at least one column");

		/* A type that draws from an entry list starts with something in it to see. */
		if (sectionUsesEntries(type))
			check(!section.entries.isEmpty(), "a list type starts with an entry");
		else
			check(section.entries.isEmpty(), "a non-list type starts with no entries");

		/* The placement and bridge a section added now is handed. */
		if (sectionUsesLogos(type) && sectionUsesText(type)) {
			check(section.logoPlacement == LogoPlacement::Hug,
			      "a logo row added now hugs, which is the placement that honours logoGap");
			check(section.bridgeType == BridgeType::Dots, "a logo row added now is handed drawn art");
		}

		/*
		 * A pair drawn in one style at one size is two lines that happen to touch, so every
		 * type that stacks one starts with the two told apart.
		 */
		if (sectionUsesSubtitles(type)) {
			check(section.useSecondaryStyle, "a stacked pair starts with its two styles separate");
			check(section.secondaryStyle.pixelSize < section.style.pixelSize,
			      "the subtitle starts smaller than the title");
			check(section.subtitleGap > 0, "the pair starts with a gap inside it");
		}
	}
}
