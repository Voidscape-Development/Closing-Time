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

#include <QFontMetricsF>

#include <obs.hpp>

#include "model/CreditsModel.hpp"
#include "render/QrArt.hpp"

using namespace closingtime;
using namespace closingtime::test;

namespace {

/* A section saved and loaded back, through the same obs_data a scene collection is. */
Section roundTrip(const Section &section)
{
	OBSDataAutoRelease data = obs_data_create();
	section.save(data);
	Section loaded;
	loaded.load(data);
	return loaded;
}

/* A seven-by-seven finder pattern with its top-left square at (x, y). */
bool finderAt(const QrMatrix &matrix, int x, int y)
{
	for (int dy = 0; dy < 7; ++dy) {
		for (int dx = 0; dx < 7; ++dx) {
			const bool ring = dx == 0 || dy == 0 || dx == 6 || dy == 6;
			const bool core = dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4;
			if (matrix.dark(x + dx, y + dy) != (ring || core))
				return false;
		}
	}
	return true;
}

/* The share of a box's pixels that carry ink at all. */
double inkedShare(const QImage &image, const QRect &box)
{
	int inked = 0;
	int total = 0;
	for (int y = box.top(); y <= box.bottom(); ++y) {
		for (int x = box.left(); x <= box.right(); ++x) {
			++total;
			if (qAlpha(image.pixel(x, y)) > 0)
				++inked;
		}
	}
	return total > 0 ? double(inked) / total : 0.0;
}

} // namespace

CT_SUITE(new_types_table, "The stacked, card and icon lists, the paragraph and the QR code in the type table")
{
	for (SectionType type : {SectionType::StackedList, SectionType::MultiStackedList, SectionType::CardList,
				 SectionType::MultiCardList, SectionType::IconList, SectionType::MultiIconList}) {
		const Context context(QString::fromUtf8(sectionTypeId(type)));
		const SectionTypeSwitches switches = decomposeSectionType(type);
		check(switches.base == SectionType::TextList, "a new list is picked as a kind of list");
		checkEq(switches.multiColumn ? 1 : 0, sectionUsesColumns(type) ? 1 : 0,
			"its column switch is whether it spreads over columns");
		check(!backgroundSlotsFor(type).contains(BackgroundSlot::Bridge),
		      "a list of pictures and words draws no leader, so it offers no leader panel");
	}

	check(decomposeSectionType(SectionType::Paragraph).base == SectionType::Paragraph,
	      "a paragraph is a base type of its own");
	check(decomposeSectionType(SectionType::QrCode).base == SectionType::QrCode, "and so is a QR code");
	check(backgroundSlotsFor(SectionType::QrCode).contains(BackgroundSlot::Logo),
	      "a QR code can have a card behind it, in the picture slot");

	check(qrErrorCorrectionFromId(qrErrorCorrectionId(QrErrorCorrection::High)) == QrErrorCorrection::High,
	      "an error-correction level round trips through its id");
	check(qrErrorCorrectionFromId("nonsense", QrErrorCorrection::Low) == QrErrorCorrection::Low,
	      "an unknown level takes the fallback");
}

