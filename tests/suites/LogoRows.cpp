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

Section logoRow(SectionType type, LogoPlacement placement)
{
	Section section = unpadded(type);
	section.text = QStringLiteral("Presented By");
	section.secondaryText = sectionUsesSubtitles(type) ? QStringLiteral("with support from the Arts Council")
							   : QString();
	section.logoPlacement = placement;
	section.logoSide = LogoSide::Left;
	withLogo(section);
	return section;
}

/*
 * The distance actually drawn between the end of the leader and the first ink of the title.
 *
 * Measured off pixels rather than off the layout boxes, because the box is the *column* the title
 * was given and the title need not fill it -- which is the whole thing this is here to catch.
 * Returns -1 when either part is missing, so a build that stopped drawing one fails rather than
 * silently comparing nothing.
 */
int leaderToWordsGap(const Document &document)
{
	const QRectF bridge = boxOf(document, LayoutBox::Kind::Bridge);
	const QRectF title = boxOf(document, LayoutBox::Kind::Text);
	if (bridge.isNull() || title.isNull())
		return -1;

	const Ink ink = inkOf(renderImage(document), title);
	return ink.isEmpty() ? -1 : ink.left - qRound(bridge.right());
}

} // namespace

CT_SUITE(logo_row_placement, "How a logo is divided from the text beside it, in all three placements")
{
	for (SectionType type : {SectionType::HeaderWithLogo, SectionType::HeaderWithSubtitleAndLogo}) {
		const Context typeContext(QString::fromUtf8(sectionTypeId(type)));

		/* Hug is the placement whose whole point is that logoGap is the distance drawn. */
		{
			const Context context(QStringLiteral("hug"));
			Section section = logoRow(type, LogoPlacement::Hug);
			section.logoGap = 40;
			/* Pointed at the logo, so the nearest line is the one the gap is measured to. */
			section.style.align = HAlign::Left;
			section.secondaryStyle.align = HAlign::Left;

			const Document document = documentWith(section);
			const QRectF logo = boxOf(document, LayoutBox::Kind::Logo);
			const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);

			check(!logo.isNull(), "the logo is placed");
			check(!lines.isEmpty(), "the text is placed");
			if (logo.isNull() || lines.isEmpty())
				return;

			checkNear(lines.at(0).left() - logo.right(), section.logoGap, 1.0,
				  "a hugging logo really is logoGap from the text column");
			checkEq(boxesOutsideContent(document), 0, "a hugging row stays inside its content area");
		}

		/* Edge and Bridged both consume the whole box, by construction. */
		for (LogoPlacement placement : {LogoPlacement::Edge, LogoPlacement::Bridged}) {
			const Context context(QString::fromUtf8(logoPlacementId(placement)));
			const Document document = documentWith(logoRow(type, placement));

			const QRectF content = boxOf(document, LayoutBox::Kind::Content);
			const QRectF logo = boxOf(document, LayoutBox::Kind::Logo);
			const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);
			if (logo.isNull() || lines.isEmpty() || content.isNull())
				continue;

			checkNear(logo.left(), content.left(), 1.0, "the logo caps the near edge of the box");
			checkNear(lines.at(0).right(), content.right(), 1.0,
				  "the text reaches the far edge of the box");
			checkEq(boxesOutsideContent(document), 0, "the row stays inside its content area");
		}
	}
}

