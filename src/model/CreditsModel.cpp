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

#include "model/CreditsModel.hpp"

#include <obs.hpp>

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

struct SectionTypeInfo {
	SectionType type;
	const char *id;
	const char *name;
	bool text;
	bool logos;
	bool entries;
	bool columns;
};

/* Listed in the order the designer's "Add Section" menu presents them. */
const SectionTypeInfo kSectionTypes[] = {
	{SectionType::Title, "title", "Title", true, false, false, false},
	{SectionType::TitleWithLogo, "title_with_logo", "Title w/ Logo", true, true, false, false},
	{SectionType::LogoTitle, "logo_title", "Logo Title", false, true, false, false},
	{SectionType::Header, "header", "Header", true, false, false, false},
	{SectionType::HeaderWithLogo, "header_with_logo", "Header w/ Logo", true, true, false, false},
	{SectionType::LogoHeader, "logo_header", "Logo Header", false, true, false, false},
	{SectionType::Bridged, "bridged", "Text to Text Bridged", true, false, true, false},
	{SectionType::TextList, "text_list", "Text List", true, false, true, false},
	{SectionType::LogoList, "logo_list", "Logo List", false, true, true, false},
	{SectionType::MultiTextList, "multi_text_list", "Multi-List of Text", true, false, true, true},
	{SectionType::MultiLogoList, "multi_logo_list", "Multi-List of Logos", false, true, true, true},
	{SectionType::Spacer, "spacer", "Spacer", false, false, false, false},
};

const SectionTypeInfo &sectionTypeInfo(SectionType type)
{
	for (const auto &info : kSectionTypes) {
		if (info.type == type)
			return info;
	}
	return kSectionTypes[0];
}

/*
 * obs_data arrays only hold objects, so every list is stored as an array of objects even
 * when a single scalar would do. These helpers keep that boilerplate in one place.
 */
void saveArray(obs_data_t *parent, const char *name, int count, void (*writer)(obs_data_t *, int, const void *),
	       const void *context)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (int i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_create();
		writer(item, i, context);
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(parent, name, array);
}

} // namespace

const char *sectionTypeId(SectionType type)
{
	return sectionTypeInfo(type).id;
}

SectionType sectionTypeFromId(const char *id, SectionType fallback)
{
	if (!id)
		return fallback;

	for (const auto &info : kSectionTypes) {
		if (strcmp(info.id, id) == 0)
			return info.type;
	}
	return fallback;
}

const char *sectionTypeName(SectionType type)
{
	return sectionTypeInfo(type).name;
}

const QVector<SectionType> &allSectionTypes()
{
	static const QVector<SectionType> types = [] {
		QVector<SectionType> result;
		result.reserve(static_cast<int>(std::size(kSectionTypes)));
		for (const auto &info : kSectionTypes)
			result.append(info.type);
		return result;
	}();
	return types;
}

bool sectionUsesText(SectionType type)
{
	return sectionTypeInfo(type).text;
}

bool sectionUsesLogos(SectionType type)
{
	return sectionTypeInfo(type).logos;
}

bool sectionUsesEntries(SectionType type)
{
	return sectionTypeInfo(type).entries;
}

bool sectionUsesColumns(SectionType type)
{
	return sectionTypeInfo(type).columns;
}

const char *hAlignId(HAlign align)
{
	switch (align) {
	case HAlign::Left:
		return "left";
	case HAlign::Right:
		return "right";
	case HAlign::Center:
	default:
		return "center";
	}
}

HAlign hAlignFromId(const char *id, HAlign fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "left") == 0)
		return HAlign::Left;
	if (strcmp(id, "right") == 0)
		return HAlign::Right;
	if (strcmp(id, "center") == 0)
		return HAlign::Center;
	return fallback;
}

const char *logoSideId(LogoSide side)
{
	return side == LogoSide::Right ? "right" : "left";
}