CT_SUITE(new_types_persistence, "Every new field survives a save and a load, and is defaulted when absent")
{
	Section section = Section::makeDefault(SectionType::QrCode);
	section.qrText = QStringLiteral("https://example.com/ünïcode");
	section.qrSize = 300;
	section.qrQuietZone = 0;
	section.qrErrorCorrection = QrErrorCorrection::Quartile;
	section.qrForeground = QColor(10, 20, 30, 200);
	section.qrBackground = QColor(0, 0, 0, 0);
	section.paragraphGap = 0;
	section.justifyText = true;
	section.iconScale = 2.25;

	const Section loaded = roundTrip(section);
	checkEq(loaded.qrText, section.qrText, "the encoded text");
	checkEq(loaded.qrSize, 300, "the code's size");
	checkEq(loaded.qrQuietZone, 0, "a quiet zone of zero stays zero rather than becoming the default");
	check(loaded.qrErrorCorrection == QrErrorCorrection::Quartile, "the error-correction level");
	check(loaded.qrForeground == section.qrForeground, "the squares' color, alpha and all");
	check(loaded.qrBackground == section.qrBackground, "a transparent plate stays transparent");
	checkEq(loaded.paragraphGap, 0, "a paragraph gap of zero stays zero");
	check(loaded.justifyText, "justification");
	checkNear(loaded.iconScale, 2.25, 1e-9, "the icon scale");
	check(loaded == section, "and nothing else moved");

	/* A section written before any of it existed. */
	OBSDataAutoRelease bare = obs_data_create();
	obs_data_set_string(bare, "type", "title");
	Section old;
	old.load(bare);
	checkEq(old.paragraphGap, 16, "an absent paragraph gap is the default");
	checkEq(old.qrSize, 240, "an absent code size is the default");
	checkEq(old.qrQuietZone, 4, "an absent quiet zone is the standard's four");
	check(old.qrBackground == QColor(255, 255, 255), "an absent plate is white");
	checkNear(old.iconScale, 1.4, 1e-9, "an absent icon scale is the default");
}

CT_SUITE(qr_encoding, "A QR code is a real code: finder patterns, sizes, capacity")
{
	const QrMatrix code = encodeQr(QStringLiteral("https://example.com"), QrErrorCorrection::Medium);
	check(!code.isEmpty(), "a link encodes");
	check(code.size >= 21 && (code.size - 21) % 4 == 0, "the side is one of the sizes the standard allows");
	check(finderAt(code, 0, 0), "a finder pattern in the top-left corner");
	check(finderAt(code, code.size - 7, 0), "and the top-right");
	check(finderAt(code, 0, code.size - 7), "and the bottom-left");

	check(encodeQr(QString(), QrErrorCorrection::Medium).isEmpty(), "nothing encodes to nothing");

	const QrMatrix low = encodeQr(QString(200, QLatin1Char('a')), QrErrorCorrection::Low);
	const QrMatrix high = encodeQr(QString(200, QLatin1Char('a')), QrErrorCorrection::High);
	check(high.size > low.size, "more error correction needs a bigger code for the same text");

	/* Past the largest code's capacity, the text is cut rather than the code failing. */
	const QString huge(5000, QChar(0x00E9));
	const QByteArray payload = qrPayload(huge, QrErrorCorrection::High);
	check(payload.size() <= qrCapacityBytes(QrErrorCorrection::High), "the payload fits the largest code");
	check(payload.size() % 2 == 0, "and is cut between characters, never inside one");
	checkEq(QString::fromUtf8(payload).size(), payload.size() / 2, "every character it keeps is whole");
	checkEq(encodeQr(huge, QrErrorCorrection::High).size, 177, "an over-long text still draws a version 40 code");

	/* Whole pixels per square, and the plate never larger than asked. */
	for (int maxSize : {100, 240, 333}) {
		for (int quiet : {0, 4}) {
			const Context context(QStringLiteral("%1 px, quiet %2").arg(maxSize).arg(quiet));
			const int side = qrPlateSize(code, maxSize, quiet);
			const int span = code.size + 2 * quiet;
			check(side <= maxSize, "the plate is never larger than asked");
			checkEq(side % span, 0, "the plate is a whole number of squares");
			check(side + span > maxSize, "and is the largest that fits");
		}
	}
}

