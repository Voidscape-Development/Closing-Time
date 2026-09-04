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

#include "source/CalendarSource.hpp"

#include <graphics/vec4.h>
#include <obs.hpp>
#include <plugin-support.h>

#include <QDateTime>
#include <QSet>
#include <QString>

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

#include "model/CalendarModel.hpp"
#include "render/CalendarRenderer.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "ui/CalendarDesignerDialog.hpp"

namespace closingtime {

namespace {

/*
 * Threading, and it is the credit source's arrangement with one thing taken out:
 *
 *   - `document` is written only by update(), which libobs defers to the graphics thread for video
 *     sources, and by create() before the source is visible to anyone else.
 *   - The rendered board crosses from the render thread to the graphics thread through
 *     `pendingBoard` under `handoffMutex`; the textures are only ever touched by the graphics
 *     thread.
 *   - Presentation state -- which page, how far scrolled -- is touched from hotkeys as well as from
 *     video_tick, so it lives behind `stateMutex`.
 *
 * What is missing compared to the roll is a playback phase. A board has no beginning and no end: it
 * is showing whatever it is showing, and the only things that move are the page and the scroll.
 */
struct CalendarSourceData {
	obs_source_t *source = nullptr;

	CalendarDocument document;
	/* What the last rebuild was rasterized from; see renderKey(). Graphics thread only. */
	QString renderedFrom;

	/* Owned by the render thread: only the rebuild job touches it, and jobs run one at a time. */
	LogoCache logos;
	QSet<QString> warnedFonts;

	std::mutex handoffMutex;
	CalendarBoard pendingBoard;
	bool hasPendingBoard = false;
	bool rebuildInFlight = false;
	bool rebuildAgain = false;

	bool settingsNeedWriteBack = false;

	/* Graphics-thread state: one texture per tile of every page. */
	struct PageTextures {
		std::vector<gs_texture_t *> tiles;
		std::vector<int> tops;
		int height = 0;
	};

	std::vector<PageTextures> pages;
	/* The free layer, drawn over whichever page is showing and never scrolled with it. */
	gs_texture_t *overlay = nullptr;

	std::mutex stateMutex;
	int page = 0;
	double pageElapsed = 0.0;
	double scrollOffset = 0.0;
	/* +1 while the board is travelling up the canvas, -1 on the way back. */
	int scrollDirection = 1;
	double scrollPauseRemaining = 0.0;
	/* Seconds since the board was last rasterized, for the clock refresh. */
	double sinceRebuild = 0.0;

	obs_hotkey_id nextPageHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id prevPageHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id refreshHotkey = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id designerHotkey = OBS_INVALID_HOTKEY_ID;
};

/* ------------------------------------------------------------------ board rebuilding */

struct RebuildTask {
	OBSWeakSourceAutoRelease weak;
	CalendarSourceData *data = nullptr;
	CalendarDocument document;
	QDateTime now;
	QString sourceName;
};

/*
 * Everything the board is rasterized from, as a string two documents can be compared by.
 *
 * Unlike the roll there is almost nothing to exclude: a board has no scroll speed or ending action
 * that leaves the picture alone. The presentation settings that really are only presentation --
 * the page dwell, the scroll speed and pause -- are blanked so that nudging them does not
 * re-rasterize a board that would come out identical.
 */
QString renderKey(const CalendarDocument &document)
{
	CalendarDocument content = document;

	content.overflow.pageDwell = 0.0;
	content.overflow.scrollSpeed = 0.0;
	content.overflow.scrollPause = 0.0;

	/* Font files are reduced to their sizes, for the reason the roll's key does it: they are megabytes. */
	for (BundledFont &font : content.bundledFonts)
		font.data = QByteArray::number(font.data.size());

	return content.toJson();
}

void runRebuild(const std::shared_ptr<RebuildTask> &task);

void queueRebuild(CalendarSourceData *data)
{
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (data->rebuildInFlight) {
			data->rebuildAgain = true;
			return;
		}
		data->rebuildInFlight = true;
	}

	auto task = std::make_shared<RebuildTask>();
	task->weak = obs_source_get_weak_source(data->source);
	task->data = data;
	task->document = data->document;
	task->now = QDateTime::currentDateTime();
	task->sourceName = QString::fromUtf8(obs_source_get_name(data->source));

