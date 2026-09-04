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

#include "ui/CalendarImportDialog.hpp"

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
#include <QRegularExpression>
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

/* How many rows of the file the preview shows. Enough to see the shape of it, not enough to be slow. */
constexpr int kPreviewRows = 40;

} // namespace

int parseScheduleTime(const QString &text)
{
	const QString trimmed = text.trimmed();
	if (trimmed.isEmpty())
		return -1;

	/*
	 * One expression covering every spelling a schedule uses: an hour, an optional colon and
	 * minutes, an optional am/pm. Written as one rather than as a chain of QTime::fromString
	 * attempts because the failure mode of that chain is silent -- a format that nearly matches
	 * consumes the string and returns something wrong.
	 */
	static const QRegularExpression pattern(QStringLiteral("^(\\d{1,2})(?::?(\\d{2}))?\\s*([ap])\\.?m?\\.?$"),
						QRegularExpression::CaseInsensitiveOption);

	const QRegularExpressionMatch meridiem = pattern.match(trimmed);
	if (meridiem.hasMatch()) {
		int hour = meridiem.captured(1).toInt();
		const int minute = meridiem.captured(2).isEmpty() ? 0 : meridiem.captured(2).toInt();
		const bool pm = meridiem.captured(3).compare(QLatin1String("p"), Qt::CaseInsensitive) == 0;

		if (hour == 12)
			hour = 0;
		if (pm)
			hour += 12;

		if (hour > 23 || minute > 59)
			return -1;
		return hour * 60 + minute;
	}

	static const QRegularExpression plain(QStringLiteral("^(\\d{1,2}):(\\d{2})$"));
	const QRegularExpressionMatch clock = plain.match(trimmed);
	if (clock.hasMatch()) {
		const int hour = clock.captured(1).toInt();
		const int minute = clock.captured(2).toInt();
		if (hour > 23 || minute > 59)
			return -1;
		return hour * 60 + minute;
	}

	/* Bare digits: four of them are an hour and minutes, one or two are an hour. */
	static const QRegularExpression bare(QStringLiteral("^(\\d{1,4})$"));
	const QRegularExpressionMatch digits = bare.match(trimmed);
	if (!digits.hasMatch())
		return -1;

	const QString value = digits.captured(1);
	if (value.size() >= 3) {
		const int hour = value.left(value.size() - 2).toInt();
		const int minute = value.right(2).toInt();
		if (hour > 23 || minute > 59)
			return -1;
		return hour * 60 + minute;
	}

	const int hour = value.toInt();
	return hour <= 23 ? hour * 60 : -1;
}

CalendarImportDialog::CalendarImportDialog(const CalendarDocument &document, QWidget *parent)
	: QDialog(parent),
	  document(document)
{
	setWindowTitle(moduleText("CalendarImport.Title"));
	setModal(true);
	resize(1000, 640);

	auto *layout = new QVBoxLayout(this);

	auto *fileRow = new QHBoxLayout();
	pathEdit = new QLineEdit(this);
	pathEdit->setPlaceholderText(moduleText("CalendarImport.File"));
	browseButton = new QPushButton(moduleText("CalendarImport.Browse"), this);
	fileRow->addWidget(pathEdit, 1);
	fileRow->addWidget(browseButton);
	layout->addLayout(fileRow);

	auto *optionRow = new QHBoxLayout();
	delimiterBox = new QComboBox(this);
	delimiterBox->addItem(moduleText("CalendarImport.Delimiter.Auto"), QString());
	delimiterBox->addItem(moduleText("CalendarImport.Delimiter.Comma"), QStringLiteral(","));
	delimiterBox->addItem(moduleText("CalendarImport.Delimiter.Tab"), QStringLiteral("\t"));
	delimiterBox->addItem(moduleText("CalendarImport.Delimiter.Semicolon"), QStringLiteral(";"));
	delimiterBox->addItem(moduleText("CalendarImport.Delimiter.Pipe"), QStringLiteral("|"));

	headerBox = new QCheckBox(moduleText("CalendarImport.HasHeader"), this);
	headerBox->setChecked(true);
	replaceBox = new QCheckBox(moduleText("CalendarImport.Replace"), this);

	optionRow->addWidget(new QLabel(moduleText("CalendarImport.Delimiter"), this));
	optionRow->addWidget(delimiterBox);
	optionRow->addWidget(headerBox);
	optionRow->addWidget(replaceBox);
	optionRow->addStretch(1);
	layout->addLayout(optionRow);

	auto *split = new QHBoxLayout();

	preview = new QTableWidget(this);
	preview->setEditTriggers(QAbstractItemView::NoEditTriggers);
	preview->setSelectionMode(QAbstractItemView::NoSelection);
	preview->horizontalHeader()->setStretchLastSection(true);
	split->addWidget(preview, 3);

	mappingArea = new QScrollArea(this);
	mappingArea->setWidgetResizable(true);
	mappingPanel = new QWidget(mappingArea);
	new QFormLayout(mappingPanel);
	mappingArea->setWidget(mappingPanel);
	split->addWidget(mappingArea, 2);

	layout->addLayout(split, 1);

	statusLabel = new QLabel(this);
	statusLabel->setWordWrap(true);
	layout->addWidget(statusLabel);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);

	connect(browseButton, &QPushButton::clicked, this, &CalendarImportDialog::browse);
	connect(pathEdit, &QLineEdit::editingFinished, this, &CalendarImportDialog::reloadFile);
	connect(delimiterBox, &QComboBox::currentIndexChanged, this, &CalendarImportDialog::reloadFile);
	connect(headerBox, &QCheckBox::toggled, this, [this] {
		refreshPreview();
		updateMappingNames();
	});
	connect(buttons, &QDialogButtonBox::accepted, this, &CalendarImportDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &CalendarImportDialog::reject);
}

