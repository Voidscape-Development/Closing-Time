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

#include "ui/StyleLibraryDialog.hpp"

#include <obs-module.h>

#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QInputDialog>
#include <QComboBox>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

#include "model/StyleLibrary.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* A one-line description of a style, so the two lists are readable without opening anything. */
QString describe(const TextStyle &style)
{
	QStringList parts;
	parts.append(QStringLiteral("%1 %2px").arg(style.family).arg(style.pixelSize));

	/*
	 * The face's own name when the style names one, and the flags it falls back to when it does
	 * not. A style set in Semibold that described itself as neither bold nor italic would read
	 * as the family's regular, which is the one thing it is not.
	 */
	if (!style.styleName.isEmpty())
		parts.append(style.styleName);
	else if (style.bold && style.italic)
		parts.append(moduleText("FontPicker.Face.BoldItalic"));
	else if (style.bold)
		parts.append(moduleText("Designer.Bold"));
	else if (style.italic)
		parts.append(moduleText("Designer.Italic"));

	if (style.underline)
		parts.append(moduleText("FontPicker.Underline"));
	if (style.strikeOut)
		parts.append(moduleText("FontPicker.Strikeout"));
	if (style.fill != TextFill::Solid)
		parts.append(moduleText("Designer.Gradient"));
	if (style.outline.enabled)
		parts.append(moduleText("Designer.Outline"));
	if (style.shadow.enabled)
		parts.append(moduleText("Designer.Shadow"));

	return parts.join(QStringLiteral(" · "));
}

/* And of a panel, so the lists read the same way whichever collection they are showing. */
QString describe(const BackgroundPanel &panel)
{
	QStringList parts;
	parts.append(moduleText(QStringLiteral("Designer.Background.Fill.%1")
					.arg(QString::fromUtf8(backgroundFillId(panel.fill)))
					.toUtf8()
					.constData()));

	if (panel.fill == BackgroundFill::Image && !panel.imagePath.isEmpty()) {
		parts.append(moduleText(QStringLiteral("Designer.Background.Fit.%1")
						.arg(QString::fromUtf8(backgroundImageFitId(panel.imageFit)))
						.toUtf8()
						.constData()));
	}

	if (!panel.hasUniformRadius() || panel.radiusTopLeft > 0.0)
		parts.append(moduleText("Library.Describe.Rounded"));
	if (panel.border.enabled)
		parts.append(moduleText("Designer.Background.Border"));
	if (panel.bleed() > 0.0)
		parts.append(moduleText("Library.Describe.Outset"));
	if (panel.opacity < 1.0)
		parts.append(QStringLiteral("%1 %").arg(qRound(panel.opacity * 100.0)));

	return parts.join(QStringLiteral(" · "));
}

QListWidgetItem *makeItem(const QString &name, const QString &tooltip, bool linked)
{
	auto *item = new QListWidgetItem(linked ? QStringLiteral("%1  ⇄").arg(name) : name);
	item->setData(Qt::UserRole, name);
	item->setToolTip(tooltip);
	return item;
}

} // namespace