	postRenderJob([task] { runRebuild(task); });
}

void runRebuild(const std::shared_ptr<RebuildTask> &task)
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(task->weak);
	if (!source)
		return;

	CalendarSourceData *data = task->data;

	for (const QString &family : installBundledFonts(task->document.bundledFonts)) {
		obs_log(LOG_INFO, "font '%s' is not installed; '%s' is rendering it from its own bundle",
			family.toUtf8().constData(), task->sourceName.toUtf8().constData());
	}

	/*
	 * A missing font is not fatal -- Qt substitutes one -- but it silently changes what goes to
	 * air, so each family is called out once per source in the OBS log.
	 */
	for (const QString &family : task->document.usedFontFamilies()) {
		if (fontFamilyAvailable(family) || data->warnedFonts.contains(family))
			continue;

		data->warnedFonts.insert(family);

		const QString substitute = task->document.fontSubstitute(family);
		if (substitute.isEmpty())
			obs_log(LOG_WARNING, "font '%s' is not installed; '%s' will render with a substitute",
				family.toUtf8().constData(), task->sourceName.toUtf8().constData());
		else
			obs_log(LOG_INFO, "font '%s' is not installed; '%s' will render it as '%s'",
				family.toUtf8().constData(), task->sourceName.toUtf8().constData(),
				substitute.toUtf8().constData());
	}

	CalendarRenderer renderer(&data->logos);
	CalendarBoard board = renderer.render(task->document, task->now);

	bool again = false;
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		data->pendingBoard = std::move(board);
		data->hasPendingBoard = true;
		data->rebuildInFlight = false;
		again = data->rebuildAgain;
		data->rebuildAgain = false;
	}

	if (again)
		queueRebuild(data);
}

/* ------------------------------------------------------------------------ textures */

/* Graphics thread only; requires an active graphics context. */
gs_texture_t *createTexture(const QImage &image)
{
	if (image.isNull())
		return nullptr;

	const uint8_t *bits = image.constBits();
	return gs_texture_create(static_cast<uint32_t>(image.width()), static_cast<uint32_t>(image.height()), GS_BGRA,
				 1, &bits, GS_DYNAMIC);
}

void releaseTextures(CalendarSourceData *data)
{
	for (CalendarSourceData::PageTextures &page : data->pages) {
		for (gs_texture_t *texture : page.tiles)
			gs_texture_destroy(texture);
	}
	data->pages.clear();

	if (data->overlay) {
		gs_texture_destroy(data->overlay);
		data->overlay = nullptr;
	}
}

/* Graphics thread only. */
void uploadPendingBoard(CalendarSourceData *data)
{
	CalendarBoard board;
	{
		std::lock_guard<std::mutex> lock(data->handoffMutex);
		if (!data->hasPendingBoard)
			return;

		board = std::move(data->pendingBoard);
		data->pendingBoard = CalendarBoard();
		data->hasPendingBoard = false;
	}

	releaseTextures(data);

	for (const CalendarPage &page : board.pages) {
		CalendarSourceData::PageTextures textures;
		textures.height = page.height;

		for (const StripTile &tile : page.tiles) {
			if (gs_texture_t *texture = createTexture(tile.image)) {
				textures.tiles.push_back(texture);
				textures.tops.push_back(tile.top);
			}
		}

		data->pages.push_back(std::move(textures));
	}

	data->overlay = createTexture(board.overlay);

	std::lock_guard<std::mutex> lock(data->stateMutex);
	/*
	 * A rebuild can leave the board with fewer pages than it had -- an event deleted, a day hidden
	 * -- and a page number left pointing past the end would draw nothing at all.
	 */
	if (data->page >= static_cast<int>(data->pages.size()))
		data->page = 0;
	data->sinceRebuild = 0.0;
}

/* ---------------------------------------------------------------------- presentation */

/* How far a scrolling board can travel before its foot reaches the bottom of the canvas. */
double scrollTravel(const CalendarSourceData *data)
{
	if (data->pages.empty())
		return 0.0;

	return std::max(0.0, static_cast<double>(data->pages.front().height) -
				     static_cast<double>(std::max(1, data->document.height)));
}

