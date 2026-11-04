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

#include "model/DividerDesigns.hpp"
#include "render/RollCompositor.hpp"

#include <obs.hpp>

using namespace closingtime;
using namespace closingtime::test;

namespace {

DividerElement shape(DividerShape which, double scale = 1.0)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Shape;
	element.shape = which;
	element.scale = scale;
	return element;
}

DividerElement line(DividerShape which = DividerShape::Rule, double weight = 1.0)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Line;
	element.lineShape = which;
	element.weight = weight;
	return element;
}

DividerElement word(const QString &text)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Text;
	element.text = text;
	return element;
}

DividerLane lane(QVector<DividerElement> elements, double thickness = 6.0, double inset = 0.0)
{
	DividerLane built;
	built.elements = std::move(elements);
	built.thickness = thickness;
	built.inset = inset;
	return built;
}

/* A divider with nothing but its own artwork in the document, no padding, and the full width. */
Section divider(QVector<DividerLane> lanes, bool symmetric = true)
{
	Section section = unpadded(SectionType::SectionDivider);
	section.sectionWidth = 1.0;
	section.marginX = 0;
	section.divider.lanes = std::move(lanes);
	section.divider.symmetric = symmetric;
	section.divider.length = 1.0;
	section.divider.lineGap = 12.0;
	section.divider.pieceGap = 10.0;
	section.divider.laneGap = 6.0;
	return section;
}

int asymmetricColumns(const QImage &image)
{
	int asymmetric = 0;
	for (int x = 0; x < image.width() / 2; ++x) {
		if (inksColumn(image, x) != inksColumn(image, image.width() - 1 - x))
			++asymmetric;
	}
	return asymmetric;
}

/* Ink in the horizontal band `top`..`bottom` of the image, across the whole width. */
Ink bandInk(const QImage &image, qreal top, qreal bottom)
{
	return inkOf(image, QRectF(0, top, image.width(), bottom - top));
}

} // namespace

CT_SUITE(divider_shape_parity, "Every piece shape offered wherever a piece can go")
{
	const QVector<DividerShape> &pieces = dividerShapesForRole(DividerRolePiece);

	check(pieces.contains(DividerShape::Arrow), "an arrowhead is a piece");
	check(pieces.contains(DividerShape::Heart), "and so is a heart");
	check(pieces.contains(DividerShape::Custom), "and a file of the user's own");

	const QVector<DividerShape> &lines = dividerShapesForRole(DividerRoleArm);
	check(lines.contains(DividerShape::Rule), "a rule is a line");
	check(!lines.contains(DividerShape::Heart), "a heart is not");
	check(!pieces.contains(DividerShape::Rule), "and a rule is not a piece");

	for (DividerShape which : pieces)
		check(dividerShapeHasRole(which, DividerRolePiece), "the list agrees with the table");
}

CT_SUITE(divider_symmetric, "A symmetric divider is its half reflected")
{
	/*
	 * A deliberately lopsided half: a big arrow outside a small dot. Reflected, the whole figure
	 * has to come out symmetric about the section's own center -- which catches both halves of the
	 * reflection at once, since the order has to reverse *and* each piece's artwork has to flip.
	 */
	Section section = divider({lane({shape(DividerShape::Arrow, 1.5), shape(DividerShape::Dot), line()})});
	const QImage image = renderImage(documentWith(section));
	check(!inkOf(image).isEmpty(), "the divider draws");
	checkEq(asymmetricColumns(image), 0, "a symmetric divider inks the same columns at both ends");

	const QVector<DividerElement> row = section.divider.drawnRow(section.divider.lanes.first());
	checkEq(row.size(), 6, "the half and its reflection are both drawn");
	if (row.size() == 6)
		check(!row.at(0).flip && row.at(5).flip, "the far copy is flipped");

	/* Not symmetric, the same list is the whole row: an arrow at one end only. */
	Section lopsided = section;
	lopsided.divider.symmetric = false;
	const QImage oneEnd = renderImage(documentWith(lopsided));
	check(asymmetricColumns(oneEnd) > 0, "the same list, not reflected, is lopsided");
	checkEq(lopsided.divider.drawnRow(lopsided.divider.lanes.first()).size(), 3, "and is drawn as written");
}

