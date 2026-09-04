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

#include "model/CalendarModel.hpp"

#include <obs.hpp>

#include <QSet>
#include <QTime>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>

#include "model/StyleLibrary.hpp"

namespace closingtime {

namespace {

/* --- persistence helpers ------------------------------------------------------------------ */

template<typename T> void saveVector(obs_data_t *data, const char *name, const QVector<T> &items)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const T &item : items) {
		OBSDataAutoRelease entry = obs_data_create();
		item.save(entry);
		obs_data_array_push_back(array, entry);
	}
	obs_data_set_array(data, name, array);
}

template<typename T> void loadVector(obs_data_t *data, const char *name, QVector<T> *items)
{
	items->clear();

	OBSDataArrayAutoRelease array = obs_data_get_array(data, name);
	if (!array)
		return;

	const size_t count = obs_data_array_count(array);
	items->reserve(static_cast<int>(count));
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease entry = obs_data_array_item(array, i);
		T item;
		item.load(entry);
		items->append(item);
	}
}

/*
 * A list of plain strings, written as an array of one-key objects rather than joined.
 *
 * Joining would be shorter and would quietly corrupt the first ticker message somebody puts a line
 * break in, which is a thing ticker messages have.
 */
void saveStringList(obs_data_t *data, const char *name, const QStringList &items)
{
	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const QString &item : items) {
		OBSDataAutoRelease entry = obs_data_create();
		obs_data_set_string(entry, "value", item.toUtf8().constData());
		obs_data_array_push_back(array, entry);
	}
	obs_data_set_array(data, name, array);
}

void loadStringList(obs_data_t *data, const char *name, QStringList *items)
{
	items->clear();

	OBSDataArrayAutoRelease array = obs_data_get_array(data, name);
	if (!array)
		return;

	const size_t count = obs_data_array_count(array);
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease entry = obs_data_array_item(array, i);
		items->append(QString::fromUtf8(obs_data_get_string(entry, "value")));
	}
}

void saveColor(obs_data_t *data, const char *name, const QColor &color)
{
	obs_data_set_int(data, name, static_cast<long long>(color.rgba()));
}

QColor loadColor(obs_data_t *data, const char *name, const QColor &fallback)
{
	if (!obs_data_has_user_value(data, name))
		return fallback;
	return QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(data, name)));
}

void saveString(obs_data_t *data, const char *name, const QString &value)
{
	obs_data_set_string(data, name, value.toUtf8().constData());
}

QString loadString(obs_data_t *data, const char *name)
{
	return QString::fromUtf8(obs_data_get_string(data, name));
}

QString loadString(obs_data_t *data, const char *name, const QString &fallback)
{
	if (!obs_data_has_user_value(data, name))
		return fallback;
	return QString::fromUtf8(obs_data_get_string(data, name));
}

double loadDouble(obs_data_t *data, const char *name, double fallback)
{
	if (!obs_data_has_user_value(data, name))
		return fallback;
	return obs_data_get_double(data, name);
}

int loadInt(obs_data_t *data, const char *name, int fallback)
{
	if (!obs_data_has_user_value(data, name))
		return fallback;
	return static_cast<int>(obs_data_get_int(data, name));
}

bool loadBool(obs_data_t *data, const char *name, bool fallback)
{
	if (!obs_data_has_user_value(data, name))
		return fallback;
	return obs_data_get_bool(data, name);
}

void saveStyle(obs_data_t *data, const char *name, const TextStyle &style)
{
	OBSDataAutoRelease entry = obs_data_create();
	style.save(entry);
	obs_data_set_obj(data, name, entry);
}

void loadStyle(obs_data_t *data, const char *name, TextStyle *style)
{
	OBSDataAutoRelease entry = obs_data_get_obj(data, name);
	if (entry)
		style->load(entry);
}

void savePanel(obs_data_t *data, const char *name, const BackgroundPanel &panel)
{
	OBSDataAutoRelease entry = obs_data_create();
	panel.save(entry);
	obs_data_set_obj(data, name, entry);
}

void loadPanel(obs_data_t *data, const char *name, BackgroundPanel *panel)
{
	OBSDataAutoRelease entry = obs_data_get_obj(data, name);
	if (entry)
		panel->load(entry);
}

void saveLogo(obs_data_t *data, const char *name, const LogoRef &logo)
{
	OBSDataAutoRelease entry = obs_data_create();
	logo.save(entry);
	obs_data_set_obj(data, name, entry);
}

void loadLogo(obs_data_t *data, const char *name, LogoRef *logo)
{
	OBSDataAutoRelease entry = obs_data_get_obj(data, name);
	if (entry)
		logo->load(entry);
}

void saveBlockStyle(obs_data_t *data, const char *name, const BlockStyle &style)
{
	OBSDataAutoRelease entry = obs_data_create();
	style.save(entry);
	obs_data_set_obj(data, name, entry);
}

void loadBlockStyle(obs_data_t *data, const char *name, BlockStyle *style)
{
	OBSDataAutoRelease entry = obs_data_get_obj(data, name);
	if (entry)
		style->load(entry);
}

/* --- id tables ---------------------------------------------------------------------------- */

template<typename T> struct IdRow {
	T value;
	const char *id;
	const char *name;
};

template<typename T, size_t N> const char *idOf(const IdRow<T> (&table)[N], T value)
{
	for (const auto &row : table) {
		if (row.value == value)
			return row.id;
	}
	return table[0].id;
}

template<typename T, size_t N> T valueOf(const IdRow<T> (&table)[N], const char *id, T fallback)
{
	if (!id)
		return fallback;

	for (const auto &row : table) {
		if (strcmp(row.id, id) == 0)
			return row.value;
	}
	return fallback;
}

template<typename T, size_t N> const char *nameOf(const IdRow<T> (&table)[N], T value)
{
	for (const auto &row : table) {
		if (row.value == value)
			return row.name;
	}
	return table[0].name;
}

template<typename T, size_t N> QVector<T> valuesOf(const IdRow<T> (&table)[N])
{
	QVector<T> result;
	result.reserve(static_cast<int>(N));
	for (const auto &row : table)
		result.append(row.value);
	return result;
}

const IdRow<CalendarLayout> kLayouts[] = {
	{CalendarLayout::UpNext, "up_next", "Up Next List"},
	{CalendarLayout::Grid, "grid", "Time Grid"},
	{CalendarLayout::Stacked, "stacked", "Stacked Blocks"},
};

const IdRow<TimeAxisMode> kAxisModes[] = {
	{TimeAxisMode::Clock, "clock", "Clock Time"},
	{TimeAxisMode::Slots, "slots", "Named Slots"},
};

const IdRow<GridOrientation> kOrientations[] = {
	{GridOrientation::TimeDown, "time_down", "Time Down The Side"},
	{GridOrientation::TimeAcross, "time_across", "Time Across The Top"},
};

const IdRow<OverflowMode> kOverflowModes[] = {
	{OverflowMode::Fit, "fit", "Scale To Fit"},
	{OverflowMode::Page, "page", "Page Through"},
	{OverflowMode::Scroll, "scroll", "Scroll"},
};

const IdRow<LaneOverlap> kOverlaps[] = {
	{LaneOverlap::Split, "split", "Side By Side"},
	{LaneOverlap::Stack, "stack", "Stacked"},
};

const IdRow<OpenEndedRule> kOpenEnded[] = {
	{OpenEndedRule::UntilNext, "until_next", "Until The Next Event"},
	{OpenEndedRule::Fixed, "fixed", "A Fixed Length"},
	{OpenEndedRule::Minimal, "minimal", "As Small As It Fits"},
};

const IdRow<GutterSide> kGutterSides[] = {
	{GutterSide::Leading, "leading", "Leading Edge"},
	{GutterSide::Trailing, "trailing", "Trailing Edge"},
	{GutterSide::Both, "both", "Both Edges"},
	{GutterSide::None, "none", "None"},
};

const IdRow<EventStatus> kStatuses[] = {
	{EventStatus::Auto, "auto", "From The Clock"},      {EventStatus::Upcoming, "upcoming", "Upcoming"},
	{EventStatus::Live, "live", "Happening Now"},       {EventStatus::Finished, "finished", "Finished"},
	{EventStatus::Cancelled, "cancelled", "Cancelled"},
};

const IdRow<ClockZoneMode> kZoneModes[] = {
	{ClockZoneMode::Automatic, "automatic", "This Machine's Zone"},
	{ClockZoneMode::Fixed, "fixed", "A Named Zone"},
};

const IdRow<HatchPattern> kHatches[] = {
	{HatchPattern::None, "none", "None"},
	{HatchPattern::Diagonal, "diagonal", "Diagonal"},
	{HatchPattern::BackDiagonal, "back_diagonal", "Back Diagonal"},
	{HatchPattern::Cross, "cross", "Cross"},
	{HatchPattern::Horizontal, "horizontal", "Horizontal"},
	{HatchPattern::Vertical, "vertical", "Vertical"},
};

const IdRow<TextFit> kTextFits[] = {
	{TextFit::Shrink, "shrink", "Shrink To Fit"},
	{TextFit::Wrap, "wrap", "Wrap"},
	{TextFit::Ellipsize, "ellipsize", "Cut With An Ellipsis"},
};

const IdRow<ElementType> kElementTypes[] = {
	{ElementType::Text, "text", "Text"},       {ElementType::Image, "image", "Image"},
	{ElementType::Panel, "panel", "Panel"},    {ElementType::Clock, "clock", "Clock"},
	{ElementType::Ticker, "ticker", "Ticker"}, {ElementType::ChipRow, "chip_row", "Chip Row"},
	{ElementType::Legend, "legend", "Legend"},
};

const IdRow<ElementAnchor> kAnchors[] = {
	{ElementAnchor::TopLeft, "top_left", "Top Left"},
	{ElementAnchor::TopCenter, "top_center", "Top Center"},
	{ElementAnchor::TopRight, "top_right", "Top Right"},
	{ElementAnchor::CenterLeft, "center_left", "Center Left"},
	{ElementAnchor::Center, "center", "Center"},
	{ElementAnchor::CenterRight, "center_right", "Center Right"},
	{ElementAnchor::BottomLeft, "bottom_left", "Bottom Left"},
	{ElementAnchor::BottomCenter, "bottom_center", "Bottom Center"},
	{ElementAnchor::BottomRight, "bottom_right", "Bottom Right"},
};

const IdRow<LegendSource> kLegendSources[] = {
	{LegendSource::Categories, "categories", "Categories"},
	{LegendSource::Lanes, "lanes", "Lanes"},
	{LegendSource::Statuses, "statuses", "Statuses"},
};

/* --- built-in appearance ------------------------------------------------------------------ */

/*
 * What a block looks like before anybody has said anything about it.
 *
 * A real appearance rather than a blank one, because the alternative is a new board that draws
 * invisible rectangles until six settings have been found. Every layer of the cascade merges over
 * this, so a document that sets nothing still renders something a user can then edit.
 */