LogoSide logoSideFromId(const char *id, LogoSide fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "right") == 0)
		return LogoSide::Right;
	if (strcmp(id, "left") == 0)
		return LogoSide::Left;
	return fallback;
}

const char *logoPlacementId(LogoPlacement placement)
{
	switch (placement) {
	case LogoPlacement::Hug:
		return "hug";
	case LogoPlacement::Bridged:
		return "bridged";
	case LogoPlacement::Edge:
	default:
		return "edge";
	}
}

LogoPlacement logoPlacementFromId(const char *id, LogoPlacement fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "hug") == 0)
		return LogoPlacement::Hug;
	if (strcmp(id, "bridged") == 0)
		return LogoPlacement::Bridged;
	if (strcmp(id, "edge") == 0)
		return LogoPlacement::Edge;
	return fallback;
}

const char *bridgeFillId(BridgeFill fill)
{
	switch (fill) {
	case BridgeFill::Repeat:
		return "repeat";
	case BridgeFill::Stretch:
		return "stretch";
	case BridgeFill::Fixed:
	default:
		return "fixed";
	}
}

BridgeFill bridgeFillFromId(const char *id, BridgeFill fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "repeat") == 0)
		return BridgeFill::Repeat;
	if (strcmp(id, "stretch") == 0)
		return BridgeFill::Stretch;
	if (strcmp(id, "fixed") == 0)
		return BridgeFill::Fixed;
	return fallback;
}

const char *bridgeSizingId(BridgeSizing sizing)
{
	return sizing == BridgeSizing::Natural ? "natural" : "split";
}

BridgeSizing bridgeSizingFromId(const char *id, BridgeSizing fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "natural") == 0)
		return BridgeSizing::Natural;
	if (strcmp(id, "split") == 0)
		return BridgeSizing::Split;
	return fallback;
}

const char *textFillId(TextFill fill)
{
	switch (fill) {
	case TextFill::LinearGradient:
		return "linear_gradient";
	case TextFill::RadialGradient:
		return "radial_gradient";
	case TextFill::Solid:
	default:
		return "solid";
	}
}

TextFill textFillFromId(const char *id, TextFill fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "solid") == 0)
		return TextFill::Solid;
	if (strcmp(id, "linear_gradient") == 0)
		return TextFill::LinearGradient;
	if (strcmp(id, "radial_gradient") == 0)
		return TextFill::RadialGradient;
	return fallback;
}

QVector<QPair<qreal, QColor>> GradientSpec::resolvedStops() const
{
	QVector<QPair<qreal, QColor>> resolved;
	resolved.reserve(std::max<qsizetype>(2, stops.size()));

	for (const GradientStop &stop : stops)
		resolved.append({std::clamp(stop.position, 0.0, 1.0), stop.color});

	std::stable_sort(resolved.begin(), resolved.end(),
			 [](const QPair<qreal, QColor> &a, const QPair<qreal, QColor> &b) {
				 return a.first < b.first;
			 });

	/*
	 * QGradient paints black wherever it has no stop to interpolate from, so a spec that
	 * has been edited down to one stop -- or none -- is padded out rather than allowed to
	 * blank the text out from under the user mid-edit.
	 */
	if (resolved.isEmpty())
		resolved.append({0.0, QColor(255, 255, 255)});
	if (resolved.size() == 1)
		resolved.append({1.0, resolved.first().second});

	return resolved;
}

void GradientSpec::save(obs_data_t *data) const
{
	obs_data_set_double(data, "angle", angle);

	saveArray(
		data, "stops", stops.size(),
		[](obs_data_t *item, int index, const void *context) {
			const GradientStop &stop = static_cast<const QVector<GradientStop> *>(context)->at(index);
			obs_data_set_double(item, "position", stop.position);
			obs_data_set_int(item, "color", static_cast<long long>(stop.color.rgba()));
		},
		&stops);
}