CT_SUITE(divider_center_piece, "The last piece of a symmetric half on the axis, or a pair meeting there")
{
	/*
	 * A diamond written last sits on the center line once. Told not to, it is drawn either side of
	 * the line -- the ordinary way to build a pair -- so the ink at the middle is twice as wide.
	 */
	Section once = divider({lane({line(), shape(DividerShape::Diamond)})});
	Section pair = once;
	pair.divider.lanes.first().centerPiece = false;

	const auto middleInk = [](const Section &s) {
		const Document document = documentWith(s);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		/* Above the rule only the diamonds reach. */
		return inkOf(renderImage(document),
			     QRectF(box.left(), box.top(), box.width(), box.center().y() - 6.0 - box.top()));
	};

	const Ink single = middleInk(once);
	const Ink double_ = middleInk(pair);
	check(!single.isEmpty() && !double_.isEmpty(), "both draw their diamonds");
	check(double_.width() > single.width() * 1.8, "a pair is two diamonds rather than one");

	const QRectF box = boxOf(documentWith(once), LayoutBox::Kind::Divider);
	checkNear((single.left + single.right) / 2.0, box.center().x(), 2.0, "the single one is on the axis");
	checkEq(once.divider.drawnRow(once.divider.lanes.first()).size(), 3, "drawn once");
	checkEq(pair.divider.drawnRow(pair.divider.lanes.first()).size(), 4, "or twice");

	/* A line written last has nothing to sit on the axis: the two halves' lines simply meet. */
	Section lines = divider({lane({shape(DividerShape::Arrow), line()})});
	const Ink rule = inkOf(renderImage(documentWith(lines)));
	checkNear(rule.width(), box.width(), 2.0, "two halves of a rule run edge to edge");
}

CT_SUITE(divider_words, "A word in a divider is set, counted and never mirrored")
{
	Section section = divider({lane({word(QStringLiteral("MMXXVI")), line()})});
	section.style.family = QStringLiteral("Sans Serif");

	const Document document = documentWith(section);
	check(document.usedFontFamilies().contains(QStringLiteral("Sans Serif")),
	      "a word anywhere in a divider counts towards the roll's fonts");

	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	const Ink leftInk = inkOf(image, QRectF(box.left(), box.top(), box.width() / 4.0, box.height()));
	const Ink rightInk =
		inkOf(image, QRectF(box.right() - box.width() / 4.0, box.top(), box.width() / 4.0, box.height()));
	check(!leftInk.isEmpty() && !rightInk.isEmpty(), "the word is drawn at both ends of a symmetric divider");
	check(asymmetricColumns(image) > 0, "and the right way round at both, rather than reflected");

	checkEq(section.displayLabel(), QStringLiteral("MMXXVI"), "a divider holding a word is named after it");

	/* A divider of artwork alone sets no font at all. */
	Section art = divider({lane({line()})});
	art.style.family = QStringLiteral("Unused Face");
	check(documentWith(art).usedFontFamilies().isEmpty(), "artwork alone needs no font");
}

CT_SUITE(divider_plain_line, "A lane with nothing but a line, and pieces that measure to nothing")
{
	Section bare = divider({lane({line()})});
	const Document document = documentWith(bare);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	const Ink ink = inkOf(renderImage(document));

	check(!ink.isEmpty(), "a lone line draws");
	checkNear(ink.left, box.left(), 2.0, "reaching the left edge");
	checkNear(ink.right, box.right(), 2.0, "and the right one");
	checkNear(box.height(), 6.0, 0.5, "exactly as tall as the lane's thickness");

	/* A piece that measures to nothing is dropped along with its gap, wherever it sits. */
	DividerElement nothing = shape(DividerShape::None);
	Section empties = divider({lane({word(QString()), nothing, line()})});
	checkNear(inkOf(renderImage(documentWith(empties))).left, ink.left, 2.0,
		  "an empty word and an empty shape leave no hole");

	/* An empty line is space: it holds its room and draws nothing. */
	Section spaced = divider({lane({line(DividerShape::None), shape(DividerShape::Diamond)})});
	const Ink diamond = inkOf(renderImage(documentWith(spaced)));
	check(!diamond.isEmpty(), "the diamond is drawn");
	checkNear((diamond.left + diamond.right) / 2.0, box.center().x(), 2.0, "pushed to the middle by the space");
	check(diamond.width() < box.width() / 4.0, "and the space itself draws nothing");

	/* A lane with nothing in it draws nothing, and a divider of empty lanes takes no height. */
	Section empty = divider({lane({})});
	checkEq(measure(documentWith(empty)), 0, "an empty divider takes no height");
	checkEq(boxesOutsideContent(documentWith(empty)), 0, "and places nothing");
}

