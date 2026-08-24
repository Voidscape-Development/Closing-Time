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

#include "harness/Fixtures.hpp"
#include "harness/Harness.hpp"
#include "harness/Probe.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* A one-row bridged section with nothing but its own content in the document. */
Section bridgedRow(const QString &left, const QString &right)
{
	Section section = unpadded(SectionType::Bridged);
	section.marginX = 0;
	section.entries = {Entry{left, right, {}, {}, {}}};
	return section;
}

/* The two text columns of the first row, left first. */
QPair<QRectF, QRectF> textColumns(const Document &document)
{
	const QVector<QRectF> texts = boxesOf(document, LayoutBox::Kind::Text);
	if (texts.size() < 2)
		return {};

	return {texts.at(0), texts.at(1)};
}

} // namespace

CT_SUITE(bridge_none_gap, "An empty bridge keeping its two texts apart")
{
	/*
	 * Natural sizing is where an empty bridge could go wrong: each column takes only the width
	 * its own text needs, and with no leader between them there is nothing left to hold them
	 * off each other except the minimum this type carries for exactly that reason.
	 */
	for (int minGap : {0, 24, 120}) {
		const Context context(QStringLiteral("minGap=%1").arg(minGap));

		Section section = bridgedRow(QStringLiteral("Director"), QStringLiteral("Jane Doe"));
		section.bridgeType = BridgeType::None;
		section.bridgeSizing = BridgeSizing::Natural;
		section.bridgeFill = BridgeFill::Fixed;
		section.bridgeMinGap = minGap;

		const Document document = documentWith(section);
		const auto [left, right] = textColumns(document);

		check(!left.isNull() && !right.isNull(), "both columns placed");
		checkNear(right.left() - left.right(), minGap, 1.0, "the gap is the minimum asked for");

		/*
		 * Nothing is drawn in it. Measured across the middle of the gap rather than its full
		 * width, because a glyph may ink a pixel or two outside the column it was laid out
		 * in -- a negative left side bearing is ordinary type design, not a bridge.
		 */
		if (minGap >= 8) {
			const qreal inset = 4.0;
			const QRectF middle(left.right() + inset, left.top(), minGap - inset * 2.0,
					    std::max(1.0, left.height()));
			check(inkOf(renderImage(document), middle).isEmpty(), "nothing is drawn in the gap");
		}
	}
}

CT_SUITE(bridge_none_fill_ignored, "An empty bridge laid out the same whatever its fill says")
{
	/*
	 * `bridgeFill` is hidden for an empty bridge and forced to Fixed by the model, so all three
	 * settings have to produce the identical row -- otherwise the setting is doing something
	 * invisible from a control the user cannot see.
	 */
	Section section = bridgedRow(QStringLiteral("Editor"), QStringLiteral("R Singh"));
	section.bridgeType = BridgeType::None;
	section.bridgeSizing = BridgeSizing::Natural;
	section.bridgeMinGap = 40;

	section.bridgeFill = BridgeFill::Fixed;
	const auto [fixedLeft, fixedRight] = textColumns(documentWith(section));

	for (BridgeFill fill : {BridgeFill::Repeat, BridgeFill::Stretch}) {
		const Context context(QStringLiteral("fill=%1").arg(bridgeFillId(fill)));

		Section filled = section;
		filled.bridgeFill = fill;
		const auto [left, right] = textColumns(documentWith(filled));

		checkNear(left.left(), fixedLeft.left(), 0.5, "left column unmoved");
		checkNear(right.left(), fixedRight.left(), 0.5, "right column unmoved");
	}

	/* And the model agrees, so nothing downstream has to special-case it a second time. */
	Section stretched = section;
	stretched.bridgeFill = BridgeFill::Stretch;
	check(effectiveBridgeFill(stretched) == BridgeFill::Fixed, "an empty bridge is laid out as Fixed");
	stretched.bridgeType = BridgeType::Dots;
	check(effectiveBridgeFill(stretched) == BridgeFill::Stretch, "and a drawn one keeps its own fill");
}

CT_SUITE(bridge_none_split, "An empty bridge reserving its gap under Split sizing too")
{
	Section section = bridgedRow(QStringLiteral("Director of Photography"), QStringLiteral("A Longer Name"));
	section.bridgeType = BridgeType::None;
	section.bridgeSizing = BridgeSizing::Split;
	section.bridgeSplit = 0.5;
	section.bridgeMinGap = 60;

	const Document document = documentWith(section);
	const auto [left, right] = textColumns(document);

	check(!left.isNull() && !right.isNull(), "both columns placed");
	checkNear(right.left() - left.right(), 60.0, 1.5, "the reserved gap is the minimum");
	/* Split sizing still fills the section: the row reaches both edges of the content box. */
	checkNear(left.left() + left.width() + 60.0 + right.width(), left.left() + document.width, 2.0,
		  "the row still spans the section");
}

