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

#include "model/DividerDesigns.hpp"

#include <cmath>
#include <cstring>

namespace closingtime {

namespace {

DividerElement piece(DividerShape shape, double scale = 1.0)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Shape;
	element.shape = shape;
	element.scale = scale;
	return element;
}

DividerElement line(DividerShape shape = DividerShape::Rule, bool join = false)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Line;
	element.lineShape = shape;
	element.join = join;
	return element;
}

DividerElement word(const char *text)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Text;
	element.text = QString::fromUtf8(text);
	return element;
}

DividerElement spanning(DividerElement element)
{
	element.spanLanes = true;
	return element;
}

DividerLane lane(double thickness, QVector<DividerElement> elements, double inset = 0.0)
{
	DividerLane built;
	built.thickness = thickness;
	built.inset = inset;
	built.elements = std::move(elements);
	return built;
}

/*
 * Each design is written the way the editor shows it: a symmetric divider as its left half, outside
 * in, with the piece written last set on the center line. Only the fields that make a design what it
 * is are named; everything else is the plain divider's.
 */
QVector<DividerDesign> buildDesigns()
{
	QVector<DividerDesign> designs;

	const auto add = [&designs](const char *id, const char *name, QVector<DividerLane> lanes) -> Divider & {
		DividerDesign design{id, name, Divider{}};
		design.figure.lanes = std::move(lanes);
		designs.append(design);
		return designs.last().figure;
	};

	/* The one a new divider starts as: see Section::makeDefault. */
	add("arrow_rule", "Arrow Rule",
	    {lane(5.0, {piece(DividerShape::Arrow), line(), piece(DividerShape::Dot), piece(DividerShape::Diamond)})});

	add("plain_rule", "Plain Rule", {lane(3.0, {line()})});

	/* Two tapers thick at a diamond and drawn down to hairlines at the ends: no caps at all. */
	add("needle", "Needle", {lane(5.0, {line(DividerShape::Taper), piece(DividerShape::Diamond, 0.8)})}).lineGap =
		8.0;

	/* A hairline the ornament sits *on*, which is what joining is for. */
	add("diamond_line", "Diamond on the Line",
	    {lane(2.0, {piece(DividerShape::Dot, 1.6), line(DividerShape::Rule, true),
			piece(DividerShape::NestedDiamond, 2.6)})});

	/* Three lines of their own -- hairlines either side of a heavier rule -- broken by one ornament. */
	{
		Divider &d = add("three_lines", "Three Lines",
				 {lane(1.0, {line()}, 18.0),
				  lane(5.0, {line(), spanning(piece(DividerShape::Diamond, 1.4))}),
				  lane(1.0, {line()}, 18.0)});
		d.laneGap = 7.0;
	}
	{
		Divider &d = add("deco_triple", "Deco Triple",
				 {lane(2.0, {line()}, 24.0),
				  lane(3.0, {spanning(piece(DividerShape::DecoStep, 2.0)), line(),
					     spanning(piece(DividerShape::DecoStack, 2.0))}),
				  lane(2.0, {line()}, 24.0)});
		d.laneGap = 5.0;
		d.lineGap = 10.0;
	}
	/* A rule over a run of dots over a rule: three different lines, nothing in the middle. */
	{
		Divider &d = add("railway", "Railway",
				 {lane(2.0, {line()}), lane(4.0, {line(DividerShape::Dots)}), lane(2.0, {line()})});
		d.laneGap = 7.0;
	}
	add("double_star", "Double Rule & Star",
	    {lane(6.0, {line(DividerShape::DoubleRule), piece(DividerShape::Star)})})
		.lineGap = 14.0;

	add("dotted", "Dotted Line", {lane(5.0, {line(DividerShape::Dots), piece(DividerShape::Sparkle, 1.6)})})
		.lineGap = 14.0;

	add("scrollwork", "Scrollwork",
	    {lane(3.0, {piece(DividerShape::ScrollEnd, 2.0), line(), piece(DividerShape::Scroll, 2.5)})})
		.lineGap = 10.0;

	add("filigree", "Filigree", {lane(4.0, {line(DividerShape::Taper), piece(DividerShape::Filigree, 2.0)})})
		.lineGap = 6.0;

	add("spearpoints", "Spearpoints",
	    {lane(4.0, {piece(DividerShape::Spear, 1.5), line(DividerShape::Rule, true),
			piece(DividerShape::Sparkle, 2.0)})});

	add("hearts", "Hearts",
	    {lane(4.0, {line(DividerShape::Dashes), piece(DividerShape::Heart), piece(DividerShape::Heart, 1.5)})})
		.pieceGap = 6.0;

	add("chain", "Diamond Chain",
	    {lane(6.0, {piece(DividerShape::Chevron), line(DividerShape::DiamondChain),
			piece(DividerShape::NestedDiamond)})})
		.lineGap = 10.0;

	add("ticked", "Ticked Rule", {lane(8.0, {piece(DividerShape::Chevron), line(DividerShape::DecoTicks)})})
		.lineGap = 8.0;

	add("geometric", "Geometric",
	    {lane(3.0, {piece(DividerShape::Circle, 1.4), line(DividerShape::Rule, true),
			piece(DividerShape::Ring, 1.6), piece(DividerShape::Hexagon, 2.6)})})
		.pieceGap = 8.0;

	/* Two colors: the diamond in gold of its own, the rule in the divider's ink. */
	{
		DividerElement gold = piece(DividerShape::NestedDiamond, 1.6);
		gold.ownColor = true;
		gold.color = QColor(232, 182, 74);
		add("two_tone", "Two-Tone",
		    {lane(3.0, {piece(DividerShape::Arrow), line(DividerShape::Rule, true), gold})});
	}

	/* A word breaking the rule, set in the section's own style. */
	{
		Divider &d = add("labeled", "Labeled Rule",
				 {lane(2.0, {piece(DividerShape::Dot, 1.6), line(), word("INTERMISSION")})});
		d.lineGap = 16.0;
		d.pieceGap = 16.0;
	}

	/* Not symmetric: the label leads the rule rather than breaking it. */
	{
		Divider &d = add("heading_rule", "Heading Rule", {lane(2.0, {word("CHAPTER ONE"), line()})});
		d.symmetric = false;
		d.lineGap = 20.0;
	}

	/* Not symmetric either: a rule that flies one way, thin at its tail and closed by a point. */
	{
		DividerElement head = piece(DividerShape::Arrow);
		head.flip = true;
		Divider &d = add("flight", "Flight",
				 {lane(4.0, {piece(DividerShape::Dot), line(DividerShape::Taper, true), head})});
		d.symmetric = false;
	}

	return designs;
}

