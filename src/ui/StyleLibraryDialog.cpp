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

	if (style.bold)
		parts.append(moduleText("Designer.Bold"));
	if (style.italic)
		parts.append(moduleText("Designer.Italic"));
	if (style.fill != TextFill::Solid)
		parts.append(moduleText("Designer.Gradient"));
	if (style.outline.enabled)
		parts.append(moduleText("Designer.Outline"));
	if (style.shadow.enabled)
		parts.append(moduleText("Designer.Shadow"));

	return parts.join(QStringLiteral(" · "));
}

QListWidgetItem *makeItem(const StylePreset &preset, bool linked)
{
	auto *item = new QListWidgetItem(linked ? QStringLiteral("%1  ⇄").arg(preset.name) : preset.name);
	item->setData(Qt::UserRole, preset.name);
	item->setToolTip(describe(preset.style));
	return item;
}

} // namespace

StyleLibraryDialog::StyleLibraryDialog(Document *document, QWidget *parent) : QDialog(parent), document(document)
{
	setWindowTitle(moduleText("Library.Title"));
	setModal(false);
	resize(720, 420);

	auto *layout = new QVBoxLayout(this);
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

	refreshLists();
}

void StyleLibraryDialog::refreshLists()
{
	StyleLibrary &library = StyleLibrary::instance();

	if (documentList) {
		const QString selected = selectedDocumentPreset();
		documentList->clear();
		for (const StylePreset &preset : document->stylePresets) {
			QListWidgetItem *item = makeItem(preset, preset.linked);
			documentList->addItem(item);
			if (preset.name == selected)
				documentList->setCurrentItem(item);
		}
	}

	const QString selectedLibrary = selectedLibraryPreset();
	libraryList->clear();
	for (const StylePreset &preset : library.presets()) {
		QListWidgetItem *item = makeItem(preset, false);
		libraryList->addItem(item);
		if (preset.name == selectedLibrary)
			libraryList->setCurrentItem(item);
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
		const bool clash = std::any_of(document->stylePresets.cbegin(), document->stylePresets.cend(),
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

	const TextStyle *style = document->findStylePreset(name);
	if (!style)
		return;

	if (StyleLibrary::instance().contains(name) &&
	    QMessageBox::question(this, moduleText("Library.Publish"), moduleText("Library.Replace").arg(name)) !=
		    QMessageBox::Yes)
		return;

	StyleLibrary::instance().set(name, *style);

	/*
	 * Published and then linked, so the next library edit reaches this roll too. Publishing a
	 * style and having the roll it came from carry on ignoring the library would be a copy, not a
	 * publication.
	 */
	emit documentAboutToChange();
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
	document->linkStylePreset(name);
	emit documentChanged();

	refreshLists();
}

void StyleLibraryDialog::copySelected()
{
	const QString name = selectedLibraryPreset();
	if (name.isEmpty())
		return;

	TextStyle style;
	if (!StyleLibrary::instance().find(name, &style))
		return;

	/*
	 * The same style, unlinked: a starting point to edit for this roll alone. Without this the
	 * only way to base one roll's title on the house style without being bound to it would be to
	 * link it and then break the link.
	 */
	emit documentAboutToChange();
	document->setStylePreset(name, style);
	emit documentChanged();

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

	if (StyleLibrary::instance().contains(renamed)) {
		QMessageBox::warning(this, moduleText("Library.Rename"), moduleText("Library.NameTaken").arg(renamed));
		return;
	}

	StyleLibrary::instance().rename(name, renamed);

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
		const bool stillBound = std::any_of(document->stylePresets.cbegin(), document->stylePresets.cend(),
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
	QString error;
	if (!StyleLibrary::parseJson(QString::fromUtf8(file.readAll()), &imported, &error)) {
		QMessageBox::warning(this, moduleText("Library.Import"), error);
		return;
	}

	/*
	 * Merged rather than replacing, and the imported side wins a clash. Replacing would throw
	 * away every style on the machine to take one collection's; a merge is what carrying a house
	 * style from one machine to another actually means.
	 */
	QVector<StylePreset> merged = StyleLibrary::instance().presets();
	for (const StylePreset &preset : imported) {
		const auto existing = std::find_if(merged.begin(), merged.end(), [&preset](const StylePreset &entry) {
			return entry.name == preset.name;
		});

		if (existing != merged.end())
			existing->style = preset.style;
		else
			merged.append(preset);
	}

	StyleLibrary::instance().replaceAll(merged);
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