void GradientSpec::load(obs_data_t *data)
{
	angle = obs_data_get_double(data, "angle");

	OBSDataArrayAutoRelease array = obs_data_get_array(data, "stops");
	if (!array)
		return;

	stops.clear();
	const size_t count = obs_data_array_count(array);
	stops.reserve(static_cast<int>(count));
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		GradientStop stop;
		stop.position = std::clamp(obs_data_get_double(item, "position"), 0.0, 1.0);
		stop.color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(item, "color")));
		stops.append(stop);
	}
}

bool TextStyle::hasEffects() const
{
	return fill != TextFill::Solid || outline.enabled || shadow.enabled;
}

double TextStyle::effectBleed() const
{
	double bleed = 0.0;

	if (outline.enabled)
		bleed = std::max(bleed, outline.width);

	if (shadow.enabled) {
		const double reach = std::max(std::abs(shadow.offsetX), std::abs(shadow.offsetY)) + shadow.blur;
		/* The shadow carries the outline out with it, since it is cast by both together. */
		bleed = std::max(bleed, reach + (outline.enabled ? outline.width : 0.0));
	}

	return bleed;
}

void TextStyle::save(obs_data_t *data) const
{
	obs_data_set_string(data, "family", family.toUtf8().constData());
	obs_data_set_int(data, "pixel_size", pixelSize);
	obs_data_set_bool(data, "bold", bold);
	obs_data_set_bool(data, "italic", italic);
	obs_data_set_int(data, "color", static_cast<long long>(color.rgba()));
	obs_data_set_string(data, "align", hAlignId(align));
	obs_data_set_double(data, "line_spacing", lineSpacing);
	obs_data_set_string(data, "fill", textFillId(fill));

	OBSDataAutoRelease gradientData = obs_data_create();
	gradient.save(gradientData);
	obs_data_set_obj(data, "gradient", gradientData);

	OBSDataAutoRelease outlineData = obs_data_create();
	obs_data_set_bool(outlineData, "enabled", outline.enabled);
	obs_data_set_double(outlineData, "width", outline.width);
	obs_data_set_int(outlineData, "color", static_cast<long long>(outline.color.rgba()));
	obs_data_set_obj(data, "outline", outlineData);

	OBSDataAutoRelease shadowData = obs_data_create();
	obs_data_set_bool(shadowData, "enabled", shadow.enabled);
	obs_data_set_double(shadowData, "offset_x", shadow.offsetX);
	obs_data_set_double(shadowData, "offset_y", shadow.offsetY);
	obs_data_set_double(shadowData, "blur", shadow.blur);
	obs_data_set_int(shadowData, "color", static_cast<long long>(shadow.color.rgba()));
	obs_data_set_obj(data, "shadow", shadowData);
}

void TextStyle::load(obs_data_t *data)
{
	family = QString::fromUtf8(obs_data_get_string(data, "family"));
	if (family.isEmpty())
		family = QStringLiteral("Sans Serif");

	pixelSize = static_cast<int>(obs_data_get_int(data, "pixel_size"));
	if (pixelSize <= 0)
		pixelSize = 32;

	bold = obs_data_get_bool(data, "bold");
	italic = obs_data_get_bool(data, "italic");
	color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(data, "color")));
	align = hAlignFromId(obs_data_get_string(data, "align"), HAlign::Center);

	lineSpacing = obs_data_get_double(data, "line_spacing");
	if (lineSpacing <= 0.0)
		lineSpacing = 1.0;

	/* Absent in documents written before fills existed, which were all solid colour. */
	fill = textFillFromId(obs_data_get_string(data, "fill"), TextFill::Solid);

	OBSDataAutoRelease gradientData = obs_data_get_obj(data, "gradient");
	if (gradientData)
		gradient.load(gradientData);

	OBSDataAutoRelease outlineData = obs_data_get_obj(data, "outline");
	if (outlineData) {
		outline.enabled = obs_data_get_bool(outlineData, "enabled");
		outline.width = std::max(0.0, obs_data_get_double(outlineData, "width"));
		outline.color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(outlineData, "color")));
	}

	OBSDataAutoRelease shadowData = obs_data_get_obj(data, "shadow");
	if (shadowData) {
		shadow.enabled = obs_data_get_bool(shadowData, "enabled");
		shadow.offsetX = obs_data_get_double(shadowData, "offset_x");
		shadow.offsetY = obs_data_get_double(shadowData, "offset_y");
		shadow.blur = std::max(0.0, obs_data_get_double(shadowData, "blur"));
		shadow.color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(shadowData, "color")));
	}
}

