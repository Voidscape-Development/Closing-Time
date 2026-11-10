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

#include "ui/DesignerWidgets.hpp"

#include <obs-module.h>

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QSettings>
#include <QSpinBox>
#include <QTextEdit>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <memory>

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* How long a toast stays up before it goes by itself, in milliseconds. */
constexpr int kToastMs = 6000;

/* The amber used for "something here differs" and for warnings, everywhere in the designer. */
const QColor kAmber(240, 164, 58);

bool isTitleType(SectionType type)
{
	switch (type) {
	case SectionType::Title:
	case SectionType::TitleWithSubtitle:
	case SectionType::TitleWithLogo:
	case SectionType::TitleWithSubtitleAndLogo:
		return true;
	default:
		return false;
	}
}

QSettings &uiSettings()
{
	/*
	 * Beside the plugin's other per-machine files. obs_module_config_path hands back an allocation
	 * of its own, which is copied and freed at once rather than held.
	 */
	/* Held by pointer: QSettings cannot be copied or moved, so it is built where it will live. */
	static const std::unique_ptr<QSettings> settings = [] {
		char *raw = obs_module_config_path("designer-ui.ini");
		const QString path = raw ? QString::fromUtf8(raw) : QString();
		bfree(raw);
		return path.isEmpty()
			       ? std::make_unique<QSettings>(QStringLiteral("Voidscape"), QStringLiteral("ClosingTime"))
			       : std::make_unique<QSettings>(path, QSettings::IniFormat);
	}();
	return *settings;
}

} // namespace

/* --- what a section looks like ------------------------------------------------------------- */

SectionFamily sectionFamily(SectionType type)
{
	switch (type) {
	case SectionType::LogoTitle:
	case SectionType::LogoHeader:
		return SectionFamily::LogoHeading;
	case SectionType::Bridged:
		return SectionFamily::Bridged;
	case SectionType::TextList:
	case SectionType::TitleSubtitleList:
	case SectionType::LogoList:
	case SectionType::StackedList:
	case SectionType::CardList:
	case SectionType::IconList:
		return SectionFamily::List;
	case SectionType::MultiTextList:
	case SectionType::MultiTitleSubtitleList:
	case SectionType::MultiLogoList:
	case SectionType::MultiStackedList:
	case SectionType::MultiCardList:
	case SectionType::MultiIconList:
		return SectionFamily::Columns;
	case SectionType::Paragraph:
		return SectionFamily::Paragraph;
	case SectionType::QrCode:
		return SectionFamily::QrCode;
	case SectionType::SectionDivider:
		return SectionFamily::Divider;
	case SectionType::Spacer:
		return SectionFamily::Spacer;
	case SectionType::StickyBlock:
		return SectionFamily::Sticky;
	case SectionType::Action:
		return SectionFamily::Action;
	case SectionType::PageBreak:
		return SectionFamily::PageBreak;
	default:
		return SectionFamily::Heading;
	}
}

QColor familyTileColor(SectionFamily family)
{
	switch (family) {
	case SectionFamily::Heading:
		return QColor(84, 66, 140);
	case SectionFamily::LogoHeading:
		return QColor(120, 86, 40);
	case SectionFamily::List:
	case SectionFamily::Columns:
		return QColor(38, 92, 138);
	case SectionFamily::Bridged:
		return QColor(30, 110, 96);
	case SectionFamily::Divider:
		return QColor(132, 98, 34);
	case SectionFamily::Spacer:
		return QColor(84, 90, 104);
	case SectionFamily::Sticky:
		return QColor(118, 70, 140);
	case SectionFamily::Action:
		return QColor(150, 60, 60);
	case SectionFamily::Paragraph:
		return QColor(70, 96, 70);
	case SectionFamily::QrCode:
		return QColor(56, 60, 70);
	case SectionFamily::PageBreak:
		return QColor(140, 92, 30);
	}
	return QColor(84, 90, 104);
}

QColor familyMarkColor(SectionFamily family)
{
	switch (family) {
	case SectionFamily::Heading:
		return QColor(222, 212, 255);
	case SectionFamily::LogoHeading:
	case SectionFamily::Divider:
		return QColor(255, 222, 160);
	case SectionFamily::List:
	case SectionFamily::Columns:
		return QColor(196, 228, 255);
	case SectionFamily::Bridged:
		return QColor(178, 245, 225);
	case SectionFamily::Spacer:
		return QColor(220, 224, 232);
	case SectionFamily::Sticky:
		return QColor(240, 210, 255);
	case SectionFamily::Action:
		return QColor(255, 210, 210);
	case SectionFamily::Paragraph:
		return QColor(214, 240, 210);
	case SectionFamily::QrCode:
		return QColor(240, 242, 248);
	case SectionFamily::PageBreak:
		return QColor(255, 214, 160);
	}
	return Qt::white;
}