void advancePresentation(CalendarSourceData *data, double seconds)
{
	std::lock_guard<std::mutex> lock(data->stateMutex);

	switch (data->document.overflow.mode) {
	case OverflowMode::Page: {
		const int pages = static_cast<int>(data->pages.size());
		if (pages <= 1)
			break;

		data->pageElapsed += seconds;
		if (data->pageElapsed < data->document.overflow.pageDwell)
			break;

		data->pageElapsed = 0.0;
		data->page = (data->page + 1) % pages;
		break;
	}

	case OverflowMode::Scroll: {
		const double travel = scrollTravel(data);
		if (travel <= 0.0) {
			data->scrollOffset = 0.0;
			break;
		}

		if (data->scrollPauseRemaining > 0.0) {
			data->scrollPauseRemaining -= seconds;
			break;
		}

		data->scrollOffset += data->document.overflow.scrollSpeed * seconds * data->scrollDirection;

		/*
		 * A board scrolls back and forth rather than wrapping. A schedule is a single picture with
		 * a top and a bottom, and wrapping it would put the end of Sunday immediately above the
		 * start of Friday -- which reads as a mistake rather than as a loop.
		 */
		if (data->scrollOffset >= travel) {
			data->scrollOffset = travel;
			data->scrollDirection = -1;
			data->scrollPauseRemaining = data->document.overflow.scrollPause;
		} else if (data->scrollOffset <= 0.0) {
			data->scrollOffset = 0.0;
			data->scrollDirection = 1;
			data->scrollPauseRemaining = data->document.overflow.scrollPause;
		}
		break;
	}

	case OverflowMode::Fit:
	default:
		break;
	}
}

void stepPage(CalendarSourceData *data, int by)
{
	std::lock_guard<std::mutex> lock(data->stateMutex);

	const int pages = static_cast<int>(data->pages.size());
	if (pages <= 1)
		return;

	data->page = ((data->page + by) % pages + pages) % pages;
	data->pageElapsed = 0.0;
}

/* ------------------------------------------------------------------------- callbacks */

const char *getName(void *)
{
	return obs_module_text("CalendarDisplay");
}

void getDefaults(obs_data_t *settings)
{
	CalendarDocument::defaults(settings);
}

void writeDocumentBack(CalendarSourceData *data)
{
	OBSDataAutoRelease settings = obs_source_get_settings(data->source);
	if (!settings)
		return;

	data->document.save(settings);
	/* Kept in step first: the update() this schedules must not read as a content change. */
	data->renderedFrom = renderKey(data->document);
	obs_source_update(data->source, settings);
}

void update(void *raw, obs_data_t *settings)
{
	auto *data = static_cast<CalendarSourceData *>(raw);

	bool migrated = false;
	data->document.load(settings, &migrated);
	if (migrated)
		data->settingsNeedWriteBack = true;

	const QString key = renderKey(data->document);
	if (key != data->renderedFrom) {
		data->renderedFrom = key;
		queueRebuild(data);
	}

	std::lock_guard<std::mutex> lock(data->stateMutex);
	data->pageElapsed = 0.0;
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *data = new CalendarSourceData();
	data->source = source;

	proc_handler_t *procs = obs_source_get_proc_handler(source);
	proc_handler_add(
		procs, "void refresh()",
		[](void *param, calldata_t *) { queueRebuild(static_cast<CalendarSourceData *>(param)); }, data);
	proc_handler_add(
		procs, "void next_page()",
		[](void *param, calldata_t *) { stepPage(static_cast<CalendarSourceData *>(param), 1); }, data);
	proc_handler_add(
		procs, "void previous_page()",
		[](void *param, calldata_t *) { stepPage(static_cast<CalendarSourceData *>(param), -1); }, data);

	data->nextPageHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Calendar.NextPage", obs_module_text("Hotkey.Calendar.NextPage"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				stepPage(static_cast<CalendarSourceData *>(param), 1);
		},
		data);

	data->prevPageHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Calendar.PreviousPage", obs_module_text("Hotkey.Calendar.PreviousPage"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				stepPage(static_cast<CalendarSourceData *>(param), -1);
		},
		data);

	/*
	 * A board is redrawn on a timer only when something on it depends on the clock. This is the
	 * other way to get a fresh one: the schedule has been changed somewhere else, or somebody wants
	 * the board caught up now rather than at the next refresh.
	 */
	data->refreshHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Calendar.Refresh", obs_module_text("Hotkey.Calendar.Refresh"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				queueRebuild(static_cast<CalendarSourceData *>(param));
		},
		data);

	data->designerHotkey = obs_hotkey_register_source(
		source, "ClosingTime.Calendar.Designer", obs_module_text("Hotkey.Calendar.Designer"),
		[](void *param, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				openCalendarDesignerForAsync(static_cast<CalendarSourceData *>(param)->source);
		},
		data);

	update(data, settings);
	return data;
}