void TextStyle::defaults(obs_data_t *data, int pixelSize, bool bold)
{
	obs_data_set_default_string(data, "family", "Sans Serif");
	obs_data_set_default_int(data, "pixel_size", pixelSize);
	obs_data_set_default_bool(data, "bold", bold);
	obs_data_set_default_bool(data, "italic", false);
	obs_data_set_default_int(data, "color", static_cast<long long>(qRgba(255, 255, 255, 255)));
	obs_data_set_default_string(data, "align", "center");
	obs_data_set_default_double(data, "line_spacing", 1.0);
	obs_data_set_default_string(data, "fill", textFillId(TextFill::Solid));
}

void StylePreset::save(obs_data_t *data) const
{
	obs_data_set_string(data, "name", name.toUtf8().constData());

	OBSDataAutoRelease styleData = obs_data_create();
	style.save(styleData);
	obs_data_set_obj(data, "style", styleData);
}

void StylePreset::load(obs_data_t *data)
{
	name = QString::fromUtf8(obs_data_get_string(data, "name"));

	OBSDataAutoRelease styleData = obs_data_get_obj(data, "style");
	if (styleData)
		style.load(styleData);
}

void LogoRef::save(obs_data_t *data) const
{
	obs_data_set_string(data, "path", path.toUtf8().constData());
	obs_data_set_int(data, "max_height", maxHeight);
}

void LogoRef::load(obs_data_t *data)
{
	path = QString::fromUtf8(obs_data_get_string(data, "path"));
	maxHeight = static_cast<int>(obs_data_get_int(data, "max_height"));
	if (maxHeight <= 0)
		maxHeight = 96;
}

void Entry::save(obs_data_t *data) const
{
	obs_data_set_string(data, "text", text.toUtf8().constData());
	obs_data_set_string(data, "secondary_text", secondaryText.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);
}

void Entry::load(obs_data_t *data)
{
	text = QString::fromUtf8(obs_data_get_string(data, "text"));
	secondaryText = QString::fromUtf8(obs_data_get_string(data, "secondary_text"));

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		logo.load(logoData);
}

