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

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QThread>

#include <cstdlib>

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"
#include "render/RollCompositor.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

LogoCache &stills()
{
	static LogoCache cache;
	return cache;
}

RollLayoutPtr layoutOf(const Document &document)
{
	return RollRenderer(&stills()).layout(document);
}

/* The section layers' keys, in order. */
QVector<quint64> sectionKeys(const RollLayout &layout)
{
	QVector<quint64> keys;
	for (const RollLayer &layer : layout.layers) {
		if (layer.kind == RollLayer::Kind::Section)
			keys.append(layer.key);
	}
	return keys;
}

/*
 * The largest difference in any channel between two premultiplied pictures of the same size, and
 * how many pixels differ at all. -1 when they are not the same size.
 */
struct Difference {
	int largest = -1;
	int pixels = 0;
};

Difference compare(const QImage &a, const QImage &b)
{
	Difference difference;
	if (a.size() != b.size())
		return difference;

	difference.largest = 0;
	for (int y = 0; y < a.height(); ++y) {
		const auto *ra = reinterpret_cast<const QRgb *>(a.constScanLine(y));
		const auto *rb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
		for (int x = 0; x < a.width(); ++x) {
			if (ra[x] == rb[x])
				continue;
			++difference.pixels;
			for (int shift : {0, 8, 16, 24}) {
				const int delta = std::abs(static_cast<int>((ra[x] >> shift) & 0xff) -
							   static_cast<int>((rb[x] >> shift) & 0xff));
				difference.largest = std::max(difference.largest, delta);
			}
		}
	}
	return difference;
}

Section namedTitle(const QString &text)
{
	Section section = Section::makeDefault(SectionType::Title);
	section.text = text;
	return section;
}

} // namespace

CT_SUITE(layers_match_one_picture, "Separate pictures composite to the roll one picture would have been")
{
	/*
	 * The whole bargain: a picture per section, composited in order, is the same roll as every
	 * section painted into one shared picture. Over is associative, so the only place they may differ
	 * is where two sections' pictures overlap and each was rounded to eight bits before being
	 * blended -- a level or two in a channel, never more.
	 */
	QVector<Document> documents;
	for (const Scene &scene : scenes())
		documents.append(scene.document);
	documents.append(everySectionType());

	/*
	 * Type set tighter than its font, with nothing underneath: the glyphs ink well outside the line
	 * the layout gave them, which one shared picture caught in whatever lay beneath and a picture
	 * of its own has to leave room for.
	 */
	Section tight = Section::makeDefault(SectionType::Title);
	tight.text = QStringLiteral("Àgqy Ü\nÉpjg Ç");
	tight.style.pixelSize = 72;
	tight.style.lineSpacing = 0.5;
	tight.paddingTop = 0;
	tight.paddingBottom = 0;
	Document tightDocument = documentWith({tight, tight});
	tightDocument.leadIn = 200;
	tightDocument.leadOut = 200;
	documents.append(tightDocument);

	for (int i = 0; i < documents.size(); ++i) {
		const Context context(i < scenes().size() ? scenes().at(i).name : QStringLiteral("extra %1").arg(i));
		const Document &document = documents.at(i);

		/*
		 * Painted once before anything is compared. Qt draws a run of glyphs slightly differently
		 * the first time it meets them at a size than every time after -- a few levels at the
		 * edges, whichever way the roll is being painted -- and a comparison of a first painting
		 * against a second would be measuring that rather than the layering.
		 */
		const RollRenderer renderer(&stills());
		renderer.paintReference(document);

		const QImage layered = flatten(renderer.render(document));
		const QImage reference = renderer.paintReference(document);

		if (reference.isNull()) {
			check(layered.isNull(), "an empty roll is empty both ways");
			continue;
		}

		const Difference difference = compare(layered, reference);
		check(difference.largest >= 0, "the two pictures are the same size");
		check(difference.largest <= 2,
		      QStringLiteral("and agree to within rounding (largest difference %1 over %2 pixels)")
			      .arg(difference.largest)
			      .arg(difference.pixels));
	}
}

