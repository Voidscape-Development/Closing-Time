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

	/*
	 * Rebinds the preset picker. `selected` is dropped when no preset carries that name,
	 * which is what makes deleting a preset unbind the editors that were showing it.
	 * `applySelectedStyle` writes the bound preset's values back into the fields; the
	 * editor a preset edit originated from passes false, because it already shows them.
	 */
	void setPresets(const QVector<StylePreset> &presets, const QString &selected, bool applySelectedStyle = true);

	/* Empty when the style is the section's own rather than a preset. */
	QString presetName() const { return selectedPreset; }

signals:
	void changed();
	/*
	 * Raised both by "save as preset" and by any field edit made while a preset is bound
	 * -- editing a bound style is how a preset, and with it every section following it,
	 * gets restyled. The owner writes the preset into the document and calls back through
	 * setPresets().
	 */
	void presetSaveRequested(const QString &name, const TextStyle &style);
	void presetDeleteRequested(const QString &name);

private:
	void pickColour();
	void refreshColourButton();

	void writeFields(const TextStyle &style);
	void onPresetSelected();
	void applySelectedPreset(bool applySelectedStyle);
	void savePreset();
	void deletePreset();

	/* Emits presetSaveRequested when a preset is bound and changed() when one is not. */
	void notifyEdited();

	QComboBox *presetBox = nullptr;
	QPushButton *savePresetButton = nullptr;
	QPushButton *deletePresetButton = nullptr;

	QFontComboBox *family = nullptr;
	QSpinBox *pixelSize = nullptr;
	QCheckBox *bold = nullptr;
	QCheckBox *italic = nullptr;
	QPushButton *colourButton = nullptr;
	QComboBox *alignment = nullptr;
	QDoubleSpinBox *lineSpacing = nullptr;

	QVector<StylePreset> presets;
	QString selectedPreset;

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

	/* Republishes the document's preset list into both style editors. */
	void setPresets(const QVector<StylePreset> &presets);

signals:
	/* Emitted whenever the edited section changes in a way that affects the render. */
	void changed();

	/* Forwarded from whichever StyleEditor raised them; see StyleEditor. */
	void presetSaveRequested(const QString &name, const TextStyle &style);
	void presetDeleteRequested(const QString &name);

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
	QComboBox *logoPlacement = nullptr;
	QComboBox *logoSide = nullptr;
	QSpinBox *logoGap = nullptr;

	QLineEdit *bridgeEdit = nullptr;
	QComboBox *bridgeFill = nullptr;
	QComboBox *bridgeSizing = nullptr;
	QSpinBox *bridgeSplit = nullptr;
	QComboBox *bridgeRowAlign = nullptr;
	QCheckBox *bridgeSpanEmpty = nullptr;
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

	QVector<StylePreset> presets;
	/*
	 * Set only for the duration of a forwarded preset signal, so the round trip back
	 * through setPresets() does not rewrite the fields of the editor being typed into.
	 */
	StyleEditor *presetOrigin = nullptr;

	Section current;
	bool loading = false;
};

} // namespace closingtime
