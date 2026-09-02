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

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

namespace closingtime::test {

namespace {

constexpr int kCanvasWidth = 1280;
constexpr int kCanvasHeight = 720;

QTemporaryDir &assetDir()
{
	static QTemporaryDir dir;
	return dir;
}

} // namespace

Document documentWith(const Section &section)
{
	return documentWith(QVector<Section>{section});
}

Document documentWith(const QVector<Section> &sections)
{
	Document document;
	document.width = kCanvasWidth;
	document.height = kCanvasHeight;
	document.leadIn = 0;
	document.leadOut = 0;
	document.sections = sections;
	return document;
}

Section unpadded(SectionType type)
{
	Section section = Section::makeDefault(type);
	section.paddingTop = 0;
	section.paddingBottom = 0;
	return section;
}

Section &withLogo(Section &section, int maxHeight)
{
	section.logo.path = testLogoPath();
	section.logo.maxHeight = maxHeight;

	for (Entry &entry : section.entries) {
		entry.logo.path = testLogoPath();
		entry.logo.maxHeight = maxHeight;
	}

	return section;
}

Document everySectionType()
{
	QVector<Section> sections;
	for (SectionType type : allSectionTypes()) {
		Section section = Section::makeDefault(type);
		if (sectionUsesLogos(type))
			withLogo(section);
		sections.append(section);
	}
	return documentWith(sections);
}

QString testLogoPath()
{
	static const QString path = [] {
		if (!assetDir().isValid())
			return QString();

		const QString file = assetDir().filePath(QStringLiteral("logo.png"));

		/* A ring: square, ink out to every edge, and a hole in the middle so a tint that
		 * fills the silhouette instead of painting the artwork is visible at a glance. */
		QImage image(220, 220, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			QPainter painter(&image);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(255, 210, 90));
			painter.drawEllipse(image.rect());
			painter.setCompositionMode(QPainter::CompositionMode_Clear);
			painter.drawEllipse(image.rect().adjusted(70, 70, -70, -70));
		}

		return image.save(file) ? file : QString();
	}();

	return path;
}

namespace {

/*
 * Writes a minimal two-frame GIF89a.
 *
 * Hand-rolled because Qt reads GIFs and does not write them, and because the alternative -- a
 * binary committed to the tree -- is one more file to keep in step with the checks that read it.
 * Only the properties the checks depend on are here: two frames, a known delay, and two colors
 * different enough that a decoder handing back the wrong frame is obvious.
 *
 * The LZW stream is deliberately the dullest one that is still valid: a clear code before every
 * literal, so the code table never grows past its initial size and every code stays three bits.
 * That is worth a few bytes of file to avoid hand-writing a code-width transition in a fixture.
 */
class GifWriter {
public:
	explicit GifWriter(int size) : size(size) {}

	QByteArray build() const
	{
		QByteArray out;

		out.append("GIF89a", 6);
		appendShort(out, size);
		appendShort(out, size);
		/* Global color table, 8-bit color resolution, two entries. */
		out.append(char(0xF0));
		out.append(char(0));
		out.append(char(0));

		appendColor(out, 255, 210, 90);
		appendColor(out, 40, 60, 200);

		for (int frame = 0; frame < kTestAnimationFrames; ++frame)
			appendFrame(out, frame);

		out.append(char(0x3B));
		return out;
	}

private:
	static void appendShort(QByteArray &out, int value)
	{
		out.append(char(value & 0xFF));
		out.append(char((value >> 8) & 0xFF));
	}

	static void appendColor(QByteArray &out, int r, int g, int b)
	{
		out.append(char(r));
		out.append(char(g));
		out.append(char(b));
	}

	void appendFrame(QByteArray &out, int index) const
	{
		/* Graphic control extension, carrying the frame delay in hundredths of a second. */
		out.append(char(0x21));
		out.append(char(0xF9));
		out.append(char(0x04));
		out.append(char(0x00));
		appendShort(out, kTestAnimationFrameMs / 10);
		out.append(char(0x00));
		out.append(char(0x00));

		/* Image descriptor: the whole canvas, no local color table. */
		out.append(char(0x2C));
		appendShort(out, 0);
		appendShort(out, 0);
		appendShort(out, size);
		appendShort(out, size);
		out.append(char(0x00));

		appendPixels(out, index % 2);
	}

