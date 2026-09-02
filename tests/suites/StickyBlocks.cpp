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

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* A block holding one heading, with no padding of its own to account for. */
Section stickyBlock(const QString &text = QStringLiteral("The End"))
{
	Section block = unpadded(SectionType::StickyBlock);
	block.children.clear();

	Section title = unpadded(SectionType::Title);
	title.text = text;
	block.children.push_back(title);

	return block;
}

/* The same heading on its own, for measuring the block against what it holds. */
Section loneTitle(const QString &text = QStringLiteral("The End"))
{
	Section title = unpadded(SectionType::Title);
	title.text = text;
	return title;
}

} // namespace

CT_SUITE(sticky_slot, "A sticky block taking its place in the roll")
{
	/*
	 * The block is laid into the roll like any other section: it takes exactly the room its
	 * content would have taken inline, so nothing above or below it moves for the feature.
	 */
	const int blockHeight = measure(documentWith(stickyBlock()));
	const int titleHeight = measure(documentWith(loneTitle()));
	checkEq(blockHeight, titleHeight, "the block is as tall as what it holds");

	Section padded = stickyBlock();
	padded.paddingTop = 30;
	padded.paddingBottom = 20;
	checkEq(measure(documentWith(padded)) - blockHeight, 50, "and its own padding is worth its pixels");

	/* A hidden child takes nothing with it, exactly as a hidden section does at the top level. */
	Section hiddenChild = stickyBlock();
	hiddenChild.children.front().visible = false;
	checkEq(measure(documentWith(hiddenChild)), 0, "a block whose every child is hidden takes no height");

	Section hidden = stickyBlock();
	hidden.visible = false;
	checkEq(measure(documentWith(hidden)), 0, "and a hidden block takes none either");
}

CT_SUITE(sticky_hole, "The strip leaving the block's slot empty")
{
	const Document document = documentWith(stickyBlock());
	const Strip strip = renderStrip(document);

	checkEq(strip.stickyBlocks.size(), 1, "the strip carries the block");
	if (strip.stickyBlocks.isEmpty())
		return;

	const StickyBlockPlacement &placement = strip.stickyBlocks.first();
	const QRectF slot = placement.rect;

	/*
	 * Nothing is painted into the slot. A block that stops scrolling while the roll carries on
	 * cannot be part of the picture that scrolls, so the strip has to come out empty where it
	 * would otherwise have drawn it -- otherwise the block would be on screen twice, once
	 * pinned and once traveling.
	 */
	check(inkOf(flatten(strip), slot).isEmpty(), "the strip draws nothing in the slot");

	/* And the block itself is carried as a picture, with its own content really in it. */
	check(!placement.image.isNull(), "the block came back as a picture");
	checkEq(placement.image.width(), document.width, "as wide as the canvas");
	checkEq(placement.image.height(), qRound(slot.height()) + placement.margin * 2,
		"and as tall as its slot plus the margin at each end");
	check(!inkOf(placement.image).isEmpty(), "with the block's own content drawn in it");

	/* The slot is reported to the overlay, so the designer can say where the block sits. */
	const QRectF box = boxOf(document, LayoutBox::Kind::Sticky);
	check(!box.isNull(), "the slot is reported as a layout box");
	checkNear(box.height(), slot.height(), 1.0, "over the height the block occupies");
}

CT_SUITE(sticky_anchor, "Where a pinned block lands on the canvas")
{
	Document document = documentWith(stickyBlock());
	document.height = 1080;

	const Strip strip = renderStrip(document);
	if (strip.stickyBlocks.isEmpty()) {
		fail("the strip carries no block");
		return;
	}

	StickyBlockPlacement placement = strip.stickyBlocks.first();
	const double blockHeight = placement.rect.height();

	/*
	 * The pin is a pair of points, so each anchor puts a different part of the block on the same
	 * place down the canvas. Halfway down, that is: the top edge lands at the halfway line, the
	 * middle straddles it, and the bottom edge ends on it.
	 */
	placement.canvasPosition = 0.5;
	placement.offset = 0.0;

	placement.anchor = StickyAnchor::Top;
	checkNear(placement.pinnedTop(document.height), 540.0, 0.001, "a top anchor starts at the line");

	placement.anchor = StickyAnchor::Center;
	checkNear(placement.pinnedTop(document.height), 540.0 - blockHeight / 2.0, 0.001,
		  "a center anchor straddles it");

	placement.anchor = StickyAnchor::Bottom;
	checkNear(placement.pinnedTop(document.height), 540.0 - blockHeight, 0.001, "a bottom anchor ends on it");

	/* The nudge is pixels on top of whatever the share worked out to. */
	placement.anchor = StickyAnchor::Top;
	placement.offset = -40.0;
	checkNear(placement.pinnedTop(document.height), 500.0, 0.001, "the nudge moves it by its own pixels");

	/* And the share is of the canvas, so a taller canvas pins proportionally further down. */
	placement.offset = 0.0;
	checkNear(placement.pinnedTop(2160), 1080.0, 0.001, "the share follows the canvas height");
}

