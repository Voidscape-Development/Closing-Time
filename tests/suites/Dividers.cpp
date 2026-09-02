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

namespace {

DividerPiece ornament(DividerShape shape, double scale = 1.0)
{
	return DividerPiece{DividerPiece::Kind::Ornament, shape, {}, scale, {}, {}};
}

DividerPiece word(const QString &text)
{
	return DividerPiece{DividerPiece::Kind::Text, DividerShape::None, {}, 1.0, text, {}};
}

/* A divider with nothing but its own artwork in the document, and no padding to subtract. */
Section divider()
{
	Section section = unpadded(SectionType::SectionDivider);
	section.sectionWidth = 1.0;
	section.marginX = 0;
	section.dividerCap.clear();
	section.dividerEndCap.clear();
	section.dividerCenter.clear();
	section.dividerArm = DividerShape::Rule;
	section.dividerThickness = 6.0;
	return section;
}

} // namespace

CT_SUITE(divider_shape_parity, "Every piece shape offered wherever a piece can go")
{
	/*
	 * An end and a middle hold the same kind of piece, so they draw on the same list. The point
	 * of the check is the crossover: a shape the library used to offer only as an end has to
	 * turn up in the middle, and one it used to offer only as a centerpiece has to turn up at
	 * an end.
	 */
	const QVector<DividerShape> &pieces = dividerShapesForRole(DividerRolePiece);

	check(pieces.contains(DividerShape::Arrow), "an arrowhead can break a rule as well as cap one");
	check(pieces.contains(DividerShape::ScrollEnd), "so can a scroll end");
	check(pieces.contains(DividerShape::Heart), "and a heart can cap a rule as well as break one");
	check(pieces.contains(DividerShape::Filigree), "and so can a filigree");
	check(pieces.contains(DividerShape::None), "None is offered, since a slot may hold nothing");
	check(pieces.contains(DividerShape::Custom), "and so is a file of the user's own");

	/* An arm is the one genuinely different job, and stays its own list. */
	const QVector<DividerShape> &arms = dividerShapesForRole(DividerRoleArm);
	check(arms.contains(DividerShape::Rule), "a rule is an arm");
	check(!arms.contains(DividerShape::Heart), "a heart is not");
	check(!pieces.contains(DividerShape::Rule), "and a rule is not a piece");

	for (DividerShape shape : pieces)
		check(dividerShapeHasRole(shape, DividerRolePiece), "the list agrees with the table");
}

CT_SUITE(divider_end_stack, "An end built from more than one piece")
{
	Section one = divider();
	one.dividerCap = {ornament(DividerShape::Diamond)};

	Section three = divider();
	three.dividerCap = {ornament(DividerShape::Diamond), ornament(DividerShape::Dot), ornament(DividerShape::Dot)};

	const QRectF oneBox = boxOf(documentWith(one), LayoutBox::Kind::Divider);
	const QRectF threeBox = boxOf(documentWith(three), LayoutBox::Kind::Divider);
	check(!oneBox.isNull() && !threeBox.isNull(), "both dividers were placed");

	/*
	 * The figure spans the section either way -- the arms take up the slack -- so what a longer
	 * end has to change is where the rule starts, not how wide the divider is. Measured as the
	 * gap between the leftmost ink and the first unbroken run of the rule's own thickness would
	 * be fragile; the plain statement is that a taller stack of pieces is a taller divider when
	 * the pieces are scaled up, and that the pieces are all drawn.
	 */
	checkNear(threeBox.width(), oneBox.width(), 1.0, "an end stack does not widen the divider");

	Section scaled = three;
	scaled.dividerCap = {ornament(DividerShape::Diamond, 2.0)};
	check(measure(documentWith(scaled)) > measure(documentWith(one)), "a scaled-up end piece makes it taller");

	checkEq(boxesOutsideContent(documentWith(three)), 0, "nothing placed outside the content box");
}

CT_SUITE(divider_end_mirrored, "The right-hand end as the left one flipped")
{
	/*
	 * A deliberately lopsided end: a big diamond outside a small dot. Mirrored, the whole figure
	 * has to come out symmetric about the section's own center -- which catches both halves of
	 * the rule at once, since the order has to reverse *and* each piece's artwork has to flip.
	 */
	Section section = divider();
	section.dividerCap = {ornament(DividerShape::Arrow, 1.5), ornament(DividerShape::Dot)};
	section.dividerMirrorEnds = true;

	const Document document = documentWith(section);
	const QImage image = renderImage(document);

	int asymmetric = 0;
	for (int x = 0; x < image.width() / 2; ++x) {
		if (inksColumn(image, x) != inksColumn(image, image.width() - 1 - x))
			++asymmetric;
	}
	checkEq(asymmetric, 0, "a mirrored divider inks the same columns at both ends");

	/* And an end of its own is still drawn at the right-hand side rather than left off. */
	Section separate = section;
	separate.dividerMirrorEnds = false;
	separate.dividerEndCap = {ornament(DividerShape::Diamond, 3.0)};

	const Ink ink = inkOf(renderImage(documentWith(separate)));
	const Ink mirroredInk = inkOf(image);
	check(!ink.isEmpty(), "a divider with two different ends still draws");
	check(ink.height() > mirroredInk.height(), "and the taller far end is what sets its height");
}

