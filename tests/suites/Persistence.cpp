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

#include <cstring>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

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
	section.bridgeMinGap = 33.5;
	section.rowSubtitles = true;
	{
		/* One of everything a divider element can carry, across two lanes. */
		DividerElement shape;
		shape.kind = DividerElement::Kind::Shape;
		shape.shape = DividerShape::Custom;
		shape.svgPath = QStringLiteral("/art/piece.svg");
		shape.scale = 2.25;
		shape.rotation = 37.5;
		shape.offsetY = -3.5;
		shape.flip = true;
		shape.spanLanes = true;
		shape.optional = true;
		shape.ownColor = true;
		shape.color = QColor(12, 34, 56, 200);

		DividerElement line;
		line.kind = DividerElement::Kind::Line;
		line.lineShape = DividerShape::Dots;
		line.weight = 2.5;
		line.minLength = 40.0;
		line.join = true;
		line.scale = 1.5;

		DividerElement space = line;
		space.lineShape = DividerShape::None;

		DividerElement word;
		word.kind = DividerElement::Kind::Text;
		word.text = QStringLiteral("PART II");
		word.rotation = -12.0;

		DividerElement image;
		image.kind = DividerElement::Kind::Image;
		image.logo.path = QStringLiteral("/art/mark.png");
		image.logo.maxHeight = 61;

		DividerLane first;
		first.elements = {shape, line, space, word, image};
		first.thickness = 6.5;
		first.inset = 12.5;
		first.offsetY = 2.5;
		first.centerPiece = false;

		DividerLane second;
		second.elements = {line};
		second.thickness = 1.5;

		section.divider.lanes = {first, second};
		section.divider.symmetric = false;
		section.divider.laneGap = 8.5;
		section.divider.pieceGap = -9.5;
		section.divider.lineGap = 15.5;
		section.divider.length = 0.4;
		section.divider.align = HAlign::Right;
		section.divider.tint = false;
	}
	section.columns = 4;
	section.columnGap = 37;
	section.fillAcross = true;
	section.useSecondaryStyle = true;
	section.useBridgeStyle = true;
	section.stylePresetName = QStringLiteral("preset a");
	section.secondaryStylePresetName = QStringLiteral("preset b");
	section.bridgeStylePresetName = QStringLiteral("preset c");
	section.rowSubtitleStylePresetName = QStringLiteral("preset d");
	section.rowSecondarySubtitleStylePresetName = QStringLiteral("preset e");
	section.rowSubtitleStyle.pixelSize = 19;
	section.rowSecondarySubtitleStyle.pixelSize = 21;
	section.paddingTop = 21;
	section.paddingBottom = 23;
	section.marginX = 29;
	section.contentOffsetX = -37;
	section.indentStep = 33;
	section.sectionWidth = 0.75;
	section.sectionAlign = HAlign::Right;
	section.spacerHeight = 313;
	section.stickyEntrance = StickyEntrance::AfterRoll;
	section.stickyFadeIn = 1.25;
	section.stickyAnchor = StickyAnchor::Bottom;
	section.stickyCanvasPosition = 0.2;
	section.stickyOffset = -18.0;
	section.stickyHold = 12.5;
	section.stickyHoldForever = true;
	section.stickyRelease = StickyRelease::ResumeEndAtHold;

	section.actionLine = 0.35;
	section.actions.clear();
	{
		EndingActionConfig show;
		show.type = EndingActionType::SetSourceVisible;
		show.delay = 1.5;
		show.itemSceneName = QStringLiteral("Main");
		show.itemSourceName = QStringLiteral("Backdrop");
		show.itemMode = FilterToggleMode::Toggle;
		section.actions.append(show);

		EndingActionConfig filter;
		filter.type = EndingActionType::SetFilterEnabled;
		filter.filterSourceName = QStringLiteral("Camera");
		filter.filterName = QStringLiteral("Blur");
		filter.filterMode = FilterToggleMode::Disable;
		filter.hotkeyName = QStringLiteral("name");
		filter.hotkeyPartner = QStringLiteral("partner");
		section.actions.append(filter);
	}

	/*
	 * Two slots rather than one, and one of them bound to a preset: the pair is what catches a
	 * loader that reads the panels but flattens them onto a single slot, and a binding that is
	 * written but never read back.
	 */
	SectionBackground &panel = section.backgroundEntry(BackgroundSlot::Section);
	panel.panel.fill = BackgroundFill::Color;
	panel.panel.color = QColor(12, 34, 56, 210);
	panel.panel.opacity = 0.65;
	panel.panel.outsetLeft = 11.0;
	panel.panel.outsetTop = 12.0;
	panel.panel.outsetRight = 13.0;
	panel.panel.outsetBottom = 14.0;
	panel.panel.radiusTopLeft = 4.0;
	panel.panel.radiusTopRight = 8.0;
	panel.panel.radiusBottomRight = 12.0;
	panel.panel.radiusBottomLeft = 16.0;
	panel.panel.border.enabled = true;
	panel.panel.border.width = 3.0;
	panel.panel.border.color = QColor(9, 8, 7, 200);

	SectionBackground &titlePanel = section.backgroundEntry(BackgroundSlot::Title);
	titlePanel.panel.fill = BackgroundFill::Image;
	titlePanel.panel.imagePath = QStringLiteral("/card.png");
	titlePanel.panel.imageFit = BackgroundImageFit::Tile;
	titlePanel.presetName = QStringLiteral("Card");

	section.children.push_back(Section::makeDefault(SectionType::Header));
	section.visible = false;
	section.entries = {Entry{QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three"),
				 QStringLiteral("four"), LogoRef{QStringLiteral("/e.png"), 61}, 2}};

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
	checkNear(loaded.bridgeMinGap, original.bridgeMinGap, 0.001, "bridgeMinGap");
	check(loaded.rowSubtitles == original.rowSubtitles, "rowSubtitles");
	/* Compared whole: every field of every element of every lane, with nothing left to a default. */
	check(loaded.divider == original.divider, "divider");
	checkEq(loaded.columns, original.columns, "columns");
	checkEq(loaded.columnGap, original.columnGap, "columnGap");
	check(loaded.fillAcross == original.fillAcross, "fillAcross");
	check(loaded.useSecondaryStyle == original.useSecondaryStyle, "useSecondaryStyle");
	check(loaded.useBridgeStyle == original.useBridgeStyle, "useBridgeStyle");
	checkEq(loaded.stylePresetName, original.stylePresetName, "stylePresetName");
	checkEq(loaded.secondaryStylePresetName, original.secondaryStylePresetName, "secondaryStylePresetName");
	checkEq(loaded.bridgeStylePresetName, original.bridgeStylePresetName, "bridgeStylePresetName");
	checkEq(loaded.rowSubtitleStylePresetName, original.rowSubtitleStylePresetName, "rowSubtitleStylePresetName");
	checkEq(loaded.rowSecondarySubtitleStylePresetName, original.rowSecondarySubtitleStylePresetName,
		"rowSecondarySubtitleStylePresetName");
	checkEq(loaded.rowSubtitleStyle.pixelSize, original.rowSubtitleStyle.pixelSize, "rowSubtitleStyle.pixelSize");
	checkEq(loaded.rowSecondarySubtitleStyle.pixelSize, original.rowSecondarySubtitleStyle.pixelSize,
		"rowSecondarySubtitleStyle.pixelSize");
	checkEq(loaded.paddingTop, original.paddingTop, "paddingTop");
	checkEq(loaded.paddingBottom, original.paddingBottom, "paddingBottom");
	checkEq(loaded.marginX, original.marginX, "marginX");
	checkEq(loaded.contentOffsetX, original.contentOffsetX, "contentOffsetX");
	checkEq(loaded.indentStep, original.indentStep, "indentStep");
	checkNear(loaded.sectionWidth, original.sectionWidth, 0.001, "sectionWidth");
	check(loaded.sectionAlign == original.sectionAlign, "sectionAlign");
	checkEq(loaded.spacerHeight, original.spacerHeight, "spacerHeight");
	check(loaded.stickyEntrance == original.stickyEntrance, "stickyEntrance");
	checkNear(loaded.stickyFadeIn, original.stickyFadeIn, 0.001, "stickyFadeIn");
	check(loaded.stickyAnchor == original.stickyAnchor, "stickyAnchor");
	checkNear(loaded.stickyCanvasPosition, original.stickyCanvasPosition, 0.001, "stickyCanvasPosition");
	checkNear(loaded.stickyOffset, original.stickyOffset, 0.001, "stickyOffset");
	checkNear(loaded.stickyHold, original.stickyHold, 0.001, "stickyHold");
	check(loaded.stickyHoldForever == original.stickyHoldForever, "stickyHoldForever");
	check(loaded.stickyRelease == original.stickyRelease, "stickyRelease");
	checkNear(loaded.actionLine, original.actionLine, 0.001, "actionLine");
	check(loaded.actions == original.actions, "actions");

	checkEq(loaded.backgrounds.size(), original.backgrounds.size(), "background slot count");
	for (const BackgroundSlot slot : allBackgroundSlots()) {
		check(loaded.hasBackground(slot) == original.hasBackground(slot),
		      qPrintable(QStringLiteral("the %1 slot is there or is not")
					 .arg(QString::fromUtf8(backgroundSlotId(slot)))));
		/*
		 * Field for field through operator==, which is the same comparison a library reload uses
		 * to decide a panel did not move -- so a field the round trip drops is a field that would
		 * also make an unchanged library look changed.
		 */
		check(loaded.background(slot) == original.background(slot),
		      qPrintable(QStringLiteral("the %1 panel").arg(QString::fromUtf8(backgroundSlotId(slot)))));
		checkEq(loaded.backgroundPresetName(slot), original.backgroundPresetName(slot),
			qPrintable(QStringLiteral("the %1 binding").arg(QString::fromUtf8(backgroundSlotId(slot)))));
	}

	checkEq(static_cast<int>(loaded.children.size()), static_cast<int>(original.children.size()), "child count");
	if (!loaded.children.empty() && !original.children.empty()) {
		check(loaded.children.front().type == original.children.front().type, "a child keeps its type");
		checkEq(loaded.children.front().text, original.children.front().text, "and its content");
	}
	check(loaded.visible == original.visible, "visible");

	checkEq(loaded.entries.size(), original.entries.size(), "entry count");
	for (int i = 0; i < std::min(loaded.entries.size(), original.entries.size()); ++i) {
		checkEq(loaded.entries.at(i).text, original.entries.at(i).text, QStringLiteral("entry %1 text").arg(i));
		checkEq(loaded.entries.at(i).secondaryText, original.entries.at(i).secondaryText,
			QStringLiteral("entry %1 secondaryText").arg(i));
		checkEq(loaded.entries.at(i).subtitle, original.entries.at(i).subtitle,
			QStringLiteral("entry %1 subtitle").arg(i));
		checkEq(loaded.entries.at(i).secondarySubtitle, original.entries.at(i).secondarySubtitle,
			QStringLiteral("entry %1 secondarySubtitle").arg(i));
		checkEq(loaded.entries.at(i).indent, original.entries.at(i).indent,
			QStringLiteral("entry %1 indent").arg(i));
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
	checkEq(legacy.contentOffsetX, 0, "a section from before the nudge sits where its box puts it");
	checkEq(legacy.indentStep, 24, "and its entries tab by the documented step rather than by nothing");
	/* An absent divider is the plainest one: a single lane with a plain rule across it. */
	checkEq(legacy.divider.lanes.size(), 1, "an absent divider has one lane");
	if (!legacy.divider.lanes.isEmpty()) {
		const DividerLane &lane = legacy.divider.lanes.first();
		checkEq(lane.elements.size(), 1, "holding one element");
		check(!lane.elements.isEmpty() && lane.elements.first().isLine() &&
			      lane.elements.first().lineShape == DividerShape::Rule,
		      "a plain rule, not None");
		check(lane.elements.isEmpty() || !lane.elements.first().join,
		      "drawn apart, which is the divider it was written as");
	}
	check(legacy.divider.symmetric, "and symmetric by default");
	check(legacy.children.empty(), "a section from before sticky blocks holds nothing");
	check(legacy.stickyAnchor == StickyAnchor::Center, "a sticky block pins by its middle unless told otherwise");
	checkNear(legacy.stickyCanvasPosition, 0.5, 0.001, "halfway down the frame");
	checkNear(legacy.stickyHold, 5.0, 0.001, "and holds for a measured time rather than none");
	check(legacy.stickyRelease == StickyRelease::EndAtHold, "with the hold being the end of the roll");
	check(legacy.stickyEntrance == StickyEntrance::WithRoll,
	      "a block from before there was a choice arrives with the roll, as its author saw it");
	checkNear(legacy.stickyFadeIn, 0.75, 0.001, "and carries a fade for the entrance it does not use yet");
	checkNear(legacy.sectionWidth, 1.0, 0.001, "sectionWidth falls back to the full canvas");

	/*
	 * An end written as a single shape, which is every document from before an end was a stack.
	 * With different ends it cannot be symmetric, so it comes back as the whole row: the arrow, the
	 * rule, and the far end flipped round to point outward, as it was always drawn.
	 */
	OBSDataAutoRelease oldEnds = obs_data_create();
	obs_data_set_string(oldEnds, "type", "section_divider");
	obs_data_set_string(oldEnds, "divider_cap", "arrow");
	obs_data_set_bool(oldEnds, "divider_mirror_ends", false);
	obs_data_set_string(oldEnds, "divider_end_cap", "custom");
	obs_data_set_string(oldEnds, "divider_end_cap_svg", "/art/end.svg");

	Section migrated;
	migrated.load(oldEnds);

	check(!migrated.divider.symmetric, "different ends make a divider that is not symmetric");
	checkEq(migrated.divider.lanes.size(), 1, "one rule is one lane");
	if (migrated.divider.lanes.size() == 1 && migrated.divider.lanes.first().elements.size() == 3) {
		const QVector<DividerElement> &row = migrated.divider.lanes.first().elements;
		check(row.at(0).kind == DividerElement::Kind::Shape && row.at(0).shape == DividerShape::Arrow,
		      "the single cap shape is the first piece");
		checkNear(row.at(0).scale, 1.0, 0.001, "at its own size");
		check(!row.at(0).flip, "pointing outward to the left as written");
		check(row.at(1).isLine(), "then the rule");
		check(row.at(2).shape == DividerShape::Custom, "then the far end, keeping its custom shape");
		checkEq(row.at(2).svgPath, QStringLiteral("/art/end.svg"), "and its file");
		check(row.at(2).flip, "flipped to point outward to the right");
	} else {
		check(false, "the migrated row is an end, a rule and an end");
	}

	/*
	 * A whole old divider: mirrored arrow ends, a palindromic middle of three, three rules tapering
	 * by an inset. It is symmetric, its middle lane carries the half-row with the diamond on the
	 * axis, and the outer lanes carry the rule alone, inset, broken by the ornaments.
	 */
	{
		OBSDataAutoRelease old = obs_data_create();
		obs_data_set_string(old, "type", "section_divider");
		obs_data_set_string(old, "divider_cap", "arrow");
		obs_data_set_double(old, "divider_thickness", 5.0);
		obs_data_set_int(old, "divider_rules", 3);
		obs_data_set_double(old, "divider_rule_inset", 20.0);
		obs_data_set_double(old, "divider_rule_gap", 7.0);
		OBSDataArrayAutoRelease center = obs_data_array_create();
		for (const char *shape : {"dot", "diamond", "dot"}) {
			OBSDataAutoRelease piece = obs_data_create();
			obs_data_set_string(piece, "kind", "ornament");
			obs_data_set_string(piece, "shape", shape);
			obs_data_array_push_back(center, piece);
		}
		obs_data_set_array(old, "divider_centre", center);

		Section whole;
		whole.load(old);
		whole.visible = true;
		const Divider &d = whole.divider;
		check(d.symmetric, "a mirrored, centered, palindromic divider stays symmetric");
		checkEq(d.lanes.size(), 3, "three rules are three lanes");
		checkNear(d.laneGap, 7.0, 0.001, "spaced by the old rule gap");
		if (d.lanes.size() == 3) {
			const QVector<DividerElement> &half = d.lanes.at(1).elements;
			checkEq(half.size(), 4, "the middle lane is the arrow, the rule, a dot and the diamond");
			if (half.size() == 4) {
				check(half.at(3).shape == DividerShape::Diamond, "the diamond last, on the axis");
				check(half.at(0).spanLanes && half.at(3).spanLanes, "its pieces break the other lanes");
			}
			check(d.lanes.at(1).centerPiece, "and drawn once there");
			checkNear(d.lanes.at(0).inset, 20.0, 0.001, "the outer lanes are inset by the old taper");
			checkNear(d.lanes.at(1).inset, 0.0, 0.001, "and the middle one is not");
			checkEq(d.lanes.at(0).elements.size(), 1, "an outer lane is the rule alone");
			checkNear(d.lanes.at(0).thickness, 5.0, 0.001, "at the old thickness");
			checkEq(d.drawnRow(d.lanes.at(1)).size(), 7,
				"drawn as arrow, rule, dot, diamond, dot, rule, arrow");
		}

		/* And it draws: the migrated divider is as tall as the tallest thing in it, inside its box. */
		check(measure(documentWith(whole)) > 0, "the migrated divider draws");
	}

	/*
	 * Every one of these is a value a user can legitimately store, so the loader has to tell a
	 * stored zero apart from a missing key rather than infer one from the other.
	 */
	Section zeroed = Section::makeDefault(SectionType::TitleWithSubtitle);
	zeroed.subtitleGap = 0;
	zeroed.bridgeGap = 0.0;
	zeroed.bridgeSplit = 0.0;
	zeroed.divider.lineGap = 0.0;
	zeroed.divider.pieceGap = 0.0;
	zeroed.divider.laneGap = 0.0;

	OBSDataAutoRelease data = obs_data_create();
	zeroed.save(data);

	Section reloaded;
	reloaded.load(data);

	checkEq(reloaded.subtitleGap, 0, "a stored subtitleGap of zero stays zero");
	checkNear(reloaded.bridgeGap, 0.0, 0.001, "a stored bridgeGap of zero stays zero");
	checkNear(reloaded.bridgeSplit, 0.0, 0.001, "a stored bridgeSplit of zero stays zero");
	checkNear(reloaded.divider.lineGap, 0.0, 0.001, "a stored line gap of zero stays zero");
	checkNear(reloaded.divider.pieceGap, 0.0, 0.001, "a stored piece gap of zero stays zero");
	checkNear(reloaded.divider.laneGap, 0.0, 0.001, "a stored lane gap of zero stays zero");

	/* Values that would break the renderer are clamped rather than trusted off the file. */
	OBSDataAutoRelease hostile = obs_data_create();
	obs_data_set_string(hostile, "type", "section_divider");
	obs_data_set_int(hostile, "divider_rules", 9999);
	obs_data_set_int(hostile, "columns", -4);
	obs_data_set_double(hostile, "divider_thickness", -1.0);
	obs_data_set_double(hostile, "section_width", 17.0);
	obs_data_set_int(hostile, "entry_gap", -50);
	obs_data_set_double(hostile, "divider_gap", -1e9);
	obs_data_set_double(hostile, "divider_piece_gap", -1e9);

	Section clamped;
	clamped.load(hostile);

	check(clamped.divider.lanes.size() >= 1 && clamped.divider.lanes.size() <= kMaxDividerLanes,
	      "a rule count off a file is bounded");
	check(clamped.columns >= 1, "a column count off a file is at least one");
	check(!clamped.divider.lanes.isEmpty() && clamped.divider.lanes.first().thickness > 0.0,
	      "a thickness off a file is drawable");
	check(clamped.sectionWidth <= 1.0, "a section width off a file is a real share");
	check(clamped.entryGap >= 0, "an entry gap off a file is not negative");
	/*
	 * A join is allowed to be negative -- that is what makes the parts touch -- so what is
	 * bounded is how far, not which side of zero.
	 */
	check(clamped.divider.lineGap >= -kMaxDividerJoin, "a join off a file cannot overlap without bound");
	check(clamped.divider.pieceGap >= -kMaxDividerJoin, "and neither can a piece join");

	/* The same of a divider in the new shape, written by hand. */
	OBSDataAutoRelease hostileNew = obs_data_create();
	obs_data_set_string(hostileNew, "type", "section_divider");
	OBSDataAutoRelease hostileDivider = obs_data_create();
	OBSDataArrayAutoRelease manyLanes = obs_data_array_create();
	for (int i = 0; i < 50; ++i) {
		OBSDataAutoRelease lane = obs_data_create();
		obs_data_set_double(lane, "thickness", -5.0);
		obs_data_array_push_back(manyLanes, lane);
	}
	obs_data_set_array(hostileDivider, "lanes", manyLanes);
	obs_data_set_double(hostileDivider, "length", 40.0);
	obs_data_set_double(hostileDivider, "line_gap", -1e12);
	obs_data_set_obj(hostileNew, "divider", hostileDivider);

	Section clampedNew;
	clampedNew.load(hostileNew);
	checkEq(clampedNew.divider.lanes.size(), kMaxDividerLanes, "a lane count off a file is bounded");
	check(clampedNew.divider.lanes.first().thickness > 0.0, "every lane is drawable");
	check(clampedNew.divider.length <= 1.0, "a length off a file is a real share");
	check(clampedNew.divider.lineGap >= -kMaxDividerJoin, "and a join is bounded");

	/* A divider with no lanes at all is still given one to put something in. */
	OBSDataAutoRelease empty = obs_data_create();
	obs_data_set_string(empty, "type", "section_divider");
	OBSDataAutoRelease emptyDivider = obs_data_create();
	OBSDataArrayAutoRelease noLanes = obs_data_array_create();
	obs_data_set_array(emptyDivider, "lanes", noLanes);
	obs_data_set_obj(empty, "divider", emptyDivider);
	Section emptied;
	emptied.load(empty);
	checkEq(emptied.divider.lanes.size(), 1, "an empty lane list loads as one empty lane");
}