CT_SUITE(logo_row_pairs, "A logo beside a stacked pair, against the same row holding one line")
{
	Section paired = logoRow(SectionType::HeaderWithSubtitleAndLogo, LogoPlacement::Hug);
	const Document document = documentWith(paired);

	const QRectF logo = boxOf(document, LayoutBox::Kind::Logo);
	const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);
	checkEq(lines.size(), 2, "a paired logo row places both lines");
	check(!logo.isNull(), "a paired logo row places its logo");
	if (lines.size() != 2 || logo.isNull())
		return;

	/* The logo centers against the whole block, not against the title line. */
	const QRectF block(lines.at(0).left(), lines.at(0).top(), lines.at(0).width(),
			   lines.at(1).bottom() - lines.at(0).top());
	checkNear(logo.center().y(), block.center().y(), 1.0, "the logo is centered against the two-line block");

	/* Both lines share one column, sized by the wider of them. */
	checkNear(lines.at(0).width(), lines.at(1).width(), 0.5, "both lines are laid out into the same column");

	Section wider = paired;
	wider.secondaryText = QStringLiteral("a subtitle considerably longer than the title above it is");
	const QVector<QRectF> widerLines = boxesOf(documentWith(wider), LayoutBox::Kind::Text);
	if (widerLines.size() == 2)
		check(widerLines.at(0).width() > lines.at(0).width(),
		      "the column widens for a subtitle wider than the title");

	/* A short logo leaves the block deciding the row's height, and a tall one the logo. */
	Section shortLogo = paired;
	withLogo(shortLogo, 20);
	checkNear(measure(documentWith(shortLogo)), block.height(), 1.0,
		  "a short logo leaves the block deciding the row height");

	Section tallLogo = paired;
	withLogo(tallLogo, 400);
	checkEq(measure(documentWith(tallLogo)), qRound(boxOf(documentWith(tallLogo), LayoutBox::Kind::Logo).height()),
		"a tall logo decides the row height");

	/*
	 * A single-line type is the pair with an empty subtitle, so a stray subtitle left on the
	 * section by a type change must not move it by so much as a pixel.
	 */
	Section single = logoRow(SectionType::HeaderWithLogo, LogoPlacement::Hug);
	Section singleWithStray = single;
	singleWithStray.secondaryText = QStringLiteral("a type with nowhere to put this");

	checkEq(measure(documentWith(singleWithStray)), measure(documentWith(single)),
		"a single-line type ignores a subtitle left on the section");
	checkEq(boxesOf(documentWith(singleWithStray), LayoutBox::Kind::Text).size(), 1,
		"a single-line type draws one line whatever the field holds");
}