ResolvedBlockStyle builtInBlockStyle()
{
	ResolvedBlockStyle resolved;

	resolved.panel.fill = BackgroundFill::Color;
	resolved.panel.color = QColor(28, 32, 44, 235);
	resolved.panel.setRadius(6.0);

	resolved.titleStyle.pixelSize = 24;
	resolved.titleStyle.bold = true;
	resolved.titleStyle.color = QColor(255, 255, 255);
	resolved.titleStyle.align = HAlign::Left;

	resolved.subtitleStyle.pixelSize = 18;
	resolved.subtitleStyle.color = QColor(226, 232, 240);
	resolved.subtitleStyle.align = HAlign::Left;

	resolved.metaStyle.pixelSize = 15;
	resolved.metaStyle.color = QColor(148, 163, 184);
	resolved.metaStyle.align = HAlign::Left;

	return resolved;
}

/* Everything on the document that carries a text style, for the font walk. */
void visitTextStyles(const CalendarDocument &document, const std::function<void(const TextStyle &)> &visit)
{
	const auto visitLayer = [&](const BlockStyle &layer) {
		if (layer.useTitleStyle)
			visit(layer.titleStyle);
		if (layer.useSubtitleStyle)
			visit(layer.subtitleStyle);
		if (layer.useMetaStyle)
			visit(layer.metaStyle);
	};

	visitLayer(document.blockStyle);
	visitLayer(document.upcomingStyle);
	visitLayer(document.liveStyle);
	visitLayer(document.finishedStyle);
	visitLayer(document.cancelledStyle);
	visitLayer(document.continuationStyle);
	visitLayer(document.live.currentStyle);

	for (const CalendarLane &lane : document.lanes) {
		visitLayer(lane.style);
		visitLayer(lane.headerStyle);
	}
	for (const CalendarCategory &category : document.categories)
		visitLayer(category.style);
	for (const CalendarEvent &event : document.events)
		visitLayer(event.style);

	visit(document.grid.gutterStyle);
	visit(document.grid.laneStyle);
	visit(document.grid.dayStyle);

	for (const CalendarElement &element : document.elements)
		visit(element.textStyle);
}

/* Every text-preset binding on the document, so a rename can be followed into all of them. */
void visitStylePresetNames(CalendarDocument &document, const std::function<void(QString &)> &visit)
{
	const auto visitLayer = [&](BlockStyle &layer) {
		visit(layer.titlePresetName);
		visit(layer.subtitlePresetName);
		visit(layer.metaPresetName);
	};

	visitLayer(document.blockStyle);
	visitLayer(document.upcomingStyle);
	visitLayer(document.liveStyle);
	visitLayer(document.finishedStyle);
	visitLayer(document.cancelledStyle);
	visitLayer(document.continuationStyle);
	visitLayer(document.live.currentStyle);

	for (CalendarLane &lane : document.lanes) {
		visitLayer(lane.style);
		visitLayer(lane.headerStyle);
	}
	for (CalendarCategory &category : document.categories)
		visitLayer(category.style);
	for (CalendarEvent &event : document.events)
		visitLayer(event.style);

	visit(document.grid.gutterPresetName);
	visit(document.grid.lanePresetName);
	visit(document.grid.dayPresetName);

	for (CalendarElement &element : document.elements)
		visit(element.textPresetName);
}

/* And the same for the panel bindings, which are a separate namespace. */
void visitBackgroundPresetNames(CalendarDocument &document, const std::function<void(QString &)> &visit)
{
	const auto visitLayer = [&](BlockStyle &layer) {
		visit(layer.panelPresetName);
	};

	visitLayer(document.blockStyle);
	visitLayer(document.upcomingStyle);
	visitLayer(document.liveStyle);
	visitLayer(document.finishedStyle);
	visitLayer(document.cancelledStyle);
	visitLayer(document.continuationStyle);
	visitLayer(document.live.currentStyle);

	for (CalendarLane &lane : document.lanes) {
		visitLayer(lane.style);
		visitLayer(lane.headerStyle);
	}
	for (CalendarCategory &category : document.categories)
		visitLayer(category.style);
	for (CalendarEvent &event : document.events)
		visitLayer(event.style);

	visit(document.grid.gutterPanelPresetName);
	visit(document.grid.lanePanelPresetName);
	visit(document.grid.dayPanelPresetName);

	for (CalendarElement &element : document.elements)
		visit(element.panelPresetName);
}

} // namespace

/* --- enum ids ----------------------------------------------------------------------------- */

const char *calendarLayoutId(CalendarLayout layout)
{
	return idOf(kLayouts, layout);
}
CalendarLayout calendarLayoutFromId(const char *id, CalendarLayout fallback)
{
	return valueOf(kLayouts, id, fallback);
}
const char *calendarLayoutName(CalendarLayout layout)
{
	return nameOf(kLayouts, layout);
}
const QVector<CalendarLayout> &allCalendarLayouts()
{
	static const QVector<CalendarLayout> values = valuesOf(kLayouts);
	return values;
}

const char *timeAxisModeId(TimeAxisMode mode)
{
	return idOf(kAxisModes, mode);
}
TimeAxisMode timeAxisModeFromId(const char *id, TimeAxisMode fallback)
{
	return valueOf(kAxisModes, id, fallback);
}

const char *gridOrientationId(GridOrientation orientation)
{
	return idOf(kOrientations, orientation);
}
GridOrientation gridOrientationFromId(const char *id, GridOrientation fallback)
{
	return valueOf(kOrientations, id, fallback);
}

const char *overflowModeId(OverflowMode mode)
{
	return idOf(kOverflowModes, mode);
}
OverflowMode overflowModeFromId(const char *id, OverflowMode fallback)
{
	return valueOf(kOverflowModes, id, fallback);
}
const char *overflowModeName(OverflowMode mode)
{
	return nameOf(kOverflowModes, mode);
}
const QVector<OverflowMode> &allOverflowModes()
{
	static const QVector<OverflowMode> values = valuesOf(kOverflowModes);
	return values;
}

const char *laneOverlapId(LaneOverlap overlap)
{
	return idOf(kOverlaps, overlap);
}
LaneOverlap laneOverlapFromId(const char *id, LaneOverlap fallback)
{
	return valueOf(kOverlaps, id, fallback);
}

const char *openEndedRuleId(OpenEndedRule rule)
{
	return idOf(kOpenEnded, rule);
}
OpenEndedRule openEndedRuleFromId(const char *id, OpenEndedRule fallback)
{
	return valueOf(kOpenEnded, id, fallback);
}

const char *gutterSideId(GutterSide side)
{
	return idOf(kGutterSides, side);
}
GutterSide gutterSideFromId(const char *id, GutterSide fallback)
{
	return valueOf(kGutterSides, id, fallback);
}

const char *eventStatusId(EventStatus status)
{
	return idOf(kStatuses, status);
}
EventStatus eventStatusFromId(const char *id, EventStatus fallback)
{
	return valueOf(kStatuses, id, fallback);
}
const char *eventStatusName(EventStatus status)
{
	return nameOf(kStatuses, status);
}
const QVector<EventStatus> &allEventStatuses()
{
	static const QVector<EventStatus> values = valuesOf(kStatuses);
	return values;
}

const char *clockZoneModeId(ClockZoneMode mode)
{
	return idOf(kZoneModes, mode);
}
ClockZoneMode clockZoneModeFromId(const char *id, ClockZoneMode fallback)
{
	return valueOf(kZoneModes, id, fallback);
}

const char *hatchPatternId(HatchPattern pattern)
{
	return idOf(kHatches, pattern);
}
HatchPattern hatchPatternFromId(const char *id, HatchPattern fallback)
{
	return valueOf(kHatches, id, fallback);
}
const char *hatchPatternName(HatchPattern pattern)
{
	return nameOf(kHatches, pattern);
}
const QVector<HatchPattern> &allHatchPatterns()
{
	static const QVector<HatchPattern> values = valuesOf(kHatches);
	return values;
}

const char *textFitId(TextFit fit)
{
	return idOf(kTextFits, fit);
}
TextFit textFitFromId(const char *id, TextFit fallback)
{
	return valueOf(kTextFits, id, fallback);
}
const char *textFitName(TextFit fit)
{
	return nameOf(kTextFits, fit);
}
const QVector<TextFit> &allTextFits()
{
	static const QVector<TextFit> values = valuesOf(kTextFits);
	return values;
}

const char *elementTypeId(ElementType type)
{
	return idOf(kElementTypes, type);
}
ElementType elementTypeFromId(const char *id, ElementType fallback)
{
	return valueOf(kElementTypes, id, fallback);
}
const char *elementTypeName(ElementType type)
{
	return nameOf(kElementTypes, type);
}
const QVector<ElementType> &allElementTypes()
{
	static const QVector<ElementType> values = valuesOf(kElementTypes);
	return values;
}

const char *elementAnchorId(ElementAnchor anchor)
{
	return idOf(kAnchors, anchor);
}
ElementAnchor elementAnchorFromId(const char *id, ElementAnchor fallback)
{
	return valueOf(kAnchors, id, fallback);
}
const char *elementAnchorName(ElementAnchor anchor)
{
	return nameOf(kAnchors, anchor);
}
const QVector<ElementAnchor> &allElementAnchors()
{
	static const QVector<ElementAnchor> values = valuesOf(kAnchors);
	return values;
}

double elementAnchorX(ElementAnchor anchor)
{
	switch (anchor) {
	case ElementAnchor::TopLeft:
	case ElementAnchor::CenterLeft:
	case ElementAnchor::BottomLeft:
		return 0.0;
	case ElementAnchor::TopCenter:
	case ElementAnchor::Center:
	case ElementAnchor::BottomCenter:
		return 0.5;
	default:
		return 1.0;
	}
}

double elementAnchorY(ElementAnchor anchor)
{
	switch (anchor) {
	case ElementAnchor::TopLeft:
	case ElementAnchor::TopCenter:
	case ElementAnchor::TopRight:
		return 0.0;
	case ElementAnchor::CenterLeft:
	case ElementAnchor::Center:
	case ElementAnchor::CenterRight:
		return 0.5;
	default:
		return 1.0;
	}
}

const char *legendSourceId(LegendSource source)
{
	return idOf(kLegendSources, source);
}
LegendSource legendSourceFromId(const char *id, LegendSource fallback)
{
	return valueOf(kLegendSources, id, fallback);
}

/* --- HatchSpec ---------------------------------------------------------------------------- */

void HatchSpec::save(obs_data_t *data) const
{
	obs_data_set_string(data, "pattern", hatchPatternId(pattern));
	saveColor(data, "color", color);
	obs_data_set_double(data, "width", width);
	obs_data_set_double(data, "spacing", spacing);
}

