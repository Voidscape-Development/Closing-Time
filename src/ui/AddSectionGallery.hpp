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

#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QVector>

#include "model/CreditsModel.hpp"

class QLabel;
class QLineEdit;

namespace closingtime {

/* One type in the gallery: a sketch of it, its name and a line on what it is for. */
class GalleryTile : public QAbstractButton {
	Q_OBJECT

public:
	GalleryTile(SectionType type, const QString &name, const QString &description, QWidget *parent = nullptr);

	SectionType type() const { return sectionType; }
	/* True when `needle` appears in the name or the description. */
	bool matches(const QString &needle) const;

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void enterEvent(QEnterEvent *event) override;
	void leaveEvent(QEvent *event) override;

private:
	SectionType sectionType;
	QString name;
	QString description;
	bool hovered = false;
};

/*
 * The add-section picker: every kind of section laid out as a small picture of itself, grouped by
 * what it is for, with a search box on top.
 *
 * It replaces a menu of twenty names. A name says what the type is called; a picture says what it
 * will look like on the roll, which is the question somebody adding a section is actually asking.
 * Arrow keys move between tiles, Enter adds, Escape closes, and typing filters.
 */
class AddSectionGallery : public QFrame {
	Q_OBJECT

public:
	explicit AddSectionGallery(QWidget *parent = nullptr);

	/* Opens under `anchor`, saying where the new section will go. */
	void popup(QWidget *anchor, const QString &insertHint);

signals:
	void typeChosen(SectionType type);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void applyFilter(const QString &text);
	QVector<GalleryTile *> visibleTiles() const;
	void moveFocus(GalleryTile *from, int dx, int dy);

	QLineEdit *search = nullptr;
	QLabel *hint = nullptr;
	QVector<GalleryTile *> tiles;
	/* Each category's heading and the tiles under it, so an emptied category hides its heading too. */
	QVector<QPair<QLabel *, QVector<GalleryTile *>>> categories;
};

} // namespace closingtime