void paintTypeBadge(QPainter &painter, const QRectF &rect, SectionType type)
{
	const SectionFamily family = sectionFamily(type);
	const qreal s = std::min(rect.width(), rect.height());
	const QRectF tile(rect.center().x() - s / 2, rect.center().y() - s / 2, s, s);
	const QColor mark = familyMarkColor(family);

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(familyTileColor(family));
	painter.drawRoundedRect(tile, s * 0.24, s * 0.24);

	/* Everything below is drawn in a unit box inset from the tile's edge. */
	const QRectF box = tile.adjusted(s * 0.24, s * 0.24, -s * 0.24, -s * 0.24);
	const qreal w = box.width();
	const qreal h = box.height();
	QPen pen(mark, std::max<qreal>(1.2, s * 0.085), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);

	const auto hline = [&](qreal y, qreal from, qreal to) {
		painter.drawLine(QPointF(box.left() + w * from, box.top() + h * y),
				 QPointF(box.left() + w * to, box.top() + h * y));
	};

	switch (family) {
	case SectionFamily::Heading: {
		QFont font = painter.font();
		font.setBold(true);
		font.setPixelSize(std::max(7, qRound(s * 0.56)));
		painter.setFont(font);
		painter.drawText(tile, Qt::AlignCenter, isTitleType(type) ? QStringLiteral("T") : QStringLiteral("H"));
		break;
	}
	case SectionFamily::LogoHeading:
		painter.setBrush(mark);
		painter.setPen(Qt::NoPen);
		painter.drawRoundedRect(QRectF(box.left(), box.top() + h * 0.2, w, h * 0.6), s * 0.06, s * 0.06);
		break;
	case SectionFamily::List:
		hline(0.1, 0.15, 0.85);
		hline(0.5, 0.15, 0.85);
		hline(0.9, 0.15, 0.85);
		break;
	case SectionFamily::Columns:
		for (const qreal y : {0.2, 0.8}) {
			hline(y, 0.0, 0.22);
			hline(y, 0.39, 0.61);
			hline(y, 0.78, 1.0);
		}
		break;
	case SectionFamily::Bridged: {
		for (const qreal y : {0.22, 0.78}) {
			hline(y, 0.0, 0.2);
			hline(y, 0.8, 1.0);
			QPen dots = pen;
			dots.setWidthF(pen.widthF() * 0.8);
			dots.setStyle(Qt::DotLine);
			painter.setPen(dots);
			hline(y, 0.32, 0.68);
			painter.setPen(pen);
		}
		break;
	}
	case SectionFamily::Divider: {
		hline(0.5, 0.0, 0.32);
		hline(0.5, 0.68, 1.0);
		const QPointF c(box.center());
		const qreal r = w * 0.17;
		QPainterPath diamond;
		diamond.moveTo(c.x(), c.y() - r);
		diamond.lineTo(c.x() + r, c.y());
		diamond.lineTo(c.x(), c.y() + r);
		diamond.lineTo(c.x() - r, c.y());
		diamond.closeSubpath();
		painter.fillPath(diamond, mark);
		break;
	}
	case SectionFamily::Spacer: {
		const qreal x = box.center().x();
		painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
		painter.drawLine(QPointF(x - w * 0.25, box.top() + h * 0.25), QPointF(x, box.top()));
		painter.drawLine(QPointF(x + w * 0.25, box.top() + h * 0.25), QPointF(x, box.top()));
		painter.drawLine(QPointF(x - w * 0.25, box.bottom() - h * 0.25), QPointF(x, box.bottom()));
		painter.drawLine(QPointF(x + w * 0.25, box.bottom() - h * 0.25), QPointF(x, box.bottom()));
		break;
	}
	case SectionFamily::Sticky:
		painter.drawRoundedRect(box, s * 0.05, s * 0.05);
		painter.setBrush(mark);
		painter.setPen(Qt::NoPen);
		painter.drawRect(QRectF(box.left() + w * 0.28, box.top() + h * 0.3, w * 0.44, h * 0.4));
		break;
	case SectionFamily::Action: {
		QPainterPath play;
		play.moveTo(box.left() + w * 0.18, box.top());
		play.lineTo(box.right() - w * 0.05, box.center().y());
		play.lineTo(box.left() + w * 0.18, box.bottom());
		play.closeSubpath();
		painter.fillPath(play, mark);
		break;
	}
	case SectionFamily::Paragraph:
		/* A block of prose: full lines and a short last one. */
		hline(0.05, 0.0, 1.0);
		hline(0.35, 0.0, 1.0);
		hline(0.65, 0.0, 1.0);
		hline(0.95, 0.0, 0.55);
		break;
	case SectionFamily::PageBreak: {
		/* Two pages, one over the other, with a dashed break between them. */
		painter.setBrush(Qt::NoBrush);
		QPen thin = pen;
		thin.setWidthF(pen.widthF() * 0.8);
		painter.setPen(thin);
		painter.drawRect(QRectF(box.left() + w * 0.15, box.top(), w * 0.7, h * 0.36));
		painter.drawRect(QRectF(box.left() + w * 0.15, box.bottom() - h * 0.36, w * 0.7, h * 0.36));
		QPen dashed = thin;
		dashed.setStyle(Qt::DashLine);
		painter.setPen(dashed);
		hline(0.5, 0.0, 1.0);
		break;
	}
	case SectionFamily::QrCode: {
		/* Three finder squares and a scatter of modules. */
		painter.setBrush(Qt::NoBrush);
		const qreal f = w * 0.36;
		QPen thin = pen;
		thin.setWidthF(pen.widthF() * 0.8);
		painter.setPen(thin);
		for (const QPointF &corner :
		     {box.topLeft(), QPointF(box.right() - f, box.top()), QPointF(box.left(), box.bottom() - f)})
			painter.drawRect(QRectF(corner, QSizeF(f, f)));
		painter.setPen(Qt::NoPen);
		painter.setBrush(mark);
		const qreal m = w * 0.14;
		for (const QPointF &cell : {QPointF(0.62, 0.62), QPointF(0.86, 0.62), QPointF(0.62, 0.86),
					    QPointF(0.86, 0.86), QPointF(0.45, 0.45)})
			painter.drawRect(
				QRectF(box.left() + w * cell.x() - m / 2, box.top() + h * cell.y() - m / 2, m, m));
		break;
	}
	}

	painter.restore();
}

QPixmap typeBadgePixmap(SectionType type, int size, qreal devicePixelRatio)
{
	QPixmap pixmap(QSize(size, size) * devicePixelRatio);
	pixmap.setDevicePixelRatio(devicePixelRatio);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	paintTypeBadge(painter, QRectF(0, 0, size, size), type);
	return pixmap;
}

