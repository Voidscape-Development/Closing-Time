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

#include "model/StyleLibrary.hpp"
#include "render/BackgroundPainter.hpp"

#include <QPainter>
#include <QTemporaryDir>

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* An opaque flat panel, which is the one every geometric check here is written against. */
BackgroundPanel flatPanel(const QColor &color = QColor(255, 0, 0, 255))
{
	BackgroundPanel panel;
	panel.fill = BackgroundFill::Color;
	panel.color = color;
	return panel;
}

/* `section` with `slot` carrying `panel`. */
Section withPanel(Section section, BackgroundSlot slot, const BackgroundPanel &panel)
{
	section.backgroundEntry(slot).panel = panel;
	return section;
}

/*
 * A solid opaque square, written once per run.
 *
 * The harness's own test logo is a ring -- a circle with a hole in it, so a tint that fills the
 * silhouette is visible at a glance -- which makes it the wrong thing to measure a *fit* against:
 * its ink reaches only the middle of each edge and its center is transparent. What these checks
 * need is an image whose ink is exactly its rectangle, so "cover fills the panel" is a measurement
 * rather than an approximation.
 */
QString solidImagePath()
{
	static QTemporaryDir dir;
	static const QString path = [] {
		if (!dir.isValid())
			return QString();

		const QString file = dir.filePath(QStringLiteral("solid.png"));
		QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(0, 200, 255));
		return image.save(file) ? file : QString();
	}();

	return path;
}

/* A heading with something in it, at a size worth measuring. */
Section heading(const QString &text = QStringLiteral("Closing Time"))
{
	Section section = unpadded(SectionType::Title);
	section.text = text;
	return section;
}

/* A list of `count` plain entries. */
Section textList(int count)
{
	Section section = unpadded(SectionType::TextList);
	section.entries.clear();
	for (int i = 0; i < count; ++i)
		section.entries.append(Entry{QStringLiteral("Name %1").arg(i + 1), {}, {}, {}, {}, 0});
	section.entryGap = 10;
	return section;
}

} // namespace

CT_SUITE(background_never_reflows, "A panel is painted, never laid out")
{
	/*
	 * The whole bargain of this feature. A panel is drawn behind a box that has already been
	 * decided and reaches outside it the way a drop shadow does, so switching one on -- at any
	 * size, on any slot -- must not move a section, grow the roll or change how long it runs.
	 * If this ever fails, a roll's duration has started depending on its decoration.
	 */
	const Section plain = heading();
	const int bare = measure(documentWith(plain));

	BackgroundPanel huge = flatPanel();
	huge.outsetLeft = 300.0;
	huge.outsetTop = 300.0;
	huge.outsetRight = 300.0;
	huge.outsetBottom = 300.0;
	huge.border.enabled = true;
	huge.border.width = 24.0;
	huge.setRadius(64.0);

	for (const BackgroundSlot slot : allBackgroundSlots()) {
		checkEq(measure(documentWith(withPanel(plain, slot, huge))), bare,
			qPrintable(QStringLiteral("a panel in the %1 slot changes no height")
					   .arg(QString::fromUtf8(backgroundSlotId(slot)))));
	}

	/* And the sections either side of it stay exactly where they were. */
	const Section second = heading(QStringLiteral("Second"));
	const QVector<QRectF> bareBoxes = boxesOf(documentWith({plain, second}), LayoutBox::Kind::Section);
	const QVector<QRectF> paneledBoxes = boxesOf(
		documentWith({withPanel(plain, BackgroundSlot::Section, huge), second}), LayoutBox::Kind::Section);

	checkEq(paneledBoxes.size(), bareBoxes.size(), "the same sections are placed");
	for (int i = 0; i < std::min(bareBoxes.size(), paneledBoxes.size()); ++i)
		check(bareBoxes.at(i) == paneledBoxes.at(i), "and every one of them in the same place");
}