void destroy(void *raw)
{
	auto *data = static_cast<CalendarSourceData *>(raw);

	obs_hotkey_unregister(data->nextPageHotkey);
	obs_hotkey_unregister(data->prevPageHotkey);
	obs_hotkey_unregister(data->refreshHotkey);
	obs_hotkey_unregister(data->designerHotkey);

	closeCalendarDesignerFor(data->source);

	obs_enter_graphics();
	releaseTextures(data);
	obs_leave_graphics();

	delete data;
}

uint32_t getWidth(void *raw)
{
	return static_cast<uint32_t>(std::max(1, static_cast<CalendarSourceData *>(raw)->document.width));
}

uint32_t getHeight(void *raw)
{
	return static_cast<uint32_t>(std::max(1, static_cast<CalendarSourceData *>(raw)->document.height));
}

void videoTick(void *raw, float seconds)
{
	auto *data = static_cast<CalendarSourceData *>(raw);

	if (data->settingsNeedWriteBack) {
		data->settingsNeedWriteBack = false;
		writeDocumentBack(data);
	}

	advancePresentation(data, seconds);

	/*
	 * The clock refresh. A board with none of the live features on never gets here, which is what
	 * makes a still board free: it is rasterized when it is edited and never again.
	 */
	if (!data->document.needsClock())
		return;

	std::unique_lock<std::mutex> lock(data->stateMutex);
	data->sinceRebuild += seconds;
	if (data->sinceRebuild < data->document.live.refreshSeconds)
		return;

	data->sinceRebuild = 0.0;
	lock.unlock();

	queueRebuild(data);
}

