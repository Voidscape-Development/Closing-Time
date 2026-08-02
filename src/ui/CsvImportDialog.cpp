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

#include "ui/CsvImportDialog.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Rows beyond this are parsed and imported but not shown, to keep the dialog responsive. */
constexpr int kPreviewRowLimit = 200;

struct DelimiterOption {
	const char *labelKey;
	QChar value;
};

const DelimiterOption kDelimiters[] = {
	{"Import.Delimiter.Comma", QLatin1Char(',')},
	{"Import.Delimiter.Tab", QLatin1Char('\t')},
	{"Import.Delimiter.Semicolon", QLatin1Char(';')},
	{"Import.Delimiter.Pipe", QLatin1Char('|')},
};

} // namespace

CsvImportDialog::CsvImportDialog(SectionType type, QWidget *parent) : QDialog(parent), targetType(type)
{
	setWindowTitle(moduleText("Import.Title"));
	setModal(true);
	resize(760, 560);

	auto *outer = new QVBoxLayout(this);

	auto *form = new QFormLayout();

	auto *fileRow = new QWidget(this);
	auto *fileLayout = new QHBoxLayout(fileRow);
	fileLayout->setContentsMargins(0, 0, 0, 0);
	pathEdit = new QLineEdit(fileRow);
	browseButton = new QPushButton(moduleText("Import.Browse"), fileRow);
	fileLayout->addWidget(pathEdit);
	fileLayout->addWidget(browseButton);
	form->addRow(moduleText("Import.File"), fileRow);

	delimiterBox = new QComboBox(this);
	for (const auto &option : kDelimiters)
		delimiterBox->addItem(moduleText(option.labelKey), option.value);
	form->addRow(moduleText("Import.Delimiter"), delimiterBox);

	headerBox = new QCheckBox(moduleText("Import.HasHeader"), this);
	headerBox->setChecked(true);
	form->addRow(QString(), headerBox);

	skipEmptyBox = new QCheckBox(moduleText("Import.SkipEmpty"), this);
	skipEmptyBox->setChecked(true);
	form->addRow(QString(), skipEmptyBox);

	replaceBox = new QCheckBox(moduleText("Import.Replace"), this);
	form->addRow(QString(), replaceBox);

	outer->addLayout(form);

	outer->addWidget(new QLabel(moduleText("Import.MappingHint"), this));

	auto *mappingScroll = new QScrollArea(this);
	mappingScroll->setWidgetResizable(true);
	mappingScroll->setFixedHeight(72);
	mappingRow = new QWidget(mappingScroll);
	mappingLayout = new QHBoxLayout(mappingRow);
	mappingLayout->setContentsMargins(0, 0, 0, 0);
	mappingScroll->setWidget(mappingRow);
	outer->addWidget(mappingScroll);

	preview = new QTableWidget(this);
	preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
	preview->verticalHeader()->setVisible(false);
	outer->addWidget(preview, 1);

	statusLabel = new QLabel(this);
	outer->addWidget(statusLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	outer->addWidget(buttons);

	connect(browseButton, &QPushButton::clicked, this, &CsvImportDialog::browse);
	connect(pathEdit, &QLineEdit::editingFinished, this, &CsvImportDialog::reloadFile);
	connect(delimiterBox, &QComboBox::currentIndexChanged, this, &CsvImportDialog::reloadFile);
	connect(headerBox, &QCheckBox::toggled, this, &CsvImportDialog::refreshPreview);
	connect(buttons, &QDialogButtonBox::accepted, this, &CsvImportDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &CsvImportDialog::reject);
}

bool CsvImportDialog::replaceExisting() const
{
	return replaceBox->isChecked();
}

QVector<CsvImportDialog::Field> CsvImportDialog::availableFields() const
{
	switch (targetType) {
	case SectionType::Bridged:
		return {Field::Ignore, Field::Text, Field::SecondaryText};

	case SectionType::LogoList:
	case SectionType::MultiLogoList:
		return {Field::Ignore, Field::LogoPath, Field::LogoHeight};

	default:
		return {Field::Ignore, Field::Text};
	}
}

QString CsvImportDialog::fieldLabel(Field field) const
{
	switch (field) {
	case Field::Text:
		return targetType == SectionType::Bridged ? moduleText("Designer.Column.Left")
							  : moduleText("Designer.Column.Text");
	case Field::SecondaryText:
		return moduleText("Designer.Column.Right");
	case Field::LogoPath:
		return moduleText("Designer.Column.Logo");
	case Field::LogoHeight:
		return moduleText("Designer.Column.Height");
	case Field::Ignore:
	default:
		return moduleText("Import.Field.Ignore");
	}
}

void CsvImportDialog::browse()
{
	const QString path =
		QFileDialog::getOpenFileName(this, moduleText("Import.ChooseFile"), pathEdit->text(),
					     moduleText("Import.FileFilter") + QStringLiteral(" (*.csv *.tsv *.txt)"));
	if (path.isEmpty())
		return;

	pathEdit->setText(path);

	/*
	 * Guess the delimiter from the file's own contents so the common cases -- a comma CSV
	 * or a tab-separated export -- need no extra clicks.
	 */
	QString sample;
	if (QFile file(path); file.open(QIODevice::ReadOnly))
		sample = QString::fromUtf8(file.read(64 * 1024));

	if (!sample.isEmpty()) {
		const QChar guessed = guessDelimiter(sample);
		const int index = delimiterBox->findData(guessed);
		if (index >= 0)
			delimiterBox->setCurrentIndex(index);
	}

	reloadFile();
}

void CsvImportDialog::reloadFile()
{
	table.clear();

	const QString path = pathEdit->text();
	if (path.isEmpty()) {
		refreshPreview();
		return;
	}

	const QChar delimiter = delimiterBox->currentData().toChar();

	QString error;
	if (!parseCsvFile(path, delimiter, &table, &error)) {
		table.clear();
		statusLabel->setText(moduleText("Import.ReadFailed") + QStringLiteral(": ") + error);
	}

	rebuildMappingRow();
	refreshPreview();
}

void CsvImportDialog::rebuildMappingRow()
{
	/* The combos are children of the per-column cells, so clearing the row frees them. */
	mapping.clear();
	while (QLayoutItem *item = mappingLayout->takeAt(0)) {
		delete item->widget();
		delete item;
	}

	int columnCount = 0;
	for (const QStringList &row : table)
		columnCount = std::max(columnCount, static_cast<int>(row.size()));

	const QVector<Field> fields = availableFields();

	for (int column = 0; column < columnCount; ++column) {
		auto *cell = new QWidget(mappingRow);
		auto *layout = new QVBoxLayout(cell);
		layout->setContentsMargins(0, 0, 0, 0);

		auto *box = new QComboBox(cell);
		for (Field field : fields)
			box->addItem(fieldLabel(field), static_cast<int>(field));

		/*
		 * Default mapping walks the real fields in order and then ignores the rest, which
		 * matches how people actually lay these spreadsheets out.
		 */
		box->setCurrentIndex(column + 1 < fields.size() ? column + 1 : 0);

		layout->addWidget(
			new QLabel(QStringLiteral("%1 %2").arg(moduleText("Import.Column")).arg(column + 1), cell));
		layout->addWidget(box);

		mappingLayout->addWidget(cell);
		mapping.append(box);

		connect(box, &QComboBox::currentIndexChanged, this, &CsvImportDialog::refreshPreview);
	}

	mappingLayout->addStretch();
}

void CsvImportDialog::refreshPreview()
{
	preview->clear();

	const bool hasHeader = headerBox->isChecked();
	const int firstRow = hasHeader && !table.isEmpty() ? 1 : 0;

	int columnCount = 0;
	for (const QStringList &row : table)
		columnCount = std::max(columnCount, static_cast<int>(row.size()));

	preview->setColumnCount(columnCount);

	QStringList headers;
	for (int column = 0; column < columnCount; ++column) {
		if (hasHeader && !table.isEmpty() && column < table.first().size())
			headers.append(table.first().at(column));
		else
			headers.append(QStringLiteral("%1 %2").arg(moduleText("Import.Column")).arg(column + 1));
	}
	preview->setHorizontalHeaderLabels(headers);

	const int dataRows = std::max(0, static_cast<int>(table.size()) - firstRow);
	const int shown = std::min(dataRows, kPreviewRowLimit);
	preview->setRowCount(shown);

	for (int row = 0; row < shown; ++row) {
		const QStringList &source = table.at(firstRow + row);
		for (int column = 0; column < columnCount; ++column) {
			const QString value = column < source.size() ? source.at(column) : QString();
			preview->setItem(row, column, new QTableWidgetItem(value));
		}
	}

	preview->resizeColumnsToContents();

	QString status = QStringLiteral("%1 %2").arg(moduleText("Import.RowCount")).arg(dataRows);
	if (dataRows > shown)
		status += QStringLiteral(" — %1 %2").arg(moduleText("Import.PreviewLimit")).arg(shown);
	statusLabel->setText(status);
}

void CsvImportDialog::accept()
{
	imported.clear();

	const bool hasHeader = headerBox->isChecked();
	const bool skipEmpty = skipEmptyBox->isChecked();
	const int firstRow = hasHeader && !table.isEmpty() ? 1 : 0;

	for (int row = firstRow; row < table.size(); ++row) {
		const QStringList &source = table.at(row);

		Entry entry;
		bool hasContent = false;

		for (int column = 0; column < mapping.size() && column < source.size(); ++column) {
			const auto field = static_cast<Field>(mapping.at(column)->currentData().toInt());
			const QString value = source.at(column).trimmed();
			if (field == Field::Ignore)
				continue;

			switch (field) {
			case Field::Text:
				entry.text = value;
				break;
			case Field::SecondaryText:
				entry.secondaryText = value;
				break;
			case Field::LogoPath:
				entry.logo.path = value;
				break;
			case Field::LogoHeight:
				if (const int height = value.toInt(); height > 0)
					entry.logo.maxHeight = height;
				break;
			case Field::Ignore:
				break;
			}

			hasContent = hasContent || !value.isEmpty();
		}

		if (skipEmpty && !hasContent)
			continue;

		imported.append(entry);
	}

	QDialog::accept();
}

} // namespace closingtime
