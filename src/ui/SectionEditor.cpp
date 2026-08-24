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
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>

#include "render/AnimatedLogo.hpp"
#include "ui/CsvImportDialog.hpp"
#include "ui/FontPickerDialog.hpp"
#include "ui/ToolButtons.hpp"

namespace closingtime {

const QVector<DividerPiece> &dividerPieces(const Section &section, PieceSlot slot)
{
	switch (slot) {
	case PieceSlot::LeftEnd:
		return section.dividerCap;
	case PieceSlot::RightEnd:
		return section.dividerEndCap;
	case PieceSlot::Centre:
		break;
	}
	return section.dividerCentre;
}

QVector<DividerPiece> &dividerPieces(Section &section, PieceSlot slot)
{
	/* The const one is the definition; this is the same walk with the constness taken back off. */
	return const_cast<QVector<DividerPiece> &>(dividerPieces(std::as_const(section), slot));
}

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Height the entry table asks for before it starts scrolling, in pixels. */
constexpr int kEntryTableMinimumHeight = 260;

/* Width of a logo list's height column, which never holds more than four digits and a suffix. */
constexpr int kEntryHeightColumnWidth = 80;

/*
 * Where an entry row keeps the whole of the entry it was written from.
 *
 * The table shows only the columns the section's type has a use for, and reading a section back
 * rebuilds its entries from those columns -- so any field without a column of its own would be
 * dropped by the next read, which is the one thing changing a type or turning a setting off must
 * never do. Stashing the entry on the row's first cell costs a QVariantMap per row and makes every
 * unshown field survive a type change, a subtitle toggle and a row being moved alike: a move swaps
 * whole items, so the stash travels with the row it belongs to.
 */
constexpr int kEntryStashRole = Qt::UserRole + 1;

QVariant entryStash(const Entry &entry)
{
	QVariantMap stash;
	stash.insert(QStringLiteral("text"), entry.text);
	stash.insert(QStringLiteral("secondary"), entry.secondaryText);
	stash.insert(QStringLiteral("subtitle"), entry.subtitle);
	stash.insert(QStringLiteral("secondary_subtitle"), entry.secondarySubtitle);
	stash.insert(QStringLiteral("logo"), entry.logo.path);
	stash.insert(QStringLiteral("logo_height"), entry.logo.maxHeight);
	return stash;
}

Entry entryFromStash(const QVariant &value)
{
	Entry entry;
	if (!value.canConvert<QVariantMap>())
		return entry;

	const QVariantMap stash = value.toMap();
	entry.text = stash.value(QStringLiteral("text")).toString();
	entry.secondaryText = stash.value(QStringLiteral("secondary")).toString();
	entry.subtitle = stash.value(QStringLiteral("subtitle")).toString();
	entry.secondarySubtitle = stash.value(QStringLiteral("secondary_subtitle")).toString();
	entry.logo.path = stash.value(QStringLiteral("logo")).toString();

	const int height = stash.value(QStringLiteral("logo_height")).toInt();
	if (height > 0)
		entry.logo.maxHeight = height;

	return entry;
}

/* Height the divider's centre table asks for before it starts scrolling, in pixels. A centre
 * stack is a handful of pieces where an entry list is a cast, so it asks for less. */
constexpr int kCentreTableMinimumHeight = 150;

/* Image formats QImageReader can decode without extra plugins on every OBS platform. */
QString imageFilter()
{
	/*
	 * The patterns come from the render layer, because what can be opened is decided by what can
	 * be decoded, and there is no sense offering a file the decoder would only refuse.
	 */
	const QString filter = moduleText("Designer.LogoFilter") + QStringLiteral(" (%1)").arg(imageLogoPatterns());

	return filter + QStringLiteral(";;%1 (*)").arg(moduleText("Designer.AllFilesFilter"));
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

/*
 * A divider shape's name for the pickers, built from the same id the shape table already
 * carries -- so adding a shape needs a string and needs nothing here, exactly as with bridges.
 */
QString dividerShapeText(DividerShape shape)
{
	const QString key = QStringLiteral("Designer.DividerShape.") + QString::fromLatin1(dividerShapeId(shape));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));

	return text == key ? QString::fromUtf8(dividerShapeName(shape)) : text;
}

QString dividerPieceKindText(DividerPiece::Kind kind)
{
	const QString key = QStringLiteral("Designer.DividerPiece.") + QString::fromLatin1(dividerPieceKindId(kind));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));

	return text == key ? QString::fromUtf8(dividerPieceKindName(kind)) : text;
}

/* Columns of the divider's centre-piece table. */
enum PieceColumn {
	/* Ornament, text or logo. */
	PieceKind = 0,
	/* Ornament pieces only: which shape from the library. */
	PieceShape,
	/* The word, for a text piece; the file, for a logo or a custom ornament. */
	PieceValue,
	/* Ornament pieces only: a multiplier on the size its shape asks for. */
	PieceSize,
	PieceColumnCount,
};

/* Width of the centre table's two narrow columns, neither of which holds a long word. */
constexpr int kPieceKindColumnWidth = 110;
constexpr int kPieceSizeColumnWidth = 70;

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

	fontButton = new QPushButton(this);
	fontButton->setToolTip(moduleText("Designer.Font.Tip"));
	layout->addRow(moduleText("Designer.Font"), fontButton);

	pixelSize = new QSpinBox(this);
	pixelSize->setRange(1, 1024);
	pixelSize->setSuffix(QStringLiteral(" px"));
	layout->addRow(moduleText("Designer.FontSize"), pixelSize);

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

	connect(fontButton, &QPushButton::clicked, this, &StyleEditor::pickFont);
	connect(pixelSize, &QSpinBox::valueChanged, this, notify);
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

	/* So the button reads as a font rather than as a blank before any style is written into it. */
	updateFontButton();
	applyFillVisibility();
}