CT_SUITE(layers_position_independent, "A section's picture does not depend on where in the roll it sits")
{
	/*
	 * What lets a picture be reused across a rebuild: the same section with something new above it
	 * is the same picture and the same key, only further down.
	 */
	Section heading = namedTitle(QStringLiteral("Directed by"));
	heading.style.shadow.enabled = true;

	/*
	 * Leads at both ends, so that neither picture is cut by an end of the strip -- a picture that is
	 * cut there is a different picture, and rightly a different key.
	 */
	Document alone = documentWith(heading);
	Document pushed = documentWith({namedTitle(QStringLiteral("Above")), heading});
	for (Document *document : {&alone, &pushed}) {
		document->leadIn = 400;
		document->leadOut = 400;
	}

	const RollLayoutPtr first = layoutOf(alone);
	const RollLayoutPtr second = layoutOf(pushed);
	if (first->layers.isEmpty() || second->layers.size() < 2) {
		fail(QStringLiteral("the layouts have the layers they should"));
		return;
	}

	const RollLayer &a = first->layers.at(0);
	const RollLayer &b = second->layers.at(1);
	check(b.sectionTop > a.sectionTop, "the second roll really does put it further down");
	check(a.key == b.key, "it keeps its key");

	const RollRenderer renderer(&stills());
	const Difference difference = compare(renderer.rasterizeLayer(*first, 0), renderer.rasterizeLayer(*second, 1));
	checkEq(difference.largest, 0, "and its picture is identical, pixel for pixel");
}

CT_SUITE(layers_keys_are_local, "An edit changes the keys of what it touched and nothing else")
{
	Section a = namedTitle(QStringLiteral("Alpha"));
	Section b = namedTitle(QStringLiteral("Bravo"));
	Section c = namedTitle(QStringLiteral("Charlie"));
	Document document = documentWith({a, b, c});

	const QVector<quint64> before = sectionKeys(*layoutOf(document));

	document.sections[1].text = QStringLiteral("Bravo Two");
	const QVector<quint64> edited = sectionKeys(*layoutOf(document));
	checkEq(edited.size(), 3, "three sections, three layers");
	if (edited.size() == 3 && before.size() == 3) {
		check(edited.at(0) == before.at(0) && edited.at(2) == before.at(2),
		      "editing one leaves the others alone");
		check(edited.at(1) != before.at(1), "and changes the one that was edited");
	}

	/* A preset bound to the first section only: editing it moves that section's key alone. */
	document.sections[1].text = QStringLiteral("Bravo");
	TextStyle preset = document.sections.at(0).style;
	document.setStylePreset(QStringLiteral("Lead"), preset);
	document.sections[0].stylePresetName = QStringLiteral("Lead");
	const QVector<quint64> bound = sectionKeys(*layoutOf(document));

	preset.color = QColor(255, 0, 0);
	document.setStylePreset(QStringLiteral("Lead"), preset);
	const QVector<quint64> recolored = sectionKeys(*layoutOf(document));
	if (bound.size() == 3 && recolored.size() == 3) {
		check(recolored.at(0) != bound.at(0), "editing a preset changes the section bound to it");
		check(recolored.at(1) == bound.at(1) && recolored.at(2) == bound.at(2), "and no section that is not");
	}

	/* A playback setting is not part of any picture. */
	document.scrollSpeed *= 2.0;
	document.loop = !document.loop;
	check(sectionKeys(*layoutOf(document)) == recolored, "changing playback changes no key");

	/* The canvas width is part of every picture. */
	document.width += 100;
	const QVector<quint64> wider = sectionKeys(*layoutOf(document));
	if (wider.size() == 3)
		check(wider.at(0) != recolored.at(0) && wider.at(1) != recolored.at(1),
		      "changing the width changes them all");
}