void CalendarImportDialog::browse()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("CalendarImport.Browse"), QString(),
							  moduleText("CalendarImport.Filter"));
	if (path.isEmpty())
		return;

	pathEdit->setText(path);
	reloadFile();
}

void CalendarImportDialog::reloadFile()
{
	table.clear();

	const QString path = pathEdit->text().trimmed();
	if (path.isEmpty()) {
		refreshPreview();
		rebuildMappingPanel();
		return;
	}

	QChar delimiter = QLatin1Char(',');
	const QString chosen = delimiterBox->currentData().toString();

	if (chosen.isEmpty()) {
		QFile file(path);
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			delimiter = guessDelimiter(QString::fromUtf8(file.read(64 * 1024)));
			file.close();
		}
	} else {
		delimiter = chosen.at(0);
	}

	QString error;
	if (!parseCsvFile(path, delimiter, &table, &error)) {
		statusLabel->setText(error);
		table.clear();
	}

	refreshPreview();
	rebuildMappingPanel();
	guessMapping();
}

void CalendarImportDialog::refreshPreview()
{
	preview->clear();

	if (table.isEmpty()) {
		preview->setRowCount(0);
		preview->setColumnCount(0);
		return;
	}

	int columns = 0;
	for (const QStringList &row : table)
		columns = std::max(columns, static_cast<int>(row.size()));

	const bool header = headerBox->isChecked();
	const int first = header ? 1 : 0;
	const int rows = std::min(kPreviewRows, static_cast<int>(table.size()) - first);

	preview->setColumnCount(columns);
	preview->setRowCount(std::max(0, rows));

	QStringList headers;
	for (int column = 0; column < columns; ++column)
		headers.append(columnName(column));
	preview->setHorizontalHeaderLabels(headers);

	for (int row = 0; row < rows; ++row) {
		const QStringList &source = table.at(row + first);
		for (int column = 0; column < columns; ++column) {
			const QString value = column < source.size() ? source.at(column) : QString();
			preview->setItem(row, column, new QTableWidgetItem(value));
		}
	}

	statusLabel->setText(moduleText("CalendarImport.Status")
				     .arg(std::max(0, static_cast<int>(table.size()) - first))
				     .arg(columns));
}

void CalendarImportDialog::rebuildMappingPanel()
{
	mappingBoxes.clear();
	mappingLabels.clear();

	delete mappingPanel;
	mappingPanel = new QWidget(mappingArea);
	auto *form = new QFormLayout(mappingPanel);

	int columns = 0;
	for (const QStringList &row : table)
		columns = std::max(columns, static_cast<int>(row.size()));

	/* Every field a column can be mapped onto, in the order the picker lists them. */
	const Field fields[] = {
		Field::Ignore,   Field::Title, Field::Subtitle, Field::Day,   Field::Lane,
		Field::Category, Field::Start, Field::End,      Field::Slot,  Field::EndSlot,
		Field::Location, Field::Tags,  Field::Status,   Field::Notes,
	};

	for (int column = 0; column < columns; ++column) {
		auto *box = new QComboBox(mappingPanel);
		for (Field field : fields)
			box->addItem(fieldLabel(field), static_cast<int>(field));

		auto *label = new QLabel(columnName(column), mappingPanel);
		form->addRow(label, box);

		mappingBoxes.append(box);
		mappingLabels.append(label);
	}

	mappingArea->setWidget(mappingPanel);
}