void StyleEditor::writeFields(const TextStyle &style)
{
	loaded = style;

	chosenFont.family = style.family;
	chosenFont.styleName = style.styleName;
	chosenFont.bold = style.bold;
	chosenFont.italic = style.italic;
	chosenFont.underline = style.underline;
	chosenFont.strikeOut = style.strikeOut;
	updateFontButton();

	pixelSize->setValue(style.pixelSize);
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

void StyleEditor::pickFont()
{
	FontPickerDialog dialog(chosenFont, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const FontChoice picked = dialog.choice();
	if (picked == chosenFont)
		return;

	chosenFont = picked;
	updateFontButton();

	/*
	 * Reported the same way a spin box's edit is. Opening the picker on a bound preset and
	 * choosing a face is an edit to that preset, which is what restyles every section following
	 * it -- there is nothing special about the font here.
	 */
	notifyEdited();
}

void StyleEditor::updateFontButton()
{
	fontButton->setText(describeFontChoice(chosenFont));
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
	for (const StylePreset &preset : presets) {
		/*
		 * A preset that follows the machine-wide library is marked, because editing one has
		 * consequences outside this roll. The mark is in the label only -- the name is still
		 * what the item carries and what a section binds to.
		 */
		presetBox->addItem(preset.linked ? QStringLiteral("%1  ⇄").arg(preset.name) : preset.name, preset.name);
	}

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
	/*
	 * Built on top of what the fields were filled from rather than from a default style, so the
	 * rows an ink-only editor hides come back out exactly as they went in. With every row on
	 * show the widgets below overwrite all of it and the starting point makes no difference.
	 */
	TextStyle style = loaded;

	if (!inkOnly) {
		style.family = chosenFont.family;
		style.styleName = chosenFont.styleName;
		style.bold = chosenFont.bold;
		style.italic = chosenFont.italic;
		style.underline = chosenFont.underline;
		style.strikeOut = chosenFont.strikeOut;
		style.pixelSize = pixelSize->value();
		style.align = static_cast<HAlign>(alignment->currentData().toInt());
		style.lineSpacing = lineSpacing->value();
	}

	style.color = colourButton->colour();

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

void StyleEditor::setInkOnly(bool value)
{
	inkOnly = value;

	form->setRowVisible(fontButton, !inkOnly);
	form->setRowVisible(pixelSize, !inkOnly);
	form->setRowVisible(alignment, !inkOnly);
	form->setRowVisible(lineSpacing, !inkOnly);

	/* "Font Color" is the wrong name for the colour of a run of dots. */
	if (auto *label = qobject_cast<QLabel *>(form->labelForField(colourButton)))
		label->setText(moduleText(inkOnly ? "Designer.InkColor" : "Designer.FontColor"));
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

	subtitleEdit = new QPlainTextEdit(this);
	subtitleEdit->setMaximumHeight(80);
	form->addRow(moduleText("Designer.Subtitle"), subtitleEdit);

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

	/*
	 * Playback, on one row: three settings that are only ever read together, and that appear at
	 * all only once the section holds artwork that can move.
	 */
	auto *playbackRow = new QWidget(this);
	auto *playbackLayout = new QHBoxLayout(playbackRow);
	playbackLayout->setContentsMargins(0, 0, 0, 0);
	logoLoop = new QCheckBox(moduleText("Designer.LogoLoop"), playbackRow);
	logoLoop->setToolTip(moduleText("Designer.LogoLoop.Tip"));
	logoStartOnEnter = new QCheckBox(moduleText("Designer.LogoStartOnEnter"), playbackRow);
	logoStartOnEnter->setToolTip(moduleText("Designer.LogoStartOnEnter.Tip"));
	logoSpeed = new QDoubleSpinBox(playbackRow);
	logoSpeed->setRange(kMinLogoSpeed, kMaxLogoSpeed);
	logoSpeed->setSingleStep(0.1);
	logoSpeed->setDecimals(2);
	logoSpeed->setPrefix(moduleText("Designer.LogoSpeed") + QStringLiteral(" "));
	logoSpeed->setSuffix(QStringLiteral("x"));
	logoSpeed->setToolTip(moduleText("Designer.LogoSpeed.Tip"));
	playbackLayout->addWidget(logoLoop);
	playbackLayout->addWidget(logoStartOnEnter);
	playbackLayout->addWidget(logoSpeed);
	playbackLayout->addStretch();
	form->addRow(moduleText("Designer.LogoPlayback"), playbackRow);

	logoAnimatedShadow = new QCheckBox(moduleText("Designer.LogoAnimatedShadow"), this);
	logoAnimatedShadow->setToolTip(moduleText("Designer.LogoAnimatedShadow.Tip"));
	form->addRow(QString(), logoAnimatedShadow);

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

	bridgeMinGap = new QSpinBox(this);
	bridgeMinGap->setRange(0, 2048);
	bridgeMinGap->setSuffix(QStringLiteral(" px"));
	bridgeMinGap->setToolTip(moduleText("Designer.BridgeMinGap.Tip"));
	form->addRow(moduleText("Designer.BridgeMinGap"), bridgeMinGap);

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

	rowSubtitles = new QCheckBox(moduleText("Designer.RowSubtitles"), this);
	rowSubtitles->setToolTip(moduleText("Designer.RowSubtitles.Tip"));
	form->addRow(QString(), rowSubtitles);

	/*
	 * The divider's three artwork slots. Each picker is filled from the shape library filtered
	 * by the slot it serves, so a shape offered in two places is one row in that table rather
	 * than one entry per picker here.
	 */
	const auto addShapeRow = [&](QComboBox **box, QLineEdit **path, DividerRole role, const char *boxKey,
				     const char *pathKey) {
		*box = new QComboBox(this);
		for (DividerShape shape : dividerShapesForRole(role))
			(*box)->addItem(dividerShapeText(shape), static_cast<int>(shape));
		form->addRow(moduleText(boxKey), *box);

		auto *row = new QWidget(this);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		*path = new QLineEdit(row);
		auto *browse = new QToolButton(row);
		browse->setText(QStringLiteral("..."));
		layout->addWidget(*path);
		layout->addWidget(browse);
		form->addRow(moduleText(pathKey), row);

		QLineEdit *target = *path;
		connect(browse, &QToolButton::clicked, this, [this, target] { browseForDividerSvg(target); });
	};

	dividerMirrorEnds = new QCheckBox(moduleText("Designer.DividerMirrorEnds"), this);
	dividerMirrorEnds->setToolTip(moduleText("Designer.DividerMirrorEnds.Tip"));
	form->addRow(QString(), dividerMirrorEnds);

	addShapeRow(&dividerArm, &dividerArmSvgPath, DividerRoleArm, "Designer.DividerArm", "Designer.DividerArmSvg");

	dividerThickness = new QSpinBox(this);
	dividerThickness->setRange(1, 1024);
	dividerThickness->setSuffix(QStringLiteral(" px"));
	dividerThickness->setToolTip(moduleText("Designer.DividerThickness.Tip"));
	form->addRow(moduleText("Designer.DividerThickness"), dividerThickness);

	dividerGap = new QSpinBox(this);
	dividerGap->setRange(0, 2048);
	dividerGap->setSuffix(QStringLiteral(" px"));
	dividerGap->setToolTip(moduleText("Designer.DividerGap.Tip"));
	form->addRow(moduleText("Designer.DividerGap"), dividerGap);

	dividerPieceGap = new QSpinBox(this);
	dividerPieceGap->setRange(0, 2048);
	dividerPieceGap->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.DividerPieceGap"), dividerPieceGap);

	dividerRules = new QSpinBox(this);
	dividerRules->setRange(1, 16);
	dividerRules->setToolTip(moduleText("Designer.DividerRules.Tip"));
	form->addRow(moduleText("Designer.DividerRules"), dividerRules);

	dividerRuleGap = new QSpinBox(this);
	dividerRuleGap->setRange(0, 2048);
	dividerRuleGap->setSuffix(QStringLiteral(" px"));
	form->addRow(moduleText("Designer.DividerRuleGap"), dividerRuleGap);

	dividerRuleInset = new QSpinBox(this);
	dividerRuleInset->setRange(0, 4096);
	dividerRuleInset->setSuffix(QStringLiteral(" px"));
	dividerRuleInset->setToolTip(moduleText("Designer.DividerRuleInset.Tip"));
	form->addRow(moduleText("Designer.DividerRuleInset"), dividerRuleInset);

	dividerTint = new QCheckBox(moduleText("Designer.DividerTint"), this);
	form->addRow(QString(), dividerTint);

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

	subtitleGap = new QSpinBox(this);
	subtitleGap->setRange(0, 2048);
	subtitleGap->setSuffix(QStringLiteral(" px"));
	subtitleGap->setToolTip(moduleText("Designer.SubtitleGap.Tip"));
	form->addRow(moduleText("Designer.SubtitleGap"), subtitleGap);

	subtitleOrder = new QComboBox(this);
	subtitleOrder->addItem(moduleText("Designer.SubtitleOrder.TitleFirst"), 0);
	subtitleOrder->addItem(moduleText("Designer.SubtitleOrder.SubtitleFirst"), 1);
	subtitleOrder->setToolTip(moduleText("Designer.SubtitleOrder.Tip"));
	form->addRow(moduleText("Designer.SubtitleOrder"), subtitleOrder);

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

	/*
	 * Unchecked, the bridge is drawn in the section's own style, which is what makes a leader
	 * read as part of the row. Checked, it keeps the row's font and takes its ink from here --
	 * yellow dots under white names, or a gradient across a run of diamonds.
	 */
	bridgeStyleGroup = new QGroupBox(moduleText("Designer.BridgeStyle"), this);
	bridgeStyleGroup->setCheckable(true);
	bridgeStyleGroup->setToolTip(moduleText("Designer.BridgeStyle.Tip"));
	auto *bridgeStyleLayout = new QVBoxLayout(bridgeStyleGroup);
	bridgeStyle = new StyleEditor(bridgeStyleGroup);
	bridgeStyle->setInkOnly(true);
	bridgeStyleLayout->addWidget(bridgeStyle);
	outer->addWidget(bridgeStyleGroup);

	/*
	 * The two subtitles of a bridged row. Two style groups rather than one, because the two
	 * sides of the row are already styled apart and a subtitle that could not follow the line it
	 * belongs under would be the one part of the row unable to. Neither is checkable: a subtitle
	 * is drawn whenever the row's subtitles are on and there is something in it to draw, which
	 * is what the entry's own cell already says.
	 */
	rowSubtitleStyleGroup = new QGroupBox(moduleText("Designer.RowSubtitleStyle"), this);
	auto *rowSubtitleLayout = new QVBoxLayout(rowSubtitleStyleGroup);
	rowSubtitleStyle = new StyleEditor(rowSubtitleStyleGroup);
	rowSubtitleLayout->addWidget(rowSubtitleStyle);
	outer->addWidget(rowSubtitleStyleGroup);

	rowSecondarySubtitleStyleGroup = new QGroupBox(moduleText("Designer.RowSecondarySubtitleStyle"), this);
	auto *rowSecondarySubtitleLayout = new QVBoxLayout(rowSecondarySubtitleStyleGroup);
	rowSecondarySubtitleStyle = new StyleEditor(rowSecondarySubtitleStyleGroup);
	rowSecondarySubtitleLayout->addWidget(rowSecondarySubtitleStyle);
	outer->addWidget(rowSecondarySubtitleStyleGroup);

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
	 * The divider's three piece stacks: its two ends and its middle.
	 *
	 * One table apiece, in tabs, rather than one table and a selector -- an end and a middle
	 * hold exactly the same kind of piece, and tabs are what say so while keeping three lists
	 * out of one pane. A table of its own rather than a second mode of the entry table above:
	 * a piece is a kind, a shape, a word and a size, which shares no column with a credit.
	 */
	dividerPiecesGroup = new QGroupBox(moduleText("Designer.DividerPieces"), this);
	dividerPiecesGroup->setToolTip(moduleText("Designer.DividerPieces.Tip"));
	auto *dividerPiecesLayout = new QVBoxLayout(dividerPiecesGroup);

	pieceTabs = new QTabWidget(dividerPiecesGroup);
	dividerPiecesLayout->addWidget(pieceTabs);

	static const char *const kSlotTitles[kPieceSlotCount] = {"Designer.DividerLeftEnd", "Designer.DividerCentre",
								 "Designer.DividerRightEnd"};

	for (int index = 0; index < kPieceSlotCount; ++index) {
		const auto slot = static_cast<PieceSlot>(index);

		auto *page = new QWidget(pieceTabs);
		auto *pageLayout = new QVBoxLayout(page);
		pageLayout->setContentsMargins(0, 6, 0, 0);

		auto *table = new QTableWidget(page);
		pieceTables[index] = table;
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->verticalHeader()->setVisible(false);
		table->setColumnCount(PieceColumnCount);
		table->setHorizontalHeaderLabels(
			{moduleText("Designer.Column.PieceKind"), moduleText("Designer.Column.PieceShape"),
			 moduleText("Designer.Column.PieceValue"), moduleText("Designer.Column.PieceSize")});
		table->setMinimumHeight(kCentreTableMinimumHeight);

		/*
		 * The value column is the one thing in a row long enough to need reading -- a word,
		 * or a path to a file -- so it takes the slack and the other three keep only what
		 * they need.
		 */
		QHeaderView *header = table->horizontalHeader();
		header->setStretchLastSection(false);
		header->setSectionResizeMode(QHeaderView::Interactive);
		header->setSectionResizeMode(PieceValue, QHeaderView::Stretch);
		table->setColumnWidth(PieceKind, kPieceKindColumnWidth);
		table->setColumnWidth(PieceSize, kPieceSizeColumnWidth);

		pageLayout->addWidget(table);

		auto *buttonRow = new QHBoxLayout();
		const auto addPieceButton = [&](QToolButton *button, auto slotFn) {
			buttonRow->addWidget(button);
			connect(button, &QToolButton::clicked, this, slotFn);
			return button;
		};

		addPieceButton(makeGlyphButton(page, QStringLiteral("+"), moduleText("Designer.AddPiece")),
			       [this, slot] { addPiece(slot); });
		addPieceButton(makeGlyphButton(page, QStringLiteral("−"), moduleText("Designer.RemovePiece")),
			       [this, slot] { removeSelectedPieces(slot); });
		addPieceButton(makeArrowButton(page, Qt::UpArrow, moduleText("Designer.MoveUp")),
			       [this, slot] { movePiece(slot, -1); });
		addPieceButton(makeArrowButton(page, Qt::DownArrow, moduleText("Designer.MoveDown")),
			       [this, slot] { movePiece(slot, 1); });
		/*
		 * One button for both kinds of file a piece can carry, because which one it opens
		 * follows from the row it is pointed at: a logo piece wants an image, a custom
		 * ornament an SVG.
		 */
		pieceFileButtons[index] =
			addPieceButton(makeLabelledButton(page, moduleText("Designer.SetPieceFile")),
				       [this, slot] { browseForPieceFile(slot); });
		buttonRow->addStretch();

		pageLayout->addLayout(buttonRow);
		pieceTabs->addTab(page, moduleText(kSlotTitles[index]));

		connect(table, &QTableWidget::itemChanged, this, [this] { emitChanged(); });
	}

	outer->addWidget(dividerPiecesGroup, 1);


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
		rebuildEntryTable(type, rowSubtitles->isChecked());
		emitChanged();
	});

	connect(labelEdit, &QLineEdit::textChanged, this, notify);
	connect(visibleBox, &QCheckBox::toggled, this, notify);
	connect(textEdit, &QPlainTextEdit::textChanged, this, notify);
	connect(subtitleEdit, &QPlainTextEdit::textChanged, this, notify);
	connect(logoPath, &QLineEdit::textChanged, this, notify);
	/* The controls come and go with what the file turns out to be, so a retyped path re-asks. */
	connect(logoPath, &QLineEdit::textChanged, this, &SectionEditor::refreshLogoPlayback);
	connect(logoHeight, &QSpinBox::valueChanged, this, notify);
	connect(logoLoop, &QCheckBox::toggled, this, notify);
	connect(logoStartOnEnter, &QCheckBox::toggled, this, notify);
	connect(logoSpeed, &QDoubleSpinBox::valueChanged, this, notify);
	connect(logoAnimatedShadow, &QCheckBox::toggled, this, notify);
	connect(logoSide, &QComboBox::currentIndexChanged, this, notify);
	connect(logoGap, &QSpinBox::valueChanged, this, notify);
	connect(bridgeEdit, &QLineEdit::textChanged, this, notify);
	connect(bridgeSvgPath, &QLineEdit::textChanged, this, notify);
	connect(bridgeThickness, &QSpinBox::valueChanged, this, notify);
	connect(bridgeOffset, &QSpinBox::valueChanged, this, notify);
	connect(bridgeGap, &QSpinBox::valueChanged, this, notify);
	connect(bridgeMinGap, &QSpinBox::valueChanged, this, notify);
	connect(bridgeSplit, &QSpinBox::valueChanged, this, notify);
	connect(bridgeRowAlign, &QComboBox::currentIndexChanged, this, notify);
	connect(bridgeSpanEmpty, &QCheckBox::toggled, this, notify);

	/* These decide which of the other bridge rows are worth showing. */
	const auto revisitVisibility = [this] {
		if (loading)
			return;

		applyTypeVisibility(static_cast<SectionType>(typeBox->currentData().toInt()));
		emitChanged();
	};

	for (QComboBox *box : {bridgeType, bridgeFill, bridgeSizing, logoPlacement})
		connect(box, &QComboBox::currentIndexChanged, this, revisitVisibility);

	/* Untinting a custom bridge leaves nothing for the bridge's own colours to reach. */
	connect(bridgeTint, &QCheckBox::toggled, this, revisitVisibility);

	connect(dividerArmSvgPath, &QLineEdit::textChanged, this, notify);
	connect(dividerThickness, &QSpinBox::valueChanged, this, notify);
	connect(dividerGap, &QSpinBox::valueChanged, this, notify);
	connect(dividerPieceGap, &QSpinBox::valueChanged, this, notify);
	connect(dividerRules, &QSpinBox::valueChanged, this, notify);
	connect(dividerRuleGap, &QSpinBox::valueChanged, this, notify);
	connect(dividerRuleInset, &QSpinBox::valueChanged, this, notify);

	/*
	 * Each of these decides which of the other divider rows is worth showing: a slot set to a
	 * custom shape reveals its file picker, mirroring hides the second end, and untinting a
	 * custom file leaves nothing for the artwork's own colours to reach.
	 */
	for (QComboBox *box : {dividerArm})
		connect(box, &QComboBox::currentIndexChanged, this, revisitVisibility);

	connect(dividerMirrorEnds, &QCheckBox::toggled, this, revisitVisibility);
	connect(dividerTint, &QCheckBox::toggled, this, revisitVisibility);
	connect(columns, &QSpinBox::valueChanged, this, notify);
	connect(columnGap, &QSpinBox::valueChanged, this, notify);
	connect(fillOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(entryGap, &QSpinBox::valueChanged, this, notify);
	connect(subtitleGap, &QSpinBox::valueChanged, this, notify);
	connect(subtitleOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(spacerHeight, &QSpinBox::valueChanged, this, notify);
	connect(paddingTop, &QSpinBox::valueChanged, this, notify);
	connect(paddingBottom, &QSpinBox::valueChanged, this, notify);
	connect(marginX, &QSpinBox::valueChanged, this, notify);
	connect(sectionWidth, &QSpinBox::valueChanged, this, notify);
	connect(sectionAlign, &QComboBox::currentIndexChanged, this, notify);
	connect(primaryStyle, &StyleEditor::changed, this, notify);
	connect(secondaryStyle, &StyleEditor::changed, this, notify);
	connect(bridgeStyle, &StyleEditor::changed, this, notify);
	connect(rowSubtitleStyle, &StyleEditor::changed, this, notify);
	connect(rowSecondarySubtitleStyle, &StyleEditor::changed, this, notify);

	/*
	 * Preset edits are routed up to the designer, which owns the document the presets live
	 * on. `presetOrigin` marks the editor mid-signal so the synchronous round trip back
	 * through setPresets() leaves the fields being typed into alone.
	 */
	for (StyleEditor *editor : {primaryStyle, secondaryStyle, bridgeStyle, rowSubtitleStyle,
				    rowSecondarySubtitleStyle}) {
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
	/*
	 * The subtitle columns of the entry table come and go with this, so it rebuilds the table
	 * rather than only re-rendering. Reading the section back first is what carries what is
	 * already typed into the rebuild instead of taking the rebuild from stale entries.
	 */
	connect(rowSubtitles, &QCheckBox::toggled, this, [this] {
		if (loading)
			return;

		const Section held = section();
		const QSignalBlocker blocker(entryTable);
		rebuildEntryTable(held.type, held.rowSubtitles);
		writeEntriesToTable(held);
		applyTypeVisibility(held.type);
		emitChanged();
	});
	connect(bridgeStyleGroup, &QGroupBox::toggled, this, notify);
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
	subtitleEdit->setPlainText(source.secondaryText);
	logoPath->setText(source.logo.path);
	/*
	 * Taken from the section's own logo, which is the one the fields describe. For a list the
	 * entries carry their own copies, and the first of them is as good an answer as any -- they
	 * are written together by this editor, so they only differ in a document that came from
	 * somewhere else.
	 */
	{
		const LogoPlayback &playback = !source.entries.isEmpty() && sectionUsesLogos(source.type) &&
							       sectionUsesEntries(source.type)
						       ? source.entries.first().logo.playback
						       : source.logo.playback;
		logoLoop->setChecked(playback.loop);
		logoStartOnEnter->setChecked(playback.startOnEnter);
		logoSpeed->setValue(playback.speed);
		logoAnimatedShadow->setChecked(playback.animatedShadow);
	}
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
	bridgeMinGap->setValue(qRound(source.bridgeMinGap));
	bridgeTint->setChecked(source.bridgeTint);
	selectByData(bridgeFill, static_cast<int>(source.bridgeFill));
	selectByData(bridgeSizing, static_cast<int>(source.bridgeSizing));
	bridgeSplit->setValue(qRound(source.bridgeSplit * 100.0));
	selectByData(bridgeRowAlign, static_cast<int>(source.bridgeRowAlign));
	bridgeSpanEmpty->setChecked(source.bridgeSpanEmpty);
	rowSubtitles->setChecked(source.rowSubtitles);
	dividerMirrorEnds->setChecked(source.dividerMirrorEnds);
	selectByData(dividerArm, static_cast<int>(source.dividerArm));
	dividerArmSvgPath->setText(source.dividerArmSvg);
	dividerThickness->setValue(qRound(source.dividerThickness));
	dividerGap->setValue(qRound(source.dividerGap));
	dividerPieceGap->setValue(qRound(source.dividerPieceGap));
	dividerRules->setValue(source.dividerRules);
	dividerRuleGap->setValue(qRound(source.dividerRuleGap));
	dividerRuleInset->setValue(qRound(source.dividerRuleInset));
	dividerTint->setChecked(source.dividerTint);
	columns->setValue(source.columns);
	columnGap->setValue(source.columnGap);
	selectByData(fillOrder, source.fillAcross ? 1 : 0);
	entryGap->setValue(source.entryGap);
	subtitleGap->setValue(source.subtitleGap);
	selectByData(subtitleOrder, source.subtitleFirst ? 1 : 0);
	spacerHeight->setValue(source.spacerHeight);
	paddingTop->setValue(source.paddingTop);
	paddingBottom->setValue(source.paddingBottom);
	marginX->setValue(source.marginX);
	sectionWidth->setValue(std::clamp(qRound(source.sectionWidth * 100.0), 1, 100));
	selectByData(sectionAlign, static_cast<int>(source.sectionAlign));

	primaryStyle->setStyle(source.style);
	secondaryStyle->setStyle(source.secondaryStyle);
	bridgeStyle->setStyle(source.bridgeStyle);
	rowSubtitleStyle->setStyle(source.rowSubtitleStyle);
	rowSecondarySubtitleStyle->setStyle(source.rowSecondarySubtitleStyle);
	/* After setStyle, so a bound preset's values win over the section's own copy. */
	primaryStyle->setPresets(presets, source.stylePresetName);
	secondaryStyle->setPresets(presets, source.secondaryStylePresetName);
	bridgeStyle->setPresets(presets, source.bridgeStylePresetName);
	rowSubtitleStyle->setPresets(presets, source.rowSubtitleStylePresetName);
	rowSecondarySubtitleStyle->setPresets(presets, source.rowSecondarySubtitleStylePresetName);
	secondaryGroup->setChecked(source.useSecondaryStyle);
	bridgeStyleGroup->setChecked(source.useBridgeStyle);

	/*
	 * The centre table is filled before the visibility pass rather than after it, because that
	 * pass asks the table whether any piece is drawn from a file. Filling it afterwards would
	 * have the question answered from the section the user just clicked away from.
	 */
	for (int index = 0; index < kPieceSlotCount; ++index)
		writePiecesToTable(static_cast<PieceSlot>(index), source);
	applyTypeVisibility(source.type);
	rebuildEntryTable(source.type, source.rowSubtitles);
	writeEntriesToTable(source);
	/* After the tables, since what it asks about includes the artwork they hold. */
	refreshLogoPlayback();

	loading = false;
}

Section SectionEditor::section() const
{
	Section result = current;

	result.type = static_cast<SectionType>(typeBox->currentData().toInt());
	result.label = labelEdit->text();
	result.visible = visibleBox->isChecked();
	result.text = textEdit->toPlainText();
	result.secondaryText = subtitleEdit->toPlainText();
	result.logo.path = logoPath->text();
	result.logo.maxHeight = logoHeight->value();

	/*
	 * One set of controls, written to every logo the section places -- its own, its entries' and
	 * its divider centre's alike. The entry table rebuilds each Entry from its cells, so a
	 * playback setting that lived only on an entry would be dropped by the next keystroke; this
	 * is also what keeps a grid of sponsor logos animating as one block rather than as twelve
	 * things that drifted apart.
	 */
	const LogoPlayback playback = currentLogoPlayback();
	result.logo.playback = playback;
	result.logoPlacement = static_cast<LogoPlacement>(logoPlacement->currentData().toInt());
	result.logoSide = static_cast<LogoSide>(logoSide->currentData().toInt());
	result.logoGap = logoGap->value();
	result.bridgeType = static_cast<BridgeType>(bridgeType->currentData().toInt());
	result.bridge = bridgeEdit->text();
	result.bridgeSvg = bridgeSvgPath->text();
	result.bridgeThickness = bridgeThickness->value();
	result.bridgeOffset = bridgeOffset->value();
	result.bridgeGap = bridgeGap->value();
	result.bridgeMinGap = bridgeMinGap->value();
	result.bridgeTint = bridgeTint->isChecked();
	result.bridgeFill = static_cast<BridgeFill>(bridgeFill->currentData().toInt());
	result.bridgeSizing = static_cast<BridgeSizing>(bridgeSizing->currentData().toInt());
	result.bridgeSplit = bridgeSplit->value() / 100.0;
	result.bridgeRowAlign = static_cast<HAlign>(bridgeRowAlign->currentData().toInt());
	result.bridgeSpanEmpty = bridgeSpanEmpty->isChecked();
	result.rowSubtitles = rowSubtitles->isChecked();
	result.dividerMirrorEnds = dividerMirrorEnds->isChecked();
	result.dividerArm = static_cast<DividerShape>(dividerArm->currentData().toInt());
	result.dividerArmSvg = dividerArmSvgPath->text();
	result.dividerThickness = dividerThickness->value();
	result.dividerGap = dividerGap->value();
	result.dividerPieceGap = dividerPieceGap->value();
	result.dividerRules = dividerRules->value();
	result.dividerRuleGap = dividerRuleGap->value();
	result.dividerRuleInset = dividerRuleInset->value();
	result.dividerTint = dividerTint->isChecked();
	result.columns = columns->value();
	result.columnGap = columnGap->value();
	result.fillAcross = fillOrder->currentData().toInt() == 1;
	result.entryGap = entryGap->value();
	result.subtitleGap = subtitleGap->value();
	result.subtitleFirst = subtitleOrder->currentData().toInt() == 1;
	result.spacerHeight = spacerHeight->value();
	result.paddingTop = paddingTop->value();
	result.paddingBottom = paddingBottom->value();
	result.marginX = marginX->value();
	result.sectionWidth = sectionWidth->value() / 100.0;
	result.sectionAlign = static_cast<HAlign>(sectionAlign->currentData().toInt());
	result.style = primaryStyle->style();
	result.secondaryStyle = secondaryStyle->style();
	result.bridgeStyle = bridgeStyle->style();
	result.rowSubtitleStyle = rowSubtitleStyle->style();
	result.rowSecondarySubtitleStyle = rowSecondarySubtitleStyle->style();
	result.useSecondaryStyle = secondaryGroup->isChecked();
	result.useBridgeStyle = bridgeStyleGroup->isChecked();
	result.stylePresetName = primaryStyle->presetName();
	result.secondaryStylePresetName = secondaryStyle->presetName();
	result.bridgeStylePresetName = bridgeStyle->presetName();
	result.rowSubtitleStylePresetName = rowSubtitleStyle->presetName();
	result.rowSecondarySubtitleStylePresetName = rowSecondarySubtitleStyle->presetName();

	readEntriesFromTable(&result);
	for (int index = 0; index < kPieceSlotCount; ++index)
		readPiecesFromTable(static_cast<PieceSlot>(index), &result);

	/* After both tables, which rebuild their logos from cells that carry no playback of their own. */
	for (Entry &entry : result.entries)
		entry.logo.playback = playback;
	for (DividerPiece &piece : result.dividerCentre)
		piece.logo.playback = playback;

	return result;
}

void SectionEditor::setPresets(const QVector<StylePreset> &newPresets)
{
	presets = newPresets;

	for (StyleEditor *editor :
	     {primaryStyle, secondaryStyle, bridgeStyle, rowSubtitleStyle, rowSecondarySubtitleStyle})
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
	/* Only the "... w/ Logo" types carry a logo alongside text on the section itself. */
	const bool sectionLogo = hasLogos && !hasEntries;
	const bool logoBesideText = sectionLogo && hasText;

	form->setRowVisible(textEdit, singleLineText);
	/*
	 * A heading's own subtitle. The list types stack one too, but theirs is the entry table's
	 * second column, so the field belongs to the single-heading shapes alone.
	 */
	form->setRowVisible(subtitleEdit, singleLineText && sectionUsesSubtitles(type));
	form->setRowVisible(logoPath->parentWidget(), sectionLogo);
	form->setRowVisible(logoHeight, sectionLogo);
	/*
	 * Hidden here and shown again by refreshLogoPlayback, which is the one that knows whether the
	 * artwork moves. Doing it in two steps means every path into this pass -- a type change, a
	 * section switch, a retyped filename -- ends up asking the same question of the same code.
	 */
	form->setRowVisible(logoLoop->parentWidget(), false);
	form->setRowVisible(logoAnimatedShadow, false);
	form->setRowVisible(logoSide, logoBesideText);
	form->setRowVisible(logoGap, logoBesideText);
	const bool bridged = type == SectionType::Bridged;
	/*
	 * The fill the row will really be laid out with, not the one the combo happens to hold: an
	 * empty bridge is always Fixed, and the rows below that turn on the distinction have to
	 * agree with the renderer about which mode is in force or they show settings that do nothing.
	 */
	const auto fill = bridgeTypeIsEmpty(static_cast<BridgeType>(bridgeType->currentData().toInt()))
				  ? BridgeFill::Fixed
				  : static_cast<BridgeFill>(bridgeFill->currentData().toInt());
	const auto sizing = static_cast<BridgeSizing>(bridgeSizing->currentData().toInt());
	const auto placement = static_cast<LogoPlacement>(logoPlacement->currentData().toInt());

	/* A logo row bridged across to its text uses the same bridge fields a Bridged section does. */
	const bool usesBridge = bridged || (logoBesideText && placement == LogoPlacement::Bridged);

	/* Art and text bridges are configured by different halves of the same set of rows. */
	const auto bridgeArt = static_cast<BridgeType>(bridgeType->currentData().toInt());
	const bool drawnArt = usesBridge && bridgeTypeUsesArt(bridgeArt);
	const bool artFromFile = usesBridge && bridgeTypeUsesFile(bridgeArt);
	/*
	 * An empty bridge has one setting of its own -- how much room it keeps -- and no use for the
	 * rest. Its fill row goes with them: nothing is drawn, so the three modes would differ only
	 * in how the text columns come out, which is not what a control called Fill promises. The
	 * renderer lays it out as Fixed to match (see effectiveBridgeFill).
	 */
	const bool emptyBridge = usesBridge && bridgeTypeIsEmpty(bridgeArt);

	form->setRowVisible(logoPlacement, logoBesideText);
	form->setRowVisible(bridgeType, usesBridge);
	form->setRowVisible(bridgeEdit, usesBridge && !drawnArt && !emptyBridge);
	form->setRowVisible(bridgeSvgPath->parentWidget(), artFromFile);
	form->setRowVisible(bridgeThickness, drawnArt);
	form->setRowVisible(bridgeOffset, drawnArt);
	form->setRowVisible(bridgeGap, drawnArt);
	/* Only a Bridged section reserves the gap between two columns; a logo row spans what is left. */
	form->setRowVisible(bridgeMinGap, emptyBridge && bridged);
	/* The built-in tiles are drawn white to be tinted; only a user's file has colours to keep. */
	form->setRowVisible(bridgeTint, artFromFile);
	form->setRowVisible(bridgeFill, usesBridge && !emptyBridge);
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
	form->setRowVisible(rowSubtitles, bridged);

	/*
	 * The two subtitle styles show only while the row actually draws subtitles. They are read
	 * back whether or not they are on screen -- like every other hidden row here -- so turning
	 * the subtitles off and on again returns the styles the user set rather than the defaults.
	 */
	const bool rowSubtitlesOn = bridged && rowSubtitles->isChecked();
	rowSubtitleStyleGroup->setVisible(rowSubtitlesOn);
	rowSecondarySubtitleStyleGroup->setVisible(rowSubtitlesOn);
	/*
	 * The divider's own rows. The arm's file picker follows the shape picked for it, and the
	 * right-hand end's tab follows the mirror toggle: a divider whose two ends are the same list
	 * has one end to edit, and a second tab holding a list nothing draws from invites the reader
	 * to look for a difference that is not there.
	 */
	const bool divider = type == SectionType::SectionDivider;
	const auto slotShape = [](QComboBox *box) {
		return static_cast<DividerShape>(box->currentData().toInt());
	};

	const bool separateEnds = divider && !dividerMirrorEnds->isChecked();
	const bool armFromFile = divider && dividerShapeUsesFile(slotShape(dividerArm));

	pieceTabs->setTabVisible(static_cast<int>(PieceSlot::RightEnd), separateEnds);
	/*
	 * A tab going away under the reader takes the selection with it, so the left-hand end --
	 * which is what the right one mirrors -- is what they are left looking at.
	 */
	if (!separateEnds && pieceTabs->currentIndex() == static_cast<int>(PieceSlot::RightEnd))
		pieceTabs->setCurrentIndex(static_cast<int>(PieceSlot::LeftEnd));

	form->setRowVisible(dividerMirrorEnds, divider);
	form->setRowVisible(dividerArm, divider);
	form->setRowVisible(dividerArmSvgPath->parentWidget(), armFromFile);
	form->setRowVisible(dividerThickness, divider);
	form->setRowVisible(dividerGap, divider);
	form->setRowVisible(dividerPieceGap, divider);
	form->setRowVisible(dividerRules, divider);
	/* One rule has nothing to be spaced from and nothing to taper against. */
	form->setRowVisible(dividerRuleGap, divider && dividerRules->value() > 1);
	form->setRowVisible(dividerRuleInset, divider && dividerRules->value() > 1);

	/*
	 * The built-in shapes are drawn white to be tinted, so the flag only means anything once
	 * some slot -- an end, an arm, or a piece of the centre stack -- is pointed at a file.
	 */
	const bool dividerFiles = armFromFile || (divider && dividerUsesFile());
	form->setRowVisible(dividerTint, dividerFiles);

	form->setRowVisible(columns, hasColumns);
	form->setRowVisible(columnGap, hasColumns);
	form->setRowVisible(fillOrder, hasColumns);
	form->setRowVisible(entryGap, hasEntries);
	/*
	 * The pair's own two settings apply wherever anything is really stacked, which for a bridged
	 * row is a choice rather than a property of the type -- so this asks about the section
	 * being edited and not only about its type. `secondaryGroup`'s title stays keyed on the
	 * type: for a bridged row the second style is still the right-hand *text*, subtitles or not.
	 */
	const bool hasSubtitles = sectionUsesSubtitles(type);
	const bool stacksSubtitles = hasSubtitles || (bridged && rowSubtitles->isChecked());
	form->setRowVisible(subtitleGap, stacksSubtitles);
	form->setRowVisible(subtitleOrder, stacksSubtitles);
	form->setRowVisible(spacerHeight, type == SectionType::Spacer);

	/*
	 * A divider has a style even though it carries no section text: the artwork is inked from
	 * it, and a word or a mark in the centre stack is drawn with it.
	 */
	primaryStyle->parentWidget()->setVisible(hasText || hasLogos || divider);
	/*
	 * The same style serves whichever second text the type carries, so the group is titled
	 * after the one being edited rather than after the Bridged section it was written for.
	 */
	secondaryGroup->setVisible(sectionUsesSecondaryText(type));
	secondaryGroup->setTitle(hasSubtitles ? moduleText("Designer.SubtitleStyle")
					      : moduleText("Designer.SecondaryStyle"));
	/*
	 * Both shapes that draw a bridge have one to ink separately -- except a custom file left in
	 * the colours it was authored with, which is painted straight to the strip with nothing here
	 * getting a say over it.
	 */
	/*
	 * A divider's artwork is the same thing to ink apart from the text as a bridge is, and takes
	 * the same override -- so the group is on show for both, and titled after whichever is being
	 * edited. It goes away for artwork left in a file's own colours, which nothing here reaches.
	 */
	const bool inkableDivider = divider && !(dividerFiles && !dividerTint->isChecked());
	bridgeStyleGroup->setVisible((usesBridge && !(artFromFile && !bridgeTint->isChecked())) || inkableDivider);
	bridgeStyleGroup->setTitle(divider ? moduleText("Designer.DividerArtStyle")
					   : moduleText("Designer.BridgeStyle"));

	entriesGroup->setVisible(hasEntries);
	/* Nothing for a file picker to fill in when the entries are lines of text. */
	setLogoButton->setVisible(hasEntries && hasLogos);

	dividerPiecesGroup->setVisible(divider);

	/*
	 * Whichever table is on show is the one thing here worth growing, so it takes the leftover
	 * height. With no table at all the trailing spacer takes it instead, which is what keeps the
	 * rows packed at the top rather than spread down the pane.
	 */
	outerLayout->setStretch(trailingStretchIndex, hasEntries || divider ? 0 : 1);
}

void SectionEditor::rebuildEntryTable(SectionType type, bool rowSubtitles)
{
	const QSignalBlocker blocker(entryTable);

	entryTable->clear();
	entryTable->setRowCount(0);

	switch (type) {
	case SectionType::Bridged:
		/*
		 * Each side of the row gains a column of its own when the section draws subtitles,
		 * with the pair kept side by side rather than the two subtitles gathered at the end:
		 * a row is read across, and a subtitle belongs beside the line it sits under.
		 */
		if (rowSubtitles) {
			entryTable->setColumnCount(4);
			entryTable->setHorizontalHeaderLabels({moduleText("Designer.Column.Left"),
							       moduleText("Designer.Column.LeftSubtitle"),
							       moduleText("Designer.Column.Right"),
							       moduleText("Designer.Column.RightSubtitle")});
			break;
		}

		entryTable->setColumnCount(2);
		entryTable->setHorizontalHeaderLabels(
			{moduleText("Designer.Column.Left"), moduleText("Designer.Column.Right")});
		break;

	case SectionType::TitleSubtitleList:
	case SectionType::MultiTitleSubtitleList:
		/*
		 * Headed by what the two texts are rather than by where they end up, so swapping the
		 * order does not relabel the columns the entries were typed into.
		 */
		entryTable->setColumnCount(2);
		entryTable->setHorizontalHeaderLabels(
			{moduleText("Designer.Column.EntryTitle"), moduleText("Designer.Column.Subtitle")});
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
	const bool subtitleColumns = source.type == SectionType::Bridged && source.rowSubtitles;

	entryTable->setRowCount(source.entries.size());
	for (int row = 0; row < source.entries.size(); ++row) {
		const Entry &entry = source.entries.at(row);

		auto *first = new QTableWidgetItem(logoMode ? entry.logo.path : entry.text);
		/* The whole entry travels with the row; see kEntryStashRole. */
		first->setData(kEntryStashRole, entryStash(entry));
		entryTable->setItem(row, 0, first);

		if (logoMode) {
			entryTable->setItem(row, 1, new QTableWidgetItem(QString::number(entry.logo.maxHeight)));
			continue;
		}

		if (subtitleColumns) {
			entryTable->setItem(row, 1, new QTableWidgetItem(entry.subtitle));
			entryTable->setItem(row, 2, new QTableWidgetItem(entry.secondaryText));
			entryTable->setItem(row, 3, new QTableWidgetItem(entry.secondarySubtitle));
			continue;
		}

		if (sectionUsesSecondaryText(source.type))
			entryTable->setItem(row, 1, new QTableWidgetItem(entry.secondaryText));
	}
}

void SectionEditor::readEntriesFromTable(Section *target) const
{
	const bool logoMode = sectionUsesLogos(target->type) && sectionUsesEntries(target->type);
	const bool subtitleColumns = target->type == SectionType::Bridged && target->rowSubtitles;

	QVector<Entry> entries;
	entries.reserve(entryTable->rowCount());

	for (int row = 0; row < entryTable->rowCount(); ++row) {
		const auto cell = [this, row](int column) {
			const QTableWidgetItem *item = entryTable->item(row, column);
			return item ? item->text() : QString();
		};

		/*
		 * Everything the row was last written from, then whatever its visible columns now
		 * hold on top of it. That order is what makes reading a section non-destructive: a
		 * field the table is not currently showing -- a subtitle whose toggle is off, the
		 * text behind a type that shows logos -- comes back from the stash rather than
		 * coming back empty.
		 */
		const QTableWidgetItem *first = entryTable->item(row, 0);
		Entry entry = first ? entryFromStash(first->data(kEntryStashRole)) : Entry();

		if (logoMode) {
			entry.logo.path = cell(0);
			const int height = cell(1).toInt();
			entry.logo.maxHeight = height > 0 ? height : 96;
		} else if (subtitleColumns) {
			entry.text = cell(0);
			entry.subtitle = cell(1);
			entry.secondaryText = cell(2);
			entry.secondarySubtitle = cell(3);
		} else {
			entry.text = cell(0);
			if (sectionUsesSecondaryText(target->type))
				entry.secondaryText = cell(1);
		}

		entries.append(entry);
	}

	target->entries = entries;
}

LogoPlayback SectionEditor::currentLogoPlayback() const
{
	LogoPlayback playback;
	playback.loop = logoLoop->isChecked();
	playback.startOnEnter = logoStartOnEnter->isChecked();
	playback.speed = logoSpeed->value();
	playback.animatedShadow = logoAnimatedShadow->isChecked();
	return playback;
}

bool SectionEditor::sectionHasAnimatedArt(const Section &source) const
{
	if (sectionUsesLogos(source.type) && !source.logo.isEmpty() && logoPathLooksAnimated(source.logo.path))
		return true;

	if (sectionUsesLogos(source.type) && sectionUsesEntries(source.type)) {
		for (const Entry &entry : source.entries) {
			if (!entry.logo.isEmpty() && logoPathLooksAnimated(entry.logo.path))
				return true;
		}
	}

	if (source.type == SectionType::SectionDivider) {
		for (const DividerPiece &piece : source.dividerCentre) {
			if (piece.kind == DividerPiece::Kind::Logo && !piece.logo.isEmpty() &&
			    logoPathLooksAnimated(piece.logo.path))
				return true;
		}
	}

	return false;
}

void SectionEditor::refreshLogoPlayback()
{
	/*
	 * Shown only for a section that actually holds something that moves. Playback settings beside
	 * a PNG describe nothing, and offering them everywhere would put four dead controls on the
	 * majority of sections in the majority of rolls.
	 */
	const bool animated = sectionHasAnimatedArt(section());

	form->setRowVisible(logoLoop->parentWidget(), animated);
	form->setRowVisible(logoAnimatedShadow, animated);
}

void SectionEditor::writePiecesToTable(PieceSlot slot, const Section &source)
{
	QTableWidget *table = pieceTable(slot);
	const QSignalBlocker blocker(table);

	/*
	 * Cleared for every other type, and read back only for a divider (see readPiecesFromTable),
	 * so a section switched to something else and back keeps the stacks it was built with rather
	 * than being handed an empty table's worth of nothing.
	 */
	table->setRowCount(0);
	if (source.type != SectionType::SectionDivider)
		return;

	/*
	 * A change to either picker can change which of the other cells in the row mean anything,
	 * and whether the divider draws from a file at all -- which is a row of the form above.
	 */
	const auto onPickerChanged = [this, slot](int row) {
		return [this, slot, row] {
			if (loading)
				return;
			applyPieceRowVisibility(slot, row);
			applyTypeVisibility(static_cast<SectionType>(typeBox->currentData().toInt()));
			emitChanged();
		};
	};

	const QVector<DividerPiece> &pieces = dividerPieces(source, slot);

	table->setRowCount(pieces.size());
	for (int row = 0; row < pieces.size(); ++row) {
		const DividerPiece &piece = pieces.at(row);

		auto *kindBox = new QComboBox(table);
		for (DividerPiece::Kind kind : allDividerPieceKinds())
			kindBox->addItem(dividerPieceKindText(kind), static_cast<int>(kind));
		selectByData(kindBox, static_cast<int>(piece.kind));
		table->setCellWidget(row, PieceKind, kindBox);
		connect(kindBox, &QComboBox::currentIndexChanged, this, onPickerChanged(row));

		/*
		 * One list of shapes for all three stacks: an end and a middle are the same slot as
		 * far as the library is concerned, so whatever is offered in one is offered in the
		 * other. See DividerRolePiece.
		 */
		auto *shapeBox = new QComboBox(table);
		for (DividerShape shape : dividerShapesForRole(DividerRolePiece))
			shapeBox->addItem(dividerShapeText(shape), static_cast<int>(shape));
		selectByData(shapeBox, static_cast<int>(piece.shape));
		table->setCellWidget(row, PieceShape, shapeBox);
		connect(shapeBox, &QComboBox::currentIndexChanged, this, onPickerChanged(row));

		/*
		 * One column for the three things a piece can be given: the word, the image, or the
		 * artwork. They are mutually exclusive by construction -- a piece is one kind -- so
		 * three columns would mean two empty ones on every row.
		 */
		QString value;
		QString size;
		switch (piece.kind) {
		case DividerPiece::Kind::Ornament:
			value = piece.svgPath;
			size = QString::number(piece.scale, 'g', 3);
			break;
		case DividerPiece::Kind::Text:
			value = piece.text;
			break;
		case DividerPiece::Kind::Logo:
			value = piece.logo.path;
			size = QString::number(piece.logo.maxHeight);
			break;
		}

		table->setItem(row, PieceValue, new QTableWidgetItem(value));
		table->setItem(row, PieceSize, new QTableWidgetItem(size));

		applyPieceRowVisibility(slot, row);
	}
}

void SectionEditor::readPiecesFromTable(PieceSlot slot, Section *target) const
{
	/* Only a divider's tables are ever filled, so only a divider's are ever believed. */
	if (target->type != SectionType::SectionDivider)
		return;

	QTableWidget *table = pieceTable(slot);

	QVector<DividerPiece> pieces;
	pieces.reserve(table->rowCount());

	for (int row = 0; row < table->rowCount(); ++row) {
		const auto *kindBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceKind));
		const auto *shapeBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceShape));
		if (!kindBox || !shapeBox)
			continue;

		const auto cell = [table, row](int column) {
			const QTableWidgetItem *item = table->item(row, column);
			return item ? item->text() : QString();
		};

		DividerPiece piece;
		piece.kind = static_cast<DividerPiece::Kind>(kindBox->currentData().toInt());
		piece.shape = static_cast<DividerShape>(shapeBox->currentData().toInt());

		switch (piece.kind) {
		case DividerPiece::Kind::Ornament: {
			piece.svgPath = cell(PieceValue);
			const double scale = cell(PieceSize).toDouble();
			piece.scale = scale > 0.0 ? scale : 1.0;
			break;
		}
		case DividerPiece::Kind::Text:
			piece.text = cell(PieceValue);
			break;
		case DividerPiece::Kind::Logo: {
			piece.logo.path = cell(PieceValue);
			const int height = cell(PieceSize).toInt();
			piece.logo.maxHeight = height > 0 ? height : 96;
			break;
		}
		}

		pieces.append(piece);
	}

	dividerPieces(*target, slot) = pieces;
}

void SectionEditor::applyPieceRowVisibility(PieceSlot slot, int row)
{
	QTableWidget *table = pieceTable(slot);
	const auto *kindBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceKind));
	auto *shapeBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceShape));
	if (!kindBox || !shapeBox)
		return;

	const auto kind = static_cast<DividerPiece::Kind>(kindBox->currentData().toInt());
	const auto shape = static_cast<DividerShape>(shapeBox->currentData().toInt());

	/* Only an ornament comes out of the shape library; a word and a mark are their own shapes. */
	shapeBox->setEnabled(kind == DividerPiece::Kind::Ornament);

	/*
	 * A built-in ornament has nothing to fill in: its artwork is in the table. Everything else
	 * carries either a word, a file, or both a file and a height.
	 */
	const bool hasValue = kind != DividerPiece::Kind::Ornament || dividerShapeUsesFile(shape);
	const bool hasSize = kind != DividerPiece::Kind::Text;

	/*
	 * Greyed rather than blanked, so a cell that stops applying keeps what was typed into it
	 * and gives it back when the kind is switched round again.
	 */
	const auto setEditable = [table, row](int column, bool editable) {
		QTableWidgetItem *item = table->item(row, column);
		if (!item)
			return;

		Qt::ItemFlags flags = Qt::ItemIsSelectable;
		if (editable)
			flags |= Qt::ItemIsEnabled | Qt::ItemIsEditable;
		item->setFlags(flags);
	};

	setEditable(PieceValue, hasValue);
	setEditable(PieceSize, hasSize);
}

