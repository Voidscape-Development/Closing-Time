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

/* The stacked pair, as a single heading and as a list entry -- the same geometry either way. */

CT_SUITE(stack_geometry, "Two lines stacked: the gap inside a pair, and the empty-line courtesy")
{
	Section section = unpadded(SectionType::TitleWithSubtitle);
	const Document paired = documentWith(section);

	const QVector<QRectF> lines = boxesOf(paired, LayoutBox::Kind::Text);
	checkEq(lines.size(), 2, "a filled pair places two lines");
	if (lines.size() != 2)
		return;

	checkNear(lines.at(1).top() - lines.at(0).bottom(), section.subtitleGap, 0.5,
		  "the two lines are separated by subtitleGap and nothing else");
	checkNear(measure(paired), lines.at(1).bottom() - lines.at(0).top(), 1.0,
		  "the section is exactly as tall as the pair inside it");

	for (const QRectF &line : lines)
		checkNear(line.width(), paired.width, 1.0, "each line is laid out into the full content column");

	/* The gap is worth its own pixels, so zero really is no gap. */
	Section tight = section;
	tight.subtitleGap = 0;
	checkEq(measure(documentWith(tight)) + section.subtitleGap, measure(paired), "the gap is worth its own pixels");

	/*
	 * An empty line takes neither height nor gap with it. That is what lets a heading with no
	 * subtitle sit exactly where a plain Title would, rather than reserving a blank line.
	 */
	Section titleOnly = section;
	titleOnly.secondaryText.clear();

	Section plain = titleOnly;
	plain.type = SectionType::Title;

	checkEq(measure(documentWith(titleOnly)), measure(documentWith(plain)),
		"a heading with its subtitle blank is exactly as tall as a plain Title");
	checkEq(boxesOf(documentWith(titleOnly), LayoutBox::Kind::Text).size(), 1,
		"a pair with one line blank places one line");

	Section subtitleOnly = section;
	subtitleOnly.text.clear();
	checkEq(boxesOf(documentWith(subtitleOnly), LayoutBox::Kind::Text).size(), 1,
		"a pair with no title places one line");
}

CT_SUITE(stack_order, "subtitleFirst moving the placement and nothing else")
{
	Section section = unpadded(SectionType::TitleWithSubtitle);
	const Document titleFirst = documentWith(section);

	Section flippedSection = section;
	flippedSection.subtitleFirst = true;
	const Document subtitleFirst = documentWith(flippedSection);

	checkEq(measure(subtitleFirst), measure(titleFirst), "flipping the stack leaves the height alone");

	const QVector<QRectF> upright = boxesOf(titleFirst, LayoutBox::Kind::Text);
	const QVector<QRectF> flipped = boxesOf(subtitleFirst, LayoutBox::Kind::Text);
	checkEq(upright.size(), 2, "the upright pair places two lines");
	checkEq(flipped.size(), 2, "the flipped pair places two lines");
	if (upright.size() != 2 || flipped.size() != 2)
		return;

	/*
	 * The two are set at different sizes, so which box is taller says which line took the top.
	 * Comparing the boxes rather than the text is deliberate: a build that swapped the styles
	 * without swapping the texts would measure identically to one that swapped both, and only
	 * the drawn result tells them apart -- which is what the ink check below is for.
	 */
	check(upright.at(0).height() > upright.at(1).height(), "title first puts the larger line on top");
	check(flipped.at(0).height() < flipped.at(1).height(), "subtitle first puts the smaller line on top");

	/*
	 * And the *text* really moved with it. The two lines are different lengths, so the width of
	 * the ink on the top row is what says which string is up there.
	 */
	Section wide = section;
	wide.text = QStringLiteral("A Title Considerably Wider Than Its Subtitle Is");
	wide.secondaryText = QStringLiteral("short");
	wide.style.align = HAlign::Left;
	wide.secondaryStyle.align = HAlign::Left;

	Section wideFlipped = wide;
	wideFlipped.subtitleFirst = true;

	const Document wideDocument = documentWith(wide);
	const Document wideFlippedDocument = documentWith(wideFlipped);

	const Ink topUpright = inkOf(renderImage(wideDocument), boxesOf(wideDocument, LayoutBox::Kind::Text).at(0));
	const Ink topFlipped =
		inkOf(renderImage(wideFlippedDocument), boxesOf(wideFlippedDocument, LayoutBox::Kind::Text).at(0));

	check(topUpright.width() > topFlipped.width(), "the flip moves the text, not only the style");
}

