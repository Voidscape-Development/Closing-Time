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

/*
 * The Section Divider's own model: its elements, its lanes, how a symmetric divider is reflected,
 * and how a document written with the old fixed slots becomes one.
 */

#include "model/CreditsModel.hpp"

#include <obs.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

struct ElementKindInfo {
	DividerElement::Kind kind;
	const char *id;
};

constexpr ElementKindInfo kElementKinds[] = {
	{DividerElement::Kind::Shape, "shape"},
	{DividerElement::Kind::Text, "text"},
	{DividerElement::Kind::Image, "image"},
	{DividerElement::Kind::Line, "line"},
};

double finiteOr(double value, double fallback)
{
	return std::isfinite(value) ? value : fallback;
}

/* A stored number, or `fallback` when the key is missing -- told apart from a stored zero. */
double numberOr(obs_data_t *data, const char *key, double fallback)
{
	return obs_data_has_user_value(data, key) ? finiteOr(obs_data_get_double(data, key), fallback) : fallback;
}

bool boolOr(obs_data_t *data, const char *key, bool fallback)
{
	return obs_data_has_user_value(data, key) ? obs_data_get_bool(data, key) : fallback;
}

template<typename T> void saveList(obs_data_t *parent, const char *key, const QVector<T> &items)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const T &item : items) {
		OBSDataAutoRelease data = obs_data_create();
		item.save(data);
		obs_data_array_push_back(array, data);
	}
	obs_data_set_array(parent, key, array);
}

template<typename T> QVector<T> loadList(obs_data_t *parent, const char *key, int limit)
{
	QVector<T> items;
	OBSDataArrayAutoRelease array = obs_data_get_array(parent, key);
	if (!array)
		return items;

	const size_t count = std::min<size_t>(obs_data_array_count(array), static_cast<size_t>(limit));
	items.reserve(static_cast<int>(count));
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease data = obs_data_array_item(array, i);
		T item;
		item.load(data);
		items.append(item);
	}
	return items;
}

/* ---- The old shape, for documents written before lanes -------------------------------------- */

/* One piece of an old end or center stack, read straight into the element it now is. */
DividerElement legacyPiece(obs_data_t *data)
{
	DividerElement element;

	const char *kind = obs_data_get_string(data, "kind");
	if (kind && std::strcmp(kind, "text") == 0)
		element.kind = DividerElement::Kind::Text;
	else if (kind && std::strcmp(kind, "logo") == 0)
		element.kind = DividerElement::Kind::Image;
	else
		element.kind = DividerElement::Kind::Shape;

	element.shape = dividerShapeFromId(obs_data_get_string(data, "shape"), DividerShape::Diamond);
	element.svgPath = QString::fromUtf8(obs_data_get_string(data, "svg"));
	element.scale = obs_data_get_double(data, "scale");
	if (!(element.scale > 0.0))
		element.scale = 1.0;
	element.rotation = finiteOr(obs_data_get_double(data, "rotation"), 0.0);
	element.text = QString::fromUtf8(obs_data_get_string(data, "text"));

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		element.logo.load(logoData);

	return element;
}

QVector<DividerElement> legacyStack(obs_data_t *data, const char *key, bool *present = nullptr)
{
	QVector<DividerElement> pieces;
	OBSDataArrayAutoRelease array = obs_data_get_array(data, key);
	if (present)
		*present = array != nullptr;
	if (!array)
		return pieces;

	const size_t count = std::min<size_t>(obs_data_array_count(array), kMaxDividerElements);
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		pieces.append(legacyPiece(item));
	}
	return pieces;
}

/*
 * An end, from whichever of its two old shapes the document holds: the stack of pieces, or -- from
 * before an end was a stack -- a single shape, where None meant an end with nothing on it.
 */