void HatchSpec::load(obs_data_t *data)
{
	pattern = hatchPatternFromId(obs_data_get_string(data, "pattern"), HatchPattern::None);
	color = loadColor(data, "color", QColor(255, 255, 255, 70));
	width = std::max(0.0, loadDouble(data, "width", 3.0));
	spacing = std::max(1.0, loadDouble(data, "spacing", 12.0));
}

/* --- BlockStyle --------------------------------------------------------------------------- */

bool BlockStyle::isEmpty() const
{
	return !usePanel && !useTitleStyle && !useSubtitleStyle && !useMetaStyle && !useHatch && !useOpacity;
}

void BlockStyle::applyOver(const BlockStyle &over)
{
	if (over.usePanel) {
		usePanel = true;
		panel = over.panel;
		panelPresetName = over.panelPresetName;
	}
	if (over.useTitleStyle) {
		useTitleStyle = true;
		titleStyle = over.titleStyle;
		titlePresetName = over.titlePresetName;
	}
	if (over.useSubtitleStyle) {
		useSubtitleStyle = true;
		subtitleStyle = over.subtitleStyle;
		subtitlePresetName = over.subtitlePresetName;
	}
	if (over.useMetaStyle) {
		useMetaStyle = true;
		metaStyle = over.metaStyle;
		metaPresetName = over.metaPresetName;
	}
	if (over.useHatch) {
		useHatch = true;
		hatch = over.hatch;
	}
	/*
	 * Opacity multiplies rather than replaces. It is the one part of a layer that says "less of
	 * whatever is underneath" rather than "this instead", so a finished event inside a lane that
	 * is already half-present should be half of that rather than back up at the status's number.
	 */
	if (over.useOpacity) {
		const double running = useOpacity ? opacity : 1.0;
		useOpacity = true;
		opacity = running * over.opacity;
	}
	if (over.useFit) {
		useFit = true;
		fit = over.fit;
		minPixelSize = over.minPixelSize;
	}
}

bool BlockStyle::operator==(const BlockStyle &other) const
{
	return usePanel == other.usePanel && panel == other.panel && panelPresetName == other.panelPresetName &&
	       useTitleStyle == other.useTitleStyle && titleStyle == other.titleStyle &&
	       titlePresetName == other.titlePresetName && useSubtitleStyle == other.useSubtitleStyle &&
	       subtitleStyle == other.subtitleStyle && subtitlePresetName == other.subtitlePresetName &&
	       useMetaStyle == other.useMetaStyle && metaStyle == other.metaStyle &&
	       metaPresetName == other.metaPresetName && useHatch == other.useHatch && hatch == other.hatch &&
	       useOpacity == other.useOpacity && qFuzzyCompare(opacity + 1.0, other.opacity + 1.0) &&
	       useFit == other.useFit && fit == other.fit && minPixelSize == other.minPixelSize;
}

void BlockStyle::save(obs_data_t *data) const
{
	obs_data_set_bool(data, "use_panel", usePanel);
	savePanel(data, "panel", panel);
	saveString(data, "panel_preset", panelPresetName);

	obs_data_set_bool(data, "use_title", useTitleStyle);
	saveStyle(data, "title", titleStyle);
	saveString(data, "title_preset", titlePresetName);

	obs_data_set_bool(data, "use_subtitle", useSubtitleStyle);
	saveStyle(data, "subtitle", subtitleStyle);
	saveString(data, "subtitle_preset", subtitlePresetName);

	obs_data_set_bool(data, "use_meta", useMetaStyle);
	saveStyle(data, "meta", metaStyle);
	saveString(data, "meta_preset", metaPresetName);

	obs_data_set_bool(data, "use_hatch", useHatch);
	OBSDataAutoRelease hatchData = obs_data_create();
	hatch.save(hatchData);
	obs_data_set_obj(data, "hatch", hatchData);

	obs_data_set_bool(data, "use_opacity", useOpacity);
	obs_data_set_double(data, "opacity", opacity);

	obs_data_set_bool(data, "use_fit", useFit);
	obs_data_set_string(data, "fit", textFitId(fit));
	obs_data_set_int(data, "min_pixel_size", minPixelSize);
}

void BlockStyle::load(obs_data_t *data)
{
	usePanel = obs_data_get_bool(data, "use_panel");
	loadPanel(data, "panel", &panel);
	panelPresetName = loadString(data, "panel_preset");

	useTitleStyle = obs_data_get_bool(data, "use_title");
	loadStyle(data, "title", &titleStyle);
	titlePresetName = loadString(data, "title_preset");

	useSubtitleStyle = obs_data_get_bool(data, "use_subtitle");
	loadStyle(data, "subtitle", &subtitleStyle);
	subtitlePresetName = loadString(data, "subtitle_preset");

	useMetaStyle = obs_data_get_bool(data, "use_meta");
	loadStyle(data, "meta", &metaStyle);
	metaPresetName = loadString(data, "meta_preset");

	useHatch = obs_data_get_bool(data, "use_hatch");
	OBSDataAutoRelease hatchData = obs_data_get_obj(data, "hatch");
	if (hatchData)
		hatch.load(hatchData);

	useOpacity = obs_data_get_bool(data, "use_opacity");
	opacity = std::clamp(loadDouble(data, "opacity", 1.0), 0.0, 1.0);

	useFit = obs_data_get_bool(data, "use_fit");
	fit = textFitFromId(obs_data_get_string(data, "fit"), TextFit::Shrink);
	minPixelSize = std::clamp(loadInt(data, "min_pixel_size", 11), 4, 200);
}

/* --- the small pieces --------------------------------------------------------------------- */

void ChannelTag::save(obs_data_t *data) const
{
	saveString(data, "label", label);
	obs_data_set_string(data, "glyph", tagGlyphId(glyph));
	saveString(data, "image", imagePath);
}

void ChannelTag::load(obs_data_t *data)
{
	label = loadString(data, "label");
	glyph = tagGlyphFromId(obs_data_get_string(data, "glyph"), TagGlyph::None);
	imagePath = loadString(data, "image");
}

void Chip::save(obs_data_t *data) const
{
	saveString(data, "label", label);
	obs_data_set_string(data, "glyph", tagGlyphId(glyph));
	saveString(data, "image", imagePath);
	obs_data_set_bool(data, "use_color", useColor);
	saveColor(data, "color", color);
}

void Chip::load(obs_data_t *data)
{
	label = loadString(data, "label");
	glyph = tagGlyphFromId(obs_data_get_string(data, "glyph"), TagGlyph::None);
	imagePath = loadString(data, "image");
	useColor = obs_data_get_bool(data, "use_color");
	color = loadColor(data, "color", QColor(255, 255, 255, 40));
}

void CalendarDay::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	saveString(data, "date", date.isValid() ? date.toString(Qt::ISODate) : QString());
	saveString(data, "label", label);
	saveString(data, "sub_label", subLabel);
	obs_data_set_bool(data, "visible", visible);
}

void CalendarDay::load(obs_data_t *data)
{
	id = loadString(data, "id");
	date = QDate::fromString(loadString(data, "date"), Qt::ISODate);
	label = loadString(data, "label");
	subLabel = loadString(data, "sub_label");
	visible = loadBool(data, "visible", true);
}

void CalendarLane::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	saveString(data, "name", name);
	saveString(data, "sub_label", subLabel);
	saveLogo(data, "logo", logo);
	obs_data_set_string(data, "overlap", laneOverlapId(overlap));
	obs_data_set_string(data, "open_ended", openEndedRuleId(openEnded));
	obs_data_set_int(data, "open_ended_minutes", openEndedMinutes);
	saveBlockStyle(data, "style", style);
	saveBlockStyle(data, "header_style", headerStyle);
	obs_data_set_bool(data, "visible", visible);
}

void CalendarLane::load(obs_data_t *data)
{
	id = loadString(data, "id");
	name = loadString(data, "name");
	subLabel = loadString(data, "sub_label");
	loadLogo(data, "logo", &logo);
	overlap = laneOverlapFromId(obs_data_get_string(data, "overlap"), LaneOverlap::Split);
	openEnded = openEndedRuleFromId(obs_data_get_string(data, "open_ended"), OpenEndedRule::UntilNext);
	openEndedMinutes = std::max(1, loadInt(data, "open_ended_minutes", 60));
	loadBlockStyle(data, "style", &style);
	loadBlockStyle(data, "header_style", &headerStyle);
	visible = loadBool(data, "visible", true);
}

void CalendarSlot::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	saveString(data, "name", name);
	obs_data_set_int(data, "start", startMinutes);
	obs_data_set_int(data, "end", endMinutes);
	obs_data_set_double(data, "weight", weight);
	obs_data_set_bool(data, "visible", visible);
}

void CalendarSlot::load(obs_data_t *data)
{
	id = loadString(data, "id");
	name = loadString(data, "name");
	startMinutes = loadInt(data, "start", -1);
	endMinutes = loadInt(data, "end", -1);
	weight = std::max(0.05, loadDouble(data, "weight", 1.0));
	visible = loadBool(data, "visible", true);
}

void CalendarCategory::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	saveString(data, "name", name);
	saveBlockStyle(data, "style", style);
	obs_data_set_bool(data, "in_legend", inLegend);
}

void CalendarCategory::load(obs_data_t *data)
{
	id = loadString(data, "id");
	name = loadString(data, "name");
	loadBlockStyle(data, "style", &style);
	inLegend = loadBool(data, "in_legend", true);
}

void CalendarEvent::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	saveString(data, "title", title);
	saveString(data, "subtitle", subtitle);
	saveString(data, "day", dayId);
	saveString(data, "lane", laneId);
	saveString(data, "category", categoryId);
	obs_data_set_int(data, "start", startMinutes);
	obs_data_set_int(data, "end", endMinutes);
	saveString(data, "slot", slotId);
	saveString(data, "end_slot", endSlotId);
	saveString(data, "zone", timeZone);
	saveLogo(data, "logo", logo);
	saveVector(data, "tags", tags);
	saveString(data, "location", location);
	saveString(data, "notes", notes);
	obs_data_set_string(data, "status", eventStatusId(status));
	obs_data_set_bool(data, "band", band);
	saveString(data, "continuation_of", continuationOf);
	saveBlockStyle(data, "style", style);
	obs_data_set_bool(data, "visible", visible);
}

void CalendarEvent::load(obs_data_t *data)
{
	id = loadString(data, "id");
	title = loadString(data, "title");
	subtitle = loadString(data, "subtitle");
	dayId = loadString(data, "day");
	laneId = loadString(data, "lane");
	categoryId = loadString(data, "category");
	startMinutes = loadInt(data, "start", -1);
	endMinutes = loadInt(data, "end", -1);
	slotId = loadString(data, "slot");
	endSlotId = loadString(data, "end_slot");
	timeZone = loadString(data, "zone");
	loadLogo(data, "logo", &logo);
	loadVector(data, "tags", &tags);
	location = loadString(data, "location");
	notes = loadString(data, "notes");
	status = eventStatusFromId(obs_data_get_string(data, "status"), EventStatus::Auto);
	band = obs_data_get_bool(data, "band");
	continuationOf = loadString(data, "continuation_of");
	loadBlockStyle(data, "style", &style);
	visible = loadBool(data, "visible", true);
}