CT_SUITE(divider_end_words, "A word and a picture at the end of a rule")
{
	/*
	 * The parity that matters most: what the middle could always hold, an end can hold too. A
	 * word at an end has to be drawn, has to count towards the fonts the roll uses, and must not
	 * be flipped along with the ornaments around it.
	 */
	Section section = divider();
	section.dividerCap = {word(QStringLiteral("MMXXVI"))};
	section.dividerMirrorEnds = true;
	section.style.family = QStringLiteral("Sans Serif");

	const Document document = documentWith(section);
	check(document.usedFontFamilies().contains(QStringLiteral("Sans Serif")),
	      "a word at an end counts towards the roll's fonts");

	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	check(!box.isNull(), "the divider was placed");

	/* Ink at both ends: the word is drawn at each, not only where it was written. */
	const Ink leftInk = inkOf(image, QRectF(box.left(), box.top(), box.width() / 4.0, box.height()));
	const Ink rightInk =
		inkOf(image, QRectF(box.right() - box.width() / 4.0, box.top(), box.width() / 4.0, box.height()));
	check(!leftInk.isEmpty(), "the word is drawn at the left-hand end");
	check(!rightInk.isEmpty(), "and at the right-hand one");

	/*
	 * Type is not mirrored, so the two ends are *not* pixel-for-pixel reflections here -- which
	 * is the whole point of leaving words and pictures unflipped. A run of asymmetric columns is
	 * what proves it, against the run of zero the ornament-only divider above measures.
	 */
	int asymmetric = 0;
	for (int x = 0; x < image.width() / 2; ++x) {
		if (inksColumn(image, x) != inksColumn(image, image.width() - 1 - x))
			++asymmetric;
	}
	check(asymmetric > 0, "a word at an end is drawn the right way round rather than reflected");
}

CT_SUITE(divider_end_empty, "An end with nothing on it")
{
	/*
	 * The ordinary case, and the one a document written before ends were stacks migrates to: an
	 * empty end is a rule that runs to the edge of the section rather than a hole where a piece
	 * would have been.
	 */
	Section bare = divider();
	const Document document = documentWith(bare);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	const Ink ink = inkOf(renderImage(document));

	check(!ink.isEmpty(), "a divider with no ends still draws its rule");
	checkNear(ink.left, box.left(), 2.0, "the rule reaches the left edge of the section");
	checkNear(ink.right, box.right(), 2.0, "and the right one");

	/* A piece that measures to nothing is dropped along with its gap, wherever it sits. */
	Section empties = divider();
	empties.dividerCap = {word(QString()), ornament(DividerShape::None)};
	checkNear(inkOf(renderImage(documentWith(empties))).left, ink.left, 2.0,
		  "pieces that draw nothing take no room at an end");
}

namespace {

/* Columns inside `box` that carry no ink at all -- the breaks in a divider, counted. */
int breaksIn(const QImage &image, const QRectF &box)
{
	int breaks = 0;
	for (int x = qRound(box.left()); x <= qRound(box.right()) && x < image.width(); ++x) {
		if (!inksColumn(image, x))
			++breaks;
	}
	return breaks;
}

} // namespace