CT_SUITE(qr_section, "A QR Code section draws its code on its plate, and its caption under it")
{
	Section section = unpadded(SectionType::QrCode);
	section.text.clear();
	section.qrSize = 250;
	section.qrQuietZone = 4;

	const Document bare = documentWith(section);
	const QRectF plate = boxOf(bare, LayoutBox::Kind::Logo);
	const QrMatrix matrix = encodeQr(section.qrText, section.qrErrorCorrection);
	checkEq(qRound(plate.width()), qrPlateSize(matrix, 250, 4), "the plate is the size the code comes to");
	checkEq(qRound(plate.height()), qRound(plate.width()), "and square");
	checkEq(measure(bare), qRound(plate.height()), "with no caption, the section is the plate");
	checkNear(plate.center().x(), bare.width / 2.0, 1.0, "centered by the style's alignment");

	const QImage image = renderImage(bare);
	const int module = qRound(plate.width()) / (matrix.size + 8);
	/* The top-left finder's outer ring starts four squares in. */
	const QPoint finder = plate.topLeft().toPoint() + QPoint(4 * module + module / 2, 4 * module + module / 2);
	const QRgb dark = image.pixel(finder);
	const QRgb quiet = image.pixel(plate.topLeft().toPoint() + QPoint(module / 2, module / 2));
	check(qGray(dark) < 40 && qAlpha(dark) == 255, "a finder square is drawn dark");
	check(qGray(quiet) > 215 && qAlpha(quiet) == 255, "the quiet zone is the light plate");

	/* A transparent plate draws only the squares. */
	Section clear = section;
	clear.qrBackground = QColor(0, 0, 0, 0);
	const QImage squares = renderImage(documentWith(clear));
	check(qAlpha(squares.pixel(plate.topLeft().toPoint() + QPoint(module / 2, module / 2))) == 0,
	      "a transparent plate leaves the quiet zone empty");
	const double share = inkedShare(squares, plate.toRect());
	check(share > 0.2 && share < 0.8, "and the squares are some of the code, not all or none of it");

	/* The caption goes `logoGap` under the plate. */
	Section captioned = section;
	captioned.text = QStringLiteral("Scan to donate");
	captioned.logoGap = 30;
	const Document withCaption = documentWith(captioned);
	const QRectF caption = boxOf(withCaption, LayoutBox::Kind::Text);
	checkEq(qRound(caption.top()), qRound(plate.bottom()) + 30, "the caption sits logoGap under the plate");
	checkEq(measure(withCaption), qRound(caption.bottom()), "and the section ends with it");

	/* A code too wide for its column is drawn smaller, not cut off. */
	Section narrow = section;
	narrow.sectionWidth = 0.1;
	narrow.qrSize = 2000;
	const QRectF small = boxOf(documentWith(narrow), LayoutBox::Kind::Logo);
	check(small.width() <= 192.0, "the plate fits the column it is in");

	/* Nothing to encode yet still holds its place. */
	Section empty = section;
	empty.qrText.clear();
	checkEq(measure(documentWith(empty)), 250, "an empty code is a placeholder of the size asked for");
}

CT_SUITE(paragraph_section, "Paragraphs break at blank lines, are held apart by the gap, and can be justified")
{
	Section one = unpadded(SectionType::Paragraph);
	one.text = QStringLiteral("First paragraph.");
	one.paragraphGap = 0;
	const int single = measure(documentWith(one));
	check(single > 0, "one paragraph has height");

	Section two = one;
	two.text = QStringLiteral("First paragraph.\n\n\n  \nFirst paragraph.");
	two.paragraphGap = 40;
	checkEq(measure(documentWith(two)), single * 2 + 40,
		"two paragraphs are two runs and one gap, however many blank lines are between them");
	checkEq(boxesOf(documentWith(two), LayoutBox::Kind::Text).size(), 2, "each paragraph is its own box");

	Section broken = one;
	broken.text = QStringLiteral("First paragraph.\nFirst paragraph.");
	broken.paragraphGap = 40;
	checkEq(boxesOf(documentWith(broken), LayoutBox::Kind::Text).size(), 1,
		"a single line break stays inside its paragraph");
	/* Within a pixel: two lines are rounded up once, one line on its own is rounded up too. */
	checkNear(measure(documentWith(broken)), single * 2, 1.0, "and takes no paragraph gap");

	/* Justified, a wrapped line reaches the right edge; ragged, it does not. */
	Section prose = unpadded(SectionType::Paragraph);
	prose.style.align = HAlign::Left;
	prose.style.lineSpacing = 1.0;
	prose.sectionWidth = 0.5;
	prose.text = QStringLiteral("The events, characters and firms depicted in this program are fictitious. "
				    "Any similarity to actual persons, living or dead, or to actual firms is purely "
				    "coincidental and unintended.");

	const auto firstLineRight = [](const Section &section) {
		const Document document = documentWith(section);
		const QRectF box = boxOf(document, LayoutBox::Kind::Text);
		QFont font(section.style.family);
		font.setPixelSize(section.style.pixelSize);
		const int lineHeight = qFloor(QFontMetricsF(font).lineSpacing());
		const QRect firstLine(qRound(box.left()), qRound(box.top()), qRound(box.width()), lineHeight - 2);
		const Ink ink = inkOf(renderImage(document), firstLine);
		return std::pair<int, int>(ink.right, qRound(box.right()));
	};

	Section justified = prose;
	justified.justifyText = true;
	const auto [raggedRight, edge] = firstLineRight(prose);
	const auto [justifiedRight, sameEdge] = firstLineRight(justified);
	check(edge - justifiedRight <= 3, "a justified line reaches the right edge of the column");
	check(edge - raggedRight > 3, "a ragged one stops short of it");
	checkEq(sameEdge, edge, "justifying moves no box");
	checkEq(measure(documentWith(justified)), measure(documentWith(prose)),
		"and changes no height: the same lines, spread wider");
	checkEq(boxesOutsideContent(documentWith(justified)), 0, "every box stays inside the section");
}