QString CalendarEvent::displayLabel() const
{
	if (!title.isEmpty())
		return title;
	if (!subtitle.isEmpty())
		return subtitle;
	return QStringLiteral("(untitled)");
}

/* --- elements ----------------------------------------------------------------------------- */

void CalendarElement::save(obs_data_t *data) const
{
	saveString(data, "id", id);
	obs_data_set_string(data, "type", elementTypeId(type));
	saveString(data, "label", label);
	obs_data_set_string(data, "anchor", elementAnchorId(anchor));
	obs_data_set_double(data, "x", x);
	obs_data_set_double(data, "y", y);
	obs_data_set_double(data, "width", width);
	obs_data_set_double(data, "height", height);

	saveString(data, "text", text);
	saveStyle(data, "text_style", textStyle);
	saveString(data, "text_preset", textPresetName);

	savePanel(data, "panel", panel);
	saveString(data, "panel_preset", panelPresetName);
	obs_data_set_double(data, "padding_x", paddingX);
	obs_data_set_double(data, "padding_y", paddingY);

	saveLogo(data, "image", image);

	saveString(data, "clock_format", clockFormat);
	obs_data_set_bool(data, "clock_local", clockUsesLocalZone);
	saveString(data, "clock_zone_label", clockZoneLabel);

	saveStringList(data, "messages", messages);
	obs_data_set_double(data, "message_dwell", messageDwell);

	saveVector(data, "chips", chips);
	obs_data_set_double(data, "chip_gap", chipGap);
	obs_data_set_double(data, "chip_padding_x", chipPaddingX);
	obs_data_set_double(data, "chip_padding_y", chipPaddingY);
	obs_data_set_double(data, "chip_radius", chipRadius);

	obs_data_set_string(data, "legend_source", legendSourceId(legendSource));
	obs_data_set_int(data, "legend_columns", legendColumns);
	obs_data_set_double(data, "legend_swatch", legendSwatchSize);
	obs_data_set_double(data, "legend_gap", legendGap);
	saveStringList(data, "legend_hidden", legendHidden);
	saveStringList(data, "legend_labels", legendLabels);

	obs_data_set_bool(data, "visible", visible);
}

void CalendarElement::load(obs_data_t *data)
{
	id = loadString(data, "id");
	type = elementTypeFromId(obs_data_get_string(data, "type"), ElementType::Text);
	label = loadString(data, "label");
	anchor = elementAnchorFromId(obs_data_get_string(data, "anchor"), ElementAnchor::TopLeft);
	x = loadDouble(data, "x", 0.0);
	y = loadDouble(data, "y", 0.0);
	width = std::max(0.0, loadDouble(data, "width", 400.0));
	height = std::max(0.0, loadDouble(data, "height", 0.0));

	text = loadString(data, "text");
	loadStyle(data, "text_style", &textStyle);
	textPresetName = loadString(data, "text_preset");

	loadPanel(data, "panel", &panel);
	panelPresetName = loadString(data, "panel_preset");
	paddingX = loadDouble(data, "padding_x", 16.0);
	paddingY = loadDouble(data, "padding_y", 10.0);

	loadLogo(data, "image", &image);

	clockFormat = loadString(data, "clock_format", QStringLiteral("h:mm AP"));
	clockUsesLocalZone = obs_data_get_bool(data, "clock_local");
	clockZoneLabel = loadString(data, "clock_zone_label");

	loadStringList(data, "messages", &messages);
	messageDwell = std::max(0.5, loadDouble(data, "message_dwell", 8.0));

	loadVector(data, "chips", &chips);
	chipGap = loadDouble(data, "chip_gap", 12.0);
	chipPaddingX = loadDouble(data, "chip_padding_x", 14.0);
	chipPaddingY = loadDouble(data, "chip_padding_y", 6.0);
	chipRadius = std::max(0.0, loadDouble(data, "chip_radius", 14.0));

	legendSource = legendSourceFromId(obs_data_get_string(data, "legend_source"), LegendSource::Categories);
	legendColumns = std::max(1, loadInt(data, "legend_columns", 1));
	legendSwatchSize = std::max(1.0, loadDouble(data, "legend_swatch", 18.0));
	legendGap = std::max(0.0, loadDouble(data, "legend_gap", 10.0));
	loadStringList(data, "legend_hidden", &legendHidden);
	loadStringList(data, "legend_labels", &legendLabels);

	visible = loadBool(data, "visible", true);
}

CalendarElement CalendarElement::makeDefault(ElementType type)
{
	CalendarElement element;
	element.type = type;

	switch (type) {
	case ElementType::Text:
		element.text = QStringLiteral("Text");
		element.textStyle.pixelSize = 36;
		element.textStyle.bold = true;
		element.textStyle.align = HAlign::Left;
		break;

	case ElementType::Image:
		element.width = 240.0;
		element.height = 120.0;
		element.image.maxHeight = 120;
		break;

	case ElementType::Panel:
		element.width = 400.0;
		element.height = 120.0;
		element.panel.fill = BackgroundFill::Color;
		element.panel.color = QColor(0, 0, 0, 160);
		element.panel.setRadius(8.0);
		break;

	case ElementType::Clock:
		element.width = 240.0;
		element.text = QStringLiteral("LOCAL TIME");
		element.clockUsesLocalZone = true;
		element.textStyle.pixelSize = 44;
		element.textStyle.bold = true;
		element.textStyle.align = HAlign::Center;
		break;

	case ElementType::Ticker:
		element.anchor = ElementAnchor::BottomCenter;
		element.width = 1200.0;
		element.height = 56.0;
		element.messages = QStringList{QStringLiteral("Welcome in")};
		element.textStyle.pixelSize = 28;
		element.textStyle.bold = true;
		element.textStyle.align = HAlign::Center;
		element.panel.fill = BackgroundFill::Color;
		element.panel.color = QColor(20, 24, 34, 220);
		element.panel.setRadius(8.0);
		break;

	case ElementType::ChipRow:
		element.width = 900.0;
		element.chips = QVector<Chip>{Chip{}};
		element.textStyle.pixelSize = 18;
		element.textStyle.bold = true;
		element.textStyle.align = HAlign::Center;
		break;

	case ElementType::Legend:
		element.width = 320.0;
		element.textStyle.pixelSize = 18;
		element.textStyle.align = HAlign::Left;
		break;
	}

	return element;
}

QString CalendarElement::displayLabel() const
{
	if (!label.isEmpty())
		return label;

	switch (type) {
	case ElementType::Text:
	case ElementType::Ticker:
		if (!text.isEmpty())
			return text;
		if (!messages.isEmpty())
			return messages.first();
		break;
	case ElementType::Clock:
		return QString::fromUtf8(elementTypeName(type));
	default:
		break;
	}

	return QString::fromUtf8(elementTypeName(type));
}

/* --- settings groups ---------------------------------------------------------------------- */

void GridSettings::save(obs_data_t *data) const
{
	obs_data_set_string(data, "gutter", gutterSideId(gutter));
	obs_data_set_double(data, "gutter_width", gutterWidth);
	obs_data_set_bool(data, "gutter_24h", gutter24Hour);
	obs_data_set_int(data, "gutter_step", gutterStep);
	obs_data_set_bool(data, "gutter_slot_times", gutterShowSlotTimes);
	saveStyle(data, "gutter_style", gutterStyle);
	saveString(data, "gutter_preset", gutterPresetName);
	savePanel(data, "gutter_panel", gutterPanel);
	saveString(data, "gutter_panel_preset", gutterPanelPresetName);

	obs_data_set_bool(data, "lane_headers", showLaneHeaders);
	obs_data_set_double(data, "lane_header_size", laneHeaderSize);
	obs_data_set_bool(data, "lane_header_logos", laneHeaderLogos);
	saveStyle(data, "lane_style", laneStyle);
	saveString(data, "lane_preset", lanePresetName);
	savePanel(data, "lane_panel", lanePanel);
	saveString(data, "lane_panel_preset", lanePanelPresetName);

	obs_data_set_bool(data, "day_headers", showDayHeaders);
	obs_data_set_double(data, "day_header_height", dayHeaderHeight);
	saveString(data, "day_format", dayFormat);
	saveString(data, "date_format", dateFormat);
	saveStyle(data, "day_style", dayStyle);
	saveString(data, "day_preset", dayPresetName);
	savePanel(data, "day_panel", dayPanel);
	saveString(data, "day_panel_preset", dayPanelPresetName);
	obs_data_set_bool(data, "days_as_columns", daysAsColumns);

	obs_data_set_double(data, "pixels_per_hour", pixelsPerHour);
	obs_data_set_double(data, "slot_size", slotSize);
	obs_data_set_double(data, "lane_size", laneSize);
	obs_data_set_double(data, "lane_gap", laneGap);
	obs_data_set_double(data, "block_inset", blockInset);
	obs_data_set_double(data, "day_gap", dayGap);

	obs_data_set_bool(data, "time_lines", showTimeLines);
	saveColor(data, "time_line_color", timeLineColor);
	obs_data_set_double(data, "time_line_width", timeLineWidth);
	obs_data_set_bool(data, "lane_lines", showLaneLines);
	saveColor(data, "lane_line_color", laneLineColor);
	obs_data_set_double(data, "lane_line_width", laneLineWidth);

	obs_data_set_int(data, "axis_start", axisStart);
	obs_data_set_int(data, "axis_end", axisEnd);

	obs_data_set_bool(data, "block_times", showBlockTimes);
	obs_data_set_bool(data, "block_subtitles", showBlockSubtitles);
	obs_data_set_bool(data, "block_tags", showBlockTags);
	obs_data_set_bool(data, "block_location", showBlockLocation);
	obs_data_set_bool(data, "block_logos", showBlockLogos);
	obs_data_set_double(data, "block_logo_height", blockLogoHeight);
	obs_data_set_double(data, "block_padding_x", blockPaddingX);
	obs_data_set_double(data, "block_padding_y", blockPaddingY);
	obs_data_set_double(data, "block_gap", blockGap);
}