CT_SUITE(background_section_extent, "What a section's panel covers")
{
	Section section = heading();
	section.paddingTop = 40;
	section.paddingBottom = 30;

	const Document plain = documentWith(section);
	const QRectF box = boxOf(plain, LayoutBox::Kind::Section);

	const Document paneled = documentWith(withPanel(section, BackgroundSlot::Section, flatPanel()));
	const Ink ink = inkOf(renderImage(paneled));

	/*
	 * The panel is the section's box -- its share of the canvas width, over the whole height it
	 * occupies including its padding. The padding is deliberately the room *inside* the panel
	 * rather than a second set of numbers, so a heading with 40px above it has 40px of panel
	 * above its letters.
	 */
	checkEq(ink.top, qRound(box.top()), "the panel starts at the top of the section's box");
	checkEq(ink.height(), qRound(box.height()), "and runs its whole height, padding included");
	checkEq(ink.left, qRound(box.left()), "across the box's own left edge");
	checkEq(ink.width(), qRound(box.width()), "and its whole width");

	saveArtifact(QStringLiteral("background-section"), paneled);
}

CT_SUITE(background_outset, "The outset, which reaches past the box without moving it")
{
	Section section = heading();
	section.sectionWidth = 0.5;

	/*
	 * Run above and below, because a panel is painted into the strip and the strip is exactly as
	 * tall as the layout made it -- so an outset on the very first section has nothing above it to
	 * be drawn into. That is the one bound on the setting and it is worth a section rather than a
	 * footnote: room in the roll is what the lead-in and a spacer are for.
	 */
	Section room = unpadded(SectionType::Spacer);
	room.spacerHeight = 200;

	const QVector<QRectF> boxes = boxesOf(documentWith({room, section, room}), LayoutBox::Kind::Section);
	checkEq(boxes.size(), 3, "three sections are placed");
	if (boxes.size() != 3)
		return;

	const QRectF box = boxes.at(1);

	BackgroundPanel panel = flatPanel();
	panel.outsetLeft = 20.0;
	panel.outsetTop = 10.0;
	panel.outsetRight = 30.0;
	panel.outsetBottom = 15.0;

	const Document document = documentWith({room, withPanel(section, BackgroundSlot::Section, panel), room});
	const Ink ink = inkOf(renderImage(document));

	checkEq(ink.left, qRound(box.left() - 20.0), "the panel reaches past the left edge");
	checkEq(ink.right, qRound(box.right() + 30.0) - 1, "and past the right one, by its own figure");
	checkEq(ink.top, qRound(box.top() - 10.0), "past the top");
	checkEq(ink.bottom, qRound(box.bottom() + 15.0) - 1, "and past the bottom");

	/* Negative pulls it in, which is the same setting read the other way. */
	BackgroundPanel inset = flatPanel();
	inset.outsetLeft = -25.0;
	inset.outsetRight = -25.0;

	const Ink insetInk =
		inkOf(renderImage(documentWith({room, withPanel(section, BackgroundSlot::Section, inset), room})));
	checkEq(insetInk.left, qRound(box.left() + 25.0), "a negative outset insets the panel");
}

CT_SUITE(background_bleed, "A panel that reaches outside its section is not cut at a tile seam")
{
	/*
	 * A panel paints outside its own box, and the tile loop only visits the sections whose boxes
	 * touch the tile being drawn. Without the bleed counting the outset, a band drawn across a
	 * seam would land in one tile and be missing from the next -- a hairline through the roll that
	 * appears only at certain lengths, which is the worst kind of fault to meet on air.
	 *
	 * So the section is placed to *end* just short of a seam and its panel told to reach well past
	 * it: the box belongs to the first tile alone, and only the bleed puts the section in front of
	 * the second one.
	 */
	Section above = unpadded(SectionType::Spacer);
	above.spacerHeight = StripRenderer::kTileHeight - 48;

	Section band = unpadded(SectionType::Spacer);
	band.spacerHeight = 20;

	BackgroundPanel panel = flatPanel();
	panel.outsetBottom = 200.0;

	Section below = unpadded(SectionType::Spacer);
	below.spacerHeight = 400;

	const Document document = documentWith({above, withPanel(band, BackgroundSlot::Section, panel), below});
	const Strip strip = renderStrip(document);

	checkEq(tilingProblem(strip), QString(), "the strip still tiles cleanly");
	check(strip.tiles.size() > 1, "and is long enough to have a seam in it");

	const QVector<QRectF> boxes = boxesOf(document, LayoutBox::Kind::Section);
	checkEq(boxes.size(), 3, "three sections are placed");
	if (boxes.size() != 3)
		return;

	const QRectF box = boxes.at(1);
	check(box.bottom() < StripRenderer::kTileHeight, "the band's own box stops short of the seam");
	check(box.bottom() + 200.0 > StripRenderer::kTileHeight, "while its panel reaches past it");

	/*
	 * A Spacer draws nothing of its own, so every pixel here is the panel -- which also pins down
	 * that the bleed is taken over every type rather than only the ones that set text or place
	 * artwork.
	 */
	const QImage image = flatten(strip);
	const Ink ink = inkOf(image);
	checkEq(ink.top, qRound(box.top()), "the panel starts at the band's own top edge");
	checkEq(ink.bottom, qRound(box.bottom() + 200.0) - 1, "and runs the whole of its reach");

	/* Continuous across the seam rather than stopping at it. */
	for (int y = StripRenderer::kTileHeight - 2; y < StripRenderer::kTileHeight + 2; ++y) {
		check(image.pixelColor(4, y).alpha() > 0, qPrintable(QStringLiteral("row %1 is drawn").arg(y)));
	}

	/* And a panel that draws nothing asks for no bleed at all. */
	BackgroundPanel invisible;
	invisible.outsetTop = 500.0;
	checkNear(invisible.bleed(), 0.0, 0.001, "an unfilled panel bleeds nothing");
}