void paintTypeThumbnail(QPainter &painter, const QRectF &rect, SectionType type)
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(21, 23, 29));
	painter.drawRoundedRect(rect, 4, 4);

	const QColor ink(214, 216, 224);
	const QColor soft(138, 143, 156);
	const qreal cx = rect.center().x();
	const qreal cy = rect.center().y();
	const qreal w = rect.width();
	const qreal h = rect.height();

	const auto bar = [&](qreal y, qreal width, qreal thickness, const QColor &color) {
		painter.setBrush(color);
		painter.drawRoundedRect(QRectF(cx - width / 2, y - thickness / 2, width, thickness), thickness / 2,
					thickness / 2);
	};

	switch (sectionFamily(type)) {
	case SectionFamily::Heading:
		if (isTitleType(type)) {
			bar(cy - h * 0.08, w * 0.58, 3.5, ink);
			bar(cy + h * 0.14, w * 0.36, 2.0, soft);
		} else {
			bar(cy, w * 0.38, 3.0, ink);
		}
		break;
	case SectionFamily::LogoHeading:
		painter.setBrush(QColor(245, 199, 122));
		painter.drawRoundedRect(QRectF(cx - w * 0.14, cy - h * 0.18, w * 0.28, h * 0.36), 2, 2);
		break;
	case SectionFamily::List:
		if (sectionUsesCards(type)) {
			/* A picture with its caption under it. */
			painter.setBrush(QColor(245, 199, 122));
			painter.drawRoundedRect(QRectF(cx - w * 0.08, rect.top() + h * 0.12, w * 0.16, h * 0.42), 1.5,
						1.5);
			bar(cy + h * 0.2, w * 0.24, 2.2, ink);
			bar(cy + h * 0.33, w * 0.16, 1.4, soft);
		} else if (sectionUsesIcons(type)) {
			/* A small mark leading each line. */
			for (int i = -1; i <= 1; ++i) {
				const qreal y = cy + i * h * 0.24;
				painter.setBrush(QColor(245, 199, 122));
				painter.drawRoundedRect(QRectF(cx - w * 0.17, y - 3, 6, 6), 1.2, 1.2);
				painter.setBrush(soft);
				painter.drawRoundedRect(QRectF(cx - w * 0.17 + 9, y - 1, w * 0.24, 2), 1, 1);
			}
		} else if (sectionStacksLines(type)) {
			/* A stack of four lines, each smaller than the last. */
			bar(cy - h * 0.27, w * 0.34, 2.4, ink);
			bar(cy - h * 0.09, w * 0.28, 1.6, soft);
			bar(cy + h * 0.07, w * 0.28, 1.6, soft);
			bar(cy + h * 0.23, w * 0.22, 1.2, soft);
		} else if (sectionUsesLogos(type)) {
			/* A run of images, one under the next. */
			painter.setBrush(QColor(245, 199, 122));
			for (int i = -1; i <= 1; i += 2)
				painter.drawRoundedRect(QRectF(cx - w * 0.07, cy + i * h * 0.17 - h * 0.12, w * 0.14,
							       h * 0.24),
							1.5, 1.5);
		} else if (sectionUsesSubtitles(type)) {
			/* Pairs: a line with a smaller one under it. */
			for (int i = -1; i <= 1; i += 2) {
				bar(cy + i * h * 0.2 - 2, w * 0.3, 2.2, ink);
				bar(cy + i * h * 0.2 + 2.5, w * 0.2, 1.4, soft);
			}
		} else {
			for (int i = -1; i <= 1; ++i)
				bar(cy + i * h * 0.2, w * 0.32, 2.0, soft);
		}
		break;
	case SectionFamily::Columns:
		for (int col = -1; col <= 1; ++col) {
			for (int i = -1; i <= 1; ++i) {
				if (sectionUsesCards(type)) {
					/* A row of cards: picture over caption. */
					if (i != -1)
						continue;
					painter.setBrush(QColor(245, 199, 122));
					painter.drawRoundedRect(QRectF(cx + col * w * 0.22 - w * 0.07,
								       rect.top() + h * 0.12, w * 0.14, h * 0.42),
								1.5, 1.5);
					painter.setBrush(soft);
					painter.drawRoundedRect(QRectF(cx + col * w * 0.22 - w * 0.07, cy + h * 0.2,
								       w * 0.14, 2),
								1, 1);
					continue;
				}
				if (sectionUsesIcons(type)) {
					const qreal x = cx + col * w * 0.28 - w * 0.1;
					painter.setBrush(QColor(245, 199, 122));
					painter.drawRect(QRectF(x, cy + i * h * 0.24 - 2, 4, 4));
					painter.setBrush(soft);
					painter.drawRoundedRect(QRectF(x + 6, cy + i * h * 0.24 - 1, w * 0.12, 2), 1,
								1);
					continue;
				}
				if (sectionUsesLogos(type)) {
					/* A wall of sponsor marks. */
					if (i == 0)
						continue;
					painter.setBrush(QColor(245, 199, 122));
					painter.drawRoundedRect(QRectF(cx + col * w * 0.22 - w * 0.07,
								       cy + i * h * 0.18 - h * 0.11, w * 0.14,
								       h * 0.22),
								1.5, 1.5);
					continue;
				}
				painter.setBrush(soft);
				painter.drawRoundedRect(QRectF(cx + col * w * 0.24 - w * 0.08, cy + i * h * 0.2 - 1,
							       w * 0.16, 2),
							1, 1);
			}
		}
		break;
	case SectionFamily::Bridged: {
		for (const qreal y : {cy - h * 0.13, cy + h * 0.13}) {
			painter.setBrush(soft);
			painter.drawRoundedRect(QRectF(rect.left() + w * 0.14, y - 1, w * 0.16, 2), 1, 1);
			painter.drawRoundedRect(QRectF(rect.right() - w * 0.30, y - 1, w * 0.16, 2), 1, 1);
			for (qreal x = rect.left() + w * 0.35; x < rect.right() - w * 0.35; x += 4)
				painter.drawEllipse(QPointF(x, y), 0.9, 0.9);
		}
		break;
	}
	case SectionFamily::Divider: {
		painter.setPen(QPen(ink, 1.2));
		painter.drawLine(QPointF(rect.left() + w * 0.12, cy), QPointF(cx - 6, cy));
		painter.drawLine(QPointF(cx + 6, cy), QPointF(rect.right() - w * 0.12, cy));
		painter.setPen(Qt::NoPen);
		QPainterPath diamond;
		diamond.moveTo(cx, cy - 4);
		diamond.lineTo(cx + 4, cy);
		diamond.lineTo(cx, cy + 4);
		diamond.lineTo(cx - 4, cy);
		diamond.closeSubpath();
		painter.fillPath(diamond, ink);
		break;
	}
	case SectionFamily::Spacer:
		painter.setPen(QPen(soft, 1.4, Qt::SolidLine, Qt::RoundCap));
		painter.drawLine(QPointF(cx, cy - h * 0.3), QPointF(cx, cy + h * 0.3));
		painter.drawLine(QPointF(cx - 4, cy - h * 0.3 + 4), QPointF(cx, cy - h * 0.3));
		painter.drawLine(QPointF(cx + 4, cy - h * 0.3 + 4), QPointF(cx, cy - h * 0.3));
		painter.drawLine(QPointF(cx - 4, cy + h * 0.3 - 4), QPointF(cx, cy + h * 0.3));
		painter.drawLine(QPointF(cx + 4, cy + h * 0.3 - 4), QPointF(cx, cy + h * 0.3));
		break;
	case SectionFamily::Sticky:
		painter.setPen(QPen(QColor(227, 176, 255), 1.2));
		painter.setBrush(Qt::NoBrush);
		painter.drawRoundedRect(QRectF(cx - w * 0.22, cy - h * 0.28, w * 0.44, h * 0.56), 2, 2);
		painter.setPen(Qt::NoPen);
		bar(cy, w * 0.26, 2.5, ink);
		break;
	case SectionFamily::PageBreak: {
		/* The end of one page and the start of the next, cut between them. */
		painter.setBrush(soft);
		painter.drawRoundedRect(QRectF(cx - w * 0.2, rect.top() + h * 0.18, w * 0.4, 2), 1, 1);
		painter.drawRoundedRect(QRectF(cx - w * 0.2, rect.bottom() - h * 0.18 - 2, w * 0.4, 2), 1, 1);
		QPen cut(QColor(255, 180, 90), 1.2);
		cut.setStyle(Qt::DashLine);
		painter.setPen(cut);
		painter.drawLine(QPointF(rect.left() + w * 0.1, cy), QPointF(rect.right() - w * 0.1, cy));
		painter.setPen(Qt::NoPen);
		break;
	}
	case SectionFamily::Paragraph:
		for (int i = 0; i < 4; ++i) {
			const qreal y = rect.top() + h * (0.24 + i * 0.17);
			const qreal width = i == 3 ? w * 0.3 : w * 0.56;
			painter.setBrush(soft);
			painter.drawRoundedRect(QRectF(cx - w * 0.28, y - 1, width, 2), 1, 1);
		}
		break;
	case SectionFamily::QrCode: {
		/* A small code on its plate, drawn from a fixed pattern. */
		const qreal side = h * 0.78;
		const QRectF plate(cx - side / 2, cy - side / 2, side, side);
		painter.setBrush(QColor(236, 238, 244));
		painter.drawRect(plate);
		static const char *rows[] = {"1110101", "1010011", "1110101", "0001010",
					     "1101110", "0110101", "1011011"};
		const qreal m = side * 0.8 / 7.0;
		painter.setBrush(QColor(21, 23, 29));
		for (int y = 0; y < 7; ++y) {
			for (int x = 0; x < 7; ++x) {
				if (rows[y][x] == '1')
					painter.drawRect(QRectF(plate.left() + side * 0.1 + x * m,
								plate.top() + side * 0.1 + y * m, m, m));
			}
		}
		break;
	}
	case SectionFamily::Action: {
		QPen dashed(QColor(80, 230, 230), 1.2);
		dashed.setStyle(Qt::DashLine);
		painter.setPen(dashed);
		painter.drawLine(QPointF(rect.left() + w * 0.1, cy), QPointF(rect.right() - w * 0.1, cy));
		painter.setPen(Qt::NoPen);
		QPainterPath play;
		play.moveTo(rect.left() + w * 0.1, cy - 5);
		play.lineTo(rect.left() + w * 0.1 + 8, cy);
		play.lineTo(rect.left() + w * 0.1, cy + 5);
		play.closeSubpath();
		painter.fillPath(play, QColor(80, 230, 230));
		break;
	}
	}

	painter.restore();
}

