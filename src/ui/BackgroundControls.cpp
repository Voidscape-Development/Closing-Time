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

#include "ui/BackgroundControls.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>

#include <algorithm>
#include <initializer_list>

#include "render/AnimatedLogo.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/*
 * The gradient editor was written for a TextFill, and a panel's gradient is the same gradient
 * mapped the same way -- so the two fills that are gradients are handed across rather than the
 * editor learning a second enum it would only ever use for the one question it asks of it.
 */
TextFill gradientFillFor(BackgroundFill fill)
{
	return fill == BackgroundFill::RadialGradient ? TextFill::RadialGradient : TextFill::LinearGradient;
}

bool isGradient(BackgroundFill fill)
{
	return fill == BackgroundFill::LinearGradient || fill == BackgroundFill::RadialGradient;
}

/*
 * What a panel's image may be opened from.
 *
 * The patterns come from the render layer, because what can be opened is decided by what can be
 * decoded, and there is no sense offering a file the decoder would only refuse. An animated file is
 * offered and contributes its first frame -- a panel is baked into the strip and has no quad of its
 * own to animate in, which the tooltip on the image row says.
 */
QString imageFilter()
{
	const QString filter = moduleText("Designer.LogoFilter") + QStringLiteral(" (%1)").arg(imageLogoPatterns());
	return filter + QStringLiteral(";;%1 (*)").arg(moduleText("Designer.AllFilesFilter"));
}

/* One pixel spin box, of which this editor has ten. */
QSpinBox *pixelBox(QWidget *parent, int minimum, int maximum)
{
	auto *box = new QSpinBox(parent);
	box->setRange(minimum, maximum);
	box->setSuffix(QStringLiteral(" px"));
	return box;
}

} // namespace