CT_SUITE(stacked_list, "A stack is up to four lines, each in its own style, `subtitleGap` apart")
{
	Section section = unpadded(SectionType::StackedList);
	section.subtitleGap = 10;
	const Entry &cue = section.entries.first();
	check(!cue.subtitle.isEmpty() && !cue.secondarySubtitle.isEmpty(), "a new stack starts with all four lines");

	const Document document = documentWith(section);
	const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);
	checkEq(lines.size(), 4, "four lines, four boxes");
	for (int i = 1; i < lines.size(); ++i)
		checkEq(qRound(lines.at(i).top() - lines.at(i - 1).bottom()), 10,
			QStringLiteral("line %1 sits subtitleGap under the one above").arg(i + 1));
	check(lines.at(3).height() < lines.at(0).height(), "the courtesy line is set smaller than the title");

	/* A blank line takes no room and no gap. */
	Section gapped = section;
	gapped.entries.first().subtitle.clear();
	const QVector<QRectF> three = boxesOf(documentWith(gapped), LayoutBox::Kind::Text);
	checkEq(three.size(), 3, "a blank third line is skipped");
	checkEq(measure(documentWith(gapped)), measure(document) - qRound(lines.at(2).height()) - section.subtitleGap,
		"taking exactly its own height and one gap with it");

	/* The fourth line is drawn in the fourth style, which can be bound to a preset. */
	Section styled = section;
	styled.rowSecondarySubtitleStyle.pixelSize = 60;
	check(boxesOf(documentWith(styled), LayoutBox::Kind::Text).at(3).height() > lines.at(3).height(),
	      "the fourth line follows the fourth style");

	/* Over columns, entries share rows. */
	Section multi = unpadded(SectionType::MultiStackedList);
	const QVector<QRectF> grid = boxesOf(documentWith(multi), LayoutBox::Kind::Text);
	checkEq(grid.size(), 8, "two cues, four lines each");
	checkNear(grid.at(0).top(), grid.at(4).top(), 0.5, "the two cues start on the same row");
	check(grid.at(4).left() > grid.at(0).left(), "in two different columns");
	checkEq(boxesOutsideContent(documentWith(multi)), 0, "every box stays inside the section");
}

