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

#include <QWidget>

#include "model/CreditsModel.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFontComboBox;
class QFormLayout;
class QGroupBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QToolButton;

namespace closingtime {

/* Font family, size, weight, colour and alignment for one TextStyle. */
class StyleEditor : public QWidget {
	Q_OBJECT

public:
	explicit StyleEditor(QWidget *parent = nullptr);

	void setStyle(const TextStyle &style);
	TextStyle style() const;

signals:
	void changed();

private:
	void pickColour();
	void refreshColourButton();

	QFontComboBox *family = nullptr;
	QSpinBox *pixelSize = nullptr;
	QCheckBox *bold = nullptr;
	QCheckBox *italic = nullptr;
	QPushButton *colourButton = nullptr;
	QComboBox *alignment = nullptr;
	QDoubleSpinBox *lineSpacing = nullptr;

	QColor colour = QColor(255, 255, 255);
	bool loading = false;
};

/*
 * Editor for a single section. One instance is reused for every section; the rows that do
 * not apply to the selected type are hidden rather than rebuilt, which keeps focus and
 * scroll position stable as the user clicks down the section list.
 */
class SectionEditor : public QWidget {
	Q_OBJECT

public:
	explicit SectionEditor(QWidget *parent = nullptr);

	void setSection(const Section &section);
	Section section() const;

signals:
	/* Emitted whenever the edited section changes in a way that affects the render. */
	void changed();

private:
	void applyTypeVisibility(SectionType type);
	void rebuildEntryTable(SectionType type);
	void readEntriesFromTable(Section *target) const;
	void writeEntriesToTable(const Section &source);

	void addEntry();
	void removeSelectedEntries();
	void moveSelectedEntry(int delta);
	void importCsv();
	void browseForSectionLogo();
	void browseForEntryLogo();

	void emitChanged();

	QFormLayout *form = nullptr;

	QComboBox *typeBox = nullptr;
	QLineEdit *labelEdit = nullptr;
	QCheckBox *visibleBox = nullptr;
	QPlainTextEdit *textEdit = nullptr;

	QLineEdit *logoPath = nullptr;
	QToolButton *logoBrowse = nullptr;
	QSpinBox *logoHeight = nullptr;
	QComboBox *logoSide = nullptr;
	QSpinBox *logoGap = nullptr;

	QLineEdit *bridgeEdit = nullptr;
	QSpinBox *columns = nullptr;
	QSpinBox *columnGap = nullptr;
	QComboBox *fillOrder = nullptr;
	QSpinBox *entryGap = nullptr;

	QSpinBox *paddingTop = nullptr;
	QSpinBox *paddingBottom = nullptr;
	QSpinBox *marginX = nullptr;
	QSpinBox *spacerHeight = nullptr;

	StyleEditor *primaryStyle = nullptr;
	QGroupBox *secondaryGroup = nullptr;
	StyleEditor *secondaryStyle = nullptr;

	QGroupBox *entriesGroup = nullptr;
	QTableWidget *entryTable = nullptr;

	Section current;
	bool loading = false;
};

} // namespace closingtime