CT_SUITE(divider_line_weights, "Where a piece sits is the weights of the lines either side of it")
{
	const auto squareInk = [](const Section &s) {
		const Document document = documentWith(s);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		return inkOf(renderImage(document),
			     QRectF(box.left(), box.top(), box.width(), box.center().y() - 4.0 - box.top()));
	};

	Section even = divider({lane({line(DividerShape::Rule, 1.0), shape(DividerShape::Square, 2.0),
				      line(DividerShape::Rule, 1.0)})},
			       false);
	Section quarter = even;
	quarter.divider.lanes[0].elements[0].weight = 1.0;
	quarter.divider.lanes[0].elements[2].weight = 3.0;
	Section leading = divider({lane({shape(DividerShape::Square, 2.0), line()})}, false);
	Section closing = divider({lane({line(), shape(DividerShape::Square, 2.0)})}, false);

	const QRectF box = boxOf(documentWith(even), LayoutBox::Kind::Divider);
	const Ink middle = squareInk(even);
	const Ink first = squareInk(quarter);
	const Ink start = squareInk(leading);
	const Ink end = squareInk(closing);

	checkNear((middle.left + middle.right) / 2.0, box.center().x(), 2.0, "equal weights center it");
	check(first.left > start.left && first.right < middle.right, "one to three puts it a quarter of the way");
	checkNear(start.left, box.left(), 2.0, "a piece written first leads the rule");
	checkNear(end.right, box.right(), 2.0, "and one written last closes it");

	/* Height never moves with where the piece sits. */
	checkEq(measure(documentWith(quarter)), measure(documentWith(even)), "the height does not move");
}

CT_SUITE(divider_narrow, "A lane too narrow for its lines drops its optional pieces first")
{
	DividerElement flourish = shape(DividerShape::Filigree, 2.0);
	flourish.optional = true;

	DividerElement rule = line();
	rule.minLength = 300.0;

	Section section = divider({lane({rule, flourish, shape(DividerShape::Diamond)})});
	section.sectionWidth = 0.4;

	Section kept = section;
	kept.divider.lanes[0].elements[1].optional = false;

	const Ink dropped = inkOf(renderImage(documentWith(section)));
	const Ink crowded = inkOf(renderImage(documentWith(kept)));
	check(!dropped.isEmpty(), "the narrow divider still draws");
	check(dropped.height() < crowded.height(), "the optional flourish was dropped");
	checkEq(boxesOutsideContent(documentWith(section)), 0, "and nothing overflows its box once it is");
}

CT_SUITE(divider_join, "A line run under the pieces beside it")
{
	/*
	 * Held apart, there are columns with no ink in them between the arrow and its rule; joined, there
	 * are none -- and the figure is exactly as wide and as tall either way, since a section that
	 * grew when its parts were joined would move everything under it.
	 */
	Section apart = divider({lane({shape(DividerShape::Arrow), line(), shape(DividerShape::Diamond)})});
	apart.divider.lineGap = 24.0;
	Section joined = apart;
	joined.divider.lanes[0].elements[1].join = true;

	const auto gaps = [](const QImage &image) {
		const Ink ink = inkOf(image);
		int empty = 0;
		for (int x = ink.left; x <= ink.right; ++x) {
			if (!inksColumn(image, x))
				++empty;
		}
		return empty;
	};

	const QImage apartImage = renderImage(documentWith(apart));
	const QImage joinedImage = renderImage(documentWith(joined));
	check(gaps(apartImage) > 0, "held apart, the parts have air between them");
	checkEq(gaps(joinedImage), 0, "joined, there is none");
	checkEq(inkOf(joinedImage).width(), inkOf(apartImage).width(), "the figure is as wide either way");
	checkEq(measure(documentWith(joined)), measure(documentWith(apart)), "and as tall");

	/* A negative gap pushes them together by hand, and the same columns close up. */
	Section overlapped = apart;
	overlapped.divider.lineGap = -6.0;
	checkEq(gaps(renderImage(documentWith(overlapped))), 0, "a negative gap joins them too");

	/* However deep, nothing is pushed outside the section's own box. */
	Section deep = apart;
	deep.divider.lineGap = -kMaxDividerJoin;
	deep.divider.pieceGap = -kMaxDividerJoin;
	checkEq(boxesOutsideContent(documentWith(deep)), 0, "a join dragged to its end stays inside the box");
	const Ink deepInk = inkOf(renderImage(documentWith(deep)));
	const QRectF box = boxOf(documentWith(deep), LayoutBox::Kind::Divider);
	check(deepInk.left >= qRound(box.left()) - 2 && deepInk.right <= qRound(box.right()) + 2,
	      "and draws nothing past its edges");
}

