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

#pragma once

#include <QDialog>
#include <QVector>

#include "model/CreditsModel.hpp"
#include "util/CsvParser.hpp"

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace closingtime {

/*
 * Picks a delimited file, previews the parsed rows, and maps each column onto a field of
 * the target section type. The set of mappable fields is derived from the section type, so
 * a Bridged section offers a left and a right column while a logo list offers a path and a
 * height.
 */
class CsvImportDialog : public QDialog {
	Q_OBJECT

public:
	explicit CsvImportDialog(SectionType targetType, QWidget *parent = nullptr);

	/* Valid once the dialog has been accepted. */
	QVector<Entry> entries() const { return imported; }
	bool replaceExisting() const;

private:
	/* A field of Entry that a CSV column can be mapped onto. */
	enum class Field { Ignore, Text, SecondaryText, LogoPath, LogoHeight };

	void browse();
	void reloadFile();
	void refreshPreview();
	void rebuildMappingRow();
	void accept() override;

	QVector<Field> availableFields() const;
	QString fieldLabel(Field field) const;

	SectionType targetType;

	QLineEdit *pathEdit = nullptr;
	QPushButton *browseButton = nullptr;
	QComboBox *delimiterBox = nullptr;
	QCheckBox *headerBox = nullptr;
	QCheckBox *skipEmptyBox = nullptr;
	QCheckBox *replaceBox = nullptr;
	QLabel *statusLabel = nullptr;
	QTableWidget *preview = nullptr;
	/* One combo per detected column, laid out above the preview table. */
	QVector<QComboBox *> mapping;
	QWidget *mappingRow = nullptr;
	QHBoxLayout *mappingLayout = nullptr;

	CsvTable table;
	QVector<Entry> imported;
};

} // namespace closingtime
