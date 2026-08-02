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

#include "util/CsvParser.hpp"

#include <QFile>
#include <QHash>

namespace closingtime {

CsvTable parseCsv(const QString &text, QChar delimiter)
{
	CsvTable rows;
	QStringList row;
	QString field;

	bool inQuotes = false;
	bool rowStarted = false;

	const int length = text.length();
	for (int i = 0; i < length; ++i) {
		const QChar ch = text.at(i);

		if (inQuotes) {
			if (ch != QLatin1Char('"')) {
				field.append(ch);
				continue;
			}

			/* A doubled quote inside a quoted field is a literal quote. */
			if (i + 1 < length && text.at(i + 1) == QLatin1Char('"')) {
				field.append(QLatin1Char('"'));
				++i;
			} else {
				inQuotes = false;
			}
			continue;
		}

		if (ch == QLatin1Char('"') && field.isEmpty()) {
			inQuotes = true;
			rowStarted = true;
			continue;
		}

		if (ch == delimiter) {
			row.append(field);
			field.clear();
			rowStarted = true;
			continue;
		}

		if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) {
			/* Swallow the LF of a CRLF pair so it does not open an empty row. */
			if (ch == QLatin1Char('\r') && i + 1 < length && text.at(i + 1) == QLatin1Char('\n'))
				++i;

			if (rowStarted || !field.isEmpty()) {
				row.append(field);
				rows.append(row);
			}

			field.clear();
			row.clear();
			rowStarted = false;
			continue;
		}

		field.append(ch);
		rowStarted = true;
	}

	if (rowStarted || !field.isEmpty()) {
		row.append(field);
		rows.append(row);
	}

	return rows;
}

bool parseCsvFile(const QString &path, QChar delimiter, CsvTable *table, QString *error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		if (error)
			*error = file.errorString();
		return false;
	}

	QString text = QString::fromUtf8(file.readAll());
	if (!text.isEmpty() && text.at(0) == QChar(0xFEFF))
		text.remove(0, 1);

	*table = parseCsv(text, delimiter);
	return true;
}

QChar guessDelimiter(const QString &text)
{
	static const QChar candidates[] = {QLatin1Char(','), QLatin1Char('\t'), QLatin1Char(';'), QLatin1Char('|')};

	/* Only the head of the file is sampled; that is plenty to tell the separators apart. */
	const QString sample = text.left(64 * 1024);

	QChar best = QLatin1Char(',');
	int bestScore = -1;

	for (const QChar candidate : candidates) {
		const CsvTable rows = parseCsv(sample, candidate);
		if (rows.isEmpty())
			continue;

		/*
		 * A delimiter is judged by how many rows agree on the most common column count,
		 * weighted by that count, so a separator that never actually splits anything
		 * loses to one that produces a consistent table.
		 */
		QHash<int, int> counts;
		for (const QStringList &row : rows)
			++counts[row.size()];

		int columns = 1;
		int agreement = 0;
		for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
			if (it.value() > agreement || (it.value() == agreement && it.key() > columns)) {
				agreement = it.value();
				columns = it.key();
			}
		}

		const int score = columns > 1 ? agreement * columns : 0;
		if (score > bestScore) {
			bestScore = score;
			best = candidate;
		}
	}

	return best;
}

} // namespace closingtime
