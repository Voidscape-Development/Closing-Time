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

#include "model/StyleLibrary.hpp"

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
	bool subtitles;
};

/* Listed in the order the designer's "Add Section" menu presents them. */
const SectionTypeInfo kSectionTypes[] = {
	{SectionType::Title, "title", "Title", true, false, false, false, false},
	/*
	 * The subtitle variants say yes to `subtitles` without saying yes to `entries`: the flag is
	 * about a subtitle being stacked under a title, which a single heading does as much as a list
	 * of pairs does, and it is what gives `subtitleGap` and `subtitleFirst` something to act on.
	 */
	{SectionType::TitleWithSubtitle, "title_with_subtitle", "Title w/ Subtitle", true, false, false, false, true},
	{SectionType::TitleWithLogo, "title_with_logo", "Title w/ Logo", true, true, false, false, false},
	{SectionType::TitleWithSubtitleAndLogo, "title_with_subtitle_logo", "Title w/ Subtitle & Logo", true, true,
	 false, false, true},
	{SectionType::LogoTitle, "logo_title", "Logo Title", false, true, false, false, false},
	{SectionType::Header, "header", "Header", true, false, false, false, false},
	{SectionType::HeaderWithSubtitle, "header_with_subtitle", "Header w/ Subtitle", true, false, false, false,
	 true},
	{SectionType::HeaderWithLogo, "header_with_logo", "Header w/ Logo", true, true, false, false, false},
	{SectionType::HeaderWithSubtitleAndLogo, "header_with_subtitle_logo", "Header w/ Subtitle & Logo", true, true,
	 false, false, true},
	{SectionType::LogoHeader, "logo_header", "Logo Header", false, true, false, false, false},
	{SectionType::Bridged, "bridged", "Text to Text Bridged", true, false, true, false, false},
	{SectionType::TextList, "text_list", "Text List", true, false, true, false, false},
	{SectionType::TitleSubtitleList, "title_subtitle_list", "Title and Subtitle List", true, false, true, false,
	 true},
	{SectionType::LogoList, "logo_list", "Logo List", false, true, true, false, false},
	{SectionType::MultiTextList, "multi_text_list", "Multi-List of Text", true, false, true, true, false},
	{SectionType::MultiTitleSubtitleList, "multi_title_subtitle_list", "Title and Subtitle Multi-List", true, false,
	 true, true, true},
	{SectionType::MultiLogoList, "multi_logo_list", "Multi-List of Logos", false, true, true, true, false},
	/*
	 * Every flag is false: a divider has no section text, no section logo and no entry list.
	 * The text and the logos it can carry live in its own centre stack, which is a different
	 * shape from an entry list and is edited by a table of its own -- so answering yes to any
	 * of these would hand the divider the rows and the buttons belonging to a list it does not
	 * have. What it does show is decided by type in SectionEditor::applyTypeVisibility.
	 */
	{SectionType::SectionDivider, "section_divider", "Section Divider", false, false, false, false, false},
	{SectionType::Spacer, "spacer", "Spacer", false, false, false, false, false},
	/*
	 * Every flag is false for the same reason a divider's are: a sticky block has no content of
	 * its own at all. What it holds is whole sections, each of which answers these questions for
	 * itself, so a block claiming to carry text or a list would be claiming its children's.
	 */
	{SectionType::StickyBlock, "sticky_block", "Sticky Ending Block", false, false, false, false, false},
};

/* Listed in the order the divider's centre-piece picker presents them. */
const struct {
	DividerPiece::Kind kind;
	const char *id;
	const char *name;
} kDividerPieceKinds[] = {
	{DividerPiece::Kind::Ornament, "ornament", "Ornament"},
	{DividerPiece::Kind::Text, "text", "Text"},
	{DividerPiece::Kind::Logo, "logo", "Logo"},
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

bool sectionUsesSubtitles(SectionType type)
{
	return sectionTypeInfo(type).subtitles;
}

void visitSections(const QVector<Section> &sections, const std::function<void(const Section &)> &visit)
{
	for (const Section &section : sections) {
		visit(section);
		for (const Section &child : section.children)
			visit(child);
	}
}

void visitSections(QVector<Section> &sections, const std::function<void(Section &)> &visit)
{
	for (Section &section : sections) {
		visit(section);
		for (Section &child : section.children)
			visit(child);
	}
}

bool sectionStacksSubtitles(const Section &section)
{
	/*
	 * A bridged row is the one shape whose subtitles are a choice rather than a property of the
	 * type, so it is the one that has to be asked about the section rather than about the type.
	 */
	if (section.type == SectionType::Bridged)
		return section.rowSubtitles;

	return sectionUsesSubtitles(section.type);
}

bool sectionUsesSecondaryText(SectionType type)
{
	return type == SectionType::Bridged || sectionUsesSubtitles(type);
}

const char *dividerPieceKindId(DividerPiece::Kind kind)
{
	for (const auto &info : kDividerPieceKinds) {
		if (info.kind == kind)
			return info.id;
	}
	return kDividerPieceKinds[0].id;
}

DividerPiece::Kind dividerPieceKindFromId(const char *id, DividerPiece::Kind fallback)
{
	if (!id)
		return fallback;

	for (const auto &info : kDividerPieceKinds) {
		if (strcmp(info.id, id) == 0)
			return info.kind;
	}
	return fallback;
}

const char *dividerPieceKindName(DividerPiece::Kind kind)
{
	for (const auto &info : kDividerPieceKinds) {
		if (info.kind == kind)
			return info.name;
	}
	return kDividerPieceKinds[0].name;
}

const QVector<DividerPiece::Kind> &allDividerPieceKinds()
{
	static const QVector<DividerPiece::Kind> kinds = [] {
		QVector<DividerPiece::Kind> result;
		result.reserve(static_cast<int>(std::size(kDividerPieceKinds)));
		for (const auto &info : kDividerPieceKinds)
			result.append(info.kind);
		return result;
	}();
	return kinds;
}

SectionTypeSwitches decomposeSectionType(SectionType type)
{
	SectionTypeSwitches switches;

	switch (type) {
	case SectionType::Title:
	case SectionType::TitleWithSubtitle:
	case SectionType::TitleWithLogo:
	case SectionType::TitleWithSubtitleAndLogo:
	case SectionType::LogoTitle:
		switches.base = SectionType::Title;
		break;

	case SectionType::Header:
	case SectionType::HeaderWithSubtitle:
	case SectionType::HeaderWithLogo:
	case SectionType::HeaderWithSubtitleAndLogo:
	case SectionType::LogoHeader:
		switches.base = SectionType::Header;
		break;

	case SectionType::TextList:
	case SectionType::TitleSubtitleList:
	case SectionType::LogoList:
	case SectionType::MultiTextList:
	case SectionType::MultiTitleSubtitleList:
	case SectionType::MultiLogoList:
		switches.base = SectionType::TextList;
		break;

	default:
		/* Every other type is its own base and carries no switches at all. */
		switches.base = type;
		return switches;
	}

	if (switches.base == SectionType::TextList) {
		switch (type) {
		case SectionType::TitleSubtitleList:
		case SectionType::MultiTitleSubtitleList:
			switches.content = SectionListContent::Pairs;
			break;
		case SectionType::LogoList:
		case SectionType::MultiLogoList:
			switches.content = SectionListContent::Logos;
			break;
		default:
			switches.content = SectionListContent::Text;
			break;
		}

		switches.multiColumn = sectionUsesColumns(type);
		return switches;
	}

	/*
	 * A heading is either a logo or words, so "logo only" is the absence of text rather than a
	 * flag of its own -- which is what keeps the two switches from ever both being on.
	 */
	switches.logoOnly = sectionUsesLogos(type) && !sectionUsesText(type);
	switches.logo = sectionUsesLogos(type) && sectionUsesText(type);
	switches.subtitle = sectionUsesSubtitles(type);

	return switches;
}

SectionType composeSectionType(const SectionTypeSwitches &switches)
{
	const bool title = switches.base == SectionType::Title;

	if (title || switches.base == SectionType::Header) {
		if (switches.logoOnly)
			return title ? SectionType::LogoTitle : SectionType::LogoHeader;
		if (switches.subtitle && switches.logo)
			return title ? SectionType::TitleWithSubtitleAndLogo
				     : SectionType::HeaderWithSubtitleAndLogo;
		if (switches.subtitle)
			return title ? SectionType::TitleWithSubtitle : SectionType::HeaderWithSubtitle;
		if (switches.logo)
			return title ? SectionType::TitleWithLogo : SectionType::HeaderWithLogo;

		return title ? SectionType::Title : SectionType::Header;
	}

	if (switches.base == SectionType::TextList) {
		switch (switches.content) {
		case SectionListContent::Pairs:
			return switches.multiColumn ? SectionType::MultiTitleSubtitleList
						    : SectionType::TitleSubtitleList;
		case SectionListContent::Logos:
			return switches.multiColumn ? SectionType::MultiLogoList : SectionType::LogoList;
		case SectionListContent::Text:
		default:
			return switches.multiColumn ? SectionType::MultiTextList : SectionType::TextList;
		}
	}

	return switches.base;
}

const char *stickyAnchorId(StickyAnchor anchor)
{
	switch (anchor) {
	case StickyAnchor::Top:
		return "top";
	case StickyAnchor::Bottom:
		return "bottom";
	case StickyAnchor::Center:
	default:
		return "center";
	}
}

StickyAnchor stickyAnchorFromId(const char *id, StickyAnchor fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "top") == 0)
		return StickyAnchor::Top;
	if (strcmp(id, "bottom") == 0)
		return StickyAnchor::Bottom;
	if (strcmp(id, "center") == 0)
		return StickyAnchor::Center;
	return fallback;
}