CT_SUITE(divider_lanes, "Lanes: several different lines stacked")
{
	/*
	 * Three lanes, three different lines: a hairline, a heavy rule and a run of dots, the outer two
	 * inset. Down a column near the left end only the middle lane reaches; down one nearer the
	 * middle, all three are drawn as three separate bands of their own thicknesses.
	 */
	Section section =
		divider({lane({line()}, 1.0, 60.0), lane({line()}, 8.0), lane({line(DividerShape::Dots)}, 4.0, 60.0)});
	section.divider.laneGap = 10.0;

	const Document document = documentWith(section);
	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);

	checkNear(box.height(), 1.0 + 8.0 + 4.0 + 2 * 10.0, 1.0, "the divider is as tall as its stack of lanes");

	const auto bandsAt = [&image](int x) {
		int bands = 0;
		bool inside = false;
		for (int y = 0; y < image.height(); ++y) {
			const bool ink = qAlpha(image.pixel(x, y)) > 40;
			if (ink && !inside)
				++bands;
			inside = ink;
		}
		return bands;
	};

	/* The dots have air between them, so the most bands seen across a short run of columns. */
	const auto mostBandsNear = [&bandsAt](int x) {
		int most = 0;
		for (int dx = 0; dx < 16; ++dx)
			most = std::max(most, bandsAt(x + dx));
		return most;
	};
	checkEq(mostBandsNear(qRound(box.left()) + 20), 1, "near the end, only the middle lane reaches");
	checkEq(mostBandsNear(qRound(box.left()) + 300), 3, "further in, three separate lines");

	const Ink top = bandInk(image, box.top(), box.top() + 1.5);
	const Ink middle = bandInk(image, box.top() + 11.0, box.top() + 19.0);
	check(top.left > middle.left + 40, "an inset lane starts further in than the one beside it");

	/* Each lane is laid out on its own: a piece in the middle lane does not move the outer ones. */
	Section withPiece = section;
	withPiece.divider.lanes[1].elements.append(shape(DividerShape::Diamond, 0.3));
	const Ink outer = bandInk(renderImage(documentWith(withPiece)), box.top(), box.top() + 1.5);
	checkEq(outer.left, top.left, "the top lane starts where it did");
	checkEq(outer.right, top.right, "and ends where it did");
}

CT_SUITE(divider_span_lanes, "One ornament breaking every lane")
{
	DividerElement diamond = shape(DividerShape::Diamond, 0.5);
	Section section = divider({lane({line()}, 2.0), lane({line(), diamond}, 2.0), lane({line()}, 2.0)});
	section.divider.laneGap = 12.0;
	Section spanning = section;
	spanning.divider.lanes[1].elements[1].spanLanes = true;

	const auto topLaneAtCenter = [](const Section &s) {
		const Document document = documentWith(s);
		const QImage image = renderImage(document);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		/* The top lane's band, a few columns either side of the axis. */
		const qreal laneTop = box.center().y() - 2.0 - 12.0 - 2.0;
		return inkOf(image, QRectF(box.center().x() - 3.0, laneTop, 6.0, 2.0));
	};

	check(!topLaneAtCenter(section).isEmpty(), "a piece that does not span leaves the other lanes whole");
	check(topLaneAtCenter(spanning).isEmpty(), "one that spans cuts them around itself");
	checkEq(measure(documentWith(spanning)), measure(documentWith(section)), "without the divider moving");
}