CT_SUITE(stack_styles, "The subtitle's own style, and the preset bindings behind it")
{
	Section section = unpadded(SectionType::TitleWithSubtitle);
	section.text = QStringLiteral("Heading");
	section.secondaryText = QStringLiteral("Heading");

	/* Same string both lines: with the styles separated they cannot measure the same. */
	const QVector<QRectF> lines = boxesOf(documentWith(section), LayoutBox::Kind::Text);
	checkEq(lines.size(), 2, "both lines are placed");
	if (lines.size() != 2)
		return;
	check(lines.at(0).height() != lines.at(1).height(), "the secondary style really is a separate style");

	/* Turned off, the subtitle is drawn in the primary style, so the two match exactly. */
	Section shared = section;
	shared.useSecondaryStyle = false;
	const QVector<QRectF> sharedLines = boxesOf(documentWith(shared), LayoutBox::Kind::Text);
	checkEq(sharedLines.size(), 2, "both lines are still placed");
	if (sharedLines.size() == 2)
		checkNear(sharedLines.at(0).height(), sharedLines.at(1).height(), 0.5,
			  "without the override both lines take the primary style");

	/* A preset bound to the subtitle contributes its size; deleting it degrades to the section's own. */
	Document document = documentWith(section);
	TextStyle preset;
	preset.pixelSize = 11;
	document.setStylePreset(QStringLiteral("tiny"), preset);
	document.sections[0].secondaryStylePresetName = QStringLiteral("tiny");

	const QVector<QRectF> bound = boxesOf(document, LayoutBox::Kind::Text);
	if (bound.size() == 2)
		check(bound.at(1).height() < lines.at(1).height(), "a bound preset restyles the subtitle");

	Document unbound = document;
	unbound.removeStylePreset(QStringLiteral("tiny"));
	check(unbound.sections.at(0).secondaryStylePresetName.isEmpty(),
	      "deleting a preset unbinds the section rather than leaving it dangling");
	checkEq(measure(unbound), measure(documentWith(section)),
		"an unbound section falls back to the style it always had");
}

CT_SUITE(stack_shared_with_lists, "A heading's pair and a list entry's pair being the same geometry")
{
	/*
	 * The single heading and a one-entry list are laid out by the same helper, so a change that
	 * moved one without the other would show up here rather than in a screenshot months later.
	 */
	Section heading = unpadded(SectionType::TitleWithSubtitle);
	heading.text = QStringLiteral("Position");
	heading.secondaryText = QStringLiteral("Full Name");

	Section list = unpadded(SectionType::TitleSubtitleList);
	list.entries = {Entry{heading.text, heading.secondaryText, {}, {}, {}}};
	list.style = heading.style;
	list.secondaryStyle = heading.secondaryStyle;
	list.useSecondaryStyle = heading.useSecondaryStyle;
	list.subtitleGap = heading.subtitleGap;

	checkEq(measure(documentWith(heading)), measure(documentWith(list)),
		"a heading's pair measures exactly as a list's one entry does");

	const QVector<QRectF> headingLines = boxesOf(documentWith(heading), LayoutBox::Kind::Text);
	const QVector<QRectF> listLines = boxesOf(documentWith(list), LayoutBox::Kind::Text);
	checkEq(headingLines.size(), listLines.size(), "both place the same number of lines");

	for (int i = 0; i < std::min(headingLines.size(), listLines.size()); ++i) {
		checkNear(headingLines.at(i).top(), listLines.at(i).top(), 0.5,
			  QStringLiteral("line %1 sits at the same height").arg(i));
		checkNear(headingLines.at(i).width(), listLines.at(i).width(), 0.5,
			  QStringLiteral("line %1 takes the same width").arg(i));
	}

	/* And the flip applies to both the same way. */
	Section flippedHeading = heading;
	flippedHeading.subtitleFirst = true;
	Section flippedList = list;
	flippedList.subtitleFirst = true;
	checkEq(measure(documentWith(flippedHeading)), measure(documentWith(flippedList)),
		"the flip costs both shapes the same");
}