CT_SUITE(layers_file_dependencies, "A picture knows which files it was drawn from")
{
	QTemporaryDir directory;
	if (!directory.isValid()) {
		fail(QStringLiteral("a temporary directory to write a logo into"));
		return;
	}

	const QString path = directory.filePath(QStringLiteral("logo.png"));
	QImage art(32, 32, QImage::Format_ARGB32_Premultiplied);
	art.fill(QColor(200, 40, 40));
	check(art.save(path), "the logo is written");

	Section logo = unpadded(SectionType::TitleWithLogo);
	logo.text = QStringLiteral("Sponsor");
	logo.logo.path = path;
	Section plain = namedTitle(QStringLiteral("No art"));

	/* A cache of its own, so the logo is read here rather than remembered from another suite. */
	LogoCache cache;
	const RollRenderer renderer(&cache);
	const RollLayoutPtr layout = renderer.layout(documentWith({logo, plain}));
	if (layout->layers.size() < 2) {
		fail(QStringLiteral("two layers"));
		return;
	}

	FileStamps withArt;
	renderer.rasterize(*layout, 0, 0, 1.0, &withArt);
	FileStamps without;
	renderer.rasterize(*layout, 1, 0, 1.0, &without);

	checkEq(withArt.size(), 1, "the section with a logo depends on one file");
	if (!withArt.isEmpty())
		checkEq(withArt.first().path, path, "the logo's");
	check(without.isEmpty(), "and the section without one depends on none");
	check(fileStampsCurrent(withArt), "a file nobody has touched is current");

	/* A different picture of a different size, so the stamp moves even on a coarse clock. */
	QImage replaced(48, 48, QImage::Format_ARGB32_Premultiplied);
	replaced.fill(QColor(40, 200, 40));
	check(replaced.save(path), "the logo is replaced");
	check(!fileStampsCurrent(withArt), "and the picture drawn from it knows it is out of date");

	QFile::remove(path);
	check(!fileStampsCurrent(withArt), "as it does when the file goes away");
}

CT_SUITE(layers_chunks, "A tall picture is cut into chunks that meet at every scale")
{
	Section list = Section::makeDefault(SectionType::TextList);
	for (int i = 0; i < 120; ++i)
		list.entries.append(Entry{QStringLiteral("Name %1").arg(i), {}, {}, {}, {}});

	const RollLayoutPtr layout = layoutOf(documentWith(list));
	if (layout->layers.isEmpty()) {
		fail(QStringLiteral("a layer"));
		return;
	}

	const RollLayer &layer = layout->layers.first();
	check(layer.pictureHeight > kChunkHeight * 2, "the fixture is several chunks tall");
	checkEq(layer.chunkCount(), (layer.pictureHeight + kChunkHeight - 1) / kChunkHeight,
		"and cut into as many as it takes");

	for (qreal scale : {1.0, 0.5, 0.37, 1.5}) {
		const Context context(QStringLiteral("scale %1").arg(scale));

		int expectedTop = chunkRows(layer, 0, scale).top;
		int total = 0;
		for (int chunk = 0; chunk < layer.chunkCount(); ++chunk) {
			const ChunkRows rows = chunkRows(layer, chunk, scale);
			checkEq(rows.top, expectedTop, "each chunk starts where the last one stopped");
			expectedTop = rows.top + rows.height;
			total += rows.height;
		}
		check(total >= qFloor(layer.pictureHeight * scale), "and between them they cover the picture");
		check(chunkKey(layer, 0, scale) != chunkKey(layer, 1, scale), "every chunk has a key of its own");
	}

	check(chunkKey(layer, 0, 1.0) != chunkKey(layer, 0, 0.5), "and so does every scale");

	const RollRenderer renderer(&stills());
	/*
	 * Cut down to its ink: the picture sits inside the chunk's full rectangle at its offset, and a
	 * column of names in the middle of a wide canvas is a fraction of the canvas wide.
	 */
	const QImage half = renderer.rasterize(*layout, 0, 1, 0.5);
	const QRect full(0, 0, qCeil(layout->width * 0.5), chunkRows(layer, 1, 0.5).height);
	check(full.contains(QRect(half.offset(), half.size())), "a half-scale chunk lies inside its rows");
	check(half.width() < full.width() / 2, "and is cut down to the names, a fraction of the canvas wide");
	check(!inkOf(half).isEmpty(), "with the names drawn into it");
	checkEq(qAlpha(half.pixel(0, half.height() / 2)), 0, "keeping a transparent edge for filtering to fade into");

	/* Nothing in it at all: a single transparent pixel rather than a canvas-wide nothing. */
	Section spacer = Section::makeDefault(SectionType::Spacer);
	spacer.spacerHeight = 300;
	const RollLayoutPtr blank = layoutOf(documentWith(spacer));
	if (!blank->layers.isEmpty()) {
		const QImage nothing = renderer.rasterize(*blank, 0, 0);
		check(nothing.size() == QSize(1, 1) && qAlpha(nothing.pixel(0, 0)) == 0,
		      "an empty chunk is a single transparent pixel");
	}
}