bool near(double a, double b)
{
	return std::abs(a - b) < 1e-6;
}

bool sameElement(const DividerElement &a, const DividerElement &b, bool severalLanes)
{
	if (a.kind != b.kind || !near(a.offsetY, b.offsetY) || a.flip != b.flip || a.ownColor != b.ownColor ||
	    (a.ownColor && a.color != b.color))
		return false;

	/* Only what the element's own kind draws from: a word keeps the shape it was before, unread. */
	if (a.isLine()) {
		return a.lineShape == b.lineShape &&
		       (!dividerShapeUsesFile(a.lineShape) || a.lineSvgPath == b.lineSvgPath) &&
		       near(a.weight, b.weight) && near(a.minLength, b.minLength) && a.join == b.join &&
		       near(a.scale, b.scale);
	}

	if (!near(a.rotation, b.rotation) || a.optional != b.optional || (severalLanes && a.spanLanes != b.spanLanes))
		return false;

	switch (a.kind) {
	case DividerElement::Kind::Shape:
		return a.shape == b.shape && near(a.scale, b.scale) &&
		       (!dividerShapeUsesFile(a.shape) || a.svgPath == b.svgPath);
	case DividerElement::Kind::Text:
		return a.text == b.text;
	case DividerElement::Kind::Image:
		return a.logo.path == b.logo.path && a.logo.maxHeight == b.logo.maxHeight;
	case DividerElement::Kind::Line:
		break;
	}
	return false;
}

} // namespace

const QVector<DividerDesign> &dividerDesigns()
{
	static const QVector<DividerDesign> designs = buildDesigns();
	return designs;
}

const DividerDesign *findDividerDesign(const char *id)
{
	if (!id)
		return nullptr;

	for (const DividerDesign &design : dividerDesigns()) {
		if (std::strcmp(design.id, id) == 0)
			return &design;
	}
	return nullptr;
}

void applyDividerDesign(Section &section, const DividerDesign &design)
{
	const Divider kept = section.divider;
	section.divider = design.figure;
	section.divider.length = kept.length;
	section.divider.align = kept.align;
	section.divider.tint = kept.tint;
}

bool sameDividerFigure(const Divider &a, const Divider &b)
{
	if (a.symmetric != b.symmetric || a.lanes.size() != b.lanes.size() || !near(a.pieceGap, b.pieceGap) ||
	    !near(a.lineGap, b.lineGap))
		return false;

	const bool severalLanes = a.lanes.size() > 1;
	if (severalLanes && !near(a.laneGap, b.laneGap))
		return false;

	for (int i = 0; i < a.lanes.size(); ++i) {
		const DividerLane &x = a.lanes.at(i);
		const DividerLane &y = b.lanes.at(i);
		if (!near(x.thickness, y.thickness) || !near(x.inset, y.inset) || !near(x.offsetY, y.offsetY) ||
		    x.elements.size() != y.elements.size())
			return false;

		/* Whether the last piece sits on the axis matters only where there is one to sit there. */
		const bool axisPiece = a.symmetric && !x.elements.isEmpty() && !x.elements.last().isLine();
		if (axisPiece && x.centerPiece != y.centerPiece)
			return false;

		for (int j = 0; j < x.elements.size(); ++j) {
			if (!sameElement(x.elements.at(j), y.elements.at(j), severalLanes))
				return false;
		}
	}
	return true;
}

const DividerDesign *matchingDividerDesign(const Section &section)
{
	for (const DividerDesign &design : dividerDesigns()) {
		if (sameDividerFigure(section.divider, design.figure))
			return &design;
	}
	return nullptr;
}

} // namespace closingtime