QVector<DividerElement> legacyEnd(obs_data_t *data, const char *key, const char *shapeKey, const char *fileKey)
{
	bool present = false;
	QVector<DividerElement> pieces = legacyStack(data, key, &present);
	if (present)
		return pieces;

	const DividerShape shape = dividerShapeFromId(obs_data_get_string(data, shapeKey), DividerShape::None);
	if (dividerShapeIsEmpty(shape))
		return pieces;

	DividerElement piece;
	piece.kind = DividerElement::Kind::Shape;
	piece.shape = shape;
	piece.svgPath = QString::fromUtf8(obs_data_get_string(data, fileKey));
	pieces.append(piece);
	return pieces;
}

} // namespace

namespace {

struct RevealInfo {
	DividerReveal reveal;
	const char *id;
};

constexpr RevealInfo kReveals[] = {
	{DividerReveal::None, "none"},
	{DividerReveal::FromCenter, "center"},
	{DividerReveal::FromLeft, "left"},
	{DividerReveal::FromRight, "right"},
};

} // namespace

const char *dividerRevealId(DividerReveal reveal)
{
	for (const auto &info : kReveals) {
		if (info.reveal == reveal)
			return info.id;
	}
	return kReveals[0].id;
}

DividerReveal dividerRevealFromId(const char *id, DividerReveal fallback)
{
	if (!id)
		return fallback;
	for (const auto &info : kReveals) {
		if (std::strcmp(info.id, id) == 0)
			return info.reveal;
	}
	return fallback;
}

const char *dividerElementKindId(DividerElement::Kind kind)
{
	for (const auto &info : kElementKinds) {
		if (info.kind == kind)
			return info.id;
	}
	return kElementKinds[0].id;
}

DividerElement::Kind dividerElementKindFromId(const char *id, DividerElement::Kind fallback)
{
	if (!id)
		return fallback;
	for (const auto &info : kElementKinds) {
		if (std::strcmp(info.id, id) == 0)
			return info.kind;
	}
	return fallback;
}

/* ---- DividerElement ------------------------------------------------------------------------- */

void DividerElement::save(obs_data_t *data) const
{
	obs_data_set_string(data, "kind", dividerElementKindId(kind));
	obs_data_set_string(data, "shape", dividerShapeId(shape));
	obs_data_set_string(data, "svg", svgPath.toUtf8().constData());
	obs_data_set_string(data, "line_shape", dividerShapeId(lineShape));
	obs_data_set_string(data, "line_svg", lineSvgPath.toUtf8().constData());
	obs_data_set_string(data, "text", text.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);

	obs_data_set_double(data, "scale", scale);
	obs_data_set_double(data, "rotation", rotation);
	obs_data_set_double(data, "offset_y", offsetY);
	obs_data_set_bool(data, "flip", flip);
	obs_data_set_double(data, "weight", weight);
	obs_data_set_double(data, "min_length", minLength);
	obs_data_set_bool(data, "join", join);
	obs_data_set_bool(data, "span_lanes", spanLanes);
	obs_data_set_bool(data, "optional", optional);
	obs_data_set_bool(data, "own_color", ownColor);
	obs_data_set_int(data, "color", static_cast<long long>(color.rgba()));
}