void Section::save(obs_data_t *data) const
{
	obs_data_set_string(data, "type", sectionTypeId(type));
	obs_data_set_string(data, "label", label.toUtf8().constData());
	obs_data_set_string(data, "text", text.toUtf8().constData());
	obs_data_set_string(data, "logo_side", logoSideId(logoSide));
	obs_data_set_string(data, "logo_placement", logoPlacementId(logoPlacement));
	obs_data_set_int(data, "logo_gap", logoGap);
	obs_data_set_string(data, "bridge_type", bridgeTypeId(bridgeType));
	obs_data_set_string(data, "bridge", bridge.toUtf8().constData());
	obs_data_set_string(data, "bridge_svg", bridgeSvg.toUtf8().constData());
	obs_data_set_double(data, "bridge_thickness", bridgeThickness);
	obs_data_set_double(data, "bridge_offset", bridgeOffset);
	obs_data_set_double(data, "bridge_gap", bridgeGap);
	obs_data_set_bool(data, "bridge_tint", bridgeTint);
	obs_data_set_string(data, "bridge_fill", bridgeFillId(bridgeFill));
	obs_data_set_string(data, "bridge_sizing", bridgeSizingId(bridgeSizing));
	obs_data_set_double(data, "bridge_split", bridgeSplit);
	obs_data_set_string(data, "bridge_row_align", hAlignId(bridgeRowAlign));
	obs_data_set_bool(data, "bridge_span_empty", bridgeSpanEmpty);
	obs_data_set_int(data, "columns", columns);
	obs_data_set_int(data, "column_gap", columnGap);
	obs_data_set_int(data, "entry_gap", entryGap);
	obs_data_set_bool(data, "fill_across", fillAcross);
	obs_data_set_int(data, "padding_top", paddingTop);
	obs_data_set_int(data, "padding_bottom", paddingBottom);
	obs_data_set_int(data, "margin_x", marginX);
	obs_data_set_int(data, "spacer_height", spacerHeight);
	obs_data_set_bool(data, "visible", visible);
	obs_data_set_bool(data, "use_secondary_style", useSecondaryStyle);
	obs_data_set_string(data, "style_preset", stylePresetName.toUtf8().constData());
	obs_data_set_string(data, "secondary_style_preset", secondaryStylePresetName.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);

	OBSDataAutoRelease styleData = obs_data_create();
	style.save(styleData);
	obs_data_set_obj(data, "style", styleData);

	OBSDataAutoRelease secondaryData = obs_data_create();
	secondaryStyle.save(secondaryData);
	obs_data_set_obj(data, "secondary_style", secondaryData);

	saveArray(
		data, "entries", entries.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<Entry> *>(context)->at(index).save(item);
		},
		&entries);
}