StyleLibraryDialog::StyleLibraryDialog(Document *document, QWidget *parent) : QDialog(parent), document(document)
{
	setWindowTitle(moduleText("Library.Title"));
	setModal(false);
	resize(720, 420);

	auto *layout = new QVBoxLayout(this);

	/*
	 * Which collection the lists are showing. A picker rather than a second pair of lists,
	 * because every button between them means the same thing for either kind.
	 */
	auto *kindRow = new QHBoxLayout;
	kindRow->addWidget(new QLabel(moduleText("Library.Kind"), this));
	kindBox = new QComboBox(this);
	kindBox->addItem(moduleText("Library.Kind.Styles"), false);
	kindBox->addItem(moduleText("Library.Kind.Backgrounds"), true);
	kindRow->addWidget(kindBox);
	kindRow->addStretch();
	layout->addLayout(kindRow);

	auto *grid = new QGridLayout;
	layout->addLayout(grid, 1);

	int column = 0;

	if (document) {
		grid->addWidget(new QLabel(moduleText("Library.DocumentPresets"), this), 0, column);
		documentList = new QListWidget(this);
		grid->addWidget(documentList, 1, column);
		grid->setColumnStretch(column, 1);
		++column;

		/* The traffic between the two lists, in the gap between them. */
		auto *middle = new QVBoxLayout;
		middle->addStretch();
		publishButton = new QPushButton(moduleText("Library.Publish"), this);
		publishButton->setToolTip(moduleText("Library.Publish.Tip"));
		linkButton = new QPushButton(moduleText("Library.Link"), this);
		linkButton->setToolTip(moduleText("Library.Link.Tip"));
		copyButton = new QPushButton(moduleText("Library.Copy"), this);
		copyButton->setToolTip(moduleText("Library.Copy.Tip"));
		middle->addWidget(publishButton);
		middle->addWidget(linkButton);
		middle->addWidget(copyButton);
		middle->addStretch();
		grid->addLayout(middle, 1, column);
		++column;
	}

	grid->addWidget(new QLabel(moduleText("Library.LibraryPresets"), this), 0, column);
	libraryList = new QListWidget(this);
	grid->addWidget(libraryList, 1, column);
	grid->setColumnStretch(column, 1);

	auto *libraryButtons = new QVBoxLayout;
	libraryButtons->addStretch();
	renameButton = new QPushButton(moduleText("Library.Rename"), this);
	deleteButton = new QPushButton(moduleText("Library.Delete"), this);
	auto *importButton = new QPushButton(moduleText("Library.Import"), this);
	auto *exportButton = new QPushButton(moduleText("Library.Export"), this);
	libraryButtons->addWidget(renameButton);
	libraryButtons->addWidget(deleteButton);
	libraryButtons->addSpacing(12);
	libraryButtons->addWidget(importButton);
	libraryButtons->addWidget(exportButton);
	libraryButtons->addStretch();
	grid->addLayout(libraryButtons, 1, column + 1);

	pathLabel = new QLabel(this);
	pathLabel->setWordWrap(true);
	pathLabel->setEnabled(false);
	layout->addWidget(pathLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	if (documentList) {
		connect(documentList, &QListWidget::itemSelectionChanged, this, &StyleLibraryDialog::updateButtons);
		connect(publishButton, &QPushButton::clicked, this, &StyleLibraryDialog::publishSelected);
		connect(linkButton, &QPushButton::clicked, this, &StyleLibraryDialog::linkSelected);
		connect(copyButton, &QPushButton::clicked, this, &StyleLibraryDialog::copySelected);
	}

	connect(libraryList, &QListWidget::itemSelectionChanged, this, &StyleLibraryDialog::updateButtons);
	connect(renameButton, &QPushButton::clicked, this, &StyleLibraryDialog::renameSelected);
	connect(deleteButton, &QPushButton::clicked, this, &StyleLibraryDialog::deleteSelected);
	connect(importButton, &QPushButton::clicked, this, &StyleLibraryDialog::importLibrary);
	connect(exportButton, &QPushButton::clicked, this, &StyleLibraryDialog::exportLibrary);
	connect(kindBox, &QComboBox::currentIndexChanged, this, &StyleLibraryDialog::refreshLists);

	refreshLists();
}

bool StyleLibraryDialog::showingBackgrounds() const
{
	return kindBox && kindBox->currentData().toBool();
}

void StyleLibraryDialog::refreshLists()
{
	StyleLibrary &library = StyleLibrary::instance();

	const bool backgrounds = showingBackgrounds();

	if (documentList) {
		const QString selected = selectedDocumentPreset();
		documentList->clear();

		if (backgrounds) {
			for (const BackgroundPreset &preset : document->backgroundPresets) {
				QListWidgetItem *item = makeItem(preset.name, describe(preset.panel), preset.linked);
				documentList->addItem(item);
				if (preset.name == selected)
					documentList->setCurrentItem(item);
			}
		} else {
			for (const StylePreset &preset : document->stylePresets) {
				QListWidgetItem *item = makeItem(preset.name, describe(preset.style), preset.linked);
				documentList->addItem(item);
				if (preset.name == selected)
					documentList->setCurrentItem(item);
			}
		}
	}

	const QString selectedLibrary = selectedLibraryPreset();
	libraryList->clear();

	if (backgrounds) {
		for (const BackgroundPreset &preset : library.backgrounds()) {
			QListWidgetItem *item = makeItem(preset.name, describe(preset.panel), false);
			libraryList->addItem(item);
			if (preset.name == selectedLibrary)
				libraryList->setCurrentItem(item);
		}
	} else {
		for (const StylePreset &preset : library.presets()) {
			QListWidgetItem *item = makeItem(preset.name, describe(preset.style), false);
			libraryList->addItem(item);
			if (preset.name == selectedLibrary)
				libraryList->setCurrentItem(item);
		}
	}

	const QString path = library.filePath();
	pathLabel->setText(path.isEmpty() ? moduleText("Library.NoPath")
					  : moduleText("Library.Path").arg(QDir::toNativeSeparators(path)));

	updateButtons();
}

QString StyleLibraryDialog::selectedDocumentPreset() const
{
	const QListWidgetItem *item = documentList ? documentList->currentItem() : nullptr;
	return item && item->isSelected() ? item->data(Qt::UserRole).toString() : QString();
}

QString StyleLibraryDialog::selectedLibraryPreset() const
{
	const QListWidgetItem *item = libraryList->currentItem();
	return item && item->isSelected() ? item->data(Qt::UserRole).toString() : QString();
}

void StyleLibraryDialog::updateButtons()
{
	const QString library = selectedLibraryPreset();

	if (documentList) {
		const QString local = selectedDocumentPreset();
		publishButton->setEnabled(!local.isEmpty());
		/*
		 * A preset the document already carries under that name cannot be linked without
		 * silently replacing what is there, so the two paths in are kept apart: link brings a
		 * new one in, publish sends an existing one out.
		 */
		const bool clash = showingBackgrounds()
					   ? std::any_of(document->backgroundPresets.cbegin(),
							 document->backgroundPresets.cend(),
							 [&library](const BackgroundPreset &preset) {
								 return preset.name == library && !preset.linked;
							 })
					   : std::any_of(document->stylePresets.cbegin(), document->stylePresets.cend(),
							 [&library](const StylePreset &preset) {
								 return preset.name == library && !preset.linked;
							 });
		linkButton->setEnabled(!library.isEmpty() && !clash);
		copyButton->setEnabled(!library.isEmpty() && !clash);
	}

	renameButton->setEnabled(!library.isEmpty());
	deleteButton->setEnabled(!library.isEmpty());
}

void StyleLibraryDialog::publishSelected()
{
	const QString name = selectedDocumentPreset();
	if (name.isEmpty())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	const bool backgrounds = showingBackgrounds();

	const TextStyle *style = backgrounds ? nullptr : document->findStylePreset(name);
	const BackgroundPanel *panel = backgrounds ? document->findBackgroundPreset(name) : nullptr;
	if (!style && !panel)
		return;

	const bool taken = backgrounds ? library.containsBackground(name) : library.contains(name);
	if (taken && QMessageBox::question(this, moduleText("Library.Publish"),
					   moduleText("Library.Replace").arg(name)) != QMessageBox::Yes)
		return;

	if (backgrounds)
		library.setBackground(name, *panel);
	else
		library.set(name, *style);

	/*
	 * Published and then linked, so the next library edit reaches this roll too. Publishing a
	 * style and having the roll it came from carry on ignoring the library would be a copy, not a
	 * publication.
	 */
	emit documentAboutToChange();
	if (backgrounds)
		document->linkBackgroundPreset(name);
	else
		document->linkStylePreset(name);
	emit documentChanged();

	refreshLists();
}

void StyleLibraryDialog::linkSelected()
{
	const QString name = selectedLibraryPreset();
	if (name.isEmpty())
		return;

	emit documentAboutToChange();
	if (showingBackgrounds())
		document->linkBackgroundPreset(name);
	else
		document->linkStylePreset(name);
	emit documentChanged();

	refreshLists();
}

void StyleLibraryDialog::copySelected()
{
	const QString name = selectedLibraryPreset();
	if (name.isEmpty())
		return;

	/*
	 * The same style, unlinked: a starting point to edit for this roll alone. Without this the
	 * only way to base one roll's title on the house style without being bound to it would be to
	 * link it and then break the link.
	 */
	if (showingBackgrounds()) {
		BackgroundPanel panel;
		if (!StyleLibrary::instance().findBackground(name, &panel))
			return;

		emit documentAboutToChange();
		document->setBackgroundPreset(name, panel);
		emit documentChanged();
	} else {
		TextStyle style;
		if (!StyleLibrary::instance().find(name, &style))
			return;

		emit documentAboutToChange();
		document->setStylePreset(name, style);
		emit documentChanged();
	}

	refreshLists();
}

void StyleLibraryDialog::renameSelected()
{
	const QString name = selectedLibraryPreset();
	if (name.isEmpty())
		return;

	bool accepted = false;
	const QString renamed = QInputDialog::getText(this, moduleText("Library.Rename"),
						      moduleText("Library.RenameLabel"), QLineEdit::Normal, name,
						      &accepted);
	if (!accepted || renamed.isEmpty() || renamed == name)
		return;

	StyleLibrary &library = StyleLibrary::instance();
	const bool backgrounds = showingBackgrounds();

	if (backgrounds ? library.containsBackground(renamed) : library.contains(renamed)) {
		QMessageBox::warning(this, moduleText("Library.Rename"), moduleText("Library.NameTaken").arg(renamed));
		return;
	}

	if (backgrounds)
		library.renameBackground(name, renamed);
	else
		library.rename(name, renamed);

	/*
	 * The library remembers the rename, and every document brought up to date against it follows:
	 * this one now, the other loaded rolls on their next tick, and a scene collection that is not
	 * open whenever it next loads. Doing it here as well as leaving it to the poll is what keeps
	 * the roll being designed from showing a stale binding for the second in between.
	 */
	if (document) {
		emit documentAboutToChange();
		const bool moved = document->applyLibraryRenames();
		emit documentChanged();

		/*
		 * A linked preset still sitting under the old name is one the migration declined to
		 * move -- the document already has something else called `renamed`, and merging the two
		 * is not a rename. Saying so beats leaving the user to notice a binding that did not
		 * follow; every other case moved, or had nothing here to move.
		 */
		const bool stillBound =
			backgrounds
				? std::any_of(document->backgroundPresets.cbegin(), document->backgroundPresets.cend(),
					      [&name](const BackgroundPreset &preset) {
						      return preset.linked && preset.name == name;
					      })
				: std::any_of(document->stylePresets.cbegin(), document->stylePresets.cend(),
					      [&name](const StylePreset &preset) {
						      return preset.linked && preset.name == name;
					      });

		if (!moved && stillBound)
			QMessageBox::information(this, moduleText("Library.Rename"),
						 moduleText("Library.RenameSkipped").arg(name, renamed));
	}

	refreshLists();
}

void StyleLibraryDialog::deleteSelected()
{
	const QString name = selectedLibraryPreset();
	if (name.isEmpty())
		return;

	if (QMessageBox::question(this, moduleText("Library.Delete"), moduleText("Library.DeleteConfirm").arg(name)) !=
	    QMessageBox::Yes)
		return;

	if (showingBackgrounds())
		StyleLibrary::instance().removeBackground(name);
	else
		StyleLibrary::instance().remove(name);

	refreshLists();
}

void StyleLibraryDialog::importLibrary()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Library.Import"), QString(),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, moduleText("Library.Import"), file.errorString());
		return;
	}

	QVector<StylePreset> imported;
	QVector<BackgroundPreset> importedBackgrounds;
	QString error;
	if (!StyleLibrary::parseJson(QString::fromUtf8(file.readAll()), &imported, &importedBackgrounds, &error)) {
		QMessageBox::warning(this, moduleText("Library.Import"), error);
		return;
	}

	/*
	 * Merged rather than replacing, and the imported side wins a clash. Replacing would throw
	 * away every style on the machine to take one collection's; a merge is what carrying a house
	 * style from one machine to another actually means.
	 *
	 * Both collections come in whichever the lists happen to be showing: a file is imported as a
	 * whole, and leaving half of it on disk because a picker was set one way would be a surprise
	 * with nothing on screen to explain it.
	 */
	StyleLibrary &library = StyleLibrary::instance();

	QVector<StylePreset> merged = library.presets();
	for (const StylePreset &preset : imported) {
		const auto existing = std::find_if(merged.begin(), merged.end(), [&preset](const StylePreset &entry) {
			return entry.name == preset.name;
		});

		if (existing != merged.end())
			existing->style = preset.style;
		else
			merged.append(preset);
	}

	QVector<BackgroundPreset> mergedBackgrounds = library.backgrounds();
	for (const BackgroundPreset &preset : importedBackgrounds) {
		const auto existing =
			std::find_if(mergedBackgrounds.begin(), mergedBackgrounds.end(),
				     [&preset](const BackgroundPreset &entry) { return entry.name == preset.name; });

		if (existing != mergedBackgrounds.end())
			existing->panel = preset.panel;
		else
			mergedBackgrounds.append(preset);
	}

	library.replaceAll(merged);
	library.replaceAllBackgrounds(mergedBackgrounds);
	refreshLists();
}

void StyleLibraryDialog::exportLibrary()
{
	const QString path = QFileDialog::getSaveFileName(this, moduleText("Library.Export"),
							  QStringLiteral("style-presets.json"),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QMessageBox::warning(this, moduleText("Library.Export"), file.errorString());
		return;
	}

	file.write(StyleLibrary::instance().toJson().toUtf8());
}

} // namespace closingtime