BackgroundEditor::BackgroundEditor(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QFormLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	form = layout;

	auto *presetRow = new QWidget(this);
	auto *presetLayout = new QHBoxLayout(presetRow);
	presetLayout->setContentsMargins(0, 0, 0, 0);
	presetBox = new QComboBox(presetRow);
	savePresetButton = new QPushButton(moduleText("Designer.BackgroundPreset.Save"), presetRow);
	deletePresetButton = new QPushButton(moduleText("Designer.BackgroundPreset.Delete"), presetRow);
	presetLayout->addWidget(presetBox, 1);
	presetLayout->addWidget(savePresetButton);
	presetLayout->addWidget(deletePresetButton);
	layout->addRow(moduleText("Designer.BackgroundPreset"), presetRow);

	presetBox->addItem(moduleText("Designer.BackgroundPreset.None"), QString());
	deletePresetButton->setEnabled(false);

	fillBox = new QComboBox(this);
	for (const BackgroundFill fill : allBackgroundFills()) {
		fillBox->addItem(
			QString::fromUtf8(obs_module_text(QStringLiteral("Designer.Background.Fill.%1")
								  .arg(QString::fromUtf8(backgroundFillId(fill)))
								  .toUtf8()
								  .constData())),
			static_cast<int>(fill));
	}
	fillBox->setToolTip(moduleText("Designer.Background.Fill.Tip"));
	layout->addRow(moduleText("Designer.Background.Fill"), fillBox);

	colorButton = new ColorButton(this);
	colorButton->setDialogTitle(moduleText("Designer.Background.Color"));
	layout->addRow(moduleText("Designer.Background.Color"), colorButton);

	gradientEditor = new GradientEditor(this);
	layout->addRow(moduleText("Designer.Background.Gradient"), gradientEditor);

	imageRow = new QWidget(this);
	auto *imageLayout = new QHBoxLayout(imageRow);
	imageLayout->setContentsMargins(0, 0, 0, 0);
	imagePath = new QLineEdit(imageRow);
	imagePath->setPlaceholderText(moduleText("Designer.Background.Image.Placeholder"));
	auto *browse = new QPushButton(moduleText("Designer.Browse"), imageRow);
	imageLayout->addWidget(imagePath, 1);
	imageLayout->addWidget(browse);
	layout->addRow(moduleText("Designer.Background.Image"), imageRow);

	imageFit = new QComboBox(this);
	for (const BackgroundImageFit fit : allBackgroundImageFits()) {
		imageFit->addItem(
			QString::fromUtf8(obs_module_text(QStringLiteral("Designer.Background.Fit.%1")
								  .arg(QString::fromUtf8(backgroundImageFitId(fit)))
								  .toUtf8()
								  .constData())),
			static_cast<int>(fit));
	}
	imageFit->setToolTip(moduleText("Designer.Background.Fit.Tip"));
	layout->addRow(moduleText("Designer.Background.Fit"), imageFit);

	/* Shown as a percentage: nobody thinks of a card as being 0.65 present. */
	opacity = new QSpinBox(this);
	opacity->setRange(0, 100);
	opacity->setSuffix(QStringLiteral(" %"));
	opacity->setToolTip(moduleText("Designer.Background.Opacity.Tip"));
	layout->addRow(moduleText("Designer.Background.Opacity"), opacity);

	const int outsetLimit = static_cast<int>(kMaxBackgroundOutset);

	outsetAll = pixelBox(this, -outsetLimit, outsetLimit);
	outsetAll->setToolTip(moduleText("Designer.Background.Outset.Tip"));
	layout->addRow(moduleText("Designer.Background.Outset"), outsetAll);

	outsetPerSide = new QCheckBox(moduleText("Designer.Background.Outset.PerSide"), this);
	outsetPerSide->setToolTip(moduleText("Designer.Background.Outset.PerSide.Tip"));
	layout->addRow(QString(), outsetPerSide);

	auto *outsetRow = new QWidget(this);
	auto *outsetLayout = new QHBoxLayout(outsetRow);
	outsetLayout->setContentsMargins(0, 0, 0, 0);
	outsetLeft = pixelBox(outsetRow, -outsetLimit, outsetLimit);
	outsetTop = pixelBox(outsetRow, -outsetLimit, outsetLimit);
	outsetRight = pixelBox(outsetRow, -outsetLimit, outsetLimit);
	outsetBottom = pixelBox(outsetRow, -outsetLimit, outsetLimit);
	for (QSpinBox *box : {outsetLeft, outsetTop, outsetRight, outsetBottom})
		outsetLayout->addWidget(box);
	outsetRow->setToolTip(moduleText("Designer.Background.Outset.Sides.Tip"));
	layout->addRow(moduleText("Designer.Background.Outset.Sides"), outsetRow);

	const int radiusLimit = static_cast<int>(kMaxBackgroundRadius);

	radiusAll = pixelBox(this, 0, radiusLimit);
	radiusAll->setToolTip(moduleText("Designer.Background.Radius.Tip"));
	layout->addRow(moduleText("Designer.Background.Radius"), radiusAll);

	radiusPerCorner = new QCheckBox(moduleText("Designer.Background.Radius.PerCorner"), this);
	radiusPerCorner->setToolTip(moduleText("Designer.Background.Radius.PerCorner.Tip"));
	layout->addRow(QString(), radiusPerCorner);

	auto *radiusRow = new QWidget(this);
	auto *radiusLayout = new QHBoxLayout(radiusRow);
	radiusLayout->setContentsMargins(0, 0, 0, 0);
	radiusTopLeft = pixelBox(radiusRow, 0, radiusLimit);
	radiusTopRight = pixelBox(radiusRow, 0, radiusLimit);
	radiusBottomRight = pixelBox(radiusRow, 0, radiusLimit);
	radiusBottomLeft = pixelBox(radiusRow, 0, radiusLimit);
	for (QSpinBox *box : {radiusTopLeft, radiusTopRight, radiusBottomRight, radiusBottomLeft})
		radiusLayout->addWidget(box);
	radiusRow->setToolTip(moduleText("Designer.Background.Radius.Corners.Tip"));
	layout->addRow(moduleText("Designer.Background.Radius.Corners"), radiusRow);

	borderGroup = new QGroupBox(moduleText("Designer.Background.Border"), this);
	borderGroup->setCheckable(true);
	auto *borderForm = new QFormLayout(borderGroup);
	borderWidth = new QDoubleSpinBox(borderGroup);
	borderWidth->setRange(0.0, kMaxBackgroundBorder);
	borderWidth->setDecimals(1);
	borderWidth->setSingleStep(0.5);
	borderWidth->setSuffix(QStringLiteral(" px"));
	borderForm->addRow(moduleText("Designer.Background.BorderWidth"), borderWidth);
	borderColor = new ColorButton(borderGroup);
	borderColor->setDialogTitle(moduleText("Designer.Background.BorderColor"));
	borderForm->addRow(moduleText("Designer.Background.BorderColor"), borderColor);
	layout->addRow(borderGroup);

	const auto notify = [this] {
		notifyEdited();
	};

	connect(fillBox, &QComboBox::currentIndexChanged, this, [this] {
		applyFillVisibility();
		notifyEdited();
	});
	connect(imageFit, &QComboBox::currentIndexChanged, this, notify);
	connect(colorButton, &ColorButton::colorChanged, this, notify);
	connect(borderColor, &ColorButton::colorChanged, this, notify);
	connect(gradientEditor, &GradientEditor::changed, this, notify);
	connect(imagePath, &QLineEdit::textChanged, this, notify);
	connect(browse, &QPushButton::clicked, this, &BackgroundEditor::browseForImage);
	connect(borderGroup, &QGroupBox::toggled, this, notify);
	connect(borderWidth, &QDoubleSpinBox::valueChanged, this, notify);

	for (QSpinBox *box : {opacity, outsetAll, outsetLeft, outsetTop, outsetRight, outsetBottom, radiusAll,
			      radiusTopLeft, radiusTopRight, radiusBottomRight, radiusBottomLeft})
		connect(box, &QSpinBox::valueChanged, this, notify);

	for (QCheckBox *box : {outsetPerSide, radiusPerCorner}) {
		connect(box, &QCheckBox::toggled, this, [this] {
			applyShapeVisibility();
			notifyEdited();
		});
	}

	connect(presetBox, &QComboBox::currentIndexChanged, this, &BackgroundEditor::onPresetSelected);
	connect(savePresetButton, &QPushButton::clicked, this, &BackgroundEditor::savePreset);
	connect(deletePresetButton, &QPushButton::clicked, this, &BackgroundEditor::deletePreset);

	applyFillVisibility();
	applyShapeVisibility();
}

