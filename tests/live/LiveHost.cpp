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
 * The live harness: the built plugin, loaded into a real libobs on a real graphics device, playing
 * credit rolls and having its frames read back.
 *
 * The offscreen harness answers everything about layout and rasterizing, but nothing it runs ever
 * touches libobs' graphics, the source's playback or the shader -- which is where a credit roll
 * actually hitches, tears or goes to air the wrong color. This does: it starts libobs headless on
 * EGL over X11 (Xvfb and Mesa's software renderer are enough), opens the plugin module, creates
 * sources from documents built with the plugin's own model, renders them into a texture and checks
 * the pixels and the timing.
 *
 *     xvfb-run -a ./build/tests/live/closing-time-live-tests build/closing-time.so data
 *
 * Linux only, and not run by CI: it needs a libobs runtime and its effect files, which the plugin's
 * build does not.
 */
#include <obs.h>
#include <obs.hpp>
#include <obs-module.h>
#include <obs-nix-platform.h>
#include <graphics/graphics.h>

#include <QApplication>
#include <QImage>
#include <QThread>

extern "C" {
typedef struct _XDisplay Display;
Display *XOpenDisplay(const char *);
int XCloseDisplay(Display *);
}

#include "model/CreditsModel.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
static int checks = 0;
static void check(bool ok, const char *what)
{
	++checks;
	if (!ok) {
		++failures;
		std::printf("  FAIL  %s\n", what);
	} else {
		std::printf("  ok    %s\n", what);
	}
}

static std::vector<std::string> warnings;
static void logHandler(int level, const char *format, va_list args, void *)
{
	char buffer[4096];
	vsnprintf(buffer, sizeof(buffer), format, args);
	if (level <= LOG_WARNING)
		warnings.push_back(buffer);
	if (level <= LOG_INFO &&
	    (std::strstr(buffer, "closing-time") || std::strstr(buffer, "Closing Time") || level <= LOG_WARNING))
		std::printf("[obs %d] %s\n", level, buffer);
}

struct Frame {
	QImage image;
};

static QImage renderSource(obs_source_t *source, int width, int height)
{
	QImage out;
	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(width, height, GS_RGBA);
	if (gs_texrender_begin(texrender, width, height)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			out = QImage(width, height, QImage::Format_RGBA8888_Premultiplied);
			for (int y = 0; y < height; ++y)
				std::memcpy(out.scanLine(y), data + y * linesize, width * 4);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();
	return out;
}

struct Ink {
	int top = -1, bottom = -1, left = -1, right = -1;
	int pixels = 0;
};

static Ink inkOf(const QImage &image)
{
	Ink ink;
	for (int y = 0; y < image.height(); ++y) {
		const uchar *row = image.constScanLine(y);
		for (int x = 0; x < image.width(); ++x) {
			if (row[x * 4 + 3] == 0)
				continue;
			++ink.pixels;
			if (ink.top < 0)
				ink.top = y;
			ink.bottom = y;
			if (ink.left < 0 || x < ink.left)
				ink.left = x;
			if (x > ink.right)
				ink.right = x;
		}
	}
	return ink;
}

static void sleepMs(int ms)
{
	// Keep Qt's event loop turning: queued work from the plugin lands on the main thread.
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	while (std::chrono::steady_clock::now() < end) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
}

struct State {
	std::string phase;
	double time = 0, duration = 0;
	bool finished = false, paused = false;
};

static State stateOf(obs_source_t *source)
{
	calldata_t call = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &call);
	State s;
	const char *phase = calldata_string(&call, "phase");
	s.phase = phase ? phase : "";
	s.time = calldata_float(&call, "time");
	s.duration = calldata_float(&call, "duration");
	s.finished = calldata_bool(&call, "finished");
	s.paused = calldata_bool(&call, "paused");
	calldata_free(&call);
	return s;
}

static void callProc(obs_source_t *source, const char *name)
{
	calldata_t call = {};
	proc_handler_call(obs_source_get_proc_handler(source), name, &call);
	calldata_free(&call);
}

// A section in the document's settings format.
static obs_data_t *titleSection(const char *text, int pixelSize = 48)
{
	obs_data_t *section = obs_data_create();
	obs_data_set_string(section, "type", "title");
	obs_data_set_string(section, "text", text);
	obs_data_t *style = obs_data_create();
	obs_data_set_int(style, "pixel_size", pixelSize);
	obs_data_set_obj(section, "style", style);
	obs_data_release(style);
	return section;
}

static int finishedSignals = 0;

int main(int argc, char **argv)
{
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QApplication app(argc, argv);

	if (argc < 3) {
		std::fprintf(stderr, "usage: host <module.so> <module data dir>\n");
		return 2;
	}

	base_set_log_handler(logHandler, nullptr);

	Display *display = XOpenDisplay(nullptr);
	if (!display) {
		std::fprintf(stderr, "no X display\n");
		return 2;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);

	if (!obs_startup("en-US", "/tmp/closing-time-livehost-config", nullptr)) {
		std::fprintf(stderr, "obs_startup failed\n");
		return 2;
	}

	struct obs_video_info ovi = {};
	ovi.graphics_module = "libobs-opengl";
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = 1280;
	ovi.base_height = 720;
	ovi.output_width = 1280;
	ovi.output_height = 720;
	ovi.output_format = VIDEO_FORMAT_RGBA;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_FULL;
	ovi.adapter = 0;
	ovi.gpu_conversion = true;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	const int reset = obs_reset_video(&ovi);
	if (reset != OBS_VIDEO_SUCCESS) {
		std::fprintf(stderr, "obs_reset_video failed: %d\n", reset);
		return 2;
	}

	obs_module_t *module = nullptr;
	if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(module)) {
		std::fprintf(stderr, "could not load the module\n");
		return 2;
	}
	obs_post_load_modules();

	signal_handler_connect(
		obs_get_signal_handler(), "closing_time_finished", [](void *, calldata_t *) { ++finishedSignals; },
		nullptr);

	const int W = 1280, H = 720;

	// --- A roll: three titles, then a sticky block that ends the roll at its hold. ---
	using namespace closingtime;
	Document document;
	document.width = W;
	document.height = H;
	document.scrollSpeed = 600.0;
	document.startOnShow = false;
	document.background = QColor(0, 0, 0, 255);
	for (const char *text : {"Directed by Somebody", "Produced by Someone Else", "Music by A Composer"}) {
		Section title = Section::makeDefault(SectionType::Title);
		title.text = QString::fromUtf8(text);
		title.style.pixelSize = 48;
		document.sections.append(title);
	}
	{
		Section block = Section::makeDefault(SectionType::StickyBlock);
		block.children.clear();
		Section card = Section::makeDefault(SectionType::Title);
		card.text = QStringLiteral("THE END");
		card.style.pixelSize = 96;
		block.children.push_back(card);
		block.stickyHold = 1.0;
		block.stickyRelease = StickyRelease::EndAtHold;
		document.sections.append(block);
	}
	OBSDataAutoRelease settings = obs_data_create();
	document.save(settings);

	OBSSourceAutoRelease source = obs_source_create_private("closing_time_credits", "roll", settings);
	check(source != nullptr, "the credits source is created");
	if (!source)
		return 1;

	std::printf("\n-- before it starts\n");
	sleepMs(300);
	QImage idle = renderSource(source, W, H);
	check(!idle.isNull(), "a frame can be read back");
	// Opaque black background, nothing else: every pixel black and opaque.
	const QRgb corner = idle.pixel(10, 10);
	check(qAlpha(corner) == 255 && qRed(corner) < 3, "a parked roll draws only its opaque black background");

	std::printf("\n-- rolling\n");
	callProc(source, "start");
	sleepMs(700);
	State s1 = stateOf(source);
	QImage a = renderSource(source, W, H);
	sleepMs(200);
	State s2 = stateOf(source);
	QImage b = renderSource(source, W, H);
	check(s1.phase == "running" && s2.time > s1.time, "roll time runs once started");

	// Ink: anything brighter than the background.
	auto textInk = [](const QImage &image) {
		Ink ink;
		for (int y = 0; y < image.height(); ++y)
			for (int x = 0; x < image.width(); ++x) {
				const QRgb p = image.pixel(x, y);
				if (qRed(p) < 40)
					continue;
				++ink.pixels;
				if (ink.top < 0)
					ink.top = y;
				ink.bottom = y;
			}
		return ink;
	};
	const Ink inkA = textInk(a), inkB = textInk(b);
	std::printf("     ink A top %d (t=%.3f) ink B top %d (t=%.3f) pixels %d %d\n", inkA.top, s1.time, inkB.top,
		    s2.time, inkA.pixels, inkB.pixels);
	check(inkA.pixels > 500 && inkB.pixels > 500, "the titles are on the canvas while rolling");
	if (inkA.top >= 0 && inkB.top >= 0) {
		const double moved = inkA.top - inkB.top;
		const double expected = 600.0 * (s2.time - s1.time);
		std::printf("     moved %.1f px, expected about %.1f\n", moved, expected);
		check(std::abs(moved - expected) < 40.0, "the roll moves up at its scroll speed");
	}

	// White text on black, blended in linear light: the solid cores of the glyphs are still white.
	int brightest = 0;
	for (int y = 0; y < b.height(); ++y)
		for (int x = 0; x < b.width(); ++x)
			brightest = std::max(brightest, qRed(b.pixel(x, y)));
	check(brightest >= 250, "glyph cores reach full white through the sRGB path");

	std::printf("\n-- an edit while it rolls\n");
	{
		document.sections[0].text = QStringLiteral("Directed by Somebody New");
		OBSDataAutoRelease edited = obs_data_create();
		document.save(edited);
		obs_source_update(source, edited);
	}
	// A content edit re-arms the roll; every frame from here on must be drawable without a gap
	// once the new roll is up, and the warnings must stay quiet.
	int blankAfterStart = 0;
	bool sawInk = false;
	for (int i = 0; i < 40; ++i) {
		sleepMs(25);
		QImage f = renderSource(source, W, H);
		const Ink ink = textInk(f);
		if (ink.pixels > 0)
			sawInk = true;
		else if (sawInk)
			++blankAfterStart;
	}
	check(sawInk, "the edited roll comes back on screen");
	check(blankAfterStart == 0, "and once it has, no frame goes out blank");

	std::printf("\n-- the block pins and ends the roll\n");
	State before = stateOf(source);
	std::printf("     duration %.3f s\n", before.duration);
	check(before.duration > 0.0 && before.duration < 30.0, "the roll knows how long it runs");
	const int waitMs = static_cast<int>((before.duration - before.time + 0.6) * 1000.0);
	sleepMs(std::max(0, waitMs));
	State after = stateOf(source);
	QImage ended = renderSource(source, W, H);
	const Ink endInk = textInk(ended);
	std::printf("     phase %s finished %d time %.3f, ink rows %d..%d\n", after.phase.c_str(), after.finished,
		    after.time, endInk.top, endInk.bottom);
	check(after.finished && after.phase == "finished", "the roll finishes at its block's hold");
	check(finishedSignals == 1, "the global finished signal fires once");
	check(endInk.pixels > 1000, "the closing card is still up after the end");
	if (endInk.top >= 0) {
		const double middle = (endInk.top + endInk.bottom) / 2.0;
		check(std::abs(middle - H / 2.0) < 60.0, "and centered on the canvas, where it pinned");
	}

	std::printf("\n-- pause and manual scroll\n");
	callProc(source, "restart");
	sleepMs(300);
	callProc(source, "pause");
	State p1 = stateOf(source);
	sleepMs(300);
	State p2 = stateOf(source);
	check(p1.paused && std::abs(p2.time - p1.time) < 1e-9, "a paused roll's time stands still");
	{
		document.manualScroll = true;
		document.scrollPosition = 40.0;
		OBSDataAutoRelease edited = obs_data_create();
		document.save(edited);
		obs_source_update(source, edited);
	}
	sleepMs(400);
	QImage parkedA = renderSource(source, W, H);
	sleepMs(200);
	QImage parkedB = renderSource(source, W, H);
	check(textInk(parkedA).pixels > 500, "a roll parked at 40% shows its titles");
	check(parkedA == parkedB, "and holds perfectly still");

	std::printf("\n-- a long roll, fast\n");
	{
		Document longRoll;
		longRoll.width = W;
		longRoll.height = H;
		longRoll.scrollSpeed = 3000.0;
		longRoll.startOnShow = false;
		longRoll.background = QColor(0, 0, 0, 255);
		for (int i = 0; i < 300; ++i) {
			Section title = Section::makeDefault(SectionType::Title);
			title.text = QStringLiteral("Crew member number %1").arg(i);
			title.style.pixelSize = 40;
			longRoll.sections.append(title);
		}
		OBSDataAutoRelease longSettings = obs_data_create();
		longRoll.save(longSettings);
		OBSSourceAutoRelease fast = obs_source_create_private("closing_time_credits", "long", longSettings);
		/* Drawn while it waits, as a source in a scene on air is. */
		for (int i = 0; i < 20; ++i) {
			sleepMs(16);
			renderSource(fast, W, H);
		}
		callProc(fast, "start");
		int blank = 0, frames = 0;
		const auto started = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - started < std::chrono::seconds(6)) {
			sleepMs(16);
			const State state = stateOf(fast);
			if (state.time < 0.5 || state.finished)
				continue;
			++frames;
			if (textInk(renderSource(fast, W, H)).pixels == 0) {
				++blank;
				std::printf("     blank at roll time %.3f\n", state.time);
			}
		}
		const State done = stateOf(fast);
		std::printf("     %d frames checked mid-roll, %d blank; roll time %.2f of %.2f s\n", frames, blank,
			    done.time, done.duration);
		check(frames > 20 && blank == 0, "a 300-section roll at 3000 px/s never shows an empty frame");

		/* Parked and jumped about: how many frames before a far-off position has its pictures. */
		int worst = 0;
		for (double position : {90.0, 10.0, 70.0, 30.0, 55.0}) {
			longRoll.manualScroll = true;
			longRoll.scrollPosition = position;
			OBSDataAutoRelease jumped = obs_data_create();
			longRoll.save(jumped);
			obs_source_update(fast, jumped);
			int waited = 0;
			while (waited < 120) {
				sleepMs(16);
				if (textInk(renderSource(fast, W, H)).pixels > 2000)
					break;
				++waited;
			}
			worst = std::max(worst, waited);
		}
		std::printf("     a jump took at most %d frames to fill in\n", worst);
		check(worst <= 6, "a jump across a long parked roll fills in within a few frames");
	}

	std::printf("\n-- a card that fades up after the roll\n");
	{
		Document fade;
		fade.width = W;
		fade.height = H;
		fade.scrollSpeed = 2000.0;
		fade.startOnShow = false;
		fade.background = QColor(0, 0, 0, 255);
		Section title = Section::makeDefault(SectionType::Title);
		title.text = QStringLiteral("Cast");
		fade.sections.append(title);
		Section block = Section::makeDefault(SectionType::StickyBlock);
		block.children.clear();
		Section card = Section::makeDefault(SectionType::Title);
		card.text = QStringLiteral("SEE YOU NEXT TIME");
		card.style.pixelSize = 96;
		block.children.push_back(card);
		block.stickyEntrance = StickyEntrance::AfterRoll;
		block.stickyFadeIn = 2.0;
		block.stickyHoldForever = true;
		fade.sections.append(block);
		OBSDataAutoRelease fadeSettings = obs_data_create();
		fade.save(fadeSettings);
		OBSSourceAutoRelease fading = obs_source_create_private("closing_time_credits", "fade", fadeSettings);
		for (int i = 0; i < 20; ++i) {
			sleepMs(16);
			renderSource(fading, W, H);
		}
		callProc(fading, "start");

		/* The strip clears the frame after (720 + strip) / 2000 s; sample the card through its fade. */
		/* The title goes by, the frame goes dark, and then the card comes up: its first light is after the dark. */
		int brightestEarly = -1, brightestLate = -1;
		bool sawTitle = false, sawDark = false;
		const auto started = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
			sleepMs(30);
			const State state = stateOf(fading);
			QImage f = renderSource(fading, W, H);
			int brightest = 0;
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					brightest = std::max(brightest, qRed(f.pixel(x, y)));
			if (brightest > 200)
				sawTitle = true;
			else if (sawTitle && brightest < 5)
				sawDark = true;
			else if (sawDark && brightestEarly < 0 && brightest > 5)
				brightestEarly = brightest;
			if (state.time > 4.0)
				brightestLate = brightest;
		}
		std::printf("     first light on the card %d, at the end %d\n", brightestEarly, brightestLate);
		check(brightestEarly > 0 && brightestEarly < 200, "the card comes up dim, part-way through its fade");
		check(brightestLate >= 250, "and reaches full strength once it is up");
		check(std::isinf(stateOf(fading).duration), "a card that holds forever leaves the roll with no end");
	}

	std::printf("\n-- warnings\n");
	int ours = 0;
	for (const std::string &w : warnings) {
		if (w.find("closing-time") != std::string::npos) {
			++ours;
			std::printf("     %s\n", w.c_str());
		}
	}
	check(ours == 0, "the plugin logged no warnings or errors");

	source = nullptr;
	sleepMs(200);
	std::printf("\n%d checks, %d failures\n", checks, failures);

	// Shutting libobs down fully needs the module unloaded first.
	obs_shutdown();
	XCloseDisplay(display);
	return failures == 0 ? 0 : 1;
}