CT_SUITE(sticky_backdrop, "The panel drawn behind a pinned block")
{
	Section plain = stickyBlock();

	Section paneled = plain;
	BackgroundPanel &panel = paneled.backgroundEntry(BackgroundSlot::Section).panel;
	panel.fill = BackgroundFill::Color;
	panel.color = QColor(0, 0, 0, 255);
	panel.outsetLeft = 40.0;
	panel.outsetTop = 40.0;
	panel.outsetRight = 40.0;
	panel.outsetBottom = 40.0;

	/* The panel is part of the picture rather than a quad behind it, and does not move the roll. */
	checkEq(measure(documentWith(paneled)), measure(documentWith(plain)),
		"a backdrop changes nothing about the layout");

	const Strip strip = renderStrip(documentWith(paneled));
	if (strip.stickyBlocks.isEmpty()) {
		fail("the strip carries no block");
		return;
	}

	const StickyBlockPlacement &placement = strip.stickyBlocks.first();
	check(placement.margin >= 40, "the picture is grown to hold the panel's outset");

	const Ink ink = inkOf(placement.image);
	checkEq(ink.left, 0, "the panel reaches the left edge of the canvas");
	checkEq(ink.right, placement.image.width() - 1, "and the right one");
	checkEq(ink.height(), placement.image.height(), "and fills the picture from top to bottom");

	/* Without it, the picture inks only where the block's own content is. */
	const Strip bare = renderStrip(documentWith(plain));
	if (!bare.stickyBlocks.isEmpty())
		check(inkOf(bare.stickyBlocks.first().image).width() < placement.image.width(),
		      "and without a panel only the content is drawn");
}

CT_SUITE(sticky_children, "What a block may hold")
{
	/*
	 * A block holds whole sections, and they are laid out by the same call that lays out the
	 * roll -- which is what makes "anything except another block" a true statement rather than a
	 * list of the types somebody remembered to support.
	 */
	Section block = unpadded(SectionType::StickyBlock);
	block.children.clear();

	int expected = 0;
	for (SectionType type : allSectionTypes()) {
		if (type == SectionType::StickyBlock)
			continue;

		Section child = Section::makeDefault(type);
		if (sectionUsesLogos(type))
			withLogo(child);

		expected += measure(documentWith(child));
		block.children.push_back(child);
	}

	checkEq(measure(documentWith(block)), expected, "a block is as tall as the sections it holds");
	checkEq(boxesOutsideContent(documentWith(block)), 0, "and nothing in it is placed outside its own box");

	/*
	 * A block inside a block is dropped by the loader rather than loaded, because everything
	 * downstream of here is written for one level of pinning.
	 */
	Section nested = unpadded(SectionType::StickyBlock);
	nested.children.clear();
	nested.children.push_back(unpadded(SectionType::StickyBlock));
	nested.children.push_back(loneTitle());

	OBSDataAutoRelease data = obs_data_create();
	nested.save(data);

	Section loaded;
	loaded.load(data);

	checkEq(static_cast<int>(loaded.children.size()), 1, "a nested block is dropped on the way in");
	if (!loaded.children.empty())
		check(loaded.children.front().type == SectionType::Title, "and everything else survives");
}

CT_SUITE(sticky_animated_logo, "Animated artwork inside a block")
{
	/*
	 * A block is drawn as one quad, so there is nowhere inside it to leave a second hole for an
	 * animation to be drawn into. Its children are therefore laid out without the animation
	 * cache, which is the renderer's own way of asking for every logo as a still -- and the
	 * check that matters is that something is drawn at all, since a logo left as a hole with
	 * nothing over it is a block with a gap in it.
	 */
	Section block = unpadded(SectionType::StickyBlock);
	block.children.clear();

	Section logos = unpadded(SectionType::LogoList);
	withAnimatedLogo(logos);
	block.children.push_back(logos);

	const Strip strip = renderAnimatedStrip(documentWith(block));
	checkEq(strip.stickyBlocks.size(), 1, "the block is carried");
	check(strip.animatedLogos.isEmpty(), "and its artwork is not reported as a hole in the strip");

	if (!strip.stickyBlocks.isEmpty())
		check(!inkOf(strip.stickyBlocks.first().image).isEmpty(), "the artwork is drawn into the block");
}

CT_SUITE(sticky_roll, "A block among the sections of a whole roll")
{
	/*
	 * The point of the slot: what is above and below the block sits exactly where it would have
	 * without it, so turning a run of sections into a block does not reflow the roll around it.
	 */
	Section before = loneTitle(QStringLiteral("Cast"));
	Section after = loneTitle(QStringLiteral("Crew"));

	const Document inline_ = documentWith({before, loneTitle(), after});
	const Document blocked = documentWith({before, stickyBlock(), after});

	checkEq(measure(blocked), measure(inline_), "the roll is the same height either way");

	const QVector<QRectF> inlineBoxes = boxesOf(inline_, LayoutBox::Kind::Section);
	const QVector<QRectF> blockedBoxes = boxesOf(blocked, LayoutBox::Kind::Section);
	if (inlineBoxes.size() == 3 && blockedBoxes.size() == 4) {
		/* The block reports its own box and its child's, which is why there is one more. */
		checkNear(blockedBoxes.at(0).top(), inlineBoxes.at(0).top(), 0.5, "the section above has not moved");
		checkNear(blockedBoxes.at(3).top(), inlineBoxes.at(2).top(), 0.5, "nor the one below");
	} else {
		fail(QStringLiteral("unexpected box counts: %1 inline, %2 blocked")
			     .arg(inlineBoxes.size())
			     .arg(blockedBoxes.size()));
	}

	/* Two blocks in one roll is an ordinary document, not a special case. */
	const Document two =
		documentWith({stickyBlock(QStringLiteral("Part One")), after, stickyBlock(QStringLiteral("Part Two"))});
	checkEq(renderStrip(two).stickyBlocks.size(), 2, "each block is carried in its own right");
}