CT_SUITE(fonts_store, "Bundled fonts are written compressed and decoded once")
{
	/* Outline data compresses; random bytes do not. A font is the former, so the fixture is too. */
	QByteArray bytes;
	for (int i = 0; i < 200000; ++i)
		bytes.append(static_cast<char>("glyf loca hmtx"[i % 14] + (i / 1000) % 3));

	Document document;
	BundledFont font;
	font.family = QStringLiteral("Closing Time Store Test");
	font.fileName = QStringLiteral("store.ttf");
	font.data = bytes;
	document.bundledFonts = {font};

	OBSDataAutoRelease saved = obs_data_create();
	document.save(saved);
	checkEq(static_cast<long long>(obs_data_get_int(saved, "schema_version")), Document::kSchemaVersion,
		"the document says which format it is in");

	OBSDataArrayAutoRelease array = obs_data_get_array(saved, "bundled_fonts");
	OBSDataAutoRelease entry = obs_data_array_item(array, 0);
	checkEq(QString::fromLatin1(obs_data_get_string(entry, "encoding")), QStringLiteral("zlib"),
		"written compressed");
	check(std::strlen(obs_data_get_string(entry, "data")) < static_cast<size_t>(bytes.toBase64().size()) / 2,
	      "and much smaller than the plain base64 it replaces");
	checkEq(QString::fromLatin1(obs_data_get_string(entry, "sha256")), font.hash(), "named by its hash");

	/* The first load may decode; every one after that, as on every frame of a slider drag, must not. */
	Document first;
	first.load(saved);
	const int decodes = fontStoreDecodes();
	for (int i = 0; i < 5; ++i) {
		Document again;
		again.load(saved);
		check(again.bundledFonts.size() == 1 && again.bundledFonts.first().data == bytes,
		      "the same bytes come back");
	}
	checkEq(fontStoreDecodes(), decodes, "and loading the same document again decodes nothing");
	check(!first.bundledFonts.isEmpty() && first.bundledFonts.first().data == bytes, "the round trip is exact");

	/* A version 1 document: plain base64, no hash, no encoding. It loads, and asks to be written back. */
	OBSDataAutoRelease old = obs_data_create();
	OBSDataArrayAutoRelease oldFonts = obs_data_array_create();
	OBSDataAutoRelease oldFont = obs_data_create();
	obs_data_set_string(oldFont, "family", "Closing Time Store Test");
	obs_data_set_string(oldFont, "file", "store.ttf");
	obs_data_set_string(oldFont, "data", bytes.toBase64().constData());
	obs_data_array_push_back(oldFonts, oldFont);
	obs_data_set_array(old, "bundled_fonts", oldFonts);

	Document older;
	bool migrated = false;
	older.load(old, &migrated);
	check(older.bundledFonts.size() == 1 && older.bundledFonts.first().data == bytes,
	      "a version 1 bundle still loads");
	check(migrated, "and says the settings are worth writing again in the current shape");

	Document current;
	bool rewritten = true;
	current.load(saved, &rewritten);
	check(!rewritten, "a current document asks for nothing");
}