void GridSettings::load(obs_data_t *data)
{
	gutter = gutterSideFromId(obs_data_get_string(data, "gutter"), GutterSide::Leading);
	gutterWidth = std::max(0.0, loadDouble(data, "gutter_width", 110.0));
	gutter24Hour = obs_data_get_bool(data, "gutter_24h");
	gutterStep = std::max(5, loadInt(data, "gutter_step", 60));
	gutterShowSlotTimes = loadBool(data, "gutter_slot_times", true);
	loadStyle(data, "gutter_style", &gutterStyle);
	gutterPresetName = loadString(data, "gutter_preset");
	loadPanel(data, "gutter_panel", &gutterPanel);
	gutterPanelPresetName = loadString(data, "gutter_panel_preset");

	showLaneHeaders = loadBool(data, "lane_headers", true);
	laneHeaderSize = std::max(0.0, loadDouble(data, "lane_header_size", 64.0));
	laneHeaderLogos = loadBool(data, "lane_header_logos", true);
	loadStyle(data, "lane_style", &laneStyle);
	lanePresetName = loadString(data, "lane_preset");
	loadPanel(data, "lane_panel", &lanePanel);
	lanePanelPresetName = loadString(data, "lane_panel_preset");

	showDayHeaders = loadBool(data, "day_headers", true);
	dayHeaderHeight = std::max(0.0, loadDouble(data, "day_header_height", 88.0));
	dayFormat = loadString(data, "day_format", QStringLiteral("dddd"));
	dateFormat = loadString(data, "date_format", QStringLiteral("M/d"));
	loadStyle(data, "day_style", &dayStyle);
	dayPresetName = loadString(data, "day_preset");
	loadPanel(data, "day_panel", &dayPanel);
	dayPanelPresetName = loadString(data, "day_panel_preset");
	daysAsColumns = loadBool(data, "days_as_columns", true);

	pixelsPerHour = std::max(1.0, loadDouble(data, "pixels_per_hour", 90.0));
	slotSize = std::max(1.0, loadDouble(data, "slot_size", 120.0));
	laneSize = std::max(1.0, loadDouble(data, "lane_size", 220.0));
	laneGap = std::max(0.0, loadDouble(data, "lane_gap", 6.0));
	blockInset = std::max(0.0, loadDouble(data, "block_inset", 2.0));
	dayGap = std::max(0.0, loadDouble(data, "day_gap", 24.0));

	showTimeLines = loadBool(data, "time_lines", true);
	timeLineColor = loadColor(data, "time_line_color", QColor(255, 255, 255, 40));
	timeLineWidth = std::max(0.0, loadDouble(data, "time_line_width", 1.0));
	showLaneLines = loadBool(data, "lane_lines", true);
	laneLineColor = loadColor(data, "lane_line_color", QColor(255, 255, 255, 25));
	laneLineWidth = std::max(0.0, loadDouble(data, "lane_line_width", 1.0));

	axisStart = loadInt(data, "axis_start", -1);
	axisEnd = loadInt(data, "axis_end", -1);

	showBlockTimes = loadBool(data, "block_times", true);
	showBlockSubtitles = loadBool(data, "block_subtitles", true);
	showBlockTags = loadBool(data, "block_tags", true);
	showBlockLocation = loadBool(data, "block_location", false);
	showBlockLogos = loadBool(data, "block_logos", true);
	blockLogoHeight = std::max(1.0, loadDouble(data, "block_logo_height", 28.0));
	blockPaddingX = std::max(0.0, loadDouble(data, "block_padding_x", 10.0));
	blockPaddingY = std::max(0.0, loadDouble(data, "block_padding_y", 6.0));
	blockGap = std::max(0.0, loadDouble(data, "block_gap", 4.0));
}

bool LiveSettings::needsClock() const
{
	return nowLine || dimFinished || highlightCurrent || dropFinished;
}

void LiveSettings::save(obs_data_t *data) const
{
	obs_data_set_bool(data, "now_line", nowLine);
	saveColor(data, "now_line_color", nowLineColor);
	obs_data_set_double(data, "now_line_width", nowLineWidth);
	saveString(data, "now_line_label", nowLineLabel);

	obs_data_set_bool(data, "dim_finished", dimFinished);
	obs_data_set_double(data, "finished_opacity", finishedOpacity);

	obs_data_set_bool(data, "highlight_current", highlightCurrent);
	saveBlockStyle(data, "current_style", currentStyle);

	obs_data_set_bool(data, "drop_finished", dropFinished);
	obs_data_set_int(data, "drop_grace", dropGraceMinutes);

	obs_data_set_int(data, "refresh_seconds", refreshSeconds);
}

void LiveSettings::load(obs_data_t *data)
{
	nowLine = obs_data_get_bool(data, "now_line");
	nowLineColor = loadColor(data, "now_line_color", QColor(255, 80, 80));
	nowLineWidth = std::max(0.0, loadDouble(data, "now_line_width", 3.0));
	nowLineLabel = loadString(data, "now_line_label");

	dimFinished = obs_data_get_bool(data, "dim_finished");
	finishedOpacity = std::clamp(loadDouble(data, "finished_opacity", 0.35), 0.0, 1.0);

	highlightCurrent = obs_data_get_bool(data, "highlight_current");
	loadBlockStyle(data, "current_style", &currentStyle);

	dropFinished = obs_data_get_bool(data, "drop_finished");
	dropGraceMinutes = std::max(0, loadInt(data, "drop_grace", 0));

	refreshSeconds = std::clamp(loadInt(data, "refresh_seconds", 30), 1, 3600);
}

void OverflowSettings::save(obs_data_t *data) const
{
	obs_data_set_string(data, "mode", overflowModeId(mode));
	obs_data_set_double(data, "min_scale", minScale);
	obs_data_set_double(data, "page_dwell", pageDwell);
	obs_data_set_bool(data, "page_by_day", pageByDay);
	obs_data_set_double(data, "scroll_speed", scrollSpeed);
	obs_data_set_double(data, "scroll_pause", scrollPause);
}

void OverflowSettings::load(obs_data_t *data)
{
	mode = overflowModeFromId(obs_data_get_string(data, "mode"), OverflowMode::Fit);
	minScale = std::clamp(loadDouble(data, "min_scale", 0.35), 0.05, 1.0);
	pageDwell = std::max(1.0, loadDouble(data, "page_dwell", 12.0));
	pageByDay = loadBool(data, "page_by_day", true);
	scrollSpeed = std::max(1.0, loadDouble(data, "scroll_speed", 40.0));
	scrollPause = std::max(0.0, loadDouble(data, "scroll_pause", 3.0));
}

/* --- lookups ------------------------------------------------------------------------------ */

const CalendarDay *CalendarDocument::findDay(const QString &id) const
{
	if (id.isEmpty())
		return nullptr;
	for (const CalendarDay &day : days) {
		if (day.id == id)
			return &day;
	}
	return nullptr;
}

const CalendarLane *CalendarDocument::findLane(const QString &id) const
{
	if (id.isEmpty())
		return nullptr;
	for (const CalendarLane &lane : lanes) {
		if (lane.id == id)
			return &lane;
	}
	return nullptr;
}

const CalendarSlot *CalendarDocument::findSlot(const QString &id) const
{
	if (id.isEmpty())
		return nullptr;
	for (const CalendarSlot &slot : timeSlots) {
		if (slot.id == id)
			return &slot;
	}
	return nullptr;
}

const CalendarCategory *CalendarDocument::findCategory(const QString &id) const
{
	if (id.isEmpty())
		return nullptr;
	for (const CalendarCategory &category : categories) {
		if (category.id == id)
			return &category;
	}
	return nullptr;
}

const CalendarEvent *CalendarDocument::findEvent(const QString &id) const
{
	if (id.isEmpty())
		return nullptr;
	for (const CalendarEvent &event : events) {
		if (event.id == id)
			return &event;
	}
	return nullptr;
}

int CalendarDocument::dayIndex(const QString &id) const
{
	for (int i = 0; i < days.size(); ++i) {
		if (days[i].id == id)
			return i;
	}
	return -1;
}

int CalendarDocument::laneIndex(const QString &id) const
{
	for (int i = 0; i < lanes.size(); ++i) {
		if (lanes[i].id == id)
			return i;
	}
	return -1;
}

int CalendarDocument::slotIndex(const QString &id) const
{
	for (int i = 0; i < timeSlots.size(); ++i) {
		if (timeSlots[i].id == id)
			return i;
	}
	return -1;
}

QString CalendarDocument::makeId(const QString &prefix, const QStringList &existing)
{
	for (int n = 1;; ++n) {
		const QString candidate = QStringLiteral("%1%2").arg(prefix).arg(n);
		if (!existing.contains(candidate))
			return candidate;
	}
}

QString CalendarDocument::makeEventId() const
{
	QStringList used;
	used.reserve(events.size());
	for (const CalendarEvent &event : events)
		used.append(event.id);
	return makeId(QStringLiteral("event"), used);
}

QString CalendarDocument::makeLaneId() const
{
	QStringList used;
	used.reserve(lanes.size());
	for (const CalendarLane &lane : lanes)
		used.append(lane.id);
	return makeId(QStringLiteral("lane"), used);
}

QString CalendarDocument::makeDayId() const
{
	QStringList used;
	used.reserve(days.size());
	for (const CalendarDay &day : days)
		used.append(day.id);
	return makeId(QStringLiteral("day"), used);
}

QString CalendarDocument::makeSlotId() const
{
	QStringList used;
	used.reserve(timeSlots.size());
	for (const CalendarSlot &slot : timeSlots)
		used.append(slot.id);
	return makeId(QStringLiteral("slot"), used);
}

QString CalendarDocument::makeCategoryId() const
{
	QStringList used;
	used.reserve(categories.size());
	for (const CalendarCategory &category : categories)
		used.append(category.id);
	return makeId(QStringLiteral("category"), used);
}

QString CalendarDocument::makeElementId() const
{
	QStringList used;
	used.reserve(elements.size());
	for (const CalendarElement &element : elements)
		used.append(element.id);
	return makeId(QStringLiteral("element"), used);
}

/* --- time --------------------------------------------------------------------------------- */

QTimeZone CalendarDocument::displayZone() const
{
	if (zoneMode == ClockZoneMode::Fixed && !timeZone.isEmpty()) {
		const QTimeZone named(timeZone.toUtf8());
		if (named.isValid())
			return named;
	}
	return QTimeZone::systemTimeZone();
}

namespace {

/*
 * The date an event is drawn on.
 *
 * A day with no date of its own is still a day -- a board of waves may never mention a date at all
 * -- so it borrows today's. That keeps every clock comparison meaningful on a board whose author
 * never typed a date, and costs nothing on one that did.
 */
QDate dateFor(const CalendarDocument &document, const CalendarEvent &event)
{
	if (const CalendarDay *day = document.findDay(event.dayId)) {
		if (day->date.isValid())
			return day->date;
	}
	return QDate::currentDate();
}

/* The minutes an event starts at on the axis, before any open end is resolved. -1 when unplaceable. */
int rawStartMinutes(const CalendarDocument &document, const CalendarEvent &event)
{
	if (event.startMinutes >= 0)
		return event.startMinutes;

	if (const CalendarSlot *slot = document.findSlot(event.slotId)) {
		if (slot->hasTime())
			return slot->startMinutes;
	}
	return -1;
}

int rawEndMinutes(const CalendarDocument &document, const CalendarEvent &event)
{
	if (event.endMinutes >= 0)
		return event.endMinutes;

	const CalendarSlot *slot = document.findSlot(event.endSlotId.isEmpty() ? event.slotId : event.endSlotId);
	if (slot && slot->endMinutes >= 0)
		return slot->endMinutes;

	return -1;
}

} // namespace