void Section::load(obs_data_t *data)
{
	type = sectionTypeFromId(obs_data_get_string(data, "type"), SectionType::Title);
	label = QString::fromUtf8(obs_data_get_string(data, "label"));
	text = QString::fromUtf8(obs_data_get_string(data, "text"));
	logoSide = logoSideFromId(obs_data_get_string(data, "logo_side"), LogoSide::Left);
	/* Documents written before this setting existed were laid out against Edge. */
	logoPlacement = logoPlacementFromId(obs_data_get_string(data, "logo_placement"), LogoPlacement::Edge);
	logoGap = static_cast<int>(obs_data_get_int(data, "logo_gap"));
	/* Documents written before the art types existed carried a string bridge and nothing else. */
	bridgeType = bridgeTypeFromId(obs_data_get_string(data, "bridge_type"), BridgeType::Text);
	bridge = QString::fromUtf8(obs_data_get_string(data, "bridge"));
	bridgeSvg = QString::fromUtf8(obs_data_get_string(data, "bridge_svg"));
	bridgeOffset = obs_data_get_double(data, "bridge_offset");
	/*
	 * 0 is a legitimate gap -- art running right up to the words -- so a missing key has to be
	 * told apart from a stored zero. Absent, it takes the same default a new section gets, so
	 * a document that predates the art types is not handed a worse one on switching to it.
	 */
	bridgeGap = obs_data_has_user_value(data, "bridge_gap") ? obs_data_get_double(data, "bridge_gap") : 8.0;
	bridgeGap = std::max(0.0, bridgeGap);
	/* A thickness of zero would draw nothing at all, which no document ever means. */
	bridgeThickness = obs_data_get_double(data, "bridge_thickness");
	if (bridgeThickness <= 0.0)
		bridgeThickness = 4.0;
	/* Absent in documents that predate custom art, whose built-in tiles are tinted regardless. */
	bridgeTint = obs_data_has_user_value(data, "bridge_tint") ? obs_data_get_bool(data, "bridge_tint") : true;
	bridgeFill = bridgeFillFromId(obs_data_get_string(data, "bridge_fill"), BridgeFill::Fixed);
	bridgeSizing = bridgeSizingFromId(obs_data_get_string(data, "bridge_sizing"), BridgeSizing::Split);
	bridgeRowAlign = hAlignFromId(obs_data_get_string(data, "bridge_row_align"), HAlign::Center);
	bridgeSpanEmpty = obs_data_get_bool(data, "bridge_span_empty");

	/*
	 * 0.0 is a legitimate split -- everything to the right of the bridge -- so a missing
	 * key has to be told apart from a stored zero rather than inferred from the value.
	 */
	bridgeSplit = obs_data_has_user_value(data, "bridge_split") ? obs_data_get_double(data, "bridge_split") : 0.5;
	bridgeSplit = std::clamp(bridgeSplit, 0.0, 1.0);
	columns = static_cast<int>(obs_data_get_int(data, "columns"));
	columnGap = static_cast<int>(obs_data_get_int(data, "column_gap"));
	entryGap = static_cast<int>(obs_data_get_int(data, "entry_gap"));
	fillAcross = obs_data_get_bool(data, "fill_across");
	paddingTop = static_cast<int>(obs_data_get_int(data, "padding_top"));
	paddingBottom = static_cast<int>(obs_data_get_int(data, "padding_bottom"));
	marginX = static_cast<int>(obs_data_get_int(data, "margin_x"));
	spacerHeight = static_cast<int>(obs_data_get_int(data, "spacer_height"));
	visible = obs_data_get_bool(data, "visible");
	useSecondaryStyle = obs_data_get_bool(data, "use_secondary_style");
	stylePresetName = QString::fromUtf8(obs_data_get_string(data, "style_preset"));
	secondaryStylePresetName = QString::fromUtf8(obs_data_get_string(data, "secondary_style_preset"));

	if (columns < 1)
		columns = 1;
	if (spacerHeight < 0)
		spacerHeight = 0;
	if (entryGap < 0)
		entryGap = 0;

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		logo.load(logoData);

	OBSDataAutoRelease styleData = obs_data_get_obj(data, "style");
	if (styleData)
		style.load(styleData);

	OBSDataAutoRelease secondaryData = obs_data_get_obj(data, "secondary_style");
	if (secondaryData)
		secondaryStyle.load(secondaryData);
	else
		secondaryStyle = style;

	entries.clear();
	OBSDataArrayAutoRelease array = obs_data_get_array(data, "entries");
	if (array) {
		const size_t count = obs_data_array_count(array);
		entries.reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			Entry entry;
			entry.load(item);
			entries.append(entry);
		}
	}
}

