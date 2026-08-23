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

#include <obs.hpp>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"
#include "render/AnimatedLogo.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

Section animatedHeading()
{
	Section section = unpadded(SectionType::LogoTitle);
	withAnimatedLogo(section);
	return section;
}

/* The strip's own pixels inside `box`, with nothing drawn over them. */
bool boxIsEmpty(const Strip &strip, const QRectF &box)
{
	const Ink ink = inkOf(flatten(strip), box);
	return ink.isEmpty();
}

} // namespace

CT_SUITE(animated_logo_decode, "Decoding an animation, and telling one from a still")
{
	const QString animated = testAnimatedLogoPath();
	check(!animated.isEmpty(), "the animated fixture was written");

	check(logoPathLooksAnimated(animated), "an animated GIF is recognised without decoding it");
	check(!logoPathLooksAnimated(testLogoPath()), "a still PNG is not mistaken for an animation");
	check(!logoPathLooksAnimated(missingLogoPath()), "a missing file is not an animation");

	AnimatedLogoCache cache;

	const LogoAnimationPtr decoded = cache.get(animated, 64);
	check(decoded != nullptr, "the animation decodes");
	if (!decoded)
		return;

	checkEq(decoded->frames.size(), kTestAnimationFrames, "every frame is decoded");
	check(decoded->isValid(), "a two-frame animation counts as one");
	checkEq(decoded->totalMs, kTestAnimationFrames * kTestAnimationFrameMs, "the pass is as long as its frames");
	check(!decoded->truncated, "a two-frame animation is nowhere near the length cap");

	for (const LogoFrame &frame : decoded->frames) {
		checkEq(frame.durationMs, kTestAnimationFrameMs, "each frame carries the delay the file set");
		check(frame.image.size() == decoded->size, "every frame is the size the animation reports");
		check(frame.image.height() <= 64, "frames are decoded no taller than the logo is drawn");
	}

	check(decoded->frames.at(0).image != decoded->frames.at(1).image, "the two frames differ");

	/* A still asked of the animation cache is not an animation, and the answer is cached as such. */
	check(cache.get(testLogoPath(), 64) == nullptr, "a still returns no animation");

	/* Same file, same size: the same decode rather than a second one. */
	check(cache.get(animated, 64) == decoded, "a second ask returns the cached decode");
}

CT_SUITE(video_logos_are_not_played, "A video file is turned away rather than decoded")
{
	/*
	 * Logos are pictures: GIF, APNG and animated WebP animate through Qt, and video is not
	 * decoded at all. What is checked here is that every part of the plugin agrees about that
	 * -- the file dialog, the designer's playback settings, and the cache -- because the way
	 * this goes wrong is one of them offering a file the others will not draw.
	 */
	const QString patterns = imageLogoPatterns();
	for (const QString &extension : {QStringLiteral("mp4"), QStringLiteral("webm"), QStringLiteral("mov")})
		check(!patterns.contains(extension), "the logo file dialog does not offer " + extension);

	check(isVideoLogoPath(QStringLiteral("sting.WebM")), "a video is known by its extension, in any case");
	check(isVideoLogoPath(QStringLiteral("/a/path/with.dots/clip.mp4")), "and by the last one of them");
	check(!isVideoLogoPath(testLogoPath()), "a still picture is not a video");
	check(!isVideoLogoPath(testAnimatedLogoPath()), "neither is an animated GIF");
	check(!isVideoLogoPath(QString()), "and neither is nothing at all");

	const QString video = videoLogoPath();
	check(!video.isEmpty(), "the video-named fixture was written");
	check(!logoPathLooksAnimated(video), "a video is offered no playback settings");

	/* No animation, no crash, and the same answer the second time from the cache. */
	AnimatedLogoCache cache;
	check(cache.get(video, 64) == nullptr, "a video decodes to no animation");
	check(cache.get(video, 64) == nullptr, "and does so again from the cache");
}