CT_SUITE(divider_own_color, "A piece in a color of its own")
{
	DividerElement gold = shape(DividerShape::Square, 3.0);
	gold.ownColor = true;
	gold.color = QColor(250, 180, 20);

	Section section = divider({lane({line(), gold})});
	section.style.color = QColor(255, 255, 255);

	const Document document = documentWith(section);
	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);

	const QColor center = image.pixelColor(qRound(box.center().x()), qRound(box.center().y()));
	const QColor rule = image.pixelColor(qRound(box.left()) + 20, qRound(box.center().y()));
	check(center.red() > 200 && center.blue() < 80, "the piece is drawn in its own color");
	check(rule.red() > 200 && rule.green() > 200 && rule.blue() > 200, "and the rule in the divider's ink");

	/* Text takes its own color the same way. */
	DividerElement label = word(QStringLiteral("III"));
	label.ownColor = true;
	label.color = QColor(0, 200, 0);
	Section worded = divider({lane({line(), label})});
	const QImage wordImage = renderImage(documentWith(worded));
	int green = 0;
	for (int y = 0; y < wordImage.height(); ++y) {
		for (int x = 0; x < wordImage.width(); ++x) {
			const QColor c = wordImage.pixelColor(x, y);
			if (c.alpha() > 200 && c.green() > 150 && c.red() < 80)
				++green;
		}
	}
	check(green > 10, "a word in a color of its own is set in it");
}

CT_SUITE(divider_rotation, "A piece turned about its own center")
{
	Section square = divider({lane({line(), shape(DividerShape::Square)})});
	Section turned = square;
	turned.divider.lanes[0].elements[1].rotation = 45.0;

	const Ink standing = inkOf(renderImage(documentWith(square)));
	const Ink diamond = inkOf(renderImage(documentWith(turned)));
	check(diamond.height() > standing.height() + 2, "a square set on its corner reaches further up and down");
	check(measure(documentWith(turned)) > measure(documentWith(square)) + 2, "and the divider grew to hold it");
	checkNear(diamond.width(), standing.width(), 3.0, "while the run of the rule is untouched");

	/* A turned piece in a symmetric half and its reflection lean the same way about the rule. */
	DividerElement arrow = shape(DividerShape::Arrow);
	arrow.rotation = 30.0;
	Section leaning = divider({lane({arrow, line(), shape(DividerShape::Diamond)})});
	leaning.marginX = 40;
	const Document document = documentWith(leaning);
	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	const int middle = qRound(box.center().x());
	const Ink left = inkOf(image, QRect(qRound(box.left()), 0, middle - qRound(box.left()), image.height()));
	const Ink right = inkOf(image, QRect(middle, 0, qRound(box.right()) - middle, image.height()));
	checkNear(left.top, right.top, 2.0, "each end leans the same way about the rule");
	checkNear(left.height(), right.height(), 2.0, "and reaches as far");
}

CT_SUITE(divider_offset, "An element nudged off its lane's midline")
{
	Section level = divider({lane({line(), shape(DividerShape::Dot)})});
	Section raised = level;
	raised.divider.lanes[0].elements[1].offsetY = -20.0;

	check(measure(documentWith(raised)) > measure(documentWith(level)) + 10, "the divider grows to hold it");
	checkEq(boxesOutsideContent(documentWith(raised)), 0, "and keeps it inside");
}

CT_SUITE(divider_length, "A divider shorter than its section, placed within it")
{
	Section section = divider({lane({line()})});
	section.divider.length = 0.5;

	for (HAlign align : {HAlign::Left, HAlign::Center, HAlign::Right}) {
		Section placed = section;
		placed.divider.align = align;
		const Document document = documentWith(placed);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		const Ink ink = inkOf(renderImage(document));
		checkNear(box.width(), document.width * 0.5, 1.0, "the divider spans half the content");
		checkNear(ink.width(), box.width(), 2.0, "and draws across exactly that");
		if (align == HAlign::Left)
			checkNear(ink.left, 0.0, 2.0, "left-aligned at the left edge");
		else if (align == HAlign::Right)
			checkNear(ink.right, document.width - 1.0, 2.0, "right-aligned at the right edge");
		else
			checkNear((ink.left + ink.right) / 2.0, document.width / 2.0, 2.0, "centered in the middle");
	}
}