Section Section::makeDefault(SectionType type)
{
	Section section;
	section.type = type;

	switch (type) {
	case SectionType::Title:
	case SectionType::TitleWithLogo:
		section.text = QStringLiteral("Title");
		section.style.pixelSize = 72;
		section.style.bold = true;
		section.paddingTop = 48;
		section.paddingBottom = 32;
		break;

	case SectionType::LogoTitle:
		section.logo.maxHeight = 200;
		section.paddingTop = 48;
		section.paddingBottom = 32;
		break;

	case SectionType::Header:
	case SectionType::HeaderWithLogo:
		section.text = QStringLiteral("Header");
		section.style.pixelSize = 44;
		section.style.bold = true;
		section.paddingTop = 32;
		section.paddingBottom = 16;
		break;

	case SectionType::LogoHeader:
		section.logo.maxHeight = 120;
		section.paddingTop = 32;
		section.paddingBottom = 16;
		break;

	case SectionType::Bridged:
		section.style.pixelSize = 32;
		section.style.align = HAlign::Right;
		section.secondaryStyle = section.style;
		section.secondaryStyle.align = HAlign::Left;
		section.useSecondaryStyle = true;
		section.marginX = 160;
		section.entries.append(Entry{QStringLiteral("Role"), QStringLiteral("Name"), {}});
		break;

	case SectionType::TextList:
		section.style.pixelSize = 32;
		section.entries.append(Entry{QStringLiteral("Name"), {}, {}});
		break;

	case SectionType::LogoList:
		section.entries.append(Entry{});
		break;

	case SectionType::MultiTextList:
		section.style.pixelSize = 32;
		section.columns = 3;
		section.marginX = 120;
		for (int i = 0; i < 3; ++i)
			section.entries.append(Entry{QStringLiteral("Name"), {}, {}});
		break;

	case SectionType::MultiLogoList:
		section.columns = 3;
		section.marginX = 120;
		for (int i = 0; i < 3; ++i)
			section.entries.append(Entry{});
		break;

	case SectionType::Spacer:
		section.paddingTop = 0;
		section.paddingBottom = 0;
		break;
	}

	/*
	 * A section being added now gets the placement that actually honours logoGap. Edge
	 * stays the load-time fallback, so documents predating the setting keep the layout they
	 * were built against.
	 */
	if (sectionUsesLogos(type) && sectionUsesText(type))
		section.logoPlacement = LogoPlacement::Hug;

	/*
	 * Likewise for the bridge: a section being added now gets drawn art, tiled across the
	 * gap, rather than a string of full stops that depends on the font to look like a leader.
	 * Text stays the load-time fallback for documents written before the art types existed.
	 */
	if (type == SectionType::Bridged || (sectionUsesLogos(type) && sectionUsesText(type))) {
		section.bridgeType = BridgeType::Dots;
		section.bridgeFill = BridgeFill::Repeat;
	}

	return section;
}

QString Section::displayLabel() const
{
	if (!label.isEmpty())
		return label;

	switch (type) {
	case SectionType::Title:
	case SectionType::TitleWithLogo:
	case SectionType::Header:
	case SectionType::HeaderWithLogo:
		return text.isEmpty() ? QString::fromUtf8(sectionTypeName(type)) : text;

	case SectionType::LogoTitle:
	case SectionType::LogoHeader:
		return logo.isEmpty() ? QString::fromUtf8(sectionTypeName(type))
				      : QFileInfo(logo.path).completeBaseName();

	case SectionType::Spacer:
		return QStringLiteral("%1 (%2 px)").arg(QString::fromUtf8(sectionTypeName(type))).arg(spacerHeight);

	default:
		break;
	}

	return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(sectionTypeName(type))).arg(entries.size());
}

const TextStyle *Document::findStylePreset(const QString &name) const
{
	if (name.isEmpty())
		return nullptr;

	for (const StylePreset &preset : stylePresets) {
		if (preset.name == name)
			return &preset.style;
	}
	return nullptr;
}

const TextStyle &Document::effectiveStyle(const Section &section) const
{
	const TextStyle *preset = findStylePreset(section.stylePresetName);
	return preset ? *preset : section.style;
}

const TextStyle &Document::effectiveSecondaryStyle(const Section &section) const
{
	if (!section.useSecondaryStyle)
		return effectiveStyle(section);

	const TextStyle *preset = findStylePreset(section.secondaryStylePresetName);
	return preset ? *preset : section.secondaryStyle;
}

void Document::setStylePreset(const QString &name, const TextStyle &style)
{
	if (name.isEmpty())
		return;

	for (StylePreset &preset : stylePresets) {
		if (preset.name == name) {
			preset.style = style;
			return;
		}
	}

	stylePresets.append(StylePreset{name, style});
}

void Document::removeStylePreset(const QString &name)
{
	if (name.isEmpty())
		return;

	for (int i = 0; i < stylePresets.size(); ++i) {
		if (stylePresets.at(i).name == name) {
			stylePresets.removeAt(i);
			break;
		}
	}

	/*
	 * Bindings are cleared rather than left dangling. Resolution would fall back to the
	 * section's own style either way, but clearing them means a later preset that happens
	 * to reuse the name does not silently recapture sections the user had unbound.
	 */
	for (Section &section : sections) {
		if (section.stylePresetName == name)
			section.stylePresetName.clear();
		if (section.secondaryStylePresetName == name)
			section.secondaryStylePresetName.clear();
	}
}