double stickyAnchorFraction(StickyAnchor anchor)
{
	switch (anchor) {
	case StickyAnchor::Top:
		return 0.0;
	case StickyAnchor::Bottom:
		return 1.0;
	case StickyAnchor::Center:
	default:
		return 0.5;
	}
}

const char *stickyReleaseId(StickyRelease release)
{
	switch (release) {
	case StickyRelease::ResumeThenEnd:
		return "resume_then_end";
	case StickyRelease::ResumeEndAtHold:
		return "resume_end_at_hold";
	case StickyRelease::EndAtHold:
	default:
		return "end_at_hold";
	}
}

StickyRelease stickyReleaseFromId(const char *id, StickyRelease fallback)
{
	if (!id)
		return fallback;
	if (strcmp(id, "resume_then_end") == 0)
		return StickyRelease::ResumeThenEnd;
	if (strcmp(id, "resume_end_at_hold") == 0)
		return StickyRelease::ResumeEndAtHold;
	if (strcmp(id, "end_at_hold") == 0)
		return StickyRelease::EndAtHold;
	return fallback;
}

bool stickyReleaseResumes(StickyRelease release)
{
	return release != StickyRelease::EndAtHold;
}

bool stickyReleaseEndsAtHold(StickyRelease release)
{
	return release != StickyRelease::ResumeThenEnd;
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

BridgeFill effectiveBridgeFill(const Section &section)
{
	/*
	 * An empty bridge has nothing to cover a gap with, so the three fills would differ only in
	 * how they measure the text columns -- invisibly, from a control whose name promises
	 * something visible. Fixed is the one that behaves the way an empty gap reads: the minimum
	 * is reserved between the two columns and the split divides what is left of them.
	 */
	return bridgeTypeIsEmpty(section.bridgeType) ? BridgeFill::Fixed : section.bridgeFill;
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
	obs_data_set_string(data, "style_name", styleName.toUtf8().constData());
	obs_data_set_int(data, "pixel_size", pixelSize);
	/*
	 * Written beside the face name rather than instead of it. They are what the roll falls back
	 * to on a machine that does not have that exact face, and they are also what a reader older
	 * than the picker sees -- which is the whole of what it sees, so a bold face has to leave
	 * `bold` true behind it.
	 */
	obs_data_set_bool(data, "bold", bold);
	obs_data_set_bool(data, "italic", italic);
	obs_data_set_bool(data, "underline", underline);
	obs_data_set_bool(data, "strikeout", strikeOut);
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

	/*
	 * Absent in every document written before the font picker existed, where the family's own
	 * default face was the only one a style could name. Empty is exactly that, so there is
	 * nothing to migrate: the bold and italic flags below carry those styles as they always did.
	 */
	styleName = QString::fromUtf8(obs_data_get_string(data, "style_name"));

	pixelSize = static_cast<int>(obs_data_get_int(data, "pixel_size"));
	if (pixelSize <= 0)
		pixelSize = 32;

	bold = obs_data_get_bool(data, "bold");
	italic = obs_data_get_bool(data, "italic");
	underline = obs_data_get_bool(data, "underline");
	strikeOut = obs_data_get_bool(data, "strikeout");
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
	obs_data_set_default_string(data, "style_name", "");
	obs_data_set_default_int(data, "pixel_size", pixelSize);
	obs_data_set_default_bool(data, "bold", bold);
	obs_data_set_default_bool(data, "italic", false);
	obs_data_set_default_bool(data, "underline", false);
	obs_data_set_default_bool(data, "strikeout", false);
	obs_data_set_default_int(data, "color", static_cast<long long>(qRgba(255, 255, 255, 255)));
	obs_data_set_default_string(data, "align", "center");
	obs_data_set_default_double(data, "line_spacing", 1.0);
	obs_data_set_default_string(data, "fill", textFillId(TextFill::Solid));
}

void StylePreset::save(obs_data_t *data) const
{
	obs_data_set_string(data, "name", name.toUtf8().constData());
	obs_data_set_bool(data, "linked", linked);

	OBSDataAutoRelease styleData = obs_data_create();
	style.save(styleData);
	obs_data_set_obj(data, "style", styleData);
}

void StylePreset::load(obs_data_t *data)
{
	name = QString::fromUtf8(obs_data_get_string(data, "name"));
	/* Absent in a document written before the library existed, which is exactly a local preset. */
	linked = obs_data_get_bool(data, "linked");

	OBSDataAutoRelease styleData = obs_data_get_obj(data, "style");
	if (styleData)
		style.load(styleData);
}

void LogoPlayback::save(obs_data_t *data) const
{
	obs_data_set_bool(data, "loop", loop);
	obs_data_set_bool(data, "start_on_enter", startOnEnter);
	obs_data_set_double(data, "speed", speed);
	obs_data_set_bool(data, "animated_shadow", animatedShadow);
}

void LogoPlayback::load(obs_data_t *data)
{
	/*
	 * Defaulted rather than read straight, so a logo saved before any of this existed loads as
	 * a looping animation at its native rate rather than as one frozen on its first frame.
	 */
	obs_data_set_default_bool(data, "loop", true);
	obs_data_set_default_double(data, "speed", 1.0);

	loop = obs_data_get_bool(data, "loop");
	startOnEnter = obs_data_get_bool(data, "start_on_enter");
	speed = std::clamp(obs_data_get_double(data, "speed"), kMinLogoSpeed, kMaxLogoSpeed);
	animatedShadow = obs_data_get_bool(data, "animated_shadow");
}

void LogoRef::save(obs_data_t *data) const
{
	obs_data_set_string(data, "path", path.toUtf8().constData());
	obs_data_set_int(data, "max_height", maxHeight);

	OBSDataAutoRelease playbackData = obs_data_create();
	playback.save(playbackData);
	obs_data_set_obj(data, "playback", playbackData);
}

void LogoRef::load(obs_data_t *data)
{
	path = QString::fromUtf8(obs_data_get_string(data, "path"));
	maxHeight = static_cast<int>(obs_data_get_int(data, "max_height"));
	if (maxHeight <= 0)
		maxHeight = 96;

	playback = LogoPlayback();
	OBSDataAutoRelease playbackData = obs_data_get_obj(data, "playback");
	if (playbackData)
		playback.load(playbackData);
}

void Entry::save(obs_data_t *data) const
{
	obs_data_set_string(data, "text", text.toUtf8().constData());
	obs_data_set_string(data, "secondary_text", secondaryText.toUtf8().constData());
	obs_data_set_string(data, "subtitle", subtitle.toUtf8().constData());
	obs_data_set_string(data, "secondary_subtitle", secondarySubtitle.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);
}

void Entry::load(obs_data_t *data)
{
	text = QString::fromUtf8(obs_data_get_string(data, "text"));
	secondaryText = QString::fromUtf8(obs_data_get_string(data, "secondary_text"));
	/* Absent in every entry written before bridged rows could carry them, and an absent
	 * subtitle is an empty one: nothing is drawn and nothing has to be migrated. */
	subtitle = QString::fromUtf8(obs_data_get_string(data, "subtitle"));
	secondarySubtitle = QString::fromUtf8(obs_data_get_string(data, "secondary_subtitle"));

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		logo.load(logoData);
}

void DividerPiece::save(obs_data_t *data) const
{
	obs_data_set_string(data, "kind", dividerPieceKindId(kind));
	obs_data_set_string(data, "shape", dividerShapeId(shape));
	obs_data_set_string(data, "svg", svgPath.toUtf8().constData());
	obs_data_set_double(data, "scale", scale);
	obs_data_set_string(data, "text", text.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);
}

void DividerPiece::load(obs_data_t *data)
{
	kind = dividerPieceKindFromId(obs_data_get_string(data, "kind"), DividerPiece::Kind::Ornament);
	shape = dividerShapeFromId(obs_data_get_string(data, "shape"), DividerShape::Diamond);
	svgPath = QString::fromUtf8(obs_data_get_string(data, "svg"));
	/* A scale of zero draws nothing at all, which no stored piece ever means. */
	scale = obs_data_get_double(data, "scale");
	if (scale <= 0.0)
		scale = 1.0;
	text = QString::fromUtf8(obs_data_get_string(data, "text"));

	OBSDataAutoRelease logoData = obs_data_get_obj(data, "logo");
	if (logoData)
		logo.load(logoData);
}

void Section::save(obs_data_t *data) const
{
	obs_data_set_string(data, "type", sectionTypeId(type));
	obs_data_set_string(data, "label", label.toUtf8().constData());
	obs_data_set_string(data, "text", text.toUtf8().constData());
	obs_data_set_string(data, "secondary_text", secondaryText.toUtf8().constData());
	obs_data_set_string(data, "logo_side", logoSideId(logoSide));
	obs_data_set_string(data, "logo_placement", logoPlacementId(logoPlacement));
	obs_data_set_int(data, "logo_gap", logoGap);
	obs_data_set_string(data, "bridge_type", bridgeTypeId(bridgeType));
	obs_data_set_string(data, "bridge", bridge.toUtf8().constData());
	obs_data_set_string(data, "bridge_svg", bridgeSvg.toUtf8().constData());
	obs_data_set_double(data, "bridge_thickness", bridgeThickness);
	obs_data_set_double(data, "bridge_offset", bridgeOffset);
	obs_data_set_double(data, "bridge_gap", bridgeGap);
	obs_data_set_double(data, "bridge_min_gap", bridgeMinGap);
	obs_data_set_bool(data, "bridge_tint", bridgeTint);
	obs_data_set_string(data, "bridge_fill", bridgeFillId(bridgeFill));
	obs_data_set_string(data, "bridge_sizing", bridgeSizingId(bridgeSizing));
	obs_data_set_double(data, "bridge_split", bridgeSplit);
	obs_data_set_string(data, "bridge_row_align", hAlignId(bridgeRowAlign));
	obs_data_set_bool(data, "bridge_span_empty", bridgeSpanEmpty);
	obs_data_set_bool(data, "row_subtitles", rowSubtitles);
	obs_data_set_bool(data, "divider_mirror_ends", dividerMirrorEnds);
	obs_data_set_string(data, "divider_arm", dividerShapeId(dividerArm));
	obs_data_set_string(data, "divider_arm_svg", dividerArmSvg.toUtf8().constData());
	obs_data_set_double(data, "divider_thickness", dividerThickness);
	obs_data_set_double(data, "divider_gap", dividerGap);
	obs_data_set_double(data, "divider_piece_gap", dividerPieceGap);
	obs_data_set_int(data, "divider_rules", dividerRules);
	obs_data_set_double(data, "divider_rule_gap", dividerRuleGap);
	obs_data_set_double(data, "divider_rule_inset", dividerRuleInset);
	obs_data_set_bool(data, "divider_tint", dividerTint);
	obs_data_set_int(data, "columns", columns);
	obs_data_set_int(data, "column_gap", columnGap);
	obs_data_set_int(data, "entry_gap", entryGap);
	obs_data_set_int(data, "subtitle_gap", subtitleGap);
	obs_data_set_bool(data, "subtitle_first", subtitleFirst);
	obs_data_set_bool(data, "fill_across", fillAcross);
	obs_data_set_int(data, "padding_top", paddingTop);
	obs_data_set_int(data, "padding_bottom", paddingBottom);
	obs_data_set_int(data, "margin_x", marginX);
	obs_data_set_double(data, "section_width", sectionWidth);
	obs_data_set_string(data, "section_align", hAlignId(sectionAlign));
	obs_data_set_int(data, "spacer_height", spacerHeight);
	obs_data_set_string(data, "sticky_anchor", stickyAnchorId(stickyAnchor));
	obs_data_set_double(data, "sticky_canvas_position", stickyCanvasPosition);
	obs_data_set_double(data, "sticky_offset", stickyOffset);
	obs_data_set_double(data, "sticky_hold", stickyHold);
	obs_data_set_bool(data, "sticky_hold_forever", stickyHoldForever);
	obs_data_set_string(data, "sticky_release", stickyReleaseId(stickyRelease));
	obs_data_set_bool(data, "sticky_backdrop", stickyBackdrop);
	obs_data_set_int(data, "sticky_backdrop_color", static_cast<long long>(stickyBackdropColor.rgba()));
	obs_data_set_double(data, "sticky_backdrop_padding", stickyBackdropPadding);
	obs_data_set_bool(data, "visible", visible);
	obs_data_set_bool(data, "use_secondary_style", useSecondaryStyle);
	obs_data_set_bool(data, "use_bridge_style", useBridgeStyle);
	obs_data_set_string(data, "style_preset", stylePresetName.toUtf8().constData());
	obs_data_set_string(data, "secondary_style_preset", secondaryStylePresetName.toUtf8().constData());
	obs_data_set_string(data, "bridge_style_preset", bridgeStylePresetName.toUtf8().constData());
	obs_data_set_string(data, "row_subtitle_style_preset", rowSubtitleStylePresetName.toUtf8().constData());
	obs_data_set_string(data, "row_secondary_subtitle_style_preset",
			    rowSecondarySubtitleStylePresetName.toUtf8().constData());

	OBSDataAutoRelease logoData = obs_data_create();
	logo.save(logoData);
	obs_data_set_obj(data, "logo", logoData);

	OBSDataAutoRelease styleData = obs_data_create();
	style.save(styleData);
	obs_data_set_obj(data, "style", styleData);

	OBSDataAutoRelease secondaryData = obs_data_create();
	secondaryStyle.save(secondaryData);
	obs_data_set_obj(data, "secondary_style", secondaryData);

	OBSDataAutoRelease bridgeStyleData = obs_data_create();
	bridgeStyle.save(bridgeStyleData);
	obs_data_set_obj(data, "bridge_style", bridgeStyleData);

	OBSDataAutoRelease rowSubtitleStyleData = obs_data_create();
	rowSubtitleStyle.save(rowSubtitleStyleData);
	obs_data_set_obj(data, "row_subtitle_style", rowSubtitleStyleData);

	OBSDataAutoRelease rowSecondarySubtitleStyleData = obs_data_create();
	rowSecondarySubtitleStyle.save(rowSecondarySubtitleStyleData);
	obs_data_set_obj(data, "row_secondary_subtitle_style", rowSecondarySubtitleStyleData);

	saveArray(
		data, "entries", entries.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<Entry> *>(context)->at(index).save(item);
		},
		&entries);

	const auto savePieces = [data](const char *key, const QVector<DividerPiece> &pieces) {
		saveArray(
			data, key, pieces.size(),
			[](obs_data_t *item, int index, const void *context) {
				static_cast<const QVector<DividerPiece> *>(context)->at(index).save(item);
			},
			&pieces);
	};

	savePieces("divider_centre", dividerCentre);
	/*
	 * Under keys of their own rather than the "divider_cap"/"divider_end_cap" a single shape was
	 * written to: those still have to be readable as what they were, and a key that is a string
	 * in one document and an array in the next is a trap for every reader of either.
	 */
	savePieces("divider_cap_pieces", dividerCap);
	savePieces("divider_end_cap_pieces", dividerEndCap);

	/*
	 * A sticky block's children are sections, saved by the very same call that saved this one.
	 * Only one level deep can ever be written, because only one level can ever be held.
	 *
	 * Written only when there are any: every other section in every document would otherwise
	 * carry an empty array it has no use for, in settings that are rewritten on every save and
	 * read back on every load.
	 */
	if (!children.empty()) {
		saveArray(
			data, "children", static_cast<int>(children.size()),
			[](obs_data_t *item, int index, const void *context) {
				static_cast<const std::vector<Section> *>(context)
					->at(static_cast<size_t>(index))
					.save(item);
			},
			&children);
	}
}