void CalendarImportDialog::updateMappingNames()
{
	for (int column = 0; column < mappingLabels.size(); ++column)
		mappingLabels[column]->setText(columnName(column));
}

void CalendarImportDialog::guessMapping()
{
	if (!headerBox->isChecked() || table.isEmpty())
		return;

	/*
	 * Matched on what the column is called, loosely. A guess that is wrong is one combo box away
	 * from right, and a guess that is right saves the whole mapping -- so the bar for including a
	 * spelling here is low.
	 */
	const struct {
		const char *needle;
		Field field;
	} hints[] = {
		{"title", Field::Title},       {"event", Field::Title},       {"name", Field::Title},
		{"subtitle", Field::Subtitle}, {"phase", Field::Subtitle},    {"round", Field::Subtitle},
		{"day", Field::Day},           {"date", Field::Day},          {"lane", Field::Lane},
		{"channel", Field::Lane},      {"stream", Field::Lane},       {"stage", Field::Lane},
		{"game", Field::Lane},         {"station", Field::Lane},      {"category", Field::Category},
		{"type", Field::Category},     {"start", Field::Start},       {"time", Field::Start},
		{"end", Field::End},           {"finish", Field::End},        {"wave", Field::Slot},
		{"slot", Field::Slot},         {"location", Field::Location}, {"room", Field::Location},
		{"tag", Field::Tags},          {"status", Field::Status},     {"note", Field::Notes},
	};

	for (int column = 0; column < mappingBoxes.size(); ++column) {
		const QString name = columnName(column).toLower();

		for (const auto &hint : hints) {
			if (!name.contains(QLatin1String(hint.needle)))
				continue;

			const int index = mappingBoxes[column]->findData(static_cast<int>(hint.field));
			if (index >= 0)
				mappingBoxes[column]->setCurrentIndex(index);
			break;
		}
	}
}

QString CalendarImportDialog::fieldLabel(Field field) const
{
	switch (field) {
	case Field::Title:
		return moduleText("CalendarImport.Field.Title");
	case Field::Subtitle:
		return moduleText("CalendarImport.Field.Subtitle");
	case Field::Day:
		return moduleText("CalendarImport.Field.Day");
	case Field::Lane:
		return moduleText("CalendarImport.Field.Lane");
	case Field::Category:
		return moduleText("CalendarImport.Field.Category");
	case Field::Start:
		return moduleText("CalendarImport.Field.Start");
	case Field::End:
		return moduleText("CalendarImport.Field.End");
	case Field::Slot:
		return moduleText("CalendarImport.Field.Slot");
	case Field::EndSlot:
		return moduleText("CalendarImport.Field.EndSlot");
	case Field::Location:
		return moduleText("CalendarImport.Field.Location");
	case Field::Tags:
		return moduleText("CalendarImport.Field.Tags");
	case Field::Status:
		return moduleText("CalendarImport.Field.Status");
	case Field::Notes:
		return moduleText("CalendarImport.Field.Notes");
	case Field::Ignore:
	default:
		return moduleText("CalendarImport.Field.Ignore");
	}
}

QString CalendarImportDialog::columnName(int column) const
{
	if (headerBox->isChecked() && !table.isEmpty()) {
		const QStringList &header = table.first();
		if (column < header.size() && !header.at(column).trimmed().isEmpty())
			return header.at(column).trimmed();
	}

	return moduleText("CalendarImport.Column").arg(column + 1);
}

