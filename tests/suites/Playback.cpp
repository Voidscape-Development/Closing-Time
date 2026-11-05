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

#include <QPainter>

#include <cmath>

#include <obs.hpp>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"
#include "render/RollCompositor.hpp"
#include "render/RollTimeline.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

LogoCache &stills()
{
	static LogoCache cache;
	return cache;
}

/* A plain roll: a 1000 px frame and a 2000 px strip, at 100 px/s -- 30 s of travel. */
TimelineInputs plainRoll()
{
	TimelineInputs in;
	in.canvasHeight = 1000;
	in.stripHeight = 2000;
	in.scrollSpeed = 100.0;
	return in;
}

/* A card that pins in the middle of the frame, holds and ends the roll there. */
TimelineBlock holdingCard(double slotTop, double hold)
{
	TimelineBlock block;
	block.slotTop = slotTop;
	block.slotHeight = 200;
	block.pinnedTop = 400;
	block.hold = hold;
	block.release = StickyRelease::EndAtHold;
	return block;
}

/* Sections of these heights one under the next, from the top of the strip. */
QVector<TimelineSpan> stacked(std::initializer_list<double> heights)
{
	QVector<TimelineSpan> spans;
	double top = 0.0;
	for (double height : heights) {
		spans.append(TimelineSpan{top, height, false});
		top += height;
	}
	return spans;
}

TimelineInputs pagedRoll(PageTransition transition)
{
	TimelineInputs in;
	in.canvasHeight = 1000;
	in.stripHeight = 1500;
	in.mode = PlaybackMode::Pages;
	in.pageMargin = 50;
	in.pageHold = 4.0;
	in.pageTransition = transition;
	in.pageTransitionTime = 1.0;
	/* 900 px of room: the first two make a page, the third a page of its own. */
	in.spans = stacked({400, 400, 300});
	return in;
}

Section titled(const QString &text)
{
	Section section = Section::makeDefault(SectionType::Title);
	section.text = text;
	return section;
}

/* The canvas at roll time `t`, composited the way the designer's preview draws it. */
QImage canvasAt(const Document &document, double t)
{
	const RollRenderer renderer(&stills());
	const RollLayoutPtr layout = renderer.layout(document);
	TimelineInputs inputs = layout->timelineInputs();
	const RollFrame frame = RollTimeline(inputs).evaluate(t);

	ComposeView view = ComposeView::canvas(frame, document.height);
	view.fadeTop = document.edgeFadeTop;
	view.fadeBottom = document.edgeFadeBottom;
	view.fadeEdgeTop = 0.0;
	view.fadeEdgeBottom = document.height;

	QImage canvas(document.width, document.height, QImage::Format_ARGB32_Premultiplied);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);
	paintDrawListFaded(
		painter, composeFrame(*layout, frame, view),
		[&](const DrawSource &source) { return renderer.rasterize(*layout, source.layer, source.chunk); }, view,
		QRect(0, 0, document.width, document.height));
	painter.end();
	return canvas;
}

/* The strongest alpha anywhere in a band of rows. */
int peakAlpha(const QImage &image, int top, int bottom)
{
	int peak = 0;
	for (int y = std::max(0, top); y < std::min(image.height(), bottom); ++y) {
		const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
		for (int x = 0; x < image.width(); ++x)
			peak = std::max(peak, qAlpha(row[x]));
	}
	return peak;
}

} // namespace