QString describeSection(const Section &section)
{
	const auto count = [](int n, const char *one, const char *many) {
		return n == 1 ? moduleText(one) : moduleText(many).arg(n);
	};

	const int entries = static_cast<int>(section.entries.size());

	switch (sectionFamily(section.type)) {
	case SectionFamily::Bridged:
		return moduleText("Designer.Detail.Bridged")
			.arg(count(entries, "Designer.Detail.Row", "Designer.Detail.Rows"));
	case SectionFamily::List:
		return QStringLiteral("%1 · %2").arg(QString::fromUtf8(sectionTypeName(section.type)),
						     count(entries, "Designer.Detail.Entry",
							   "Designer.Detail.Entries"));
	case SectionFamily::Columns:
		return moduleText("Designer.Detail.Columns")
			.arg(section.columns)
			.arg(count(entries, "Designer.Detail.Entry", "Designer.Detail.Entries"));
	case SectionFamily::Spacer:
		return moduleText("Designer.Detail.Spacer").arg(section.spacerHeight);
	case SectionFamily::Sticky:
		if (section.stickyHoldForever)
			return moduleText("Designer.Detail.StickyForever");
		return moduleText("Designer.Detail.Sticky").arg(QString::number(section.stickyHold, 'g', 3));
	case SectionFamily::Action:
		return moduleText("Designer.Detail.Action").arg(qRound(section.actionLine * 100.0));
	default:
		return QString::fromUtf8(sectionTypeName(section.type));
	}
}

QIcon changedDotIcon()
{
	QPixmap pixmap(QSize(12, 12) * 2);
	pixmap.setDevicePixelRatio(2.0);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(kAmber);
	painter.drawEllipse(QRectF(3, 3, 6, 6));
	return QIcon(pixmap);
}