void BackgroundEditor::writeFields(const BackgroundPanel &panel)
{
	const bool wasLoading = loading;
	loading = true;

	fillBox->setCurrentIndex(std::max(0, fillBox->findData(static_cast<int>(panel.fill))));
	colorButton->setColor(panel.color);

	gradient = panel.gradient;
	gradientEditor->setGradient(gradient);
	gradientEditor->setFill(gradientFillFor(panel.fill));

	imagePath->setText(panel.imagePath);
	imageFit->setCurrentIndex(std::max(0, imageFit->findData(static_cast<int>(panel.imageFit))));

	opacity->setValue(qRound(std::clamp(panel.opacity, 0.0, 1.0) * 100.0));

	/*
	 * A panel whose sides differ comes up showing all four; one whose sides agree comes up
	 * showing the one. The single box carries the shared figure either way, so switching the four
	 * off collapses to something sensible rather than to zero.
	 */
	const bool sidesDiffer = !(qFuzzyCompare(panel.outsetLeft + 1.0, panel.outsetTop + 1.0) &&
				   qFuzzyCompare(panel.outsetLeft + 1.0, panel.outsetRight + 1.0) &&
				   qFuzzyCompare(panel.outsetLeft + 1.0, panel.outsetBottom + 1.0));
	outsetPerSide->setChecked(sidesDiffer);
	outsetAll->setValue(qRound(panel.outsetLeft));
	outsetLeft->setValue(qRound(panel.outsetLeft));
	outsetTop->setValue(qRound(panel.outsetTop));
	outsetRight->setValue(qRound(panel.outsetRight));
	outsetBottom->setValue(qRound(panel.outsetBottom));

	const bool cornersDiffer = !panel.hasUniformRadius();
	radiusPerCorner->setChecked(cornersDiffer);
	radiusAll->setValue(qRound(panel.radiusTopLeft));
	radiusTopLeft->setValue(qRound(panel.radiusTopLeft));
	radiusTopRight->setValue(qRound(panel.radiusTopRight));
	radiusBottomRight->setValue(qRound(panel.radiusBottomRight));
	radiusBottomLeft->setValue(qRound(panel.radiusBottomLeft));

	borderGroup->setChecked(panel.border.enabled);
	borderWidth->setValue(panel.border.width);
	borderColor->setColor(panel.border.color);

	applyFillVisibility();
	applyShapeVisibility();

	loading = wasLoading;
}

void BackgroundEditor::setPanel(const BackgroundPanel &panel)
{
	writeFields(panel);
}

BackgroundPanel BackgroundEditor::panel() const
{
	BackgroundPanel result;

	result.fill = static_cast<BackgroundFill>(fillBox->currentData().toInt());
	result.color = colorButton->color();
	result.gradient = gradientEditor->gradient();
	result.imagePath = imagePath->text().trimmed();
	result.imageFit = static_cast<BackgroundImageFit>(imageFit->currentData().toInt());
	result.opacity = opacity->value() / 100.0;

	/*
	 * Read from whichever set of boxes is on screen. Reading the four while the one is showing
	 * would hand back whatever they held before the reader collapsed them, which is exactly the
	 * value they were collapsed to get away from.
	 */
	if (outsetPerSide->isChecked()) {
		result.outsetLeft = outsetLeft->value();
		result.outsetTop = outsetTop->value();
		result.outsetRight = outsetRight->value();
		result.outsetBottom = outsetBottom->value();
	} else {
		const double all = outsetAll->value();
		result.outsetLeft = all;
		result.outsetTop = all;
		result.outsetRight = all;
		result.outsetBottom = all;
	}

	if (radiusPerCorner->isChecked()) {
		result.radiusTopLeft = radiusTopLeft->value();
		result.radiusTopRight = radiusTopRight->value();
		result.radiusBottomRight = radiusBottomRight->value();
		result.radiusBottomLeft = radiusBottomLeft->value();
	} else {
		result.setRadius(radiusAll->value());
	}

	result.border.enabled = borderGroup->isChecked();
	result.border.width = borderWidth->value();
	result.border.color = borderColor->color();

	return result;
}