CT_SUITE(background_corners, "Rounded corners, and the four of them")
{
	const QRectF rect(0, 0, 200, 100);

	BackgroundPanel square;
	check(backgroundPath(square, rect).boundingRect() == rect, "a square panel is its rectangle");

	BackgroundPanel rounded;
	rounded.setRadius(20.0);
	const QPainterPath path = backgroundPath(rounded, rect);
	check(path.boundingRect() == rect, "a rounded one occupies the same rectangle");
	check(!path.contains(QPointF(1, 1)), "but its corner is cut away");
	check(path.contains(QPointF(100, 50)), "while its middle is not");

	/*
	 * A radius larger than the panel can hold is scaled down rather than clipped, and all four
	 * together -- which is what lets one preset sit behind sections of different heights without
	 * being re-typed for each.
	 */
	BackgroundPanel oversized;
	oversized.setRadius(400.0);
	const QPainterPath scaled = backgroundPath(oversized, rect);
	check(scaled.boundingRect() == rect, "an oversized radius still fills its rectangle");
	check(scaled.contains(QPointF(100, 50)), "and leaves a shape with a middle to it");

	/* One corner rounded and three square is a real figure, not four of the same number. */
	BackgroundPanel tab;
	tab.radiusTopLeft = 40.0;
	const QPainterPath tabPath = backgroundPath(tab, rect);
	check(!tabPath.contains(QPointF(2, 2)), "the rounded corner is cut");
	check(tabPath.contains(QPointF(198, 2)), "and the square ones are not");
	check(tabPath.contains(QPointF(2, 98)), "on either side");
}

