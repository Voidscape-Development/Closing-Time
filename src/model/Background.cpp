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

#include "model/Background.hpp"

#include <obs.hpp>

#include <algorithm>
#include <cstring>

namespace closingtime {

namespace {

struct BackgroundFillInfo {
	BackgroundFill fill;
	const char *id;
	const char *name;
};

const BackgroundFillInfo kFills[] = {
	{BackgroundFill::None, "none", "None"},
	{BackgroundFill::Color, "color", "Colour"},
	{BackgroundFill::LinearGradient, "linear_gradient", "Linear Gradient"},
	{BackgroundFill::RadialGradient, "radial_gradient", "Radial Gradient"},
	{BackgroundFill::Image, "image", "Image"},
};

struct BackgroundImageFitInfo {
	BackgroundImageFit fit;
	const char *id;
	const char *name;
};

const BackgroundImageFitInfo kFits[] = {
	{BackgroundImageFit::Cover, "cover", "Cover"},
	{BackgroundImageFit::Contain, "contain", "Contain"},
	{BackgroundImageFit::Stretch, "stretch", "Stretch"},
	{BackgroundImageFit::Tile, "tile", "Tile"},
};

struct BackgroundSlotInfo {
	BackgroundSlot slot;
	const char *id;
	const char *name;
};

const BackgroundSlotInfo kSlots[] = {
	{BackgroundSlot::Section, "section", "Section"},    {BackgroundSlot::Title, "title", "Title"},
	{BackgroundSlot::Subtitle, "subtitle", "Subtitle"}, {BackgroundSlot::Logo, "logo", "Logo"},
	{BackgroundSlot::Entry, "entry", "Entry"},          {BackgroundSlot::EntryAlt, "entry_alt", "Alternate Entry"},
	{BackgroundSlot::Bridge, "bridge", "Bridge"},       {BackgroundSlot::Divider, "divider", "Divider"},
};

double clampRadius(double radius)
{
	return std::clamp(radius, 0.0, kMaxBackgroundRadius);
}

double clampOutset(double outset)
{
	return std::clamp(outset, -kMaxBackgroundOutset, kMaxBackgroundOutset);
}

} // namespace

const char *backgroundFillId(BackgroundFill fill)
{
	for (const BackgroundFillInfo &info : kFills) {
		if (info.fill == fill)
			return info.id;
	}
	return kFills[0].id;
}

BackgroundFill backgroundFillFromId(const char *id, BackgroundFill fallback)
{
	if (!id)
		return fallback;
	for (const BackgroundFillInfo &info : kFills) {
		if (strcmp(id, info.id) == 0)
			return info.fill;
	}
	return fallback;
}

const char *backgroundFillName(BackgroundFill fill)
{
	for (const BackgroundFillInfo &info : kFills) {
		if (info.fill == fill)
			return info.name;
	}
	return kFills[0].name;
}

const QVector<BackgroundFill> &allBackgroundFills()
{
	static const QVector<BackgroundFill> fills = [] {
		QVector<BackgroundFill> list;
		for (const BackgroundFillInfo &info : kFills)
			list.append(info.fill);
		return list;
	}();
	return fills;
}

const char *backgroundImageFitId(BackgroundImageFit fit)
{
	for (const BackgroundImageFitInfo &info : kFits) {
		if (info.fit == fit)
			return info.id;
	}
	return kFits[0].id;
}

BackgroundImageFit backgroundImageFitFromId(const char *id, BackgroundImageFit fallback)
{
	if (!id)
		return fallback;
	for (const BackgroundImageFitInfo &info : kFits) {
		if (strcmp(id, info.id) == 0)
			return info.fit;
	}
	return fallback;
}

const char *backgroundImageFitName(BackgroundImageFit fit)
{
	for (const BackgroundImageFitInfo &info : kFits) {
		if (info.fit == fit)
			return info.name;
	}
	return kFits[0].name;
}

const QVector<BackgroundImageFit> &allBackgroundImageFits()
{
	static const QVector<BackgroundImageFit> fits = [] {
		QVector<BackgroundImageFit> list;
		for (const BackgroundImageFitInfo &info : kFits)
			list.append(info.fit);
		return list;
	}();
	return fits;
}

const char *backgroundSlotId(BackgroundSlot slot)
{
	for (const BackgroundSlotInfo &info : kSlots) {
		if (info.slot == slot)
			return info.id;
	}
	return kSlots[0].id;
}

BackgroundSlot backgroundSlotFromId(const char *id, BackgroundSlot fallback)
{
	if (!id)
		return fallback;
	for (const BackgroundSlotInfo &info : kSlots) {
		if (strcmp(id, info.id) == 0)
			return info.slot;
	}
	return fallback;
}

const char *backgroundSlotName(BackgroundSlot slot)
{
	for (const BackgroundSlotInfo &info : kSlots) {
		if (info.slot == slot)
			return info.name;
	}
	return kSlots[0].name;
}

const QVector<BackgroundSlot> &allBackgroundSlots()
{
	static const QVector<BackgroundSlot> all = [] {
		QVector<BackgroundSlot> list;
		for (const BackgroundSlotInfo &info : kSlots)
			list.append(info.slot);
		return list;
	}();
	return all;
}

bool BackgroundPanel::isVisible() const
{
	if (opacity <= 0.0)
		return false;

	/*
	 * A border is drawn inside the panel, so it is something the panel paints even when the fill
	 * itself is None -- an outline around a section, with the footage showing through it, is a
	 * perfectly ordinary thing to want and would otherwise be unreachable.
	 */
	const bool inked = border.enabled && border.width > 0.0 && border.color.alpha() > 0;

	switch (fill) {
	case BackgroundFill::None:
		return inked;
	case BackgroundFill::Color:
		return inked || color.alpha() > 0;
	case BackgroundFill::LinearGradient:
	case BackgroundFill::RadialGradient:
		return true;
	case BackgroundFill::Image:
		return inked || !imagePath.isEmpty();
	}
	return false;
}

double BackgroundPanel::bleed() const
{
	if (!isVisible())
		return 0.0;

	const double reach = std::max({outsetLeft, outsetTop, outsetRight, outsetBottom});
	return std::max(0.0, reach);
}

QRectF BackgroundPanel::outsetRect(const QRectF &box) const
{
	return box.adjusted(-outsetLeft, -outsetTop, outsetRight, outsetBottom);
}

void BackgroundPanel::setRadius(double radius)
{
	const double value = clampRadius(radius);
	radiusTopLeft = value;
	radiusTopRight = value;
	radiusBottomRight = value;
	radiusBottomLeft = value;
}

bool BackgroundPanel::hasUniformRadius() const
{
	return qFuzzyCompare(radiusTopLeft + 1.0, radiusTopRight + 1.0) &&
	       qFuzzyCompare(radiusTopLeft + 1.0, radiusBottomRight + 1.0) &&
	       qFuzzyCompare(radiusTopLeft + 1.0, radiusBottomLeft + 1.0);
}

bool BackgroundPanel::operator==(const BackgroundPanel &other) const
{
	return fill == other.fill && color == other.color && gradient == other.gradient &&
	       imagePath == other.imagePath && imageFit == other.imageFit &&
	       qFuzzyCompare(opacity + 1.0, other.opacity + 1.0) &&
	       qFuzzyCompare(outsetLeft + 1.0, other.outsetLeft + 1.0) &&
	       qFuzzyCompare(outsetTop + 1.0, other.outsetTop + 1.0) &&
	       qFuzzyCompare(outsetRight + 1.0, other.outsetRight + 1.0) &&
	       qFuzzyCompare(outsetBottom + 1.0, other.outsetBottom + 1.0) &&
	       qFuzzyCompare(radiusTopLeft + 1.0, other.radiusTopLeft + 1.0) &&
	       qFuzzyCompare(radiusTopRight + 1.0, other.radiusTopRight + 1.0) &&
	       qFuzzyCompare(radiusBottomRight + 1.0, other.radiusBottomRight + 1.0) &&
	       qFuzzyCompare(radiusBottomLeft + 1.0, other.radiusBottomLeft + 1.0) && border == other.border;
}

void BackgroundPanel::save(obs_data_t *data) const
{
	obs_data_set_string(data, "fill", backgroundFillId(fill));
	obs_data_set_int(data, "color", static_cast<long long>(color.rgba()));
	obs_data_set_string(data, "image_path", imagePath.toUtf8().constData());
	obs_data_set_string(data, "image_fit", backgroundImageFitId(imageFit));
	obs_data_set_double(data, "opacity", opacity);

	obs_data_set_double(data, "outset_left", outsetLeft);
	obs_data_set_double(data, "outset_top", outsetTop);
	obs_data_set_double(data, "outset_right", outsetRight);
	obs_data_set_double(data, "outset_bottom", outsetBottom);

	obs_data_set_double(data, "radius_tl", radiusTopLeft);
	obs_data_set_double(data, "radius_tr", radiusTopRight);
	obs_data_set_double(data, "radius_br", radiusBottomRight);
	obs_data_set_double(data, "radius_bl", radiusBottomLeft);

	OBSDataAutoRelease gradientData = obs_data_create();
	gradient.save(gradientData);
	obs_data_set_obj(data, "gradient", gradientData);

	OBSDataAutoRelease borderData = obs_data_create();
	obs_data_set_bool(borderData, "enabled", border.enabled);
	obs_data_set_double(borderData, "width", border.width);
	obs_data_set_int(borderData, "color", static_cast<long long>(border.color.rgba()));
	obs_data_set_obj(data, "border", borderData);
}

void BackgroundPanel::load(obs_data_t *data)
{
	fill = backgroundFillFromId(obs_data_get_string(data, "fill"), BackgroundFill::None);
	color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(data, "color")));
	imagePath = QString::fromUtf8(obs_data_get_string(data, "image_path"));
	imageFit = backgroundImageFitFromId(obs_data_get_string(data, "image_fit"), BackgroundImageFit::Cover);

	/*
	 * Absent means fully present rather than fully transparent. A panel written by anything that
	 * did not know about opacity is a panel that was drawn at its face value, and reading a
	 * missing key as 0 would make it vanish.
	 */
	opacity = obs_data_has_user_value(data, "opacity") ? obs_data_get_double(data, "opacity") : 1.0;
	opacity = std::clamp(opacity, 0.0, 1.0);

	outsetLeft = clampOutset(obs_data_get_double(data, "outset_left"));
	outsetTop = clampOutset(obs_data_get_double(data, "outset_top"));
	outsetRight = clampOutset(obs_data_get_double(data, "outset_right"));
	outsetBottom = clampOutset(obs_data_get_double(data, "outset_bottom"));

	radiusTopLeft = clampRadius(obs_data_get_double(data, "radius_tl"));
	radiusTopRight = clampRadius(obs_data_get_double(data, "radius_tr"));
	radiusBottomRight = clampRadius(obs_data_get_double(data, "radius_br"));
	radiusBottomLeft = clampRadius(obs_data_get_double(data, "radius_bl"));

	OBSDataAutoRelease gradientData = obs_data_get_obj(data, "gradient");
	if (gradientData)
		gradient.load(gradientData);

	OBSDataAutoRelease borderData = obs_data_get_obj(data, "border");
	if (borderData) {
		border.enabled = obs_data_get_bool(borderData, "enabled");
		border.width = std::clamp(obs_data_get_double(borderData, "width"), 0.0, kMaxBackgroundBorder);
		border.color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(borderData, "color")));
	}
}