CT_SUITE(layers_cancelled, "A layout that has been overtaken stops")
{
	Document document = documentWith({namedTitle(QStringLiteral("One")), namedTitle(QStringLiteral("Two"))});

	int asked = 0;
	const RollLayoutPtr abandoned = RollRenderer(&stills()).layout(document, false, [&asked] {
		++asked;
		return true;
	});
	check(!abandoned, "a cancelled layout comes back empty-handed");
	checkEq(asked, 1, "having asked once, before the first section");

	const RollLayoutPtr finished = RollRenderer(&stills()).layout(document, false, [] { return false; });
	check(finished && finished->layers.size() == 2, "and one nobody cancels finishes");
}

CT_SUITE(layers_blocks, "A sticky block is a layer of its own")
{
	Section block = Section::makeDefault(SectionType::StickyBlock);
	block.children.clear();
	block.children.push_back(namedTitle(QStringLiteral("The End")));
	block.stickyHold = 3.0;

	const Document document = documentWith({namedTitle(QStringLiteral("Cast")), block});
	const RollLayoutPtr layout = layoutOf(document);

	checkEq(layout->blocks.size(), 1, "the block is placed");
	checkEq(layout->layers.size(), 2, "one layer for the section, one for the block -- none for its empty slot");
	if (layout->blocks.isEmpty() || layout->layers.size() < 2)
		return;

	const StickyBlockPlacement &placement = layout->blocks.first();
	const RollLayer &layer = layout->layers.at(placement.layer);
	check(layer.kind == RollLayer::Kind::Block, "the block's layer says what it is");
	checkEq(layer.pictureTop, -placement.margin, "its picture starts its margin above the slot");
	checkEq(layer.pictureHeight, qRound(placement.rect.height()) + placement.margin * 2,
		"and runs past it by the same");

	const TimelineInputs inputs = layout->timelineInputs();
	checkEq(inputs.blocks.size(), 1, "the timeline is told about it");
	if (!inputs.blocks.isEmpty()) {
		checkNear(inputs.blocks.first().pinnedTop, placement.pinnedTop(document.height), 1e-9,
			  "with the pin the placement works out");
		checkNear(inputs.blocks.first().hold, 3.0, 1e-9, "and the hold it was given");
	}
	checkEq(inputs.stripHeight, layout->height, "and the strip it rides on");
}