CT_SUITE(background_border, "A border stays inside the panel's own bounds")
{
	/*
	 * The outset rectangle is meant to be the outermost thing a panel touches. If the border
	 * straddled the edge instead, making one heavier would quietly make the panel wider -- and
	 * the bleed, which counts the outset alone, would stop describing what is painted.
	 */
	QImage image(120, 80, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	BackgroundPanel panel;
	panel.fill = BackgroundFill::None;
	panel.border.enabled = true;
	panel.border.width = 10.0;
	panel.border.color = QColor(0, 255, 0);

	const QRectF box(20, 20, 80, 40);

	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	paintBackgroundPanel(&painter, panel, box, nullptr);
	painter.end();

	const Ink ink = inkOf(image);
	checkEq(ink.left, qRound(box.left()), "the border's outer edge is the panel's left edge");
	checkEq(ink.right, qRound(box.right()) - 1, "and its right one");
	checkEq(ink.top, qRound(box.top()), "its top");
	checkEq(ink.bottom, qRound(box.bottom()) - 1, "and its bottom");

	/* A fill of None with a border is an outline with the footage showing through it. */
	check(image.pixelColor(60, 40).alpha() == 0, "and nothing is painted inside it");
	check(panel.isVisible(), "a border alone is a panel worth drawing");
}

CT_SUITE(background_entry_stripes, "One panel per row, and every other row")
{
	const Section list = textList(4);

	BackgroundPanel even = flatPanel(QColor(255, 0, 0));
	const Section uniform = withPanel(list, BackgroundSlot::Entry, even);

	/*
	 * A list with no alternate draws the same panel behind every row, so the run of them inks
	 * continuously except for the gaps the entry gap leaves.
	 */
	const QVector<QRectF> rows = boxesOf(documentWith(list), LayoutBox::Kind::Text);
	checkEq(rows.size(), 4, "the list places four rows");
	if (rows.size() != 4)
		return;

	const QImage uniformImage = renderImage(documentWith(uniform));
	for (int i = 0; i < 4; ++i)
		check(!inkOf(uniformImage, rows.at(i)).isEmpty(), "every row carries the panel");

	/*
	 * With an alternate set, the odd rows take it instead. Left on None, that is how every other
	 * row is left bare -- which is why the model tells an absent slot apart from an empty one.
	 */
	Section striped = uniform;
	striped.backgroundEntry(BackgroundSlot::EntryAlt).panel = BackgroundPanel();

	const QImage stripedImage = renderImage(documentWith(striped));

	/*
	 * Measured over the panel's own rows rather than the text's, since a bare row still inks its
	 * words. The panel spans the section's full width, so the columns clear of the centered text
	 * are what say whether a panel is there.
	 */
	const auto rowHasPanel = [&stripedImage](const QRectF &row) {
		const QRect strip(0, qRound(row.top()), 8, std::max(1, qRound(row.height())));
		return !inkOf(stripedImage, strip).isEmpty();
	};

	check(rowHasPanel(rows.at(0)), "the first row keeps the panel");
	check(!rowHasPanel(rows.at(1)), "the second is left bare");
	check(rowHasPanel(rows.at(2)), "the third takes it again");
	check(!rowHasPanel(rows.at(3)), "and the fourth is bare");

	saveArtifact(QStringLiteral("background-striped-list"), documentWith(striped));
}

CT_SUITE(background_slots_for_type, "Which panels a type has anything to sit behind")
{
	/* Every type has a section to sit behind, including the two that draw no content. */
	for (const SectionType type : allSectionTypes()) {
		check(backgroundSlotsFor(type).contains(BackgroundSlot::Section),
		      qPrintable(
			      QStringLiteral("%1 offers a section panel").arg(QString::fromUtf8(sectionTypeId(type)))));
	}

	/* A block holds whole sections, each carrying every slot in its own right. */
	checkEq(backgroundSlotsFor(SectionType::StickyBlock).size(), 1, "a sticky block offers that one alone");

	/*
	 * A plain list's line and its entry are one rectangle, so it is paneled by its entry slot
	 * and is not offered a second name for the same place.
	 */
	check(!backgroundSlotsFor(SectionType::TextList).contains(BackgroundSlot::Title),
	      "a text list has no separate title panel");
	check(backgroundSlotsFor(SectionType::TextList).contains(BackgroundSlot::Entry),
	      "it has an entry panel instead");

	/* A pair-shaped list has both: the entry is the pair, the two text slots are its lines. */
	const QVector<BackgroundSlot> pairs = backgroundSlotsFor(SectionType::TitleSubtitleList);
	check(pairs.contains(BackgroundSlot::Entry), "a pair list panels its entry");
	check(pairs.contains(BackgroundSlot::Title), "and the title line inside it");
	check(pairs.contains(BackgroundSlot::Subtitle), "and the subtitle line");

	check(backgroundSlotsFor(SectionType::SectionDivider).contains(BackgroundSlot::Divider),
	      "a divider panels its artwork");
	check(backgroundSlotsFor(SectionType::Bridged).contains(BackgroundSlot::Bridge),
	      "a bridged row panels its leader");
	check(backgroundSlotsFor(SectionType::LogoList).contains(BackgroundSlot::Logo), "a logo list panels each logo");
	check(!backgroundSlotsFor(SectionType::Title).contains(BackgroundSlot::Entry),
	      "a heading has no entries to panel");
}

CT_SUITE(background_preset_binding, "A slot follows a preset, and falls back when it cannot")
{
	Document document = documentWith(withPanel(heading(), BackgroundSlot::Section, flatPanel(QColor(1, 2, 3))));
	Section &section = document.sections.first();

	/* Unbound, the slot is drawn from its own copy. */
	check(document.effectiveBackground(section, BackgroundSlot::Section).color == QColor(1, 2, 3),
	      "an unbound slot uses its own panel");

	document.setBackgroundPreset(QStringLiteral("Card"), flatPanel(QColor(9, 8, 7)));
	section.backgroundEntry(BackgroundSlot::Section).presetName = QStringLiteral("Card");

	check(document.effectiveBackground(section, BackgroundSlot::Section).color == QColor(9, 8, 7),
	      "a bound slot is drawn from the preset");

	/*
	 * Binding is non-destructive in both directions, exactly as a style binding is: the slot's
	 * own copy is untouched underneath and is what a name that no longer resolves falls back to.
	 */
	check(section.background(BackgroundSlot::Section).color == QColor(1, 2, 3),
	      "the slot's own panel is left alone underneath it");

	document.removeBackgroundPreset(QStringLiteral("Card"));
	check(document.effectiveBackground(section, BackgroundSlot::Section).color == QColor(1, 2, 3),
	      "and is what it falls back to when the preset goes");
	checkEq(section.backgroundPresetName(BackgroundSlot::Section), QString(),
		"a deleted preset unbinds rather than dangling");

	/* A name nothing carries resolves to the slot's own panel rather than failing. */
	section.backgroundEntry(BackgroundSlot::Section).presetName = QStringLiteral("Nothing");
	check(document.effectiveBackground(section, BackgroundSlot::Section).color == QColor(1, 2, 3),
	      "an unresolvable name degrades rather than breaks");
}

CT_SUITE(background_slot_presence, "A slot that is there, and a slot that draws nothing")
{
	Section section = heading();

	check(!section.hasBackground(BackgroundSlot::Entry), "a section starts with no slots at all");
	check(!section.background(BackgroundSlot::Entry).isVisible(), "and an absent slot draws nothing");
	checkEq(section.backgrounds.size(), 0, "carrying nothing to save");

	/*
	 * Asking for an entry creates it. That is the distinction the alternate row panel is built
	 * on: a slot present but filled with nothing is not the same as a slot that was never given.
	 */
	section.backgroundEntry(BackgroundSlot::EntryAlt);
	check(section.hasBackground(BackgroundSlot::EntryAlt), "asking for a slot creates it");
	check(!section.background(BackgroundSlot::EntryAlt).isVisible(), "still drawing nothing");
	checkEq(section.backgrounds.size(), 1, "and now worth saving");

	/* Asking again returns the same entry rather than adding a second. */
	section.backgroundEntry(BackgroundSlot::EntryAlt).panel.fill = BackgroundFill::Color;
	section.backgroundEntry(BackgroundSlot::EntryAlt);
	checkEq(section.backgrounds.size(), 1, "one entry per slot, however often it is asked for");
	check(section.background(BackgroundSlot::EntryAlt).fill == BackgroundFill::Color, "keeping what was set");

	section.clearBackground(BackgroundSlot::EntryAlt);
	check(!section.hasBackground(BackgroundSlot::EntryAlt), "and clearing takes the entry away");
	checkEq(section.backgrounds.size(), 0, "leaving nothing behind");
}

CT_SUITE(background_opacity, "Opacity over the whole panel")
{
	Section section = heading();

	BackgroundPanel half = flatPanel(QColor(255, 255, 255, 255));
	half.opacity = 0.5;

	const QImage image = renderImage(documentWith(withPanel(section, BackgroundSlot::Section, half)));
	const QRectF box = boxOf(documentWith(section), LayoutBox::Kind::Section);

	/* Sampled at the box's left edge, which the panel covers and the centered heading does not. */
	const QColor sampled = image.pixelColor(qRound(box.left()) + 2, qRound(box.center().y()));
	check(sampled.alpha() > 100 && sampled.alpha() < 155, "a panel at half opacity is drawn at about half alpha");

	BackgroundPanel none = flatPanel();
	none.opacity = 0.0;
	check(!none.isVisible(), "and at nothing it is not drawn at all");
}

CT_SUITE(background_logo_panel, "A card behind the artwork, and a band behind the row")
{
	Section section = unpadded(SectionType::LogoList);
	section.entries.clear();
	section.entries.append(Entry{});
	withLogo(section, 80);

	const Document plain = documentWith(section);
	const QRectF logoBox = boxOf(plain, LayoutBox::Kind::Logo);
	check(!logoBox.isNull(), "the list places a logo");
	if (logoBox.isNull())
		return;

	/*
	 * The two slots mean two different things, which is the whole reason a logo list carries
	 * both: the logo's panel hugs the artwork, the entry's runs the width of the list.
	 */
	const Ink logoInk = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Logo, flatPanel()))));
	checkNear(logoInk.width(), logoBox.width(), 2.0, "a logo panel is the width of the artwork");

	const Ink entryInk = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Entry, flatPanel()))));
	check(entryInk.width() > logoBox.width() * 2, "an entry panel runs the width of the list");
}