void Document::save(obs_data_t *data) const
{
	obs_data_set_int(data, "width", width);
	obs_data_set_int(data, "height", height);
	obs_data_set_int(data, "background", static_cast<long long>(background.rgba()));
	obs_data_set_double(data, "scroll_speed", scrollSpeed);
	obs_data_set_int(data, "lead_in", leadIn);
	obs_data_set_int(data, "lead_out", leadOut);
	obs_data_set_bool(data, "loop", loop);
	obs_data_set_bool(data, "start_on_show", startOnShow);
	obs_data_set_double(data, "start_delay", startDelay);

	endingAction.save(data);

	saveArray(
		data, "style_presets", stylePresets.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<StylePreset> *>(context)->at(index).save(item);
		},
		&stylePresets);

	saveArray(
		data, "sections", sections.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<Section> *>(context)->at(index).save(item);
		},
		&sections);
}

void Document::load(obs_data_t *data)
{
	width = static_cast<int>(obs_data_get_int(data, "width"));
	height = static_cast<int>(obs_data_get_int(data, "height"));
	background = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(data, "background")));
	scrollSpeed = obs_data_get_double(data, "scroll_speed");
	leadIn = static_cast<int>(obs_data_get_int(data, "lead_in"));
	leadOut = static_cast<int>(obs_data_get_int(data, "lead_out"));
	loop = obs_data_get_bool(data, "loop");
	startOnShow = obs_data_get_bool(data, "start_on_show");
	startDelay = obs_data_get_double(data, "start_delay");

	/* Guard against hand-edited or truncated scene collections. */
	if (width < 1)
		width = 1920;
	if (height < 1)
		height = 1080;
	if (leadIn < 0)
		leadIn = 0;
	if (leadOut < 0)
		leadOut = 0;
	if (startDelay < 0.0)
		startDelay = 0.0;

	endingAction.load(data);

	stylePresets.clear();
	OBSDataArrayAutoRelease presetArray = obs_data_get_array(data, "style_presets");
	if (presetArray) {
		const size_t count = obs_data_array_count(presetArray);
		stylePresets.reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(presetArray, i);
			StylePreset preset;
			preset.load(item);
			/* An unnamed preset can never be resolved, so it is dropped on load. */
			if (!preset.name.isEmpty())
				stylePresets.append(preset);
		}
	}

	sections.clear();
	OBSDataArrayAutoRelease array = obs_data_get_array(data, "sections");
	if (array) {
		const size_t count = obs_data_array_count(array);
		sections.reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			Section section;
			section.load(item);
			sections.append(section);
		}
	}
}

void Document::defaults(obs_data_t *data)
{
	obs_data_set_default_int(data, "width", 1920);
	obs_data_set_default_int(data, "height", 1080);
	obs_data_set_default_int(data, "background", 0);
	obs_data_set_default_double(data, "scroll_speed", 90.0);
	obs_data_set_default_int(data, "lead_in", 0);
	obs_data_set_default_int(data, "lead_out", 0);
	obs_data_set_default_bool(data, "loop", false);
	obs_data_set_default_bool(data, "start_on_show", true);
	obs_data_set_default_double(data, "start_delay", 0.0);

	EndingActionConfig::defaults(data);
}

QString Document::toJson() const
{
	OBSDataAutoRelease data = obs_data_create();
	save(data);
	return QString::fromUtf8(obs_data_get_json_pretty(data));
}

bool Document::fromJson(const QString &json, QString *error)
{
	OBSDataAutoRelease data = obs_data_create_from_json(json.toUtf8().constData());
	if (!data) {
		if (error)
			*error = QStringLiteral("The file is not valid Closing Time JSON.");
		return false;
	}

	load(data);
	return true;
}

} // namespace closingtime
