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

void TextStyle::save(obs_data_t *data) const
{
	obs_data_set_string(data, "family", family.toUtf8().constData());
	obs_data_set_int(data, "pixel_size", pixelSize);
	obs_data_set_bool(data, "bold", bold);
	obs_data_set_bool(data, "italic", italic);
	obs_data_set_int(data, "color", static_cast<long long>(color.rgba()));
	obs_data_set_string(data, "align", hAlignId(align));
	obs_data_set_double(data, "line_spacing", lineSpacing);
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
	obs_data_set_int(data, "logo_gap", logoGap);
	obs_data_set_string(data, "bridge", bridge.toUtf8().constData());
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
	logoGap = static_cast<int>(obs_data_get_int(data, "logo_gap"));
	bridge = QString::fromUtf8(obs_data_get_string(data, "bridge"));
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
