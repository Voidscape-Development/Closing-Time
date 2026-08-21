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

#include "ui/FontDialog.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "render/FontResolution.hpp"
#include "util/FontFiles.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* The column a row's family name, state and stand-in live in. */
enum Column { FamilyColumn, StateColumn, SubstituteColumn };

QString describeState(const FontStatus &status)
{
	/*
	 * Drawn from the roll's own file: this machine does not have the family and the bundle is
	 * what is supplying it. Asked separately from `available`, which the registration has
	 * already made true by the time anybody opens this window.
	 */
	if (status.fromBundle)
		return moduleText("Fonts.State.Bundled");

	if (status.bundled)
		return moduleText("Fonts.State.InstalledAndBundled");

	if (status.available)
		return moduleText("Fonts.State.Installed");

	if (!status.substitute.isEmpty())
		return moduleText("Fonts.State.Substituted");

	return moduleText("Fonts.State.Missing");
}

/*
 * The same for one face, which has a different worst case and so a different worst message.
 *
 * A missing family re-wraps every line and is unmissable once it goes to air. A missing face is
 * not: the family is there, the layout holds, and the roll comes out in the nearest weight Qt can
 * reach. Nothing about looking at it says so, which is the whole reason this row exists, so the
 * row says what will happen rather than only that something is absent.
 */
QString describeFaceState(const FontFaceStatus &face)
{
	if (face.fromBundle)
		return moduleText("Fonts.Face.Bundled");

	if (face.available)
		return face.bundled ? moduleText("Fonts.Face.InstalledAndBundled") : moduleText("Fonts.Face.Installed");

	if (face.bundled)
		return moduleText("Fonts.Face.CarriedNotInstalled");

	return moduleText("Fonts.Face.Missing");
}

} // namespace

FontDialog::FontDialog(Document *document, QWidget *parent) : QDialog(parent), document(document)
{
	setWindowTitle(moduleText("Fonts.Title"));
	setModal(true);
	resize(680, 420);

	auto *layout = new QVBoxLayout(this);

	auto *intro = new QLabel(moduleText("Fonts.Intro"), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	table = new QTreeWidget(this);
	/*
	 * Decorated, because a family with faces under it has children now. A roll that has never
	 * named a face has none, so the twisty column costs it a few pixels of indent and nothing
	 * else -- and a roll that has named one is exactly the roll that wants to see them.
	 */
	table->setUniformRowHeights(true);
	table->setColumnCount(3);
	table->setHeaderLabels({moduleText("Fonts.Column.Family"), moduleText("Fonts.Column.State"),
				moduleText("Fonts.Column.Substitute")});
	/*
	 * The name column is sized to its contents and the state column takes what is left, rather
	 * than the other way round. What a row *is* has to survive a narrow window; what is wrong
	 * with it can elide, because the summary underneath says it again in full.
	 */
	table->header()->setSectionResizeMode(FamilyColumn, QHeaderView::ResizeToContents);
	table->header()->setSectionResizeMode(StateColumn, QHeaderView::Stretch);
	table->header()->setSectionResizeMode(SubstituteColumn, QHeaderView::ResizeToContents);
	layout->addWidget(table, 1);

	bundleCheck = new QCheckBox(moduleText("Fonts.Bundle"), this);
	bundleCheck->setToolTip(moduleText("Fonts.Bundle.Tip"));
	layout->addWidget(bundleCheck);

	/*
	 * The licence note is not a warning box that can be clicked away, because it is not about
	 * anything that has gone wrong: carrying a font file is redistributing it, most commercial
	 * licences say something about that, and the person choosing is the only one who can know
	 * what theirs says. It sits under the switch it is about, permanently.
	 */
	noteLabel = new QLabel(moduleText("Fonts.Bundle.Licence"), this);
	noteLabel->setWordWrap(true);
	noteLabel->setEnabled(false);
	layout->addWidget(noteLabel);

	summaryLabel = new QLabel(this);
	summaryLabel->setWordWrap(true);
	layout->addWidget(summaryLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	auto *refreshButton = buttons->addButton(moduleText("Fonts.Refresh"), QDialogButtonBox::ActionRole);
	refreshButton->setToolTip(moduleText("Fonts.Refresh.Tip"));
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
	connect(refreshButton, &QPushButton::clicked, this, &FontDialog::refreshBundleNow);
	connect(bundleCheck, &QCheckBox::toggled, this, &FontDialog::setBundling);

	refreshRows();
}

void FontDialog::refreshRows()
{
	if (!document)
		return;

	populating = true;

	table->clear();

	const QStringList installed = QFontDatabase::families();

	for (const FontStatus &status : fontStatus(*document)) {
		auto *row = new QTreeWidgetItem(table);
		row->setText(FamilyColumn, status.family);
		row->setText(StateColumn, describeState(status));

		/*
		 * A family this machine has needs no stand-in and is offered none: the choice would
		 * be recorded and then never act, which reads as a setting that does nothing. The row
		 * still says where the family stands, which is what it is there for.
		 *
		 * Its faces get a row each, for the same reason and with the same restraint: a family
		 * that is *not* here takes every one of its faces with it, so listing them under a
		 * missing family would be five rows each saying a smaller version of what the family
		 * row has already said.
		 */
		if (status.available) {
			row->setText(SubstituteColumn, QStringLiteral("—"));

			for (const FontFaceStatus &face : status.faces) {
				auto *faceRow = new QTreeWidgetItem(row);
				faceRow->setText(FamilyColumn, face.styleName);
				faceRow->setText(StateColumn, describeFaceState(face));
				faceRow->setText(SubstituteColumn, QStringLiteral("—"));
			}

			/* Expanded, because a missing face is the thing this window is open to find. */
			row->setExpanded(status.hasMissingFace());
			continue;
		}

		auto *combo = new QComboBox(table);
		combo->addItem(moduleText("Fonts.Substitute.None"), QString());
		for (const QString &family : installed)
			combo->addItem(family, family);

		const int index = combo->findData(status.substitute);
		combo->setCurrentIndex(index >= 0 ? index : 0);

		const QString family = status.family;
		connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, family](int) {
			if (populating)
				return;

			/*
			 * Queued, because acting on the choice rebuilds these rows and this combo box
			 * is one of them: deleting the widget that is emitting the signal is a crash
			 * whatever else it is.
			 */
			const QString substitute = combo->currentData().toString();
			QMetaObject::invokeMethod(
				this, [this, family, substitute] { substituteChanged(family, substitute); },
				Qt::QueuedConnection);
		});

		table->setItemWidget(row, SubstituteColumn, combo);
	}

	bundleCheck->setChecked(document->bundleFonts);
	noteLabel->setEnabled(document->bundleFonts);

	populating = false;
	updateSummary();
}