void Section::load(obs_data_t *data)
{
	type = sectionTypeFromId(obs_data_get_string(data, "type"), SectionType::Title);
	label = QString::fromUtf8(obs_data_get_string(data, "label"));
	text = QString::fromUtf8(obs_data_get_string(data, "text"));
	/* Absent in every document written before the subtitle headings existed, none of which had a
	 * second line to carry -- and an empty one draws nothing, so there is no fallback to pick. */
	secondaryText = QString::fromUtf8(obs_data_get_string(data, "secondary_text"));
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
	/*
	 * Nor is a zero minimum gap absurd -- two columns set hard against each other is a layout,
	 * if an unusual one -- so this is read the same careful way, and a document written before
	 * the empty bridge existed gets the default rather than a gap of nothing.
	 */
	bridgeMinGap = obs_data_has_user_value(data, "bridge_min_gap") ? obs_data_get_double(data, "bridge_min_gap")
								      : 24.0;
	bridgeMinGap = std::max(0.0, bridgeMinGap);
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
	/* Off in every document written before a bridged row could carry subtitles, which is also
	 * what an entry with nothing in either subtitle draws. */
	rowSubtitles = obs_data_get_bool(data, "row_subtitles");

	/*
	 * 0.0 is a legitimate split -- everything to the right of the bridge -- so a missing
	 * key has to be told apart from a stored zero rather than inferred from the value.
	 */
	bridgeSplit = obs_data_has_user_value(data, "bridge_split") ? obs_data_get_double(data, "bridge_split") : 0.5;
	bridgeSplit = std::clamp(bridgeSplit, 0.0, 1.0);

	/*
	 * None is the fallback for every slot a document does not carry, so a section that predates
	 * dividers -- or a divider saved before a shape existed -- loads as the plainest thing the
	 * library can draw rather than as whatever happens to sit first in the table. The arm is the
	 * exception: an arm of None is a divider with no rule in it at all, which is never what a
	 * missing key means, so that one falls back to the plain rule.
	 */
	dividerMirrorEnds = obs_data_has_user_value(data, "divider_mirror_ends")
				    ? obs_data_get_bool(data, "divider_mirror_ends")
				    : true;
	dividerArm = dividerShapeFromId(obs_data_get_string(data, "divider_arm"), DividerShape::Rule);
	dividerArmSvg = QString::fromUtf8(obs_data_get_string(data, "divider_arm_svg"));

	/* A thickness of zero would draw nothing at all, which no document ever means. */
	dividerThickness = obs_data_get_double(data, "divider_thickness");
	if (dividerThickness <= 0.0)
		dividerThickness = 4.0;

	/*
	 * 0 is a legitimate gap for both of these -- a cap butted against its arm, a run of
	 * ornaments touching -- so a missing key has to be told apart from a stored zero rather
	 * than inferred from the value.
	 */
	dividerGap = obs_data_has_user_value(data, "divider_gap") ? obs_data_get_double(data, "divider_gap") : 12.0;
	dividerPieceGap = obs_data_has_user_value(data, "divider_piece_gap")
				  ? obs_data_get_double(data, "divider_piece_gap")
				  : 10.0;
	dividerRuleGap =
		obs_data_has_user_value(data, "divider_rule_gap") ? obs_data_get_double(data, "divider_rule_gap") : 6.0;
	dividerRules = static_cast<int>(obs_data_get_int(data, "divider_rules"));
	dividerRuleInset = obs_data_get_double(data, "divider_rule_inset");
	/* Absent in documents that predate custom art, whose built-in shapes are tinted regardless. */
	dividerTint = obs_data_has_user_value(data, "divider_tint") ? obs_data_get_bool(data, "divider_tint") : true;

	columns = static_cast<int>(obs_data_get_int(data, "columns"));
	columnGap = static_cast<int>(obs_data_get_int(data, "column_gap"));
	entryGap = static_cast<int>(obs_data_get_int(data, "entry_gap"));
	/*
	 * 0 is a legitimate gap -- a subtitle set tight under its title -- so a missing key has to
	 * be told apart from a stored zero rather than inferred from the value.
	 */
	subtitleGap = obs_data_has_user_value(data, "subtitle_gap")
			      ? static_cast<int>(obs_data_get_int(data, "subtitle_gap"))
			      : 4;
	subtitleFirst = obs_data_get_bool(data, "subtitle_first");
	fillAcross = obs_data_get_bool(data, "fill_across");
	paddingTop = static_cast<int>(obs_data_get_int(data, "padding_top"));
	paddingBottom = static_cast<int>(obs_data_get_int(data, "padding_bottom"));
	marginX = static_cast<int>(obs_data_get_int(data, "margin_x"));
	/*
	 * Absent in every document written before the section box existed, all of which were laid
	 * out across the full canvas width. A stored 0 would collapse the section to nothing, so a
	 * missing key has to be told apart from one rather than inferred from the value.
	 */
	sectionWidth = obs_data_has_user_value(data, "section_width") ? obs_data_get_double(data, "section_width")
								      : 1.0;
	sectionWidth = std::clamp(sectionWidth, 0.0, 1.0);
	sectionAlign = hAlignFromId(obs_data_get_string(data, "section_align"), HAlign::Center);
	spacerHeight = static_cast<int>(obs_data_get_int(data, "spacer_height"));
	stickyAnchor = stickyAnchorFromId(obs_data_get_string(data, "sticky_anchor"), StickyAnchor::Center);
	/*
	 * 0.0 is the top of the canvas and a perfectly ordinary place to pin something, so a missing
	 * key has to be told apart from a stored zero here as everywhere else.
	 */
	stickyCanvasPosition = obs_data_has_user_value(data, "sticky_canvas_position")
				       ? obs_data_get_double(data, "sticky_canvas_position")
				       : 0.5;
	stickyCanvasPosition = std::clamp(stickyCanvasPosition, 0.0, 1.0);
	stickyOffset = obs_data_get_double(data, "sticky_offset");
	stickyHold = obs_data_has_user_value(data, "sticky_hold") ? obs_data_get_double(data, "sticky_hold") : 5.0;
	stickyHold = std::max(0.0, stickyHold);
	stickyHoldForever = obs_data_get_bool(data, "sticky_hold_forever");
	stickyRelease = stickyReleaseFromId(obs_data_get_string(data, "sticky_release"), StickyRelease::EndAtHold);
	stickyBackdrop = obs_data_get_bool(data, "sticky_backdrop");
	if (obs_data_has_user_value(data, "sticky_backdrop_color")) {
		stickyBackdropColor = QColor::fromRgba(
			static_cast<QRgb>(obs_data_get_int(data, "sticky_backdrop_color")));
	}
	stickyBackdropPadding = obs_data_has_user_value(data, "sticky_backdrop_padding")
					? obs_data_get_double(data, "sticky_backdrop_padding")
					: 24.0;
	stickyBackdropPadding = std::max(0.0, stickyBackdropPadding);
	visible = obs_data_get_bool(data, "visible");
	useSecondaryStyle = obs_data_get_bool(data, "use_secondary_style");
	/* Absent in every document written before the bridge had ink of its own, all of which took the row's. */
	useBridgeStyle = obs_data_get_bool(data, "use_bridge_style");
	stylePresetName = QString::fromUtf8(obs_data_get_string(data, "style_preset"));
	secondaryStylePresetName = QString::fromUtf8(obs_data_get_string(data, "secondary_style_preset"));
	bridgeStylePresetName = QString::fromUtf8(obs_data_get_string(data, "bridge_style_preset"));
	rowSubtitleStylePresetName = QString::fromUtf8(obs_data_get_string(data, "row_subtitle_style_preset"));
	rowSecondarySubtitleStylePresetName =
		QString::fromUtf8(obs_data_get_string(data, "row_secondary_subtitle_style_preset"));

	if (columns < 1)
		columns = 1;
	/* One rule is a divider; none is nothing at all, and the stack is bounded for the same
	 * reason the tile runs are -- a rule count read off a file decides how much gets drawn. */
	dividerRules = std::clamp(dividerRules, 1, 16);
	dividerGap = std::max(0.0, dividerGap);
	dividerPieceGap = std::max(0.0, dividerPieceGap);
	dividerRuleGap = std::max(0.0, dividerRuleGap);
	dividerRuleInset = std::max(0.0, dividerRuleInset);
	if (spacerHeight < 0)
		spacerHeight = 0;
	if (entryGap < 0)
		entryGap = 0;
	if (subtitleGap < 0)
		subtitleGap = 0;

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

	/*
	 * Seeded from the row's own style when absent, so switching the override on starts from the
	 * leader as it is drawn now and the first edit is the one the user meant to make. Only the
	 * ink of it is ever read (see Document::effectiveBridgeStyle); the font it carries along is
	 * what keeps a preset saved from this editor a whole style rather than a half of one.
	 */
	OBSDataAutoRelease bridgeStyleData = obs_data_get_obj(data, "bridge_style");
	if (bridgeStyleData)
		bridgeStyle.load(bridgeStyleData);
	else
		bridgeStyle = style;

	/*
	 * A subtitle style absent from the document is seeded the way a new section's is: the line
	 * it sits under, a size down. Falling back to the line's own style outright would draw a
	 * subtitle indistinguishable from the text above it, which is the one thing a subtitle must
	 * not be -- and a document that predates these has no opinion to preserve, since it carried
	 * no subtitles for them to be wrong about.
	 */
	const auto seedSubtitleStyle = [](const TextStyle &from) {
		TextStyle seeded = from;
		seeded.pixelSize = std::max(1, static_cast<int>(std::lround(from.pixelSize * 0.7)));
		seeded.bold = false;
		return seeded;
	};

	OBSDataAutoRelease rowSubtitleStyleData = obs_data_get_obj(data, "row_subtitle_style");
	if (rowSubtitleStyleData)
		rowSubtitleStyle.load(rowSubtitleStyleData);
	else
		rowSubtitleStyle = seedSubtitleStyle(style);

	OBSDataAutoRelease rowSecondarySubtitleStyleData = obs_data_get_obj(data, "row_secondary_subtitle_style");
	if (rowSecondarySubtitleStyleData)
		rowSecondarySubtitleStyle.load(rowSecondarySubtitleStyleData);
	else
		rowSecondarySubtitleStyle = seedSubtitleStyle(useSecondaryStyle ? secondaryStyle : style);

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

	const auto loadPieces = [data](const char *key, QVector<DividerPiece> *into) {
		into->clear();

		OBSDataArrayAutoRelease array = obs_data_get_array(data, key);
		if (!array)
			return false;

		const size_t count = obs_data_array_count(array);
		into->reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(array, i);
			DividerPiece piece;
			piece.load(item);
			into->append(piece);
		}
		return true;
	};

	loadPieces("divider_centre", &dividerCentre);

	/*
	 * An end written as a single shape -- every document from before an end was a stack -- is
	 * read back as the one-piece stack it is. None is the fallback there for the reason it is
	 * everywhere else in a divider, and a `None` end is simply an end with nothing on it, so it
	 * migrates to an empty list rather than to a piece that draws nothing.
	 */
	const auto loadEnd = [&](const char *key, const char *legacyShape, const char *legacyFile,
				 QVector<DividerPiece> *into) {
		if (loadPieces(key, into))
			return;

		const DividerShape shape =
			dividerShapeFromId(obs_data_get_string(data, legacyShape), DividerShape::None);
		if (dividerShapeIsEmpty(shape))
			return;

		DividerPiece piece;
		piece.kind = DividerPiece::Kind::Ornament;
		piece.shape = shape;
		piece.svgPath = QString::fromUtf8(obs_data_get_string(data, legacyFile));
		into->append(piece);
	};

	loadEnd("divider_cap_pieces", "divider_cap", "divider_cap_svg", &dividerCap);
	loadEnd("divider_end_cap_pieces", "divider_end_cap", "divider_end_cap_svg", &dividerEndCap);

	children.clear();
	OBSDataArrayAutoRelease childArray = obs_data_get_array(data, "children");
	if (childArray) {
		const size_t count = obs_data_array_count(childArray);
		children.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(childArray, i);

			Section child;
			child.load(item);
			/*
			 * A sticky block inside a sticky block is not something the designer can
			 * make, and is dropped rather than loaded: everything downstream -- the
			 * layout, the playback, the pinned quad -- is written for one level, and a
			 * hand-written document is not a reason to find out what two would do.
			 */
			if (child.type == SectionType::StickyBlock)
				continue;

			children.push_back(child);
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

	case SectionType::TitleWithSubtitle:
	case SectionType::TitleWithSubtitleAndLogo:
	case SectionType::HeaderWithSubtitle:
	case SectionType::HeaderWithSubtitleAndLogo: {
		/*
		 * The heading's own defaults, with a second line under it set at roughly half the size
		 * and without the weight. The difference between the two lines is the whole point of the
		 * type -- a pair drawn in one style at one size is two headings that happen to touch --
		 * so the secondary style is turned on here rather than left for the user to find.
		 */
		const bool title = type == SectionType::TitleWithSubtitle ||
				   type == SectionType::TitleWithSubtitleAndLogo;

		section.text = title ? QStringLiteral("Title") : QStringLiteral("Header");
		section.secondaryText = QStringLiteral("Subtitle");
		section.style.pixelSize = title ? 72 : 44;
		section.style.bold = true;
		section.secondaryStyle = section.style;
		section.secondaryStyle.pixelSize = title ? 36 : 26;
		section.secondaryStyle.bold = false;
		section.useSecondaryStyle = true;
		/*
		 * Wider than the 4 px a list entry starts at, because a heading's two lines are set
		 * larger than a list's and the same absolute gap under them reads as the pair being
		 * crushed together rather than as one item.
		 */
		section.subtitleGap = 8;
		section.paddingTop = title ? 48 : 32;
		section.paddingBottom = title ? 32 : 16;
		break;
	}

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
		section.entries.append(Entry{QStringLiteral("Role"), QStringLiteral("Name"), {}, {}, {}});
		break;

	case SectionType::TextList:
		section.style.pixelSize = 32;
		section.entries.append(Entry{QStringLiteral("Name"), {}, {}, {}, {}});
		break;

	case SectionType::TitleSubtitleList:
	case SectionType::MultiTitleSubtitleList: {
		/*
		 * The subtitle is handed its own style set a size down from the title, and turned
		 * on, because a pair drawn in one style is a Text List with twice as many rows: it
		 * is the difference between the two lines that says which is which. Both are the
		 * section's own styles rather than presets, so either can be retyped or bound to a
		 * preset without disturbing the other.
		 */
		section.style.pixelSize = 32;
		section.style.bold = true;
		section.secondaryStyle = section.style;
		section.secondaryStyle.pixelSize = 26;
		section.secondaryStyle.bold = false;
		section.useSecondaryStyle = true;
		/*
		 * Wider than the default so the space between one pair and the next reads as larger
		 * than the space inside a pair, which is what groups the two lines together.
		 */
		section.entryGap = 24;

		const int count = type == SectionType::MultiTitleSubtitleList ? 3 : 1;
		if (type == SectionType::MultiTitleSubtitleList) {
			section.columns = 3;
			section.marginX = 120;
		}
		for (int i = 0; i < count; ++i)
			section.entries.append(Entry{QStringLiteral("Position"), QStringLiteral("Full Name"), {}, {}, {}});
		break;
	}

	case SectionType::LogoList:
		section.entries.append(Entry{});
		break;

	case SectionType::MultiTextList:
		section.style.pixelSize = 32;
		section.columns = 3;
		section.marginX = 120;
		for (int i = 0; i < 3; ++i)
			section.entries.append(Entry{QStringLiteral("Name"), {}, {}, {}, {}});
		break;

	case SectionType::MultiLogoList:
		section.columns = 3;
		section.marginX = 120;
		for (int i = 0; i < 3; ++i)
			section.entries.append(Entry{});
		break;

	case SectionType::SectionDivider:
		/*
		 * The arrow rule: a plain bar between two arrowheads, broken in the middle by a
		 * diamond with a dot either side of it. Deliberately a compound rather than a
		 * single ornament, because the first thing anyone does with a new divider is take a
		 * piece out or put one in, and starting from three shows that the centre is a list.
		 */
		section.dividerCap = {DividerPiece{DividerPiece::Kind::Ornament, DividerShape::Arrow, {}, 1.0, {}, {}}};
		section.dividerArm = DividerShape::Rule;
		section.dividerThickness = 5.0;
		section.dividerCentre.append(
			DividerPiece{DividerPiece::Kind::Ornament, DividerShape::Dot, {}, 1.0, {}, {}});
		section.dividerCentre.append(
			DividerPiece{DividerPiece::Kind::Ornament, DividerShape::Diamond, {}, 1.0, {}, {}});
		section.dividerCentre.append(
			DividerPiece{DividerPiece::Kind::Ornament, DividerShape::Dot, {}, 1.0, {}, {}});
		/*
		 * Narrower than the canvas and generously padded, because a divider that runs edge to
		 * edge and sits tight against the text above it reads as a border rather than as a
		 * break in the roll.
		 */
		section.sectionWidth = 0.6;
		section.paddingTop = 28;
		section.paddingBottom = 28;
		break;

	case SectionType::Spacer:
		section.paddingTop = 0;
		section.paddingBottom = 0;
		break;

	case SectionType::StickyBlock:
		/*
		 * A closing card to start from, because an empty block is invisible in the preview and
		 * gives the reader nothing to drag other sections next to. Held in the middle of the
		 * frame for five seconds and then treated as the end of the roll, which is the thing
		 * the type is named after; everything else about it is one control away.
		 */
		section.children.push_back(makeDefault(SectionType::Title));
		section.children.front().text = QStringLiteral("The End");
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

	/*
	 * The two subtitles a bridged row can carry are seeded from the line each of them sits
	 * under, a size down and without its weight, whatever the type -- switching a section to
	 * Bridged and turning the subtitles on then draws something that reads as a subtitle
	 * straight away, rather than a second line indistinguishable from the first.
	 *
	 * Done here for every type rather than in the Bridged arm because changing a section's type
	 * is non-destructive: the fields a type does not read are kept, so they had better be worth
	 * keeping by the time another type does read them.
	 */
	const auto subtitleOf = [](const TextStyle &line) {
		TextStyle subtitle = line;
		subtitle.pixelSize = std::max(1, static_cast<int>(std::lround(line.pixelSize * 0.7)));
		subtitle.bold = false;
		return subtitle;
	};
	section.rowSubtitleStyle = subtitleOf(section.style);
	section.rowSecondarySubtitleStyle =
		subtitleOf(section.useSecondaryStyle ? section.secondaryStyle : section.style);

	return section;
}