CT_SUITE(fit_duration_scroll, "A scrolling roll fitted to a length plays in exactly that length")
{
	{
		TimelineInputs in = plainRoll();
		in.fitSeconds = 20.0;
		const RollTimeline timeline(in);
		check(timeline.fitMet(), "a plain roll can be fitted");
		checkNear(timeline.finishTime(), 20.0, 0.01, "and finishes when it was told to");
		checkNear(timeline.speedUsed(), 150.0, 0.1, "which is its travel over that time");
	}

	{
		/* A card's hold takes the same time at any speed, so the strip has to make up the rest. */
		TimelineInputs in = plainRoll();
		in.blocks = {holdingCard(1200, 8.0)};
		in.fitSeconds = 25.0;
		const RollTimeline timeline(in);
		check(timeline.fitMet(), "a roll ending on a card can be fitted");
		checkNear(timeline.finishTime(), 25.0, 0.01, "and finishes at the time given, hold and all");
		check(!std::isfinite(RollTimeline(plainRoll()).finishTime()) || timeline.speedUsed() > 0.0,
		      "at a speed of its own");
	}

	{
		/* The hold alone outlasts the time: as fast as it goes, and said so. */
		TimelineInputs in = plainRoll();
		in.blocks = {holdingCard(1200, 60.0)};
		in.fitSeconds = 30.0;
		const RollTimeline timeline(in);
		check(!timeline.fitMet(), "a time shorter than the card's own hold cannot be met");
		checkNear(timeline.speedUsed(), RollTimeline::kMaxFitSpeed, 1e-6,
			  "so the roll plays as fast as it can");
	}

	{
		/* A loop has no end, so a pass of it is what is fitted. */
		TimelineInputs in = plainRoll();
		in.loop = true;
		in.fitSeconds = 12.0;
		const RollTimeline timeline(in);
		check(!std::isfinite(timeline.finishTime()), "a loop still never finishes");
		checkNear(timeline.travelTime(), 12.0, 0.01, "but each pass takes the time given");
	}

	{
		/* Fitting moves everything that runs off the speed with it: a marker fires on the new schedule. */
		TimelineInputs in = plainRoll();
		in.cues = {TimelineCue{500.0, -1, 1000.0}};
		const double unfitted = RollTimeline(in).cueTimes().at(0);
		in.fitSeconds = 15.0;
		const RollTimeline fitted(in);
		checkNear(fitted.cueTimes().at(0), unfitted * 0.5, 0.01,
			  "a marker fires at half the time at twice the speed");
	}
}

CT_SUITE(pages_cutting, "A roll is cut into pages at what fits the frame, and at every Page Break")
{
	const RollTimeline timeline(pagedRoll(PageTransition::Fade));
	check(timeline.paged(), "the roll is played as pages");
	checkEq(timeline.pages().size(), 2, "two sections that fit together share a page; the third goes over");
	checkNear(timeline.pages().at(0).top, 0.0, 1e-9, "the first page starts at the top of the strip");
	checkNear(timeline.pages().at(0).bottom, 800.0, 1e-9, "and ends with the second section");
	checkNear(timeline.pages().at(1).top, 800.0, 1e-9, "the second starts with the third");

	/* A page that does not fill the frame sits where the anchor says, in what the margins leave. */
	checkNear(timeline.pages().at(0).rest, 50.0 + (900.0 - 800.0) / 2.0, 1e-9, "centered in the room");
	TimelineInputs top = pagedRoll(PageTransition::Fade);
	top.pageAnchor = 0.0;
	checkNear(RollTimeline(top).pages().at(1).rest, 50.0, 1e-9, "or against the top margin");

	/* A break starts a page even with room to spare. */
	TimelineInputs broken = pagedRoll(PageTransition::Fade);
	broken.spans.insert(1, TimelineSpan{400.0, 0.0, true});
	const RollTimeline split(broken);
	checkEq(split.pages().size(), 2, "a Page Break after the first section ends its page there");
	checkNear(split.pages().at(0).bottom, 400.0, 1e-9, "so the first page is that section alone");
	checkNear(split.pages().at(1).bottom, 1100.0, 1e-9, "and the next two, which fit together, share the second");

	/* A section taller than the frame is a page of its own. */
	TimelineInputs tall = pagedRoll(PageTransition::Fade);
	tall.spans = stacked({200, 1400, 200});
	checkEq(RollTimeline(tall).pages().size(), 3, "a section taller than the room gets a page to itself");

	TimelineInputs empty = pagedRoll(PageTransition::Fade);
	empty.spans.clear();
	const RollTimeline nothing(empty);
	checkEq(nothing.pages().size(), 0, "an empty roll has no pages");
	check(nothing.evaluate(0.0).finished, "and is over as soon as it starts");
}