void BackgroundEditor::applyFillVisibility()
{
	const auto fill = static_cast<BackgroundFill>(fillBox->currentData().toInt());

	form->setRowVisible(colorButton, fill == BackgroundFill::Color);
	form->setRowVisible(gradientEditor, isGradient(fill));
	form->setRowVisible(imageRow, fill == BackgroundFill::Image);
	form->setRowVisible(imageFit, fill == BackgroundFill::Image);

	gradientEditor->setFill(gradientFillFor(fill));
}

void BackgroundEditor::applyShapeVisibility()
{
	const bool sides = outsetPerSide->isChecked();
	form->setRowVisible(outsetAll, !sides);
	form->setRowVisible(outsetLeft->parentWidget(), sides);

	const bool corners = radiusPerCorner->isChecked();
	form->setRowVisible(radiusAll, !corners);
	form->setRowVisible(radiusTopLeft->parentWidget(), corners);
}

void BackgroundEditor::setPresets(const QVector<BackgroundPreset> &newPresets, const QString &selected,
				  bool applySelectedPanel)
{
	presets = newPresets;

	const bool wasLoading = loading;
	loading = true;

	presetBox->clear();
	presetBox->addItem(moduleText("Designer.BackgroundPreset.None"), QString());
	for (const BackgroundPreset &preset : presets) {
		/* Marked when it follows the library, exactly as a style preset is; see StyleEditor. */
		presetBox->addItem(preset.linked ? QStringLiteral("%1  ⇄").arg(preset.name) : preset.name, preset.name);
	}

	const int index = selected.isEmpty() ? 0 : presetBox->findData(selected);
	selectedPreset = index > 0 ? selected : QString();
	presetBox->setCurrentIndex(index > 0 ? index : 0);

	applySelectedPreset(applySelectedPanel);

	loading = wasLoading;
}

void BackgroundEditor::applySelectedPreset(bool applySelectedPanel)
{
	const bool bound = !selectedPreset.isEmpty();

	if (bound && applySelectedPanel) {
		for (const BackgroundPreset &preset : presets) {
			if (preset.name == selectedPreset) {
				writeFields(preset.panel);
				break;
			}
		}
	}

	deletePresetButton->setEnabled(bound);
}

void BackgroundEditor::onPresetSelected()
{
	if (loading)
		return;

	selectedPreset = presetBox->currentData().toString();
	/*
	 * Unbinding leaves the preset's values in the fields as a starting point rather than snapping
	 * back to whatever the slot carried before it was bound.
	 */
	applySelectedPreset(true);

	emit changed();
}

void BackgroundEditor::notifyEdited()
{
	if (loading)
		return;

	/*
	 * Fields stay editable while a preset is bound: an edit there is an edit to the preset, which
	 * is what makes "give every header the same card" a single change.
	 */
	if (!selectedPreset.isEmpty()) {
		emit presetSaveRequested(selectedPreset, panel());
		return;
	}

	emit changed();
}

void BackgroundEditor::savePreset()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(this, moduleText("Designer.BackgroundPreset.Save"),
						   moduleText("Designer.BackgroundPreset.NamePrompt"),
						   QLineEdit::Normal, selectedPreset, &accepted)
				     .trimmed();
	if (!accepted || name.isEmpty())
		return;

	/* Bound optimistically; the owner calls back through setPresets() with the new list. */
	selectedPreset = name;
	emit presetSaveRequested(name, panel());
}

void BackgroundEditor::deletePreset()
{
	const QString name = selectedPreset;
	if (name.isEmpty())
		return;

	const auto answer = QMessageBox::question(this, moduleText("Designer.BackgroundPreset.Delete"),
						  moduleText("Designer.BackgroundPreset.DeleteConfirm").arg(name));
	if (answer != QMessageBox::Yes)
		return;

	selectedPreset.clear();
	emit presetDeleteRequested(name);
}

void BackgroundEditor::browseForImage()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.Background.Image.Pick"),
							  imagePath->text(), imageFilter());
	if (!path.isEmpty())
		imagePath->setText(path);
}

} // namespace closingtime