CT_SUITE(divider_every_shape, "Every shape as a piece and every line, drawn inside the box")
{
	for (DividerShape which : dividerShapesForRole(DividerRolePiece)) {
		if (dividerShapeIsEmpty(which) || dividerShapeUsesFile(which))
			continue;
		const Context context(QString::fromUtf8(dividerShapeId(which)));
		Section section = divider({lane({line(), shape(which)})});
		const Document document = documentWith(section);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		const Ink ink = inkOf(renderImage(document));
		check(box.height() > 6.0 + 1.0, "drawn taller than the rule it sits on");
		check(ink.left >= qRound(box.left()) - 2 && ink.right <= qRound(box.right()) + 2, "inside the box");
	}

	for (DividerShape which : dividerShapesForRole(DividerRoleArm)) {
		if (dividerShapeUsesFile(which))
			continue;
		const Context context(QString::fromUtf8(dividerShapeId(which)));
		Section section = divider({lane({line(which)})});
		const Ink ink = inkOf(renderImage(documentWith(section)));
		check(!ink.isEmpty(), "the line draws");
		checkEq(boxesOutsideContent(documentWith(section)), 0, "inside the box");
	}
}

CT_SUITE(divider_designs, "Finished designs to start a divider from")
{
	const QVector<DividerDesign> &designs = dividerDesigns();
	check(designs.size() >= 12, "there is a real choice of designs");

	QStringList ids;
	bool anyLanes = false;
	bool anyAsymmetric = false;
	for (const DividerDesign &design : designs) {
		const Context context(QString::fromLatin1(design.id));
		check(!ids.contains(QString::fromLatin1(design.id)), "ids are unique");
		ids.append(QString::fromLatin1(design.id));
		check(findDividerDesign(design.id) == &design, "a design is found by its own id");
		anyLanes = anyLanes || design.figure.lanes.size() > 1;
		anyAsymmetric = anyAsymmetric || !design.figure.symmetric;

		/* Picked onto a section that was something else entirely. */
		Section section = Section::makeDefault(SectionType::SectionDivider);
		section.divider.lanes = {lane({line(DividerShape::Dots)}), lane({line()})};
		section.divider.length = 0.7;
		section.divider.align = HAlign::Right;
		section.divider.tint = false;
		section.paddingTop = 3;
		section.style.color = QColor(10, 200, 30);
		applyDividerDesign(section, design);

		check(matchingDividerDesign(section) == &design, "a freshly picked design is recognized as itself");
		checkNear(section.divider.length, 0.7, 0.001, "the length is the section's, not the design's");
		check(section.divider.align == HAlign::Right && !section.divider.tint, "and so are alignment and tint");
		checkEq(section.paddingTop, 3, "and padding");
		check(section.style.color == QColor(10, 200, 30), "and style");

		const Document document = documentWith(section);
		check(measure(document) > 0, "the design draws something");
		checkEq(boxesOutsideContent(document), 0, "nothing outside the content box");
		check(!inkOf(renderImage(document)).isEmpty(), "and inks it");
		saveArtifact(QStringLiteral("divider_design_%1").arg(QString::fromLatin1(design.id)), document);

		OBSDataAutoRelease data = obs_data_create();
		section.save(data);
		Section loaded;
		loaded.load(data);
		check(matchingDividerDesign(loaded) == &design, "a saved design reloads as the same design");
	}
	check(anyLanes, "at least one design stacks several lanes");
	check(anyAsymmetric, "and at least one is not symmetric");

	const DividerDesign *fresh = matchingDividerDesign(Section::makeDefault(SectionType::SectionDivider));
	check(fresh && QString::fromLatin1(fresh->id) == QStringLiteral("arrow_rule"),
	      "a new divider is the arrow rule");

	Section edited = Section::makeDefault(SectionType::SectionDivider);
	edited.divider.lanes[0].elements.removeLast();
	check(matchingDividerDesign(edited) == nullptr, "taking a piece out makes it the section's own design");

	Section heavier = Section::makeDefault(SectionType::SectionDivider);
	heavier.divider.lanes[0].thickness += 1.0;
	check(matchingDividerDesign(heavier) == nullptr, "and so does a heavier line");

	Section placed = Section::makeDefault(SectionType::SectionDivider);
	placed.divider.laneGap = 40.0;
	placed.divider.length = 0.3;
	placed.sectionWidth = 0.3;
	check(matchingDividerDesign(placed) != nullptr,
	      "a single lane's lane gap, the length and the section's own settings are not the figure");
}

