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

#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <utility>

#include "render/AnimatedLogo.hpp"
#include "render/RollRenderer.hpp"
#include "ui/CsvImportDialog.hpp"
#include "ui/FontPickerDialog.hpp"
#include "ui/ActionListEditor.hpp"
#include "ui/DesignerWidgets.hpp"
#include "ui/DividerEditor.hpp"
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
		SectionType::Spacer, SectionType::SectionDivider, SectionType::StickyBlock, SectionType::Action,
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

/* A tick when the chip is in, a plus when it could be: the chip says what clicking it will do. */
void markChip(QToolButton *chip)
{
	chip->setText((chip->isChecked() ? QStringLiteral("✓ ") : QStringLiteral("+ ")) +
		      chip->property("chipText").toString());
}

/*
 * Keeps a field to the width its value needs.
 *
 * A form stretches every field to the pane's width, which turned a three-digit pixel count into a
 * box the width of the editor -- the eye travels from the label across empty field to find the
 * number. Numbers and short pickers keep a width that fits what they hold; text and paths, which
 * really are long, still take the room.
 */
void capFieldWidth(QWidget *field)
{
	constexpr int kNumberWidth = 150;
	constexpr int kPickerWidth = 320;
	constexpr int kSwatchWidth = 220;

	if (qobject_cast<QAbstractSpinBox *>(field))
		field->setMaximumWidth(kNumberWidth);
	else if (qobject_cast<QComboBox *>(field))
		field->setMaximumWidth(kPickerWidth);
	else if (qobject_cast<ColorButton *>(field))
		field->setMaximumWidth(kSwatchWidth);
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
	fontButton->setMaximumWidth(360);
	layout->addRow(moduleText("Designer.Font"), fontButton);

	/*
	 * Size and line spacing on one row: two numbers that are only ever tuned together, and that
	 * stacked one under the other read as two unrelated settings.
	 */
	sizeRow = new QWidget(this);
	auto *sizeLayout = new QHBoxLayout(sizeRow);
	sizeLayout->setContentsMargins(0, 0, 0, 0);
	pixelSize = new QSpinBox(sizeRow);
	pixelSize->setRange(1, 1024);
	pixelSize->setSuffix(QStringLiteral(" px"));
	capFieldWidth(pixelSize);
	lineSpacing = new QDoubleSpinBox(sizeRow);
	lineSpacing->setRange(0.5, 4.0);
	lineSpacing->setSingleStep(0.05);
	lineSpacing->setDecimals(2);
	lineSpacing->setToolTip(moduleText("Designer.LineSpacing"));
	capFieldWidth(lineSpacing);
	sizeLayout->addWidget(pixelSize);
	sizeLayout->addSpacing(10);
	sizeLayout->addWidget(new QLabel(moduleText("Designer.LineSpacing"), sizeRow));
	sizeLayout->addWidget(lineSpacing);
	sizeLayout->addStretch();
	layout->addRow(moduleText("Designer.FontSize"), sizeRow);

	alignment = new QComboBox(this);
	addAlignmentOptions(alignment);
	capFieldWidth(alignment);
	layout->addRow(moduleText("Designer.Alignment"), alignment);

	fillBox = new QComboBox(this);
	fillBox->addItem(moduleText("Designer.Fill.Solid"), static_cast<int>(TextFill::Solid));
	fillBox->addItem(moduleText("Designer.Fill.Linear"), static_cast<int>(TextFill::LinearGradient));
	fillBox->addItem(moduleText("Designer.Fill.Radial"), static_cast<int>(TextFill::RadialGradient));
	capFieldWidth(fillBox);
	layout->addRow(moduleText("Designer.Fill"), fillBox);

	colorButton = new ColorButton(this);
	colorButton->setDialogTitle(moduleText("Designer.FontColor"));
	capFieldWidth(colorButton);
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
	capFieldWidth(outlineWidth);
	outlineForm->addRow(moduleText("Designer.OutlineWidth"), outlineWidth);
	outlineColor = new ColorButton(outlineGroup);
	outlineColor->setDialogTitle(moduleText("Designer.OutlineColor"));
	capFieldWidth(outlineColor);
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
	capFieldWidth(shadowOffsetX);
	capFieldWidth(shadowOffsetY);
	offsetLayout->addWidget(shadowOffsetX);
	offsetLayout->addWidget(shadowOffsetY);
	offsetLayout->addStretch();
	shadowForm->addRow(moduleText("Designer.ShadowOffset"), offsetRow);

	shadowBlur = new QSpinBox(shadowGroup);
	shadowBlur->setRange(0, 200);
	shadowBlur->setSuffix(QStringLiteral(" px"));
	shadowBlur->setToolTip(moduleText("Designer.ShadowBlur.Tip"));
	capFieldWidth(shadowBlur);
	shadowForm->addRow(moduleText("Designer.ShadowBlur"), shadowBlur);

	shadowColor = new ColorButton(shadowGroup);
	shadowColor->setDialogTitle(moduleText("Designer.ShadowColor"));
	capFieldWidth(shadowColor);
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
	connect(outlineGroup, &QGroupBox::toggled, this, [this] {
		applyEffectVisibility();
		notifyEdited();
	});
	connect(outlineWidth, &QDoubleSpinBox::valueChanged, this, notify);
	connect(outlineColor, &ColorButton::colorChanged, this, notify);
	connect(shadowGroup, &QGroupBox::toggled, this, [this] {
		applyEffectVisibility();
		notifyEdited();
	});
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
	applyEffectVisibility();
}

