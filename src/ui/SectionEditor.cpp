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
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <initializer_list>
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
	case PieceSlot::Center:
		break;
	}
	return section.dividerCenter;
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

/* And of the tab column beside it, which holds a step count and never more than two digits. */
constexpr int kEntryIndentColumnWidth = 70;

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
	stash.insert(QStringLiteral("indent"), entry.indent);
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

	entry.indent = stash.value(QStringLiteral("indent")).toInt();

	return entry;
}

/* Height the divider's center table asks for before it starts scrolling, in pixels. A center
 * stack is a handful of pieces where an entry list is a cast, so it asks for less. */
constexpr int kCenterTableMinimumHeight = 150;

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

/* Columns of the divider's center-piece table. */
enum PieceColumn {
	/* Ornament, text or logo. */
	PieceKind = 0,
	/* Ornament pieces only: which shape from the library. */
	PieceShape,
	/* The word, for a text piece; the file, for a logo or a custom ornament. */
	PieceValue,
	/* Ornament pieces only: a multiplier on the size its shape asks for. */
	PieceSize,
	/* Degrees clockwise about the piece's own center; every kind reads it. */
	PieceRotation,
	PieceColumnCount,
};

/* Width of the center table's three narrow columns, none of which holds a long word. */
constexpr int kPieceKindColumnWidth = 110;
constexpr int kPieceSizeColumnWidth = 70;
constexpr int kPieceRotationColumnWidth = 80;

/*
 * Written after every angle in the table and taken off again when one is read, so the column
 * says what its numbers are without a spin box in every row saying it a second time.
 */
constexpr QChar kDegreeSign(0x00b0);

/* An angle as typed, in degrees: the number in the cell, with or without the sign after it. */
double degreesFromCell(const QString &cell)
{
	QString text = cell.trimmed();
	text.remove(kDegreeSign);

	/*
	 * Whatever will not read as a number is nothing turned, which is what an emptied cell and a
	 * mistyped one both plainly mean.
	 */
	return text.trimmed().toDouble();
}

/*
 * The types the picker offers, from which the rest are composed by the switches beside it.
 *
 * Deliberately a list here rather than a flag on the type table: which types a *picker* offers is
 * a decision about this editor, where taking a type apart into switches is a property of the type
 * table itself -- see decomposeSectionType. Everything the twenty types can express is still
 * reachable, so nothing has been taken away by asking three easy questions instead of one hard one.
 */
const QVector<SectionType> &baseSectionTypes()
{
	static const QVector<SectionType> types = {
		SectionType::Title,  SectionType::Header,         SectionType::TextList,    SectionType::Bridged,
		SectionType::Spacer, SectionType::SectionDivider, SectionType::StickyBlock,
	};
	return types;
}

/*
 * What the picker calls a base type.
 *
 * Ordinarily the type's own name, but a base stands for every type composed from it, and one of
 * them is named after only the first: "Text List" is a poor name for the entry that also produces
 * a list of logos. A locale string per base overrides it where that matters.
 */
QString baseTypeLabel(SectionType type)
{
	const QString key = QStringLiteral("Designer.BaseType.") + QString::fromLatin1(sectionTypeId(type));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));

	return text == key ? QString::fromUtf8(sectionTypeName(type)) : text;
}

/* True when the base type is one of the two headings, which are the ones the switches apply to. */
bool isHeadingBase(SectionType base)
{
	return base == SectionType::Title || base == SectionType::Header;
}

/* The one-line description of a type, or nothing when the locale carries none. */
QString sectionTypeHelp(SectionType type)
{
	const QString key = QStringLiteral("Designer.TypeHelp.") + QString::fromLatin1(sectionTypeId(type));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));

	return text == key ? QString() : text;
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

	colorButton = new ColorButton(this);
	colorButton->setDialogTitle(moduleText("Designer.FontColor"));
	layout->addRow(moduleText("Designer.FontColor"), colorButton);

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
	outlineColor = new ColorButton(outlineGroup);
	outlineColor->setDialogTitle(moduleText("Designer.OutlineColor"));
	outlineForm->addRow(moduleText("Designer.OutlineColor"), outlineColor);
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

	shadowColor = new ColorButton(shadowGroup);
	shadowColor->setDialogTitle(moduleText("Designer.ShadowColor"));
	shadowForm->addRow(moduleText("Designer.ShadowColor"), shadowColor);
	layout->addRow(shadowGroup);

	const auto notify = [this] {
		notifyEdited();
	};

	connect(fontButton, &QPushButton::clicked, this, &StyleEditor::pickFont);
	connect(pixelSize, &QSpinBox::valueChanged, this, notify);
	connect(alignment, &QComboBox::currentIndexChanged, this, notify);
	connect(lineSpacing, &QDoubleSpinBox::valueChanged, this, notify);
	connect(colorButton, &ColorButton::colorChanged, this, notify);
	connect(fillBox, &QComboBox::currentIndexChanged, this, &StyleEditor::onFillChanged);
	connect(gradientEditor, &GradientEditor::changed, this, [this] {
		gradient = gradientEditor->gradient();
		notifyEdited();
	});
	connect(outlineGroup, &QGroupBox::toggled, this, notify);
	connect(outlineWidth, &QDoubleSpinBox::valueChanged, this, notify);
	connect(outlineColor, &ColorButton::colorChanged, this, notify);
	connect(shadowGroup, &QGroupBox::toggled, this, notify);
	connect(shadowOffsetX, &QSpinBox::valueChanged, this, notify);
	connect(shadowOffsetY, &QSpinBox::valueChanged, this, notify);
	connect(shadowBlur, &QSpinBox::valueChanged, this, notify);
	connect(shadowColor, &ColorButton::colorChanged, this, notify);
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
	colorButton->setColor(style.color);
	selectByData(alignment, static_cast<int>(style.align));
	lineSpacing->setValue(style.lineSpacing);

	selectByData(fillBox, static_cast<int>(style.fill));
	gradient = style.gradient;
	gradientEditor->setGradient(gradient);
	gradientEditor->setFill(style.fill);

	outlineGroup->setChecked(style.outline.enabled);
	outlineWidth->setValue(style.outline.width);
	outlineColor->setColor(style.outline.color);

	shadowGroup->setChecked(style.shadow.enabled);
	shadowOffsetX->setValue(qRound(style.shadow.offsetX));
	shadowOffsetY->setValue(qRound(style.shadow.offsetY));
	shadowBlur->setValue(qRound(style.shadow.blur));
	shadowColor->setColor(style.shadow.color);

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

	style.color = colorButton->color();

	style.fill = static_cast<TextFill>(fillBox->currentData().toInt());
	/*
	 * Carried whatever the fill is, so switching to a solid color to see what the text
	 * looks like underneath and switching back does not cost the stops that were set up.
	 */
	style.gradient = gradient;

	style.outline.enabled = outlineGroup->isChecked();
	style.outline.width = outlineWidth->value();
	style.outline.color = outlineColor->color();

	style.shadow.enabled = shadowGroup->isChecked();
	style.shadow.offsetX = shadowOffsetX->value();
	style.shadow.offsetY = shadowOffsetY->value();
	style.shadow.blur = shadowBlur->value();
	style.shadow.color = shadowColor->color();

	return style;
}

void StyleEditor::setInkOnly(bool value)
{
	inkOnly = value;

	form->setRowVisible(fontButton, !inkOnly);
	form->setRowVisible(pixelSize, !inkOnly);
	form->setRowVisible(alignment, !inkOnly);
	form->setRowVisible(lineSpacing, !inkOnly);

	/* "Font Color" is the wrong name for the color of a run of dots. */
	if (auto *label = qobject_cast<QLabel *>(form->labelForField(colorButton)))
		label->setText(moduleText(inkOnly ? "Designer.InkColor" : "Designer.FontColor"));
}

void StyleEditor::applyFillVisibility()
{
	const auto fill = static_cast<TextFill>(fillBox->currentData().toInt());
	const bool gradientFill = fill != TextFill::Solid;

	form->setRowVisible(colorButton, !gradientFill);
	form->setRowVisible(gradientEditor, gradientFill);
	gradientEditor->setFill(fill);
}