CT_SUITE(pages_schedule, "Each page comes in, holds and goes out, in the transition chosen")
{
	{
		const RollTimeline fade(pagedRoll(PageTransition::Fade));
		const auto &pages = fade.pages();
		checkNear(pages.at(1).enter, pages.at(0).exit, 1e-9,
			  "under Fade a page comes in once the last has gone");
		checkNear(fade.finishTime(), 2 * (4.0 + 2.0), 1e-9, "two pages of a fade in, a hold and a fade out");

		const RollFrame between = fade.evaluate(pages.at(0).holdEnd + 0.5);
		checkEq(between.pages.size(), 1, "half way through a fade out, only the leaving page is up");
		checkNear(between.pages.at(0).alpha, 0.5, 1e-9, "at half strength");

		const RollFrame holding = fade.evaluate(2.5);
		checkEq(holding.pages.size(), 1, "while it holds, one page");
		checkNear(holding.pages.at(0).alpha, 1.0, 1e-9, "at full strength");
		checkNear(holding.pages.at(0).stripTop + holding.pages.at(0).top, pages.at(0).rest, 1e-9,
			  "with its top edge at its resting place");
	}

	{
		const RollTimeline cross(pagedRoll(PageTransition::Crossfade));
		const auto &pages = cross.pages();
		checkNear(pages.at(1).enter, pages.at(0).holdEnd, 1e-9,
			  "under Crossfade the next comes in as the last goes");
		const RollFrame overlap = cross.evaluate(pages.at(0).holdEnd + 0.25);
		checkEq(overlap.pages.size(), 2, "so both are up at once");
		checkNear(overlap.pages.at(0).alpha + overlap.pages.at(1).alpha, 1.0, 1e-9,
			  "and between them make one");
		checkNear(cross.finishTime(), 1.0 + 2 * (4.0 + 1.0), 1e-9,
			  "one fade in, then a hold and a crossfade a page");
	}

	{
		const RollTimeline cut(pagedRoll(PageTransition::Cut));
		checkNear(cut.finishTime(), 8.0, 1e-9, "a cut takes no time at all");
		checkEq(cut.evaluate(3.999).pages.size(), 1, "the first page is up until its hold runs out");
		checkNear(cut.evaluate(4.0).pages.at(0).top, 800.0, 1e-9, "and the next is up the moment it does");
		check(cut.evaluate(8.0).finished, "the roll is over when the last page is gone");
		checkEq(cut.evaluate(8.0).pages.size(), 0, "leaving the frame empty");
	}

	{
		/* A slide is one strip moving: the pages never overlap, and each stops where it rests. */
		const RollTimeline slide(pagedRoll(PageTransition::Slide));
		const auto &pages = slide.pages();
		const double canvas = 1000.0;
		const auto canvasTop = [](const PageFrame &frame) {
			return frame.stripTop + frame.top;
		};

		const RollFrame arriving = slide.evaluate(0.0);
		check(canvasTop(arriving.pages.at(0)) >= canvas - 1e-9, "the first page starts below the frame");

		bool overlapped = false;
		for (double t = pages.at(0).holdEnd; t <= pages.at(1).holdStart; t += 0.05) {
			const RollFrame frame = slide.evaluate(t);
			if (frame.pages.size() == 2) {
				const double leavingBottom = canvasTop(frame.pages.at(0)) +
							     (frame.pages.at(0).bottom - frame.pages.at(0).top);
				overlapped = overlapped || leavingBottom > canvasTop(frame.pages.at(1)) + 1e-6;
			}
		}
		check(!overlapped, "two pages sliding never overlap");

		const RollFrame resting = slide.evaluate(pages.at(1).holdStart + 0.1);
		checkEq(resting.pages.size(), 1, "once the slide is over, one page");
		checkNear(canvasTop(resting.pages.at(0)), pages.at(1).rest, 1e-9, "at its resting place");
		checkNear(resting.pages.at(0).alpha, 1.0, 1e-9, "a slide never fades");

		const RollFrame leaving = slide.evaluate(pages.at(1).exit - 1e-6);
		check(canvasTop(leaving.pages.at(0)) + (pages.at(1).bottom - pages.at(1).top) <= 1e-3,
		      "the last page leaves by the top of the frame");
	}

	{
		/* Looping goes back to the first page. */
		TimelineInputs in = pagedRoll(PageTransition::Cut);
		in.loop = true;
		const RollTimeline loop(in);
		check(!std::isfinite(loop.finishTime()), "a loop of pages never finishes");
		checkNear(loop.evaluate(8.5).pages.at(0).top, 0.0, 1e-9,
			  "and after the last page comes the first again");
		checkEq(loop.evaluate(8.5).pass, 1, "on the next pass");
	}

	{
		/* A marker fires as its page comes in; one between pages belongs to the page after it. */
		TimelineInputs in = pagedRoll(PageTransition::Fade);
		in.cues = {TimelineCue{100.0, -1, 0.0}, TimelineCue{850.0, -1, 500.0}, TimelineCue{800.0, -1, 0.0}};
		const RollTimeline timeline(in);
		checkNear(timeline.cueTimes().at(0), 0.0, 1e-9, "a marker on the first page fires as it comes in");
		checkNear(timeline.cueTimes().at(1), timeline.pages().at(1).enter, 1e-9,
			  "one on the second, as it does");
		checkNear(timeline.cueTimes().at(2), timeline.pages().at(1).enter, 1e-9,
			  "and one on the boundary goes with the page it comes before");
	}

	{
		/* Played as pages, a fit is met by the hold; the transitions keep their length. */
		for (PageTransition transition :
		     {PageTransition::Cut, PageTransition::Fade, PageTransition::Crossfade, PageTransition::Slide}) {
			const Context context(QString::fromLatin1(pageTransitionId(transition)));
			TimelineInputs in = pagedRoll(transition);
			in.fitSeconds = 30.0;
			const RollTimeline timeline(in);
			check(timeline.fitMet(), "thirty seconds can be met");
			checkNear(timeline.finishTime(), 30.0, 1e-9, "and is");
		}

		TimelineInputs tight = pagedRoll(PageTransition::Fade);
		tight.fitSeconds = 2.0;
		const RollTimeline timeline(tight);
		check(!timeline.fitMet(), "a time shorter than the transitions alone cannot be met");
		checkNear(timeline.pageHoldUsed(), 0.0, 1e-9, "so every page holds for no time at all");
	}

	{
		/* A scrub is a share of the running time, and never lands on the empty frame after the end. */
		const RollTimeline cut(pagedRoll(PageTransition::Cut));
		const RollFrame end = cut.evaluateScrubbed(2500.0);
		checkEq(end.pages.size(), 1, "scrubbed to the end, the last page is up");
		checkNear(end.pages.at(0).top, 800.0, 1e-9, "and it is the last page");
		checkNear(cut.evaluateScrubbed(0.0).pages.at(0).top, 0.0, 1e-9, "scrubbed to the start, the first");
	}
}

