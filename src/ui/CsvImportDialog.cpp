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
#include <QGroupBox>
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

	auto *hint = new QLabel(moduleText("Import.MappingHint"), this);
	hint->setWordWrap(true);
	outer->addWidget(hint);

	/*
	 * The mapping sits beside the preview rather than above it: a column of "name -> field"
	 * rows scrolls down as far as the file is wide, so a spreadsheet with a dozen columns
	 * needs no sideways scrolling to reach the last one, and each row is labelled with the
	 * column's own name from the file rather than a position nobody can match up by eye.
	 */
	auto *content = new QHBoxLayout();

	auto *mappingGroup = new QGroupBox(moduleText("Import.Mapping"), this);
	/* Wide enough for a field name and its combo, capped so one long header cannot take over. */
	mappingGroup->setMinimumWidth(200);
	mappingGroup->setMaximumWidth(320);
	auto *mappingGroupLayout = new QVBoxLayout(mappingGroup);

	mappingEmpty = new QLabel(moduleText("Import.NoColumns"), mappingGroup);
	mappingEmpty->setWordWrap(true);
	mappingGroupLayout->addWidget(mappingEmpty, 0, Qt::AlignTop);

	mappingScroll = new QScrollArea(mappingGroup);
	mappingScroll->setWidgetResizable(true);
	mappingScroll->setFrameShape(QFrame::NoFrame);
	mappingScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mappingPanel = new QWidget(mappingScroll);
	mappingLayout = new QFormLayout(mappingPanel);
	mappingLayout->setContentsMargins(0, 0, 0, 0);
	/*
	 * Each name sits on its own line above its combo. The panel is narrow by design, and a
	 * header like "Role on production" is common enough that squeezing it into a label column
	 * beside the combo would clip it.
	 */
	mappingLayout->setRowWrapPolicy(QFormLayout::WrapAllRows);
	mappingLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	mappingScroll->setWidget(mappingPanel);
	mappingGroupLayout->addWidget(mappingScroll, 1);

	content->addWidget(mappingGroup);

	preview = new QTableWidget(this);
	preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
	preview->verticalHeader()->setVisible(false);
	content->addWidget(preview, 1);

	outer->addLayout(content, 1);

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

	/* Starts with no file, so the panel starts on its "nothing to map yet" state. */
	rebuildMappingPanel();
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

	rebuildMappingPanel();
	refreshPreview();
}

void CsvImportDialog::rebuildMappingPanel()
{
	/* removeRow() deletes the label and the combo it holds, so the vectors go stale with it. */
	mapping.clear();
	mappingNames.clear();
	while (mappingLayout->rowCount() > 0)
		mappingLayout->removeRow(0);

	int columnCount = 0;
	for (const QStringList &row : table)
		columnCount = std::max(columnCount, static_cast<int>(row.size()));

	const QVector<Field> fields = availableFields();

	for (int column = 0; column < columnCount; ++column) {
		auto *name = new QLabel(mappingPanel);
		name->setWordWrap(true);

		auto *box = new QComboBox(mappingPanel);
		for (Field field : fields)
			box->addItem(fieldLabel(field), static_cast<int>(field));

		/*
		 * Default mapping walks the real fields in order and then ignores the rest, which
		 * matches how people actually lay these spreadsheets out.
		 */
		box->setCurrentIndex(column + 1 < fields.size() ? column + 1 : 0);

		mappingLayout->addRow(name, box);
		mappingNames.append(name);
		mapping.append(box);

		connect(box, &QComboBox::currentIndexChanged, this, &CsvImportDialog::refreshPreview);
	}

	/* Nothing to map until a file is loaded, and an empty panel does not say why. */
	mappingEmpty->setVisible(columnCount == 0);
	mappingScroll->setVisible(columnCount > 0);

	updateMappingNames();
}

void CsvImportDialog::updateMappingNames()
{
	for (int column = 0; column < mappingNames.size(); ++column) {
		const QString name = columnName(column);
		mappingNames.at(column)->setText(name);
		/* The panel is narrow, so a name the label has to wrap is worth spelling out. */
		mappingNames.at(column)->setToolTip(name);
	}
}

QString CsvImportDialog::columnName(int column) const
{
	if (headerBox->isChecked() && !table.isEmpty() && column < table.first().size()) {
		const QString header = table.first().at(column).trimmed();
		if (!header.isEmpty())
			return header;
	}

	return QStringLiteral("%1 %2").arg(moduleText("Import.Column")).arg(column + 1);
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
	for (int column = 0; column < columnCount; ++column)
		headers.append(columnName(column));
	preview->setHorizontalHeaderLabels(headers);

	/* The header row moving in or out of the data renames every column of the panel too. */
	updateMappingNames();

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