QPixmap infoPixmap(int size, const QColor &color, qreal devicePixelRatio)
{
	QPixmap pixmap(QSize(size, size) * devicePixelRatio);
	pixmap.setDevicePixelRatio(devicePixelRatio);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(color, 1.0));
	painter.drawEllipse(QRectF(0.5, 0.5, size - 1.0, size - 1.0));

	QFont font = painter.font();
	font.setBold(true);
	font.setPixelSize(std::max(6, qRound(size * 0.62)));
	painter.setFont(font);
	painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QStringLiteral("i"));
	return pixmap;
}

QIcon undoIcon(bool redo, const QColor &color)
{
	QIcon icon;

	for (const qreal dpr : {1.0, 2.0}) {
		QPixmap pixmap(QSize(16, 16) * dpr);
		pixmap.setDevicePixelRatio(dpr);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		if (redo) {
			painter.translate(16, 0);
			painter.scale(-1, 1);
		}

		QPen pen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
		painter.setPen(pen);
		QPainterPath arc;
		arc.moveTo(4, 7);
		arc.cubicTo(7, 4, 13, 4, 13, 9);
		arc.cubicTo(13, 12, 10, 13, 8, 13);
		painter.drawPath(arc);
		painter.drawLine(QPointF(4, 7), QPointF(4.5, 3.2));
		painter.drawLine(QPointF(4, 7), QPointF(7.8, 7.4));
		painter.end();

		icon.addPixmap(pixmap, QIcon::Normal);

		/* A disabled arrow is the same arrow, faded, rather than whatever the style makes of it. */
		QPixmap faded(pixmap.size());
		faded.setDevicePixelRatio(dpr);
		faded.fill(Qt::transparent);
		QPainter fade(&faded);
		fade.setOpacity(0.35);
		fade.drawPixmap(0, 0, pixmap);
		fade.end();
		icon.addPixmap(faded, QIcon::Disabled);
	}

	return icon;
}

QIcon transportIcon(TransportIcon kind, const QColor &color)
{
	QIcon icon;

	for (const qreal dpr : {1.0, 2.0}) {
		for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled}) {
			QPixmap pixmap(QSize(16, 16) * dpr);
			pixmap.setDevicePixelRatio(dpr);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing);
			painter.setOpacity(mode == QIcon::Disabled ? 0.35 : 1.0);
			painter.setPen(Qt::NoPen);
			painter.setBrush(color);

			switch (kind) {
			case TransportIcon::Play: {
				QPainterPath play;
				play.moveTo(4.5, 3);
				play.lineTo(13, 8);
				play.lineTo(4.5, 13);
				play.closeSubpath();
				painter.drawPath(play);
				break;
			}
			case TransportIcon::Pause:
				painter.drawRoundedRect(QRectF(4, 3, 3, 10), 0.8, 0.8);
				painter.drawRoundedRect(QRectF(9, 3, 3, 10), 0.8, 0.8);
				break;
			case TransportIcon::Stop:
				painter.drawRoundedRect(QRectF(4, 4, 8, 8), 1.2, 1.2);
				break;
			}
			painter.end();
			icon.addPixmap(pixmap, mode);
		}
	}

	return icon;
}

void paintEye(QPainter &painter, const QRectF &rect, bool open, const QColor &color)
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(color, 1.2));
	painter.setBrush(Qt::NoBrush);

	const qreal w = std::min(rect.width(), 14.0);
	const qreal h = w * 0.62;
	const QPointF c = rect.center();

	QPainterPath lid;
	lid.moveTo(c.x() - w / 2, c.y());
	lid.quadTo(c.x(), c.y() - h, c.x() + w / 2, c.y());
	lid.quadTo(c.x(), c.y() + h, c.x() - w / 2, c.y());
	painter.drawPath(lid);

	if (open) {
		painter.setBrush(color);
		painter.drawEllipse(c, w * 0.14, w * 0.14);
	} else {
		painter.drawLine(QPointF(c.x() - w * 0.45, c.y() + h * 0.55),
				 QPointF(c.x() + w * 0.45, c.y() - h * 0.55));
	}

	painter.restore();
}

void styleAsHint(QWidget *widget)
{
	QPalette palette = widget->palette();
	QColor text = palette.color(QPalette::WindowText);
	text.setAlphaF(0.62f);
	palette.setColor(QPalette::WindowText, text);
	palette.setColor(QPalette::Text, text);
	widget->setPalette(palette);
}

void applyReadablePlaceholder(QWidget *field)
{
	QColor placeholder = QApplication::palette(field).color(QPalette::Active, QPalette::Text);
	placeholder.setAlphaF(0.55f);

	/*
	 * Set on the field itself, after it has been polished: under a stylesheet -- the designer's,
	 * or an OBS theme's -- polishing rebuilds a widget's palette and drops anything it inherited.
	 */
	QPalette palette = field->palette();
	for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
		palette.setColor(group, QPalette::PlaceholderText, placeholder);
	field->setPalette(palette);
}

void applyReadablePlaceholders(QWidget *root)
{
	const auto apply = [](QWidget *field) {
		field->ensurePolished();
		applyReadablePlaceholder(field);
	};

	for (QLineEdit *field : root->findChildren<QLineEdit *>())
		apply(field);
	for (QPlainTextEdit *field : root->findChildren<QPlainTextEdit *>())
		apply(field);
	for (QTextEdit *field : root->findChildren<QTextEdit *>())
		apply(field);
}

/* --- remembered view state ------------------------------------------------------------------- */

QVariant designerUiValue(const QString &key, const QVariant &fallback)
{
	return uiSettings().value(key, fallback);
}

void setDesignerUiValue(const QString &key, const QVariant &value)
{
	uiSettings().setValue(key, value);
}

/* --- ToggleSwitch ----------------------------------------------------------------------------- */

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFocusPolicy(Qt::StrongFocus);
	setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize ToggleSwitch::sizeHint() const
{
	return QSize(34, 18);
}

void ToggleSwitch::enterEvent(QEnterEvent *event)
{
	hovered = true;
	update();
	QAbstractButton::enterEvent(event);
}