CT_SUITE(pages_on_canvas, "Played as pages, a frame shows one page's sections and nothing of the others")
{
	Document document = documentWith({titled(QStringLiteral("One")), Section::makeDefault(SectionType::PageBreak),
					  titled(QStringLiteral("Two"))});
	document.playbackMode = PlaybackMode::Pages;
	document.pageTransition = PageTransition::Cut;
	document.pageHold = 5.0;
	document.pageAnchor = StickyAnchor::Center;

	const RollRenderer renderer(&stills());
	const RollLayoutPtr layout = renderer.layout(document);
	checkEq(layout->spans.size(), 3, "the layout reports both titles and the break between them");
	check(layout->spans.at(1).pageBreak, "with the break marked as one");

	const RollTimeline timeline(layout->timelineInputs());
	checkEq(timeline.pages().size(), 2, "the break makes two pages of two titles that would fit on one");

	const RollFrame first = timeline.evaluate(1.0);
	const DrawList list = composeFrame(*layout, first, ComposeView::canvas(first, document.height));
	QVector<int> layers;
	for (const DrawItem &item : list) {
		if (!layers.contains(item.source.layer))
			layers.append(item.source.layer);
	}
	checkEq(layers.size(), 1, "the first page draws one section");
	checkEq(layout->layers.at(layers.at(0)).section, 0, "and it is the first title");

	/* The title sits in the middle of the frame, as the anchor says. */
	const QImage canvas = canvasAt(document, 1.0);
	saveArtifact(QStringLiteral("pages-first-page"), canvas);
	const Ink ink = inkOf(canvas);
	check(!ink.isEmpty(), "something is drawn");
	checkNear((ink.top + ink.bottom) / 2.0, document.height / 2.0, document.height * 0.06,
		  "the page's ink is centered on the frame");

	const Ink second = inkOf(canvasAt(document, 6.0));
	check(!second.isEmpty(), "the second page is drawn after the first one's hold");

	/* The same roll scrolled ignores the break: it is one strip. */
	Document scrolled = document;
	scrolled.playbackMode = PlaybackMode::Scroll;
	checkEq(measure(scrolled), measure(document), "a Page Break takes no room in the strip");
}