CT_SUITE(bridge_subtitles_stack, "Subtitles under each side of a bridged row")
{
	Section plain = bridgedRow(QStringLiteral("Director"), QStringLiteral("Jane Doe"));
	plain.bridgeType = BridgeType::Dots;
	plain.bridgeFill = BridgeFill::Repeat;

	Section stacked = plain;
	stacked.rowSubtitles = true;
	stacked.entries = {Entry{QStringLiteral("Director"), QStringLiteral("Jane Doe"),
				 QStringLiteral("second unit"), QStringLiteral("BSC"), {}}};

	const int plainHeight = measure(documentWith(plain));
	const int stackedHeight = measure(documentWith(stacked));
	check(stackedHeight > plainHeight, "a subtitle under each side makes the row taller");

	/*
	 * The leader hangs off the top line of the row, the way it does for a bridged logo row: a
	 * subtitle added under a name must not drag the dots down into the middle of the block.
	 */
	const QRectF plainBridge = boxOf(documentWith(plain), LayoutBox::Kind::Bridge);
	const QRectF stackedBridge = boxOf(documentWith(stacked), LayoutBox::Kind::Bridge);
	check(!plainBridge.isNull() && !stackedBridge.isNull(), "both rows drew a bridge");
	checkNear(stackedBridge.top(), plainBridge.top(), 1.0, "the leader stays on the top line");

	/*
	 * Four text boxes now: two lines a side, in the order left title, left subtitle, right
	 * title, right subtitle. The two titles are set at the same size, so anchoring each side on
	 * its own top line puts them on the same line as each other -- a side anchored on the wrong
	 * line of its stack drops by the difference between the two ascents.
	 */
	const QVector<QRectF> lines = boxesOf(documentWith(stacked), LayoutBox::Kind::Text);
	checkEq(lines.size(), 4, "two lines on each side");
	if (lines.size() == 4) {
		checkNear(lines.at(0).top(), lines.at(2).top(), 1.0, "both sides start on the same line");
		check(lines.at(1).top() > lines.at(0).top(), "the left subtitle sits under its own line");
		check(lines.at(3).top() > lines.at(2).top(), "the right subtitle sits under its own line");
	}
	checkEq(boxesOutsideContent(documentWith(stacked)), 0, "nothing placed outside the content box");
}

CT_SUITE(bridge_subtitles_optional, "A bridged row with the subtitles on but nothing in them")
{
	/*
	 * The whole point of the switch is that it costs nothing until a row uses it: an entry with
	 * empty subtitles has to measure, place and draw exactly as one from before the feature.
	 */
	Section plain = bridgedRow(QStringLiteral("Editor"), QStringLiteral("R Singh"));
	Section empty = plain;
	empty.rowSubtitles = true;

	checkEq(measure(documentWith(empty)), measure(documentWith(plain)), "an empty pair is one line tall");

	const auto [plainLeft, plainRight] = textColumns(documentWith(plain));
	const auto [emptyLeft, emptyRight] = textColumns(documentWith(empty));
	checkNear(emptyLeft.top(), plainLeft.top(), 0.5, "the left line has not moved");
	checkNear(emptyRight.left(), plainRight.left(), 0.5, "the right column has not moved");

	/* One side filled in, the other not: only the filled side grows. */
	Section half = empty;
	half.entries = {Entry{QStringLiteral("Editor"), QStringLiteral("R Singh"), QStringLiteral("assembly"), {}, {}}};
	checkEq(boxesOf(documentWith(half), LayoutBox::Kind::Text).size(), 3, "three lines, not four");
}

CT_SUITE(bridge_subtitles_columns, "A bridged column sized from the wider of its two lines")
{
	/*
	 * Under Natural sizing a column takes what its text needs -- which, once a side is a pair,
	 * is the wider of the two lines. A column measured from the title alone would wrap the
	 * longer subtitle underneath it inside a column that was never sized for it.
	 */
	Section section = bridgedRow(QStringLiteral("Ed"), QStringLiteral("R Singh"));
	section.bridgeType = BridgeType::None;
	section.bridgeSizing = BridgeSizing::Natural;
	section.bridgeMinGap = 20;
	section.rowSubtitles = true;

	const qreal narrow = textColumns(documentWith(section)).first.width();

	section.entries = {Entry{QStringLiteral("Ed"), QStringLiteral("R Singh"),
				 QStringLiteral("a considerably longer subtitle than the line above it"), {}, {}}};
	const QRectF wide = textColumns(documentWith(section)).first;

	check(wide.width() > narrow, "the column grew to hold the wider line");

	/* And the subtitle really fits on one line in it, rather than wrapping inside its own column. */
	checkEq(boxesOf(documentWith(section), LayoutBox::Kind::Text).size(), 3, "no line wrapped into a fourth box");
}
