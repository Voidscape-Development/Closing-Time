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

#include <obs.h>
#include <obs.hpp>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"

#include "model/CreditsModel.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* A section with every field pushed off its default, so a dropped key cannot pass unnoticed. */
Section distinctive(SectionType type)
{
	Section section = Section::makeDefault(type);

	section.label = QStringLiteral("label \xE2\x80\x94 with punctuation");
	section.text = QStringLiteral("primary");
	section.secondaryText = QStringLiteral("secondary");
	section.logo.path = QStringLiteral("/some/logo.png");
	section.logo.maxHeight = 137;
	section.logoSide = LogoSide::Right;
	section.logoPlacement = LogoPlacement::Bridged;
	section.logoGap = 41;
	section.entryGap = 13;
	section.subtitleGap = 17;
	section.subtitleFirst = true;
	section.bridgeType = BridgeType::Diamonds;
	section.bridge = QStringLiteral(" ~~~ ");
	section.bridgeThickness = 7.5;
	section.bridgeOffset = 3.5;
	section.bridgeGap = 11.5;
	section.bridgeTint = false;
	section.bridgeFill = BridgeFill::Stretch;
	section.bridgeSizing = BridgeSizing::Natural;
	section.bridgeSplit = 0.25;
	section.bridgeRowAlign = HAlign::Left;
	section.bridgeSpanEmpty = true;
	section.dividerCap = DividerShape::Diamond;
	section.dividerMirrorEnds = false;
	section.dividerEndCap = DividerShape::Dot;
	section.dividerArm = DividerShape::Rule;
	section.dividerThickness = 6.5;
	section.dividerGap = 15.5;
	section.dividerPieceGap = 9.5;
	section.dividerRules = 3;
	section.dividerRuleGap = 8.5;
	section.dividerRuleInset = 12.5;
	section.dividerTint = false;
	section.dividerCentre = {
		DividerPiece{DividerPiece::Kind::Text, DividerShape::Diamond, {}, 1.5, QStringLiteral("PART II"), {}}};
	section.columns = 4;
	section.columnGap = 37;
	section.fillAcross = true;
	section.useSecondaryStyle = true;
	section.useBridgeStyle = true;
	section.stylePresetName = QStringLiteral("preset a");
	section.secondaryStylePresetName = QStringLiteral("preset b");
	section.bridgeStylePresetName = QStringLiteral("preset c");
	section.paddingTop = 21;
	section.paddingBottom = 23;
	section.marginX = 29;
	section.sectionWidth = 0.75;
	section.sectionAlign = HAlign::Right;
	section.spacerHeight = 313;
	section.visible = false;
	section.entries = {Entry{QStringLiteral("one"), QStringLiteral("two"), LogoRef{QStringLiteral("/e.png"), 61}}};

	return section;
}