QString Section::displayLabel() const
{
	if (!label.isEmpty())
		return label;

	switch (type) {
	case SectionType::Title:
	case SectionType::TitleWithSubtitle:
	case SectionType::TitleWithLogo:
	case SectionType::TitleWithSubtitleAndLogo:
	case SectionType::Header:
	case SectionType::HeaderWithSubtitle:
	case SectionType::HeaderWithLogo:
	case SectionType::HeaderWithSubtitleAndLogo:
		/*
		 * The title names the section even when a subtitle is set: a list of headings reads by
		 * the line the eye lands on, and falling back to the subtitle would put the smaller of
		 * the two lines in the list for a heading whose title is still to be typed.
		 */
		return text.isEmpty() ? QString::fromUtf8(sectionTypeName(type)) : text;

	case SectionType::LogoTitle:
	case SectionType::LogoHeader:
		return logo.isEmpty() ? QString::fromUtf8(sectionTypeName(type))
				      : QFileInfo(logo.path).completeBaseName();

	case SectionType::SectionDivider: {
		/*
		 * A labelled break names itself: the one thing in a divider a user can pick out of a
		 * list at a glance is a word they typed into it. Everything else falls back to the
		 * type and the rule it is drawn from, which at least tells two dividers apart.
		 */
		for (const DividerPiece &piece : dividerCentre) {
			if (piece.kind == DividerPiece::Kind::Text && !piece.text.isEmpty())
				return piece.text;
		}
		return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(sectionTypeName(type)),
						     QString::fromUtf8(dividerShapeName(dividerArm)));
	}

	case SectionType::Spacer:
		return QStringLiteral("%1 (%2 px)").arg(QString::fromUtf8(sectionTypeName(type))).arg(spacerHeight);

	case SectionType::StickyBlock:
		/*
		 * Named by what it holds rather than by how long it holds for: the list is a list of
		 * content, and the children are indented under it saying the same thing in more detail.
		 */
		return QStringLiteral("%1 (%2)")
			.arg(QString::fromUtf8(sectionTypeName(type)))
			.arg(static_cast<int>(children.size()));

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

const TextStyle &Document::effectiveRowSubtitleStyle(const Section &section) const
{
	const TextStyle *preset = findStylePreset(section.rowSubtitleStylePresetName);
	return preset ? *preset : section.rowSubtitleStyle;
}

const TextStyle &Document::effectiveRowSecondarySubtitleStyle(const Section &section) const
{
	const TextStyle *preset = findStylePreset(section.rowSecondarySubtitleStylePresetName);
	return preset ? *preset : section.rowSecondarySubtitleStyle;
}

TextStyle Document::effectiveBridgeStyle(const Section &section) const
{
	TextStyle style = effectiveStyle(section);
	if (!section.useBridgeStyle)
		return style;

	const TextStyle *preset = findStylePreset(section.bridgeStylePresetName);
	const TextStyle &ink = preset ? *preset : section.bridgeStyle;

	/*
	 * Ink only. Everything the layout measures with -- family, size, weight, alignment, line
	 * spacing -- is left as the row's, so a bridge that has been recoloured occupies exactly the
	 * space it did before and nothing else in the row moves for it. It also means a preset
	 * written for a run of headings can be pointed at a leader without dragging a heading's font
	 * size across with it.
	 */
	style.color = ink.color;
	style.fill = ink.fill;
	style.gradient = ink.gradient;
	style.outline = ink.outline;
	style.shadow = ink.shadow;

	return style;
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

	stylePresets.append(StylePreset{name, style, false});
}

bool Document::linkStylePreset(const QString &name)
{
	TextStyle style;
	if (name.isEmpty() || !StyleLibrary::instance().find(name, &style))
		return false;

	for (StylePreset &preset : stylePresets) {
		if (preset.name != name)
			continue;

		preset.style = style;
		preset.linked = true;
		return true;
	}

	stylePresets.append(StylePreset{name, style, true});
	return true;
}

bool Document::applyLibraryRenames()
{
	const StyleLibrary &library = StyleLibrary::instance();
	bool changed = false;

	for (StylePreset &preset : stylePresets) {
		if (!preset.linked)
			continue;

		QString renamed;
		if (!library.renamedTo(preset.name, &renamed))
			continue;

		/* The rename is only worth following to a preset that is actually there to follow. */
		if (!library.contains(renamed))
			continue;

		const QString from = preset.name;
		const bool taken = std::any_of(stylePresets.cbegin(), stylePresets.cend(),
					       [&renamed](const StylePreset &other) { return other.name == renamed; });
		if (taken)
			continue;

		preset.name = renamed;

		/* Every binding that named it, or the roll would follow the rename into nothing. */
		visitSections(sections, [&](Section &section) {
			if (section.stylePresetName == from)
				section.stylePresetName = renamed;
			if (section.secondaryStylePresetName == from)
				section.secondaryStylePresetName = renamed;
			if (section.bridgeStylePresetName == from)
				section.bridgeStylePresetName = renamed;
			if (section.rowSubtitleStylePresetName == from)
				section.rowSubtitleStylePresetName = renamed;
			if (section.rowSecondarySubtitleStylePresetName == from)
				section.rowSecondarySubtitleStylePresetName = renamed;
		});

		changed = true;
	}

	return changed;
}

bool Document::refreshLinkedPresets()
{
	const StyleLibrary &library = StyleLibrary::instance();
	/*
	 * Renames first: a preset that has been renamed has to be found under its new name before
	 * there is any point asking the library what style is under it.
	 */
	bool changed = applyLibraryRenames();

	for (StylePreset &preset : stylePresets) {
		if (!preset.linked)
			continue;

		TextStyle style;
		if (!library.find(preset.name, &style))
			continue;

		if (style == preset.style)
			continue;

		preset.style = style;
		changed = true;
	}

	return changed;
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
	visitSections(sections, [&name](Section &section) {
		if (section.stylePresetName == name)
			section.stylePresetName.clear();
		if (section.secondaryStylePresetName == name)
			section.secondaryStylePresetName.clear();
		if (section.bridgeStylePresetName == name)
			section.bridgeStylePresetName.clear();
		if (section.rowSubtitleStylePresetName == name)
			section.rowSubtitleStylePresetName.clear();
		if (section.rowSecondarySubtitleStylePresetName == name)
			section.rowSecondarySubtitleStylePresetName.clear();
	});
}

QStringList Document::usedFontFamilies() const
{
	QStringList families;
	for (const FontUse &use : usedFonts())
		families.append(use.family);

	return families;
}

QVector<FontUse> Document::usedFonts() const
{
	QVector<FontUse> fonts;

	const auto consider = [&fonts](const TextStyle &style) {
		if (style.family.isEmpty())
			return;

		for (FontUse &use : fonts) {
			if (use.family != style.family)
				continue;

			if (!use.styleNames.contains(style.styleName))
				use.styleNames.append(style.styleName);
			return;
		}

		fonts.append(FontUse{style.family, {style.styleName}});
	};

	visitSections(sections, [&](const Section &section) {
		if (!section.visible)
			return;

		/*
		 * A divider draws text only when its centre stack holds some, so it is asked rather
		 * than assumed: reporting a font for a roll whose every divider is pure artwork
		 * would send the user hunting for a substitution that never happened.
		 */
		const auto holdsWord = [](const QVector<DividerPiece> &pieces) {
			return std::any_of(pieces.cbegin(), pieces.cend(), [](const DividerPiece &piece) {
				return piece.kind == DividerPiece::Kind::Text && !piece.text.isEmpty();
			});
		};

		/* Any of the three stacks can hold a word, so all three are asked. */
		const bool dividerText = section.type == SectionType::SectionDivider &&
					 (holdsWord(section.dividerCentre) || holdsWord(section.dividerCap) ||
					  holdsWord(section.dividerEndCap));

		if (!sectionUsesText(section.type) && !dividerText)
			return;

		consider(effectiveStyle(section));
		consider(effectiveSecondaryStyle(section));

		/*
		 * The subtitles of a bridged row are asked for the same reason a divider's text is:
		 * they are drawn only when the section turns them on, and a family reported for a
		 * roll that never draws it is a font hunt with nothing at the end of it.
		 */
		if (sectionStacksSubtitles(section) && section.type == SectionType::Bridged) {
			consider(effectiveRowSubtitleStyle(section));
			consider(effectiveRowSecondarySubtitleStyle(section));
		}
	});

	std::sort(fonts.begin(), fonts.end(), [](const FontUse &left, const FontUse &right) {
		return left.family.compare(right.family, Qt::CaseInsensitive) < 0;
	});

	for (FontUse &use : fonts)
		use.styleNames.sort(Qt::CaseInsensitive);

	return fonts;
}

QString Document::fontSubstitute(const QString &family) const
{
	for (const FontSubstitution &substitution : fontSubstitutions) {
		if (substitution.from.compare(family, Qt::CaseInsensitive) == 0)
			return substitution.to;
	}

	return QString();
}

void Document::setFontSubstitute(const QString &from, const QString &to)
{
	if (from.isEmpty())
		return;

	for (int i = 0; i < fontSubstitutions.size(); ++i) {
		if (fontSubstitutions.at(i).from.compare(from, Qt::CaseInsensitive) != 0)
			continue;

		if (to.isEmpty())
			fontSubstitutions.removeAt(i);
		else
			fontSubstitutions[i].to = to;
		return;
	}

	if (!to.isEmpty())
		fontSubstitutions.append(FontSubstitution{from, to});
}

bool Document::applyFontSubstitutions(const QStringList &families)
{
	if (fontSubstitutions.isEmpty() || families.isEmpty())
		return false;

	bool changed = false;

	const auto rewrite = [this, &families, &changed](TextStyle &style) {
		if (style.family.isEmpty() || !families.contains(style.family))
			return;

		const QString substitute = fontSubstitute(style.family);
		if (substitute.isEmpty() || substitute == style.family)
			return;

		style.family = substitute;
		changed = true;
	};

	for (StylePreset &preset : stylePresets)
		rewrite(preset.style);

	visitSections(sections, [&rewrite](Section &section) {
		rewrite(section.style);
		rewrite(section.secondaryStyle);
		rewrite(section.rowSubtitleStyle);
		rewrite(section.rowSecondarySubtitleStyle);
		/*
		 * `bridgeStyle` is deliberately left alone: a bridge keeps the row's own font and
		 * takes only ink from it, so the family recorded there is never drawn with.
		 */
	});

	return changed;
}

bool Document::refreshFontBundle(QStringList *skipped, bool recollect)
{
	const QVector<BundledFont> before = bundledFonts;

	if (!bundleFonts) {
		bundledFonts.clear();
		return !before.isEmpty();
	}

	QVector<BundledFont> kept;

	for (const FontUse &use : usedFonts()) {
		/*
		 * A family with a stand-in recorded is one the designer has already answered for, and
		 * on the machine that recorded it there was no file to find anyway.
		 */
		if (!fontSubstitute(use.family).isEmpty())
			continue;

		QVector<BundledFont> carried;
		for (const BundledFont &font : before) {
			if (font.family.compare(use.family, Qt::CaseInsensitive) == 0)
				carried.append(font);
		}

		/*
		 * Whether what is already carried covers every face the roll now names. Asked per face
		 * rather than per family, because setting one heading to the family's semibold changes
		 * nothing about the family and everything about which file has to travel: a bundle that
		 * stopped at "this family is carried" would leave that heading rendering in the nearest
		 * weight on every other machine, silently.
		 */
		bool complete = !carried.isEmpty();
		for (const QString &styleName : use.styleNames) {
			bool supplied = false;
			for (const BundledFont &font : carried)
				supplied = supplied || font.supplies(use.family, styleName);

			complete = complete && supplied;
		}

		/*
		 * Already carried, covering every face, and nobody asked for a fresh read: nothing to go
		 * to the disk for. This is the common case on every Apply after the first, and it is what
		 * keeps the machine's font directories out of the path of an ordinary edit.
		 */
		if (complete && !recollect) {
			kept += carried;
			continue;
		}

		const QVector<BundledFont> found = collectBundledFonts({use}, skipped);

		/*
		 * A file that cannot be found here does not un-carry the one already in hand. This is
		 * the machine that does not have the font -- the one the bundle exists for -- and
		 * dropping it because a local file could not be read would throw the roll's fonts away
		 * on the first edit made anywhere but home.
		 */
		kept += found.isEmpty() ? carried : found;
	}

	bundledFonts = kept;

	if (bundledFonts.size() != before.size())
		return true;

	for (int i = 0; i < bundledFonts.size(); ++i) {
		if (bundledFonts.at(i).family != before.at(i).family ||
		    bundledFonts.at(i).styleNames != before.at(i).styleNames ||
		    bundledFonts.at(i).data != before.at(i).data)
			return true;
	}

	return false;
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
	obs_data_set_bool(data, "manual_scroll", manualScroll);
	obs_data_set_double(data, "scroll_position", scrollPosition);

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

	obs_data_set_bool(data, "bundle_fonts", bundleFonts);

	saveArray(
		data, "bundled_fonts", bundledFonts.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<BundledFont> *>(context)->at(index).save(item);
		},
		&bundledFonts);

	saveArray(
		data, "font_substitutions", fontSubstitutions.size(),
		[](obs_data_t *item, int index, const void *context) {
			static_cast<const QVector<FontSubstitution> *>(context)->at(index).save(item);
		},
		&fontSubstitutions);
}