QDateTime CalendarDocument::eventStart(const CalendarEvent &event) const
{
	const int minutes = rawStartMinutes(*this, event);
	if (minutes < 0)
		return QDateTime();

	const QTimeZone zone = event.timeZone.isEmpty() ? displayZone() : QTimeZone(event.timeZone.toUtf8());
	const QDateTime stated(dateFor(*this, event), QTime(0, 0).addSecs(minutes * 60),
			       zone.isValid() ? zone : displayZone());
	return stated.toTimeZone(displayZone());
}

QDateTime CalendarDocument::eventEnd(const CalendarEvent &event) const
{
	const QDateTime start = eventStart(event);
	if (!start.isValid())
		return QDateTime();

	const EventSpan span = eventSpan(event);
	if (!span.valid)
		return QDateTime();

	const int length = std::max(0, span.endMinutes - span.startMinutes);
	return start.addSecs(length * 60);
}

EventSpan CalendarDocument::eventSpan(const CalendarEvent &event) const
{
	EventSpan span;

	/*
	 * The clock half is resolved even on a slotted board, because a slot may carry a time and the
	 * live features read minutes rather than slot indices. An event the clock cannot place at all
	 * simply comes back with the slot half filled in and no minutes, which every caller reading
	 * minutes treats as "no opinion" rather than as midnight.
	 */
	int start = rawStartMinutes(*this, event);

	/*
	 * A zone override is applied by converting the stated time into the board's own zone, which
	 * is the only reading that puts two feeds on one axis. An event that lands on another date by
	 * doing so still draws on the day it was given -- its minutes simply run past midnight or
	 * before it, which the axis clamps and the designer can see.
	 */
	if (start >= 0 && !event.timeZone.isEmpty()) {
		const QTimeZone zone(event.timeZone.toUtf8());
		if (zone.isValid()) {
			const QDate date = dateFor(*this, event);
			const QDateTime stated(date, QTime(0, 0).addSecs(start * 60), zone);
			const QDateTime local = stated.toTimeZone(displayZone());
			start = static_cast<int>(QDateTime(date, QTime(0, 0), displayZone()).secsTo(local) / 60);
		}
	}

	int end = rawEndMinutes(*this, event);

	if (start >= 0) {
		span.valid = true;
		span.startMinutes = start;

		if (end > start) {
			span.endMinutes = end;
		} else {
			span.openEnded = true;
			span.endMinutes = start + resolvedOpenEndedLength(event, start);
		}
	}

	/* The slot half. A stated slot wins; otherwise the clock time is dropped into the slot holding it. */
	const int stated = slotIndex(event.slotId);
	if (stated >= 0) {
		span.startSlot = stated;
		span.valid = true;
	} else if (start >= 0) {
		for (int i = 0; i < timeSlots.size(); ++i) {
			if (!timeSlots[i].hasTime())
				continue;
			const int slotEnd = timeSlots[i].endMinutes >= 0 ? timeSlots[i].endMinutes
									 : timeSlots[i].startMinutes + 90;
			if (start >= timeSlots[i].startMinutes && start < slotEnd) {
				span.startSlot = i;
				break;
			}
		}
	}

	const int statedEnd = slotIndex(event.endSlotId);
	span.endSlot = statedEnd >= 0 ? statedEnd : span.startSlot;
	if (span.endSlot < span.startSlot)
		span.endSlot = span.startSlot;

	return span;
}

int CalendarDocument::resolvedOpenEndedLength(const CalendarEvent &event, int startMinutes) const
{
	const CalendarLane *lane = findLane(event.laneId);
	const OpenEndedRule rule = lane ? lane->openEnded : OpenEndedRule::UntilNext;
	const int fallback = lane ? lane->openEndedMinutes : 60;

	switch (rule) {
	case OpenEndedRule::Fixed:
		return fallback;

	case OpenEndedRule::Minimal:
		/*
		 * "As small as it fits" is a layout answer, not a schedule one -- how tall the block's own
		 * text turns out to be. The model cannot measure text, so it reports the smallest span it
		 * is willing to call a duration and the layout grows the block past it when the words need
		 * the room. Fifteen minutes rather than zero so a block still reads as occupying a time.
		 */
		return 15;

	case OpenEndedRule::UntilNext:
	default:
		break;
	}

	/*
	 * The next event in the same lane on the same day, which is exactly what a schedule printing
	 * only start times means. Falling back to the lane's own length when there is nothing after it
	 * keeps the last event of the night from running to midnight.
	 */
	int nearest = -1;
	for (const CalendarEvent &other : events) {
		if (&other == &event || !other.visible || other.band != event.band)
			continue;
		if (other.laneId != event.laneId || other.dayId != event.dayId)
			continue;

		const int otherStart = rawStartMinutes(*this, other);
		if (otherStart <= startMinutes)
			continue;
		if (nearest < 0 || otherStart < nearest)
			nearest = otherStart;
	}

	return nearest > startMinutes ? nearest - startMinutes : fallback;
}

EventStatus CalendarDocument::statusAt(const CalendarEvent &event, const QDateTime &now) const
{
	if (event.status != EventStatus::Auto)
		return event.status;

	const QDateTime start = eventStart(event);
	if (!start.isValid() || !now.isValid())
		return EventStatus::Upcoming;

	if (now < start)
		return EventStatus::Upcoming;

	const QDateTime end = eventEnd(event);
	if (end.isValid() && now >= end)
		return EventStatus::Finished;

	return EventStatus::Live;
}

void CalendarDocument::axisRange(int *start, int *end) const
{
	int from = grid.axisStart;
	int to = grid.axisEnd;

	if (from < 0 || to < 0 || to <= from) {
		int lowest = -1;
		int highest = -1;

		for (const CalendarEvent &event : events) {
			if (!event.visible)
				continue;

			const EventSpan span = eventSpan(event);
			if (!span.valid)
				continue;

			if (lowest < 0 || span.startMinutes < lowest)
				lowest = span.startMinutes;
			if (highest < 0 || span.endMinutes > highest)
				highest = span.endMinutes;
		}

		if (lowest < 0) {
			/* An empty board still needs an axis to draw, so it gets a working day. */
			lowest = 10 * 60;
			highest = 22 * 60;
		}

		/* Rounded out to whole hours, so the gutter's labels land on the board's own edges. */
		if (from < 0)
			from = (lowest / 60) * 60;
		if (to < 0 || to <= from)
			to = ((highest + 59) / 60) * 60;
	}

	if (to <= from)
		to = from + 60;

	if (start)
		*start = from;
	if (end)
		*end = to;
}

bool CalendarDocument::needsClock() const
{
	if (live.needsClock())
		return true;

	/*
	 * A clock element is the other half of the answer: a board with no live features at all still
	 * has to be redrawn every minute if it is showing the time in the corner.
	 */
	for (const CalendarElement &element : elements) {
		if (!element.visible)
			continue;
		if (element.type == ElementType::Clock)
			return true;
		/* A ticker with more than one message rotates, which is the same kind of dependency. */
		if (element.type == ElementType::Ticker && element.messages.size() > 1)
			return true;
	}

	return false;
}

/* --- styling ------------------------------------------------------------------------------ */

const TextStyle *CalendarDocument::findStylePreset(const QString &name) const
{
	if (name.isEmpty())
		return nullptr;
	for (const StylePreset &preset : stylePresets) {
		if (preset.name == name)
			return &preset.style;
	}
	return nullptr;
}

const BackgroundPanel *CalendarDocument::findBackgroundPreset(const QString &name) const
{
	if (name.isEmpty())
		return nullptr;
	for (const BackgroundPreset &preset : backgroundPresets) {
		if (preset.name == name)
			return &preset.panel;
	}
	return nullptr;
}

ResolvedBlockStyle CalendarDocument::resolveStyle(const BlockStyle &layer) const
{
	ResolvedBlockStyle resolved = builtInBlockStyle();

	if (layer.usePanel) {
		const BackgroundPanel *preset = findBackgroundPreset(layer.panelPresetName);
		resolved.panel = preset ? *preset : layer.panel;
	}
	if (layer.useTitleStyle) {
		const TextStyle *preset = findStylePreset(layer.titlePresetName);
		resolved.titleStyle = preset ? *preset : layer.titleStyle;
	}
	if (layer.useSubtitleStyle) {
		const TextStyle *preset = findStylePreset(layer.subtitlePresetName);
		resolved.subtitleStyle = preset ? *preset : layer.subtitleStyle;
	}
	if (layer.useMetaStyle) {
		const TextStyle *preset = findStylePreset(layer.metaPresetName);
		resolved.metaStyle = preset ? *preset : layer.metaStyle;
	}
	if (layer.useHatch)
		resolved.hatch = layer.hatch;
	if (layer.useOpacity)
		resolved.opacity = layer.opacity;
	if (layer.useFit) {
		resolved.fit = layer.fit;
		resolved.minPixelSize = layer.minPixelSize;
	}

	return resolved;
}

ResolvedBlockStyle CalendarDocument::resolveBlockStyle(const CalendarEvent &event, EventStatus status) const
{
	/*
	 * The cascade, in one place and in one order: the document's own layer, the lane's, the
	 * category's, the status's, the continuation marking, and last the event's own override.
	 * Every paint site goes through here, so no site can quietly skip a layer.
	 */
	BlockStyle merged = blockStyle;

	if (const CalendarLane *lane = findLane(event.laneId))
		merged.applyOver(lane->style);

	if (const CalendarCategory *category = findCategory(event.categoryId))
		merged.applyOver(category->style);

	switch (status) {
	case EventStatus::Upcoming:
		merged.applyOver(upcomingStyle);
		break;
	case EventStatus::Live:
		merged.applyOver(liveStyle);
		if (live.highlightCurrent)
			merged.applyOver(live.currentStyle);
		break;
	case EventStatus::Finished:
		merged.applyOver(finishedStyle);
		break;
	case EventStatus::Cancelled:
		merged.applyOver(cancelledStyle);
		break;
	case EventStatus::Auto:
		break;
	}

	if (!event.continuationOf.isEmpty())
		merged.applyOver(continuationStyle);

	merged.applyOver(event.style);

	ResolvedBlockStyle resolved = resolveStyle(merged);

	/*
	 * Dimming is applied after the cascade rather than as a layer inside it, because it is a
	 * setting about the *board* rather than a style anyone chose for these events: a schedule that
	 * has been given a finished style of its own should still be dimmed by it, not instead of it.
	 */
	if (live.dimFinished && status == EventStatus::Finished)
		resolved.opacity *= live.finishedOpacity;

	return resolved;
}