CT_SUITE(edge_fade, "The roll fades out at the frame's edges, and a card does not")
{
	ComposeView view;
	view.fadeTop = 100.0;
	view.fadeBottom = 50.0;
	view.fadeEdgeTop = 0.0;
	view.fadeEdgeBottom = 1000.0;
	checkNear(edgeFadeAt(view, 0.0), 0.0, 1e-9, "nothing at the top edge");
	checkNear(edgeFadeAt(view, 50.0), 0.5, 1e-9, "half way up the top fade, half strength");
	checkNear(edgeFadeAt(view, 100.0), 1.0, 1e-9, "full strength past it");
	checkNear(edgeFadeAt(view, 500.0), 1.0, 1e-9, "and through the middle");
	checkNear(edgeFadeAt(view, 1000.0), 0.0, 1e-9, "nothing at the bottom edge");
	checkNear(edgeFadeAt(view, 975.0), 0.5, 1e-9, "the bottom fade has its own height");
	check(edgeFadeAt(view, 25.0) < 0.25, "eased: the fade starts gently rather than as a ramp");
	view.fadeTop = 0.0;
	view.fadeBottom = 0.0;
	checkNear(edgeFadeAt(view, 0.0), 1.0, 1e-9, "with no fade the edge is hard");

	/* Over a real roll: a tall title parked across the top edge is dimmed there, not at the middle. */
	Section title = titled(QStringLiteral("FADE"));
	title.style.pixelSize = 400;
	Document document = documentWith({title});
	document.leadIn = 0;
	document.manualScroll = true;
	document.edgeFadeTop = 300;

	const RollRenderer renderer(&stills());
	const RollLayoutPtr layout = renderer.layout(document);
	const RollTimeline timeline(layout->timelineInputs());
	/* Parked with the title running off the canvas's top edge. */
	const RollFrame frame = timeline.evaluateScrubbed(document.height + 120);

	const auto composite = [&](int fadeTop) {
		ComposeView canvasView = ComposeView::canvas(frame, document.height);
		canvasView.fadeTop = fadeTop;
		canvasView.fadeEdgeBottom = document.height;
		QImage canvas(document.width, document.height, QImage::Format_ARGB32_Premultiplied);
		canvas.fill(Qt::transparent);
		QPainter painter(&canvas);
		paintDrawListFaded(
			painter, composeFrame(*layout, frame, canvasView),
			[&](const DrawSource &source) {
				return renderer.rasterize(*layout, source.layer, source.chunk);
			},
			canvasView, QRect(0, 0, document.width, document.height));
		return canvas;
	};

	const QImage hard = composite(0);
	const QImage faded = composite(300);
	saveArtifact(QStringLiteral("edge-fade-off"), hard);
	saveArtifact(QStringLiteral("edge-fade-on"), faded);
	const Ink ink = inkOf(hard);
	check(ink.top < 40 && ink.bottom > 305, "the title reaches from the top edge to below the fade");
	check(peakAlpha(hard, 0, 40) > 200, "unfaded, the title is solid at the top edge");
	check(peakAlpha(faded, 0, 40) < peakAlpha(hard, 0, 40) / 4, "faded, it is all but gone there");
	checkEq(peakAlpha(faded, 300, 320), peakAlpha(hard, 300, 320), "and untouched below the fade");

	/* What is drawn: everything in the roll fades, a card never does. */
	Section card = Section::makeDefault(SectionType::StickyBlock);
	const RollLayoutPtr withCard = renderer.layout(documentWith({titled(QStringLiteral("Roll")), card}));
	const RollTimeline cardTimeline(withCard->timelineInputs());
	const RollFrame late = cardTimeline.evaluate(cardTimeline.schedules().at(0).pin + 0.5);
	bool rollFades = false;
	bool cardFades = false;
	for (const DrawItem &item : composeFrame(*withCard, late, ComposeView::canvas(late, document.height))) {
		const bool isCard = withCard->layers.at(item.source.layer).kind == RollLayer::Kind::Block;
		(isCard ? cardFades : rollFades) |= item.edgeFade;
	}
	check(rollFades, "the roll itself takes the fade");
	check(!cardFades, "a pinned card is left at full strength");

	/* On its way up, before it pins, the card is part of the roll's movement and fades with it. */
	const RollFrame rising = cardTimeline.evaluate(cardTimeline.schedules().at(0).pin * 0.5);
	bool risingCardFades = false;
	bool risingCardDrawn = false;
	for (const DrawItem &item : composeFrame(*withCard, rising, ComposeView::canvas(rising, document.height))) {
		if (withCard->layers.at(item.source.layer).kind == RollLayer::Kind::Block) {
			risingCardDrawn = true;
			risingCardFades |= item.edgeFade;
		}
	}
	check(risingCardDrawn, "the card is on the frame on its way up");
	check(risingCardFades, "and fades at the edges while it travels with the roll");
}

