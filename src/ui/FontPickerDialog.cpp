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

#include "ui/FontPickerDialog.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QStringList>
#include <QVBoxLayout>

#include "render/FontResolution.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Point size the sample is drawn at. Fixed, because the size of the roll is not decided here. */
constexpr int kSamplePointSize = 20;

/*
 * The faces the family really ships, plus the ones Qt will fake for it.
 *
 * The real ones come first and in the order the font database reports them, which is the family's
 * own order -- weights ascending, upright before italic -- and is what somebody looking for
 * Semibold expects to scan. The synthesised ones are appended and marked, because "Bold" that is
 * a designed face and "Bold" that is the regular face thickened by the rasteriser are not the
 * same thing and the difference shows at large sizes.
 */
QVector<QString> synthesisedLabels()
{
	return {moduleText("FontPicker.Face.Regular"), moduleText("FontPicker.Face.Bold"),
		moduleText("FontPicker.Face.Italic"), moduleText("FontPicker.Face.BoldItalic")};
}

/* What to call a face that has no name of its own, which is what a synthesised one is. */
QString synthesisedName(bool bold, bool italic)
{
	const QVector<QString> labels = synthesisedLabels();

	if (bold && italic)
		return labels.at(3);
	if (bold)
		return labels.at(1);
	if (italic)
		return labels.at(2);

	return labels.at(0);
}

} // namespace

QString describeFontChoice(const FontChoice &choice)
{
	QStringList parts;

	/*
	 * The face's own name, without the family in front of it, because the family is right there
	 * beside it: "DejaVu Sans · DejaVu Sans Condensed Bold" says the same thing twice.
	 */
	parts.append(choice.family);
	parts.append(choice.styleName.isEmpty() ? synthesisedName(choice.bold, choice.italic) : choice.styleName);

	if (choice.underline)
		parts.append(moduleText("FontPicker.Underline"));
	if (choice.strikeOut)
		parts.append(moduleText("FontPicker.Strikeout"));

	return parts.join(QStringLiteral(" · "));
}

FontPickerDialog::FontPickerDialog(const FontChoice &initial, QWidget *parent) : QDialog(parent), initial(initial)
{
	setWindowTitle(moduleText("FontPicker.Title"));
	setModal(true);
	resize(680, 480);

	auto *layout = new QVBoxLayout(this);

	auto *lists = new QGridLayout;
	lists->setColumnStretch(0, 3);
	lists->setColumnStretch(1, 2);

	lists->addWidget(new QLabel(moduleText("FontPicker.Family"), this), 0, 0);
	lists->addWidget(new QLabel(moduleText("FontPicker.Face"), this), 0, 1);

	search = new QLineEdit(this);
	search->setClearButtonEnabled(true);
	search->setPlaceholderText(moduleText("FontPicker.Search"));
	lists->addWidget(search, 1, 0);

	familyList = new QListWidget(this);
	faceList = new QListWidget(this);
	lists->addWidget(familyList, 2, 0);
	/*
	 * Spans the search row as well, so the two lists end level at the bottom and the face list
	 * is the taller of the two rather than starting a row down for no reason.
	 */
	lists->addWidget(faceList, 1, 1, 2, 1);
	lists->setRowStretch(2, 1);

	layout->addLayout(lists, 1);

	auto *lower = new QGridLayout;
	lower->setColumnStretch(1, 1);

	auto *effects = new QGroupBox(moduleText("FontPicker.Effects"), this);
	auto *effectsLayout = new QVBoxLayout(effects);
	underline = new QCheckBox(moduleText("FontPicker.Underline"), effects);
	strikeOut = new QCheckBox(moduleText("FontPicker.Strikeout"), effects);
	effectsLayout->addWidget(underline);
	effectsLayout->addWidget(strikeOut);
	effectsLayout->addStretch();
	lower->addWidget(effects, 0, 0);

	auto *sampleBox = new QGroupBox(moduleText("FontPicker.Sample"), this);
	auto *sampleLayout = new QVBoxLayout(sampleBox);
	sample = new QLabel(sampleBox);
	sample->setAlignment(Qt::AlignCenter);
	sample->setMinimumHeight(64);
	/*
	 * A sample in a face this narrow window cannot fit must not widen the window: the family
	 * list is what the dialog is for, and a run of Devanagari is quite capable of pushing it off
	 * the screen.
	 */
	sample->setTextInteractionFlags(Qt::NoTextInteraction);
	sampleLayout->addWidget(sample);
	lower->addWidget(sampleBox, 0, 1);

	layout->addLayout(lower);

	auto *systemRow = new QHBoxLayout;
	systemRow->addWidget(new QLabel(moduleText("FontPicker.WritingSystem"), this));
	writingSystem = new QComboBox(this);
	writingSystem->addItem(moduleText("FontPicker.WritingSystem.Any"), static_cast<int>(QFontDatabase::Any));
	for (QFontDatabase::WritingSystem system : QFontDatabase::writingSystems()) {
		if (system == QFontDatabase::Any)
			continue;
		writingSystem->addItem(QFontDatabase::writingSystemName(system), static_cast<int>(system));
	}
	systemRow->addWidget(writingSystem, 1);
	layout->addLayout(systemRow);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	connect(search, &QLineEdit::textChanged, this, &FontPickerDialog::refillFamilies);
	connect(writingSystem, &QComboBox::currentIndexChanged, this, &FontPickerDialog::refillFamilies);
	connect(familyList, &QListWidget::currentRowChanged, this, [this](int) {
		if (!populating)
			refillFaces();
	});
	connect(faceList, &QListWidget::currentRowChanged, this, [this](int) {
		if (!populating)
			updateSample();
	});
	connect(underline, &QCheckBox::toggled, this, &FontPickerDialog::updateSample);
	connect(strikeOut, &QCheckBox::toggled, this, &FontPickerDialog::updateSample);

	/* Double-clicking a face is the same as picking it and pressing OK, as in every list like it. */
	connect(faceList, &QListWidget::itemDoubleClicked, this, &QDialog::accept);

	underline->setChecked(initial.underline);
	strikeOut->setChecked(initial.strikeOut);

	refillFamilies();
	search->setFocus();
}