void StyleEditor::onFillChanged()
{
	const auto fill = static_cast<TextFill>(fillBox->currentData().toInt());

	/*
	 * A gradient still on its factory stops is seeded from the solid color the style was
	 * already using, so switching to one starts from the text as it looks now rather than
	 * from an unrelated white-to-gray ramp.
	 */
	if (fill != TextFill::Solid && gradient.stops == GradientSpec().stops) {
		const QColor base = colorButton->color();
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
	/*
	 * A margin of its own, small but not nothing: the editor now sits straight against the
	 * splitter rather than inside a scroll area whose frame used to hold it clear of the handle.
	 */
	outer->setContentsMargins(4, 4, 4, 4);

	/*
	 * The rows above the tab strip, which every tab is read under. See `headerForm`.
	 */
	form = new QFormLayout();
	headerForm = form;
	outer->addLayout(form);

	const auto notify = [this] {
		emitChanged();
	};

	/*
	 * Seven base types and a few switches rather than twenty entries in one list. The twenty are
	 * still what the document holds -- see composedType -- but "a title, with a subtitle, with a
	 * logo" is three plain answers where picking "Title w/ Subtitle & Logo" out of a list of ten
	 * headings is one hard question.
	 */
	typeBox = new QComboBox(this);
	for (SectionType type : baseSectionTypes())
		typeBox->addItem(baseTypeLabel(type), static_cast<int>(type));
	addRow(moduleText("Designer.SectionType"), typeBox);

	/*
	 * What this section is called in the list, directly under what kind of thing it is: the two
	 * together are how a section is found again in a roll of forty, and naming one is the next
	 * thing done after picking the other.
	 */
	labelEdit = new QLineEdit(this);
	labelEdit->setPlaceholderText(moduleText("Designer.LabelPlaceholder"));
	addRow(moduleText("Designer.Label"), labelEdit);

	/* The chosen type in a sentence, under the pair it describes. */
	typeHelp = new QLabel(this);
	typeHelp->setWordWrap(true);
	typeHelp->setEnabled(false);
	addRow(QString(), typeHelp);

	typeSubtitle = new QCheckBox(moduleText("Designer.TypeSubtitle"), this);
	typeSubtitle->setToolTip(moduleText("Designer.TypeSubtitle.Tip"));
	addRow(QString(), typeSubtitle);

	typeLogo = new QCheckBox(moduleText("Designer.TypeLogo"), this);
	typeLogo->setToolTip(moduleText("Designer.TypeLogo.Tip"));
	addRow(QString(), typeLogo);

	typeLogoOnly = new QCheckBox(moduleText("Designer.TypeLogoOnly"), this);
	typeLogoOnly->setToolTip(moduleText("Designer.TypeLogoOnly.Tip"));
	addRow(QString(), typeLogoOnly);

	typeListContent = new QComboBox(this);
	typeListContent->addItem(moduleText("Designer.ListContent.Text"), static_cast<int>(SectionListContent::Text));
	typeListContent->addItem(moduleText("Designer.ListContent.Pairs"), static_cast<int>(SectionListContent::Pairs));
	typeListContent->addItem(moduleText("Designer.ListContent.Logos"), static_cast<int>(SectionListContent::Logos));
	typeListContent->setToolTip(moduleText("Designer.ListContent.Tip"));
	addRow(moduleText("Designer.ListContent"), typeListContent);

	visibleBox = new QCheckBox(moduleText("Designer.Visible"), this);
	addRow(QString(), visibleBox);

	/*
	 * From here everything goes into one of the four tabs, in named groups that fold away. See
	 * EditorTab for why the settings are dealt out rather than stacked.
	 *
	 * All four are built up front and in reading order, so a group only has to say which tab it
	 * belongs to. Which of them are on show follows what is left visible on each page; see
	 * refreshTabVisibility.
	 */
	tabs = new QTabWidget(this);
	outer->addWidget(tabs, 1);

	addTab(EditorTab::Content, moduleText("Designer.Tab.Content"));
	addTab(EditorTab::Layout, moduleText("Designer.Tab.Layout"));
	addTab(EditorTab::Style, moduleText("Designer.Tab.Style"));
	addTab(EditorTab::Background, moduleText("Designer.Tab.Background"));

	connect(tabs, &QTabWidget::currentChanged, this, [this](int index) {
		if (restoringTab || index < 0)
			return;

		desiredTab = index;
	});

	/*
	 * What the section says: its words, and the artwork that stands beside or instead of them.
	 * The table of entries and the divider's piece stacks join it further down -- a list of two
	 * hundred credits is this section's content as much as a heading's one line is.
	 */
	contentGroup = new CollapsibleGroup(moduleText("Designer.Group.Content"), this);
	form = new QFormLayout();
	contentForm = form;
	contentGroup->addLayout(form);
	tabLayout(EditorTab::Content)->addWidget(contentGroup);

	textEdit = new QPlainTextEdit(this);
	textEdit->setMaximumHeight(80);
	addRow(moduleText("Designer.Text"), textEdit);

	subtitleEdit = new QPlainTextEdit(this);
	subtitleEdit->setMaximumHeight(80);
	addRow(moduleText("Designer.Subtitle"), subtitleEdit);

	auto *logoRow = new QWidget(this);
	auto *logoLayout = new QHBoxLayout(logoRow);
	logoLayout->setContentsMargins(0, 0, 0, 0);
	logoPath = new QLineEdit(logoRow);
	logoBrowse = new QToolButton(logoRow);
	logoBrowse->setText(QStringLiteral("..."));
	logoLayout->addWidget(logoPath);
	logoLayout->addWidget(logoBrowse);
	addRow(moduleText("Designer.Logo"), logoRow);

	logoHeight = new QSpinBox(this);
	logoHeight->setRange(1, 4096);
	logoHeight->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.LogoHeight"), logoHeight);

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
	addRow(moduleText("Designer.LogoPlayback"), playbackRow);

	logoAnimatedShadow = new QCheckBox(moduleText("Designer.LogoAnimatedShadow"), this);
	logoAnimatedShadow->setToolTip(moduleText("Designer.LogoAnimatedShadow.Tip"));
	addRow(QString(), logoAnimatedShadow);

	/*
	 * From here on the rows describe how this kind of section is put together rather than what
	 * it says: where the logo sits against the words, what the bridge is made of, what a divider
	 * is composed from, how a block pins. The group is retitled after the selected type, since
	 * "Bridge settings" and "Divider settings" are never on screen at the same time.
	 */
	typeSettingsGroup = new CollapsibleGroup(moduleText("Designer.Group.TypeSettings"), this);
	form = new QFormLayout();
	typeSettingsForm = form;
	typeSettingsGroup->addLayout(form);
	tabLayout(EditorTab::Layout)->addWidget(typeSettingsGroup);

	logoPlacement = new QComboBox(this);
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Hug"), static_cast<int>(LogoPlacement::Hug));
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Edge"), static_cast<int>(LogoPlacement::Edge));
	logoPlacement->addItem(moduleText("Designer.LogoPlacement.Bridged"), static_cast<int>(LogoPlacement::Bridged));
	addRow(moduleText("Designer.LogoPlacement"), logoPlacement);

	logoSide = new QComboBox(this);
	logoSide->addItem(moduleText("Designer.LogoSide.Left"), static_cast<int>(LogoSide::Left));
	logoSide->addItem(moduleText("Designer.LogoSide.Right"), static_cast<int>(LogoSide::Right));
	addRow(moduleText("Designer.LogoSide"), logoSide);

	logoGap = new QSpinBox(this);
	logoGap->setRange(0, 2048);
	logoGap->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.LogoGap"), logoGap);

	bridgeType = new QComboBox(this);
	for (BridgeType type : allBridgeTypes())
		bridgeType->addItem(bridgeTypeText(type), static_cast<int>(type));
	addRow(moduleText("Designer.BridgeType"), bridgeType);

	bridgeEdit = new QLineEdit(this);
	addRow(moduleText("Designer.Bridge"), bridgeEdit);

	auto *bridgeSvgRow = new QWidget(this);
	auto *bridgeSvgLayout = new QHBoxLayout(bridgeSvgRow);
	bridgeSvgLayout->setContentsMargins(0, 0, 0, 0);
	bridgeSvgPath = new QLineEdit(bridgeSvgRow);
	bridgeSvgBrowse = new QToolButton(bridgeSvgRow);
	bridgeSvgBrowse->setText(QStringLiteral("..."));
	bridgeSvgLayout->addWidget(bridgeSvgPath);
	bridgeSvgLayout->addWidget(bridgeSvgBrowse);
	addRow(moduleText("Designer.BridgeSvg"), bridgeSvgRow);

	bridgeThickness = new QSpinBox(this);
	bridgeThickness->setRange(1, 1024);
	bridgeThickness->setSuffix(QStringLiteral(" px"));
	bridgeThickness->setToolTip(moduleText("Designer.BridgeThickness.Tip"));
	addRow(moduleText("Designer.BridgeThickness"), bridgeThickness);

	bridgeOffset = new QSpinBox(this);
	bridgeOffset->setRange(-1024, 1024);
	bridgeOffset->setSuffix(QStringLiteral(" px"));
	bridgeOffset->setToolTip(moduleText("Designer.BridgeOffset.Tip"));
	addRow(moduleText("Designer.BridgeOffset"), bridgeOffset);

	bridgeGap = new QSpinBox(this);
	bridgeGap->setRange(0, 2048);
	bridgeGap->setSuffix(QStringLiteral(" px"));
	bridgeGap->setToolTip(moduleText("Designer.BridgeGap.Tip"));
	addRow(moduleText("Designer.BridgeGap"), bridgeGap);

	bridgeMinGap = new QSpinBox(this);
	bridgeMinGap->setRange(0, 2048);
	bridgeMinGap->setSuffix(QStringLiteral(" px"));
	bridgeMinGap->setToolTip(moduleText("Designer.BridgeMinGap.Tip"));
	addRow(moduleText("Designer.BridgeMinGap"), bridgeMinGap);

	bridgeTint = new QCheckBox(moduleText("Designer.BridgeTint"), this);
	addRow(QString(), bridgeTint);

	bridgeFill = new QComboBox(this);
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Fixed"), static_cast<int>(BridgeFill::Fixed));
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Repeat"), static_cast<int>(BridgeFill::Repeat));
	bridgeFill->addItem(moduleText("Designer.BridgeFill.Stretch"), static_cast<int>(BridgeFill::Stretch));
	bridgeFill->setToolTip(moduleText("Designer.BridgeFill.Tip"));
	addRow(moduleText("Designer.BridgeFill"), bridgeFill);

	bridgeSizing = new QComboBox(this);
	bridgeSizing->addItem(moduleText("Designer.BridgeSizing.Split"), static_cast<int>(BridgeSizing::Split));
	bridgeSizing->addItem(moduleText("Designer.BridgeSizing.Natural"), static_cast<int>(BridgeSizing::Natural));
	bridgeSizing->setToolTip(moduleText("Designer.BridgeSizing.Tip"));
	addRow(moduleText("Designer.BridgeSizing"), bridgeSizing);

	bridgeSplit = new QSpinBox(this);
	bridgeSplit->setRange(0, 100);
	bridgeSplit->setSuffix(QStringLiteral(" %"));
	bridgeSplit->setToolTip(moduleText("Designer.BridgeSplit.Tip"));
	addRow(moduleText("Designer.BridgeSplit"), bridgeSplit);

	bridgeRowAlign = new QComboBox(this);
	addAlignmentOptions(bridgeRowAlign);
	addRow(moduleText("Designer.BridgeRowAlign"), bridgeRowAlign);

	bridgeSpanEmpty = new QCheckBox(moduleText("Designer.BridgeSpanEmpty"), this);
	addRow(QString(), bridgeSpanEmpty);

	rowSubtitles = new QCheckBox(moduleText("Designer.RowSubtitles"), this);
	rowSubtitles->setToolTip(moduleText("Designer.RowSubtitles.Tip"));
	addRow(QString(), rowSubtitles);

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
		addRow(moduleText(boxKey), *box);

		auto *row = new QWidget(this);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		*path = new QLineEdit(row);
		auto *browse = new QToolButton(row);
		browse->setText(QStringLiteral("..."));
		layout->addWidget(*path);
		layout->addWidget(browse);
		addRow(moduleText(pathKey), row);

		QLineEdit *target = *path;
		connect(browse, &QToolButton::clicked, this, [this, target] { browseForDividerSvg(target); });
	};

	dividerMirrorEnds = new QCheckBox(moduleText("Designer.DividerMirrorEnds"), this);
	dividerMirrorEnds->setToolTip(moduleText("Designer.DividerMirrorEnds.Tip"));
	addRow(QString(), dividerMirrorEnds);

	addShapeRow(&dividerArm, &dividerArmSvgPath, DividerRoleArm, "Designer.DividerArm", "Designer.DividerArmSvg");

	dividerThickness = new QSpinBox(this);
	dividerThickness->setRange(1, 1024);
	dividerThickness->setSuffix(QStringLiteral(" px"));
	dividerThickness->setToolTip(moduleText("Designer.DividerThickness.Tip"));
	addRow(moduleText("Designer.DividerThickness"), dividerThickness);

	/*
	 * Joining the parts is the first question about them, so it sits above the two gaps it
	 * makes most of the difference to rather than down among the fine spacing.
	 */
	dividerConnect = new QCheckBox(moduleText("Designer.DividerConnect"), this);
	dividerConnect->setToolTip(moduleText("Designer.DividerConnect.Tip"));
	addRow(QString(), dividerConnect);

	/*
	 * Both gaps reach below zero, where they stop holding two parts apart and start pushing
	 * them into each other. That is not a second setting: a divider whose cap overlaps its rule
	 * by six pixels is the same edit as one that clears it by six, and one spin box running
	 * through zero is what says so.
	 */
	dividerGap = new QSpinBox(this);
	dividerGap->setRange(-static_cast<int>(kMaxDividerJoin), 2048);
	dividerGap->setSuffix(QStringLiteral(" px"));
	dividerGap->setToolTip(moduleText("Designer.DividerGap.Tip"));
	addRow(moduleText("Designer.DividerGap"), dividerGap);

	dividerPieceGap = new QSpinBox(this);
	dividerPieceGap->setRange(-static_cast<int>(kMaxDividerJoin), 2048);
	dividerPieceGap->setSuffix(QStringLiteral(" px"));
	dividerPieceGap->setToolTip(moduleText("Designer.DividerPieceGap.Tip"));
	addRow(moduleText("Designer.DividerPieceGap"), dividerPieceGap);

	dividerRules = new QSpinBox(this);
	dividerRules->setRange(1, 16);
	dividerRules->setToolTip(moduleText("Designer.DividerRules.Tip"));
	addRow(moduleText("Designer.DividerRules"), dividerRules);

	dividerRuleGap = new QSpinBox(this);
	dividerRuleGap->setRange(0, 2048);
	dividerRuleGap->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.DividerRuleGap"), dividerRuleGap);

	dividerRuleInset = new QSpinBox(this);
	dividerRuleInset->setRange(0, 4096);
	dividerRuleInset->setSuffix(QStringLiteral(" px"));
	dividerRuleInset->setToolTip(moduleText("Designer.DividerRuleInset.Tip"));
	addRow(moduleText("Designer.DividerRuleInset"), dividerRuleInset);

	dividerTint = new QCheckBox(moduleText("Designer.DividerTint"), this);
	addRow(QString(), dividerTint);

	columns = new QSpinBox(this);
	columns->setRange(1, 12);
	addRow(moduleText("Designer.Columns"), columns);

	columnGap = new QSpinBox(this);
	columnGap->setRange(0, 2048);
	columnGap->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.ColumnGap"), columnGap);

	fillOrder = new QComboBox(this);
	fillOrder->addItem(moduleText("Designer.FillDown"), 0);
	fillOrder->addItem(moduleText("Designer.FillAcross"), 1);
	fillOrder->setToolTip(moduleText("Designer.FillOrder.Tip"));
	addRow(moduleText("Designer.FillOrder"), fillOrder);

	entryGap = new QSpinBox(this);
	entryGap->setRange(0, 2048);
	entryGap->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.EntryGap"), entryGap);

	indentStep = new QSpinBox(this);
	indentStep->setRange(0, 2048);
	indentStep->setSuffix(QStringLiteral(" px"));
	indentStep->setToolTip(moduleText("Designer.IndentStep.Tip"));
	addRow(moduleText("Designer.IndentStep"), indentStep);

	subtitleGap = new QSpinBox(this);
	subtitleGap->setRange(0, 2048);
	subtitleGap->setSuffix(QStringLiteral(" px"));
	subtitleGap->setToolTip(moduleText("Designer.SubtitleGap.Tip"));
	addRow(moduleText("Designer.SubtitleGap"), subtitleGap);

	subtitleOrder = new QComboBox(this);
	subtitleOrder->addItem(moduleText("Designer.SubtitleOrder.TitleFirst"), 0);
	subtitleOrder->addItem(moduleText("Designer.SubtitleOrder.SubtitleFirst"), 1);
	subtitleOrder->setToolTip(moduleText("Designer.SubtitleOrder.Tip"));
	addRow(moduleText("Designer.SubtitleOrder"), subtitleOrder);

	/*
	 * The sticky block's own rows. The pin is a pair of points -- one on the block, one down the
	 * canvas -- because "the middle of the block, halfway down the frame" needs both halves and a
	 * single number can only ever say one of them.
	 */
	stickyAnchor = new QComboBox(this);
	stickyAnchor->addItem(moduleText("Designer.StickyAnchor.Top"), static_cast<int>(StickyAnchor::Top));
	stickyAnchor->addItem(moduleText("Designer.StickyAnchor.Center"), static_cast<int>(StickyAnchor::Center));
	stickyAnchor->addItem(moduleText("Designer.StickyAnchor.Bottom"), static_cast<int>(StickyAnchor::Bottom));
	stickyAnchor->setToolTip(moduleText("Designer.StickyAnchor.Tip"));
	addRow(moduleText("Designer.StickyAnchor"), stickyAnchor);

	stickyCanvasPosition = new QSpinBox(this);
	stickyCanvasPosition->setRange(0, 100);
	stickyCanvasPosition->setSuffix(QStringLiteral(" %"));
	stickyCanvasPosition->setToolTip(moduleText("Designer.StickyCanvasPosition.Tip"));
	addRow(moduleText("Designer.StickyCanvasPosition"), stickyCanvasPosition);

	stickyOffset = new QSpinBox(this);
	stickyOffset->setRange(-4096, 4096);
	stickyOffset->setSuffix(QStringLiteral(" px"));
	stickyOffset->setToolTip(moduleText("Designer.StickyOffset.Tip"));
	addRow(moduleText("Designer.StickyOffset"), stickyOffset);

	stickyHold = new QDoubleSpinBox(this);
	stickyHold->setRange(0.0, 3600.0);
	stickyHold->setDecimals(1);
	stickyHold->setSingleStep(0.5);
	stickyHold->setSuffix(moduleText("Designer.Seconds"));
	stickyHold->setToolTip(moduleText("Designer.StickyHold.Tip"));
	addRow(moduleText("Designer.StickyHold"), stickyHold);

	stickyHoldForever = new QCheckBox(moduleText("Designer.StickyHoldForever"), this);
	stickyHoldForever->setToolTip(moduleText("Designer.StickyHoldForever.Tip"));
	addRow(QString(), stickyHoldForever);

	stickyRelease = new QComboBox(this);
	stickyRelease->addItem(moduleText("Designer.StickyRelease.EndAtHold"),
			       static_cast<int>(StickyRelease::EndAtHold));
	stickyRelease->addItem(moduleText("Designer.StickyRelease.ResumeThenEnd"),
			       static_cast<int>(StickyRelease::ResumeThenEnd));
	stickyRelease->addItem(moduleText("Designer.StickyRelease.ResumeEndAtHold"),
			       static_cast<int>(StickyRelease::ResumeEndAtHold));
	stickyRelease->setToolTip(moduleText("Designer.StickyRelease.Tip"));
	addRow(moduleText("Designer.StickyRelease"), stickyRelease);

	/*
	 * Said on the form rather than in a tooltip, because a block that holds for ever and is set
	 * to end the roll at its hold is a roll with no end -- which is a perfectly reasonable thing
	 * to build on purpose and a baffling thing to meet by accident.
	 */
	stickyForeverWarning = new QLabel(moduleText("Designer.StickyHoldForever.Warning"), this);
	stickyForeverWarning->setWordWrap(true);
	addRow(QString(), stickyForeverWarning);

	spacerHeight = new QSpinBox(this);
	spacerHeight->setRange(0, 20000);
	spacerHeight->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.SpacerHeight"), spacerHeight);

	/* Where the section sits on the canvas, which is the same set of questions for every type. */
	placementGroup = new CollapsibleGroup(moduleText("Designer.Group.Placement"), this);
	form = new QFormLayout();
	placementForm = form;
	placementGroup->addLayout(form);
	tabLayout(EditorTab::Layout)->addWidget(placementGroup);

	paddingTop = new QSpinBox(this);
	paddingTop->setRange(0, 20000);
	paddingTop->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.PaddingTop"), paddingTop);

	paddingBottom = new QSpinBox(this);
	paddingBottom->setRange(0, 20000);
	paddingBottom->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.PaddingBottom"), paddingBottom);

	marginX = new QSpinBox(this);
	marginX->setRange(0, 4096);
	marginX->setSuffix(QStringLiteral(" px"));
	addRow(moduleText("Designer.MarginX"), marginX);

	contentOffsetX = new QSpinBox(this);
	contentOffsetX->setRange(-4096, 4096);
	contentOffsetX->setSuffix(QStringLiteral(" px"));
	contentOffsetX->setToolTip(moduleText("Designer.ContentOffsetX.Tip"));
	addRow(moduleText("Designer.ContentOffsetX"), contentOffsetX);

	sectionWidth = new QSpinBox(this);
	sectionWidth->setRange(1, 100);
	sectionWidth->setSuffix(QStringLiteral(" %"));
	sectionWidth->setToolTip(moduleText("Designer.SectionWidth.Tip"));
	addRow(moduleText("Designer.SectionWidth"), sectionWidth);

	sectionAlign = new QComboBox(this);
	addAlignmentOptions(sectionAlign);
	sectionAlign->setToolTip(moduleText("Designer.SectionAlign.Tip"));
	addRow(moduleText("Designer.SectionAlign"), sectionAlign);

	/*
	 * The styles get a tab to themselves.
	 *
	 * A StyleEditor is the tallest thing in this pane -- a font, a size, a fill with its stops,
	 * an outline, a shadow, an alignment -- and a bridged row with subtitles puts five of them one
	 * under the next. Stacked under the settings above they were the length that made the editor
	 * feel endless; behind a tab they are simply the answer to "what does this look like", which
	 * is a question somebody asks on purpose rather than one they scroll through.
	 *
	 * Two of them keep their checkbox, which is carried in the fold header beside the title rather
	 * than by a QGroupBox: the checkbox still says whether the style applies, and the triangle
	 * beside it says only whether it is on screen. See CollapsibleGroup.
	 */
	styleGroup = new CollapsibleGroup(moduleText("Designer.TextStyle"), this);
	primaryStyle = new StyleEditor(styleGroup->content());
	styleGroup->addWidget(primaryStyle);
	tabLayout(EditorTab::Style)->addWidget(styleGroup);

	secondaryGroup = new CollapsibleGroup(moduleText("Designer.SecondaryStyle"), this);
	secondaryGroup->setCheckable(true);
	secondaryStyle = new StyleEditor(secondaryGroup->content());
	secondaryGroup->addWidget(secondaryStyle);
	tabLayout(EditorTab::Style)->addWidget(secondaryGroup);

	/*
	 * Unchecked, the bridge is drawn in the section's own style, which is what makes a leader
	 * read as part of the row. Checked, it keeps the row's font and takes its ink from here --
	 * yellow dots under white names, or a gradient across a run of diamonds.
	 */
	bridgeStyleGroup = new CollapsibleGroup(moduleText("Designer.BridgeStyle"), this);
	bridgeStyleGroup->setCheckable(true);
	bridgeStyleGroup->setHeaderToolTip(moduleText("Designer.BridgeStyle.Tip"));
	bridgeStyle = new StyleEditor(bridgeStyleGroup->content());
	bridgeStyle->setInkOnly(true);
	bridgeStyleGroup->addWidget(bridgeStyle);
	tabLayout(EditorTab::Style)->addWidget(bridgeStyleGroup);

	/*
	 * The two subtitles of a bridged row. Two style groups rather than one, because the two
	 * sides of the row are already styled apart and a subtitle that could not follow the line it
	 * belongs under would be the one part of the row unable to. Neither is checkable: a subtitle
	 * is drawn whenever the row's subtitles are on and there is something in it to draw, which
	 * is what the entry's own cell already says.
	 */
	rowSubtitleStyleGroup = new CollapsibleGroup(moduleText("Designer.RowSubtitleStyle"), this);
	rowSubtitleStyle = new StyleEditor(rowSubtitleStyleGroup->content());
	rowSubtitleStyleGroup->addWidget(rowSubtitleStyle);
	tabLayout(EditorTab::Style)->addWidget(rowSubtitleStyleGroup);

	rowSecondarySubtitleStyleGroup = new CollapsibleGroup(moduleText("Designer.RowSecondarySubtitleStyle"), this);
	rowSecondarySubtitleStyle = new StyleEditor(rowSecondarySubtitleStyleGroup->content());
	rowSecondarySubtitleStyleGroup->addWidget(rowSecondarySubtitleStyle);
	tabLayout(EditorTab::Style)->addWidget(rowSecondarySubtitleStyleGroup);

	/*
	 * A folding group per background slot, built from the slot table so a slot added to the model
	 * turns up here with nothing to write. Eight of them, one for each thing a section can put a
	 * panel behind, which is why they get a tab rather than sharing one with the styles: a panel is
	 * a different question from the ink on the words in front of it, and eight groups is a page.
	 */
	for (const BackgroundSlot slot : allBackgroundSlots()) {
		const QString title = moduleText(QStringLiteral("Designer.Background.Slot.%1")
							 .arg(QString::fromUtf8(backgroundSlotId(slot)))
							 .toUtf8()
							 .constData());

		auto *group = new CollapsibleGroup(title, this);
		/*
		 * Checkable, and for every slot rather than only the one that needs it. The checkbox says
		 * whether the slot carries a panel at all, which is a different question from what the
		 * panel is filled with -- and for an alternate entry the two really do differ: a list that
		 * alternates onto a panel filled with nothing is how every other row is left bare, where a
		 * list with no alternate draws the same panel behind every row. Making all eight read the
		 * same way costs nothing and means one rule instead of a footnote on one group.
		 */
		group->setCheckable(true);
		group->setHeaderToolTip(moduleText(QStringLiteral("Designer.Background.Slot.%1.Tip")
							   .arg(QString::fromUtf8(backgroundSlotId(slot)))
							   .toUtf8()
							   .constData()));

		auto *editor = new BackgroundEditor(group->content());
		group->addWidget(editor);
		tabLayout(EditorTab::Background)->addWidget(group);

		backgroundGroups.insert(slot, group);
		backgroundEditors.insert(slot, editor);
	}

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
	setLogoButton = addButton(makeLabeledButton(entriesGroup, moduleText("Designer.SetLogo")),
				  &SectionEditor::browseForEntryLogo);
	buttons->addStretch();
	addButton(makeLabeledButton(entriesGroup, moduleText("Designer.ExpandTable"),
				    moduleText("Designer.ExpandTable.Tip")),
		  [this] { expandTable(entryTable, moduleText("Designer.Entries")); });
	addButton(makeLabeledButton(entriesGroup, moduleText("Designer.ImportCsv")), &SectionEditor::importCsv);

	entriesLayout->addLayout(buttons);
	tabLayout(EditorTab::Content)->addWidget(entriesGroup, 1);

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

	static const char *const kSlotTitles[kPieceSlotCount] = {"Designer.DividerLeftEnd", "Designer.DividerCenter",
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
			 moduleText("Designer.Column.PieceValue"), moduleText("Designer.Column.PieceSize"),
			 moduleText("Designer.Column.PieceRotation")});
		table->setMinimumHeight(kCenterTableMinimumHeight);

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
		table->setColumnWidth(PieceRotation, kPieceRotationColumnWidth);
		if (QTableWidgetItem *rotationHeader = table->horizontalHeaderItem(PieceRotation))
			rotationHeader->setToolTip(moduleText("Designer.Column.PieceRotation.Tip"));

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
		pieceFileButtons[index] = addPieceButton(makeLabeledButton(page, moduleText("Designer.SetPieceFile")),
							 [this, slot] { browseForPieceFile(slot); });
		buttonRow->addStretch();
		addPieceButton(makeLabeledButton(page, moduleText("Designer.ExpandTable"),
						 moduleText("Designer.ExpandTable.Tip")),
			       [this, slot] {
				       expandTable(pieceTable(slot),
						   moduleText("Designer.DividerPieces") + QStringLiteral(" - ") +
							   pieceTabs->tabText(static_cast<int>(slot)));
			       });

		pageLayout->addLayout(buttonRow);
		pieceTabs->addTab(page, moduleText(kSlotTitles[index]));

		connect(table, &QTableWidget::itemChanged, this, [this] { emitChanged(); });
	}

	/*
	 * The piece stacks are what a divider is made of, so they go where a list's entries do. A
	 * divider has no words to type, which leaves its Content tab holding these alone -- which is
	 * exactly right: this is the thing somebody opens a divider to work on.
	 */
	tabLayout(EditorTab::Content)->addWidget(dividerPiecesGroup, 1);

	/*
	 * Whatever height is left over when a page is shorter than the tab it sits in. A QVBoxLayout
	 * with nothing to give the slack to shares it out between the items it has, so without this a
	 * type carrying few fields -- Title especially -- has its handful of rows spread down the page
	 * with gaps between them rather than sitting one under the next.
	 *
	 * One per page, and added last, so each page packs its own groups at the top independently of
	 * what the others hold.
	 */
	for (int index = 0; index < kEditorTabCount; ++index) {
		tabLayouts[index]->addStretch();
		tabStretchIndex[index] = tabLayouts[index]->count() - 1;
	}

	/* Every one of these changes which type the picker adds up to, so all of them go one way. */
	connect(typeBox, &QComboBox::currentIndexChanged, this, &SectionEditor::onTypeSwitchChanged);
	connect(typeListContent, &QComboBox::currentIndexChanged, this, &SectionEditor::onTypeSwitchChanged);
	for (QCheckBox *box : {typeSubtitle, typeLogo, typeLogoOnly})
		connect(box, &QCheckBox::toggled, this, &SectionEditor::onTypeSwitchChanged);

	/*
	 * The column count is part of what a list *is* -- one column or several is the difference
	 * between two of the document's types -- so it runs the same path the switches do.
	 */
	connect(columns, &QSpinBox::valueChanged, this, &SectionEditor::onTypeSwitchChanged);

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
	connect(stickyAnchor, &QComboBox::currentIndexChanged, this, notify);
	connect(stickyCanvasPosition, &QSpinBox::valueChanged, this, notify);
	connect(stickyOffset, &QSpinBox::valueChanged, this, notify);
	connect(stickyHold, &QDoubleSpinBox::valueChanged, this, notify);
	connect(stickyRelease, &QComboBox::currentIndexChanged, this, notify);
	/* This decides what else on the form applies, so it re-runs the visibility pass. */
	connect(stickyHoldForever, &QCheckBox::toggled, this, [this] {
		if (loading)
			return;
		applyTypeVisibility(composedType());
		emitChanged();
	});
	connect(bridgeSplit, &QSpinBox::valueChanged, this, notify);
	connect(bridgeRowAlign, &QComboBox::currentIndexChanged, this, notify);
	connect(bridgeSpanEmpty, &QCheckBox::toggled, this, notify);

	/* These decide which of the other bridge rows are worth showing. */
	const auto revisitVisibility = [this] {
		if (loading)
			return;

		applyTypeVisibility(composedType());
		emitChanged();
	};

	for (QComboBox *box : {bridgeType, bridgeFill, bridgeSizing, logoPlacement})
		connect(box, &QComboBox::currentIndexChanged, this, revisitVisibility);

	/* Untinting a custom bridge leaves nothing for the bridge's own colors to reach. */
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
	 * custom file leaves nothing for the artwork's own colors to reach.
	 */
	for (QComboBox *box : {dividerArm})
		connect(box, &QComboBox::currentIndexChanged, this, revisitVisibility);

	connect(dividerMirrorEnds, &QCheckBox::toggled, this, revisitVisibility);
	connect(dividerTint, &QCheckBox::toggled, this, revisitVisibility);
	/* Joining the parts leaves the arm gap with nothing to hold apart, so the row goes with it. */
	connect(dividerConnect, &QCheckBox::toggled, this, revisitVisibility);
	connect(columnGap, &QSpinBox::valueChanged, this, notify);
	connect(fillOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(entryGap, &QSpinBox::valueChanged, this, notify);
	connect(indentStep, &QSpinBox::valueChanged, this, notify);
	connect(contentOffsetX, &QSpinBox::valueChanged, this, notify);
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

	for (BackgroundEditor *editor : backgroundEditors)
		connect(editor, &BackgroundEditor::changed, this, notify);

	/* Switching a slot on or off is a change to the roll; folding one away is not. */
	for (CollapsibleGroup *group : backgroundGroups)
		connect(group, &CollapsibleGroup::toggled, this, notify);

	/*
	 * The panels' preset edits go up the same way the styles' do, through a marker of their own so
	 * the round trip leaves the editor being typed into alone.
	 */
	for (BackgroundEditor *editor : backgroundEditors) {
		connect(editor, &BackgroundEditor::presetSaveRequested, this,
			[this, editor](const QString &name, const BackgroundPanel &panel) {
				backgroundPresetOrigin = editor;
				emit backgroundPresetSaveRequested(name, panel);
				backgroundPresetOrigin = nullptr;
			});
		connect(editor, &BackgroundEditor::presetDeleteRequested, this, [this, editor](const QString &name) {
			backgroundPresetOrigin = editor;
			emit backgroundPresetDeleteRequested(name);
			backgroundPresetOrigin = nullptr;
		});
	}

	/*
	 * Preset edits are routed up to the designer, which owns the document the presets live
	 * on. `presetOrigin` marks the editor mid-signal so the synchronous round trip back
	 * through setPresets() leaves the fields being typed into alone.
	 */
	for (StyleEditor *editor :
	     {primaryStyle, secondaryStyle, bridgeStyle, rowSubtitleStyle, rowSecondarySubtitleStyle}) {
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
	connect(secondaryGroup, &CollapsibleGroup::toggled, this, notify);
	/*
	 * The subtitle columns of the entry table come and go with this, so it rebuilds the table
	 * rather than only re-rendering. Reading the section back first is what carries what is
	 * already typed into the rebuild instead of taking the rebuild from stale entries.
	 */
	connect(rowSubtitles, &QCheckBox::toggled, this, [this] {
		if (loading)
			return;

		const SectionType type = composedType();
		applyTypeVisibility(type);
		relayoutEntryTable(type);
		emitChanged();
	});
	connect(bridgeStyleGroup, &CollapsibleGroup::toggled, this, notify);
	connect(entryTable, &QTableWidget::cellChanged, this, notify);
	connect(logoBrowse, &QToolButton::clicked, this, &SectionEditor::browseForSectionLogo);
	connect(bridgeSvgBrowse, &QToolButton::clicked, this, &SectionEditor::browseForBridgeSvg);
}

void SectionEditor::setSection(const Section &source)
{
	loading = true;
	current = source;

	showTypeAsSwitches(source.type);
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
	dividerConnect->setChecked(source.dividerConnect);
	dividerGap->setValue(qRound(source.dividerGap));
	dividerPieceGap->setValue(qRound(source.dividerPieceGap));
	dividerRules->setValue(source.dividerRules);
	dividerRuleGap->setValue(qRound(source.dividerRuleGap));
	dividerRuleInset->setValue(qRound(source.dividerRuleInset));
	dividerTint->setChecked(source.dividerTint);
	/*
	 * One column or several is the difference between two of the document's types, so the spin
	 * box shows what the *type* says rather than what the field happens to carry: a list that was
	 * once three columns wide and is now a plain one reads as one column, not as three.
	 */
	columns->setValue(sectionUsesColumns(source.type) ? source.columns : 1);
	columnGap->setValue(source.columnGap);
	selectByData(fillOrder, source.fillAcross ? 1 : 0);
	entryGap->setValue(source.entryGap);
	indentStep->setValue(source.indentStep);
	subtitleGap->setValue(source.subtitleGap);
	selectByData(subtitleOrder, source.subtitleFirst ? 1 : 0);
	spacerHeight->setValue(source.spacerHeight);
	selectByData(stickyAnchor, static_cast<int>(source.stickyAnchor));
	stickyCanvasPosition->setValue(qRound(source.stickyCanvasPosition * 100.0));
	stickyOffset->setValue(qRound(source.stickyOffset));
	stickyHold->setValue(source.stickyHold);
	stickyHoldForever->setChecked(source.stickyHoldForever);
	selectByData(stickyRelease, static_cast<int>(source.stickyRelease));

	paddingTop->setValue(source.paddingTop);
	paddingBottom->setValue(source.paddingBottom);
	marginX->setValue(source.marginX);
	contentOffsetX->setValue(source.contentOffsetX);
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

	for (auto it = backgroundEditors.cbegin(); it != backgroundEditors.cend(); ++it) {
		const BackgroundSlot slot = it.key();
		it.value()->setPanel(source.background(slot));
		/* After setPanel, so a bound preset's values win over the slot's own copy. */
		it.value()->setPresets(backgroundPresets, source.backgroundPresetName(slot));
		backgroundGroups.value(slot)->setChecked(source.hasBackground(slot));
	}

	/*
	 * The center table is filled before the visibility pass rather than after it, because that
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

	result.type = composedType();
	result.label = labelEdit->text();
	result.visible = visibleBox->isChecked();
	result.text = textEdit->toPlainText();
	result.secondaryText = subtitleEdit->toPlainText();
	result.logo.path = logoPath->text();
	result.logo.maxHeight = logoHeight->value();

	/*
	 * One set of controls, written to every logo the section places -- its own, its entries' and
	 * its divider center's alike. The entry table rebuilds each Entry from its cells, so a
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
	result.dividerConnect = dividerConnect->isChecked();
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
	result.indentStep = indentStep->value();
	result.subtitleGap = subtitleGap->value();
	result.subtitleFirst = subtitleOrder->currentData().toInt() == 1;
	result.spacerHeight = spacerHeight->value();
	result.stickyAnchor = static_cast<StickyAnchor>(stickyAnchor->currentData().toInt());
	result.stickyCanvasPosition = stickyCanvasPosition->value() / 100.0;
	result.stickyOffset = stickyOffset->value();
	result.stickyHold = stickyHold->value();
	result.stickyHoldForever = stickyHoldForever->isChecked();
	result.stickyRelease = static_cast<StickyRelease>(stickyRelease->currentData().toInt());
	result.paddingTop = paddingTop->value();
	result.paddingBottom = paddingBottom->value();
	result.marginX = marginX->value();
	result.contentOffsetX = contentOffsetX->value();
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

	/*
	 * A slot switched off is removed rather than stored empty, which is what keeps a section that
	 * has never been given a panel writing nothing into the scene collection -- and what an
	 * alternate entry reads to decide whether the list alternates at all.
	 */
	for (auto it = backgroundEditors.cbegin(); it != backgroundEditors.cend(); ++it) {
		const BackgroundSlot slot = it.key();
		if (!backgroundGroups.value(slot)->isChecked()) {
			result.clearBackground(slot);
			continue;
		}

		SectionBackground &entry = result.backgroundEntry(slot);
		entry.panel = it.value()->panel();
		entry.presetName = it.value()->presetName();
	}

	readEntriesFromTable(&result);
	for (int index = 0; index < kPieceSlotCount; ++index)
		readPiecesFromTable(static_cast<PieceSlot>(index), &result);

	/* After both tables, which rebuild their logos from cells that carry no playback of their own. */
	for (Entry &entry : result.entries)
		entry.logo.playback = playback;
	for (DividerPiece &piece : result.dividerCenter)
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

void SectionEditor::setBackgroundPresets(const QVector<BackgroundPreset> &newPresets)
{
	backgroundPresets = newPresets;

	for (BackgroundEditor *editor : backgroundEditors)
		editor->setPresets(backgroundPresets, editor->presetName(), editor != backgroundPresetOrigin);
}

namespace {

/*
 * True when any row of a form is still showing.
 *
 * Asked of the layout rather than of the widgets in it, because a widget's own `isHidden` answers a
 * different question during construction -- a child of a window that has not been shown yet reads
 * as hidden whether or not anything hid it -- and a row nothing has ever set is visible, which is
 * exactly what the layout says and what a scan of widgets would get wrong.
 */
bool formHasVisibleRow(const QFormLayout *form)
{
	for (int row = 0; row < form->rowCount(); ++row) {
		if (form->isRowVisible(row))
			return true;
	}
	return false;
}

} // namespace

void SectionEditor::addRow(const QString &label, QWidget *field)
{
	form->addRow(label, field);
	rowOwner.insert(field, form);
}

void SectionEditor::setRowVisible(QWidget *field, bool visible)
{
	QFormLayout *owner = rowOwner.value(field);
	if (!owner)
		return;

	/* The type is the only thing that decides whether a row applies; nothing is held back. */
	owner->setRowVisible(field, visible);
}

QVBoxLayout *SectionEditor::addTab(EditorTab tab, const QString &title)
{
	const int index = static_cast<int>(tab);

	/*
	 * A scroll area per page rather than one around the whole editor. The header and the tab strip
	 * are then outside every one of them and cannot be scrolled off the top, and each page keeps
	 * the place the reader left it at -- which is the point of tabs: going back to one should be
	 * going back to where you were, not to the top of it.
	 */
	auto *scroll = new QScrollArea(tabs);
	scroll->setWidgetResizable(true);
	/* The tab already draws a frame around the page; a second one inside it is a box in a box. */
	scroll->setFrameShape(QFrame::NoFrame);

	auto *page = new QWidget(scroll);
	auto *layout = new QVBoxLayout(page);
	layout->setContentsMargins(0, 6, 0, 0);
	scroll->setWidget(page);

	tabs->addTab(scroll, title);

	tabLayouts[index] = layout;

	return layout;
}

bool SectionEditor::tabHasVisibleGroup(EditorTab tab) const
{
	const QVBoxLayout *layout = tabLayouts[static_cast<int>(tab)];

	for (int item = 0; item < layout->count(); ++item) {
		const QWidget *widget = layout->itemAt(item)->widget();
		/*
		 * `isHidden` rather than `isVisible`, because a page on a tab nobody is looking at is
		 * itself hidden and every widget on it would answer no. What is being asked here is
		 * whether the group was hidden on purpose, which is the flag `isHidden` carries.
		 */
		if (widget && !widget->isHidden())
			return true;
	}

	return false;
}

void SectionEditor::refreshTabVisibility()
{
	/*
	 * Hiding the tab somebody is on moves the selection, and that move is Qt tidying up rather
	 * than the reader choosing -- so it must not be recorded as the tab to come back to.
	 */
	restoringTab = true;

	for (int index = 0; index < kEditorTabCount; ++index)
		tabs->setTabVisible(index, tabHasVisibleGroup(static_cast<EditorTab>(index)));

	/*
	 * Back to the tab the reader picked, now that it has something on it again. When it still has
	 * not, whatever Qt fell back to is left alone: the choice is remembered, not forced, so
	 * clicking through a run of sections that have no styles does not keep dragging them to a tab
	 * that is empty for the one they are looking at.
	 */
	if (desiredTab >= 0 && desiredTab < kEditorTabCount && tabs->isTabVisible(desiredTab))
		tabs->setCurrentIndex(desiredTab);

	restoringTab = false;
}

SectionType SectionEditor::composedType() const
{
	SectionTypeSwitches switches;
	switches.base = static_cast<SectionType>(typeBox->currentData().toInt());
	switches.subtitle = typeSubtitle->isChecked();
	switches.logo = typeLogo->isChecked();
	switches.logoOnly = typeLogoOnly->isChecked();
	switches.content = static_cast<SectionListContent>(typeListContent->currentData().toInt());
	switches.multiColumn = columns->value() > 1;

	return composeSectionType(switches);
}

void SectionEditor::showTypeAsSwitches(SectionType type)
{
	const SectionTypeSwitches switches = decomposeSectionType(type);

	selectByData(typeBox, static_cast<int>(switches.base));
	typeSubtitle->setChecked(switches.subtitle);
	typeLogo->setChecked(switches.logo);
	typeLogoOnly->setChecked(switches.logoOnly);
	selectByData(typeListContent, static_cast<int>(switches.content));
}

void SectionEditor::onTypeSwitchChanged()
{
	if (loading)
		return;

	const SectionType type = composedType();

	/*
	 * A heading is either a logo or words with a logo beside them, so the two switches that say
	 * which cannot both be on. Turning one off rather than disabling the other keeps the switch
	 * that was just clicked meaning what it said.
	 */
	if (typeLogoOnly->isChecked() && (typeLogo->isChecked() || typeSubtitle->isChecked())) {
		const QSignalBlocker logoBlocker(typeLogo);
		const QSignalBlocker subtitleBlocker(typeSubtitle);
		typeLogo->setChecked(false);
		typeSubtitle->setChecked(false);
	}

	applyTypeVisibility(type);
	relayoutEntryTable(type);
	emitChanged();
}

void SectionEditor::relayoutEntryTable(SectionType type)
{
	/*
	 * The table's columns depend on the type, so changing the type has to take the entries out
	 * through the old columns and put them back through the new ones. Rebuilding without that
	 * leaves an empty table for the next read to believe, which is a list of credits thrown away
	 * by a change of type -- the one thing changing a type is documented never to do.
	 *
	 * Out through the columns the table is showing, then, and in through the ones it is about to
	 * show. The subtitle switch adds and takes away columns of its own, so it is asked the same
	 * way: reading a two-column table with the answer the switch has just been given put every
	 * row's right-hand name into the left-hand subtitle the moment subtitles were switched on.
	 */
	Section held;
	held.type = tableType;
	held.rowSubtitles = tableRowSubtitles;
	readEntriesFromTable(&held);

	const bool subtitles = rowSubtitles->isChecked();
	held.type = type;
	held.rowSubtitles = subtitles;
	rebuildEntryTable(type, subtitles);
	writeEntriesToTable(held);
}

void SectionEditor::applyTypeVisibility(SectionType type)
{
	/*
	 * The picker's own switches first: which of them apply is a property of the base type, and
	 * the rest of this pass reads the type they add up to.
	 */
	const SectionType base = decomposeSectionType(type).base;
	const bool heading = isHeadingBase(base);
	const bool list = base == SectionType::TextList;

	setRowVisible(typeSubtitle, heading && !typeLogoOnly->isChecked());
	setRowVisible(typeLogo, heading && !typeLogoOnly->isChecked());
	setRowVisible(typeLogoOnly, heading);
	setRowVisible(typeListContent, list);

	const QString help = sectionTypeHelp(type);
	typeHelp->setText(help);
	setRowVisible(typeHelp, !help.isEmpty());

	/*
	 * The group of type-specific settings is named after the type in it. A group called
	 * "Settings" says nothing; one called "Divider" says what the reader is looking at, and is
	 * the only label on screen while the group is folded away.
	 */
	typeSettingsGroup->setTitle(
		moduleText("Designer.Group.TypeSettings").arg(QString::fromUtf8(sectionTypeName(type))));

	const bool hasText = sectionUsesText(type);
	const bool hasLogos = sectionUsesLogos(type);
	const bool hasEntries = sectionUsesEntries(type);
	const bool hasColumns = sectionUsesColumns(type);

	/* Single-line text lives on the section; list text lives in the entry table. */
	const bool singleLineText = hasText && !hasEntries;
	/* Only the "... w/ Logo" types carry a logo alongside text on the section itself. */
	const bool sectionLogo = hasLogos && !hasEntries;
	const bool logoBesideText = sectionLogo && hasText;

	setRowVisible(textEdit, singleLineText);
	/*
	 * A heading's own subtitle. The list types stack one too, but theirs is the entry table's
	 * second column, so the field belongs to the single-heading shapes alone.
	 */
	setRowVisible(subtitleEdit, singleLineText && sectionUsesSubtitles(type));
	setRowVisible(logoPath->parentWidget(), sectionLogo);
	setRowVisible(logoHeight, sectionLogo);
	/*
	 * Hidden here and shown again by refreshLogoPlayback, which is the one that knows whether the
	 * artwork moves. Doing it in two steps means every path into this pass -- a type change, a
	 * section switch, a retyped filename -- ends up asking the same question of the same code.
	 */
	setRowVisible(logoLoop->parentWidget(), false);
	setRowVisible(logoAnimatedShadow, false);
	setRowVisible(logoSide, logoBesideText);
	setRowVisible(logoGap, logoBesideText);
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

	setRowVisible(logoPlacement, logoBesideText);
	setRowVisible(bridgeType, usesBridge);
	setRowVisible(bridgeEdit, usesBridge && !drawnArt && !emptyBridge);
	setRowVisible(bridgeSvgPath->parentWidget(), artFromFile);
	setRowVisible(bridgeThickness, drawnArt);
	setRowVisible(bridgeOffset, drawnArt);
	setRowVisible(bridgeGap, drawnArt);
	/* Only a Bridged section reserves the gap between two columns; a logo row spans what is left. */
	setRowVisible(bridgeMinGap, emptyBridge && bridged);
	/* The built-in tiles are drawn white to be tinted; only a user's file has colors to keep. */
	setRowVisible(bridgeTint, artFromFile);
	setRowVisible(bridgeFill, usesBridge && !emptyBridge);
	/* Column sizing and row placement describe two texts, so they stay with that type. */
	setRowVisible(bridgeSizing, bridged);
	/* The split is the tab stop; with Natural sizing the text decides where things land. */
	setRowVisible(bridgeSplit, bridged && sizing == BridgeSizing::Split);
	/*
	 * Only a fixed bridge with natural columns can leave a row narrower than the section.
	 * Every other combination fills the width, so there is nothing left to align.
	 */
	setRowVisible(bridgeRowAlign, bridged && sizing == BridgeSizing::Natural && fill == BridgeFill::Fixed);
	/* A fixed bridge has nothing to run into the space an empty column would free. */
	setRowVisible(bridgeSpanEmpty, bridged && fill != BridgeFill::Fixed);
	setRowVisible(rowSubtitles, bridged);

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
	 * With the right-hand end gone the one tab left is not the left end of anything -- it is
	 * both ends of the rule, drawn twice -- so it says so rather than naming a side the reader
	 * would then look for the opposite of.
	 */
	pieceTabs->setTabText(static_cast<int>(PieceSlot::LeftEnd),
			      moduleText(separateEnds ? "Designer.DividerLeftEnd" : "Designer.DividerBothEnds"));
	/*
	 * A tab going away under the reader takes the selection with it, so the left-hand end --
	 * which is what the right one mirrors -- is what they are left looking at.
	 */
	if (!separateEnds && pieceTabs->currentIndex() == static_cast<int>(PieceSlot::RightEnd))
		pieceTabs->setCurrentIndex(static_cast<int>(PieceSlot::LeftEnd));

	setRowVisible(dividerMirrorEnds, divider);
	setRowVisible(dividerArm, divider);
	setRowVisible(dividerArmSvgPath->parentWidget(), armFromFile);
	setRowVisible(dividerThickness, divider);
	setRowVisible(dividerConnect, divider);
	/*
	 * The arm gap is what holds the rule clear of the cap and the center, and a connected
	 * divider has it clear of neither. Nothing is switched off by hiding it -- the setting keeps
	 * its value and comes straight back when the parts are unjoined.
	 */
	setRowVisible(dividerGap, divider && !dividerConnect->isChecked());
	setRowVisible(dividerPieceGap, divider);
	setRowVisible(dividerRules, divider);
	/* One rule has nothing to be spaced from and nothing to taper against. */
	setRowVisible(dividerRuleGap, divider && dividerRules->value() > 1);
	setRowVisible(dividerRuleInset, divider && dividerRules->value() > 1);

	/*
	 * The built-in shapes are drawn white to be tinted, so the flag only means anything once
	 * some slot -- an end, an arm, or a piece of the center stack -- is pointed at a file.
	 */
	const bool dividerFiles = armFromFile || (divider && dividerUsesFile());
	setRowVisible(dividerTint, dividerFiles);

	/*
	 * The column count is offered for every list, because it is how a list *becomes* one of the
	 * multi-column types: one column or several is the difference between two of the document's
	 * types, and asking for a second column is a far plainer way to say that than picking a
	 * different type out of a list. The two settings that describe a grid follow the count.
	 */
	setRowVisible(columns, list);
	setRowVisible(columnGap, hasColumns);
	setRowVisible(fillOrder, hasColumns);
	setRowVisible(entryGap, hasEntries);
	/* A step with no row to step is a setting for nothing: the tabs live in the entry table. */
	setRowVisible(indentStep, hasEntries);
	/*
	 * The pair's own two settings apply wherever anything is really stacked, which for a bridged
	 * row is a choice rather than a property of the type -- so this asks about the section
	 * being edited and not only about its type. `secondaryGroup`'s title stays keyed on the
	 * type: for a bridged row the second style is still the right-hand *text*, subtitles or not.
	 */
	const bool hasSubtitles = sectionUsesSubtitles(type);
	const bool stacksSubtitles = hasSubtitles || (bridged && rowSubtitles->isChecked());
	setRowVisible(subtitleGap, stacksSubtitles);
	setRowVisible(subtitleOrder, stacksSubtitles);
	setRowVisible(spacerHeight, type == SectionType::Spacer);

	/*
	 * A sticky block spans the canvas and lets the sections inside it place themselves, so the
	 * three settings that would narrow it say nothing here -- see the layout, which ignores them
	 * for this type rather than leaving two nested shares of the width to argue about.
	 */
	const bool placeable = type != SectionType::StickyBlock;
	setRowVisible(marginX, placeable);
	/* A spacer draws nothing, so there is nothing of it to nudge sideways. */
	setRowVisible(contentOffsetX, placeable && type != SectionType::Spacer);
	setRowVisible(sectionWidth, placeable);
	setRowVisible(sectionAlign, placeable);

	/*
	 * The sticky block's rows. The hold is hidden outright while the block holds for ever, since
	 * a number of seconds that nothing counts down is a control that lies; the warning underneath
	 * appears only for the pairing that really has no end to it.
	 */
	const bool sticky = type == SectionType::StickyBlock;
	const auto release = static_cast<StickyRelease>(stickyRelease->currentData().toInt());

	setRowVisible(stickyAnchor, sticky);
	setRowVisible(stickyCanvasPosition, sticky);
	setRowVisible(stickyOffset, sticky);
	setRowVisible(stickyHold, sticky && !stickyHoldForever->isChecked());
	setRowVisible(stickyHoldForever, sticky);
	setRowVisible(stickyRelease, sticky);
	setRowVisible(stickyForeverWarning,
		      sticky && stickyHoldForever->isChecked() && stickyReleaseEndsAtHold(release));

	/*
	 * A divider has a style even though it carries no section text: the artwork is inked from
	 * it, and a word or a mark in the center stack is drawn with it.
	 */
	styleGroup->setVisible(hasText || hasLogos || divider);
	/*
	 * The same style serves whichever second text the type carries, so the group is titled
	 * after the one being edited rather than after the Bridged section it was written for.
	 */
	secondaryGroup->setVisible(sectionUsesSecondaryText(type));
	secondaryGroup->setTitle(hasSubtitles ? moduleText("Designer.SubtitleStyle")
					      : moduleText("Designer.SecondaryStyle"));
	/*
	 * Both shapes that draw a bridge have one to ink separately -- except a custom file left in
	 * the colors it was authored with, which is painted straight to the strip with nothing here
	 * getting a say over it.
	 */
	/*
	 * A divider's artwork is the same thing to ink apart from the text as a bridge is, and takes
	 * the same override -- so the group is on show for both, and titled after whichever is being
	 * edited. It goes away for artwork left in a file's own colors, which nothing here reaches.
	 */
	const bool inkableDivider = divider && !(dividerFiles && !dividerTint->isChecked());
	bridgeStyleGroup->setVisible((usesBridge && !(artFromFile && !bridgeTint->isChecked())) || inkableDivider);
	bridgeStyleGroup->setTitle(divider ? moduleText("Designer.DividerArtStyle")
					   : moduleText("Designer.BridgeStyle"));

	/*
	 * Which panels a type has anything to sit behind is a property of the type table, so it is
	 * asked of the model rather than rebuilt out of the predicates already gathered here -- one
	 * answer, in one place, that a new section type comes with.
	 */
	const QVector<BackgroundSlot> panelSlots = backgroundSlotsFor(type);
	for (auto it = backgroundGroups.cbegin(); it != backgroundGroups.cend(); ++it)
		it.value()->setVisible(panelSlots.contains(it.key()));

	entriesGroup->setVisible(hasEntries);
	/* Nothing for a file picker to fill in when the entries are lines of text. */
	setLogoButton->setVisible(hasEntries && hasLogos);

	dividerPiecesGroup->setVisible(divider);

	/*
	 * A group with every row hidden is a heading over nothing, so it goes away with them. Asked
	 * of the form rather than tracked alongside it: the rows have just been set, and counting
	 * what is visible cannot disagree with them the way a second list of conditions could.
	 */
	contentGroup->setVisible(formHasVisibleRow(contentForm));
	typeSettingsGroup->setVisible(formHasVisibleRow(typeSettingsForm));
	placementGroup->setVisible(formHasVisibleRow(placementForm));

	/*
	 * Whichever table the Content tab is showing is the one thing on it worth growing, so it takes
	 * that page's leftover height. With no table at all the trailing spacer takes it instead,
	 * which is what keeps the rows packed at the top rather than spread down the page.
	 */
	tabLayout(EditorTab::Content)
		->setStretch(tabStretchIndex[static_cast<int>(EditorTab::Content)], hasEntries || divider ? 0 : 1);

	/* Last, so it reads the groups this pass has just settled. */
	refreshTabVisibility();
}

void SectionEditor::rebuildEntryTable(SectionType type, bool rowSubtitles)
{
	const QSignalBlocker blocker(entryTable);
	/* What the columns now stand for, so the next relayout can read them back correctly. */
	tableType = type;
	tableRowSubtitles = rowSubtitles;

	entryTable->clear();
	entryTable->setRowCount(0);

	QStringList headers;

	switch (type) {
	case SectionType::Bridged:
		/*
		 * Each side of the row gains a column of its own when the section draws subtitles,
		 * with the pair kept side by side rather than the two subtitles gathered at the end:
		 * a row is read across, and a subtitle belongs beside the line it sits under.
		 */
		headers = rowSubtitles ? QStringList{moduleText("Designer.Column.Left"),
						     moduleText("Designer.Column.LeftSubtitle"),
						     moduleText("Designer.Column.Right"),
						     moduleText("Designer.Column.RightSubtitle")}
				       : QStringList{moduleText("Designer.Column.Left"),
						     moduleText("Designer.Column.Right")};
		break;

	case SectionType::TitleSubtitleList:
	case SectionType::MultiTitleSubtitleList:
		/*
		 * Headed by what the two texts are rather than by where they end up, so swapping the
		 * order does not relabel the columns the entries were typed into.
		 */
		headers = {moduleText("Designer.Column.EntryTitle"), moduleText("Designer.Column.Subtitle")};
		break;

	case SectionType::LogoList:
	case SectionType::MultiLogoList:
		headers = {moduleText("Designer.Column.Logo"), moduleText("Designer.Column.Height")};
		break;

	default:
		headers = {moduleText("Designer.Column.Text")};
		break;
	}

	/*
	 * The tab column is last in every list, because it is the one column that is not part of
	 * what the entry says: a row is read across for its words and only then, if at all, for how
	 * far in it sits.
	 */
	const int indentColumn = headers.size();
	headers.append(moduleText("Designer.Column.Indent"));

	entryTable->setColumnCount(headers.size());
	entryTable->setHorizontalHeaderLabels(headers);
	if (QTableWidgetItem *indentHeader = entryTable->horizontalHeaderItem(indentColumn))
		indentHeader->setToolTip(moduleText("Designer.Column.Indent.Tip"));

	/*
	 * One column takes the slack and the narrow ones keep what they need. It is the file path in
	 * a logo list -- the one thing in the row long enough to need reading, where the column
	 * beside it is a three-digit pixel height -- and the last of the text columns everywhere
	 * else. Never the tab column, which holds a number of steps and would be a wide box of white
	 * space with the words it belongs to squeezed up beside it.
	 */
	const bool logoEntries = sectionUsesLogos(type) && sectionUsesEntries(type);
	QHeaderView *header = entryTable->horizontalHeader();

	header->setStretchLastSection(false);
	header->setSectionResizeMode(QHeaderView::Interactive);
	header->setSectionResizeMode(logoEntries ? 0 : indentColumn - 1, QHeaderView::Stretch);

	if (logoEntries)
		entryTable->setColumnWidth(1, kEntryHeightColumnWidth);

	entryTable->setColumnWidth(indentColumn, kEntryIndentColumnWidth);
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

		/* Last in every layout the table has; see rebuildEntryTable. */
		entryTable->setItem(row, entryTable->columnCount() - 1,
				    new QTableWidgetItem(QString::number(entry.indent)));

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

		entry.indent = cell(entryTable->columnCount() - 1).toInt();

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
		for (const DividerPiece &piece : source.dividerCenter) {
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

	setRowVisible(logoLoop->parentWidget(), animated);
	setRowVisible(logoAnimatedShadow, animated);
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
			applyTypeVisibility(composedType());
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
		/*
		 * An angle belongs to the piece rather than to any one kind of piece -- a word set
		 * sideways is as ordinary as an arrowhead stood on end -- so this column is filled
		 * and left editable whatever the row holds.
		 */
		table->setItem(row, PieceRotation,
			       new QTableWidgetItem(QString::number(piece.rotation, 'g', 4) + kDegreeSign));

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
		piece.rotation = degreesFromCell(cell(PieceRotation));

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
	 * Grayed rather than blanked, so a cell that stops applying keeps what was typed into it
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

void SectionEditor::expandTable(QTableWidget *table, const QString &title)
{
	/*
	 * Where to put the table back: the layout it is in and the place it holds in it, taken
	 * before it is moved rather than assumed, so this one call serves the entry table and the
	 * divider's three piece tables alike.
	 */
	QWidget *home = table->parentWidget();
	auto *homeLayout = home ? qobject_cast<QBoxLayout *>(home->layout()) : nullptr;
	if (!homeLayout)
		return;

	const int homeIndex = homeLayout->indexOf(table);
	if (homeIndex < 0)
		return;

	QDialog window(this);
	window.setWindowTitle(title);

	/*
	 * Most of the screen, which is the whole point of the button: what it is for is the run of
	 * credits too long to read a pane at a time.
	 */
	if (const QScreen *display = screen()) {
		const QRect available = display->availableGeometry();
		window.resize(available.width() * 3 / 4, available.height() * 3 / 4);
	}

	auto *layout = new QVBoxLayout(&window);
	layout->addWidget(table);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &window);
	connect(buttons, &QDialogButtonBox::rejected, &window, &QDialog::reject);
	layout->addWidget(buttons);

	window.exec();

	/*
	 * Home before the window goes, and before anything else: a widget still parented to a dialog
	 * being destroyed is destroyed with it, and the editor holds a pointer to this one.
	 */
	homeLayout->insertWidget(homeIndex, table);
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
	const SectionType type = composedType();
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
	const SectionType type = composedType();

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