void DividerElement::load(obs_data_t *data)
{
	*this = DividerElement{};

	kind = dividerElementKindFromId(obs_data_get_string(data, "kind"), Kind::Shape);
	shape = dividerShapeFromId(obs_data_get_string(data, "shape"), DividerShape::Diamond);
	svgPath = QString::fromUtf8(obs_data_get_string(data, "svg"));
	/*
	 * A line may be empty space, so None is a real answer here rather than a missing one -- which
	 * is why a missing key is told apart from it and falls back to the plain rule.
	 */
	lineShape = obs_data_has_user_value(data, "line_shape")
			    ? dividerShapeFromId(obs_data_get_string(data, "line_shape"), DividerShape::Rule)
			    : DividerShape::Rule;
	if (!dividerShapeIsEmpty(lineShape) && !dividerShapeHasRole(lineShape, DividerRoleArm))
		lineShape = DividerShape::Rule;
	lineSvgPath = QString::fromUtf8(obs_data_get_string(data, "line_svg"));
	text = QString::fromUtf8(obs_data_get_string(data, "text"));

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		logo.load(logoData);

	scale = numberOr(data, "scale", 1.0);
	if (!(scale > 0.0))
		scale = 1.0;
	scale = std::min(scale, 64.0);
	rotation = std::fmod(numberOr(data, "rotation", 0.0), 360.0);
	offsetY = std::clamp(numberOr(data, "offset_y", 0.0), -4096.0, 4096.0);
	flip = obs_data_get_bool(data, "flip");
	weight = std::clamp(numberOr(data, "weight", 1.0), 0.0, 1000.0);
	minLength = std::clamp(numberOr(data, "min_length", 0.0), 0.0, 16384.0);
	join = obs_data_get_bool(data, "join");
	spanLanes = obs_data_get_bool(data, "span_lanes");
	optional = obs_data_get_bool(data, "optional");
	ownColor = obs_data_get_bool(data, "own_color");
	if (obs_data_has_user_value(data, "color"))
		color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(data, "color")));
}

/* ---- DividerLane ---------------------------------------------------------------------------- */

void DividerLane::save(obs_data_t *data) const
{
	saveList(data, "elements", elements);
	obs_data_set_double(data, "thickness", thickness);
	obs_data_set_double(data, "inset", inset);
	obs_data_set_double(data, "offset_y", offsetY);
	obs_data_set_bool(data, "center_piece", centerPiece);
}

void DividerLane::load(obs_data_t *data)
{
	elements = loadList<DividerElement>(data, "elements", kMaxDividerElements);
	/* Zero thickness would draw nothing at all, which no stored lane ever means. */
	thickness = numberOr(data, "thickness", 4.0);
	if (!(thickness > 0.0))
		thickness = 4.0;
	thickness = std::min(thickness, 1024.0);
	inset = std::clamp(numberOr(data, "inset", 0.0), 0.0, 16384.0);
	offsetY = std::clamp(numberOr(data, "offset_y", 0.0), -4096.0, 4096.0);
	centerPiece = boolOr(data, "center_piece", true);
}

/* ---- Divider -------------------------------------------------------------------------------- */

QVector<DividerElement> Divider::drawnRow(const DividerLane &lane) const
{
	if (!symmetric)
		return lane.elements;

	QVector<DividerElement> row = lane.elements;
	if (row.isEmpty())
		return row;

	/*
	 * The reflection: the same list run back the other way, each element flipped. A piece written
	 * last can sit on the center line instead, drawn once -- see DividerLane::centerPiece -- and is
	 * then the one element the reflection leaves out.
	 */
	const bool centered = lane.centerPiece && !lane.elements.last().isLine();
	const int mirrored = centered ? lane.elements.size() - 1 : lane.elements.size();

	row.reserve(lane.elements.size() + mirrored);
	for (int i = mirrored - 1; i >= 0; --i) {
		DividerElement copy = lane.elements.at(i);
		copy.flip = !copy.flip;
		row.append(copy);
	}
	return row;
}

bool Divider::hasText() const
{
	return !firstText().isEmpty();
}

QString Divider::firstText() const
{
	for (const DividerLane &lane : lanes) {
		for (const DividerElement &element : lane.elements) {
			if (element.kind == DividerElement::Kind::Text && !element.text.isEmpty())
				return element.text;
		}
	}
	return QString();
}

QStringList Divider::imagePaths() const
{
	QStringList paths;
	for (const DividerLane &lane : lanes) {
		for (const DividerElement &element : lane.elements) {
			if (element.kind == DividerElement::Kind::Image && !element.logo.isEmpty())
				paths.append(element.logo.path);
		}
	}
	return paths;
}