CT_SUITE(logo_row_bridged, "The leader of a bridged logo row reaching the words it joins")
{
	Section paired = logoRow(SectionType::HeaderWithSubtitleAndLogo, LogoPlacement::Bridged);
	Section single = logoRow(SectionType::HeaderWithLogo, LogoPlacement::Bridged);

	/*
	 * A single line's column *is* that line, so logoGap is the distance drawn whatever the
	 * alignment. A pair's column is sized by the wider line, so the narrower one is placed
	 * inside it by its own alignment -- pointed at the leader, the two close identically.
	 * See the logo-row notes in ARCHITECTURE.md; this pins both halves of that.
	 */
	const auto pointedAtTheLeader = [](Section section) {
		section.style.align = HAlign::Left;
		section.secondaryStyle.align = HAlign::Left;
		return documentWith(section);
	};

	const int pairedGap = leaderToWordsGap(pointedAtTheLeader(paired));
	const int singleGap = leaderToWordsGap(pointedAtTheLeader(single));

	check(pairedGap >= 0 && singleGap >= 0, "both rows draw a leader and a title to measure between");
	checkEq(pairedGap, singleGap, "a bridged pair reaches its leader exactly as a single line does");

	/* And the alignment really is the knob, rather than the gap being fixed either way. */
	const auto gapWithAlign = [&paired](HAlign align) {
		Section section = paired;
		section.style.align = align;
		section.secondaryStyle.align = align;
		return leaderToWordsGap(documentWith(section));
	};

	const int left = gapWithAlign(HAlign::Left);
	const int center = gapWithAlign(HAlign::Center);
	const int right = gapWithAlign(HAlign::Right);
	check(left < center && center < right, "the title's own alignment moves it within the pair's column");

	/* A single line has no wider neighbor to float against, so alignment cannot move it. */
	const auto singleGapWithAlign = [&single](HAlign align) {
		Section section = single;
		section.style.align = align;
		return leaderToWordsGap(documentWith(section));
	};
	checkEq(singleGapWithAlign(HAlign::Left), singleGapWithAlign(HAlign::Center),
		"a single line fills its own column whatever its alignment");

	/*
	 * The leader is hung off the *top line's own baseline*, which is the invariant that holds
	 * everywhere: its offset from the top of that line is a property of the font and the bridge
	 * and nothing else, so it is the same for one line and for two, at every logo height.
	 *
	 * Worth stating this way round rather than as "adding a subtitle leaves the leader put",
	 * which is only true in one of the two regimes below and was how this got written wrong the
	 * first time.
	 */
	const auto leaderBelowTopLine = [](const Document &document) {
		const QRectF bridge = boxOf(document, LayoutBox::Kind::Bridge);
		const QRectF top = boxOf(document, LayoutBox::Kind::Text);
		return bridge.isNull() || top.isNull() ? -1.0 : bridge.top() - top.top();
	};

	double reference = -1.0;
	for (int logoHeight : {40, 60, 110, 300}) {
		const Context context(QStringLiteral("logo=%1").arg(logoHeight));

		Section pairedRow = paired;
		Section singleRow = paired;
		singleRow.secondaryText.clear();
		withLogo(pairedRow, logoHeight);
		withLogo(singleRow, logoHeight);

		const double withSubtitle = leaderBelowTopLine(documentWith(pairedRow));
		const double without = leaderBelowTopLine(documentWith(singleRow));

		check(withSubtitle > 0.0, "the paired row draws a leader and a line to measure between");
		checkNear(withSubtitle, without, 0.5, "the leader hangs off the top line whether or not there are two");

		if (reference < 0.0)
			reference = withSubtitle;
		checkNear(withSubtitle, reference, 0.5, "and by the same offset at every logo height");
	}

	/*
	 * Which regime the row is in decides whether the *baseline itself* moves. With a logo
	 * shorter than the block, the block sets the row height and starts at its top, so a subtitle
	 * costs the leader nothing; with a taller logo the block is centered against it, so a taller
	 * block starts higher and takes the leader up with it by half the height it gained.
	 */
	const auto leaderTop = [](const Document &document) {
		return boxOf(document, LayoutBox::Kind::Bridge).top();
	};

	Section shortLogoPaired = paired;
	Section shortLogoSingle = paired;
	shortLogoSingle.secondaryText.clear();
	withLogo(shortLogoPaired, 40);
	withLogo(shortLogoSingle, 40);
	checkNear(leaderTop(documentWith(shortLogoPaired)), leaderTop(documentWith(shortLogoSingle)), 0.5,
		  "under a logo shorter than the block, a subtitle does not move the leader at all");

	Section tallLogoPaired = paired;
	Section tallLogoSingle = paired;
	tallLogoSingle.secondaryText.clear();
	withLogo(tallLogoPaired, 300);
	withLogo(tallLogoSingle, 300);

	const double gained = measure(documentWith(QVector<Section>{tallLogoPaired})) >= 0
				      ? boxOf(documentWith(tallLogoPaired), LayoutBox::Kind::Text, 1).bottom() -
						boxOf(documentWith(tallLogoPaired), LayoutBox::Kind::Text).bottom()
				      : 0.0;
	checkNear(leaderTop(documentWith(tallLogoSingle)) - leaderTop(documentWith(tallLogoPaired)), gained / 2.0, 1.0,
		  "under a taller logo the block is centered, so the leader rises by half what the block gained");

	/* Flipping the stack moves it to whichever line took the top. */
	Section flipped = paired;
	flipped.subtitleFirst = true;
	check(!qFuzzyCompare(boxOf(documentWith(flipped), LayoutBox::Kind::Bridge).top() + 1.0,
			     boxOf(documentWith(paired), LayoutBox::Kind::Bridge).top() + 1.0),
	      "flipping the stack moves the leader to the new top line");

	/* An empty heading still places a leader rather than failing the strip. */
	Section empty = paired;
	empty.text.clear();
	empty.secondaryText.clear();
	checkEq(boxesOf(documentWith(empty), LayoutBox::Kind::Bridge).size(), 1,
		"an empty bridged heading still draws its leader");
	checkEq(boxesOf(documentWith(empty), LayoutBox::Kind::Text).size(), 0,
		"an empty bridged heading draws no text");
}

