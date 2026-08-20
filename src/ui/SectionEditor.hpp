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
#include "ui/StyleControls.hpp"

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
class QVBoxLayout;

namespace closingtime {

/*
 * Everything one TextStyle carries: font, colour and alignment, plus how the glyphs are
 * filled and what is drawn around them.
 */
class StyleEditor : public QWidget {
	Q_OBJECT

public:
	explicit StyleEditor(QWidget *parent = nullptr);

	void setStyle(const TextStyle &style);
	TextStyle style() const;

	/*
	 * Hides the rows a bridge has no say over -- font, size, weight, alignment, line spacing --
	 * leaving the fill, outline and shadow its ink is made of. A bridge is laid out in the row's
	 * own font whatever this editor holds (see Document::effectiveBridgeStyle), so showing those
	 * rows would offer settings that do nothing.
	 *
	 * What they hold is still carried straight through to style(), rather than read back off the
	 * hidden widgets, so a preset saved or edited from here keeps the font it already had
	 * instead of picking up whatever a hidden font box happened to resolve the family to.
	 */
	void setInkOnly(bool inkOnly);

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
	void writeFields(const TextStyle &style);
	void onPresetSelected();
	void applySelectedPreset(bool applySelectedStyle);
	void savePreset();
	void deletePreset();

	/* Shows the fill rows that apply to the selected fill and hides the rest. */
	void applyFillVisibility();
	void onFillChanged();

	/* Emits presetSaveRequested when a preset is bound and changed() when one is not. */
	void notifyEdited();

	QFormLayout *form = nullptr;

	QComboBox *presetBox = nullptr;
	QPushButton *savePresetButton = nullptr;
	QPushButton *deletePresetButton = nullptr;

	QFontComboBox *family = nullptr;
	QSpinBox *pixelSize = nullptr;
	QCheckBox *bold = nullptr;
	QCheckBox *italic = nullptr;
	ColourButton *colourButton = nullptr;
	QComboBox *alignment = nullptr;
	QDoubleSpinBox *lineSpacing = nullptr;

	QComboBox *fillBox = nullptr;
	GradientEditor *gradientEditor = nullptr;

	QGroupBox *outlineGroup = nullptr;
	QDoubleSpinBox *outlineWidth = nullptr;
	ColourButton *outlineColour = nullptr;

	QGroupBox *shadowGroup = nullptr;
	QSpinBox *shadowOffsetX = nullptr;
	QSpinBox *shadowOffsetY = nullptr;
	QSpinBox *shadowBlur = nullptr;
	ColourButton *shadowColour = nullptr;

	QVector<StylePreset> presets;
	QString selectedPreset;

	/* Kept alongside the widgets so a gradient's stops survive a trip through Solid. */
	GradientSpec gradient;

	/*
	 * The style the fields were last filled from. style() is built on top of it, which is what
	 * carries the rows an ink-only editor hides through unchanged.
	 */
	TextStyle loaded;
	bool inkOnly = false;

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

	/*
	 * The divider's centre stack. Unlike the entry table these go through the model rather than
	 * shuffling cells: the rows carry combo boxes, which QTableWidget::takeItem knows nothing
	 * about, and rebuilding from a reordered vector cannot leave a row's widgets behind while
	 * its text moves.
	 */
	void writeCentreToTable(const Section &source);
	void readCentreFromTable(Section *target) const;
	void addCentrePiece();
	void removeSelectedCentrePieces();
	void moveSelectedCentrePiece(int delta);
	void browseForCentreFile();
	/* Shows the fields that apply to the piece in `row` and hides the rest. */
	void applyCentreRowVisibility(int row);
	/* True when any piece of the centre stack draws its artwork from a file. */
	bool centreUsesFile() const;

	void addEntry();
	void removeSelectedEntries();
	void moveSelectedEntry(int delta);
	void importCsv();
	void browseForSectionLogo();
	void browseForEntryLogo();
	void browseForBridgeSvg();
	/* Points `target` at a file the user picks, for any of the divider's three artwork slots. */
	void browseForDividerSvg(QLineEdit *target);

