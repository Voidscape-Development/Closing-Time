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

DividerPiece ornament(DividerShape shape, double scale = 1.0, double rotation = 0.0)
{
	DividerPiece piece;
	piece.kind = DividerPiece::Kind::Ornament;
	piece.shape = shape;
	piece.scale = scale;
	piece.rotation = rotation;
	return piece;
}

DividerPiece word(const char *text)
{
	DividerPiece piece;
	piece.kind = DividerPiece::Kind::Text;
	piece.text = QString::fromUtf8(text);
	return piece;
}

/*
 * Built from a function rather than written as one aggregate, because each design names only the
 * handful of fields that make it what it is and leaves the rest at the plain divider's values --
 * an aggregate would have to spell out every field of every row, in order, to set the last one.
 */
QVector<DividerDesign> buildDesigns()
{
	QVector<DividerDesign> designs;

	const auto add = [&designs](const char *id, const char *name) -> DividerDesign & {
		DividerDesign design;
		design.id = id;
		design.name = name;
		designs.append(design);
		return designs.last();
	};

	/* The one a new divider starts as: see Section::makeDefault. */
	{
		DividerDesign &d = add("arrow_rule", "Arrow Rule");
		d.ends = {ornament(DividerShape::Arrow)};
		d.thickness = 5.0;
		d.center = {ornament(DividerShape::Dot), ornament(DividerShape::Diamond), ornament(DividerShape::Dot)};
	}
	{
		DividerDesign &d = add("plain_rule", "Plain Rule");
		d.thickness = 3.0;
	}
	/* Two tapers thick at a diamond and drawn down to hairlines at the ends: no caps at all. */
	{
		DividerDesign &d = add("needle", "Needle");
		d.arm = DividerShape::Taper;
		d.thickness = 5.0;
		d.gap = 8.0;
		d.center = {ornament(DividerShape::Diamond, 0.8)};
	}
	/* A hairline the ornament sits *on*, which is what joining is for. */
	{
		DividerDesign &d = add("diamond_line", "Diamond on the Line");
		d.ends = {ornament(DividerShape::Dot, 1.6)};
		d.thickness = 2.0;
		d.connect = true;
		d.center = {ornament(DividerShape::NestedDiamond, 2.6)};
	}
	{
		DividerDesign &d = add("deco_triple", "Deco Triple");
		d.ends = {ornament(DividerShape::DecoStep, 2.0)};
		d.thickness = 3.0;
		d.gap = 10.0;
		d.center = {ornament(DividerShape::DecoStack, 2.0)};
		d.rules = 3;
		d.ruleGap = 5.0;
		d.ruleInset = 24.0;
	}
	{
		DividerDesign &d = add("double_star", "Double Rule & Star");
		d.arm = DividerShape::DoubleRule;
		d.thickness = 6.0;
		d.gap = 14.0;
		d.center = {ornament(DividerShape::Star)};
	}
	{
		DividerDesign &d = add("dotted", "Dotted Line");
		d.arm = DividerShape::Dots;
		d.thickness = 5.0;
		d.gap = 14.0;
		d.center = {ornament(DividerShape::Sparkle, 1.6)};
	}
	{
		DividerDesign &d = add("scrollwork", "Scrollwork");
		d.ends = {ornament(DividerShape::ScrollEnd, 2.0)};
		d.thickness = 3.0;
		d.gap = 10.0;
		d.center = {ornament(DividerShape::Scroll, 2.5)};
	}
	{
		DividerDesign &d = add("filigree", "Filigree");
		d.arm = DividerShape::Taper;
		d.thickness = 4.0;
		d.gap = 6.0;
		d.center = {ornament(DividerShape::Filigree, 2.0)};
	}
	{
		DividerDesign &d = add("spearpoints", "Spearpoints");
		d.ends = {ornament(DividerShape::Spear, 1.5)};
		d.thickness = 4.0;
		d.connect = true;
		d.center = {ornament(DividerShape::Sparkle, 2.0)};
	}
	{
		DividerDesign &d = add("hearts", "Hearts");
		d.arm = DividerShape::Dashes;
		d.thickness = 4.0;
		d.center = {ornament(DividerShape::Heart), ornament(DividerShape::Heart, 1.5),
			    ornament(DividerShape::Heart)};
		d.pieceGap = 6.0;
	}
	{
		DividerDesign &d = add("chain", "Diamond Chain");
		d.ends = {ornament(DividerShape::Chevron)};
		d.arm = DividerShape::DiamondChain;
		d.thickness = 6.0;
		d.gap = 10.0;
		d.center = {ornament(DividerShape::NestedDiamond)};
	}
	{
		DividerDesign &d = add("ticked", "Ticked Rule");
		d.ends = {ornament(DividerShape::Chevron)};
		d.arm = DividerShape::DecoTicks;
		d.thickness = 8.0;
		d.gap = 8.0;
	}
	{
		DividerDesign &d = add("geometric", "Geometric");
		d.ends = {ornament(DividerShape::Circle, 1.4)};
		d.thickness = 3.0;
		d.connect = true;
		d.center = {ornament(DividerShape::Ring, 1.6), ornament(DividerShape::Hexagon, 2.6),
			    ornament(DividerShape::Ring, 1.6)};
		d.pieceGap = 8.0;
	}
	/* A word breaking the rule, set in the section's own style. */
	{
		DividerDesign &d = add("labeled", "Labeled Rule");
		d.ends = {ornament(DividerShape::Dot, 1.6)};
		d.thickness = 2.0;
		d.gap = 16.0;
		d.pieceGap = 16.0;
		d.center = {word("INTERMISSION")};
	}
	/* The label leading the rule rather than breaking it -- what the center position is for. */
	{
		DividerDesign &d = add("heading_rule", "Heading Rule");
		d.thickness = 2.0;
		d.gap = 20.0;
		d.center = {word("CHAPTER ONE")};
		d.centerPosition = 0.0;
	}

	return designs;
}