CT_SUITE(compositor_in_slots, "The roll end to end, composited from its layers")
{
	/*
	 * The designer's view: every section where the roll puts it and every block back in its slot.
	 * Composited from the draw list, it is the eager render's tiles with each block's picture laid
	 * into its slot -- the same pixels, arrived at the way the preview arrives at them.
	 */
	Section block = Section::makeDefault(SectionType::StickyBlock);
	block.children.clear();
	block.children.push_back(namedTitle(QStringLiteral("The End")));
	const Document document =
		documentWith({namedTitle(QStringLiteral("Cast")), block, namedTitle(QStringLiteral("Crew"))});

	const RollRenderer renderer(&stills());
	renderer.paintReference(document);
	const RollLayoutPtr layout = renderer.layout(document);

	ComposeView view;
	view.stripTop = 0.0;
	view.clipTop = 0.0;
	view.clipBottom = layout->height;
	view.blocks = ComposeView::Blocks::InSlots;

	const DrawList list = composeFrame(*layout, RollFrame(), view);
	check(!list.isEmpty(), "there is something to draw");

	QImage composed(layout->width, layout->height, QImage::Format_ARGB32_Premultiplied);
	composed.fill(Qt::transparent);
	{
		QPainter painter(&composed);
		paintDrawList(painter, list, [&](const DrawSource &source) {
			return renderer.rasterize(*layout, source.layer, source.chunk);
		});
	}

	const Strip strip = renderer.render(document);
	QImage expected = flatten(strip);
	{
		QPainter painter(&expected);
		for (const StickyBlockPlacement &placement : strip.stickyBlocks)
			painter.drawImage(QPointF(0, placement.rect.top() - placement.margin), placement.image);
	}

	checkEq(compare(composed, expected).largest, 0,
		"the composited roll is the eager render, blocks in their slots");
}

CT_SUITE(compositor_canvas, "One moment of the roll on the canvas")
{
	Section block = Section::makeDefault(SectionType::StickyBlock);
	block.children.clear();
	block.children.push_back(namedTitle(QStringLiteral("The End")));
	block.stickyEntrance = StickyEntrance::AfterRoll;
	block.stickyFadeIn = 2.0;

	Document document = documentWith({namedTitle(QStringLiteral("Cast")), block});
	document.height = 720;
	const RollLayoutPtr layout = layoutOf(document);
	const RollTimeline timeline(layout->timelineInputs());

	/* At the start the strip is still wholly below the canvas: nothing is drawn. */
	const RollFrame start = timeline.evaluate(0.0);
	checkEq(composeFrame(*layout, start, ComposeView::canvas(start, document.height)).size(), 0,
		"nothing is on the canvas before the roll has entered it");

	/* Partway in, only what is inside the canvas is drawn, and none of it outside. */
	const RollFrame partway = timeline.evaluate(timeline.travelTime() * 0.25);
	const DrawList rolling = composeFrame(*layout, partway, ComposeView::canvas(partway, document.height));
	check(!rolling.isEmpty(), "the roll is on the canvas once it has entered");
	bool inside = true;
	for (const DrawItem &item : rolling)
		inside = inside && item.target.top() >= 0.0 && item.target.bottom() <= document.height;
	check(inside, "and every draw is cut to the canvas");

	/* Half way through its fade, the card is drawn at half strength, where the pin puts it. */
	const RollTimeline::BlockSchedule &times = timeline.schedules().at(0);
	const RollFrame fading = timeline.evaluate(times.enter + 1.0);
	const DrawList cardList = composeFrame(*layout, fading, ComposeView::canvas(fading, document.height));
	const StickyBlockPlacement &placement = layout->blocks.first();
	bool found = false;
	for (const DrawItem &item : cardList) {
		if (item.source.layer != placement.layer)
			continue;
		found = true;
		checkNear(item.alpha, 0.5, 1e-9, "the card is drawn at its fade's opacity");
		checkNear(item.target.top(), std::max(0.0, placement.pinnedTop(document.height) - placement.margin),
			  1e-6, "at the pin");
	}
	check(found, "the card is drawn while it fades up");
}