CT_SUITE(document_content, "Telling a content edit from a playback one, without serializing anything")
{
	Document document;
	Section title = Section::makeDefault(SectionType::Title);
	title.text = QStringLiteral("Directed by");
	document.sections = {title};

	Document playback = document;
	playback.scrollSpeed *= 3.0;
	playback.loop = !playback.loop;
	playback.startDelay = 4.0;
	playback.manualScroll = true;
	playback.scrollPosition = 40.0;
	playback.endingAction.delay = 2.0;
	check(document.sameContent(playback), "playback settings are not content");
	check(!(document == playback), "though they are part of the document");

	Document leads = document;
	leads.leadIn = 300;
	check(!document.sameContent(leads), "lead-in is content: it is part of the strip");

	Document text = document;
	text.sections[0].text = QStringLiteral("Directed by someone");
	check(!document.sameContent(text), "a section's text is content");

	Document styled = document;
	styled.sections[0].style.shadow.enabled = true;
	check(!document.sameContent(styled), "and so is its style");

	Document child = document;
	Section block = Section::makeDefault(SectionType::StickyBlock);
	child.sections.append(block);
	Document childEdited = child;
	childEdited.sections[1].children.front().text = QStringLiteral("Changed");
	check(!child.sameContent(childEdited), "and so is anything inside a block");
}