CT_SUITE(divider_connected_rule, "A divider whose parts are joined into one figure")
{
	/*
	 * The complaint this answers: a composed divider reads as a row of separate marks, because
	 * every join in it is a gap. Joined, the rule runs the whole way through and the ornaments
	 * sit *on* it -- so what the check measures is the absence of a break, column by column.
	 */
	Section apart = divider();
	apart.dividerCap = {ornament(DividerShape::Arrow)};
	apart.dividerCenter = {ornament(DividerShape::Diamond)};
	apart.dividerGap = 30.0;

	const Document apartDocument = documentWith(apart);
	const QRectF box = boxOf(apartDocument, LayoutBox::Kind::Divider);
	check(!box.isNull(), "the divider was placed");
	check(breaksIn(renderImage(apartDocument), box) > 0, "held apart, the rule stops short of what it meets");

	Section joined = apart;
	joined.dividerConnect = true;

	const Document joinedDocument = documentWith(joined);
	const QImage joinedImage = renderImage(joinedDocument);
	checkEq(breaksIn(joinedImage, box), 0, "joined, there is no column of the divider without ink in it");

	/*
	 * The figure is the same width either way. Connecting runs the rule further, not the
	 * divider: a section that grew when its parts were joined would move everything under it.
	 */
	const QRectF joinedBox = boxOf(joinedDocument, LayoutBox::Kind::Divider);
	checkNear(joinedBox.width(), box.width(), 1.0, "joining does not widen the divider");
	checkNear(measure(joinedDocument), measure(apartDocument), 1.0, "nor make it taller");

	/*
	 * The rule stops at the middle of the outermost piece rather than at the section's edge, so
	 * a cap still ends the figure: the ink reaches the same place it did unjoined.
	 */
	const Ink ink = inkOf(joinedImage);
	checkNear(ink.left, box.left(), 2.0, "the joined figure still ends where the cap does");
	checkNear(ink.right, box.right(), 2.0, "at both ends");

	/* A gap that is not consulted cannot change the drawing. */
	Section widened = joined;
	widened.dividerGap = 400.0;
	checkEq(breaksIn(renderImage(documentWith(widened)), box), 0, "the end gap is not consulted once joined");
}

CT_SUITE(divider_negative_join, "Parts pushed into each other rather than held apart")
{
	/*
	 * The other half of making a divider read as one piece, and the half that reaches artwork
	 * the connect switch cannot: a stack whose pieces overlap. Measured on the center alone,
	 * with no arm drawn, so the ink is exactly the stack's own width.
	 */
	Section section = divider();
	section.dividerArm = DividerShape::None;
	section.dividerCenter = {ornament(DividerShape::Diamond), ornament(DividerShape::Diamond),
				 ornament(DividerShape::Diamond)};

	section.dividerPieceGap = 0.0;
	const Ink touching = inkOf(renderImage(documentWith(section)));
	check(!touching.isEmpty(), "three diamonds in a row were drawn");

	section.dividerPieceGap = 20.0;
	const Ink spaced = inkOf(renderImage(documentWith(section)));
	checkNear(spaced.width(), touching.width() + 40.0, 2.0, "a positive gap opens both joins");

	/*
	 * Bounded at half the narrower of the two pieces a join sits between. Three equal diamonds
	 * touching are three piece widths across, so both joins clamped takes exactly one of those
	 * widths back out -- and asking for very much more takes no more than that.
	 */
	const double pieceWidth = touching.width() / 3.0;

	section.dividerPieceGap = -100000.0;
	const Ink crushed = inkOf(renderImage(documentWith(section)));
	checkNear(crushed.width(), touching.width() - pieceWidth, 3.0,
		  "an unbounded overlap is held to half a piece per join");

	section.dividerPieceGap = -pieceWidth / 4.0;
	const Ink overlapped = inkOf(renderImage(documentWith(section)));
	checkNear(overlapped.width(), touching.width() - pieceWidth / 2.0, 3.0,
		  "and an overlap inside that bound is taken at face value");

	check(overlapped.width() < touching.width() && crushed.width() < overlapped.width(),
	      "the stack narrows as the join goes further below zero");
}

CT_SUITE(divider_join_stays_inside, "An overlap deep enough to push the rule out of the section")
{
	/*
	 * A join is a setting somebody can drag to its end, and the arms are what it moves. Whatever
	 * it is set to, the rule stays inside the section's own box: past that it would be painting
	 * over whatever the section above or below drew.
	 */
	Section section = divider();
	section.marginX = 60;
	section.dividerCap = {ornament(DividerShape::Arrow)};
	section.dividerCenter = {ornament(DividerShape::Diamond)};
	section.dividerGap = -kMaxDividerJoin;

	const Document document = documentWith(section);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
	const Ink ink = inkOf(renderImage(document));

	check(!ink.isEmpty(), "the divider still draws");
	check(ink.left >= qRound(box.left()) - 2, "no ink to the left of the section's own box");
	check(ink.right <= qRound(box.right()) + 2, "and none to the right of it");
	checkEq(boxesOutsideContent(document), 0, "nothing placed outside the content box");
}