	void appendPixels(QByteArray &out, int colorIndex) const
	{
		constexpr int kMinCodeSize = 2;
		constexpr int kCodeBits = 3;
		constexpr int kClear = 4;
		constexpr int kEnd = 5;

		out.append(char(kMinCodeSize));

		QByteArray bits;
		int accumulator = 0;
		int bitCount = 0;

		/* Not called `emit`: Qt defines that as a macro, and it expands to nothing here. */
		const auto emitCode = [&](int code) {
			accumulator |= code << bitCount;
			bitCount += kCodeBits;
			while (bitCount >= 8) {
				bits.append(char(accumulator & 0xFF));
				accumulator >>= 8;
				bitCount -= 8;
			}
		};

		for (int pixel = 0; pixel < size * size; ++pixel) {
			emitCode(kClear);
			emitCode(colorIndex);
		}
		emitCode(kEnd);

		if (bitCount > 0)
			bits.append(char(accumulator & 0xFF));

		/* Sub-blocks, each at most 255 bytes, terminated by an empty one. */
		for (int offset = 0; offset < bits.size(); offset += 255) {
			const QByteArray chunk = bits.mid(offset, 255);
			out.append(char(chunk.size()));
			out.append(chunk);
		}

		out.append(char(0x00));
	}

	int size;
};

} // namespace

QString testAnimatedLogoPath()
{
	static const QString path = [] {
		if (!assetDir().isValid())
			return QString();

		const QString file = assetDir().filePath(QStringLiteral("logo.gif"));

		QFile out(file);
		if (!out.open(QIODevice::WriteOnly))
			return QString();

		out.write(GifWriter(16).build());
		out.close();

		return file;
	}();

	return path;
}

Section &withAnimatedLogo(Section &section, int maxHeight)
{
	section.logo.path = testAnimatedLogoPath();
	section.logo.maxHeight = maxHeight;

	for (Entry &entry : section.entries) {
		entry.logo.path = testAnimatedLogoPath();
		entry.logo.maxHeight = maxHeight;
	}

	return section;
}

QString missingLogoPath()
{
	return QStringLiteral("/closing-time/no/such/file.png");
}