CT_SUITE(divider_legacy_draws, "An old divider draws after it is brought into lanes")
{
	/* The old default, written the old way: mirrored arrow ends, a rule, dot-diamond-dot. */
	OBSDataAutoRelease old = obs_data_create();
	obs_data_set_string(old, "type", "section_divider");
	obs_data_set_string(old, "divider_cap", "arrow");
	obs_data_set_double(old, "divider_thickness", 5.0);
	OBSDataArrayAutoRelease center = obs_data_array_create();
	for (const char *which : {"dot", "diamond", "dot"}) {
		OBSDataAutoRelease piece = obs_data_create();
		obs_data_set_string(piece, "kind", "ornament");
		obs_data_set_string(piece, "shape", which);
		obs_data_array_push_back(center, piece);
	}
	obs_data_set_array(old, "divider_centre", center);

	Section loaded;
	loaded.load(old);
	loaded.visible = true;
	loaded.sectionWidth = 1.0;
	loaded.marginX = 0;
	loaded.paddingTop = loaded.paddingBottom = 0;

	const DividerDesign *design = findDividerDesign("arrow_rule");
	check(design && matchingDividerDesign(loaded) == design, "the old default is the arrow rule design");

	const QImage image = renderImage(documentWith(loaded));
	check(!inkOf(image).isEmpty(), "and draws");
	checkEq(asymmetricColumns(image), 0, "symmetric, as it always was");

	/* Different ends and a middle slid to one side become a row that is not symmetric. */
	OBSDataAutoRelease slid = obs_data_create();
	obs_data_set_string(slid, "type", "section_divider");
	obs_data_set_double(slid, "divider_center_position", 0.0);
	OBSDataArrayAutoRelease words = obs_data_array_create();
	OBSDataAutoRelease piece = obs_data_create();
	obs_data_set_string(piece, "kind", "text");
	obs_data_set_string(piece, "text", "ACT II");
	obs_data_array_push_back(words, piece);
	obs_data_set_array(slid, "divider_centre", words);

	Section heading;
	heading.load(slid);
	check(!heading.divider.symmetric, "a middle off the middle is not symmetric");
	if (!heading.divider.lanes.isEmpty() && !heading.divider.lanes.first().elements.isEmpty())
		check(heading.divider.lanes.first().elements.first().kind == DividerElement::Kind::Text,
		      "and the word leads the row, as it led the rule");
}

namespace {

LogoCache &revealLogos()
{
	static LogoCache cache;
	return cache;
}

/* The columns a divider's layer is drawn across, with the divider's middle at `anchorY` on the canvas. */
QPair<double, double> drawnColumns(const RollLayout &layout, int layerIndex, double anchorY, bool *any,
				   int canvasHeight = 1000)
{
	const RollLayer &layer = layout.layers.at(layerIndex);
	RollFrame frame;
	/* Where the strip has to be for the anchor to sit at `anchorY`: stripTop + anchor = anchorY. */
	frame.offset = canvasHeight + layer.reveal.anchor - anchorY;
	const DrawList list = composeFrame(layout, frame, ComposeView::canvas(frame, canvasHeight));

	double left = 1e9;
	double right = -1e9;
	*any = false;
	for (const DrawItem &item : list) {
		if (item.source.kind != DrawSource::Kind::Chunk || item.source.layer != layerIndex)
			continue;
		*any = true;
		left = std::min(left, item.target.left());
		right = std::max(right, item.target.right());
	}
	return {left, right};
}

} // namespace