CT_SUITE(background_image_fit, "How an image is fitted to a panel it does not match")
{
	Section section = heading();
	section.paddingTop = 0;
	section.paddingBottom = 0;

	BackgroundPanel panel;
	panel.fill = BackgroundFill::Image;
	panel.imagePath = solidImagePath();

	const QRectF box = boxOf(documentWith(section), LayoutBox::Kind::Section);

	/*
	 * The test logo is square and its ink reaches its own edges; a section's box is far wider
	 * than it is tall. Cover therefore has to fill the box edge to edge, and Contain cannot.
	 */
	panel.imageFit = BackgroundImageFit::Cover;
	const Ink cover = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Section, panel))));
	checkNear(cover.width(), box.width(), 2.0, "cover fills the panel's width");
	checkNear(cover.height(), box.height(), 2.0, "and its height");

	panel.imageFit = BackgroundImageFit::Contain;
	const Ink contain = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Section, panel))));
	check(contain.width() < box.width() - 2, "contain leaves the panel showing to the sides");
	checkNear(contain.height(), box.height(), 2.0, "while fitting its height exactly");

	panel.imageFit = BackgroundImageFit::Stretch;
	const Ink stretch = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Section, panel))));
	checkNear(stretch.width(), box.width(), 2.0, "stretch fills it too");

	panel.imageFit = BackgroundImageFit::Tile;
	const Ink tile = inkOf(renderImage(documentWith(withPanel(section, BackgroundSlot::Section, panel))));
	checkNear(tile.width(), box.width(), 2.0, "and so does a tiled texture");

	/*
	 * A file that is not there draws nothing rather than failing the strip, which is the bargain
	 * a missing logo already strikes.
	 */
	panel.imagePath = missingLogoPath();
	const Document broken = documentWith(withPanel(section, BackgroundSlot::Section, panel));
	checkEq(measure(broken), measure(documentWith(section)), "a missing image still changes no height");
	check(!renderStrip(broken).isEmpty(), "and the strip still renders");
}

