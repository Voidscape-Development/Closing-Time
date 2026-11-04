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

#include "ui/AddSectionGallery.hpp"

#include <obs-module.h>

#include <QEnterEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QScreen>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

#include "ui/DesignerWidgets.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* A locale string built from a type's id, or `fallback` when the locale has none. */
QString typeText(const char *prefix, SectionType type, const QString &fallback)
{
	const QString key = QString::fromLatin1(prefix) + QString::fromLatin1(sectionTypeId(type));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));
	return text == key ? fallback : text;
}

constexpr int kTileColumns = 3;
constexpr int kTileWidth = 168;
constexpr int kThumbHeight = 38;

struct GalleryCategory {
	const char *titleKey;
	QVector<SectionType> types;
};

const QVector<GalleryCategory> &galleryCategories()
{
	static const QVector<GalleryCategory> categories = {
		{"Designer.Gallery.Headings", {SectionType::Title, SectionType::Header, SectionType::LogoTitle}},
		{"Designer.Gallery.Lists",
		 {SectionType::TextList, SectionType::Bridged, SectionType::MultiTextList,
		  SectionType::TitleSubtitleList, SectionType::LogoList, SectionType::MultiLogoList}},
		{"Designer.Gallery.Other",
		 {SectionType::SectionDivider, SectionType::Spacer, SectionType::StickyBlock, SectionType::Action}},
	};
	return categories;
}

} // namespace

/* --- GalleryTile ------------------------------------------------------------------------------ */

GalleryTile::GalleryTile(SectionType type, const QString &tileName, const QString &tileDescription, QWidget *parent)
	: QAbstractButton(parent),
	  sectionType(type),
	  name(tileName),
	  description(tileDescription)
{
	setFocusPolicy(Qt::StrongFocus);
	setCursor(Qt::PointingHandCursor);
	setToolTip(description);
	setAccessibleName(name);
	setAccessibleDescription(description);
}

bool GalleryTile::matches(const QString &needle) const
{
	return needle.isEmpty() || name.contains(needle, Qt::CaseInsensitive) ||
	       description.contains(needle, Qt::CaseInsensitive);
}

QSize GalleryTile::sizeHint() const
{
	const QFontMetrics metrics(font());
	return QSize(kTileWidth, 8 + kThumbHeight + 6 + metrics.height() + 2 + metrics.height() * 2 + 8);
}

void GalleryTile::enterEvent(QEnterEvent *event)
{
	hovered = true;
	update();
	QAbstractButton::enterEvent(event);
}

void GalleryTile::leaveEvent(QEvent *event)
{
	hovered = false;
	update();
	QAbstractButton::leaveEvent(event);
}

void GalleryTile::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QRectF frame = QRectF(rect()).adjusted(1, 1, -1, -1);
	const bool focused = hasFocus();
	QColor border = focused ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid);
	QColor fill = palette().color(QPalette::Base);
	if (hovered && !focused)
		fill = palette().color(QPalette::AlternateBase);

	painter.setPen(QPen(border, focused ? 1.6 : 1.0));
	painter.setBrush(fill);
	painter.drawRoundedRect(frame, 6, 6);

	const QRectF thumb(frame.left() + 7, frame.top() + 7, frame.width() - 14, kThumbHeight);
	paintTypeThumbnail(painter, thumb, sectionType);

	QFont bold = font();
	bold.setBold(true);
	const QFontMetrics boldMetrics(bold);
	const qreal nameTop = thumb.bottom() + 6;

	painter.setFont(bold);
	painter.setPen(palette().color(QPalette::Text));
	painter.drawText(QRectF(thumb.left(), nameTop, thumb.width(), boldMetrics.height()),
			 Qt::AlignLeft | Qt::AlignVCenter, name);

	QColor dim = palette().color(QPalette::Text);
	dim.setAlphaF(0.62);
	painter.setFont(font());
	painter.setPen(dim);
	painter.drawText(QRectF(thumb.left(), nameTop + boldMetrics.height() + 2, thumb.width(),
				frame.bottom() - nameTop - boldMetrics.height() - 6),
			 Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, description);
}

/* --- AddSectionGallery ------------------------------------------------------------------------ */

AddSectionGallery::AddSectionGallery(QWidget *parent) : QFrame(parent, Qt::Popup)
{
	setFrameShape(QFrame::StyledPanel);
	setAttribute(Qt::WA_DeleteOnClose, false);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(12, 12, 12, 10);
	layout->setSpacing(6);

	search = new QLineEdit(this);
	search->setPlaceholderText(moduleText("Designer.Gallery.Search"));
	search->setClearButtonEnabled(true);
	search->installEventFilter(this);
	layout->addWidget(search);

	hint = new QLabel(this);
	styleAsHint(hint);
	layout->addWidget(hint);

	for (const GalleryCategory &category : galleryCategories()) {
		auto *title = new QLabel(moduleText(category.titleKey).toUpper(), this);
		QFont font = title->font();
		font.setBold(true);
		font.setPointSizeF(std::max(7.0, font.pointSizeF() * 0.85));
		font.setLetterSpacing(QFont::AbsoluteSpacing, 1.0);
		title->setFont(font);
		styleAsHint(title);
		layout->addSpacing(4);
		layout->addWidget(title);

		auto *grid = new QGridLayout();
		grid->setSpacing(6);
		QVector<GalleryTile *> categoryTiles;

		for (int index = 0; index < category.types.size(); ++index) {
			const SectionType type = category.types.at(index);
			auto *tile = new GalleryTile(type,
						     typeText("Designer.Gallery.Name.", type,
							      QString::fromUtf8(sectionTypeName(type))),
						     typeText("Designer.Gallery.Desc.", type,
							      typeText("Designer.TypeHelp.", type, QString())),
						     this);
			tile->installEventFilter(this);
			grid->addWidget(tile, index / kTileColumns, index % kTileColumns);
			tiles.append(tile);
			categoryTiles.append(tile);

			connect(tile, &QAbstractButton::clicked, this, [this, type] {
				hide();
				emit typeChosen(type);
			});
		}

		layout->addLayout(grid);
		categories.append({title, categoryTiles});
	}

	auto *footer = new QLabel(moduleText("Designer.Gallery.Keys"), this);
	styleAsHint(footer);
	layout->addSpacing(4);
	layout->addWidget(footer);

	connect(search, &QLineEdit::textChanged, this, &AddSectionGallery::applyFilter);
}