bool near(double a, double b)
{
	return std::abs(a - b) < 1e-6;
}

bool samePiece(const DividerPiece &a, const DividerPiece &b)
{
	if (a.kind != b.kind || !near(a.rotation, b.rotation))
		return false;

	/*
	 * Only what the piece's kind actually draws from: a word keeps the shape its row had before it
	 * became a word, and that leftover is not part of the figure.
	 */
	switch (a.kind) {
	case DividerPiece::Kind::Ornament:
		return a.shape == b.shape && near(a.scale, b.scale) &&
		       (!dividerShapeUsesFile(a.shape) || a.svgPath == b.svgPath);
	case DividerPiece::Kind::Text:
		return a.text == b.text;
	case DividerPiece::Kind::Logo:
		return a.logo == b.logo;
	}
	return false;
}

bool sameStack(const QVector<DividerPiece> &a, const QVector<DividerPiece> &b)
{
	if (a.size() != b.size())
		return false;
	for (int i = 0; i < a.size(); ++i) {
		if (!samePiece(a.at(i), b.at(i)))
			return false;
	}
	return true;
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
	section.dividerCap = design.ends;
	section.dividerMirrorEnds = true;
	section.dividerArm = design.arm;
	section.dividerThickness = design.thickness;
	section.dividerConnect = design.connect;
	section.dividerGap = design.gap;
	section.dividerPieceGap = design.pieceGap;
	section.dividerCenter = design.center;
	section.dividerRules = design.rules;
	section.dividerRuleGap = design.ruleGap;
	section.dividerRuleInset = design.ruleInset;
	section.dividerCenterPosition = design.centerPosition;
}

const DividerDesign *matchingDividerDesign(const Section &section)
{
	for (const DividerDesign &design : dividerDesigns()) {
		/*
		 * A setting the figure does not consult is not compared: the end gap means nothing while
		 * the rule runs through, nor the rule spacing with only one rule, nor the position of an
		 * empty center. Comparing them would call a divider "changed" over a value it cannot show.
		 */
		const bool hasCenter = !design.center.isEmpty();

		if (section.dividerMirrorEnds && section.dividerArm == design.arm &&
		    near(section.dividerThickness, design.thickness) && section.dividerConnect == design.connect &&
		    (design.connect || near(section.dividerGap, design.gap)) &&
		    near(section.dividerPieceGap, design.pieceGap) && section.dividerRules == design.rules &&
		    (design.rules == 1 || (near(section.dividerRuleGap, design.ruleGap) &&
					   near(section.dividerRuleInset, design.ruleInset))) &&
		    (!hasCenter || near(section.dividerCenterPosition, design.centerPosition)) &&
		    sameStack(section.dividerCap, design.ends) && sameStack(section.dividerCenter, design.center))
			return &design;
	}
	return nullptr;
}

} // namespace closingtime