	void emitChanged();

	QFormLayout *form = nullptr;

	QComboBox *typeBox = nullptr;
	QLineEdit *labelEdit = nullptr;
	QCheckBox *visibleBox = nullptr;
	QPlainTextEdit *textEdit = nullptr;
	/* The second line of a "... w/ Subtitle" heading; a list's subtitles live in the entry table. */
	QPlainTextEdit *subtitleEdit = nullptr;

	QLineEdit *logoPath = nullptr;
	QToolButton *logoBrowse = nullptr;
	QSpinBox *logoHeight = nullptr;
	QComboBox *logoPlacement = nullptr;
	QComboBox *logoSide = nullptr;
	QSpinBox *logoGap = nullptr;

	QComboBox *bridgeType = nullptr;
	QLineEdit *bridgeEdit = nullptr;
	QLineEdit *bridgeSvgPath = nullptr;
	QToolButton *bridgeSvgBrowse = nullptr;
	QSpinBox *bridgeThickness = nullptr;
	QSpinBox *bridgeOffset = nullptr;
	QSpinBox *bridgeGap = nullptr;
	QCheckBox *bridgeTint = nullptr;
	QComboBox *bridgeFill = nullptr;
	QComboBox *bridgeSizing = nullptr;
	QSpinBox *bridgeSplit = nullptr;
	QComboBox *bridgeRowAlign = nullptr;
	QCheckBox *bridgeSpanEmpty = nullptr;

	QComboBox *dividerCap = nullptr;
	QLineEdit *dividerCapSvgPath = nullptr;
	QCheckBox *dividerMirrorEnds = nullptr;
	QComboBox *dividerEndCap = nullptr;
	QLineEdit *dividerEndCapSvgPath = nullptr;
	QComboBox *dividerArm = nullptr;
	QLineEdit *dividerArmSvgPath = nullptr;
	QSpinBox *dividerThickness = nullptr;
	QSpinBox *dividerGap = nullptr;
	QSpinBox *dividerPieceGap = nullptr;
	QSpinBox *dividerRules = nullptr;
	QSpinBox *dividerRuleGap = nullptr;
	QSpinBox *dividerRuleInset = nullptr;
	QCheckBox *dividerTint = nullptr;

	QGroupBox *centreGroup = nullptr;
	QTableWidget *centreTable = nullptr;
	QToolButton *centreFileButton = nullptr;

	QSpinBox *columns = nullptr;
	QSpinBox *columnGap = nullptr;
	QComboBox *fillOrder = nullptr;
	QSpinBox *entryGap = nullptr;
	QSpinBox *subtitleGap = nullptr;
	QComboBox *subtitleOrder = nullptr;

	QSpinBox *paddingTop = nullptr;
	QSpinBox *paddingBottom = nullptr;
	QSpinBox *marginX = nullptr;
	QSpinBox *sectionWidth = nullptr;
	QComboBox *sectionAlign = nullptr;
	QSpinBox *spacerHeight = nullptr;

	StyleEditor *primaryStyle = nullptr;
	QGroupBox *secondaryGroup = nullptr;
	StyleEditor *secondaryStyle = nullptr;
	/* Shown for the two shapes that draw a bridge, and only ever edits that bridge's ink. */
	QGroupBox *bridgeStyleGroup = nullptr;
	StyleEditor *bridgeStyle = nullptr;

	QGroupBox *entriesGroup = nullptr;
	QTableWidget *entryTable = nullptr;
	/* Hidden for the entry types that have no logo path to set. */
	QToolButton *setLogoButton = nullptr;

	/*
	 * Absorbs whatever height is left once the visible rows have been laid out. Without it a
	 * type carrying few fields has its rows spread down the pane by the leftover space rather
	 * than sitting one under the next.
	 */
	QVBoxLayout *outerLayout = nullptr;
	int trailingStretchIndex = -1;

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