void drawBackground(const QColor &color, int width, int height)
{
	if (color.alpha() <= 0)
		return;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");

	struct vec4 value;
	vec4_set(&value, static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
		 static_cast<float>(color.blueF()), static_cast<float>(color.alphaF()));
	gs_effect_set_vec4(colorParam, &value);

	while (gs_effect_loop(solid, "Solid"))
		gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

/*
 * Draws one tile, clipped to the canvas.
 *
 * The same quad the roll builds, and for the same reason: a scrolling board advances by a fraction
 * of a pixel per frame, and gs_draw_sprite_subregion takes whole texels only -- so the fraction goes
 * into the geometry and the texture coordinates instead of being dropped.
 */
void drawClipped(double top, double width, double height, int canvasHeight)
{
	if (width <= 0.0 || height <= 0.0)
		return;

	const double visibleTop = std::max(top, 0.0);
	const double visibleBottom = std::min(top + height, static_cast<double>(canvasHeight));
	if (visibleBottom <= visibleTop)
		return;

	const auto x0 = 0.0f;
	const auto x1 = static_cast<float>(width);
	const auto y0 = static_cast<float>(visibleTop);
	const auto y1 = static_cast<float>(visibleBottom);
	const auto v0 = static_cast<float>((visibleTop - top) / height);
	const auto v1 = static_cast<float>((visibleBottom - top) / height);

	/* Vertices in the order libobs builds its own sprites for a triangle strip: tl, tr, bl, br. */
	gs_render_start(false);
	gs_texcoord(0.0f, v0, 0);
	gs_vertex2f(x0, y0);
	gs_texcoord(1.0f, v0, 0);
	gs_vertex2f(x1, y0);
	gs_texcoord(0.0f, v1, 0);
	gs_vertex2f(x0, y1);
	gs_texcoord(1.0f, v1, 0);
	gs_vertex2f(x1, y1);
	gs_render_stop(GS_TRISTRIP);
}

void videoRender(void *raw, gs_effect_t *)
{
	auto *data = static_cast<CalendarSourceData *>(raw);

	uploadPendingBoard(data);

	const int canvasWidth = std::max(1, data->document.width);
	const int canvasHeight = std::max(1, data->document.height);

	drawBackground(data->document.background, canvasWidth, canvasHeight);

	if (data->pages.empty())
		return;

	int page = 0;
	double offset = 0.0;
	{
		std::lock_guard<std::mutex> lock(data->stateMutex);
		page = std::clamp(data->page, 0, static_cast<int>(data->pages.size()) - 1);
		offset = data->scrollOffset;
	}

	const CalendarSourceData::PageTextures &textures = data->pages[page];

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *imageParam = gs_effect_get_param_by_name(effect, "image");

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(effect, "Draw")) {
		for (size_t i = 0; i < textures.tiles.size(); ++i) {
			gs_texture_t *texture = textures.tiles[i];
			gs_effect_set_texture(imageParam, texture);
			drawClipped(textures.tops[i] - offset, static_cast<double>(gs_texture_get_width(texture)),
				    static_cast<double>(gs_texture_get_height(texture)), canvasHeight);
		}

		/*
		 * The free layer last and unscrolled: a clock in the corner belongs to the canvas rather
		 * than to the board, so it holds still while the board moves under it.
		 */
		if (data->overlay) {
			gs_effect_set_texture(imageParam, data->overlay);
			drawClipped(0.0, static_cast<double>(gs_texture_get_width(data->overlay)),
				    static_cast<double>(gs_texture_get_height(data->overlay)), canvasHeight);
		}
	}

	gs_blend_state_pop();
}

/* ------------------------------------------------------------------------ properties */

bool onOpenDesigner(obs_properties_t *, obs_property_t *, void *raw)
{
	openCalendarDesignerFor(static_cast<CalendarSourceData *>(raw)->source);
	return false;
}

obs_properties_t *getProperties(void *raw)
{
	auto *data = static_cast<CalendarSourceData *>(raw);
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_button2(props, "open_designer", obs_module_text("OpenCalendarDesigner"), onOpenDesigner,
				   data);

	obs_properties_add_int(props, "width", obs_module_text("Width"), 16, 8192, 1);
	obs_properties_add_int(props, "height", obs_module_text("Height"), 16, 8192, 1);
	obs_properties_add_color_alpha(props, "background", obs_module_text("BackgroundColor"));

	/*
	 * Everything else about a board -- its layout, its schedule, how it deals with not fitting, what
	 * it does about the clock -- is edited in the designer, where there is a preview to judge it
	 * against. A properties window with a page dwell in it and nothing to show for it would be
	 * asking for a number nobody can answer without watching the board.
	 */
	obs_property_t *note =
		obs_properties_add_text(props, "designer_note", obs_module_text("Calendar.Note"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(note, OBS_TEXT_INFO_NORMAL);

	return props;
}

struct obs_source_info calendarSourceInfo = {};

} // namespace

void registerCalendarSource()
{
	calendarSourceInfo.id = kCalendarSourceId;
	calendarSourceInfo.type = OBS_SOURCE_TYPE_INPUT;
	calendarSourceInfo.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	calendarSourceInfo.icon_type = OBS_ICON_TYPE_TEXT;
	calendarSourceInfo.get_name = getName;
	calendarSourceInfo.create = create;
	calendarSourceInfo.destroy = destroy;
	calendarSourceInfo.get_width = getWidth;
	calendarSourceInfo.get_height = getHeight;
	calendarSourceInfo.get_defaults = getDefaults;
	calendarSourceInfo.get_properties = getProperties;
	calendarSourceInfo.update = update;
	calendarSourceInfo.video_tick = videoTick;
	calendarSourceInfo.video_render = videoRender;

	obs_register_source(&calendarSourceInfo);
}

} // namespace closingtime
