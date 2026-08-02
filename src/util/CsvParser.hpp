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

#include <QChar>
#include <QString>
#include <QStringList>
#include <QVector>

namespace closingtime {

using CsvTable = QVector<QStringList>;

/*
 * RFC 4180 style parser with a configurable delimiter. Handles quoted fields, doubled
 * quotes inside them, and embedded newlines; accepts LF, CRLF and lone CR line endings.
 *
 * Rows are returned exactly as they appear -- no padding to a common column count, since
 * ragged input is usually a sign the user picked the wrong delimiter and the import dialog
 * shows the raggedness rather than hiding it.
 */
CsvTable parseCsv(const QString &text, QChar delimiter = QLatin1Char(','));

/*
 * Reads a file and parses it. Text is decoded as UTF-8, with a leading byte-order mark
 * stripped if present -- spreadsheet exports on Windows frequently include one.
 * Returns false and fills `error` when the file cannot be read.
 */
bool parseCsvFile(const QString &path, QChar delimiter, CsvTable *table, QString *error);

/* Guesses the delimiter by scoring candidates on how consistent a column count they give. */
QChar guessDelimiter(const QString &text);

} // namespace closingtime