void Divider::save(obs_data_t *data) const
{
	saveList(data, "lanes", lanes);
	obs_data_set_bool(data, "symmetric", symmetric);
	obs_data_set_double(data, "lane_gap", laneGap);
	obs_data_set_double(data, "piece_gap", pieceGap);
	obs_data_set_double(data, "line_gap", lineGap);
	obs_data_set_double(data, "length", length);
	obs_data_set_string(data, "align", hAlignId(align));
	obs_data_set_bool(data, "tint", tint);
	obs_data_set_string(data, "reveal", dividerRevealId(reveal));
	obs_data_set_double(data, "reveal_start", revealStart);
	obs_data_set_double(data, "reveal_length", revealLength);
}

void Divider::load(obs_data_t *data)
{
	*this = Divider{};

	lanes = loadList<DividerLane>(data, "lanes", kMaxDividerLanes);
	/* A divider always has a lane to put something in, even one a hand-written file left empty. */
	if (lanes.isEmpty())
		lanes.append(DividerLane{});

	symmetric = boolOr(data, "symmetric", true);
	laneGap = std::clamp(numberOr(data, "lane_gap", 6.0), 0.0, 4096.0);
	/*
	 * Both may be negative -- that is a join rather than a gap -- and are bounded on that side at
	 * the figure the designer offers, so a file cannot ask for an overlap no window could type.
	 */
	pieceGap = std::clamp(numberOr(data, "piece_gap", 10.0), -kMaxDividerJoin, 4096.0);
	lineGap = std::clamp(numberOr(data, "line_gap", 12.0), -kMaxDividerJoin, 4096.0);
	length = std::clamp(numberOr(data, "length", 1.0), 0.01, 1.0);
	align = hAlignFromId(obs_data_get_string(data, "align"), HAlign::Center);
	tint = boolOr(data, "tint", true);
	reveal = dividerRevealFromId(obs_data_get_string(data, "reveal"), DividerReveal::None);
	/* A start above the top edge would never be reached, and one far below it is already passed. */
	revealStart = std::clamp(numberOr(data, "reveal_start", 0.95), 0.0, 1.5);
	/* Zero is a cut: the divider appears whole the moment it crosses the start line. */
	revealLength = std::clamp(numberOr(data, "reveal_length", 0.2), 0.0, 2.0);
}

Divider plainDivider()
{
	Divider divider;
	DividerElement line;
	line.kind = DividerElement::Kind::Line;
	divider.lanes.first().elements = {line};
	return divider;
}

