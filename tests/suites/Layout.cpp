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

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

using namespace closingtime;
using namespace closingtime::test;

/*
 * The invariants that hold for every section type, whatever it draws. A new type gets these for
 * free the moment it is added to the table -- which is the point of writing them against
 * `allSectionTypes()` rather than against a list.
 */

CT_SUITE(layout_every_type, "Measure/render agreement and containment, over every section type")
{
	const Document document = everySectionType();

	const Strip strip = renderStrip(document);
	checkEq(strip.height, measure(document), "the rendered strip is exactly as tall as measure() said");
	checkEq(strip.width, document.width, "the strip is the canvas width");

	const QString tiling = tilingProblem(strip);
	check(tiling.isEmpty(), QStringLiteral("the tiles partition the strip: %1").arg(tiling));

	checkEq(boxesOutsideContent(document), 0, "nothing is placed outside the content area of its own section");

	/*
	 * Every section reports its own box, so the designer's overlay can find it -- the children of
	 * a sticky block included, since those are sections too and are laid out by the same call.
	 */
	int sectionCount = 0;
	visitSections(document.sections, [&sectionCount](const Section &section) {
		if (section.visible)
			++sectionCount;
	});

	checkEq(boxesOf(document, LayoutBox::Kind::Section).size(), sectionCount, "every section reports a box");
	checkEq(boxesOf(document, LayoutBox::Kind::Content).size(), sectionCount,
		"every section reports a content area");

	saveArtifact(QStringLiteral("layout-every-type"), document);
}

CT_SUITE(layout_padding, "Padding and the section box, over every section type")
{
	for (SectionType type : allSectionTypes()) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));

		Section section = Section::makeDefault(type);
		if (sectionUsesLogos(type))
			withLogo(section);
		section.paddingTop = 40;
		section.paddingBottom = 24;

		const Document document = documentWith(section);
		const int height = measure(document);

		/* Padding is space, so it is worth exactly its own pixels and never less. */
		Section unpaddedSection = section;
		unpaddedSection.paddingTop = 0;
		unpaddedSection.paddingBottom = 0;
		checkEq(height - measure(documentWith(unpaddedSection)), 64, "padding is worth its own pixels");

		/* Nothing is drawn into either band of padding. */
		const QImage image = renderImage(document);
		const Ink ink = inkOf(image);
		if (!ink.isEmpty()) {
			check(ink.top >= section.paddingTop, "nothing is drawn into the top padding");
			check(ink.bottom < height - section.paddingBottom, "nothing is drawn into the bottom padding");
		}

		/* A hidden section contributes nothing at all. */
		Section hidden = section;
		hidden.visible = false;
		checkEq(measure(documentWith(hidden)), 0, "a hidden section takes no height");

		/* A narrower box really is narrower, and stays inside itself. */
		Section narrow = section;
		narrow.sectionWidth = 0.5;
		narrow.sectionAlign = HAlign::Left;
		const Document narrowDocument = documentWith(narrow);
		checkEq(boxesOutsideContent(narrowDocument), 0, "a narrowed section stays inside its own box");

		/*
		 * A sticky block is the one type the box does not narrow: it holds whole sections, each
		 * with a box of its own, so it spans the canvas and lets them place themselves. Asserted
		 * rather than skipped, because "this type ignores the setting" is exactly the kind of
		 * thing that should fail here if it ever stops being true quietly.
		 */
		const QRectF box = boxOf(narrowDocument, LayoutBox::Kind::Section);
		if (type == SectionType::StickyBlock) {
			checkNear(box.width(), document.width, 1.0, "a sticky block spans the canvas");
			continue;
		}

		checkNear(box.width(), document.width / 2.0, 1.0, "the section box is the share it was given");
		checkNear(box.left(), 0.0, 1.0, "a left-aligned box sits against the left edge");
	}
}

CT_SUITE(layout_empty, "Degenerate documents, which must not crash or measure negative")
{
	/* Nothing at all. */
	Document empty;
	empty.width = 1280;
	empty.height = 720;
	checkEq(measure(empty), 0, "an empty document has no height");
	check(renderStrip(empty).isEmpty(), "an empty document renders no tiles");

	/* Every type, stripped of the content it usually draws. */
	for (SectionType type : allSectionTypes()) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));

		Section section = Section::makeDefault(type);
		section.text.clear();
		section.secondaryText.clear();
		section.entries.clear();
		section.dividerCentre.clear();
		section.logo.path.clear();

		const Document document = documentWith(section);
		const int height = measure(document);

		check(height >= 0, "an emptied section does not measure negative");
		check(height >= section.paddingTop + section.paddingBottom,
		      "an emptied section never collapses below its own padding");
		checkEq(boxesOutsideContent(document), 0, "an emptied section places nothing outside itself");

		/* And with artwork pointed at a file that is not there. */
		Section missing = Section::makeDefault(type);
		missing.logo.path = missingLogoPath();
		for (Entry &entry : missing.entries)
			entry.logo.path = missingLogoPath();
		check(measure(documentWith(missing)) >= 0, "a missing image leaves the section measurable");
	}
}

CT_SUITE(layout_tiling, "A roll long enough to be cut into several tiles")
{
	QVector<Section> sections;
	for (int i = 0; i < 60; ++i) {
		Section section = Section::makeDefault(SectionType::TextList);
		section.entries = {Entry{QStringLiteral("Name %1").arg(i), {}, {}, {}, {}}};
		sections.append(section);
	}

	const Document document = documentWith(sections);
	const Strip strip = renderStrip(document);

	check(strip.height > StripRenderer::kTileHeight, "the fixture really is taller than one tile");
	check(strip.tiles.size() > 1, "a tall roll is cut into several tiles");

	const QString tiling = tilingProblem(strip);
	check(tiling.isEmpty(), QStringLiteral("the tiles partition the strip: %1").arg(tiling));

	for (const StripTile &tile : strip.tiles)
		check(tile.image.height() <= StripRenderer::kTileHeight, "no tile exceeds the cap");

	checkEq(strip.height, measure(document), "measure agrees over a multi-tile strip");

	/*
	 * A seam is invisible only if the rows either side of it carry the ink they would have
	 * carried unbroken, so a section straddling one is the case worth checking.
	 */
	checkEq(boxesOutsideContent(document), 0, "nothing escapes its section across a tile boundary");
}