CT_SUITE(playback_settings_persist, "The playback settings are saved, loaded and kept out of the content")
{
	Document document;
	document.playbackMode = PlaybackMode::Pages;
	document.fitDuration = true;
	document.fitSeconds = 92.5;
	document.edgeFadeTop = 0;
	document.edgeFadeBottom = 120;
	document.pageHold = 0.0;
	document.pageTransition = PageTransition::Slide;
	document.pageTransitionTime = 0.0;
	document.pageAnchor = StickyAnchor::Bottom;
	document.pageMargin = 0;

	OBSDataAutoRelease data = obs_data_create();
	document.save(data);
	Document loaded;
	loaded.load(data);
	check(loaded.playbackMode == PlaybackMode::Pages, "the mode");
	check(loaded.fitDuration, "the fit");
	checkNear(loaded.fitSeconds, 92.5, 1e-9, "its length");
	checkEq(loaded.edgeFadeBottom, 120, "the bottom fade");
	checkNear(loaded.pageHold, 0.0, 1e-9, "a hold of nothing stays nothing");
	check(loaded.pageTransition == PageTransition::Slide, "the transition");
	checkNear(loaded.pageTransitionTime, 0.0, 1e-9, "an instant transition stays instant");
	check(loaded.pageAnchor == StickyAnchor::Bottom, "where a page sits");
	checkEq(loaded.pageMargin, 0, "a margin of nothing stays nothing");

	OBSDataAutoRelease bare = obs_data_create();
	Document old;
	old.load(bare);
	check(old.playbackMode == PlaybackMode::Scroll, "a roll written before pages scrolls");
	check(!old.fitDuration, "at its own speed");
	checkEq(old.edgeFadeTop + old.edgeFadeBottom, 0, "with hard edges");
	checkNear(old.pageTransitionTime, 0.6, 1e-9, "and the default transition waiting for it");

	/* None of it is content: changing it lays nothing out again. */
	Document changed = document;
	changed.playbackMode = PlaybackMode::Scroll;
	changed.fitSeconds = 10.0;
	changed.edgeFadeTop = 400;
	changed.pageHold = 9.0;
	changed.pageMargin = 200;
	check(changed.sameContent(document), "the playback settings never send the roll to be laid out again");
}
