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
class QFormLayout;
class QGroupBox;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace closingtime {

/*
 * Everything one BackgroundPanel carries: what it is filled with, how far it reaches, what its
 * corners do and what runs around its edge -- plus the preset it follows, on exactly the terms a
 * StyleEditor offers one.
 *
 * A widget of its own rather than rows inside the section editor, because a section has eight
 * slots and every one of them wants the same eleven controls. Written once here, the editor puts
 * up as many of them as the section's type has a use for and hides the rest, which is the same
 * bargain every other row in that editor strikes.
 *
 * Two of the settings are offered as one number with a way to have four. A panel with all four
 * corners alike is what nearly every design wants and a row of four spin boxes to say so is four
 * times the question; the same is true of the outset. So each shows a single figure until the
 * reader asks for the sides to differ, at which point the four appear carrying what the one was --
 * see BackgroundPanel::hasUniformRadius, which is what decides how a loaded panel comes up.
 */
class BackgroundEditor : public QWidget {
	Q_OBJECT

public:
	explicit BackgroundEditor(QWidget *parent = nullptr);

	void setPanel(const BackgroundPanel &panel);
	BackgroundPanel panel() const;

	/*
	 * Rebinds the preset picker. `selected` is dropped when no preset carries that name, which is
	 * what makes deleting a preset unbind the editors that were showing it. `applySelectedPanel`
	 * writes the bound preset's values into the fields; the editor an edit originated from passes
	 * false, because it already shows them.
	 */
	void setPresets(const QVector<BackgroundPreset> &presets, const QString &selected,
			bool applySelectedPanel = true);

	/* Empty when the panel is the section's own rather than a preset. */
	QString presetName() const { return selectedPreset; }

signals:
	void changed();
	/*
	 * Raised by "save as preset" and by any field edit made while a preset is bound -- editing a
	 * bound panel is how a preset, and with it every slot following it, is restyled. The owner
	 * writes the preset into the document and calls back through setPresets().
	 */
	void presetSaveRequested(const QString &name, const BackgroundPanel &panel);
	void presetDeleteRequested(const QString &name);

private:
	void writeFields(const BackgroundPanel &panel);

	/* Shows the rows the selected fill has a use for and hides the rest. */
	void applyFillVisibility();
	void applyShapeVisibility();

	void onPresetSelected();
	void applySelectedPreset(bool applySelectedPanel);
	void savePreset();
	void deletePreset();
	void browseForImage();

	/* Emits presetSaveRequested when a preset is bound and changed() when one is not. */
	void notifyEdited();

	QFormLayout *form = nullptr;

	QComboBox *presetBox = nullptr;
	QPushButton *savePresetButton = nullptr;
	QPushButton *deletePresetButton = nullptr;

	QComboBox *fillBox = nullptr;
	ColorButton *colorButton = nullptr;
	GradientEditor *gradientEditor = nullptr;

	QWidget *imageRow = nullptr;
	QLineEdit *imagePath = nullptr;
	QComboBox *imageFit = nullptr;

	QSpinBox *opacity = nullptr;

	QCheckBox *outsetPerSide = nullptr;
	QSpinBox *outsetAll = nullptr;
	QSpinBox *outsetLeft = nullptr;
	QSpinBox *outsetTop = nullptr;
	QSpinBox *outsetRight = nullptr;
	QSpinBox *outsetBottom = nullptr;

	QCheckBox *radiusPerCorner = nullptr;
	QSpinBox *radiusAll = nullptr;
	QSpinBox *radiusTopLeft = nullptr;
	QSpinBox *radiusTopRight = nullptr;
	QSpinBox *radiusBottomRight = nullptr;
	QSpinBox *radiusBottomLeft = nullptr;

	QGroupBox *borderGroup = nullptr;
	QDoubleSpinBox *borderWidth = nullptr;
	ColorButton *borderColor = nullptr;

	QVector<BackgroundPreset> presets;
	QString selectedPreset;

	/* Kept beside the widgets so a gradient's stops survive a trip through None. */
	GradientSpec gradient;

	bool loading = false;
};

} // namespace closingtime