void StyleEditor::applyEffectVisibility()
{
	/*
	 * Only the switch shows while an effect is off. Its settings greyed out underneath taught
	 * nothing and doubled the length of every style, and they keep their values while hidden, so
	 * switching the effect back on brings back exactly what was set.
	 */
	for (QGroupBox *group : {outlineGroup, shadowGroup}) {
		const bool on = group->isChecked();
		for (QWidget *child : group->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
			child->setVisible(on);
		/* Flat while off, so an empty frame is not left drawn under the switch. */
		group->setFlat(!on);
	}
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
	applyEffectVisibility();
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
	form->setRowVisible(sizeRow, !inkOnly);
	form->setRowVisible(alignment, !inkOnly);

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
	 * The header above the tab strip, which every tab is read under: a badge for the kind of
	 * section, its name, whether it is in the roll, and the chips that shape it. See `typeBadge`.
	 */
	auto *header = new QWidget(this);
	auto *headerLayout = new QVBoxLayout(header);
	headerLayout->setContentsMargins(2, 2, 2, 8);
	headerLayout->setSpacing(8);
	outer->addWidget(header);

	const auto notify = [this] {
		emitChanged();
	};

	/*
	 * Seven base types and a few switches rather than twenty entries in one list. The twenty are
	 * still what the document holds -- see composedType -- but "a title, with a subtitle, with a
	 * logo" is three plain answers where picking "Title w/ Subtitle & Logo" out of a list of ten
	 * headings is one hard question. The combo is never shown: the header's menu sets it.
	 */
	typeBox = new QComboBox(this);
	for (SectionType type : baseSectionTypes())
		typeBox->addItem(baseTypeLabel(type), static_cast<int>(type));
	typeBox->hide();

	auto *identity = new QHBoxLayout();
	identity->setSpacing(10);
	headerLayout->addLayout(identity);

	typeBadge = new QLabel(header);
	typeBadge->setFixedSize(40, 40);
	identity->addWidget(typeBadge, 0, Qt::AlignTop);

	auto *naming = new QVBoxLayout();
	naming->setSpacing(1);
	identity->addLayout(naming, 1);

	/*
	 * The name is the header's title and is edited where it is read. Its placeholder is the name
	 * the section gives itself, so an unnamed section still shows what the list calls it.
	 */
	labelEdit = new QLineEdit(header);
	labelEdit->setObjectName(QStringLiteral("designerSectionName"));
	labelEdit->setToolTip(moduleText("Designer.Label.Tip"));
	labelEdit->setAccessibleName(moduleText("Designer.Label"));
	naming->addWidget(labelEdit);

	auto *typeLine = new QHBoxLayout();
	typeLine->setContentsMargins(5, 0, 0, 0);
	typeLine->setSpacing(6);
	naming->addLayout(typeLine);

	typeNameLabel = new QLabel(header);
	styleAsHint(typeNameLabel);
	typeLine->addWidget(typeNameLabel);

	/* What the type is for lives behind this, rather than in a paragraph pushing the tabs down. */
	typeInfo = new QLabel(header);
	typeInfo->setCursor(Qt::WhatsThisCursor);
	typeLine->addWidget(typeInfo);

	auto *dot = new QLabel(QStringLiteral("·"), header);
	styleAsHint(dot);
	typeLine->addWidget(dot);

	changeTypeButton = new QToolButton(header);
	changeTypeButton->setObjectName(QStringLiteral("designerLink"));
	changeTypeButton->setText(moduleText("Designer.ChangeType"));
	changeTypeButton->setPopupMode(QToolButton::InstantPopup);
	changeTypeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	changeTypeButton->setCursor(Qt::PointingHandCursor);
	changeTypeMenu = new QMenu(changeTypeButton);
	auto *typeActions = new QActionGroup(changeTypeMenu);
	for (SectionType type : baseSectionTypes()) {
		QAction *action = changeTypeMenu->addAction(baseTypeLabel(type));
		action->setCheckable(true);
		action->setData(static_cast<int>(type));
		typeActions->addAction(action);
		connect(action, &QAction::triggered, this,
			[this, type] { selectByData(typeBox, static_cast<int>(type)); });
	}
	changeTypeButton->setMenu(changeTypeMenu);
	typeLine->addWidget(changeTypeButton);
	typeLine->addStretch();

	auto *rollToggle = new QHBoxLayout();
	rollToggle->setSpacing(8);
	auto *inRoll = new QLabel(moduleText("Designer.InRoll"), header);
	styleAsHint(inRoll);
	visibleBox = new ToggleSwitch(header);
	visibleBox->setToolTip(moduleText("Designer.Visible"));
	visibleBox->setAccessibleName(moduleText("Designer.Visible"));
	inRoll->setBuddy(visibleBox);
	rollToggle->addWidget(inRoll);
	rollToggle->addWidget(visibleBox);
	identity->addLayout(rollToggle);
	identity->setAlignment(rollToggle, Qt::AlignTop);

	/*
	 * The chips that shape the section: what a heading includes, or what each entry of a list is.
	 * Only the ones that apply to the type are shown; with none, the row goes away.
	 */
	variantRow = new QWidget(header);
	auto *variants = new QHBoxLayout(variantRow);
	variants->setContentsMargins(50, 0, 0, 0);
	variants->setSpacing(6);
	variantLabel = new QLabel(variantRow);
	styleAsHint(variantLabel);
	variants->addWidget(variantLabel);

	typeSubtitle =
		makeChip(moduleText("Designer.Chip.Subtitle"), moduleText("Designer.TypeSubtitle.Tip"), variantRow);
	typeLogo = makeChip(moduleText("Designer.Chip.Logo"), moduleText("Designer.TypeLogo.Tip"), variantRow);
	typeLogoOnly =
		makeChip(moduleText("Designer.Chip.LogoOnly"), moduleText("Designer.TypeLogoOnly.Tip"), variantRow);
	for (QToolButton *chip : {typeSubtitle, typeLogo, typeLogoOnly}) {
		variants->addWidget(chip);
		chip->setProperty("chipText", chip->text());
		markChip(chip);
		connect(chip, &QToolButton::toggled, chip, [chip] { markChip(chip); });
	}

	typeListContent = new QComboBox(this);
	typeListContent->addItem(moduleText("Designer.ListContent.Text"), static_cast<int>(SectionListContent::Text));
	typeListContent->addItem(moduleText("Designer.ListContent.Pairs"), static_cast<int>(SectionListContent::Pairs));
	typeListContent->addItem(moduleText("Designer.ListContent.Logos"), static_cast<int>(SectionListContent::Logos));
	typeListContent->hide();
	listContentSegments = new SegmentedControl(variantRow);
	listContentSegments->setToolTip(moduleText("Designer.ListContent.Tip"));
	listContentSegments->bindTo(typeListContent);
	variants->addWidget(listContentSegments);
	variants->addStretch();
	headerLayout->addWidget(variantRow);

	/*
	 * From here everything goes into one of the four tabs, in named groups that fold away. See
	 * EditorTab for why the settings are dealt out rather than stacked.
	 *
	 * All four are built up front and in reading order, so a group only has to say which tab it
	 * belongs to. Which of them are on show follows what is left visible on each page; see
	 * refreshTabVisibility.
	 */
	tabs = new QTabWidget(this);
	tabs->setObjectName(QStringLiteral("designerEditorTabs"));
	tabs->tabBar()->setObjectName(QStringLiteral("designerEditorTabBar"));
	tabs->setDocumentMode(true);
	tabs->setIconSize(QSize(12, 12));
	outer->addWidget(tabs, 1);
	changedIcon = changedDotIcon();

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
	contentGroup->setStateKey(QStringLiteral("content"));
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

	addLogoSlot = new QPushButton(moduleText("Designer.AddLogoSlot"), this);
	addLogoSlot->setObjectName(QStringLiteral("designerEmptySlot"));
	addLogoSlot->setToolTip(moduleText("Designer.TypeLogo.Tip"));
	addLogoSlot->setCursor(Qt::PointingHandCursor);
	addRow(QString(), addLogoSlot);
	connect(addLogoSlot, &QPushButton::clicked, this, [this] { typeLogo->setChecked(true); });

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
	typeSettingsGroup->setStateKey(QStringLiteral("type-settings"));
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
	stickyEntrance = new QComboBox(this);
	stickyEntrance->addItem(moduleText("Designer.StickyEntrance.WithRoll"),
				static_cast<int>(StickyEntrance::WithRoll));
	stickyEntrance->addItem(moduleText("Designer.StickyEntrance.AfterRoll"),
				static_cast<int>(StickyEntrance::AfterRoll));
	stickyEntrance->setToolTip(moduleText("Designer.StickyEntrance.Tip"));
	addRow(moduleText("Designer.StickyEntrance"), stickyEntrance);

	stickyFadeIn = new QDoubleSpinBox(this);
	stickyFadeIn->setRange(0.0, 60.0);
	stickyFadeIn->setDecimals(2);
	stickyFadeIn->setSingleStep(0.25);
	stickyFadeIn->setSuffix(moduleText("Designer.Seconds"));
	stickyFadeIn->setToolTip(moduleText("Designer.StickyFadeIn.Tip"));
	addRow(moduleText("Designer.StickyFadeIn"), stickyFadeIn);

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
	stickyRelease->addItem(moduleText("Designer.StickyRelease.ResumeOnly"),
			       static_cast<int>(StickyRelease::ResumeOnly));
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

	/*
	 * Where down the frame an Action section fires: a share of the canvas height, like a sticky
	 * block's pin, so it means the same place after a resize. 100 % is the bottom edge, which is the
	 * moment the marker comes into frame.
	 */
	actionLine = new QSpinBox(this);
	actionLine->setRange(0, 100);
	actionLine->setSuffix(QStringLiteral(" %"));
	actionLine->setToolTip(moduleText("Designer.ActionLine.Tip"));
	addRow(moduleText("Designer.ActionLine"), actionLine);

	/*
	 * Where the section sits on the canvas, which is the same set of questions for every type. The
	 * four edges are set on a drawing of the box they move; width and placement follow as rows.
	 */
	placementGroup = new CollapsibleGroup(moduleText("Designer.Group.Placement"), this);
	placementGroup->setStateKey(QStringLiteral("placement"));
	spacing = new SpacingDiagram(placementGroup->content());
	placementGroup->addWidget(spacing);
	paddingTop = spacing->above();
	paddingBottom = spacing->below();
	marginX = spacing->sides();
	contentOffsetX = spacing->offset();

	form = new QFormLayout();
	placementForm = form;
	placementGroup->addLayout(form);
	tabLayout(EditorTab::Layout)->addWidget(placementGroup);

	sectionWidth = new QSpinBox(this);
	sectionWidth->setRange(1, 100);
	sectionWidth->setSuffix(QStringLiteral(" %"));
	sectionWidth->setToolTip(moduleText("Designer.SectionWidth.Tip"));
	addRow(moduleText("Designer.SectionWidth"), sectionWidth);

	/* Three choices, so three buttons rather than a dropdown; the combo behind them is the value. */
	sectionAlign = new QComboBox(this);
	addAlignmentOptions(sectionAlign);
	sectionAlign->hide();
	alignSegments = new SegmentedControl(this);
	alignSegments->setToolTip(moduleText("Designer.SectionAlign.Tip"));
	alignSegments->bindTo(sectionAlign);
	addRow(moduleText("Designer.SectionAlign"), alignSegments);
	rowAlias.insert(sectionAlign, alignSegments);

	connect(sectionWidth, &QSpinBox::valueChanged, this,
		[this](int value) { spacing->setContentText(moduleText("Designer.Spacing.Content").arg(value)); });

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
	styleGroup->setStateKey(QStringLiteral("style"));
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
	rowSubtitleStyleGroup->setStateKey(QStringLiteral("style.row-subtitle"));
	rowSubtitleStyle = new StyleEditor(rowSubtitleStyleGroup->content());
	rowSubtitleStyleGroup->addWidget(rowSubtitleStyle);
	tabLayout(EditorTab::Style)->addWidget(rowSubtitleStyleGroup);

	rowSecondarySubtitleStyleGroup = new CollapsibleGroup(moduleText("Designer.RowSecondarySubtitleStyle"), this);
	rowSecondarySubtitleStyleGroup->setStateKey(QStringLiteral("style.row-secondary-subtitle"));
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
	entryTable->installEventFilter(this);
	entryTable->viewport()->installEventFilter(this);

	/* What an empty table is for, said inside it rather than left as a blank grid. */
	entryEmptyHint = new QLabel(moduleText("Designer.Entries.Empty"), entryTable->viewport());
	entryEmptyHint->setAlignment(Qt::AlignCenter);
	entryEmptyHint->setWordWrap(true);
	styleAsHint(entryEmptyHint);
	entryEmptyHint->setAttribute(Qt::WA_TransparentForMouseEvents);
	/*
	 * Tied to the hint's own lifetime rather than the editor's: the table resets its model while
	 * it is being torn down, after the hint -- a child of its viewport -- has already gone.
	 */
	const auto refreshHint = [this] {
		refreshEntryEmptyHint();
	};
	connect(entryTable->model(), &QAbstractItemModel::rowsInserted, entryEmptyHint, refreshHint);
	connect(entryTable->model(), &QAbstractItemModel::rowsRemoved, entryEmptyHint, refreshHint);
	connect(entryTable->model(), &QAbstractItemModel::modelReset, entryEmptyHint, refreshHint);

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
	 * The divider's whole editor: a design to start from, the preview, the palette, a track per
	 * lane and the settings of whatever is selected. A divider has no words of its own to type, so
	 * this is the whole of its Content tab -- the thing somebody opens a divider to work on.
	 */
	dividerEditor = new DividerEditor(this);
	dividerEditor->setPreviewSource([this] { return section(); });
	tabLayout(EditorTab::Content)->addWidget(dividerEditor, 1);
	connect(dividerEditor, &DividerEditor::changed, this, [this] {
		if (loading)
			return;
		applyTypeVisibility(composedType());
		refreshLogoPlayback();
		emitChanged();
	});

	/*
	 * What an Action section runs. It has nothing else to show, so this is its Content tab, the way
	 * the piece stacks are a divider's.
	 */
	actionsGroup = new QGroupBox(moduleText("Designer.Actions"), this);
	actionsGroup->setToolTip(moduleText("Designer.Actions.Tip"));
	auto *actionsLayout = new QVBoxLayout(actionsGroup);
	actionList = new ActionListEditor(actionsGroup);
	actionsLayout->addWidget(actionList);
	tabLayout(EditorTab::Content)->addWidget(actionsGroup);
	connect(actionList, &ActionListEditor::changed, this, [this] { emitChanged(); });

	/*
	 * Style at a glance: one line per style the section draws with, under its words. Somebody
	 * typing a heading can see that it follows the "Heading Large" preset without leaving the
	 * words to go and look, and a click on the line takes them to it.
	 */
	styleSummary = new QWidget(this);
	styleSummaryLayout = new QVBoxLayout(styleSummary);
	styleSummaryLayout->setContentsMargins(4, 10, 4, 4);
	styleSummaryLayout->setSpacing(4);
	tabLayout(EditorTab::Content)->addWidget(styleSummary);

	changedTimer = new QTimer(this);
	changedTimer->setSingleShot(true);
	changedTimer->setInterval(300);
	connect(changedTimer, &QTimer::timeout, this, [this] {
		refreshChangedTabs();
		refreshStyleSummary();
	});

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
	for (QToolButton *chip : {typeSubtitle, typeLogo, typeLogoOnly})
		connect(chip, &QToolButton::toggled, this, &SectionEditor::onTypeSwitchChanged);

	/*
	 * The column count is part of what a list *is* -- one column or several is the difference
	 * between two of the document's types -- so it runs the same path the switches do.
	 */
	connect(columns, &QSpinBox::valueChanged, this, &SectionEditor::onTypeSwitchChanged);

	connect(labelEdit, &QLineEdit::textChanged, this, notify);
	connect(visibleBox, &ToggleSwitch::toggled, this, notify);
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
	connect(stickyFadeIn, &QDoubleSpinBox::valueChanged, this, notify);

	/*
	 * These three decide what else on the form applies -- an entrance that waits has a fade to
	 * time, and a release that ends the roll is what makes holding for ever worth warning about --
	 * so each re-runs the visibility pass rather than only reporting a change.
	 */
	const auto revisitSticky = [this] {
		if (loading)
			return;
		applyTypeVisibility(composedType());
		emitChanged();
	};

	connect(stickyEntrance, &QComboBox::currentIndexChanged, this, revisitSticky);
	connect(stickyRelease, &QComboBox::currentIndexChanged, this, revisitSticky);
	connect(stickyHoldForever, &QCheckBox::toggled, this, revisitSticky);
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

	connect(columnGap, &QSpinBox::valueChanged, this, notify);
	connect(fillOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(entryGap, &QSpinBox::valueChanged, this, notify);
	connect(indentStep, &QSpinBox::valueChanged, this, notify);
	connect(contentOffsetX, &QSpinBox::valueChanged, this, notify);
	connect(subtitleGap, &QSpinBox::valueChanged, this, notify);
	connect(subtitleOrder, &QComboBox::currentIndexChanged, this, notify);
	connect(spacerHeight, &QSpinBox::valueChanged, this, notify);
	connect(actionLine, &QSpinBox::valueChanged, this, notify);
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
	{
		/* The name the section gives itself, shown for as long as it has not been given one. */
		Section unnamed = source;
		unnamed.label.clear();
		labelEdit->setPlaceholderText(unnamed.displayLabel());
	}
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
	/* Kept whatever the type, so a section switched away from a divider and back keeps its figure. */
	dividerEditor->setDivider(source.divider);
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
	actionLine->setValue(qRound(source.actionLine * 100.0));
	actionList->setActions(source.actions);
	selectByData(stickyEntrance, static_cast<int>(source.stickyEntrance));
	stickyFadeIn->setValue(source.stickyFadeIn);
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

	applyTypeVisibility(source.type);
	rebuildEntryTable(source.type, source.rowSubtitles);
	writeEntriesToTable(source);
	/* After the tables, since what it asks about includes the artwork they hold. */
	refreshLogoPlayback();
	refreshEntryEmptyHint();

	loading = false;

	changedTimer->stop();
	refreshChangedTabs();
	refreshStyleSummary();
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
	result.divider = dividerEditor->divider();
	result.columns = columns->value();
	result.columnGap = columnGap->value();
	result.fillAcross = fillOrder->currentData().toInt() == 1;
	result.entryGap = entryGap->value();
	result.indentStep = indentStep->value();
	result.subtitleGap = subtitleGap->value();
	result.subtitleFirst = subtitleOrder->currentData().toInt() == 1;
	result.spacerHeight = spacerHeight->value();
	result.actionLine = actionLine->value() / 100.0;
	result.actions = actionList->actions();
	result.stickyEntrance = static_cast<StickyEntrance>(stickyEntrance->currentData().toInt());
	result.stickyFadeIn = stickyFadeIn->value();
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

	/* After both tables, which rebuild their logos from cells that carry no playback of their own. */
	for (Entry &entry : result.entries)
		entry.logo.playback = playback;
	for (DividerLane &lane : result.divider.lanes) {
		for (DividerElement &element : lane.elements) {
			if (element.kind == DividerElement::Kind::Image)
				element.logo.playback = playback;
		}
	}

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
	capFieldWidth(field);
	form->addRow(label, field);
	rowOwner.insert(field, form);
}

void SectionEditor::setRowVisible(QWidget *field, bool visible)
{
	if (QWidget *shownAs = rowAlias.value(field))
		field = shownAs;

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
	tabPages[index] = scroll;

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

	typeSubtitle->setVisible(heading && !typeLogoOnly->isChecked());
	typeLogo->setVisible(heading && !typeLogoOnly->isChecked());
	typeLogoOnly->setVisible(heading);
	listContentSegments->setVisible(list);
	for (QToolButton *chip : {typeSubtitle, typeLogo, typeLogoOnly})
		markChip(chip);
	variantLabel->setText(moduleText(list ? "Designer.EachEntryIs" : "Designer.Includes"));
	variantRow->setVisible(heading || list);

	/* The header's picture of the section, its kind in words, and what that kind is for. */
	typeBadge->setPixmap(typeBadgePixmap(type, typeBadge->width(), devicePixelRatioF()));
	typeNameLabel->setText(baseTypeLabel(base));
	const QString help = sectionTypeHelp(type);
	typeInfo->setPixmap(infoPixmap(13, typeNameLabel->palette().color(QPalette::WindowText), devicePixelRatioF()));
	typeInfo->setToolTip(help);
	typeNameLabel->setToolTip(help);
	typeInfo->setVisible(!help.isEmpty());
	for (QAction *action : changeTypeMenu->actions())
		action->setChecked(action->data().toInt() == static_cast<int>(base));

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
	setRowVisible(addLogoSlot, heading && !typeLogo->isChecked() && !typeLogoOnly->isChecked());
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
	const bool divider = type == SectionType::SectionDivider;
	/*
	 * A user's own SVG anywhere in the divider, which is what decides whether drawing files in the
	 * divider's ink means anything -- and so whether that ink has anything left to reach.
	 */
	bool dividerFiles = false;
	if (divider) {
		for (const DividerLane &lane : dividerEditor->divider().lanes) {
			for (const DividerElement &element : lane.elements) {
				dividerFiles = dividerFiles ||
					       (element.kind == DividerElement::Kind::Shape &&
						dividerShapeUsesFile(element.shape)) ||
					       (element.isLine() && dividerShapeUsesFile(element.lineShape));
			}
		}
	}

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
	/*
	 * An Action section is a marker with no size at all, so nothing about a box -- its width, its
	 * margins, its padding -- has anything to act on.
	 */
	const bool marker = type == SectionType::Action;
	setRowVisible(actionLine, marker);
	actionsGroup->setVisible(marker);

	const bool placeable = type != SectionType::StickyBlock && !marker;
	/* A spacer draws nothing, so there is nothing of it to nudge sideways. */
	spacing->setEdgesVisible(!marker, placeable, placeable && type != SectionType::Spacer);
	spacing->setVisible(!marker);
	spacing->setContentText(placeable ? moduleText("Designer.Spacing.Content").arg(sectionWidth->value())
					  : moduleText("Designer.Spacing.Block"));
	setRowVisible(sectionWidth, placeable);
	setRowVisible(sectionAlign, placeable);

	/*
	 * The sticky block's rows. The hold is hidden outright while the block holds for ever, since
	 * a number of seconds that nothing counts down is a control that lies; the warning underneath
	 * appears only for the pairing that really has no end to it.
	 */
	const bool sticky = type == SectionType::StickyBlock;
	const auto release = static_cast<StickyRelease>(stickyRelease->currentData().toInt());
	const auto entrance = static_cast<StickyEntrance>(stickyEntrance->currentData().toInt());

	setRowVisible(stickyEntrance, sticky);
	/* A block that arrives with the roll is already on screen by the time it pins: nothing to fade. */
	setRowVisible(stickyFadeIn, sticky && stickyEntranceWaitsForRoll(entrance));
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
	const bool inkableDivider = divider && !(dividerFiles && !dividerEditor->divider().tint);
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

	dividerEditor->setVisible(divider);

	/*
	 * A group with every row hidden is a heading over nothing, so it goes away with them. Asked
	 * of the form rather than tracked alongside it: the rows have just been set, and counting
	 * what is visible cannot disagree with them the way a second list of conditions could.
	 */
	contentGroup->setVisible(formHasVisibleRow(contentForm));
	typeSettingsGroup->setVisible(formHasVisibleRow(typeSettingsForm));
	placementGroup->setVisible(formHasVisibleRow(placementForm) || !marker);

	/*
	 * Whichever table the Content tab is showing is the one thing on it worth growing, so it takes
	 * that page's leftover height. With no table at all the trailing spacer takes it instead,
	 * which is what keeps the rows packed at the top rather than spread down the page.
	 */
	tabLayout(EditorTab::Content)
		->setStretch(tabStretchIndex[static_cast<int>(EditorTab::Content)], hasEntries || divider ? 0 : 1);

	/* Last, so it reads the groups this pass has just settled. */
	refreshTabVisibility();
	refreshStyleSummary();
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

	/*
	 * Every stack a divider draws, not only its middle: an end is the same list of pieces and can
	 * hold a moving mark just as well. The right-hand list counts only while it is the one drawn.
	 */
	if (source.type == SectionType::SectionDivider) {
		for (const QString &path : source.divider.imagePaths()) {
			if (logoPathLooksAnimated(path))
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

void SectionEditor::showTab(EditorTab tab)
{
	const int index = static_cast<int>(tab);
	if (tabs->isTabVisible(index))
		tabs->setCurrentIndex(index);
}

void SectionEditor::focusName()
{
	labelEdit->setFocus(Qt::ShortcutFocusReason);
	labelEdit->selectAll();
}

namespace {

/* True when anything the Layout tab sets differs between the two sections. */
bool layoutDiffers(const Section &a, const Section &b)
{
	/* Read through the spin boxes, which hold whole numbers, so a default of 2.5 is not "changed". */
	const auto px = [](double value) {
		return qRound(value);
	};
	const auto pct = [](double value) {
		return qRound(value * 100.0);
	};

	return a.paddingTop != b.paddingTop || a.paddingBottom != b.paddingBottom || a.marginX != b.marginX ||
	       a.contentOffsetX != b.contentOffsetX || pct(a.sectionWidth) != pct(b.sectionWidth) ||
	       a.sectionAlign != b.sectionAlign || a.logoPlacement != b.logoPlacement || a.logoSide != b.logoSide ||
	       a.logoGap != b.logoGap || a.bridgeType != b.bridgeType || a.bridge != b.bridge ||
	       a.bridgeSvg != b.bridgeSvg || px(a.bridgeThickness) != px(b.bridgeThickness) ||
	       px(a.bridgeOffset) != px(b.bridgeOffset) || px(a.bridgeGap) != px(b.bridgeGap) ||
	       px(a.bridgeMinGap) != px(b.bridgeMinGap) || a.bridgeTint != b.bridgeTint ||
	       a.bridgeFill != b.bridgeFill || a.bridgeSizing != b.bridgeSizing ||
	       pct(a.bridgeSplit) != pct(b.bridgeSplit) || a.bridgeRowAlign != b.bridgeRowAlign ||
	       a.bridgeSpanEmpty != b.bridgeSpanEmpty || a.rowSubtitles != b.rowSubtitles ||
	       a.columnGap != b.columnGap || a.fillAcross != b.fillAcross || a.entryGap != b.entryGap ||
	       a.indentStep != b.indentStep || a.subtitleGap != b.subtitleGap || a.subtitleFirst != b.subtitleFirst ||
	       a.spacerHeight != b.spacerHeight || pct(a.actionLine) != pct(b.actionLine) ||
	       a.stickyEntrance != b.stickyEntrance || a.stickyFadeIn != b.stickyFadeIn ||
	       a.stickyAnchor != b.stickyAnchor || pct(a.stickyCanvasPosition) != pct(b.stickyCanvasPosition) ||
	       px(a.stickyOffset) != px(b.stickyOffset) || a.stickyHold != b.stickyHold ||
	       a.stickyHoldForever != b.stickyHoldForever || a.stickyRelease != b.stickyRelease;
}

bool styleDiffers(const Section &a, const Section &b)
{
	return !(a.style == b.style) || !(a.secondaryStyle == b.secondaryStyle) ||
	       a.useSecondaryStyle != b.useSecondaryStyle || !(a.bridgeStyle == b.bridgeStyle) ||
	       a.useBridgeStyle != b.useBridgeStyle || !(a.rowSubtitleStyle == b.rowSubtitleStyle) ||
	       !(a.rowSecondarySubtitleStyle == b.rowSecondarySubtitleStyle) || !a.stylePresetName.isEmpty() ||
	       !a.secondaryStylePresetName.isEmpty() || !a.bridgeStylePresetName.isEmpty() ||
	       !a.rowSubtitleStylePresetName.isEmpty() || !a.rowSecondarySubtitleStylePresetName.isEmpty();
}

bool backgroundDiffers(const Section &a, const Section &b)
{
	for (const BackgroundSlot slot : allBackgroundSlots()) {
		if (a.hasBackground(slot) != b.hasBackground(slot))
			return true;
		if (a.hasBackground(slot) && !(a.background(slot) == b.background(slot)))
			return true;
	}
	return false;
}

} // namespace

void SectionEditor::refreshChangedTabs()
{
	const Section edited = section();
	const Section defaults = Section::makeDefault(edited.type);

	const auto mark = [this](EditorTab tab, bool changed) {
		const int index = static_cast<int>(tab);
		tabs->setTabIcon(index, changed ? changedIcon : QIcon());
		tabs->setTabToolTip(index, changed ? moduleText("Designer.Tab.Changed") : QString());
	};

	mark(EditorTab::Layout, layoutDiffers(edited, defaults));
	mark(EditorTab::Style, styleDiffers(edited, defaults));
	mark(EditorTab::Background, backgroundDiffers(edited, defaults));
}

void SectionEditor::refreshStyleSummary()
{
	while (QLayoutItem *item = styleSummaryLayout->takeAt(0)) {
		delete item->widget();
		delete item;
	}

	struct Shown {
		CollapsibleGroup *group;
		StyleEditor *editor;
	};
	const QVector<Shown> styles = {{styleGroup, primaryStyle},
				       {secondaryGroup, secondaryStyle},
				       {bridgeStyleGroup, bridgeStyle},
				       {rowSubtitleStyleGroup, rowSubtitleStyle},
				       {rowSecondarySubtitleStyleGroup, rowSecondarySubtitleStyle}};

	int shown = 0;
	for (const Shown &entry : styles) {
		/* Only the styles this section really draws with: on show, and switched on where they switch. */
		if (entry.group->isHidden() || !entry.group->isChecked())
			continue;

		const TextStyle style = entry.editor->style();
		QString what;
		if (!entry.editor->presetName().isEmpty()) {
			what = moduleText("Designer.StyleSummary.Preset").arg(entry.editor->presetName());
		} else if (entry.group == bridgeStyleGroup) {
			what = style.color.name();
		} else {
			what = QStringLiteral("%1 · %2 px · %3")
				       .arg(style.family.isEmpty() ? moduleText("Designer.StyleSummary.DefaultFont")
								   : style.family)
				       .arg(style.pixelSize)
				       .arg(style.color.name());
		}

		auto *line = new QPushButton(styleSummary);
		line->setObjectName(QStringLiteral("designerStyleLink"));
		line->setText(QStringLiteral("%1:  %2").arg(entry.group->title(), what));
		line->setToolTip(moduleText("Designer.StyleSummary.Tip"));
		line->setCursor(Qt::PointingHandCursor);
		line->setMaximumWidth(480);
		CollapsibleGroup *group = entry.group;
		connect(line, &QPushButton::clicked, this, [this, group] {
			showTab(EditorTab::Style);
			group->setExpanded(true);
			if (auto *scroll = qobject_cast<QScrollArea *>(tabPages[static_cast<int>(EditorTab::Style)]))
				QTimer::singleShot(0, scroll,
						   [scroll, group] { scroll->ensureWidgetVisible(group, 0, 0); });
		});

		if (shown == 0) {
			auto *title = new QLabel(moduleText("Designer.StyleSummary"), styleSummary);
			styleAsHint(title);
			styleSummaryLayout->addWidget(title);
		}
		styleSummaryLayout->addWidget(line);
		++shown;
	}

	styleSummary->setVisible(shown > 0);
}

void SectionEditor::refreshEntryEmptyHint()
{
	const bool empty = entryTable->rowCount() == 0;
	entryEmptyHint->setVisible(empty);
	if (empty)
		entryEmptyHint->setGeometry(entryTable->viewport()->rect().adjusted(16, 0, -16, 0));
}

bool SectionEditor::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::Resize && watched == entryTable->viewport())
		refreshEntryEmptyHint();

	if (watched == entryTable && event->type() == QEvent::KeyPress) {
		auto *key = static_cast<QKeyEvent *>(event);
		if (key->matches(QKeySequence::Paste)) {
			pasteIntoEntries();
			return true;
		}
	}

	return QWidget::eventFilter(watched, event);
}

void SectionEditor::pasteIntoEntries()
{
	QString text = QGuiApplication::clipboard()->text();
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QLatin1Char('\r'), QLatin1Char('\n'));

	QStringList lines = text.split(QLatin1Char('\n'));
	/* A copied block of cells ends with a newline, which is not one more empty row. */
	while (!lines.isEmpty() && lines.last().isEmpty())
		lines.removeLast();
	if (lines.isEmpty())
		return;

	const int startRow = entryTable->currentRow() >= 0 ? entryTable->currentRow() : entryTable->rowCount();
	const int startColumn = std::max(0, entryTable->currentColumn());

	{
		const QSignalBlocker blocker(entryTable);

		for (int line = 0; line < lines.size(); ++line) {
			const int row = startRow + line;
			if (row >= entryTable->rowCount())
				entryTable->insertRow(entryTable->rowCount());

			const QStringList cells = lines.at(line).split(QLatin1Char('\t'));
			for (int cell = 0; cell < cells.size(); ++cell) {
				const int column = startColumn + cell;
				if (column >= entryTable->columnCount())
					break;

				/* Written into an existing item, so a row keeps the entry stashed on its first cell. */
				if (QTableWidgetItem *item = entryTable->item(row, column))
					item->setText(cells.at(cell).trimmed());
				else
					entryTable->setItem(row, column,
							    new QTableWidgetItem(cells.at(cell).trimmed()));
			}
		}
	}

	entryTable->setCurrentCell(startRow + static_cast<int>(lines.size()) - 1, startColumn);
	refreshEntryEmptyHint();
	refreshLogoPlayback();
	emitChanged();
}

void SectionEditor::emitChanged()
{
	if (loading)
		return;

	changedTimer->start();
	/* The preview draws in the section's own style and width, so any edit can change it. */
	if (dividerEditor && composedType() == SectionType::SectionDivider)
		dividerEditor->refreshPreview();
	emit changed();
}

} // namespace closingtime