CT_SUITE(divider_reveal, "A divider drawing itself on as it scrolls into frame")
{
	Section section = divider({lane({shape(DividerShape::Arrow), line(), shape(DividerShape::Diamond)})});
	section.paddingTop = section.paddingBottom = 40;
	section.divider.length = 0.5;
	section.divider.reveal = DividerReveal::FromCenter;
	section.divider.revealStart = 0.9;
	section.divider.revealLength = 0.25;

	Document document = documentWith({section});
	document.height = 1000;
	const RollLayoutPtr layout = RollRenderer(&revealLogos()).layout(document);
	check(layout && layout->layers.size() == 1, "the divider is a layer of its own");
	if (!layout || layout->layers.size() != 1)
		return;

	const LayerReveal &reveal = layout->layers.first().reveal;
	check(reveal.active(), "and carries its reveal");
	checkNear(reveal.right - reveal.left, document.width * 0.5, 1.0, "across the divider's own box");
	const double middle = (reveal.left + reveal.right) / 2.0;

	bool any = false;
	drawnColumns(*layout, 0, 950.0, &any);
	check(!any, "below the start line nothing of it is drawn");

	/* Halfway: the anchor has risen half the length past the start line. */
	const QPair<double, double> half = drawnColumns(*layout, 0, 900.0 - 0.125 * 1000.0, &any);
	check(any, "partway, some of it is drawn");
	checkNear((half.first + half.second) / 2.0, middle, 1.5, "from the center, it grows about its middle");
	const double fullWidth = reveal.right - reveal.left + 2.0 * reveal.bleed;
	checkNear(half.second - half.first, fullWidth / 2.0, 2.0, "and is half drawn halfway through");

	const QPair<double, double> done = drawnColumns(*layout, 0, 500.0, &any);
	check(any, "once complete it is drawn");
	checkNear(done.first, 0.0, 0.5, "the whole picture, uncut");
	checkNear(done.second, layout->layers.first().width, 1.0, "edge to edge");

	/* From the left: the drawn span starts at the box's left edge and grows rightward. */
	Section leftward = section;
	leftward.divider.reveal = DividerReveal::FromLeft;
	Document leftDocument = documentWith({leftward});
	leftDocument.height = 1000;
	const RollLayoutPtr leftLayout = RollRenderer(&revealLogos()).layout(leftDocument);
	const LayerReveal &leftReveal = leftLayout->layers.first().reveal;
	const QPair<double, double> pen = drawnColumns(*leftLayout, 0, 900.0 - 0.125 * 1000.0, &any);
	checkNear(pen.first, leftReveal.left - leftReveal.bleed, 1.0, "from the left, it starts at the left edge");
	check(pen.second < (leftReveal.left + leftReveal.right) / 2.0 + 2.0, "and has reached about halfway");

	/* The roll laid end to end -- the designer's overview -- shows every divider whole. */
	ComposeView overview;
	overview.blocks = ComposeView::Blocks::InSlots;
	overview.clipTop = -1e9;
	overview.clipBottom = 1e9;
	bool whole = false;
	for (const DrawItem &item : composeFrame(*layout, RollFrame(), overview))
		whole = whole || (item.target.left() <= 0.5 && item.target.right() >= layout->width - 1.0);
	check(whole, "a view that is not the canvas shows it whole");

	/* No reveal, no cut; and the picture is the same picture either way. */
	Section plain = section;
	plain.divider.reveal = DividerReveal::None;
	Document plainDocument = documentWith({plain});
	plainDocument.height = 1000;
	const RollLayoutPtr plainLayout = RollRenderer(&revealLogos()).layout(plainDocument);
	check(!plainLayout->layers.first().reveal.active(), "a divider with no reveal carries none");
	const QPair<double, double> early = drawnColumns(*plainLayout, 0, 950.0, &any);
	check(any && early.first <= 0.5, "and is drawn whole as soon as it is on the canvas");
	checkEq(plainLayout->layers.first().key, layout->layers.first().key,
		"a reveal does not change the picture, so the picture is not drawn again");

	/* Saved and loaded with everything else. */
	OBSDataAutoRelease data = obs_data_create();
	section.save(data);
	Section loaded;
	loaded.load(data);
	check(loaded.divider.reveal == DividerReveal::FromCenter, "the reveal is saved");
	checkNear(loaded.divider.revealStart, 0.9, 0.0001, "with where it starts");
	checkNear(loaded.divider.revealLength, 0.25, 0.0001, "and how long it takes");
}