CT_SUITE(raster_cache, "Rasterized chunks are kept, aged out, and dropped when their files change")
{
	QImage image(4, 4, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	const qint64 each = image.sizeInBytes();

	RasterCache cache(each * 2);
	cache.insert(1, image, {});
	cache.insert(2, image, {});
	check(!cache.find(1).isNull(), "a chunk put in comes back out");

	/* 1 was just used, so 2 is the oldest when a third arrives. */
	cache.insert(3, image, {});
	check(cache.find(2).isNull(), "the least recently used goes first once the budget is full");
	check(!cache.find(1).isNull() && !cache.find(3).isNull(), "and the others stay");
	checkEq(cache.bytes(), each * 2, "the cache holds what its budget allows");

	QTemporaryDir directory;
	const QString path = directory.filePath(QStringLiteral("art.svg"));
	{
		QFile file(path);
		check(file.open(QIODevice::WriteOnly), "an artwork file is written");
		file.write("<svg/>");
	}

	FileStamps files;
	{
		const FileDependencyScope scope(&files);
		noteFileRead(path);
	}
	cache.insert(4, image, files);
	check(!cache.find(4).isNull(), "a chunk drawn from a file is held while the file is unchanged");

	{
		QFile file(path);
		check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "the file is rewritten");
		file.write("<svg width='10'/>");
	}
	check(cache.find(4).isNull(), "and dropped once it changes");
}

CT_SUITE(layers_long_lists, "A list many chunks tall paints only the rows each chunk shows, and the same pixels")
{
	/*
	 * Every chunk of a long list measures nothing and paints only the rows that reach it, skipping
	 * the rest by the heights the layout recorded. That has to come out identical to painting every
	 * row into one picture, for every shape of list, with a shadow reaching across the chunk seams.
	 */
	for (SectionType type :
	     {SectionType::TextList, SectionType::TitleSubtitleList, SectionType::LogoList, SectionType::MultiTextList,
	      SectionType::MultiTitleSubtitleList, SectionType::Bridged}) {
		const Context context(QString::fromLatin1(sectionTypeId(type)));

		Section list = Section::makeDefault(type);
		list.entries.clear();
		/* Enough rows for two chunks: twice as many for a list in two columns. */
		const int count = sectionUsesColumns(type) ? 80 : 40;
		for (int i = 0; i < count; ++i) {
			Entry entry{QStringLiteral("Name %1 with descenders gjpqy").arg(i),
				    QStringLiteral("Role %1").arg(i),
				    {},
				    {},
				    {}};
			list.entries.append(entry);
		}
		if (type == SectionType::LogoList)
			withLogo(list, 60);
		/*
		 * One shape carries a shadow, which is what reaches across a chunk seam from a row painted
		 * into the chunk on the other side; a blur per row is slow enough that all six would make
		 * this the longest suite in the run for no more coverage.
		 */
		if (type == SectionType::TextList) {
			list.style.shadow.enabled = true;
			list.style.shadow.blur = 6.0;
			list.style.shadow.offsetY = 10.0;
		}

		Document document = documentWith({namedTitle(QStringLiteral("Above")), list});
		document.leadOut = 100;

		const RollRenderer renderer(&stills());
		renderer.paintReference(document);

		const RollLayoutPtr layout = renderer.layout(document);
		const RollLayer *tall = nullptr;
		for (const RollLayer &layer : layout->layers) {
			if (layer.section == 1)
				tall = &layer;
		}
		check(tall && tall->chunkCount() >= 2, "the list is more than one chunk tall");
		check(type == SectionType::LogoList || (tall && tall->rowHeights && !tall->rowHeights->isEmpty()),
		      "and its row heights were recorded by the layout");

		const Difference difference =
			compare(flatten(renderer.render(document)), renderer.paintReference(document));
		check(difference.largest >= 0 && difference.largest <= 2,
		      QStringLiteral(
			      "and painting it a chunk at a time changes no pixel (largest difference %1 over %2)")
			      .arg(difference.largest)
			      .arg(difference.pixels));
	}
}
