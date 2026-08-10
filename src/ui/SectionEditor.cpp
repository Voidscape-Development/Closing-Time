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

#include "ui/SectionEditor.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "ui/CsvImportDialog.hpp"
#include "ui/ToolButtons.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Height the entry table asks for before it starts scrolling, in pixels. */
constexpr int kEntryTableMinimumHeight = 260;

/* Width of a logo list's height column, which never holds more than four digits and a suffix. */
constexpr int kEntryHeightColumnWidth = 80;

/* Image formats QImageReader can decode without extra plugins on every OBS platform. */
QString imageFilter()
{
	return moduleText("Designer.LogoFilter") + QStringLiteral(" (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.svg)");
}

/* The only format QSvgRenderer reads, which is what a bridge tile is rendered through. */
QString svgFilter()
{
	return moduleText("Designer.BridgeSvgFilter") + QStringLiteral(" (*.svg *.svgz)");
}

/*
 * A bridge type's name for the picker. Bridge types come from a table rather than from a fixed
 * list of enum cases, so their locale keys are built from the same ids the table already
 * carries: adding a type needs a string, and needs nothing here.
 */
QString bridgeTypeText(BridgeType type)
{
	const QString key = QStringLiteral("Designer.BridgeType.") + QString::fromLatin1(bridgeTypeId(type));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));

	/* obs_module_text hands the key back when nothing carries it; the table has a name. */
	return text == key ? QString::fromUtf8(bridgeTypeName(type)) : text;
}

void addAlignmentOptions(QComboBox *box)
{
	box->addItem(moduleText("Designer.Align.Left"), static_cast<int>(HAlign::Left));
	box->addItem(moduleText("Designer.Align.Center"), static_cast<int>(HAlign::Center));
	box->addItem(moduleText("Designer.Align.Right"), static_cast<int>(HAlign::Right));
}

void selectByData(QComboBox *box, int value)
{
	const int index = box->findData(value);
	box->setCurrentIndex(index >= 0 ? index : 0);
}

} // namespace

/* ---------------------------------------------------------------------- StyleEditor */

StyleEditor::StyleEditor(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QFormLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	form = layout;

	auto *presetRow = new QWidget(this);
	auto *presetLayout = new QHBoxLayout(presetRow);
	presetLayout->setContentsMargins(0, 0, 0, 0);
	presetBox = new QComboBox(presetRow);
	savePresetButton = new QPushButton(moduleText("Designer.StylePreset.Save"), presetRow);
	deletePresetButton = new QPushButton(moduleText("Designer.StylePreset.Delete"), presetRow);
	presetLayout->addWidget(presetBox, 1);
	presetLayout->addWidget(savePresetButton);
	presetLayout->addWidget(deletePresetButton);
	layout->addRow(moduleText("Designer.StylePreset"), presetRow);

	presetBox->addItem(moduleText("Designer.StylePreset.None"), QString());
	deletePresetButton->setEnabled(false);

	family = new QFontComboBox(this);
	layout->addRow(moduleText("Designer.FontFamily"), family);

	pixelSize = new QSpinBox(this);
	pixelSize->setRange(1, 1024);
	pixelSize->setSuffix(QStringLiteral(" px"));
	layout->addRow(moduleText("Designer.FontSize"), pixelSize);

	auto *weightRow = new QWidget(this);
	auto *weightLayout = new QHBoxLayout(weightRow);
	weightLayout->setContentsMargins(0, 0, 0, 0);
	bold = new QCheckBox(moduleText("Designer.Bold"), weightRow);
	italic = new QCheckBox(moduleText("Designer.Italic"), weightRow);
	weightLayout->addWidget(bold);
	weightLayout->addWidget(italic);
	weightLayout->addStretch();
	layout->addRow(QString(), weightRow);

	alignment = new QComboBox(this);
	addAlignmentOptions(alignment);
	layout->addRow(moduleText("Designer.Alignment"), alignment);

	lineSpacing = new QDoubleSpinBox(this);
	lineSpacing->setRange(0.5, 4.0);
	lineSpacing->setSingleStep(0.05);
	lineSpacing->setDecimals(2);
	layout->addRow(moduleText("Designer.LineSpacing"), lineSpacing);

	fillBox = new QComboBox(this);
	fillBox->addItem(moduleText("Designer.Fill.Solid"), static_cast<int>(TextFill::Solid));
	fillBox->addItem(moduleText("Designer.Fill.Linear"), static_cast<int>(TextFill::LinearGradient));
	fillBox->addItem(moduleText("Designer.Fill.Radial"), static_cast<int>(TextFill::RadialGradient));
	layout->addRow(moduleText("Designer.Fill"), fillBox);

	colourButton = new ColourButton(this);
	colourButton->setDialogTitle(moduleText("Designer.FontColor"));
	layout->addRow(moduleText("Designer.FontColor"), colourButton);

	gradientEditor = new GradientEditor(this);
	layout->addRow(moduleText("Designer.Gradient"), gradientEditor);

	outlineGroup = new QGroupBox(moduleText("Designer.Outline"), this);
	outlineGroup->setCheckable(true);
	auto *outlineForm = new QFormLayout(outlineGroup);
	outlineWidth = new QDoubleSpinBox(outlineGroup);
	outlineWidth->setRange(0.0, 64.0);
	outlineWidth->setDecimals(1);
	outlineWidth->setSingleStep(0.5);
	outlineWidth->setSuffix(QStringLiteral(" px"));
	outlineForm->addRow(moduleText("Designer.OutlineWidth"), outlineWidth);
	outlineColour = new ColourButton(outlineGroup);
	outlineColour->setDialogTitle(moduleText("Designer.OutlineColor"));
	outlineForm->addRow(moduleText("Designer.OutlineColor"), outlineColour);
	layout->addRow(outlineGroup);

	shadowGroup = new QGroupBox(moduleText("Designer.Shadow"), this);
	shadowGroup->setCheckable(true);
	auto *shadowForm = new QFormLayout(shadowGroup);

	auto *offsetRow = new QWidget(shadowGroup);
	auto *offsetLayout = new QHBoxLayout(offsetRow);
	offsetLayout->setContentsMargins(0, 0, 0, 0);
	shadowOffsetX = new QSpinBox(offsetRow);
	shadowOffsetX->setRange(-2048, 2048);
	shadowOffsetX->setPrefix(QStringLiteral("X "));
	shadowOffsetX->setSuffix(QStringLiteral(" px"));
	shadowOffsetY = new QSpinBox(offsetRow);
	shadowOffsetY->setRange(-2048, 2048);
	shadowOffsetY->setPrefix(QStringLiteral("Y "));
	shadowOffsetY->setSuffix(QStringLiteral(" px"));
	offsetLayout->addWidget(shadowOffsetX);
	offsetLayout->addWidget(shadowOffsetY);
	shadowForm->addRow(moduleText("Designer.ShadowOffset"), offsetRow);

	shadowBlur = new QSpinBox(shadowGroup);
	shadowBlur->setRange(0, 200);
	shadowBlur->setSuffix(QStringLiteral(" px"));
	shadowBlur->setToolTip(moduleText("Designer.ShadowBlur.Tip"));
	shadowForm->addRow(moduleText("Designer.ShadowBlur"), shadowBlur);

	shadowColour = new ColourButton(shadowGroup);
	shadowColour->setDialogTitle(moduleText("Designer.ShadowColor"));
	shadowForm->addRow(moduleText("Designer.ShadowColor"), shadowColour);
	layout->addRow(shadowGroup);

	const auto notify = [this] {
		notifyEdited();
	};

	connect(family, &QFontComboBox::currentFontChanged, this, notify);
	connect(pixelSize, &QSpinBox::valueChanged, this, notify);
	connect(bold, &QCheckBox::toggled, this, notify);
	connect(italic, &QCheckBox::toggled, this, notify);
	connect(alignment, &QComboBox::currentIndexChanged, this, notify);
	connect(lineSpacing, &QDoubleSpinBox::valueChanged, this, notify);
	connect(colourButton, &ColourButton::colourChanged, this, notify);
	connect(fillBox, &QComboBox::currentIndexChanged, this, &StyleEditor::onFillChanged);
	connect(gradientEditor, &GradientEditor::changed, this, [this] {
		gradient = gradientEditor->gradient();
		notifyEdited();
	});
	connect(outlineGroup, &QGroupBox::toggled, this, notify);
	connect(outlineWidth, &QDoubleSpinBox::valueChanged, this, notify);
	connect(outlineColour, &ColourButton::colourChanged, this, notify);
	connect(shadowGroup, &QGroupBox::toggled, this, notify);
	connect(shadowOffsetX, &QSpinBox::valueChanged, this, notify);
	connect(shadowOffsetY, &QSpinBox::valueChanged, this, notify);
	connect(shadowBlur, &QSpinBox::valueChanged, this, notify);
	connect(shadowColour, &ColourButton::colourChanged, this, notify);
	connect(presetBox, &QComboBox::currentIndexChanged, this, &StyleEditor::onPresetSelected);
	connect(savePresetButton, &QPushButton::clicked, this, &StyleEditor::savePreset);
	connect(deletePresetButton, &QPushButton::clicked, this, &StyleEditor::deletePreset);

	applyFillVisibility();
}