void FontPickerDialog::refillFamilies()
{
	const bool wasPopulating = populating;
	populating = true;

	/*
	 * What is selected now, not what the dialog opened on: retyping the filter must not throw
	 * away a family already picked, and the filter is retyped a character at a time.
	 */
	const QString keep = familyList->count() > 0 ? selectedFamily() : initial.family;

	familyList->clear();

	const auto system = static_cast<QFontDatabase::WritingSystem>(writingSystem->currentData().toInt());
	const QString filter = search->text().trimmed();

	QStringList families = QFontDatabase::families(system);

	/*
	 * A family the style names but this machine does not have is put at the top rather than left
	 * out. The roll was designed somewhere that had it, the picker is where somebody would go to
	 * see what it is currently set to, and a list that simply omits it says the style is set to
	 * nothing. It is only offered under "Any", since a font nobody has declares no writing system.
	 */
	if (system == QFontDatabase::Any && !initial.family.isEmpty() && !families.contains(initial.family))
		families.prepend(initial.family);

	for (const QString &family : families) {
		if (!filter.isEmpty() && !family.contains(filter, Qt::CaseInsensitive))
			continue;

		familyList->addItem(family);
	}

	populating = wasPopulating;

	/*
	 * Falls back to the first match rather than to nothing, so a filter that excludes the current
	 * family still leaves a face list to look at. The pick itself is only committed on OK.
	 */
	if (!selectFamily(keep) && familyList->count() > 0)
		familyList->setCurrentRow(0);

	refillFaces();
}

bool FontPickerDialog::selectFamily(const QString &family)
{
	if (family.isEmpty())
		return false;

	const QList<QListWidgetItem *> matches = familyList->findItems(family, Qt::MatchFixedString | Qt::MatchExactly);
	if (matches.isEmpty())
		return false;

	familyList->setCurrentItem(matches.first());
	familyList->scrollToItem(matches.first());
	return true;
}

QString FontPickerDialog::selectedFamily() const
{
	const QListWidgetItem *item = familyList->currentItem();
	return item ? item->text() : QString();
}