bool SectionEditor::dividerUsesFile() const
{
	for (QTableWidget *table : pieceTables) {
		for (int row = 0; row < table->rowCount(); ++row) {
			const auto *kindBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceKind));
			const auto *shapeBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceShape));
			if (!kindBox || !shapeBox)
				continue;

			if (static_cast<DividerPiece::Kind>(kindBox->currentData().toInt()) !=
			    DividerPiece::Kind::Ornament)
				continue;

			if (dividerShapeUsesFile(static_cast<DividerShape>(shapeBox->currentData().toInt())))
				return true;
		}
	}

	return false;
}

/*
 * The four that follow go through the model rather than shuffling cells, because the rows carry
 * combo boxes and QTableWidget::takeItem knows nothing about those. Round-tripping through
 * setSection is the same move importCsv makes, and for the same reason: it is the one path that
 * cannot leave the table and the section disagreeing.
 */
void SectionEditor::addPiece(PieceSlot slot)
{
	Section updated = section();
	dividerPieces(updated, slot).append(DividerPiece{});
	const int added = dividerPieces(updated, slot).size() - 1;
	setSection(updated);

	pieceTable(slot)->setCurrentCell(added, PieceKind);
	emitChanged();
}

void SectionEditor::removeSelectedPieces(PieceSlot slot)
{
	QList<int> rows;
	for (const QModelIndex &index : pieceTable(slot)->selectionModel()->selectedRows())
		rows.append(index.row());

	if (rows.isEmpty())
		return;

	Section updated = section();
	QVector<DividerPiece> &pieces = dividerPieces(updated, slot);

	/* Remove from the bottom up so earlier indices stay valid. */
	std::sort(rows.begin(), rows.end(), std::greater<int>());
	for (int row : rows) {
		if (row >= 0 && row < pieces.size())
			pieces.removeAt(row);
	}

	setSection(updated);
	emitChanged();
}

