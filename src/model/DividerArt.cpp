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

#include "model/DividerArt.hpp"

#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

constexpr unsigned kPiece = DividerRolePiece;
constexpr unsigned kArm = DividerRoleArm;

/*
 * The built-in divider shapes, in the order the designer's pickers list them.
 *
 * A tile is drawn in a box one unit tall with the divider's midline along y = 0.5, so the
 * section's thickness scales all of it; `aspect` says how many of those units wide the tile is
 * and `height` how many thicknesses tall it is drawn. Adding a shape is a row here and nothing
 * else: the renderer measures, places, tiles and paints whatever the table hands it.
 *
 * An end cap is drawn as the *left* one, pointing outward along -x. The right-hand end is that
 * same tile mirrored, so a cap is authored once and cannot end up subtly different at the two
 * ends of the same rule.
 *
 * The art is white because the renderer uses it as a stencil for the section's own fill, which
 * is what lets a divider carry the same colour, gradient, outline and shadow as the titles
 * around it. Filigree is stroked rather than filled -- a curl has no interior worth naming --
 * and no stroked shape is a scaling arm, so nothing here is ever scaled unevenly enough to
 * thin a stroke on one axis.
 */
const DividerShapeInfo kDividerShapes[] = {
	{DividerShape::None, "none", "None", "", 0.0, 0.0, DividerStretch::Spread, kPiece},

	/* --- Ends ------------------------------------------------------------------------- */

	/*
	 * A swept head with the barbs notched back to the shaft, which is what reads as an arrow
	 * rather than as a triangle stuck on the end of a line.
	 */
	{DividerShape::Arrow, "arrow", "Arrowhead",
	 R"(<path d="M0 0.5 L1.15 0.03 L0.78 0.5 L1.15 0.97 Z" fill="#ffffff"/>)", 1.15, 4.5, DividerStretch::Spread,
	 kPiece},

	{DividerShape::Spear, "spear", "Spearpoint", R"(<path d="M0 0.5 Q1 0 2 0 L2 1 Q1 1 0 0.5 Z" fill="#ffffff"/>)",
	 2.0, 3.0, DividerStretch::Spread, kPiece},

	{DividerShape::Teardrop, "teardrop", "Teardrop",
	 R"(<path d="M0 0.5 Q0.7 0.02 1.2 0 A0.5 0.5 0 1 1 1.2 1 Q0.7 0.98 0 0.5 Z" fill="#ffffff"/>)", 1.7, 3.0,
	 DividerStretch::Spread, kPiece},

	/* Two curls off one stem, so the terminal is symmetric about the rule it caps. */
	{DividerShape::ScrollEnd, "scroll_end", "Scroll End",
	 R"(<g fill="none" stroke="#ffffff" stroke-width="0.1" stroke-linecap="round">)"
	 R"(<path d="M1.8 0.5 L0.9 0.5"/>)"
	 R"(<path d="M0.9 0.5 C0.4 0.5 0.1 0.35 0.1 0.2 C0.1 0.06 0.32 0.02 0.44 0.12 C0.54 0.2 0.5 0.34 0.34 0.36"/>)"
	 R"(<path d="M0.9 0.5 C0.4 0.5 0.1 0.65 0.1 0.8 C0.1 0.94 0.32 0.98 0.44 0.88 C0.54 0.8 0.5 0.66 0.34 0.64"/>)"
	 R"(</g>)",
	 1.8, 4.0, DividerStretch::Spread, kPiece},

	{DividerShape::DecoStep, "deco_step", "Deco Step",
	 R"(<g fill="#ffffff">)"
	 R"(<rect x="0" y="0.3" width="0.2" height="0.4"/>)"
	 R"(<rect x="0.4" y="0.15" width="0.2" height="0.7"/>)"
	 R"(<rect x="0.8" y="0" width="0.2" height="1"/>)"
	 R"(</g>)",
	 1.0, 3.0, DividerStretch::Spread, kPiece},

	{DividerShape::Chevron, "chevron", "Chevron",
	 R"(<path d="M0.62 0.06 L0 0.5 L0.62 0.94 L0.34 0.5 Z" fill="#ffffff"/>)", 0.62, 3.5, DividerStretch::Spread,
	 kPiece},

	/* --- Rules ------------------------------------------------------------------------ */

	{DividerShape::Rule, "rule", "Rule", R"(<rect x="0" y="0" width="8" height="1" fill="#ffffff"/>)", 8.0, 1.0,
	 DividerStretch::Scale, kArm},

	{DividerShape::DoubleRule, "double_rule", "Double Rule",
	 R"(<g fill="#ffffff">)"
	 R"(<rect x="0" y="0" width="8" height="0.3"/>)"
	 R"(<rect x="0" y="0.7" width="8" height="0.3"/>)"
	 R"(</g>)",
	 8.0, 1.0, DividerStretch::Scale, kArm},

	/*
	 * Thickest where it meets the centre and drawn down to a hairline at the cap, rather than
	 * to a true point: a zero-width end disappears into the antialiasing instead of tapering
	 * into it. Paired with a `None` cap this is the needle rule that has no ends at all.
	 */
	{DividerShape::Taper, "taper", "Tapered Rule", R"(<path d="M0 0.44 L8 0 L8 1 L0 0.56 Z" fill="#ffffff"/>)", 8.0,
	 1.0, DividerStretch::Scale, kArm},

	{DividerShape::Dots, "dots", "Dots", R"(<circle cx="1" cy="0.5" r="0.5" fill="#ffffff"/>)", 2.0, 1.0,
	 DividerStretch::Spread, kArm},

	{DividerShape::Dashes, "dashes", "Dashes",
	 R"(<rect x="0.5" y="0" width="2" height="1" rx="0.5" fill="#ffffff"/>)", 3.0, 1.0, DividerStretch::Spread,
	 kArm},

	{DividerShape::DiamondChain, "diamond_chain", "Diamond Chain",
	 R"(<path d="M1 0 L1.5 0.5 L1 1 L0.5 0.5 Z" fill="#ffffff"/>)", 2.0, 1.0, DividerStretch::Spread, kArm},

	/* The tick sits at the tile's leading edge, so a run of them is a rule ruled off at a pitch. */
	{DividerShape::DecoTicks, "deco_ticks", "Ticked Rule",
	 R"(<g fill="#ffffff">)"
	 R"(<rect x="0" y="0.4" width="4" height="0.2"/>)"
	 R"(<rect x="0" y="0" width="0.22" height="1"/>)"
	 R"(</g>)",
	 4.0, 1.0, DividerStretch::Spread, kArm},

	/* --- Centrepieces ----------------------------------------------------------------- */

	{DividerShape::Diamond, "diamond", "Diamond", R"(<path d="M1 0 L2 0.5 L1 1 L0 0.5 Z" fill="#ffffff"/>)", 2.0,
	 3.5, DividerStretch::Spread, kPiece},

	{DividerShape::NestedDiamond, "nested_diamond", "Nested Diamond",
	 R"(<path d="M1.3 0.07 L2.53 0.5 L1.3 0.93 L0.07 0.5 Z" fill="none" stroke="#ffffff" stroke-width="0.13"/>)"
	 R"(<path d="M1.3 0.3 L1.85 0.5 L1.3 0.7 L0.75 0.5 Z" fill="#ffffff"/>)",
	 2.6, 4.5, DividerStretch::Spread, kPiece},

	{DividerShape::Sparkle, "sparkle", "Four-Point Star",
	 R"(<path d="M0.65 0 C0.72 0.42 0.88 0.5 1.3 0.5 C0.88 0.5 0.72 0.58 0.65 1)"
	 R"( C0.58 0.58 0.42 0.5 0 0.5 C0.42 0.5 0.58 0.42 0.65 0 Z" fill="#ffffff"/>)",
	 1.3, 4.5, DividerStretch::Spread, kPiece},

	{DividerShape::Star, "star", "Five-Point Star",
	 R"(<path d="M0.5 0 L0.618 0.338 L0.976 0.346 L0.69 0.562 L0.794 0.905)"
	 R"( L0.5 0.7 L0.206 0.905 L0.31 0.562 L0.024 0.346 L0.382 0.338 Z" fill="#ffffff"/>)",
	 1.0, 5.0, DividerStretch::Spread, kPiece},

	{DividerShape::Dot, "dot", "Dot", R"(<circle cx="0.5" cy="0.5" r="0.5" fill="#ffffff"/>)", 1.0, 2.5,
	 DividerStretch::Spread, kPiece},

	{DividerShape::Heart, "heart", "Heart",
	 R"(<path d="M0.55 1 C0.18 0.72 0 0.5 0 0.3 C0 0.12 0.16 0.02 0.31 0.02 C0.42 0.02 0.5 0.08 0.55 0.16)"
	 R"( C0.6 0.08 0.68 0.02 0.79 0.02 C0.94 0.02 1.1 0.12 1.1 0.3 C1.1 0.5 0.92 0.72 0.55 1 Z" fill="#ffffff"/>)",
	 1.1, 3.5, DividerStretch::Spread, kPiece},

	/* An ogee hooked under at each end -- the flourish, without the diamond it usually frames. */
	{DividerShape::Scroll, "scroll", "Scroll",
	 R"(<g fill="none" stroke="#ffffff" stroke-width="0.09" stroke-linecap="round">)"
	 R"(<path d="M0.1 0.62 C0.1 0.86 0.46 0.9 0.6 0.7 C0.78 0.44 0.9 0.2 1.3 0.2)"
	 R"( C1.7 0.2 1.82 0.44 2.0 0.7 C2.14 0.9 2.5 0.86 2.5 0.62 C2.5 0.44 2.24 0.4 2.14 0.54"/>)"
	 R"(<path d="M0.1 0.62 C0.1 0.44 0.36 0.4 0.46 0.54"/>)"
	 R"(</g>)",
	 2.6, 3.5, DividerStretch::Spread, kPiece},

	/*
	 * Two mirrored curls with nothing between them, which is the point: whatever belongs in
	 * that space -- a diamond, a word, a monogram -- is another piece in the centre stack
	 * rather than a second copy of this shape with the middle filled in.
	 */
	{DividerShape::Filigree, "filigree", "Filigree",
	 R"(<g fill="none" stroke="#ffffff" stroke-width="0.09" stroke-linecap="round">)"
	 R"(<path d="M1.5 0.5 C1.2 0.5 1.05 0.28 0.78 0.28 C0.4 0.28 0.22 0.6 0.42 0.82 C0.6 1.0 0.92 0.9 0.9 0.62"/>)"
	 R"(<path d="M1.5 0.5 C1.8 0.5 1.95 0.28 2.22 0.28 C2.6 0.28 2.78 0.6 2.58 0.82 C2.4 1.0 2.08 0.9 2.1 0.62"/>)"
	 R"(</g>)",
	 3.0, 4.5, DividerStretch::Spread, kPiece},

	/*
	 * The two lozenges overlap by a little over a tenth of their width -- enough to read as
	 * interlocked, not enough to collapse into an X the way a third of a diamond's width does.
	 */
	{DividerShape::DecoStack, "deco_stack", "Deco Interlock",
	 R"(<g fill="none" stroke="#ffffff" stroke-width="0.11">)"
	 R"(<path d="M0.95 0.08 L1.75 0.5 L0.95 0.92 L0.15 0.5 Z"/>)"
	 R"(<path d="M2.25 0.08 L3.05 0.5 L2.25 0.92 L1.45 0.5 Z"/>)"
	 R"(</g>)"
	 R"(<path d="M1.6 0.31 L1.94 0.5 L1.6 0.69 L1.26 0.5 Z" fill="#ffffff"/>)",
	 3.2, 4.5, DividerStretch::Spread, kPiece},

	/*
	 * Custom art is measured from the file's own viewBox rather than from `aspect`, and it
	 * spreads rather than scales in an arm: stretching artwork nobody here drew is far more
	 * likely to ruin it than to fill the span nicely. Its `height` is a guess at what a cap or
	 * a centrepiece usually is -- a file declares no proportion to the rule anywhere -- and the
	 * piece's own size multiplier is what corrects it. In an arm the number is not read at all:
	 * an arm is the rule, and is drawn at exactly the thickness.
	 */
	{DividerShape::Custom, "custom", "Custom SVG", "", 1.0, 4.0, DividerStretch::Spread, kPiece | kArm},
};

} // namespace

