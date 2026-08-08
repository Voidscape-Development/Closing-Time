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

#include "model/BridgeArt.hpp"

#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

/*
 * The built-in bridges, in the order the designer lists them.
 *
 * A tile is drawn in a box one unit tall, so the section's thickness in pixels scales all of
 * it, and `aspect` says how many of those units wide the tile is. Adding a bridge is a row
 * here and nothing else: the renderer measures, tiles, spreads and paints whatever the table
 * hands it.
 *
 * The art is white because the renderer uses it as a stencil for the section's own fill, which
 * is what keeps a leader picking up the same colour, gradient, outline and shadow as the text
 * either side of it.
 */
const BridgeTypeInfo kBridgeTypes[] = {
	{BridgeType::Text, "text", "Text", "", 0.0, BridgeStretch::Spread},

	{BridgeType::Dots, "dots", "Dots", R"(<circle cx="1" cy="0.5" r="0.5" fill="#ffffff"/>)", 2.0,
	 BridgeStretch::Spread},

	/* Centred in its tile, like the dot, so a run of them is symmetric at both ends. */
	{BridgeType::Dashes, "dashes", "Dashes",
	 R"(<rect x="0.5" y="0" width="2" height="1" rx="0.5" fill="#ffffff"/>)", 3.0, BridgeStretch::Spread},

	/*
	 * A rule is continuous, so it scales rather than tiles: tiling would leave it short of
	 * the gap by up to a whole tile, which on an unbroken line reads as damage.
	 */
	{BridgeType::Line, "line", "Line", R"(<rect x="0" y="0" width="8" height="1" fill="#ffffff"/>)", 8.0,
	 BridgeStretch::Scale},

	{BridgeType::DoubleLine, "double_line", "Double Line",
	 R"(<rect x="0" y="0" width="8" height="0.3" fill="#ffffff"/>)"
	 R"(<rect x="0" y="0.7" width="8" height="0.3" fill="#ffffff"/>)",
	 8.0, BridgeStretch::Scale},

	{BridgeType::Diamonds, "diamonds", "Diamonds", R"(<path d="M1 0 L1.5 0.5 L1 1 L0.5 0.5 Z" fill="#ffffff"/>)",
	 2.0, BridgeStretch::Spread},

	/*
	 * Custom art is measured from the file's own viewBox rather than from `aspect`, and it
	 * spreads rather than scales: stretching artwork nobody here drew is far more likely to
	 * ruin it than to fill the gap nicely.
	 */
	{BridgeType::Custom, "custom", "Custom SVG", "", 1.0, BridgeStretch::Spread},
};

} // namespace

const BridgeTypeInfo &bridgeTypeInfo(BridgeType type)
{
	for (const auto &info : kBridgeTypes) {
		if (info.type == type)
			return info;
	}
	return kBridgeTypes[0];
}

const char *bridgeTypeId(BridgeType type)
{
	return bridgeTypeInfo(type).id;
}

BridgeType bridgeTypeFromId(const char *id, BridgeType fallback)
{
	if (!id)
		return fallback;

	for (const auto &info : kBridgeTypes) {
		if (strcmp(info.id, id) == 0)
			return info.type;
	}
	return fallback;
}

const char *bridgeTypeName(BridgeType type)
{
	return bridgeTypeInfo(type).name;
}

const QVector<BridgeType> &allBridgeTypes()
{
	static const QVector<BridgeType> types = [] {
		QVector<BridgeType> result;
		result.reserve(static_cast<int>(std::size(kBridgeTypes)));
		for (const auto &info : kBridgeTypes)
			result.append(info.type);
		return result;
	}();
	return types;
}

bool bridgeTypeUsesArt(BridgeType type)
{
	return type != BridgeType::Text;
}

bool bridgeTypeUsesFile(BridgeType type)
{
	return type == BridgeType::Custom;
}

QString bridgeTypeSvg(BridgeType type)
{
	const BridgeTypeInfo &info = bridgeTypeInfo(type);
	if (!info.svg || info.svg[0] == '\0' || info.aspect <= 0.0)
		return QString();

	/*
	 * preserveAspectRatio is off so a scaling type really does stretch along x instead of
	 * being letterboxed back to its own proportions. A spreading type is only ever rendered
	 * into a rectangle of the tile's own aspect, so it never notices.
	 */
	return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 1\" "
			      "preserveAspectRatio=\"none\">%2</svg>")
		.arg(QString::number(info.aspect, 'g', 6), QString::fromLatin1(info.svg));
}

} // namespace closingtime