void SectionBackground::save(obs_data_t *data) const
{
	obs_data_set_string(data, "slot", backgroundSlotId(slot));
	obs_data_set_string(data, "preset", presetName.toUtf8().constData());

	OBSDataAutoRelease panelData = obs_data_create();
	panel.save(panelData);
	obs_data_set_obj(data, "panel", panelData);
}

void SectionBackground::load(obs_data_t *data)
{
	slot = backgroundSlotFromId(obs_data_get_string(data, "slot"), BackgroundSlot::Section);
	presetName = QString::fromUtf8(obs_data_get_string(data, "preset"));

	OBSDataAutoRelease panelData = obs_data_get_obj(data, "panel");
	if (panelData)
		panel.load(panelData);
}

void BackgroundPreset::save(obs_data_t *data) const
{
	obs_data_set_string(data, "name", name.toUtf8().constData());
	obs_data_set_bool(data, "linked", linked);

	OBSDataAutoRelease panelData = obs_data_create();
	panel.save(panelData);
	obs_data_set_obj(data, "panel", panelData);
}

void BackgroundPreset::load(obs_data_t *data)
{
	name = QString::fromUtf8(obs_data_get_string(data, "name"));
	linked = obs_data_get_bool(data, "linked");

	OBSDataAutoRelease panelData = obs_data_get_obj(data, "panel");
	if (panelData)
		panel.load(panelData);
}

} // namespace closingtime