void ToggleSwitch::leaveEvent(QEvent *event)
{
	hovered = false;
	update();
	QAbstractButton::leaveEvent(event);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QRectF track = QRectF(rect()).adjusted(1, 1, -1, -1);
	const qreal radius = track.height() / 2;

	QColor on = palette().color(QPalette::Highlight);
	QColor off = palette().color(QPalette::Mid);
	if (!isEnabled()) {
		on.setAlpha(110);
		off.setAlpha(110);
	} else if (hovered) {
		on = on.lighter(112);
		off = off.lighter(118);
	}

	painter.setPen(Qt::NoPen);
	painter.setBrush(isChecked() ? on : off);
	painter.drawRoundedRect(track, radius, radius);

	const qreal knob = track.height() - 4;
	const qreal x = isChecked() ? track.right() - 2 - knob : track.left() + 2;
	painter.setBrush(Qt::white);
	painter.drawEllipse(QRectF(x, track.top() + 2, knob, knob));

	if (hasFocus()) {
		painter.setBrush(Qt::NoBrush);
		painter.setPen(QPen(palette().color(QPalette::Highlight).lighter(140), 1));
		painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius + 1, radius + 1);
	}
}

/* --- chips ------------------------------------------------------------------------------------ */

QToolButton *makeChip(const QString &text, const QString &tooltip, QWidget *parent)
{
	auto *chip = new QToolButton(parent);
	chip->setObjectName(QStringLiteral("designerChip"));
	chip->setCheckable(true);
	chip->setText(text);
	chip->setToolTip(tooltip);
	chip->setAccessibleName(text);
	chip->setCursor(Qt::PointingHandCursor);
	chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
	return chip;
}

/* --- SegmentedControl ------------------------------------------------------------------------- */

SegmentedControl::SegmentedControl(QWidget *parent) : QFrame(parent)
{
	setObjectName(QStringLiteral("designerSegmented"));
	row = new QHBoxLayout(this);
	row->setContentsMargins(2, 2, 2, 2);
	row->setSpacing(2);
	group = new QButtonGroup(this);
	group->setExclusive(true);
	setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

	connect(group, &QButtonGroup::idClicked, this, [this](int id) {
		if (bound && bound->currentIndex() != id)
			bound->setCurrentIndex(id);
		emit currentIndexChanged(id);
	});
}

int SegmentedControl::addSegment(const QString &text, const QString &tooltip)
{
	auto *button = new QToolButton(this);
	button->setObjectName(QStringLiteral("designerSegment"));
	button->setCheckable(true);
	button->setText(text);
	button->setToolTip(tooltip);
	button->setCursor(Qt::PointingHandCursor);
	button->setToolButtonStyle(Qt::ToolButtonTextOnly);

	const int id = group->buttons().size();
	group->addButton(button, id);
	row->addWidget(button);

	if (id == 0)
		button->setChecked(true);
	return id;
}

void SegmentedControl::bindTo(QComboBox *combo)
{
	bound = combo;

	for (int index = 0; index < combo->count(); ++index)
		addSegment(combo->itemText(index), combo->itemData(index, Qt::ToolTipRole).toString());

	setCurrentIndex(combo->currentIndex());
	connect(combo, &QComboBox::currentIndexChanged, this, &SegmentedControl::setCurrentIndex);
}

void SegmentedControl::setCurrentIndex(int index)
{
	if (QAbstractButton *button = group->button(index)) {
		const QSignalBlocker blocker(group);
		button->setChecked(true);
	}
}

int SegmentedControl::currentIndex() const
{
	return group->checkedId();
}

/* --- UndoToast -------------------------------------------------------------------------------- */

UndoToast::UndoToast(QWidget *window) : QFrame(window)
{
	setObjectName(QStringLiteral("designerToast"));
	setAttribute(Qt::WA_StyledBackground);

	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(14, 6, 6, 6);
	layout->setSpacing(12);

	label = new QLabel(this);
	layout->addWidget(label);

	undoButton = new QToolButton(this);
	undoButton->setObjectName(QStringLiteral("designerToastUndo"));
	undoButton->setText(moduleText("Designer.Undo"));
	undoButton->setAutoRaise(true);
	undoButton->setCursor(Qt::PointingHandCursor);
	layout->addWidget(undoButton);

	auto *close = new QToolButton(this);
	close->setText(QStringLiteral("✕"));
	close->setAutoRaise(true);
	close->setToolTip(moduleText("Designer.Toast.Dismiss"));
	layout->addWidget(close);

	hideTimer = new QTimer(this);
	hideTimer->setSingleShot(true);
	hideTimer->setInterval(kToastMs);

	connect(hideTimer, &QTimer::timeout, this, &QWidget::hide);
	connect(close, &QToolButton::clicked, this, &QWidget::hide);
	connect(undoButton, &QToolButton::clicked, this, [this] {
		hide();
		/* Taken first: the undo may well show another toast, which would replace it under us. */
		const std::function<void()> action = std::move(undoAction);
		undoAction = nullptr;
		if (action)
			action();
	});

	window->installEventFilter(this);
	hide();
}

void UndoToast::showMessage(const QString &message, std::function<void()> undo)
{
	label->setText(message);
	undoAction = std::move(undo);
	undoButton->setVisible(static_cast<bool>(undoAction));

	adjustSize();
	place();
	show();
	raise();
	hideTimer->start();
}

bool UndoToast::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible())
		place();
	return QFrame::eventFilter(watched, event);
}

void UndoToast::place()
{
	const QWidget *window = parentWidget();
	if (!window)
		return;

	adjustSize();
	move((window->width() - width()) / 2, window->height() - height() - 18);
}

/* --- SpacingDiagram --------------------------------------------------------------------------- */

namespace {

QSpinBox *makeEdgeBox(QWidget *parent, int minimum, int maximum)
{
	auto *box = new QSpinBox(parent);
	box->setRange(minimum, maximum);
	box->setSuffix(QStringLiteral(" px"));
	box->setAlignment(Qt::AlignCenter);
	box->setButtonSymbols(QAbstractSpinBox::NoButtons);
	box->setFixedWidth(72);
	return box;
}

QLabel *makeEdgeLabel(const QString &text, QWidget *parent)
{
	auto *label = new QLabel(text, parent);
	styleAsHint(label);
	label->setAlignment(Qt::AlignCenter);
	return label;
}

/* The gap kept around the inner box for the edge controls, in pixels. */
constexpr int kDiagramSideRoom = 92;
constexpr int kDiagramTopRoom = 34;

} // namespace