const QVector<Scene> &scenes()
{
	static const QVector<Scene> built = [] {
		QVector<Scene> list;

		const auto add = [&list](const QString &name, const QString &description, const Document &document) {
			list.append(Scene{name, description, document});
		};

		/* --- headings ---------------------------------------------------------------- */

		{
			Section title = Section::makeDefault(SectionType::TitleWithSubtitle);
			title.text = QStringLiteral("Closing Time");
			title.secondaryText = QStringLiteral("A Voidscape Production");
			title.marginX = 80;

			Section header = Section::makeDefault(SectionType::HeaderWithSubtitle);
			header.text = QStringLiteral("Principal Cast");
			header.secondaryText = QStringLiteral("in order of appearance");
			header.marginX = 80;

			Section flipped = Section::makeDefault(SectionType::TitleWithSubtitle);
			flipped.text = QStringLiteral("Jane Doe");
			flipped.secondaryText = QStringLiteral("Directed by");
			flipped.subtitleFirst = true;
			flipped.marginX = 80;

			add(QStringLiteral("headings-with-subtitles"),
			    QStringLiteral("A title, a header and a flipped pair, all without logos"),
			    documentWith({title, header, flipped}));
		}

		/* --- logo rows --------------------------------------------------------------- */

		{
			QVector<Section> rows;
			for (LogoPlacement placement :
			     {LogoPlacement::Hug, LogoPlacement::Edge, LogoPlacement::Bridged}) {
				Section section = Section::makeDefault(SectionType::HeaderWithSubtitleAndLogo);
				section.text = QStringLiteral("Presented By");
				section.secondaryText = QStringLiteral("with support from the Arts Council");
				section.logoPlacement = placement;
				section.marginX = 80;
				/*
				 * Pointed at the logo, which is what closes the distance to `logoGap`
				 * for the narrower of the two lines -- see the logo-row notes in
				 * ARCHITECTURE.md.
				 */
				section.style.align = HAlign::Left;
				section.secondaryStyle.align = HAlign::Left;
				withLogo(section);
				rows.append(section);
			}

			add(QStringLiteral("logo-rows-paired"),
			    QStringLiteral("A subtitle heading with a logo, in all three placements"),
			    documentWith(rows));
		}

		/* --- lists ------------------------------------------------------------------- */

		{
			Section bridged = Section::makeDefault(SectionType::Bridged);
			bridged.entries = {Entry{QStringLiteral("Director"), QStringLiteral("Jane Doe"), {}, {}, {}},
					   Entry{QStringLiteral("Director of Photography"),
						 QStringLiteral("A Rather Longer Name"),
						 {},
						 {},
						 {}},
					   Entry{QStringLiteral("Editor"), QStringLiteral("R Singh"), {}, {}, {}}};

			Section pairs = Section::makeDefault(SectionType::TitleSubtitleList);
			pairs.entries = {
				Entry{QStringLiteral("Producer"), QStringLiteral("Chris Nakamura"), {}, {}, {}},
				Entry{QStringLiteral("Line Producer"), QStringLiteral("Sam Oyelaran"), {}, {}, {}}};

			add(QStringLiteral("lists"), QStringLiteral("A bridged list over a title/subtitle list"),
			    documentWith({bridged, pairs}));
		}

		/* --- offsets and tabs ---------------------------------------------------------- */

		{
			/*
			 * The two ways a section is moved sideways without touching its margins: the
			 * whole of it nudged off its own center, and single entries tabbed in from the
			 * rest of the list they belong to.
			 */
			Section heading = Section::makeDefault(SectionType::Header);
			heading.text = QStringLiteral("Nudged Left");
			heading.contentOffsetX = -160;

			Section tabbed = Section::makeDefault(SectionType::TextList);
			tabbed.style.align = HAlign::Left;
			tabbed.marginX = 320;
			tabbed.indentStep = 40;
			tabbed.entries = {Entry{QStringLiteral("Art Department")},
					  Entry{QStringLiteral("Ada Lovelace")}, Entry{QStringLiteral("Grace Hopper")},
					  Entry{QStringLiteral("Assistants")},
					  Entry{QStringLiteral("Karen Sparck Jones")}};
			tabbed.entries[1].indent = 1;
			tabbed.entries[2].indent = 1;
			tabbed.entries[3].indent = 1;
			tabbed.entries[4].indent = 2;

			Section shifted = Section::makeDefault(SectionType::Header);
			shifted.text = QStringLiteral("Nudged Right");
			shifted.contentOffsetX = 160;

			add(QStringLiteral("offsets-and-tabs"),
			    QStringLiteral("A section nudged either way, and a list tabbed into a hierarchy"),
			    documentWith({heading, tabbed, shifted}));
		}

		/* --- backgrounds --------------------------------------------------------------- */

		{
			/*
			 * What the picture is for: a card behind a heading, a band that reaches out past
			 * the section's own box, and a list striped by its alternate entry panel -- the
			 * three shapes a panel is actually reached for. All three keep the layout they
			 * would have had with no panel at all.
			 */
			const auto card = [](const QColor &color, double radius) {
				BackgroundPanel panel;
				panel.fill = BackgroundFill::Color;
				panel.color = color;
				panel.setRadius(radius);
				return panel;
			};

			Section titled = Section::makeDefault(SectionType::TitleWithSubtitle);
			titled.text = QStringLiteral("Closing Time");
			titled.secondaryText = QStringLiteral("A Voidscape Production");
			titled.sectionWidth = 0.7;
			BackgroundPanel &titlePanel = titled.backgroundEntry(BackgroundSlot::Section).panel;
			titlePanel = card(QColor(24, 28, 44, 235), 28.0);
			titlePanel.border.enabled = true;
			titlePanel.border.width = 2.0;
			titlePanel.border.color = QColor(255, 210, 90, 220);

			Section band = Section::makeDefault(SectionType::Header);
			band.text = QStringLiteral("Principal Cast");
			BackgroundPanel &bandPanel = band.backgroundEntry(BackgroundSlot::Section).panel;
			bandPanel = card(QColor(255, 210, 90, 60), 0.0);
			/* Out past the section's box to the edges of the frame, without moving anything. */
			bandPanel.outsetLeft = 400.0;
			bandPanel.outsetRight = 400.0;

			Section striped = Section::makeDefault(SectionType::TextList);
			striped.entries = {Entry{QStringLiteral("Ada Lovelace")}, Entry{QStringLiteral("Grace Hopper")},
					   Entry{QStringLiteral("Karen Sparck Jones")},
					   Entry{QStringLiteral("Barbara Liskov")}};
			striped.sectionWidth = 0.6;
			striped.entryGap = 0;
			striped.backgroundEntry(BackgroundSlot::Entry).panel = card(QColor(255, 255, 255, 26), 6.0);
			/* Present but drawing nothing, which is what leaves every other row bare. */
			striped.backgroundEntry(BackgroundSlot::EntryAlt);

			add(QStringLiteral("backgrounds"),
			    QStringLiteral("A card behind a heading, a band reaching past its box, and a striped list"),
			    documentWith({titled, band, striped}));
		}

		/* --- dividers ---------------------------------------------------------------- */

		{
			Section plain = Section::makeDefault(SectionType::SectionDivider);

			Section stacked = Section::makeDefault(SectionType::SectionDivider);
			stacked.dividerRules = 3;
			stacked.dividerRuleInset = 40.0;

			Section labeled = Section::makeDefault(SectionType::SectionDivider);
			labeled.dividerCenter = {DividerPiece{DividerPiece::Kind::Text,
							      DividerShape::Diamond,
							      {},
							      1.0,
							      QStringLiteral("PART II"),
							      {}}};

			add(QStringLiteral("dividers"),
			    QStringLiteral("A plain rule, a tapered stack and a labeled break"),
			    documentWith({plain, stacked, labeled}));
		}

		/* --- plain shapes and turned pieces -------------------------------------------- */

		{
			/*
			 * The plain shapes, filled over outlined, and then the same square turned
			 * through a run of angles: what the picture is for is that the geometry is
			 * drawn as geometry -- an equilateral triangle, a regular pentagon -- and that
			 * a turned piece leans without dragging the rule it sits on with it.
			 */
			const auto ornament = [](DividerShape shape, double rotation = 0.0) {
				return DividerPiece{DividerPiece::Kind::Ornament, shape, {}, 1.0, {}, {}, rotation};
			};

			const auto row = [&](const QVector<DividerPiece> &center) {
				Section section = Section::makeDefault(SectionType::SectionDivider);
				section.dividerPieceGap = 14.0;
				section.dividerCenter = center;
				return section;
			};

			Section filled = row({ornament(DividerShape::Circle), ornament(DividerShape::Square),
					      ornament(DividerShape::RoundedSquare), ornament(DividerShape::Triangle),
					      ornament(DividerShape::Pentagon), ornament(DividerShape::Hexagon),
					      ornament(DividerShape::Octagon)});

			Section outlined =
				row({ornament(DividerShape::Ring), ornament(DividerShape::SquareOutline),
				     ornament(DividerShape::RoundedSquareOutline),
				     ornament(DividerShape::TriangleOutline), ornament(DividerShape::PentagonOutline),
				     ornament(DividerShape::HexagonOutline), ornament(DividerShape::OctagonOutline)});

			Section turned =
				row({ornament(DividerShape::Square, 0.0), ornament(DividerShape::Square, 15.0),
				     ornament(DividerShape::Square, 30.0), ornament(DividerShape::Square, 45.0),
				     ornament(DividerShape::Triangle, 90.0), ornament(DividerShape::Triangle, 180.0)});

			add(QStringLiteral("dividers-shapes"),
			    QStringLiteral("The plain shapes filled and outlined, and a run of turned pieces"),
			    documentWith({filled, outlined, turned}));
		}

		/* --- joined dividers ---------------------------------------------------------- */

		{
			/*
			 * The same composed divider three ways, because the whole of what joining
			 * does is visible only against the divider it was: held apart, run through,
			 * and pushed together by hand. One ornament and one cap, so what the picture
			 * shows is the joins rather than the shapes either side of them.
			 */
			const auto composed = [] {
				Section section = Section::makeDefault(SectionType::SectionDivider);
				section.dividerCap = {DividerPiece{DividerPiece::Kind::Ornament,
								   DividerShape::Arrow,
								   {},
								   1.0,
								   {},
								   {}}};
				section.dividerCenter = {DividerPiece{DividerPiece::Kind::Ornament,
								      DividerShape::Diamond,
								      {},
								      1.0,
								      {},
								      {}}};
				section.dividerGap = 24.0;
				return section;
			};

			Section apart = composed();

			Section joined = composed();
			joined.dividerConnect = true;

			Section overlapped = composed();
			overlapped.dividerGap = -12.0;

			add(QStringLiteral("dividers-joined"),
			    QStringLiteral("One composed divider held apart, run through, and overlapped"),
			    documentWith({apart, joined, overlapped}));
		}

		/* --- everything -------------------------------------------------------------- */

		add(QStringLiteral("every-section-type"),
		    QStringLiteral("One section of every type at its defaults, in menu order"), everySectionType());

		return list;
	}();

	return built;
}

} // namespace closingtime::test