const DividerShapeInfo &dividerShapeInfo(DividerShape shape)
{
	for (const auto &info : kDividerShapes) {
		if (info.shape == shape)
			return info;
	}
	return kDividerShapes[0];
}

const char *dividerShapeId(DividerShape shape)
{
	return dividerShapeInfo(shape).id;
}

DividerShape dividerShapeFromId(const char *id, DividerShape fallback)
{
	if (!id)
		return fallback;

	for (const auto &info : kDividerShapes) {
		if (strcmp(info.id, id) == 0)
			return info.shape;
	}
	return fallback;
}

const char *dividerShapeName(DividerShape shape)
{
	return dividerShapeInfo(shape).name;
}

const QVector<DividerShape> &dividerShapesForRole(DividerRole role)
{
	const auto build = [](unsigned wanted) {
		QVector<DividerShape> result;
		for (const auto &info : kDividerShapes) {
			if (info.roles & wanted)
				result.append(info.shape);
		}
		return result;
	};

	static const QVector<DividerShape> pieces = build(DividerRolePiece);
	static const QVector<DividerShape> arms = build(DividerRoleArm);

	return role == DividerRoleArm ? arms : pieces;
}

bool dividerShapeHasRole(DividerShape shape, DividerRole role)
{
	return (dividerShapeInfo(shape).roles & role) != 0;
}

bool dividerShapeIsEmpty(DividerShape shape)
{
	return shape == DividerShape::None;
}

bool dividerShapeUsesFile(DividerShape shape)
{
	return shape == DividerShape::Custom;
}

QString dividerShapeSvg(DividerShape shape)
{
	const DividerShapeInfo &info = dividerShapeInfo(shape);
	if (!info.svg || info.svg[0] == '\0' || info.aspect <= 0.0)
		return QString();

	/*
	 * preserveAspectRatio is off so a scaling arm really does stretch along x instead of being
	 * letterboxed back to its own proportions. Every other shape is rendered into a rectangle
	 * of the tile's own aspect, so it never notices -- which is also what keeps the stroked
	 * shapes' curls from being drawn thinner one way than the other.
	 */
	return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 1\" "
			      "preserveAspectRatio=\"none\">%2</svg>")
		.arg(QString::number(info.aspect, 'g', 6), QString::fromLatin1(info.svg));
}

} // namespace closingtime