CT_SUITE(card_list, "A card is a picture with its caption `logoGap` under it")
{
	Section section = unpadded(SectionType::CardList);
	section.logoGap = 18;
	withLogo(section, 120);

	const Document document = documentWith(section);
	const QRectF picture = boxOf(document, LayoutBox::Kind::Logo);
	const QVector<QRectF> lines = boxesOf(document, LayoutBox::Kind::Text);
	check(!picture.isNull(), "the picture is placed");
	checkEq(lines.size(), 2, "with a title and a subtitle under it");
	checkEq(qRound(picture.height()), 120, "the picture is its own height");
	checkEq(qRound(lines.at(0).top() - picture.bottom()), 18, "the caption starts logoGap under it");
	checkNear(picture.center().x(), document.width / 2.0, 1.0, "centered as the style says");

	/* In a row, pictures of different heights still put their captions on one line. */
	Section grid = unpadded(SectionType::MultiCardList);
	withLogo(grid, 80);
	grid.entries[1].logo.maxHeight = 160;
	const Document gridDocument = documentWith(grid);
	const QVector<QRectF> pictures = boxesOf(gridDocument, LayoutBox::Kind::Logo);
	const QVector<QRectF> captions = boxesOf(gridDocument, LayoutBox::Kind::Text);
	checkEq(pictures.size(), 4, "four cards, four pictures");
	checkNear(captions.at(0).top(), captions.at(2).top(), 0.5,
		  "the caption under a short picture starts level with the one under a tall picture");
	checkNear(pictures.at(0).bottom(), pictures.at(1).bottom(), 0.5, "the pictures stand on the same line");
	checkEq(boxesOutsideContent(gridDocument), 0, "every box stays inside the section");

	/* A card with no caption is just its picture. */
	Section bare = section;
	bare.entries.first().text.clear();
	bare.entries.first().secondaryText.clear();
	checkEq(measure(documentWith(bare)), 120, "a card with no words takes no gap for them");
}

CT_SUITE(icon_list, "An icon is sized to the capitals of its line and sits `logoGap` before it")
{
	Section section = unpadded(SectionType::IconList);
	section.style.align = HAlign::Left;
	section.logoGap = 20;
	section.iconScale = 1.0;
	withLogo(section);

	QFont font(section.style.family);
	font.setPixelSize(section.style.pixelSize);
	const qreal capHeight = QFontMetricsF(font).capHeight();

	const Document document = documentWith(section);
	const QRectF icon = boxOf(document, LayoutBox::Kind::Logo);
	const QRectF text = boxOf(document, LayoutBox::Kind::Text);
	check(!icon.isNull() && !text.isNull(), "the icon and the line are both placed");
	checkNear(icon.height(), capHeight, 1.0, "at a scale of one, the icon is as tall as a capital");
	checkEq(qRound(text.left() - icon.right()), 20, "the line starts logoGap after the icon");
	checkNear(icon.left(), 0.0, 1.0, "the pair starts at the left edge, as aligned");

	/* Bigger icons stay centered on the line instead of hanging off the top of the row. */
	Section big = section;
	big.iconScale = 3.0;
	const Document bigDocument = documentWith(big);
	const QRectF bigIcon = boxOf(bigDocument, LayoutBox::Kind::Logo);
	check(bigIcon.top() >= 0.0, "a tall icon pushes the row down rather than poking above it");
	checkNear(bigIcon.height(), capHeight * 3.0, 1.0, "and is three capitals tall");

	/* The other side. */
	Section right = section;
	right.logoSide = LogoSide::Right;
	const Document rightDocument = documentWith(right);
	check(boxOf(rightDocument, LayoutBox::Kind::Logo).left() > boxOf(rightDocument, LayoutBox::Kind::Text).left(),
	      "logoSide Right puts the icon after the line");

	/* A row with no icon is a plain line. */
	Section plain = section;
	plain.entries.first().logo.path.clear();
	const Document plainDocument = documentWith(plain);
	check(boxOf(plainDocument, LayoutBox::Kind::Logo).isNull(), "no file, no icon");
	checkNear(boxOf(plainDocument, LayoutBox::Kind::Text).left(), 0.0, 1.0, "and no gap held for one");

	/* An icon is decoded at the height it is drawn, not at the height stored on the entry. */
	check(section.entries.first().logo.maxHeight == 110, "the stored height is left alone");
	checkEq(boxesOutsideContent(documentWith(unpadded(SectionType::MultiIconList))), 0,
		"every box stays inside the section");
}