CT_SUITE(animated_logo_timing, "Which frame is showing, at a given point in an animation")
{
	AnimatedLogoCache cache;
	const LogoAnimationPtr animation = cache.get(testAnimatedLogoPath(), 64);
	check(animation != nullptr, "the animation decodes");
	if (!animation)
		return;

	checkEq(logoFrameAt(*animation, 0.0, true), 0, "an animation starts on its first frame");
	checkEq(logoFrameAt(*animation, kTestAnimationFrameMs - 1.0, true), 0, "the first frame is held for its delay");
	checkEq(logoFrameAt(*animation, kTestAnimationFrameMs, true), 1, "the second frame follows it");

	const double pass = kTestAnimationFrames * kTestAnimationFrameMs;
	checkEq(logoFrameAt(*animation, pass, true), 0, "a looping animation wraps to the start");
	checkEq(logoFrameAt(*animation, pass + kTestAnimationFrameMs, true), 1, "and carries on from there");

	checkEq(logoFrameAt(*animation, pass, false), kTestAnimationFrames - 1,
		"a play-once animation holds its last frame");
	checkEq(logoFrameAt(*animation, pass * 100, false), kTestAnimationFrames - 1, "and keeps holding it");

	/* A negative clock is not a thing playback produces, but a frame index out of range would be. */
	checkEq(logoFrameAt(*animation, -500.0, true), 0, "time before the start shows the first frame");
}

CT_SUITE(animated_logo_layout, "An animated logo lays out exactly as the still it replaces")
{
	Section still = unpadded(SectionType::LogoTitle);
	withLogo(still);

	const Document stillDocument = documentWith(still);
	const Document animatedDocument = documentWith(animatedHeading());

	/*
	 * The two fixtures are different pictures at different aspect ratios, so the boxes are not
	 * expected to match each other. What has to match is one document rendered both ways: the
	 * layout may not depend on whether the renderer can animate.
	 */
	const Strip plain = renderStrip(animatedDocument);
	const Strip animated = renderAnimatedStrip(animatedDocument);

	checkEq(animated.height, plain.height, "animation support does not change the strip's height");
	/*
	 * measure() never decodes an animation -- it has no cache to decode into -- so this is the
	 * check that an animated logo is measured as its first frame rather than as the placeholder
	 * box a file the still cache could not read would get.
	 */
	checkEq(measure(animatedDocument), animated.height, "a measured height matches the rendered one");
	check(measure(stillDocument) > 0, "the still fixture measures to something, for comparison");

	const QRectF box = boxOf(animatedDocument, LayoutBox::Kind::Logo);
	check(!box.isNull(), "the animated logo is placed");
	if (box.isNull())
		return;

	checkEq(animated.animatedLogos.size(), 1, "one placement is reported for one animated logo");
	if (animated.animatedLogos.isEmpty())
		return;

	const AnimatedLogoPlacement &placement = animated.animatedLogos.first();
	checkNear(placement.rect.left(), box.left(), 0.5, "the placement is the box the layout gave the logo");
	checkNear(placement.rect.top(), box.top(), 0.5, "the placement starts where the box does");
	checkNear(placement.rect.width(), box.width(), 0.5, "the placement is as wide as the box");
	checkNear(placement.rect.height(), box.height(), 0.5, "the placement is as tall as the box");
	checkEq(placement.section, 0, "the placement names the section it came from");

	/*
	 * The hole. Without an animation cache the first frame is baked into the strip, and with one
	 * the same pixels are left for the overlay quad to fill -- that difference is the entire
	 * mechanism, so it is checked on the pixels rather than inferred from the placement.
	 */
	check(!boxIsEmpty(plain, box), "without animation support the artwork is baked into the strip");
	check(boxIsEmpty(animated, box), "with it, the strip leaves the logo's box empty");
}

CT_SUITE(animated_logo_slots, "Every kind of logo slot reports its animation")
{
	/* One placement per logo, whether the logo is a heading's, a list entry's or a divider's. */
	{
		const Context context(QStringLiteral("logo list"));
		Section list = unpadded(SectionType::LogoList);
		list.entries = {Entry(), Entry(), Entry()};
		withAnimatedLogo(list);

		const Strip strip = renderAnimatedStrip(documentWith(list));
		checkEq(strip.animatedLogos.size(), 3, "a list reports one placement per entry");
	}

	{
		const Context context(QStringLiteral("multi-list"));
		Section grid = unpadded(SectionType::MultiLogoList);
		grid.columns = 2;
		grid.entries = {Entry(), Entry(), Entry(), Entry()};
		withAnimatedLogo(grid);

		const Strip strip = renderAnimatedStrip(documentWith(grid));
		checkEq(strip.animatedLogos.size(), 4, "a grid reports one placement per cell");
	}

	{
		const Context context(QStringLiteral("divider centre"));
		Section divider = unpadded(SectionType::SectionDivider);
		DividerPiece piece;
		piece.kind = DividerPiece::Kind::Logo;
		piece.logo.path = testAnimatedLogoPath();
		piece.logo.maxHeight = 48;
		divider.dividerCentre = {piece};

		const Strip strip = renderAnimatedStrip(documentWith(divider));
		checkEq(strip.animatedLogos.size(), 1, "a divider's own logo animates like any other");
	}

	{
		const Context context(QStringLiteral("still artwork"));
		Section still = unpadded(SectionType::LogoTitle);
		withLogo(still);

		const Strip strip = renderAnimatedStrip(documentWith(still));
		checkEq(strip.animatedLogos.size(), 0, "a still reports no placement and stays in the strip");
	}

	{
		const Context context(QStringLiteral("hidden section"));
		Section hidden = animatedHeading();
		hidden.visible = false;

		const Strip strip = renderAnimatedStrip(documentWith(hidden));
		checkEq(strip.animatedLogos.size(), 0, "a hidden section places nothing to animate");
	}
}