SpacingDiagram::SpacingDiagram(QWidget *parent) : QWidget(parent)
{
	aboveBox = makeEdgeBox(this, 0, 20000);
	belowBox = makeEdgeBox(this, 0, 20000);
	sidesBox = makeEdgeBox(this, 0, 4096);
	offsetBox = makeEdgeBox(this, -4096, 4096);

	aboveLabel = makeEdgeLabel(moduleText("Designer.Spacing.Above"), this);
	belowLabel = makeEdgeLabel(moduleText("Designer.Spacing.Below"), this);
	sidesLabel = makeEdgeLabel(moduleText("Designer.Spacing.Sides"), this);
	offsetLabel = makeEdgeLabel(moduleText("Designer.Spacing.Offset"), this);

	aboveBox->setToolTip(moduleText("Designer.PaddingTop"));
	belowBox->setToolTip(moduleText("Designer.PaddingBottom"));
	sidesBox->setToolTip(moduleText("Designer.MarginX"));
	offsetBox->setToolTip(moduleText("Designer.ContentOffsetX.Tip"));

	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize SpacingDiagram::sizeHint() const
{
	return QSize(460, 168);
}

QSize SpacingDiagram::minimumSizeHint() const
{
	return QSize(340, 168);
}

void SpacingDiagram::setEdgesVisible(bool vertical, bool sides, bool offset)
{
	aboveBox->setVisible(vertical);
	aboveLabel->setVisible(vertical);
	belowBox->setVisible(vertical);
	belowLabel->setVisible(vertical);
	sidesBox->setVisible(sides);
	sidesLabel->setVisible(sides);
	offsetBox->setVisible(offset);
	offsetLabel->setVisible(offset);
	update();
}

void SpacingDiagram::setContentText(const QString &text)
{
	contentText = text;
	update();
}

QRect SpacingDiagram::contentRect() const
{
	return rect().adjusted(kDiagramSideRoom, kDiagramTopRoom + 14, -kDiagramSideRoom, -kDiagramTopRoom - 14);
}

void SpacingDiagram::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	place();
}

void SpacingDiagram::place()
{
	const QRect inner = contentRect();
	const int boxHeight = aboveBox->sizeHint().height();
	const int labelHeight = aboveLabel->sizeHint().height();

	const auto placeTop = [&](QLabel *label, QSpinBox *box, int y) {
		const int labelWidth = label->sizeHint().width();
		const int total = labelWidth + 6 + box->width();
		const int x = inner.center().x() - total / 2;
		label->setGeometry(x, y + (boxHeight - labelHeight) / 2, labelWidth, labelHeight);
		box->move(x + labelWidth + 6, y);
	};
	placeTop(aboveLabel, aboveBox, inner.top() - boxHeight - 8);
	placeTop(belowLabel, belowBox, inner.bottom() + 8);

	const auto placeSide = [&](QLabel *label, QSpinBox *box, int centerX) {
		const int y = inner.center().y() - (labelHeight + 4 + boxHeight) / 2;
		label->setGeometry(centerX - box->width() / 2, y, box->width(), labelHeight);
		box->move(centerX - box->width() / 2, y + labelHeight + 4);
	};
	placeSide(sidesLabel, sidesBox, kDiagramSideRoom / 2);
	placeSide(offsetLabel, offsetBox, width() - kDiagramSideRoom / 2);
}

void SpacingDiagram::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	QColor line = palette().color(QPalette::Mid);
	QPen dashed(line, 1);
	dashed.setStyle(Qt::DashLine);
	painter.setPen(dashed);
	painter.setBrush(Qt::NoBrush);
	painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);

	const QRect inner = contentRect();
	QColor accent = palette().color(QPalette::Highlight);
	QColor fill = accent;
	fill.setAlpha(28);
	painter.setPen(QPen(accent, 1));
	painter.setBrush(fill);
	painter.drawRoundedRect(QRectF(inner).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);

	painter.setPen(palette().color(QPalette::Text));
	painter.drawText(inner, Qt::AlignCenter | Qt::TextWordWrap, contentText);
}

/* --- TimelineScrubber ------------------------------------------------------------------------ */

TimelineScrubber::TimelineScrubber(QWidget *parent) : QWidget(parent)
{
	setCursor(Qt::PointingHandCursor);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(moduleText("Designer.Preview.Scrub.Tip"));
}

QSize TimelineScrubber::sizeHint() const
{
	return QSize(160, 20);
}

void TimelineScrubber::setDuration(double seconds)
{
	length = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
	update();
}

void TimelineScrubber::setPosition(double seconds)
{
	at = seconds;
	update();
}

void TimelineScrubber::setMarkers(const QVector<double> &seconds)
{
	markers = seconds;
	update();
}

QRectF TimelineScrubber::track() const
{
	return QRectF(7, height() / 2.0 - 2, std::max(1, width() - 14), 4);
}

void TimelineScrubber::seekTo(qreal x)
{
	if (length <= 0.0)
		return;

	const QRectF bar = track();
	const double fraction = std::clamp((x - bar.left()) / bar.width(), 0.0, 1.0);
	at = fraction * length;
	update();
	emit seeked(at);
}

void TimelineScrubber::mousePressEvent(QMouseEvent *event)
{
	if (event->button() == Qt::LeftButton)
		seekTo(event->position().x());
}

void TimelineScrubber::mouseMoveEvent(QMouseEvent *event)
{
	if (event->buttons() & Qt::LeftButton)
		seekTo(event->position().x());
}