Divider Divider::fromLegacy(obs_data_t *data)
{
	/*
	 * Read with exactly the fallbacks the old loader used, so a document that never carried a key
	 * becomes the divider it was always drawn as: no ends, a plain rule, an empty middle.
	 */
	const bool mirrorEnds = boolOr(data, "divider_mirror_ends", true);
	const DividerShape arm = dividerShapeFromId(obs_data_get_string(data, "divider_arm"), DividerShape::Rule);
	const QString armSvg = QString::fromUtf8(obs_data_get_string(data, "divider_arm_svg"));
	double thickness = obs_data_get_double(data, "divider_thickness");
	if (!(thickness > 0.0))
		thickness = 4.0;
	const double gap = numberOr(data, "divider_gap", 12.0);
	const double pieceGap = numberOr(data, "divider_piece_gap", 10.0);
	const bool connect = obs_data_get_bool(data, "divider_connect");
	const double ruleGap = std::max(0.0, numberOr(data, "divider_rule_gap", 6.0));
	const int rules = std::clamp(static_cast<int>(obs_data_get_int(data, "divider_rules")), 1, kMaxDividerLanes);
	const double ruleInset = std::max(0.0, finiteOr(obs_data_get_double(data, "divider_rule_inset"), 0.0));
	const double position = std::clamp(numberOr(data, "divider_center_position", 0.5), 0.0, 1.0);

	/* British-spelled, as it was always written. */
	const QVector<DividerElement> center = legacyStack(data, "divider_centre");
	const QVector<DividerElement> leftEnd = legacyEnd(data, "divider_cap_pieces", "divider_cap", "divider_cap_svg");
	const QVector<DividerElement> rightEnd =
		mirrorEnds ? leftEnd
			   : legacyEnd(data, "divider_end_cap_pieces", "divider_end_cap", "divider_end_cap_svg");

	Divider divider;
	divider.laneGap = ruleGap;
	divider.pieceGap = std::max(-kMaxDividerJoin, pieceGap);
	divider.lineGap = std::max(-kMaxDividerJoin, gap);
	divider.tint = boolOr(data, "divider_tint", true);

	DividerElement line;
	line.kind = DividerElement::Kind::Line;
	line.lineShape = dividerShapeHasRole(arm, DividerRoleArm) ? arm : DividerShape::Rule;
	line.lineSvgPath = armSvg;
	/*
	 * Joined, the old rule ran under the ends and the middle. A line that joins does exactly that
	 * to the pieces either side of it.
	 */
	line.join = connect;

	/*
	 * Symmetric whenever the old divider was: mirrored ends, a middle that sat in the middle, and a
	 * middle that reads the same both ways. The left half is the end, the rule, and the first half
	 * of the middle, with an odd middle's own center piece set on the axis.
	 */
	const bool palindrome = std::equal(center.cbegin(), center.cbegin() + center.size() / 2, center.crbegin());
	const bool symmetric = mirrorEnds && position == 0.5 && palindrome;

	QVector<DividerElement> row;
	bool centerPiece = true;

	if (symmetric) {
		row = leftEnd;
		row.append(line);
		const int half = static_cast<int>(center.size() / 2);
		for (int i = 0; i < half; ++i)
			row.append(center.at(i));
		if (center.size() % 2 == 1)
			row.append(center.at(half));
		else
			centerPiece = false;
	} else {
		row = leftEnd;
		if (center.isEmpty()) {
			row.append(line);
		} else {
			/*
			 * The middle's position becomes the weights of the lines either side of it. A line
			 * that would get nothing next to an end with nothing on it is left out entirely, so
			 * the middle sits against the edge as it used to rather than a gap away from it.
			 */
			DividerElement before = line;
			before.weight = position;
			DividerElement after = line;
			after.weight = 1.0 - position;

			if (!(position == 0.0 && leftEnd.isEmpty()))
				row.append(before);
			row.append(center);
			if (!(position == 1.0 && rightEnd.isEmpty()))
				row.append(after);
		}
		/* The old right-hand end was its list drawn reversed and flipped, outermost piece last. */
		for (auto it = rightEnd.crbegin(); it != rightEnd.crend(); ++it) {
			DividerElement piece = *it;
			piece.flip = !piece.flip;
			row.append(piece);
		}
	}

	divider.symmetric = symmetric;

	/*
	 * One rule is one lane. Several become lanes stacked about the old midline: the middle one
	 * carries the ends and the ornaments -- set to break every other lane, which is what the old
	 * stack did -- and the rest carry the rule alone, each inset by its distance from the middle.
	 * With an even count there is no middle lane, so the ornaments ride on the one just above the
	 * middle and are nudged down onto the midline.
	 */
	divider.lanes.clear();
	const int main = (rules - 1) / 2;
	const double pitch = thickness + ruleGap;
	const double nudge = rules % 2 == 0 ? pitch / 2.0 : 0.0;

	for (int lane = 0; lane < rules; ++lane) {
		DividerLane built;
		built.thickness = thickness;
		built.centerPiece = centerPiece;
		built.inset = std::abs(lane - (rules - 1) / 2.0) * ruleInset;

		if (lane == main) {
			built.elements = row;
			for (DividerElement &element : built.elements) {
				if (element.isLine())
					continue;
				element.offsetY += nudge;
				element.spanLanes = rules > 1 && !connect;
			}
		} else {
			built.elements = {line};
		}
		divider.lanes.append(built);
	}

	return divider;
}

} // namespace closingtime