void FontPickerDialog::refillFaces()
{
	const bool wasPopulating = populating;
	populating = true;

	const QString family = selectedFamily();

	faces.clear();
	faceList->clear();

	/*
	 * No family selected at all, which is what a filter matching nothing leaves behind. The list
	 * is left empty rather than filled with the synthesised faces below: those would be the faces
	 * of no family, and accepting the dialog on one would quietly drop the face the style arrived
	 * with. With nothing here, choice() keeps what it opened on.
	 */
	if (family.isEmpty()) {
		populating = wasPopulating;
		updateSample();
		return;
	}

	bool hasUpright = false;
	bool hasBold = false;
	bool hasItalic = false;
	bool hasBoldItalic = false;

	for (const QString &name : fontStyleNames(family)) {
		Face face;
		face.label = name;
		face.styleName = name;
		/*
		 * Asked of the database rather than read out of the name. "Book" is upright and
		 * regular, "Oblique" is the family's italic, and a family is free to call its faces
		 * whatever it likes -- the database is the only thing that knows which is which, and
		 * these flags are what a machine without this face falls back to.
		 */
		face.bold = QFontDatabase::bold(family, name);
		face.italic = QFontDatabase::italic(family, name);
		faces.append(face);

		hasUpright = hasUpright || (!face.bold && !face.italic);
		hasBold = hasBold || (face.bold && !face.italic);
		hasItalic = hasItalic || (!face.bold && face.italic);
		hasBoldItalic = hasBoldItalic || (face.bold && face.italic);
	}

	/*
	 * The face the style already names, when this machine has no such face to offer it from -- a
	 * roll designed somewhere that had the font, opened here. Listed and marked rather than left
	 * out, because leaving it out would mean opening the picker to see what a roll is set in and
	 * closing it again rewrote that roll from Semibold to a bold flag, permanently, on the one
	 * machine least able to notice.
	 */
	if (family == initial.family && !initial.styleName.isEmpty() &&
	    !fontStyleAvailable(family, initial.styleName)) {
		Face face;
		face.label = QStringLiteral("%1 %2").arg(initial.styleName, moduleText("FontPicker.Face.Missing"));
		face.styleName = initial.styleName;
		face.bold = initial.bold;
		face.italic = initial.italic;
		faces.append(face);
	}

	/*
	 * The faces the family does not ship, offered anyway. Qt thickens and slants the letterforms
	 * itself, which is precisely what the bold and italic checkboxes used to get, so leaving them
	 * out would be a feature removed rather than a dropdown replaced. Marked, because a
	 * synthesised bold is a rasteriser's guess at a face a designer never drew.
	 */
	const QVector<QString> labels = synthesisedLabels();
	const QString mark = moduleText("FontPicker.Face.Synthesised");

	const auto addSynthesised = [&](int labelIndex, bool bold, bool italic) {
		Face face;
		face.label = QStringLiteral("%1 %2").arg(labels.at(labelIndex), mark);
		face.bold = bold;
		face.italic = italic;
		faces.append(face);
	};

	/*
	 * Offered only for a family with no upright face at all -- one that ships italics alone, or
	 * one this machine does not have. Every ordinary family answers "regular" with a face of its
	 * own, under whatever name it gives it.
	 */
	if (!hasUpright)
		addSynthesised(0, false, false);
	if (!hasBold)
		addSynthesised(1, true, false);
	if (!hasItalic)
		addSynthesised(2, false, true);
	if (!hasBoldItalic)
		addSynthesised(3, true, true);

	for (const Face &face : faces)
		faceList->addItem(face.label);

	/*
	 * Which face to land on. A style that names one is matched by name. A style from before the
	 * picker names none, and is matched by the flags it does carry -- against the family's real
	 * faces first, so opening the picker on an old bold heading lands on the family's designed
	 * Bold rather than on the synthesised one underneath it.
	 */
	int selected = -1;

	if (family == initial.family && !initial.styleName.isEmpty()) {
		for (int i = 0; i < faces.size(); ++i) {
			if (faces.at(i).styleName.compare(initial.styleName, Qt::CaseInsensitive) == 0) {
				selected = i;
				break;
			}
		}
	}

	if (selected < 0 && family == initial.family) {
		for (int i = 0; i < faces.size(); ++i) {
			const Face &face = faces.at(i);
			if (face.bold == initial.bold && face.italic == initial.italic) {
				selected = i;
				if (!face.styleName.isEmpty())
					break;
			}
		}
	}

	populating = wasPopulating;

	faceList->setCurrentRow(selected >= 0 ? selected : 0);
	updateSample();
}

void FontPickerDialog::updateSample()
{
	if (populating)
		return;

	const FontChoice picked = choice();

	QFont font(picked.family);
	font.setPointSize(kSamplePointSize);
	font.setBold(picked.bold);
	font.setItalic(picked.italic);
	/*
	 * Guarded the same way the renderer guards it, so the sample is what the roll will look
	 * like rather than what it would look like if the face were installed.
	 */
	if (!picked.styleName.isEmpty() && fontStyleAvailable(picked.family, picked.styleName))
		font.setStyleName(picked.styleName);
	font.setUnderline(picked.underline);
	font.setStrikeOut(picked.strikeOut);

	sample->setFont(font);

	const auto system = static_cast<QFontDatabase::WritingSystem>(writingSystem->currentData().toInt());
	const QString text = system == QFontDatabase::Any ? moduleText("FontPicker.Sample.Text")
							  : QFontDatabase::writingSystemSample(system);
	sample->setText(text);
}

FontChoice FontPickerDialog::choice() const
{
	FontChoice picked = initial;

	const QString family = selectedFamily();
	if (!family.isEmpty())
		picked.family = family;

	const int row = faceList->currentRow();
	if (row >= 0 && row < faces.size()) {
		const Face &face = faces.at(row);
		picked.styleName = face.styleName;
		picked.bold = face.bold;
		picked.italic = face.italic;
	}

	picked.underline = underline->isChecked();
	picked.strikeOut = strikeOut->isChecked();

	return picked;
}

} // namespace closingtime