void AddSectionGallery::popup(QWidget *anchor, const QString &insertHint)
{
	search->clear();
	applyFilter(QString());
	hint->setText(insertHint);
	hint->setVisible(!insertHint.isEmpty());

	adjustSize();

	QPoint at = anchor->mapToGlobal(QPoint(0, anchor->height() + 2));
	if (const QScreen *screen = anchor->screen()) {
		const QRect available = screen->availableGeometry();
		/* Flipped above the button when there is no room under it, which there rarely is at the foot of a list. */
		if (at.y() + height() > available.bottom())
			at.setY(anchor->mapToGlobal(QPoint(0, 0)).y() - height() - 2);
		at.setX(std::clamp(at.x(), available.left(), std::max(available.left(), available.right() - width())));
		at.setY(std::max(at.y(), available.top()));
	}

	move(at);
	show();
	search->setFocus();
}

void AddSectionGallery::applyFilter(const QString &text)
{
	const QString needle = text.trimmed();

	for (const auto &[title, categoryTiles] : categories) {
		bool any = false;
		for (GalleryTile *tile : categoryTiles) {
			const bool show = tile->matches(needle);
			tile->setVisible(show);
			any = any || show;
		}
		title->setVisible(any);
	}

	adjustSize();
}

QVector<GalleryTile *> AddSectionGallery::visibleTiles() const
{
	QVector<GalleryTile *> shown;
	for (GalleryTile *tile : tiles) {
		if (!tile->isHidden())
			shown.append(tile);
	}
	return shown;
}

void AddSectionGallery::moveFocus(GalleryTile *from, int dx, int dy)
{
	const QVector<GalleryTile *> shown = visibleTiles();
	if (shown.isEmpty())
		return;

	/*
	 * By position on screen rather than by index, because the categories hold different numbers
	 * of tiles: the tile "below" is the nearest one in the next row down, wherever that row starts.
	 */
	const QPoint origin = from->mapTo(this, from->rect().center());
	GalleryTile *best = nullptr;
	int bestScore = std::numeric_limits<int>::max();

	for (GalleryTile *tile : shown) {
		if (tile == from)
			continue;
		const QPoint at = tile->mapTo(this, tile->rect().center());
		const int ddx = at.x() - origin.x();
		const int ddy = at.y() - origin.y();

		if (dx != 0 && (ddx * dx <= 0 || std::abs(ddy) > from->height() / 2))
			continue;
		if (dy != 0 && ddy * dy <= 0)
			continue;

		const int score = dx != 0 ? std::abs(ddx) : std::abs(ddy) * 4 + std::abs(ddx);
		if (score < bestScore) {
			bestScore = score;
			best = tile;
		}
	}

	if (best)
		best->setFocus();
	else if (dy < 0)
		search->setFocus();
}

bool AddSectionGallery::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() != QEvent::KeyPress)
		return QFrame::eventFilter(watched, event);

	auto *key = static_cast<QKeyEvent *>(event);

	if (watched == search) {
		switch (key->key()) {
		case Qt::Key_Down:
		case Qt::Key_Tab:
			if (const QVector<GalleryTile *> shown = visibleTiles(); !shown.isEmpty())
				shown.first()->setFocus();
			return true;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			/* Enter in the search box adds the best match, which is the first one still showing. */
			if (const QVector<GalleryTile *> shown = visibleTiles(); !shown.isEmpty())
				shown.first()->click();
			return true;
		default:
			return QFrame::eventFilter(watched, event);
		}
	}

	auto *tile = qobject_cast<GalleryTile *>(watched);
	if (!tile)
		return QFrame::eventFilter(watched, event);

	switch (key->key()) {
	case Qt::Key_Left:
		moveFocus(tile, -1, 0);
		return true;
	case Qt::Key_Right:
		moveFocus(tile, 1, 0);
		return true;
	case Qt::Key_Up:
		moveFocus(tile, 0, -1);
		return true;
	case Qt::Key_Down:
		moveFocus(tile, 0, 1);
		return true;
	case Qt::Key_Return:
	case Qt::Key_Enter:
		tile->click();
		return true;
	default:
		break;
	}

	/* Typing anywhere in the gallery goes to the search box, the way a picker's search usually works. */
	if (!key->text().isEmpty() && key->text().at(0).isPrint() && key->key() != Qt::Key_Space) {
		search->setFocus();
		search->insert(key->text());
		return true;
	}

	return QFrame::eventFilter(watched, event);
}

} // namespace closingtime