CT_SUITE(background_image_clipped_to_corners, "An image fill is cropped to the panel's shape")
{
	/*
	 * Clipped to the path rather than to the rectangle around it, which is the whole of what
	 * makes a rounded corner mean anything to an image fill.
	 */
	QImage image(120, 120, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	BackgroundPanel panel;
	panel.fill = BackgroundFill::Image;
	panel.imagePath = solidImagePath();
	panel.imageFit = BackgroundImageFit::Stretch;
	panel.setRadius(40.0);

	LogoCache cache;
	const QRectF box(10, 10, 100, 100);

	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing);
	paintBackgroundPanel(&painter, panel, box, &cache);
	painter.end();

	check(image.pixelColor(60, 60).alpha() > 0, "the middle of the panel is painted");
	check(image.pixelColor(12, 12).alpha() == 0, "and its rounded corner is not");
}

CT_SUITE(background_gradient, "A panel's sweep is the renderer's own")
{
	BackgroundPanel panel;
	panel.fill = BackgroundFill::LinearGradient;
	panel.gradient.angle = 0.0;
	panel.gradient.stops = {GradientStop{0.0, QColor(255, 0, 0)}, GradientStop{1.0, QColor(0, 0, 255)}};

	QImage image(60, 200, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	QPainter painter(&image);
	paintBackgroundPanel(&painter, panel, QRectF(0, 0, 60, 200), nullptr);
	painter.end();

	/* Angle 0 runs top to bottom, which is what the text fill means by it. */
	check(image.pixelColor(30, 5).red() > image.pixelColor(30, 195).red(), "the first stop is at the top");
	check(image.pixelColor(30, 195).blue() > image.pixelColor(30, 5).blue(), "and the last at the bottom");

	/* A gradient is always worth drawing, however its stops are set. */
	check(panel.isVisible(), "a gradient panel draws");
}

CT_SUITE(background_library_round_trip, "Panels in the machine-wide library")
{
	StyleLibrary &library = StyleLibrary::instance();
	const QString path = library.filePath();

	QTemporaryDir dir;
	check(dir.isValid(), "a temporary directory for the library");
	library.setFilePath(dir.filePath(QStringLiteral("style-presets.json")));
	library.load();

	BackgroundPanel card = flatPanel(QColor(20, 30, 40, 200));
	card.setRadius(12.0);
	card.border.enabled = true;
	card.outsetLeft = 8.0;
	card.outsetTop = 8.0;
	card.outsetRight = 8.0;
	card.outsetBottom = 8.0;

	library.setBackground(QStringLiteral("Card"), card);

	BackgroundPanel loaded;
	check(library.findBackground(QStringLiteral("Card"), &loaded), "the panel came back");
	check(loaded == card, "field for field, corners and border included");

	/*
	 * The two collections are two namespaces. A style called Card and a panel called Card are
	 * two things, and neither answers for the other.
	 */
	check(!library.contains(QStringLiteral("Card")), "a panel is not a style of the same name");
	library.set(QStringLiteral("Card"), TextStyle{});
	check(library.containsBackground(QStringLiteral("Card")), "and adding the style leaves the panel alone");

	/* A rename is followed by the documents bound to it, out of a trail of its own. */
	library.renameBackground(QStringLiteral("Card"), QStringLiteral("Panel"));
	QString renamed;
	check(library.backgroundRenamedTo(QStringLiteral("Card"), &renamed), "the rename is recorded");
	checkEq(renamed, QStringLiteral("Panel"), "pointing at the new name");
	check(!library.renamedTo(QStringLiteral("Card"), nullptr), "and the styles' own trail is untouched by it");

	Document document = documentWith(heading());
	document.sections.first().backgroundEntry(BackgroundSlot::Section).presetName = QStringLiteral("Card");
	document.backgroundPresets.append(BackgroundPreset{QStringLiteral("Card"), card, true});

	check(document.applyLibraryRenames(), "the document follows the rename");
	checkEq(document.backgroundPresets.first().name, QStringLiteral("Panel"), "renaming its own preset");
	checkEq(document.sections.first().backgroundPresetName(BackgroundSlot::Section), QStringLiteral("Panel"),
		"and the binding that named it");

	/* The library's own JSON carries both collections and parses back. */
	QVector<StylePreset> styles;
	QVector<BackgroundPreset> panels;
	QString error;
	check(StyleLibrary::parseJson(library.toJson(), &styles, &panels, &error), "the library's JSON parses");
	checkEq(panels.size(), 1, "with the panel in it");
	check(panels.isEmpty() || panels.first().panel == card, "unchanged by the trip");

	library.setFilePath(path);
	library.load();
}

CT_SUITE(background_linked_refresh, "A linked panel follows the library")
{
	StyleLibrary &library = StyleLibrary::instance();
	const QString path = library.filePath();

	QTemporaryDir dir;
	check(dir.isValid(), "a temporary directory for the library");
	library.setFilePath(dir.filePath(QStringLiteral("style-presets.json")));
	library.load();

	library.setBackground(QStringLiteral("House"), flatPanel(QColor(10, 20, 30)));

	Document document = documentWith(heading());
	check(document.linkBackgroundPreset(QStringLiteral("House")), "the document links it");
	check(document.backgroundPresets.first().panel.color == QColor(10, 20, 30), "taking the library's copy");

	/* A library edit reaches the roll through a refresh, not through a lookup at paint time. */
	library.setBackground(QStringLiteral("House"), flatPanel(QColor(40, 50, 60)));
	check(document.refreshLinkedBackgroundPresets(), "the refresh reports the move");
	check(document.backgroundPresets.first().panel.color == QColor(40, 50, 60), "and brings the new panel in");

	/* An unchanged library must not read as a change, or every poll would queue a rebuild. */
	check(!document.refreshLinkedBackgroundPresets(), "a library that has not moved reports nothing");

	/*
	 * The one call brings both collections up to date, so a caller cannot follow its styles and
	 * leave its panels behind.
	 */
	library.setBackground(QStringLiteral("House"), flatPanel(QColor(70, 80, 90)));
	check(document.refreshLinkedPresets(), "refreshing the presets covers the panels too");
	check(document.backgroundPresets.first().panel.color == QColor(70, 80, 90), "with the library's latest");

	library.setFilePath(path);
	library.load();
}