CT_SUITE(divider_plain_shapes, "The plain shapes, filled and outlined")
{
	/*
	 * A shape whose artwork will not parse measures to nothing and is silently not drawn, so a
	 * typo in one of the tiles would otherwise cost a shape in the picker and nothing else. Each
	 * of the plain ones is therefore asked for a size and then asked to draw.
	 */
	const QVector<DividerShape> plain = {DividerShape::Circle,        DividerShape::Ring,
					     DividerShape::Square,        DividerShape::SquareOutline,
					     DividerShape::RoundedSquare, DividerShape::RoundedSquareOutline,
					     DividerShape::Triangle,      DividerShape::TriangleOutline,
					     DividerShape::Pentagon,      DividerShape::PentagonOutline,
					     DividerShape::Hexagon,       DividerShape::HexagonOutline,
					     DividerShape::Octagon,       DividerShape::OctagonOutline};

	const QVector<DividerShape> &pieces = dividerShapesForRole(DividerRolePiece);

	for (DividerShape shape : plain) {
		const QString name = QString::fromUtf8(dividerShapeName(shape));

		check(pieces.contains(shape), QStringLiteral("%1 is offered wherever a piece can go").arg(name));
		check(!dividerShapesForRole(DividerRoleArm).contains(shape),
		      QStringLiteral("%1 is a piece rather than a rule").arg(name));

		Section section = divider();
		section.dividerCenter = {ornament(shape)};

		const Document document = documentWith(section);
		const QRectF box = boxOf(document, LayoutBox::Kind::Divider);
		const Ink ink = inkOf(renderImage(document));

		check(!ink.isEmpty(), QStringLiteral("%1 draws something").arg(name));
		/*
		 * A centerpiece is drawn taller than the rule it breaks, so a shape that came out as
		 * nothing but the arms either side of it would leave the divider exactly one rule
		 * high -- which is what this height is asking about.
		 */
		check(box.height() > section.dividerThickness + 1.0,
		      QStringLiteral("%1 is drawn at a centerpiece's height").arg(name));
		check(ink.left >= qRound(box.left()) - 2 && ink.right <= qRound(box.right()) + 2,
		      QStringLiteral("%1 stays inside the section's box").arg(name));
	}
}

CT_SUITE(divider_piece_rotation, "A part turned about its own center")
{
	/*
	 * Turned where it stands: along the rule the piece takes the room its untilted shape took,
	 * so the arms either side of it do not move while an angle is dialed in. The divider's
	 * height is the one thing that follows the angle, because a section is only as tall as it
	 * says it is and the corners would otherwise be cut off.
	 */
	Section square = divider();
	square.dividerCenter = {ornament(DividerShape::Square)};

	Section turned = square;
	turned.dividerCenter[0].rotation = 45.0;

	const Document squareDocument = documentWith(square);
	const Document turnedDocument = documentWith(turned);

	const QRectF standingBox = boxOf(squareDocument, LayoutBox::Kind::Divider);
	const QRectF turnedBox = boxOf(turnedDocument, LayoutBox::Kind::Divider);

	checkNear(turnedBox.width(), standingBox.width(), 0.001, "the divider runs the same way across");

	const Ink standing = inkOf(renderImage(squareDocument));
	const Ink diamond = inkOf(renderImage(turnedDocument));

	check(!diamond.isEmpty(), "a turned piece is still drawn");
	/* A square's diagonal is half again its side, so a corner-on square is plainly taller. */
	check(diamond.height() > standing.height() + 2, "a square set on its corner reaches further up and down");
	check(turnedBox.height() > standingBox.height() + 2, "and the divider grew to hold it rather than cut it off");
	checkNear(diamond.width(), standing.width(), 3.0, "while the run of the rule is untouched");

	/* A quarter turn on a square is the square again, near enough to be worth checking. */
	Section quarter = square;
	quarter.dividerCenter[0].rotation = 90.0;
	const Ink upright = inkOf(renderImage(documentWith(quarter)));
	checkNear(upright.height(), standing.height(), 2.0, "a quarter turn of a square is the square");
}

CT_SUITE(divider_rotation_mirrors, "A turned end and its reflection at the other end")
{
	/*
	 * The two ends of a mirrored rule are each other's reflection, angle and all: an arrowhead
	 * leaning up at the left-hand end leans up at the right-hand one too rather than down, which
	 * is what a flip about the divider's own axis does to it.
	 */
	Section section = divider();
	section.marginX = 40;
	section.dividerCap = {ornament(DividerShape::Arrow)};
	section.dividerMirrorEnds = true;
	section.dividerCenter = {ornament(DividerShape::Diamond)};
	section.dividerCap[0].rotation = 30.0;

	const Document document = documentWith(section);
	const QImage image = renderImage(document);
	const QRectF box = boxOf(document, LayoutBox::Kind::Divider);

	const int middle = qRound(box.center().x());
	const QRect leftHalf(qRound(box.left()), 0, middle - qRound(box.left()), image.height());
	const QRect rightHalf(middle, 0, qRound(box.right()) - middle, image.height());

	const Ink left = inkOf(image, leftHalf);
	const Ink right = inkOf(image, rightHalf);

	check(!left.isEmpty() && !right.isEmpty(), "both ends are drawn");
	checkNear(left.height(), right.height(), 2.0, "each end reaches as far up and down as the other");
	checkNear(left.top, right.top, 2.0, "and leans the same way about the rule");
}
