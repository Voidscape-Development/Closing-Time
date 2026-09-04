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

#include "model/CalendarModel.hpp"
#include "util/CsvParser.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTableWidget;

namespace closingtime {

/*
 * Picks a delimited file, previews the parsed rows, and maps each column onto a field of an event.
 *
 * A schedule arrives as a spreadsheet far more often than it arrives typed, and the columns are
 * never in the same order twice -- so the mapping is per column and per import rather than a fixed
 * format the file has to be rewritten into.
 *
 * Days, lanes and categories are matched **by name** and created when the file mentions one the
 * board does not have. That is what makes a single paste of a published schedule produce a board
 * rather than a list of events with nowhere to sit: the columns that say which stream and which day
 * are the same columns that build the lanes and the days.
 */
class CalendarImportDialog : public QDialog {
	Q_OBJECT

public:
	/*
	 * `document` is read to offer its existing days, lanes and categories for matching. It is
	 * never written: what the import produces is handed back through `result` for the designer to
	 * fold in under an undo step of its own.
	 */
	explicit CalendarImportDialog(const CalendarDocument &document, QWidget *parent = nullptr);

	/* What the import produced. Valid once the dialog has been accepted. */
	struct Result {
		QVector<CalendarEvent> events;
		/* Days, lanes and categories the file named that the board did not already have. */
		QVector<CalendarDay> days;
		QVector<CalendarLane> lanes;
		QVector<CalendarCategory> categories;
		bool replaceExisting = false;
	};

	Result result() const { return imported; }

private:
	/* A field of an event that a column can be mapped onto. */
	enum class Field {
		Ignore,
		Title,
		Subtitle,
		Day,
		Lane,
		Category,
		Start,
		End,
		Slot,
		EndSlot,
		Location,
		Tags,
		Status,
		Notes,
	};

	void browse();
	void reloadFile();
	void refreshPreview();
	void rebuildMappingPanel();
	void updateMappingNames();
	/* Guesses a field for each column from its header, which is right often enough to be worth it. */
	void guessMapping();
	void accept() override;

	QString fieldLabel(Field field) const;
	QString columnName(int column) const;

	const CalendarDocument &document;

	QLineEdit *pathEdit = nullptr;
	QPushButton *browseButton = nullptr;
	QComboBox *delimiterBox = nullptr;
	QCheckBox *headerBox = nullptr;
	QCheckBox *replaceBox = nullptr;
	QLabel *statusLabel = nullptr;
	QTableWidget *preview = nullptr;
	QScrollArea *mappingArea = nullptr;
	QWidget *mappingPanel = nullptr;

	QVector<QComboBox *> mappingBoxes;
	QVector<QLabel *> mappingLabels;

	CsvTable table;
	Result imported;
};

/*
 * Reads a time written the way a schedule writes one: "13:00", "1:00 PM", "1pm", "1300", "13".
 * Returns minutes from midnight, or -1 when the text is not a time at all.
 *
 * Exposed because the event table's own time cells accept the same spellings, and a board where the
 * import understood "7pm" but the table did not would be the kind of inconsistency nobody can guess
 * their way out of.
 */
int parseScheduleTime(const QString &text);

} // namespace closingtime