void Document::load(obs_data_t *data, bool *migrated)
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
	manualScroll = obs_data_get_bool(data, "manual_scroll");
	scrollPosition = std::clamp(obs_data_get_double(data, "scroll_position"), 0.0, 100.0);

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

	/*
	 * Off is a deliberate choice -- a roll whose fonts may not be passed on -- and a document
	 * written before fonts could travel has made no choice at all, so a missing key has to be
	 * told apart from a stored false rather than inferred from it.
	 */
	bundleFonts = obs_data_has_user_value(data, "bundle_fonts") ? obs_data_get_bool(data, "bundle_fonts") : true;

	bundledFonts.clear();
	OBSDataArrayAutoRelease fontArray = obs_data_get_array(data, "bundled_fonts");
	if (fontArray) {
		const size_t count = obs_data_array_count(fontArray);
		bundledFonts.reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(fontArray, i);
			BundledFont font;
			font.load(item);
			/* A font with no family names nothing and can resolve nothing. */
			if (!font.family.isEmpty() && !font.isEmpty())
				bundledFonts.append(font);
		}
	}

	fontSubstitutions.clear();
	OBSDataArrayAutoRelease substitutionArray = obs_data_get_array(data, "font_substitutions");
	if (substitutionArray) {
		const size_t count = obs_data_array_count(substitutionArray);
		fontSubstitutions.reserve(static_cast<int>(count));
		for (size_t i = 0; i < count; ++i) {
			OBSDataAutoRelease item = obs_data_array_item(substitutionArray, i);
			FontSubstitution substitution;
			substitution.load(item);
			/* Neither half is any use without the other. */
			if (!substitution.from.isEmpty() && !substitution.to.isEmpty())
				fontSubstitutions.append(substitution);
		}
	}

	/*
	 * A document arriving from a scene collection carries whatever the library held when it was
	 * last saved, under whatever names it held them under. Bringing it up to date here -- renames
	 * followed, copies refreshed -- means a roll is styled by the library from the first frame it
	 * draws, rather than by a copy that is however many edits out of date until something happens
	 * to poll. It is also the only chance a collection that has been closed for a year gets to
	 * follow a rename made while it was away.
	 */
	const bool changed = refreshLinkedPresets();
	if (migrated)
		*migrated = changed;
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
	obs_data_set_default_bool(data, "manual_scroll", false);
	obs_data_set_default_double(data, "scroll_position", 0.0);
	obs_data_set_default_bool(data, "bundle_fonts", true);

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
