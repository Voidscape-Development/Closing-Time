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

#include <QDialog>
#include <QString>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;

namespace closingtime {

/*
 * Everything about a font that this dialog decides, which is everything a style records about one
 * except its size.
 *
 * Size is deliberately not here. A roll is laid out in video pixels and every other size in the
 * designer is a pixel spin box next to the thing it sizes; a point-size list in a font dialog is a
 * second, differently-scaled answer to a question already asked, and picking a face is hard enough
 * without it. The size row stays in the style editor where the rest of the measurements are.
 *
 * `styleName` is the face's own name and empty means the family's default face, exactly as in
 * TextStyle. `bold` and `italic` are kept in step with it, because they are what renders on a
 * machine that does not have the named face -- and, for a family that ships no bold or no italic
 * at all, they are what Qt synthesises one from.
 */
struct FontChoice {
	QString family = QStringLiteral("Sans Serif");
	QString styleName;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeOut = false;

	bool operator==(const FontChoice &other) const
	{
		return family == other.family && styleName == other.styleName && bold == other.bold &&
		       italic == other.italic && underline == other.underline && strikeOut == other.strikeOut;
	}
	bool operator!=(const FontChoice &other) const { return !(*this == other); }
};

/* One line naming a choice -- "DejaVu Sans · Bold" -- for the button that opens the picker. */
QString describeFontChoice(const FontChoice &choice);

/*
 * Picking a font by family and by face, rather than by family and two checkboxes.
 *
 * Laid out like the font dialog OBS opens for its own text sources, minus the size list: the
 * families on the left, the faces of the selected family beside them, the effects and a sample
 * underneath. What it is for is the half of a family that a weight and a slant cannot reach --
 * Semibold, Condensed, Book, Black -- which a bold checkbox has no way to ask for and which a
 * family with four weights has no way to answer.
 *
 * A family that does not ship a bold or an italic still offers one, marked as synthesised: it is
 * what the checkboxes did, Qt slants and thickens the letterforms itself, and dropping it would
 * take faux-bold away from every single-face family on the machine.
 */
class FontPickerDialog : public QDialog {
	Q_OBJECT

public:
	explicit FontPickerDialog(const FontChoice &initial, QWidget *parent = nullptr);

	/* What the lists are on now. Only meaningful once the dialog has been accepted. */
	FontChoice choice() const;

private:
	/*
	 * One row of the face list.
	 *
	 * A synthesised row names no face -- there is no file behind it -- and carries only the flags
	 * Qt fakes the face from, which is exactly what `styleName` being empty means in a style.
	 */
	struct Face {
		QString label;
		QString styleName;
		bool bold = false;
		bool italic = false;
	};

	void refillFamilies();
	void refillFaces();
	void updateSample();

	/* Selects `family` if the filtered list still holds it, and reports whether it did. */
	bool selectFamily(const QString &family);

	QString selectedFamily() const;

	QLineEdit *search = nullptr;
	QListWidget *familyList = nullptr;
	QListWidget *faceList = nullptr;
	QComboBox *writingSystem = nullptr;
	QCheckBox *underline = nullptr;
	QCheckBox *strikeOut = nullptr;
	QLabel *sample = nullptr;

	QVector<Face> faces;

	/*
	 * What the dialog opened on. It is the fallback for a family the machine no longer has --
	 * a roll designed elsewhere names one, and closing the picker must not silently rewrite the
	 * style to whatever happened to be at the top of the list.
	 */
	FontChoice initial;

	/* Set while the lists are being rebuilt, so a selection being restored is not read as a pick. */
	bool populating = false;
};

} // namespace closingtime