/* Every field the round trip has to preserve, compared one at a time so a failure names it. */
void compare(const Section &loaded, const Section &original)
{
	checkEq(loaded.label, original.label, "label");
	checkEq(loaded.text, original.text, "text");
	checkEq(loaded.secondaryText, original.secondaryText, "secondaryText");
	checkEq(loaded.logo.path, original.logo.path, "logo.path");
	checkEq(loaded.logo.maxHeight, original.logo.maxHeight, "logo.maxHeight");
	check(loaded.logoSide == original.logoSide, "logoSide");
	check(loaded.logoPlacement == original.logoPlacement, "logoPlacement");
	checkEq(loaded.logoGap, original.logoGap, "logoGap");
	checkEq(loaded.entryGap, original.entryGap, "entryGap");
	checkEq(loaded.subtitleGap, original.subtitleGap, "subtitleGap");
	check(loaded.subtitleFirst == original.subtitleFirst, "subtitleFirst");
	check(loaded.bridgeType == original.bridgeType, "bridgeType");
	checkEq(loaded.bridge, original.bridge, "bridge");
	checkNear(loaded.bridgeThickness, original.bridgeThickness, 0.001, "bridgeThickness");
	checkNear(loaded.bridgeOffset, original.bridgeOffset, 0.001, "bridgeOffset");
	checkNear(loaded.bridgeGap, original.bridgeGap, 0.001, "bridgeGap");
	check(loaded.bridgeTint == original.bridgeTint, "bridgeTint");
	check(loaded.bridgeFill == original.bridgeFill, "bridgeFill");
	check(loaded.bridgeSizing == original.bridgeSizing, "bridgeSizing");
	checkNear(loaded.bridgeSplit, original.bridgeSplit, 0.001, "bridgeSplit");
	check(loaded.bridgeRowAlign == original.bridgeRowAlign, "bridgeRowAlign");
	check(loaded.bridgeSpanEmpty == original.bridgeSpanEmpty, "bridgeSpanEmpty");
	check(loaded.dividerCap == original.dividerCap, "dividerCap");
	check(loaded.dividerMirrorEnds == original.dividerMirrorEnds, "dividerMirrorEnds");
	check(loaded.dividerEndCap == original.dividerEndCap, "dividerEndCap");
	check(loaded.dividerArm == original.dividerArm, "dividerArm");
	checkNear(loaded.dividerThickness, original.dividerThickness, 0.001, "dividerThickness");
	checkNear(loaded.dividerGap, original.dividerGap, 0.001, "dividerGap");
	checkNear(loaded.dividerPieceGap, original.dividerPieceGap, 0.001, "dividerPieceGap");
	checkEq(loaded.dividerRules, original.dividerRules, "dividerRules");
	checkNear(loaded.dividerRuleGap, original.dividerRuleGap, 0.001, "dividerRuleGap");
	checkNear(loaded.dividerRuleInset, original.dividerRuleInset, 0.001, "dividerRuleInset");
	check(loaded.dividerTint == original.dividerTint, "dividerTint");
	checkEq(loaded.dividerCentre.size(), original.dividerCentre.size(), "dividerCentre size");
	checkEq(loaded.columns, original.columns, "columns");
	checkEq(loaded.columnGap, original.columnGap, "columnGap");
	check(loaded.fillAcross == original.fillAcross, "fillAcross");
	check(loaded.useSecondaryStyle == original.useSecondaryStyle, "useSecondaryStyle");
	check(loaded.useBridgeStyle == original.useBridgeStyle, "useBridgeStyle");
	checkEq(loaded.stylePresetName, original.stylePresetName, "stylePresetName");
	checkEq(loaded.secondaryStylePresetName, original.secondaryStylePresetName, "secondaryStylePresetName");
	checkEq(loaded.bridgeStylePresetName, original.bridgeStylePresetName, "bridgeStylePresetName");
	checkEq(loaded.paddingTop, original.paddingTop, "paddingTop");
	checkEq(loaded.paddingBottom, original.paddingBottom, "paddingBottom");
	checkEq(loaded.marginX, original.marginX, "marginX");
	checkNear(loaded.sectionWidth, original.sectionWidth, 0.001, "sectionWidth");
	check(loaded.sectionAlign == original.sectionAlign, "sectionAlign");
	checkEq(loaded.spacerHeight, original.spacerHeight, "spacerHeight");
	check(loaded.visible == original.visible, "visible");

	checkEq(loaded.entries.size(), original.entries.size(), "entry count");
	for (int i = 0; i < std::min(loaded.entries.size(), original.entries.size()); ++i) {
		checkEq(loaded.entries.at(i).text, original.entries.at(i).text, QStringLiteral("entry %1 text").arg(i));
		checkEq(loaded.entries.at(i).secondaryText, original.entries.at(i).secondaryText,
			QStringLiteral("entry %1 secondaryText").arg(i));
		checkEq(loaded.entries.at(i).logo.path, original.entries.at(i).logo.path,
			QStringLiteral("entry %1 logo").arg(i));
	}
}

} // namespace

CT_SUITE(persistence_obs_data, "Every section field surviving the obs_data round trip")
{
	for (SectionType type : allSectionTypes()) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));
		const Section original = distinctive(type);

		OBSDataAutoRelease data = obs_data_create();
		original.save(data);

		Section loaded;
		loaded.load(data);

		check(loaded.type == type, "type");
		compare(loaded, original);
	}
}

CT_SUITE(persistence_json, "The designer's JSON export reloading what it wrote")
{
	Document document;
	for (SectionType type : allSectionTypes())
		document.sections.append(distinctive(type));
	document.setStylePreset(QStringLiteral("preset a"), TextStyle());

	Document restored;
	QString error;
	check(restored.fromJson(document.toJson(), &error), QStringLiteral("the export reloads: %1").arg(error));
	checkEq(restored.sections.size(), document.sections.size(), "every section survives");

	for (int i = 0; i < std::min(restored.sections.size(), document.sections.size()); ++i) {
		const Context context(QString::fromUtf8(sectionTypeId(document.sections.at(i).type)));
		check(restored.sections.at(i).type == document.sections.at(i).type, "type");
		compare(restored.sections.at(i), document.sections.at(i));
	}

	/* Nonsense in has to come back as a refusal rather than as an empty document. */
	Document broken;
	check(!broken.fromJson(QStringLiteral("{ not json at all"), nullptr), "malformed JSON is refused");
}