void SectionEditor::movePiece(PieceSlot slot, int delta)
{
	const int row = pieceTable(slot)->currentRow();

	Section updated = section();
	QVector<DividerPiece> &pieces = dividerPieces(updated, slot);

	const int target = row + delta;
	if (row < 0 || row >= pieces.size() || target < 0 || target >= pieces.size())
		return;

	pieces.swapItemsAt(row, target);
	setSection(updated);

	pieceTable(slot)->setCurrentCell(target, PieceKind);
	emitChanged();
}

void SectionEditor::browseForPieceFile(PieceSlot slot)
{
	QTableWidget *table = pieceTable(slot);
	const int row = table->currentRow();
	if (row < 0)
		return;

	const auto *kindBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceKind));
	const auto *shapeBox = qobject_cast<QComboBox *>(table->cellWidget(row, PieceShape));
	if (!kindBox || !shapeBox)
		return;

	const auto kind = static_cast<DividerPiece::Kind>(kindBox->currentData().toInt());
	const auto shape = static_cast<DividerShape>(shapeBox->currentData().toInt());

	/* A logo piece wants a picture, a custom ornament its artwork; nothing else takes a file. */
	const bool logoPiece = kind == DividerPiece::Kind::Logo;
	if (!logoPiece && !(kind == DividerPiece::Kind::Ornament && dividerShapeUsesFile(shape)))
		return;

	const QTableWidgetItem *existing = table->item(row, PieceValue);
	const QString path = QFileDialog::getOpenFileName(
		this, moduleText(logoPiece ? "Designer.ChooseLogo" : "Designer.ChooseDividerSvg"),
		existing ? existing->text() : QString(), logoPiece ? imageFilter() : svgFilter());
	if (path.isEmpty())
		return;

	table->setItem(row, PieceValue, new QTableWidgetItem(path));

	const QTableWidgetItem *size = table->item(row, PieceSize);
	if (logoPiece && (!size || size->text().isEmpty()))
		table->setItem(row, PieceSize, new QTableWidgetItem(QStringLiteral("96")));

	/* setItem hands back a fresh item, so the row's flags are reapplied over the top of it. */
	applyPieceRowVisibility(slot, row);
}

void SectionEditor::browseForDividerSvg(QLineEdit *target)
{
	if (!target)
		return;

	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.ChooseDividerSvg"), target->text(),
							  svgFilter());
	if (!path.isEmpty())
		target->setText(path);
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
	if (path.isEmpty())
		return;

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

	/*
	 * Set on the item rather than replacing it, so the row keeps the entry stashed on its first
	 * cell -- a new item would come with none and the row's unshown fields would go with it.
	 */
	if (QTableWidgetItem *item = entryTable->item(row, 0))
		item->setText(path);
	else
		entryTable->setItem(row, 0, new QTableWidgetItem(path));

	if (!entryTable->item(row, 1))
		entryTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("96")));

	refreshLogoPlayback();
}

void SectionEditor::importCsv()
{
	const auto type = static_cast<SectionType>(typeBox->currentData().toInt());

	CsvImportDialog dialog(type, rowSubtitles->isChecked(), this);
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