void FontDialog::updateSummary()
{
	if (!document)
		return;

	const qint64 bytes = fontBundleBytes(document->bundledFonts);
	const QString size = QLocale().formattedDataSize(bytes);

	QStringList lines;
	lines.append(moduleText("Fonts.Summary").arg(document->bundledFonts.size()).arg(size));

	const QStringList unresolved = unresolvedFontFamilies(*document);
	if (!unresolved.isEmpty())
		lines.append(moduleText("Fonts.Summary.Unresolved").arg(unresolved.join(QStringLiteral(", "))));

	/*
	 * Named separately from the families, because what happens to them is different and much
	 * quieter: the roll still lays out correctly and goes to air a weight off. Only faces of a
	 * family that *is* here are counted -- under a missing family the family is the problem.
	 */
	QStringList missingFaces;
	for (const FontStatus &status : fontStatus(*document)) {
		if (!status.available)
			continue;

		for (const FontFaceStatus &face : status.faces) {
			if (!face.available)
				missingFaces.append(QStringLiteral("%1 %2").arg(status.family, face.styleName));
		}
	}

	if (!missingFaces.isEmpty())
		lines.append(moduleText("Fonts.Summary.MissingFaces").arg(missingFaces.join(QStringLiteral(", "))));

	summaryLabel->setText(lines.join(QStringLiteral("\n")));
}

void FontDialog::setBundling(bool enabled)
{
	if (!document || populating || document->bundleFonts == enabled)
		return;

	emit documentAboutToChange();
	document->bundleFonts = enabled;
	/*
	 * Switching bundling off empties the bundle rather than leaving the files in the collection
	 * unused: the reason to switch it off is that they should not be there.
	 */
	document->refreshFontBundle();
	emit documentChanged();

	noteLabel->setEnabled(enabled);
	refreshRows();
}

void FontDialog::substituteChanged(const QString &family, const QString &substitute)
{
	if (!document || document->fontSubstitute(family) == substitute)
		return;

	emit documentAboutToChange();
	document->setFontSubstitute(family, substitute);
	/*
	 * A family that has been answered with a stand-in is no longer one to go looking for a file
	 * for, so the bundle is brought back in line with what is left.
	 */
	document->refreshFontBundle();
	emit documentChanged();

	refreshRows();
}

void FontDialog::refreshBundleNow()
{
	if (!document)
		return;

	/* Walking every font directory on the machine is the one thing here that takes a moment. */
	QGuiApplication::setOverrideCursor(Qt::WaitCursor);

	/*
	 * The one call that goes back to the disk for families the document is already carrying.
	 * A refresh after installing the missing font has to see a directory the index was built
	 * before it existed, so the index is dropped as well.
	 */
	clearFontFileIndex();

	QStringList skipped;
	emit documentAboutToChange();
	const bool changed = document->refreshFontBundle(&skipped, true);
	emit documentChanged();

	QGuiApplication::restoreOverrideCursor();

	refreshRows();

	if (!skipped.isEmpty())
		summaryLabel->setText(summaryLabel->text() + QStringLiteral("\n") +
				      moduleText("Fonts.Summary.TooLarge")
					      .arg(skipped.join(QStringLiteral(", ")))
					      .arg(QLocale().formattedDataSize(kMaxBundledFontBytes)));
	else if (!changed)
		summaryLabel->setText(summaryLabel->text() + QStringLiteral("\n") +
				      moduleText("Fonts.Summary.Unchanged"));
}

} // namespace closingtime