CT_SUITE(animated_logo_shadow, "How an animated logo's drop shadow is drawn")
{
	Section section = animatedHeading();
	section.style.shadow.enabled = true;
	section.style.shadow.blur = 0.0;
	section.style.shadow.offsetX = 6.0;
	section.style.shadow.offsetY = 6.0;

	{
		const Context context(QStringLiteral("baked"));
		const Document document = documentWith(section);
		const Strip strip = renderAnimatedStrip(document);
		checkEq(strip.animatedLogos.size(), 1, "the logo is placed");
		if (strip.animatedLogos.isEmpty())
			return;

		check(strip.animatedLogos.first().shadowFrames.isEmpty(),
		      "by default no per-frame shadow is carried: the strip baked the first frame's");

		/*
		 * The artwork is gone from the strip but its shadow is not, so there is still ink in
		 * the section -- offset from where the logo itself would have been.
		 */
		const QRectF logo = boxOf(document, LayoutBox::Kind::Logo);
		const Ink ink = inkOf(flatten(strip), logo.adjusted(0, 0, 12, 12));
		check(!ink.isEmpty(), "the baked shadow is still drawn behind the hole");
	}

	{
		const Context context(QStringLiteral("per frame"));
		Section following = section;
		following.logo.playback.animatedShadow = true;

		const Document document = documentWith(following);
		const Strip strip = renderAnimatedStrip(document);
		if (strip.animatedLogos.isEmpty()) {
			fail(QStringLiteral("the logo is placed"));
			return;
		}

		const AnimatedLogoPlacement &placement = strip.animatedLogos.first();
		checkEq(placement.shadowFrames.size(), kTestAnimationFrames,
			"a shadow that follows the animation carries one image per frame");

		const QRectF logo = boxOf(document, LayoutBox::Kind::Logo);
		const Ink ink = inkOf(flatten(strip), logo.adjusted(0, 0, 12, 12));
		check(ink.isEmpty(), "and nothing at all is baked into the strip for it");
	}
}

CT_SUITE(animated_logo_persistence, "Playback settings survive a save and a load")
{
	Section section = animatedHeading();
	section.logo.playback.loop = false;
	section.logo.playback.startOnEnter = true;
	section.logo.playback.speed = 2.5;
	section.logo.playback.animatedShadow = true;

	const Document document = documentWith(section);

	Document loaded;
	check(loaded.fromJson(document.toJson()), "the document round-trips through JSON");
	check(!loaded.sections.isEmpty(), "the section came back");
	if (loaded.sections.isEmpty())
		return;

	const LogoPlayback &playback = loaded.sections.first().logo.playback;
	checkEq(playback.loop, false, "loop survives");
	checkEq(playback.startOnEnter, true, "the entry trigger survives");
	checkNear(playback.speed, 2.5, 0.001, "the speed survives");
	checkEq(playback.animatedShadow, true, "the shadow setting survives");

	/* A logo saved before any of this existed has to load as a looping animation, not a frozen one. */
	OBSDataAutoRelease empty = obs_data_create();
	LogoRef legacy;
	legacy.load(empty);
	checkEq(legacy.playback.loop, true, "artwork from an older document loops by default");
	checkNear(legacy.playback.speed, 1.0, 0.001, "and plays at its own rate");
	checkEq(legacy.playback.startOnEnter, false, "and starts with the roll");

	/* Nonsense in a hand-edited document is clamped rather than carried into the renderer. */
	OBSDataAutoRelease wild = obs_data_create();
	obs_data_set_double(wild, "speed", 10000.0);
	LogoPlayback clamped;
	clamped.load(wild);
	check(clamped.speed <= kMaxLogoSpeed, "an absurd speed is clamped on load");
}