CT_SUITE(persistence_legacy, "Documents written before a field existed, and stored zeroes")
{
	/*
	 * A section carrying nothing but a type. Every fallback the loader documents has to be the
	 * one that turns up here, because this is what a scene collection from an older build is.
	 */
	OBSDataAutoRelease bare = obs_data_create();
	obs_data_set_string(bare, "type", "title");

	Section legacy;
	legacy.load(bare);

	check(legacy.type == SectionType::Title, "an old section still loads");
	check(legacy.secondaryText.isEmpty(), "no stored subtitle means no subtitle");
	checkEq(legacy.subtitleGap, 4, "subtitleGap falls back to its documented default");
	check(legacy.logoPlacement == LogoPlacement::Edge,
	      "logoPlacement falls back to the layout it was built against");
	check(legacy.bridgeType == BridgeType::Text, "bridgeType falls back to the string bridge");
	checkNear(legacy.bridgeGap, 8.0, 0.001, "bridgeGap falls back rather than to zero");
	checkNear(legacy.bridgeThickness, 4.0, 0.001, "bridgeThickness falls back to something drawable");
	check(legacy.bridgeTint, "bridgeTint falls back to on");
	checkNear(legacy.bridgeSplit, 0.5, 0.001, "bridgeSplit falls back to an even split");
	check(legacy.dividerCap == DividerShape::None, "an absent divider cap is None");
	check(legacy.dividerArm == DividerShape::Rule, "an absent divider arm is the plain rule, not None");
	check(legacy.dividerMirrorEnds, "divider ends are mirrored by default");
	checkEq(legacy.dividerRules, 1, "a divider has at least one rule");
	checkNear(legacy.sectionWidth, 1.0, 0.001, "sectionWidth falls back to the full canvas");

	/*
	 * Every one of these is a value a user can legitimately store, so the loader has to tell a
	 * stored zero apart from a missing key rather than infer one from the other.
	 */
	Section zeroed = Section::makeDefault(SectionType::TitleWithSubtitle);
	zeroed.subtitleGap = 0;
	zeroed.bridgeGap = 0.0;
	zeroed.bridgeSplit = 0.0;
	zeroed.dividerGap = 0.0;
	zeroed.dividerPieceGap = 0.0;
	zeroed.dividerRuleGap = 0.0;

	OBSDataAutoRelease data = obs_data_create();
	zeroed.save(data);

	Section reloaded;
	reloaded.load(data);

	checkEq(reloaded.subtitleGap, 0, "a stored subtitleGap of zero stays zero");
	checkNear(reloaded.bridgeGap, 0.0, 0.001, "a stored bridgeGap of zero stays zero");
	checkNear(reloaded.bridgeSplit, 0.0, 0.001, "a stored bridgeSplit of zero stays zero");
	checkNear(reloaded.dividerGap, 0.0, 0.001, "a stored dividerGap of zero stays zero");
	checkNear(reloaded.dividerPieceGap, 0.0, 0.001, "a stored dividerPieceGap of zero stays zero");
	checkNear(reloaded.dividerRuleGap, 0.0, 0.001, "a stored dividerRuleGap of zero stays zero");

	/* Values that would break the renderer are clamped rather than trusted off the file. */
	OBSDataAutoRelease hostile = obs_data_create();
	obs_data_set_string(hostile, "type", "section_divider");
	obs_data_set_int(hostile, "divider_rules", 9999);
	obs_data_set_int(hostile, "columns", -4);
	obs_data_set_double(hostile, "divider_thickness", -1.0);
	obs_data_set_double(hostile, "section_width", 17.0);
	obs_data_set_int(hostile, "entry_gap", -50);

	Section clamped;
	clamped.load(hostile);

	check(clamped.dividerRules >= 1 && clamped.dividerRules <= 16, "a rule count off a file is bounded");
	check(clamped.columns >= 1, "a column count off a file is at least one");
	check(clamped.dividerThickness > 0.0, "a thickness off a file is drawable");
	check(clamped.sectionWidth <= 1.0, "a section width off a file is a real share");
	check(clamped.entryGap >= 0, "an entry gap off a file is not negative");
}