void TimelineScrubber::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QRectF bar = track();
	painter.setPen(Qt::NoPen);
	painter.setBrush(palette().color(QPalette::Mid));
	painter.drawRoundedRect(bar, 2, 2);

	const double fraction = length > 0.0 ? std::clamp(at / length, 0.0, 1.0) : 0.0;
	painter.setBrush(palette().color(QPalette::Highlight));
	painter.drawRoundedRect(QRectF(bar.left(), bar.top(), bar.width() * fraction, bar.height()), 2, 2);

	painter.setBrush(kAmber);
	for (const double marker : markers) {
		if (length <= 0.0 || !std::isfinite(marker) || marker > length)
			continue;
		const qreal x = bar.left() + bar.width() * (marker / length);
		painter.drawRect(QRectF(x - 1, bar.top() - 3, 2, bar.height() + 6));
	}

	painter.setBrush(Qt::white);
	painter.setPen(QPen(palette().color(QPalette::Mid), 1));
	painter.drawEllipse(QPointF(bar.left() + bar.width() * fraction, bar.center().y()), 6, 6);
}

QString formatClock(double seconds)
{
	if (!std::isfinite(seconds))
		return QStringLiteral("∞");

	const int whole = std::max(0, static_cast<int>(seconds));
	return QStringLiteral("%1:%2").arg(whole / 60).arg(whole % 60, 2, 10, QLatin1Char('0'));
}

/* --- stylesheet ------------------------------------------------------------------------------- */

QColor subtleFill(const QPalette &palette, qreal strength)
{
	const QColor base = palette.color(QPalette::Base);
	const QColor text = palette.color(QPalette::Text);
	const qreal t = std::clamp(strength, 0.0, 1.0);
	const auto mix = [t](int from, int to) {
		return static_cast<int>(std::lround(from + (to - from) * t));
	};
	return QColor(mix(base.red(), text.red()), mix(base.green(), text.green()), mix(base.blue(), text.blue()));
}

QString designerStyleSheet(const QPalette &palette)
{
	const QString hover = subtleFill(palette, 0.10).name();
	const QString selected = subtleFill(palette, 0.18).name();

	QString sheet = QStringLiteral(R"(
QToolButton#designerChip {
	border: 1px solid palette(mid);
	border-radius: 11px;
	padding: 3px 11px;
	background: transparent;
}
QToolButton#designerChip:hover { background: @HOVER@; color: palette(text); }
QToolButton#designerChip:checked {
	border-color: palette(highlight);
	background: rgba(90, 130, 240, 40);
	color: palette(text);
}
QFrame#designerSegmented {
	border: 1px solid palette(mid);
	border-radius: 7px;
	background: palette(base);
}
QToolButton#designerSegment {
	border: 1px solid transparent;
	border-radius: 5px;
	padding: 3px 12px;
	background: transparent;
}
QToolButton#designerSegment:hover { background: @HOVER@; color: palette(text); }
QToolButton#designerSegment:checked {
	background: @SELECTED@;
	border-color: palette(mid);
	color: palette(text);
}
QLabel#designerPill {
	border: 1px solid palette(mid);
	border-radius: 11px;
	padding: 2px 10px;
}
QToolButton#designerWarning {
	border: 1px solid rgba(240, 164, 58, 120);
	border-radius: 11px;
	padding: 2px 10px;
	color: rgb(240, 164, 58);
	background: rgba(240, 164, 58, 30);
}
QLabel#designerDirty { color: rgb(240, 164, 58); }
QLabel#designerPaneTitle { font-weight: 600; }
QLineEdit#designerSectionName {
	font-size: 15pt;
	font-weight: 600;
	border: 1px solid transparent;
	border-radius: 4px;
	background: transparent;
	padding: 1px 4px;
}
QLineEdit#designerSectionName:hover, QLineEdit#designerSectionName:focus {
	border-color: palette(mid);
	background: palette(base);
}
QToolButton#designerLink::menu-indicator, QToolButton#designerMenuButton::menu-indicator {
	image: none;
	width: 0px;
}
QToolButton#designerLink {
	border: none;
	background: transparent;
	color: palette(link);
	padding: 0px 2px;
}
QPushButton#designerStyleLink {
	text-align: left;
	border: 1px solid palette(mid);
	border-radius: 5px;
	padding: 5px 9px;
	background: palette(base);
}
QPushButton#designerStyleLink:hover { border-color: palette(highlight); }
QPushButton#designerPrimary {
	background: palette(highlight);
	color: palette(highlighted-text);
	border: 1px solid palette(highlight);
	border-radius: 4px;
	padding: 4px 16px;
	font-weight: 600;
}
QPushButton#designerEmptySlot {
	border: 1px dashed palette(mid);
	border-radius: 6px;
	padding: 9px 12px;
	text-align: left;
	background: transparent;
}
QPushButton#designerEmptySlot:hover { border-color: palette(highlight); }
QToolButton#designerAddSection {
	border: 1px solid rgba(90, 130, 240, 140);
	border-radius: 5px;
	padding: 5px 10px;
	background: rgba(90, 130, 240, 45);
	font-weight: 600;
}
QToolButton#designerAddSection:hover { background: rgba(90, 130, 240, 70); color: palette(text); }
QFrame#designerToast {
	background: rgb(47, 51, 62);
	border: 1px solid rgb(69, 74, 88);
	border-radius: 8px;
	color: rgb(230, 230, 235);
}
QFrame#designerToast QLabel { color: rgb(230, 230, 235); }
QToolButton#designerToastUndo { color: rgb(185, 203, 255); font-weight: 600; }
QTabWidget#designerEditorTabs::pane { border: none; border-top: 1px solid palette(mid); }
QTabBar#designerEditorTabBar::tab {
	border: 1px solid transparent;
	border-radius: 5px;
	padding: 4px 14px;
	margin: 0px 2px 6px 0px;
	background: transparent;
	color: palette(text);
}
QTabBar#designerEditorTabBar::tab:hover:!selected { background: @HOVER@; color: palette(text); }
QTabBar#designerEditorTabBar::tab:selected {
	background: @SELECTED@;
	border-color: palette(mid);
	color: palette(text);
}
)");
	sheet.replace(QStringLiteral("@HOVER@"), hover);
	sheet.replace(QStringLiteral("@SELECTED@"), selected);
	return sheet;
}

} // namespace closingtime
