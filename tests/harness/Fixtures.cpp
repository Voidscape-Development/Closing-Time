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
			bridged.entries = {Entry{QStringLiteral("Director"), QStringLiteral("Jane Doe"), {}},
					   Entry{QStringLiteral("Director of Photography"),
						 QStringLiteral("A Rather Longer Name"),
						 {}},
					   Entry{QStringLiteral("Editor"), QStringLiteral("R Singh"), {}}};

			Section pairs = Section::makeDefault(SectionType::TitleSubtitleList);
			pairs.entries = {Entry{QStringLiteral("Producer"), QStringLiteral("Chris Nakamura"), {}},
					 Entry{QStringLiteral("Line Producer"), QStringLiteral("Sam Oyelaran"), {}}};

			add(QStringLiteral("lists"), QStringLiteral("A bridged list over a title/subtitle list"),
			    documentWith({bridged, pairs}));
		}

		/* --- dividers ---------------------------------------------------------------- */

		{
			Section plain = Section::makeDefault(SectionType::SectionDivider);

			Section stacked = Section::makeDefault(SectionType::SectionDivider);
			stacked.dividerRules = 3;
			stacked.dividerRuleInset = 40.0;

			Section labelled = Section::makeDefault(SectionType::SectionDivider);
			labelled.dividerCentre = {DividerPiece{DividerPiece::Kind::Text,
							       DividerShape::Diamond,
							       {},
							       1.0,
							       QStringLiteral("PART II"),
							       {}}};

			add(QStringLiteral("dividers"),
			    QStringLiteral("A plain rule, a tapered stack and a labelled break"),
			    documentWith({plain, stacked, labelled}));
		}

		/* --- everything -------------------------------------------------------------- */

		add(QStringLiteral("every-section-type"),
		    QStringLiteral("One section of every type at its defaults, in menu order"), everySectionType());

		return list;
	}();

	return built;
}

} // namespace closingtime::test