CT_SUITE(new_types_fonts, "Only lines that are drawn report a font")
{
	Document document;
	Section stack = Section::makeDefault(SectionType::StackedList);
	stack.rowSecondarySubtitleStyle.family = QStringLiteral("Only On Line Four");
	document.sections = {stack};
	check(document.usedFontFamilies().contains(QStringLiteral("Only On Line Four")),
	      "a stack's fourth line reports its font");

	document.sections[0].entries[0].secondarySubtitle.clear();
	check(!document.usedFontFamilies().contains(QStringLiteral("Only On Line Four")),
	      "and stops once no entry has a fourth line");

	Section code = Section::makeDefault(SectionType::QrCode);
	code.style.family = QStringLiteral("Caption Face");
	document.sections = {code};
	check(document.usedFontFamilies().contains(QStringLiteral("Caption Face")),
	      "a code's caption reports its font");
	document.sections[0].text.clear();
	check(!document.usedFontFamilies().contains(QStringLiteral("Caption Face")),
	      "a code without a caption sets no text");
}

CT_SUITE(qr_design, "A styled QR code: shapes, eyes, fill and a logo, none of which move anything")
{
	/* --- the model ----------------------------------------------------------------------- */

	Section section = unpadded(SectionType::QrCode);
	section.text.clear();
	section.qrDesign.moduleShape = QrModuleShape::Connected;
	section.qrDesign.moduleScale = 0.8;
	section.qrDesign.eyeFrame = QrEyeShape::Rounded;
	section.qrDesign.eyePupil = QrEyeShape::Circle;
	section.qrDesign.eyeColors = true;
	section.qrDesign.eyeFrameColor = QColor(255, 0, 0);
	section.qrDesign.eyePupilColor = QColor(0, 0, 255);
	section.qrDesign.fill = TextFill::RadialGradient;
	section.qrDesign.plateRadius = 20;
	section.qrDesign.logo.path = testLogoPath();
	section.qrDesign.logoSize = 0.25;
	section.qrDesign.logoClear = false;
	check(roundTrip(section).qrDesign == section.qrDesign, "every design field survives a save and a load");

	OBSDataAutoRelease bare = obs_data_create();
	obs_data_set_string(bare, "type", "qr_code");
	Section old;
	old.load(bare);
	check(old.qrDesign == QrDesign{}, "a code written before codes could be styled loads as plain squares");

	OBSDataAutoRelease diamondFrame = obs_data_create();
	OBSDataAutoRelease diamondDesign = obs_data_create();
	obs_data_set_string(diamondDesign, "eye_frame", "diamond");
	obs_data_set_string(diamondDesign, "eye_pupil", "diamond");
	obs_data_set_obj(diamondFrame, "qr_design", diamondDesign);
	Section diamonds;
	diamonds.load(diamondFrame);
	check(diamonds.qrDesign.eyeFrame == QrEyeShape::Square, "a diamond frame, which phones misread, loads square");
	check(diamonds.qrDesign.eyePupil == QrEyeShape::Diamond, "while a diamond pupil is kept");

	OBSDataAutoRelease wild = obs_data_create();
	OBSDataAutoRelease wildDesign = obs_data_create();
	obs_data_set_double(wildDesign, "module_scale", 0.1);
	obs_data_set_double(wildDesign, "logo_size", 0.9);
	obs_data_set_obj(wild, "qr_design", wildDesign);
	Section clamped;
	clamped.load(wild);
	checkNear(clamped.qrDesign.moduleScale, 0.5, 1e-9, "a module scale too small to read is held at half");
	checkNear(clamped.qrDesign.logoSize, 0.3, 1e-9, "a logo too big to recover from is held at 30 %");

	for (const QrLook &look : qrLooks()) {
		const Context context(QString::fromLatin1(look.id));
		QrDesign design;
		design.fill = TextFill::LinearGradient;
		applyQrLook(design, look);
		check(matchingQrLook(design) == &look, "a look applied is the look matched");
		check(design.fill == TextFill::LinearGradient, "and leaves the fill alone");
	}
	QrDesign own;
	own.moduleShape = QrModuleShape::Dots;
	own.eyeFrame = QrEyeShape::Diamond;
	check(matchingQrLook(own) == nullptr, "a mix no look makes is the user's own");

	/* --- a logo is High, and clears the middle --------------------------------------------- */

	Section plain = unpadded(SectionType::QrCode);
	plain.text.clear();
	plain.qrErrorCorrection = QrErrorCorrection::Low;
	check(effectiveQrLevel(plain) == QrErrorCorrection::Low, "with no logo, the chosen level is used");
	Section logo = plain;
	logo.qrDesign.logo.path = testLogoPath();
	check(effectiveQrLevel(logo) == QrErrorCorrection::High, "with a logo, the code is encoded at High");

	const QrMatrix matrix = encodeQr(logo.qrText, QrErrorCorrection::High);
	QrDesign withLogo = logo.qrDesign;
	const QrPlacement placed = placeQr(matrix, 300, 4, 0, 0, withLogo);
	check(!placed.logo.isNull(), "the logo has a square");
	checkNear(placed.logo.center().x(), placed.code.center().x(), placed.module, "centered on the code");
	check(!placed.ink.intersects(QRectF(placed.logo)), "no square is drawn under a cleared logo");
	withLogo.logoClear = false;
	check(placeQr(matrix, 300, 4, 0, 0, withLogo).ink.intersects(QRectF(placed.logo)),
	      "and the squares stay when clearing is off");

	const Document logoDocument = documentWith(logo);
	checkEq(boxesOf(logoDocument, LayoutBox::Kind::Logo).size(), 2, "the plate and the logo are both placed");
	check(boxOf(logoDocument, LayoutBox::Kind::Logo).contains(boxOf(logoDocument, LayoutBox::Kind::Logo, 1)),
	      "the logo sits inside the plate");

	/* --- the eyes are drawn apart from the data -------------------------------------------- */

	const QrPlacement square = placeQr(matrix, 300, 4, 0, 0, QrDesign());
	const QRectF topLeftEye(square.code.left(), square.code.top(), 7 * square.module, 7 * square.module);
	check(!square.ink.intersects(topLeftEye.adjusted(1, 1, -1, -1)), "the data squares leave the eyes out");
	check(square.eyeFrames.intersects(topLeftEye) && square.eyePupils.intersects(topLeftEye),
	      "which are drawn as a frame and a pupil");

	Section colored = plain;
	colored.qrDesign.eyeColors = true;
	colored.qrDesign.eyeFrameColor = QColor(255, 0, 0);
	colored.qrDesign.eyePupilColor = QColor(0, 0, 255);
	const QImage image = renderImage(documentWith(colored));
	const QRectF plate = boxOf(documentWith(colored), LayoutBox::Kind::Logo);
	const QrMatrix plainMatrix = encodeQr(colored.qrText, effectiveQrLevel(colored));
	const int m = qRound(plate.width()) / (plainMatrix.size + 2 * colored.qrQuietZone);
	const QPoint eye = plate.topLeft().toPoint() + QPoint(4 * m, 4 * m);
	const QRgb frame = image.pixel(eye + QPoint(m / 2, 3 * m));
	const QRgb pupil = image.pixel(eye + QPoint(3 * m + m / 2, 3 * m + m / 2));
	check(qRed(frame) > 200 && qBlue(frame) < 60, "the eye's frame takes its own color");
	check(qBlue(pupil) > 200 && qRed(pupil) < 60, "and its pupil another");

	/* --- styling draws differently and lays out identically -------------------------------- */

	const int height = measure(documentWith(plain));
	const QRectF plainPlate = boxOf(documentWith(plain), LayoutBox::Kind::Logo);
	const double plainShare = inkedShare(renderImage(documentWith([&] {
						     Section clear = plain;
						     clear.qrBackground = QColor(0, 0, 0, 0);
						     return clear;
					     }())),
					     plainPlate.toRect());
	for (const QrLook &look : qrLooks()) {
		const Context context(QString::fromLatin1(look.id));
		Section styled = plain;
		applyQrLook(styled.qrDesign, look);
		checkEq(measure(documentWith(styled)), height, "a look changes no height");
		check(boxOf(documentWith(styled), LayoutBox::Kind::Logo) == plainPlate, "nor where the plate is");

		styled.qrBackground = QColor(0, 0, 0, 0);
		const double share = inkedShare(renderImage(documentWith(styled)), plainPlate.toRect());
		check(share > plainShare * 0.5 && share < plainShare * 1.3,
		      "and still inks roughly as much of the code as plain squares do");
	}
}