const TextStyle &CalendarDocument::effectiveElementStyle(const CalendarElement &element) const
{
	if (const TextStyle *preset = findStylePreset(element.textPresetName))
		return *preset;
	return element.textStyle;
}

const BackgroundPanel &CalendarDocument::effectiveElementPanel(const CalendarElement &element) const
{
	if (const BackgroundPanel *preset = findBackgroundPreset(element.panelPresetName))
		return *preset;
	return element.panel;
}

void CalendarDocument::setStylePreset(const QString &name, const TextStyle &style)
{
	if (name.isEmpty())
		return;

	for (StylePreset &preset : stylePresets) {
		if (preset.name == name) {
			preset.style = style;
			return;
		}
	}

	StylePreset preset;
	preset.name = name;
	preset.style = style;
	stylePresets.append(preset);
}

void CalendarDocument::removeStylePreset(const QString &name)
{
	if (name.isEmpty())
		return;

	stylePresets.erase(std::remove_if(stylePresets.begin(), stylePresets.end(),
					  [&name](const StylePreset &preset) { return preset.name == name; }),
			   stylePresets.end());

	visitStylePresetNames(*this, [&name](QString &binding) {
		if (binding == name)
			binding.clear();
	});
}

void CalendarDocument::setBackgroundPreset(const QString &name, const BackgroundPanel &panel)
{
	if (name.isEmpty())
		return;

	for (BackgroundPreset &preset : backgroundPresets) {
		if (preset.name == name) {
			preset.panel = panel;
			return;
		}
	}

	BackgroundPreset preset;
	preset.name = name;
	preset.panel = panel;
	backgroundPresets.append(preset);
}

void CalendarDocument::removeBackgroundPreset(const QString &name)
{
	if (name.isEmpty())
		return;

	backgroundPresets.erase(std::remove_if(backgroundPresets.begin(), backgroundPresets.end(),
					       [&name](const BackgroundPreset &preset) { return preset.name == name; }),
				backgroundPresets.end());

	visitBackgroundPresetNames(*this, [&name](QString &binding) {
		if (binding == name)
			binding.clear();
	});
}

bool CalendarDocument::linkStylePreset(const QString &name)
{
	TextStyle style;
	if (!StyleLibrary::instance().find(name, &style))
		return false;

	for (StylePreset &preset : stylePresets) {
		if (preset.name == name) {
			preset.style = style;
			preset.linked = true;
			return true;
		}
	}

	StylePreset preset;
	preset.name = name;
	preset.style = style;
	preset.linked = true;
	stylePresets.append(preset);
	return true;
}

bool CalendarDocument::linkBackgroundPreset(const QString &name)
{
	BackgroundPanel panel;
	if (!StyleLibrary::instance().findBackground(name, &panel))
		return false;

	for (BackgroundPreset &preset : backgroundPresets) {
		if (preset.name == name) {
			preset.panel = panel;
			preset.linked = true;
			return true;
		}
	}

	BackgroundPreset preset;
	preset.name = name;
	preset.panel = panel;
	preset.linked = true;
	backgroundPresets.append(preset);
	return true;
}

bool CalendarDocument::applyLibraryRenames()
{
	const StyleLibrary &library = StyleLibrary::instance();
	bool changed = false;

	for (StylePreset &preset : stylePresets) {
		if (!preset.linked)
			continue;

		QString renamed;
		if (!library.renamedTo(preset.name, &renamed) || !library.contains(renamed))
			continue;

		const bool taken = std::any_of(stylePresets.cbegin(), stylePresets.cend(),
					       [&renamed](const StylePreset &other) { return other.name == renamed; });
		if (taken)
			continue;

		const QString from = preset.name;
		preset.name = renamed;
		visitStylePresetNames(*this, [&](QString &binding) {
			if (binding == from)
				binding = renamed;
		});
		changed = true;
	}

	for (BackgroundPreset &preset : backgroundPresets) {
		if (!preset.linked)
			continue;

		QString renamed;
		if (!library.backgroundRenamedTo(preset.name, &renamed) || !library.containsBackground(renamed))
			continue;

		const bool taken =
			std::any_of(backgroundPresets.cbegin(), backgroundPresets.cend(),
				    [&renamed](const BackgroundPreset &other) { return other.name == renamed; });
		if (taken)
			continue;

		const QString from = preset.name;
		preset.name = renamed;
		visitBackgroundPresetNames(*this, [&](QString &binding) {
			if (binding == from)
				binding = renamed;
		});
		changed = true;
	}

	return changed;
}

bool CalendarDocument::refreshLinkedPresets()
{
	const StyleLibrary &library = StyleLibrary::instance();
	bool changed = applyLibraryRenames();

	if (refreshLinkedBackgroundPresets())
		changed = true;

	for (StylePreset &preset : stylePresets) {
		if (!preset.linked)
			continue;

		TextStyle style;
		if (!library.find(preset.name, &style) || style == preset.style)
			continue;

		preset.style = style;
		changed = true;
	}

	return changed;
}

bool CalendarDocument::refreshLinkedBackgroundPresets()
{
	const StyleLibrary &library = StyleLibrary::instance();
	bool changed = false;

	for (BackgroundPreset &preset : backgroundPresets) {
		if (!preset.linked)
			continue;

		BackgroundPanel panel;
		if (!library.findBackground(preset.name, &panel) || panel == preset.panel)
			continue;

		preset.panel = panel;
		changed = true;
	}

	return changed;
}

/* --- housekeeping ------------------------------------------------------------------------- */

bool CalendarDocument::syncDays()
{
	bool changed = false;

	/*
	 * A board with no day at all cannot place an event, so one is made rather than the schedule
	 * being quietly undrawable. Every other day is the user's: this never removes one, because a
	 * day emptied while its events are being retyped is not a day anybody meant to delete.
	 */
	if (days.isEmpty()) {
		CalendarDay day;
		day.id = makeDayId();
		day.date = QDate::currentDate();
		days.append(day);
		changed = true;
	}

	for (CalendarEvent &event : events) {
		if (findDay(event.dayId))
			continue;
		event.dayId = days.first().id;
		changed = true;
	}

	return changed;
}

QVector<OverlapReport> CalendarDocument::overlaps() const
{
	QVector<OverlapReport> reports;

	struct Placed {
		int index;
		int start;
		int end;
	};

	/* Grouped by day and lane, since two events in different lanes never collide by definition. */
	for (const CalendarDay &day : days) {
		for (const CalendarLane &lane : lanes) {
			QVector<Placed> placed;

			for (int i = 0; i < events.size(); ++i) {
				const CalendarEvent &event = events[i];
				if (!event.visible || event.band)
					continue;
				if (event.dayId != day.id || event.laneId != lane.id)
					continue;

				const EventSpan span = eventSpan(event);
				if (!span.valid)
					continue;

				placed.append({i, span.startMinutes, span.endMinutes});
			}

			std::sort(placed.begin(), placed.end(),
				  [](const Placed &a, const Placed &b) { return a.start < b.start; });

			for (int i = 1; i < placed.size(); ++i) {
				if (placed[i].start >= placed[i - 1].end)
					continue;

				OverlapReport report;
				report.firstEvent = placed[i - 1].index;
				report.secondEvent = placed[i].index;
				report.laneId = lane.id;
				reports.append(report);
			}
		}
	}

	return reports;
}

QVector<int> CalendarDocument::orderedEvents() const
{
	QVector<int> order;
	order.reserve(events.size());
	for (int i = 0; i < events.size(); ++i)
		order.append(i);

	std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
		const int dayA = dayIndex(events[a].dayId);
		const int dayB = dayIndex(events[b].dayId);
		if (dayA != dayB)
			return dayA < dayB;

		const EventSpan spanA = eventSpan(events[a]);
		const EventSpan spanB = eventSpan(events[b]);
		if (spanA.startMinutes != spanB.startMinutes)
			return spanA.startMinutes < spanB.startMinutes;

		return laneIndex(events[a].laneId) < laneIndex(events[b].laneId);
	});

	return order;
}

/* --- fonts -------------------------------------------------------------------------------- */

QVector<FontUse> CalendarDocument::usedFonts() const
{
	QVector<FontUse> uses;

	visitTextStyles(*this, [&uses](const TextStyle &style) {
		if (style.family.isEmpty())
			return;

		for (FontUse &use : uses) {
			if (use.family != style.family)
				continue;
			if (!use.styleNames.contains(style.styleName))
				use.styleNames.append(style.styleName);
			return;
		}

		FontUse use;
		use.family = style.family;
		use.styleNames.append(style.styleName);
		uses.append(use);
	});

	std::sort(uses.begin(), uses.end(), [](const FontUse &a, const FontUse &b) { return a.family < b.family; });
	for (FontUse &use : uses)
		use.styleNames.sort();

	return uses;
}

QStringList CalendarDocument::usedFontFamilies() const
{
	QStringList families;
	for (const FontUse &use : usedFonts())
		families.append(use.family);
	return families;
}

QString CalendarDocument::fontSubstitute(const QString &family) const
{
	for (const FontSubstitution &substitution : fontSubstitutions) {
		if (substitution.from == family)
			return substitution.to;
	}
	return QString();
}

void CalendarDocument::setFontSubstitute(const QString &from, const QString &to)
{
	if (from.isEmpty())
		return;

	for (int i = 0; i < fontSubstitutions.size(); ++i) {
		if (fontSubstitutions[i].from != from)
			continue;

		if (to.isEmpty())
			fontSubstitutions.remove(i);
		else
			fontSubstitutions[i].to = to;
		return;
	}

	if (to.isEmpty())
		return;

	FontSubstitution substitution;
	substitution.from = from;
	substitution.to = to;
	fontSubstitutions.append(substitution);
}

bool CalendarDocument::applyFontSubstitutions(const QStringList &families)
{
	if (families.isEmpty())
		return false;

	bool changed = false;

	const auto rewrite = [&](TextStyle &style) {
		if (!families.contains(style.family))
			return;

		const QString substitute = fontSubstitute(style.family);
		if (substitute.isEmpty() || substitute == style.family)
			return;

		style.family = substitute;
		/* The stand-in's own faces are not this family's, so the named face is dropped with it. */
		style.styleName.clear();
		changed = true;
	};

	const auto rewriteLayer = [&](BlockStyle &layer) {
		rewrite(layer.titleStyle);
		rewrite(layer.subtitleStyle);
		rewrite(layer.metaStyle);
	};

	rewriteLayer(blockStyle);
	rewriteLayer(upcomingStyle);
	rewriteLayer(liveStyle);
	rewriteLayer(finishedStyle);
	rewriteLayer(cancelledStyle);
	rewriteLayer(continuationStyle);
	rewriteLayer(live.currentStyle);

	for (CalendarLane &lane : lanes) {
		rewriteLayer(lane.style);
		rewriteLayer(lane.headerStyle);
	}
	for (CalendarCategory &category : categories)
		rewriteLayer(category.style);
	for (CalendarEvent &event : events)
		rewriteLayer(event.style);

	rewrite(grid.gutterStyle);
	rewrite(grid.laneStyle);
	rewrite(grid.dayStyle);

	for (CalendarElement &element : elements)
		rewrite(element.textStyle);

	for (StylePreset &preset : stylePresets)
		rewrite(preset.style);

	return changed;
}