void StyleEditor::writeFields(const TextStyle &style)
{
	family->setCurrentFont(QFont(style.family));
	pixelSize->setValue(style.pixelSize);
	bold->setChecked(style.bold);
	italic->setChecked(style.italic);
	colourButton->setColour(style.color);
	selectByData(alignment, static_cast<int>(style.align));
	lineSpacing->setValue(style.lineSpacing);

	selectByData(fillBox, static_cast<int>(style.fill));
	gradient = style.gradient;
	gradientEditor->setGradient(gradient);
	gradientEditor->setFill(style.fill);

	outlineGroup->setChecked(style.outline.enabled);
	outlineWidth->setValue(style.outline.width);
	outlineColour->setColour(style.outline.color);

	shadowGroup->setChecked(style.shadow.enabled);
	shadowOffsetX->setValue(qRound(style.shadow.offsetX));
	shadowOffsetY->setValue(qRound(style.shadow.offsetY));
	shadowBlur->setValue(qRound(style.shadow.blur));
	shadowColour->setColour(style.shadow.color);

	applyFillVisibility();
}

void StyleEditor::setStyle(const TextStyle &style)
{
	loading = true;
	writeFields(style);
	loading = false;
}

void StyleEditor::setPresets(const QVector<StylePreset> &newPresets, const QString &selected, bool applySelectedStyle)
{
	presets = newPresets;

	const bool wasLoading = loading;
	loading = true;

	presetBox->clear();
	presetBox->addItem(moduleText("Designer.StylePreset.None"), QString());
	for (const StylePreset &preset : presets)
		presetBox->addItem(preset.name, preset.name);

	const int index = selected.isEmpty() ? 0 : presetBox->findData(selected);
	selectedPreset = index > 0 ? selected : QString();
	presetBox->setCurrentIndex(index > 0 ? index : 0);

	applySelectedPreset(applySelectedStyle);

	loading = wasLoading;
}

void StyleEditor::applySelectedPreset(bool applySelectedStyle)
{
	const bool bound = !selectedPreset.isEmpty();

	if (bound && applySelectedStyle) {
		for (const StylePreset &preset : presets) {
			if (preset.name == selectedPreset) {
				writeFields(preset.style);
				break;
			}
		}
	}

	deletePresetButton->setEnabled(bound);
}