CT_SUITE(logo_row_containment, "The whole logo-row configuration matrix staying inside its box")
{
	/*
	 * Text landing in the canvas's last pixel column is deliberately *not* what this looks for:
	 * right-aligned text in a box whose edge is the canvas edge inks that column for every
	 * section type in the plugin, a plain Header included, so treating it as a fault would pin
	 * a fiction. What has to hold is that nothing the layout places leaves its content area.
	 */
	const QVector<Axis> axes = {
		axis<SectionType>(QStringLiteral("type"),
				  {{QStringLiteral("paired"), SectionType::HeaderWithSubtitleAndLogo},
				   {QStringLiteral("single"), SectionType::HeaderWithLogo},
				   {QStringLiteral("titled"), SectionType::TitleWithSubtitleAndLogo}},
				  [](Section &s, SectionType v) { s.type = v; }),
		axis<LogoPlacement>(QStringLiteral("place"),
				    {{QStringLiteral("edge"), LogoPlacement::Edge},
				     {QStringLiteral("hug"), LogoPlacement::Hug},
				     {QStringLiteral("bridged"), LogoPlacement::Bridged}},
				    [](Section &s, LogoPlacement v) { s.logoPlacement = v; }),
		axis<LogoSide>(QStringLiteral("side"),
			       {{QStringLiteral("left"), LogoSide::Left}, {QStringLiteral("right"), LogoSide::Right}},
			       [](Section &s, LogoSide v) { s.logoSide = v; }),
		axis<HAlign>(QStringLiteral("align"),
			     {{QStringLiteral("left"), HAlign::Left},
			      {QStringLiteral("center"), HAlign::Center},
			      {QStringLiteral("right"), HAlign::Right}},
			     [](Section &s, HAlign v) {
				     s.style.align = v;
				     s.secondaryStyle.align = v;
			     }),
		axis<int>(QStringLiteral("gap"),
			  {{QStringLiteral("0"), 0}, {QStringLiteral("24"), 24}, {QStringLiteral("200"), 200}},
			  [](Section &s, int v) { s.logoGap = v; }),
		axis<double>(QStringLiteral("width"), {{QStringLiteral("full"), 1.0}, {QStringLiteral("half"), 0.5}},
			     [](Section &s, double v) { s.sectionWidth = v; }),
		axis<int>(QStringLiteral("margin"), {{QStringLiteral("0"), 0}, {QStringLiteral("120"), 120}},
			  [](Section &s, int v) { s.marginX = v; }),
		axis<QString>(QStringLiteral("title"),
			      {{QStringLiteral("short"), QStringLiteral("X")},
			       {QStringLiteral("normal"), QStringLiteral("Presented By")},
			       {QStringLiteral("long"), QStringLiteral("A Very Long Heading Indeed That Runs On")}},
			      [](Section &s, const QString &v) { s.text = v; }),
		axis<QString>(QStringLiteral("subtitle"),
			      {{QStringLiteral("short"), QStringLiteral("MMXXVI")},
			       {QStringLiteral("normal"), QStringLiteral("with support from the Arts Council")},
			       {QStringLiteral("long"),
				QStringLiteral("with the generous support of the Arts Council of Great Britain and "
					       "several other bodies besides")}},
			      [](Section &s, const QString &v) { s.secondaryText = v; }),
	};

	checkEq(sweepSize(axes), 5832, "the matrix is the size it is meant to be");

	Section base = unpadded(SectionType::HeaderWithSubtitleAndLogo);
	withLogo(base);

	int escaped = 0;
	sweep(base, axes,
	      [&escaped](const Section &section) { escaped += boxesOutsideContent(documentWith(section)); });

	checkEq(escaped, 0, "nothing a logo row places leaves its content area");
}