bool CalendarDocument::refreshFontBundle(QStringList *skipped, bool recollect)
{
	if (!bundleFonts) {
		if (bundledFonts.isEmpty())
			return false;
		bundledFonts.clear();
		return true;
	}

	const QVector<FontUse> uses = usedFonts();

	QVector<FontUse> wanted;
	if (recollect) {
		wanted = uses;
	} else {
		for (const FontUse &use : uses) {
			const bool held =
				std::any_of(bundledFonts.cbegin(), bundledFonts.cend(),
					    [&use](const BundledFont &font) { return font.family == use.family; });
			if (!held)
				wanted.append(use);
		}
	}

	QVector<BundledFont> collected = collectBundledFonts(wanted, skipped);

	QVector<BundledFont> merged;
	for (const FontUse &use : uses) {
		bool found = false;
		for (const BundledFont &font : collected) {
			if (font.family != use.family)
				continue;
			merged.append(font);
			found = true;
		}
		if (found)
			continue;

		/*
		 * A family whose file could not be found here keeps whatever the document already
		 * carries: this is the machine that does not have the font, which is the one the bundle
		 * exists for, and re-reading is no reason to throw it away.
		 */
		for (const BundledFont &font : bundledFonts) {
			if (font.family == use.family)
				merged.append(font);
		}
	}

	/*
	 * Compared by what a bundle *is* -- which families, from which files -- rather than by the
	 * bytes. The bytes are megabytes and are the one thing that cannot have changed without the
	 * file name changing with it, and this runs on every Apply.
	 */
	const auto sameBundle = [](const QVector<BundledFont> &a, const QVector<BundledFont> &b) {
		if (a.size() != b.size())
			return false;
		for (int i = 0; i < a.size(); ++i) {
			if (a[i].family != b[i].family || a[i].fileName != b[i].fileName ||
			    a[i].styleNames != b[i].styleNames || a[i].data.size() != b[i].data.size())
				return false;
		}
		return true;
	};

	if (sameBundle(merged, bundledFonts))
		return false;

	bundledFonts = merged;
	return true;
}

/* --- persistence -------------------------------------------------------------------------- */

void CalendarDocument::save(obs_data_t *data) const
{
	obs_data_set_int(data, "width", width);
	obs_data_set_int(data, "height", height);
	saveColor(data, "background", background);

	obs_data_set_string(data, "layout", calendarLayoutId(layout));
	obs_data_set_string(data, "axis_mode", timeAxisModeId(axisMode));
	obs_data_set_string(data, "orientation", gridOrientationId(orientation));
	obs_data_set_double(data, "margin_x", marginX);
	obs_data_set_double(data, "margin_y", marginY);

	saveVector(data, "days", days);
	saveVector(data, "lanes", lanes);
	saveVector(data, "slots", timeSlots);
	saveVector(data, "categories", categories);
	saveVector(data, "events", events);
	saveVector(data, "elements", elements);

	OBSDataAutoRelease gridData = obs_data_create();
	grid.save(gridData);
	obs_data_set_obj(data, "grid", gridData);

	OBSDataAutoRelease liveData = obs_data_create();
	live.save(liveData);
	obs_data_set_obj(data, "live", liveData);

	OBSDataAutoRelease overflowData = obs_data_create();
	overflow.save(overflowData);
	obs_data_set_obj(data, "overflow", overflowData);

	saveBlockStyle(data, "block_style", blockStyle);
	saveBlockStyle(data, "upcoming_style", upcomingStyle);
	saveBlockStyle(data, "live_style", liveStyle);
	saveBlockStyle(data, "finished_style", finishedStyle);
	saveBlockStyle(data, "cancelled_style", cancelledStyle);
	saveBlockStyle(data, "continuation_style", continuationStyle);

	obs_data_set_int(data, "up_next_count", upNextCount);
	obs_data_set_double(data, "up_next_row_height", upNextRowHeight);
	obs_data_set_double(data, "up_next_row_gap", upNextRowGap);
	obs_data_set_double(data, "up_next_time_width", upNextTimeWidth);
	obs_data_set_bool(data, "up_next_past", upNextIncludesPast);

	obs_data_set_string(data, "zone_mode", clockZoneModeId(zoneMode));
	saveString(data, "time_zone", timeZone);

	saveVector(data, "style_presets", stylePresets);
	saveVector(data, "background_presets", backgroundPresets);

	obs_data_set_bool(data, "bundle_fonts", bundleFonts);
	saveVector(data, "bundled_fonts", bundledFonts);
	saveVector(data, "font_substitutions", fontSubstitutions);
}

void CalendarDocument::load(obs_data_t *data, bool *migrated)
{
	width = std::max(1, loadInt(data, "width", 1920));
	height = std::max(1, loadInt(data, "height", 1080));
	background = loadColor(data, "background", QColor(0, 0, 0, 0));

	layout = calendarLayoutFromId(obs_data_get_string(data, "layout"), CalendarLayout::Grid);
	axisMode = timeAxisModeFromId(obs_data_get_string(data, "axis_mode"), TimeAxisMode::Clock);
	orientation = gridOrientationFromId(obs_data_get_string(data, "orientation"), GridOrientation::TimeDown);
	marginX = std::max(0.0, loadDouble(data, "margin_x", 40.0));
	marginY = std::max(0.0, loadDouble(data, "margin_y", 40.0));

	loadVector(data, "days", &days);
	loadVector(data, "lanes", &lanes);
	loadVector(data, "slots", &timeSlots);
	loadVector(data, "categories", &categories);
	loadVector(data, "events", &events);
	loadVector(data, "elements", &elements);

	OBSDataAutoRelease gridData = obs_data_get_obj(data, "grid");
	if (gridData)
		grid.load(gridData);

	OBSDataAutoRelease liveData = obs_data_get_obj(data, "live");
	if (liveData)
		live.load(liveData);

	OBSDataAutoRelease overflowData = obs_data_get_obj(data, "overflow");
	if (overflowData)
		overflow.load(overflowData);

	loadBlockStyle(data, "block_style", &blockStyle);
	loadBlockStyle(data, "upcoming_style", &upcomingStyle);
	loadBlockStyle(data, "live_style", &liveStyle);
	loadBlockStyle(data, "finished_style", &finishedStyle);
	loadBlockStyle(data, "cancelled_style", &cancelledStyle);
	loadBlockStyle(data, "continuation_style", &continuationStyle);

	upNextCount = std::clamp(loadInt(data, "up_next_count", 5), 1, 64);
	upNextRowHeight = std::max(1.0, loadDouble(data, "up_next_row_height", 92.0));
	upNextRowGap = std::max(0.0, loadDouble(data, "up_next_row_gap", 10.0));
	upNextTimeWidth = std::max(0.0, loadDouble(data, "up_next_time_width", 220.0));
	upNextIncludesPast = obs_data_get_bool(data, "up_next_past");

	zoneMode = clockZoneModeFromId(obs_data_get_string(data, "zone_mode"), ClockZoneMode::Automatic);
	timeZone = loadString(data, "time_zone");

	stylePresets.clear();
	{
		QVector<StylePreset> loaded;
		loadVector(data, "style_presets", &loaded);
		for (const StylePreset &preset : loaded) {
			/* An unnamed preset can never be resolved, so it is dropped on load. */
			if (!preset.name.isEmpty())
				stylePresets.append(preset);
		}
	}

	backgroundPresets.clear();
	{
		QVector<BackgroundPreset> loaded;
		loadVector(data, "background_presets", &loaded);
		for (const BackgroundPreset &preset : loaded) {
			if (!preset.name.isEmpty())
				backgroundPresets.append(preset);
		}
	}

	bundleFonts = loadBool(data, "bundle_fonts", true);

	bundledFonts.clear();
	{
		QVector<BundledFont> loaded;
		loadVector(data, "bundled_fonts", &loaded);
		for (const BundledFont &font : loaded) {
			if (!font.family.isEmpty() && !font.isEmpty())
				bundledFonts.append(font);
		}
	}

	fontSubstitutions.clear();
	{
		QVector<FontSubstitution> loaded;
		loadVector(data, "font_substitutions", &loaded);
		for (const FontSubstitution &substitution : loaded) {
			if (!substitution.from.isEmpty() && !substitution.to.isEmpty())
				fontSubstitutions.append(substitution);
		}
	}

	/*
	 * Ids are what everything here points at, so a document whose ids did not survive -- a
	 * hand-edited collection, a truncated file -- would draw nothing and say nothing about why.
	 * Filling in the missing ones costs nothing and makes the failure recoverable by hand.
	 */
	for (CalendarDay &day : days) {
		if (day.id.isEmpty())
			day.id = makeDayId();
	}
	for (CalendarLane &lane : lanes) {
		if (lane.id.isEmpty())
			lane.id = makeLaneId();
	}
	for (CalendarSlot &slot : timeSlots) {
		if (slot.id.isEmpty())
			slot.id = makeSlotId();
	}
	for (CalendarCategory &category : categories) {
		if (category.id.isEmpty())
			category.id = makeCategoryId();
	}
	for (CalendarEvent &event : events) {
		if (event.id.isEmpty())
			event.id = makeEventId();
	}
	for (CalendarElement &element : elements) {
		if (element.id.isEmpty())
			element.id = makeElementId();
	}

	const bool changed = refreshLinkedPresets();
	if (migrated)
		*migrated = changed;
}

void CalendarDocument::defaults(obs_data_t *data)
{
	obs_data_set_default_int(data, "width", 1920);
	obs_data_set_default_int(data, "height", 1080);
	obs_data_set_default_int(data, "background", 0);
	obs_data_set_default_string(data, "layout", calendarLayoutId(CalendarLayout::Grid));
	obs_data_set_default_string(data, "axis_mode", timeAxisModeId(TimeAxisMode::Clock));
	obs_data_set_default_string(data, "orientation", gridOrientationId(GridOrientation::TimeDown));
	obs_data_set_default_bool(data, "bundle_fonts", true);
}

QString CalendarDocument::toJson() const
{
	OBSDataAutoRelease data = obs_data_create();
	save(data);
	return QString::fromUtf8(obs_data_get_json_pretty(data));
}

bool CalendarDocument::fromJson(const QString &json, QString *error)
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