void StyleEditor::onPresetSelected()
{
	if (loading)
		return;

	selectedPreset = presetBox->currentData().toString();
	/*
	 * Unbinding leaves the preset's values in the fields as a starting point rather than
	 * snapping back to whatever the section carried before it was bound.
	 */
	applySelectedPreset(true);

	emit changed();
}

void StyleEditor::notifyEdited()
{
	if (loading)
		return;

	/*
	 * Fields stay editable while a preset is bound: an edit there is an edit to the
	 * preset, which is what makes "restyle every header" a single change.
	 */
	if (!selectedPreset.isEmpty()) {
		emit presetSaveRequested(selectedPreset, style());
		return;
	}

	emit changed();
}

void StyleEditor::savePreset()
{
	bool accepted = false;
	const QString name = QInputDialog::getText(this, moduleText("Designer.StylePreset.Save"),
						   moduleText("Designer.StylePreset.NamePrompt"), QLineEdit::Normal,
						   selectedPreset, &accepted)
				     .trimmed();
	if (!accepted || name.isEmpty())
		return;

	/* Bound optimistically; the owner calls back through setPresets() with the new list. */
	selectedPreset = name;
	emit presetSaveRequested(name, style());
}

void StyleEditor::deletePreset()
{
	const QString name = selectedPreset;
	if (name.isEmpty())
		return;

	const auto answer = QMessageBox::question(this, moduleText("Designer.StylePreset.Delete"),
						  moduleText("Designer.StylePreset.DeleteConfirm").arg(name));
	if (answer != QMessageBox::Yes)
		return;

	selectedPreset.clear();
	emit presetDeleteRequested(name);
}

TextStyle StyleEditor::style() const
{
	TextStyle style;
	style.family = family->currentFont().family();
	style.pixelSize = pixelSize->value();
	style.bold = bold->isChecked();
	style.italic = italic->isChecked();
	style.color = colourButton->colour();
	style.align = static_cast<HAlign>(alignment->currentData().toInt());
	style.lineSpacing = lineSpacing->value();

	style.fill = static_cast<TextFill>(fillBox->currentData().toInt());
	/*
	 * Carried whatever the fill is, so switching to a solid colour to see what the text
	 * looks like underneath and switching back does not cost the stops that were set up.
	 */
	style.gradient = gradient;

	style.outline.enabled = outlineGroup->isChecked();
	style.outline.width = outlineWidth->value();
	style.outline.color = outlineColour->colour();

	style.shadow.enabled = shadowGroup->isChecked();
	style.shadow.offsetX = shadowOffsetX->value();
	style.shadow.offsetY = shadowOffsetY->value();
	style.shadow.blur = shadowBlur->value();
	style.shadow.color = shadowColour->colour();

	return style;
}

void StyleEditor::applyFillVisibility()
{
	const auto fill = static_cast<TextFill>(fillBox->currentData().toInt());
	const bool gradientFill = fill != TextFill::Solid;

	form->setRowVisible(colourButton, !gradientFill);
	form->setRowVisible(gradientEditor, gradientFill);
	gradientEditor->setFill(fill);
}

void StyleEditor::onFillChanged()
{
	const auto fill = static_cast<TextFill>(fillBox->currentData().toInt());

	/*
	 * A gradient still on its factory stops is seeded from the solid colour the style was
	 * already using, so switching to one starts from the text as it looks now rather than
	 * from an unrelated white-to-grey ramp.
	 */
	if (fill != TextFill::Solid && gradient.stops == GradientSpec().stops) {
		const QColor base = colourButton->colour();
		gradient.stops = {GradientStop{0.0, base}, GradientStop{1.0, base.darker(220)}};
		gradientEditor->setGradient(gradient);
	}

	applyFillVisibility();

	if (loading)
		return;

	notifyEdited();
}

/* -------------------------------------------------------------------- SectionEditor */