void CalendarImportDialog::accept()
{
	imported = Result();
	imported.replaceExisting = replaceBox->isChecked();

	const int first = headerBox->isChecked() ? 1 : 0;

	/*
	 * Names are matched against the board first and against what this import has already made
	 * second, so a file naming the same stream on forty rows produces one lane rather than forty.
	 */
	const auto laneFor = [this](const QString &name) {
		if (name.trimmed().isEmpty())
			return QString();

		for (const CalendarLane &lane : document.lanes) {
			if (lane.name.compare(name, Qt::CaseInsensitive) == 0)
				return lane.id;
		}
		for (const CalendarLane &lane : imported.lanes) {
			if (lane.name.compare(name, Qt::CaseInsensitive) == 0)
				return lane.id;
		}

		CalendarLane lane;
		lane.id = QStringLiteral("import_lane%1").arg(imported.lanes.size() + 1);
		lane.name = name.trimmed();
		imported.lanes.append(lane);
		return lane.id;
	};

	const auto dayFor = [this](const QString &name) {
		if (name.trimmed().isEmpty())
			return QString();

		/* A day is matched on its label or on the date it carries, whichever the file gives. */
		const QDate date = QDate::fromString(name.trimmed(), Qt::ISODate);

		for (const CalendarDay &day : document.days) {
			if (day.label.compare(name, Qt::CaseInsensitive) == 0 || (date.isValid() && day.date == date))
				return day.id;
		}
		for (const CalendarDay &day : imported.days) {
			if (day.label.compare(name, Qt::CaseInsensitive) == 0 || (date.isValid() && day.date == date))
				return day.id;
		}

		CalendarDay day;
		day.id = QStringLiteral("import_day%1").arg(imported.days.size() + 1);
		if (date.isValid())
			day.date = date;
		else
			day.label = name.trimmed();
		imported.days.append(day);
		return day.id;
	};

	const auto categoryFor = [this](const QString &name) {
		if (name.trimmed().isEmpty())
			return QString();

		for (const CalendarCategory &category : document.categories) {
			if (category.name.compare(name, Qt::CaseInsensitive) == 0)
				return category.id;
		}
		for (const CalendarCategory &category : imported.categories) {
			if (category.name.compare(name, Qt::CaseInsensitive) == 0)
				return category.id;
		}

		CalendarCategory category;
		category.id = QStringLiteral("import_cat%1").arg(imported.categories.size() + 1);
		category.name = name.trimmed();
		imported.categories.append(category);
		return category.id;
	};

	const auto slotFor = [this](const QString &name) {
		if (name.trimmed().isEmpty())
			return QString();

		for (const CalendarSlot &slot : document.timeSlots) {
			if (slot.name.compare(name, Qt::CaseInsensitive) == 0)
				return slot.id;
		}
		return QString();
	};

	for (int row = first; row < table.size(); ++row) {
		const QStringList &source = table.at(row);

		CalendarEvent event;
		event.id = QStringLiteral("import%1").arg(imported.events.size() + 1);

		for (int column = 0; column < mappingBoxes.size() && column < source.size(); ++column) {
			const auto field = static_cast<Field>(mappingBoxes[column]->currentData().toInt());
			const QString value = source.at(column).trimmed();
			if (field == Field::Ignore || value.isEmpty())
				continue;

			switch (field) {
			case Field::Title:
				event.title = value;
				break;
			case Field::Subtitle:
				event.subtitle = value;
				break;
			case Field::Day:
				event.dayId = dayFor(value);
				break;
			case Field::Lane:
				event.laneId = laneFor(value);
				break;
			case Field::Category:
				event.categoryId = categoryFor(value);
				break;
			case Field::Start:
				event.startMinutes = parseScheduleTime(value);
				break;
			case Field::End:
				event.endMinutes = parseScheduleTime(value);
				break;
			case Field::Slot:
				event.slotId = slotFor(value);
				break;
			case Field::EndSlot:
				event.endSlotId = slotFor(value);
				break;
			case Field::Location:
				event.location = value;
				break;
			case Field::Notes:
				event.notes = value;
				break;
			case Field::Status:
				event.status =
					eventStatusFromId(value.toLower().toUtf8().constData(), EventStatus::Auto);
				break;
			case Field::Tags: {
				/* Several tags in one cell, however the file happens to separate them. */
				const QStringList labels =
					value.split(QRegularExpression(QStringLiteral("[,;/]")), Qt::SkipEmptyParts);
				for (const QString &label : labels) {
					ChannelTag tag;
					tag.label = label.trimmed();
					event.tags.append(tag);
				}
				break;
			}
			default:
				break;
			}
		}

		/* A row with nothing in it is a blank line in the file rather than an event. */
		if (event.title.isEmpty() && event.subtitle.isEmpty() && event.startMinutes < 0 &&
		    event.slotId.isEmpty())
			continue;

		imported.events.append(event);
	}

	QDialog::accept();
}

} // namespace closingtime