SectionEditor::SectionEditor(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outerLayout = outer;

	form = new QFormLayout();
	outer->addLayout(form);

	const auto notify = [this] {
		emitChanged();
	};

	typeBox = new QComboBox(this);
	for (SectionType type : allSectionTypes())
		typeBox->addItem(QString::fromUtf8(sectionTypeName(type)), static_cast<int>(type));
	form->addRow(moduleText("Designer.SectionType"), typeBox);

	labelEdit = new QLineEdit(this);
	labelEdit->setPlaceholderText(moduleText("Designer.LabelPlaceholder"));
	form->addRow(moduleText("Designer.Label"), labelEdit);

	visibleBox = new QCheckBox(moduleText("Designer.Visible"), this);
	form->addRow(QString(), visibleBox);

	textEdit = new QPlainTextEdit(this);
	textEdit->setMaximumHeight(80);
	form->addRow(moduleText("Designer.Text"), textEdit);

	auto *logoRow = new QWidget(this);
	auto *logoLayout = new QHBoxLayout(logoRow);
	logoLayout->setContentsMargins(0, 0, 0, 0);
	logoPath = new QLineEdit(logoRow);
	logoBrowse = new QToolButton(logoRow);
	logoBrowse->setText(QStringLiteral("..."));
	logoLayout->addWidget(logoPath);
	logoLayout->addWidget(logoBrowse);
	form->addRow(moduleText("Designer.Logo"), logoRow);

	logoHeight = new QSpinBox(this);
	logoHeight->setRange(1, 4096);
	logoHeight->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.LogoHeight"), logoHeight);

	logoPlacement = new QComboBox(this);
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Hug"), static_cast<int>(LogoPlacement::Hug));
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Edge"), static_cast<int>(LogoPlacement::Edge));
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Bridged"), static_cast<int>(LogoPlacement::Bridged));
	form->addRow(moduleText("Designer.LogoPlacement"), logoPlacement);

	logoSide = new QComboBox(this);
	logoSide->addItem(moduleText("Designer.LogoSide.Left"), static_cast<int>(LogoSide::Left));
	logoSide->addItem(moduleText("Designer.LogoSide.Right"), static_cast<int>(LogoSide::Right));
	form->addRow(moduleText("Designer.LogoSide"), logoSide);

	logoGap = new QSpinBox(this);
	logoGap->setRange(0, 2048);
	logoGap->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.LogoGap"), logoGap);

	bridgeType = new QComboBox(this);
	for (BridgeType type : allBridgeTypes())
		bridgeType->addItem(bridgeTypeText(type), static_cast<int>(type));
	form->addRow(moduleText("Designer.BridgeType"), bridgeType);

	bridgeEdit = new QLineEdit(this);
	form->addRow(moduleText("Designer.Bridge"), bridgeEdit);

	auto *bridgeSvgRow = new QWidget(this);
	auto *bridgeSvgLayout = new QHBoxLayout(bridgeSvgRow);
	bridgeSvgLayout->setContentsMargins(0, 0, 0, 0);
	bridgeSvgPath = new QLineEdit(bridgeSvgRow);
	bridgeSvgBrowse = new QToolButton(bridgeSvgRow);
	bridgeSvgBrowse->setText(QStringLiteral("..."));
	bridgeSvgLayout->addWidget(bridgeSvgPath);
	bridgeSvgLayout->addWidget(bridgeSvgBrowse);
	form->addRow(moduleText("Designer.BridgeSvg"), bridgeSvgRow);

	bridgeThickness = new QSpinBox(this);
	bridgeThickness->setRange(1, 1024);
	bridgeThickness->setSuffix(QStringLiteral(" px"));
	bridgeThickness->setToolTip(moduleText("Designer.BridgeThickness.Tip"));
	form->addRow(moduleText("Designer.BridgeThickness"), bridgeThickness);

	bridgeOffset = new QSpinBox(this);
	bridgeOffset->setRange(-1024, 1024);
	bridgeOffset->setSuffix(QStringLiteral(" px"));
	bridgeOffset->setToolTip(moduleText("Designer.BridgeOffset.Tip"));
	form->addRow(moduleText("Designer.BridgeOffset"), bridgeOffset);

	bridgeGap = new QSpinBox(this);
	bridgeGap->setRange(0, 2048);
	bridgeGap->setSuffix(QStringLiteral(" px"));
	bridgeGap->setToolTip(moduleText("Designer.BridgeGap.Tip"));
	form->addRow(moduleText("Designer.BridgeGap"), bridgeGap);

	bridgeTint = new QCheckBox(moduleText("Designer.BridgeTint"), this);
	form->addRow(QString(), bridgeTint);

	bridgeFill = new QComboBox(this);
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Fixed"), static_cast<int>(BridgeFill::Fixed));
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Repeat"), static_cast<int>(BridgeFill::Repeat));
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Stretch"), static_cast<int>(BridgeFill::Stretch));
	form->addRow(moduleText("Designer.BridgeFill"), bridgeFill);

	bridgeSizing = new QComboBox(this);
	bridgeSizing->addItem(moduleText("Designer.BridgeSizing.Split"), static_cast<int>(BridgeSizing::Split));
	bridgeSizing->addItem(moduleText("Designer.BridgeSizing.Natural"), static_cast<int>(BridgeSizing::Natural));
	form->addRow(moduleText("Designer.BridgeSizing"), bridgeSizing);

	bridgeSplit = new QSpinBox(this);
	bridgeSplit->setRange(0, 100);
	bridgeSplit->setSuffix(QStringLiteral(" %"));
	bridgeSplit->setToolTip(moduleText("Designer.BridgeSplit.Tip"));
	form->addRow(moduleText("Designer.BridgeSplit"), bridgeSplit);

	bridgeRowAlign = new QComboBox(this);
	addAlignmentOptions(bridgeRowAlign);
	form->addRow(moduleText("Designer.BridgeRowAlign"), bridgeRowAlign);

	bridgeSpanEmpty = new QCheckBox(moduleText("Designer.BridgeSpanEmpty"), this);
	form->addRow(QString(), bridgeSpanEmpty);

	columns = new QSpinBox(this);
	columns->setRange(1, 12);
	form->addRow(moduleText("Designer.Columns"), columns);

	columnGap = new QSpinBox(this);
	columnGap->setRange(0, 2048);
	columnGap->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.ColumnGap"), columnGap);

	fillOrder = new QComboBox(this);
	fillOrder->addItem(moduleText("Designer.FillDown"), 0);
	fillOrder->addItem(moduleText("Designer.FillAcross"), 1);
	form->addRow(moduleText("Designer.FillOrder"), fillOrder);

	entryGap = new QSpinBox(this);
	entryGap->setRange(0, 2048);
	entryGap->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.EntryGap"), entryGap);

	spacerHeight = new QSpinBox(this);
	spacerHeight->setRange(0, 20000);
	spacerHeight->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.SpacerHeight"), spacerHeight);

	paddingTop = new QSpinBox(this);
	paddingTop->setRange(0, 20000);
	paddingTop->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.PaddingTop"), paddingTop);

	paddingBottom = new QSpinBox(this);
	paddingBottom->setRange(0, 20000);
	paddingBottom->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.PaddingBottom"), paddingBottom);

	marginX = new QSpinBox(this);
	marginX->setRange(0, 4096);
	marginX->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.MarginX"), marginX);

	sectionWidth = new QSpinBox(this);
	sectionWidth->setRange(1, 100);
	sectionWidth->setSuffix(QStringLiteral(" %"));
	sectionWidth->setToolTip(moduleText("Designer.SectionWidth.Tip"));
	form->addRow(moduleText("Designer.SectionWidth"), sectionWidth);

	sectionAlign = new QComboBox(this);
	addAlignmentOptions(sectionAlign);
	sectionAlign->setToolTip(moduleText("Designer.SectionAlign.Tip"));
	form->addRow(moduleText("Designer.SectionAlign"), sectionAlign);

	auto *styleGroup = new QGroupBox(moduleText("Designer.TextStyle"), this);
	auto *styleLayout = new QVBoxLayout(styleGroup);
	primaryStyle = new StyleEditor(styleGroup);
	styleLayout->addWidget(primaryStyle);
	outer->addWidget(styleGroup);

	secondaryGroup = new QGroupBox(moduleText("Designer.SecondaryStyle"), this);
	secondaryGroup->setCheckable(true);
	auto *secondaryLayout = new QVBoxLayout(secondaryGroup);
	secondaryStyle = new StyleEditor(secondaryGroup);
	secondaryLayout->addWidget(secondaryStyle);
	outer->addWidget(secondaryGroup);

	entriesGroup = new QGroupBox(moduleText("Designer.Entries"), this);
	auto *entriesLayout = new QVBoxLayout(entriesGroup);

	entryTable = new QTableWidget(entriesGroup);
	entryTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	entryTable->verticalHeader()->setVisible(false);
	entryTable->horizontalHeader()->setStretchLastSection(true);
	/*
	 * A list is the thing being worked on when a section has one, so the table asks for enough
	 * height to read a run of entries at a glance and to have somewhere to scroll. It still
	 * grows past this with the pane -- see the trailing stretch below.
	 */
	entryTable->setMinimumHeight(kEntryTableMinimumHeight);
	entriesLayout->addWidget(entryTable);

	auto *buttons = new QHBoxLayout();
	const auto addButton = [&](QToolButton *button, auto slot) {
		buttons->addWidget(button);
		connect(button, &QToolButton::clicked, this, slot);
		return button;
	};

	addButton(makeGlyphButton(entriesGroup, QStringLiteral("+"), moduleText("Designer.AddEntry")),
		  &SectionEditor::addEntry);
	addButton(makeGlyphButton(entriesGroup, QStringLiteral("−"), moduleText("Designer.RemoveEntry")),
		  &SectionEditor::removeSelectedEntries);
	addButton(makeArrowButton(entriesGroup, Qt::UpArrow, moduleText("Designer.MoveUp")),
		  [this] { moveSelectedEntry(-1); });
	addButton(makeArrowButton(entriesGroup, Qt::DownArrow, moduleText("Designer.MoveDown")),
		  [this] { moveSelectedEntry(1); });
	/*
	 * Only a logo list has a path to set, so this one comes and goes with the section type --
	 * see applyTypeVisibility. Everything else in the row acts on a row of the table whatever
	 * the table happens to hold.
	 */
	setLogoButton = addButton(makeLabelledButton(entriesGroup, moduleText("Designer.SetLogo")),
				  &SectionEditor::browseForEntryLogo);
	buttons->addStretch();
	addButton(makeLabelledButton(entriesGroup, moduleText("Designer.ImportCsv")), &SectionEditor::importCsv);

	entriesLayout->addLayout(buttons);
	outer->addWidget(entriesGroup, 1);

	/*
	 * Whatever height is left over when the editor is shorter than the pane it sits in. A
	 * QVBoxLayout with nothing to give the slack to shares it out between the items it has, so
	 * without this a type carrying few fields -- Title especially -- has its handful of rows
	 * spread down the pane with gaps between them rather than sitting one under the next.
	 */
	outer->addStretch();
	trailingStretchIndex = outer->count() - 1;

	connect(typeBox, &QComboBox::currentIndexChanged, this, [this] {
		if (loading)
			return;

		const auto type = static_cast<SectionType>(typeBox->currentData().toInt());
		applyTypeVisibility(type);
		rebuildEntryTable(type);
		emitChanged();
	});

	connect(labelEdit, &QLineEdit::textChanged, this, notify);
	connect(visibleBox, &QCheckBox::toggled, this, notify);
	connect(textEdit, &QPlainTextEdit::textChanged, this, notify);
	connect(logoPath, &QLineEdit::textChanged, this, notify);
	connect(logoHeight, &QSpinBox::valueChanged, this, notify);
	connect(logoSide, &QComboBox::currentIndexChanged, this, notify);
	connect(logoGap, &QSpinBox::valueChanged, this, notify);
	connect(bridgeEdit, &QLineEdit::textChanged, this, notify);
	connect(bridgeSvgPath, &QLineEdit::textChanged, this, notify);
	connect(bridgeThickness, &QSpinBox::valueChanged, this, notify);
	connect(bridgeOffset, &QSpinBox::valueChanged, this, notify);
	connect(bridgeGap, &QSpinBox::valueChanged, this, notify);
	connect(bridgeTint, &QCheckBox::toggled, this, notify);
	connect(bridgeSplit, &QSpinBox::valueChanged, this, notify);
	connect(bridgeRowAlign, &QComboBox::currentIndexChanged, this, notify);
	connect(bridgeSpanEmpty, &QCheckBox::toggled, this, notify);

	/* These decide which of the other bridge rows are worth showing. */
	for (QComboBox *box : {bridgeType, bridgeFill, bridgeSizing, logoPlacement}) {
		connect(box, &QComboBox::currentIndexChanged, this, [this] {
			if (loading)
				return;

			applyTypeVisibility(static_cast<SectionType>(typeBox->currentData().toInt()));
			emitChanged();
		});
	}
	connect(columns, &QSpinBox::valueChanged, this, notify);
	connect(columnGap, &QSpinBox::valueChanged, this, notify);
	connect(fillOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(entryGap, &QSpinBox::valueChanged, this, notify);
	connect(spacerHeight, &QSpinBox::valueChanged, this, notify);
	connect(paddingTop, &QSpinBox::valueChanged, this, notify);
	connect(paddingBottom, &QSpinBox::valueChanged, this, notify);
	connect(marginX, &QSpinBox::valueChanged, this, notify);
	connect(sectionWidth, &QSpinBox::valueChanged, this, notify);
	connect(sectionAlign, &QComboBox::currentIndexChanged, this, notify);
	connect(primaryStyle, &StyleEditor::changed, this, notify);
	connect(secondaryStyle, &StyleEditor::changed, this, notify);

	/*
	 * Preset edits are routed up to the designer, which owns the document the presets live
	 * on. `presetOrigin` marks the editor mid-signal so the synchronous round trip back
	 * through setPresets() leaves the fields being typed into alone.
	 */
	for (StyleEditor *editor : {primaryStyle, secondaryStyle}) {
		connect(editor, &StyleEditor::presetSaveRequested, this,
			[this, editor](const QString &name, const TextStyle &style) {
				presetOrigin = editor;
				emit presetSaveRequested(name, style);
				presetOrigin = nullptr;
			});
		connect(editor, &StyleEditor::presetDeleteRequested, this, [this, editor](const QString &name) {
			presetOrigin = editor;
			emit presetDeleteRequested(name);
			presetOrigin = nullptr;
		});
	}
	connect(secondaryGroup, &QGroupBox::toggled, this, notify);
	connect(entryTable, &QTableWidget::cellChanged, this, notify);
	connect(logoBrowse, &QToolButton::clicked, this, &SectionEditor::browseForSectionLogo);
	connect(bridgeSvgBrowse, &QToolButton::clicked, this, &SectionEditor::browseForBridgeSvg);
}

void SectionEditor::setSection(const Section &source)
{
	loading = true;
	current = source;

	selectByData(typeBox, static_cast<int>(source.type));
	labelEdit->setText(source.label);
	visibleBox->setChecked(source.visible);
	textEdit->setPlainText(source.text);
	logoPath->setText(source.logo.path);
	logoHeight->setValue(source.logo.maxHeight);
	selectByData(logoPlacement, static_cast<int>(source.logoPlacement));
	selectByData(logoSide, static_cast<int>(source.logoSide));
	logoGap->setValue(source.logoGap);
	selectByData(bridgeType, static_cast<int>(source.bridgeType));
	bridgeEdit->setText(source.bridge);
	bridgeSvgPath->setText(source.bridgeSvg);
	bridgeThickness->setValue(qRound(source.bridgeThickness));
	bridgeOffset->setValue(qRound(source.bridgeOffset));
	bridgeGap->setValue(qRound(source.bridgeGap));
	bridgeTint->setChecked(source.bridgeTint);
	selectByData(bridgeFill, static_cast<int>(source.bridgeFill));
	selectByData(bridgeSizing, static_cast<int>(source.bridgeSizing));
	bridgeSplit->setValue(qRound(source.bridgeSplit * 100.0));
	selectByData(bridgeRowAlign, static_cast<int>(source.bridgeRowAlign));
	bridgeSpanEmpty->setChecked(source.bridgeSpanEmpty);
	columns->setValue(source.columns);
	columnGap->setValue(source.columnGap);
	selectByData(fillOrder, source.fillAcross ? 1 : 0);
	entryGap->setValue(source.entryGap);
	spacerHeight->setValue(source.spacerHeight);
	paddingTop->setValue(source.paddingTop);
	paddingBottom->setValue(source.paddingBottom);
	marginX->setValue(source.marginX);
	sectionWidth->setValue(std::clamp(qRound(source.sectionWidth * 100.0), 1, 100));
	selectByData(sectionAlign, static_cast<int>(source.sectionAlign));

	primaryStyle->setStyle(source.style);
	secondaryStyle->setStyle(source.secondaryStyle);
	/* After setStyle, so a bound preset's values win over the section's own copy. */
	primaryStyle->setPresets(presets, source.stylePresetName);
	secondaryStyle->setPresets(presets, source.secondaryStylePresetName);
	secondaryGroup->setChecked(source.useSecondaryStyle);

	applyTypeVisibility(source.type);
	rebuildEntryTable(source.type);
	writeEntriesToTable(source);

	loading = false;
}

Section SectionEditor::section() const
{
	Section result = current;

	result.type = static_cast<SectionType>(typeBox->currentData().toInt());
	result.label = labelEdit->text();
	result.visible = visibleBox->isChecked();
	result.text = textEdit->toPlainText();
	result.logo.path = logoPath->text();
	result.logo.maxHeight = logoHeight->value();
	result.logoPlacement = static_cast<LogoPlacement>(logoPlacement->currentData().toInt());
	result.logoSide = static_cast<LogoSide>(logoSide->currentData().toInt());
	result.logoGap = logoGap->value();
	result.bridgeType = static_cast<BridgeType>(bridgeType->currentData().toInt());
	result.bridge = bridgeEdit->text();
	result.bridgeSvg = bridgeSvgPath->text();
	result.bridgeThickness = bridgeThickness->value();
	result.bridgeOffset = bridgeOffset->value();
	result.bridgeGap = bridgeGap->value();
	result.bridgeTint = bridgeTint->isChecked();
	result.bridgeFill = static_cast<BridgeFill>(bridgeFill->currentData().toInt());
	result.bridgeSizing = static_cast<BridgeSizing>(bridgeSizing->currentData().toInt());
	result.bridgeSplit = bridgeSplit->value() / 100.0;
	result.bridgeRowAlign = static_cast<HAlign>(bridgeRowAlign->currentData().toInt());
	result.bridgeSpanEmpty = bridgeSpanEmpty->isChecked();
	result.columns = columns->value();
	result.columnGap = columnGap->value();
	result.fillAcross = fillOrder->currentData().toInt() == 1;
	result.entryGap = entryGap->value();
	result.spacerHeight = spacerHeight->value();
	result.paddingTop = paddingTop->value();
	result.paddingBottom = paddingBottom->value();
	result.marginX = marginX->value();
	result.sectionWidth = sectionWidth->value() / 100.0;
	result.sectionAlign = static_cast<HAlign>(sectionAlign->currentData().toInt());
	result.style = primaryStyle->style();
	result.secondaryStyle = secondaryStyle->style();
	result.useSecondaryStyle = secondaryGroup->isChecked();
	result.stylePresetName = primaryStyle->presetName();
	result.secondaryStylePresetName = secondaryStyle->presetName();

	readEntriesFromTable(&result);
	return result;
}

void SectionEditor::setPresets(const QVector<StylePreset> &newPresets)
{
	presets = newPresets;

	for (StyleEditor *editor : {primaryStyle, secondaryStyle})
		editor->setPresets(presets, editor->presetName(), editor != presetOrigin);
}

void SectionEditor::applyTypeVisibility(SectionType type)
{
	const bool hasText = sectionUsesText(type);
	const bool hasLogos = sectionUsesLogos(type);
	const bool hasEntries = sectionUsesEntries(type);
	const bool hasColumns = sectionUsesColumns(type);

	/* Single-line text lives on the section; list text lives in the entry table. */
	const bool singleLineText = hasText && !hasEntries;
	/* Only the "... w/ Logo" pair carries a logo alongside text on the section itself. */
	const bool sectionLogo = hasLogos && !hasEntries;
	const bool logoBesideText = sectionLogo && hasText;

	form->setRowVisible(textEdit, singleLineText);
	form->setRowVisible(logoPath->parentWidget(), sectionLogo);
	form->setRowVisible(logoHeight, sectionLogo);
	form->setRowVisible(logoSide, logoBesideText);
	form->setRowVisible(logoGap, logoBesideText);
	const bool bridged = type == SectionType::Bridged;
	const auto fill = static_cast<BridgeFill>(bridgeFill->currentData().toInt());
	const auto sizing = static_cast<BridgeSizing>(bridgeSizing->currentData().toInt());
	const auto placement = static_cast<LogoPlacement>(logoPlacement->currentData().toInt());

	/* A logo row bridged across to its text uses the same bridge fields a Bridged section does. */
	const bool usesBridge = bridged || (logoBesideText && placement == LogoPlacement::Bridged);

	/* Art and text bridges are configured by different halves of the same set of rows. */
	const auto bridgeArt = static_cast<BridgeType>(bridgeType->currentData().toInt());
	const bool drawnArt = usesBridge && bridgeTypeUsesArt(bridgeArt);
	const bool artFromFile = usesBridge && bridgeTypeUsesFile(bridgeArt);

	form->setRowVisible(logoPlacement, logoBesideText);
	form->setRowVisible(bridgeType, usesBridge);
	form->setRowVisible(bridgeEdit, usesBridge && !drawnArt);
	form->setRowVisible(bridgeSvgPath->parentWidget(), artFromFile);
	form->setRowVisible(bridgeThickness, drawnArt);
	form->setRowVisible(bridgeOffset, drawnArt);
	form->setRowVisible(bridgeGap, drawnArt);
	/* The built-in tiles are drawn white to be tinted; only a user's file has colours to keep. */
	form->setRowVisible(bridgeTint, artFromFile);
	form->setRowVisible(bridgeFill, usesBridge);
	/* Column sizing and row placement describe two texts, so they stay with that type. */
	form->setRowVisible(bridgeSizing, bridged);
	/* The split is the tab stop; with Natural sizing the text decides where things land. */
	form->setRowVisible(bridgeSplit, bridged && sizing == BridgeSizing::Split);
	/*
	 * Only a fixed bridge with natural columns can leave a row narrower than the section.
	 * Every other combination fills the width, so there is nothing left to align.
	 */
	form->setRowVisible(bridgeRowAlign, bridged && sizing == BridgeSizing::Natural && fill == BridgeFill::Fixed);
	/* A fixed bridge has nothing to run into the space an empty column would free. */
	form->setRowVisible(bridgeSpanEmpty, bridged && fill != BridgeFill::Fixed);
	form->setRowVisible(columns, hasColumns);
	form->setRowVisible(columnGap, hasColumns);
	form->setRowVisible(fillOrder, hasColumns);
	form->setRowVisible(entryGap, hasEntries);
	form->setRowVisible(spacerHeight, type == SectionType::Spacer);

	primaryStyle->parentWidget()->setVisible(hasText || hasLogos);
	secondaryGroup->setVisible(type == SectionType::Bridged);
	entriesGroup->setVisible(hasEntries);
	/* Nothing for a file picker to fill in when the entries are lines of text. */
	setLogoButton->setVisible(hasEntries && hasLogos);

	/*
	 * The entry table is the one thing here worth growing, so it takes the leftover height
	 * whenever it is on show. With no table the trailing spacer takes it instead, which is what
	 * keeps the rows packed at the top rather than spread down the pane.
	 */
	outerLayout->setStretch(trailingStretchIndex, hasEntries ? 0 : 1);
}

void SectionEditor::rebuildEntryTable(SectionType type)
{
	const QSignalBlocker blocker(entryTable);

	entryTable->clear();
	entryTable->setRowCount(0);

	switch (type) {
	case SectionType::Bridged:
		entryTable->setColumnCount(2);
		entryTable->setHorizontalHeaderLabels(
			{moduleText("Designer.Column.Left"), moduleText("Designer.Column.Right")});
		break;

	case SectionType::LogoList:
	case SectionType::MultiLogoList:
		entryTable->setColumnCount(2);
		entryTable->setHorizontalHeaderLabels(
			{moduleText("Designer.Column.Logo"), moduleText("Designer.Column.Height")});
		break;

	default:
		entryTable->setColumnCount(1);
		entryTable->setHorizontalHeaderLabels({moduleText("Designer.Column.Text")});
		break;
	}

	/*
	 * The last column takes the slack everywhere except a logo list, where the last column is a
	 * pixel height -- three digits' worth of table given to it while the file path beside it,
	 * the one thing in the row long enough to need reading, is squeezed into what is left.
	 * There the path column takes the width instead and the height keeps only what it needs.
	 */
	const bool logoEntries = sectionUsesLogos(type) && sectionUsesEntries(type);
	QHeaderView *header = entryTable->horizontalHeader();

	header->setStretchLastSection(!logoEntries);
	header->setSectionResizeMode(QHeaderView::Interactive);

	if (logoEntries) {
		header->setSectionResizeMode(0, QHeaderView::Stretch);
		entryTable->setColumnWidth(1, kEntryHeightColumnWidth);
	}
}

void SectionEditor::writeEntriesToTable(const Section &source)
{
	const QSignalBlocker blocker(entryTable);
	const bool logoMode = sectionUsesLogos(source.type) && sectionUsesEntries(source.type);

	entryTable->setRowCount(source.entries.size());
	for (int row = 0; row < source.entries.size(); ++row) {
		const Entry &entry = source.entries.at(row);

		if (logoMode) {
			entryTable->setItem(row, 0, new QTableWidgetItem(entry.logo.path));
			entryTable->setItem(row, 1, new QTableWidgetItem(QString::number(entry.logo.maxHeight)));
			continue;
		}

		entryTable->setItem(row, 0, new QTableWidgetItem(entry.text));
		if (source.type == SectionType::Bridged)
			entryTable->setItem(row, 1, new QTableWidgetItem(entry.secondaryText));
	}
}

void SectionEditor::readEntriesFromTable(Section *target) const
{
	const bool logoMode = sectionUsesLogos(target->type) && sectionUsesEntries(target->type);

	QVector<Entry> entries;
	entries.reserve(entryTable->rowCount());

	for (int row = 0; row < entryTable->rowCount(); ++row) {
		const auto cell = [this, row](int column) {
			const QTableWidgetItem *item = entryTable->item(row, column);
			return item ? item->text() : QString();
		};

		Entry entry;
		if (logoMode) {
			entry.logo.path = cell(0);
			const int height = cell(1).toInt();
			entry.logo.maxHeight = height > 0 ? height : 96;
		} else {
			entry.text = cell(0);
			if (target->type == SectionType::Bridged)
				entry.secondaryText = cell(1);
		}

		entries.append(entry);
	}

	target->entries = entries;
}

void SectionEditor::addEntry()
{
	entryTable->insertRow(entryTable->rowCount());
	emitChanged();
}

void SectionEditor::removeSelectedEntries()
{
	QList<int> rows;
	for (const QModelIndex &index : entryTable->selectionModel()->selectedRows())
		rows.append(index.row());

	/* Remove from the bottom up so earlier indices stay valid. */
	std::sort(rows.begin(), rows.end(), std::greater<int>());
	for (int row : rows)
		entryTable->removeRow(row);

	if (!rows.isEmpty())
		emitChanged();
}

void SectionEditor::moveSelectedEntry(int delta)
{
	const int row = entryTable->currentRow();
	const int target = row + delta;
	if (row < 0 || target < 0 || target >= entryTable->rowCount())
		return;

	const QSignalBlocker blocker(entryTable);
	for (int column = 0; column < entryTable->columnCount(); ++column) {
		QTableWidgetItem *from = entryTable->takeItem(row, column);
		QTableWidgetItem *to = entryTable->takeItem(target, column);
		entryTable->setItem(target, column, from);
		entryTable->setItem(row, column, to);
	}

	entryTable->setCurrentCell(target, std::max(0, entryTable->currentColumn()));
	emitChanged();
}

void SectionEditor::browseForSectionLogo()
{
	const QString path =
		QFileDialog::getOpenFileName(this, moduleText("Designer.ChooseLogo"), logoPath->text(), imageFilter());
	if (!path.isEmpty())
		logoPath->setText(path);
}

void SectionEditor::browseForBridgeSvg()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.ChooseBridgeSvg"),
							  bridgeSvgPath->text(), svgFilter());
	if (!path.isEmpty())
		bridgeSvgPath->setText(path);
}

void SectionEditor::browseForEntryLogo()
{
	const auto type = static_cast<SectionType>(typeBox->currentData().toInt());
	if (!sectionUsesLogos(type) || !sectionUsesEntries(type))
		return;

	const int row = entryTable->currentRow();
	if (row < 0)
		return;

	const QTableWidgetItem *existing = entryTable->item(row, 0);
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.ChooseLogo"),
							  existing ? existing->text() : QString(), imageFilter());
	if (path.isEmpty())
		return;

	entryTable->setItem(row, 0, new QTableWidgetItem(path));
	if (!entryTable->item(row, 1))
		entryTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("96")));
}

void SectionEditor::importCsv()
{
	const auto type = static_cast<SectionType>(typeBox->currentData().toInt());

	CsvImportDialog dialog(type, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const QVector<Entry> imported = dialog.entries();
	if (imported.isEmpty())
		return;

	Section updated = section();
	if (dialog.replaceExisting())
		updated.entries = imported;
	else
		updated.entries += imported;

	/* Round-tripping through setSection keeps the table and the model in step. */
	setSection(updated);
	emitChanged();
}

void SectionEditor::emitChanged()
{
	if (!loading)
		emit changed();
}

} // namespace closingtime
