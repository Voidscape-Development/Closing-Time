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

#include "util/NaturalOrder.hpp"

#include <algorithm>

namespace closingtime {

namespace {

/* -1, 0 or 1, comparing the two names naturally and treating case and leading zeros as equal. */
int naturalCompare(const QString &a, const QString &b)
{
	qsizetype i = 0;
	qsizetype j = 0;

	while (i < a.size() && j < b.size()) {
		const QChar ca = a.at(i);
		const QChar cb = b.at(j);

		if (ca.isDigit() && cb.isDigit()) {
			/* A run of digits each: skip leading zeros, then the longer run is the larger number. */
			qsizetype startA = i;
			qsizetype startB = j;
			while (startA < a.size() && a.at(startA) == QLatin1Char('0'))
				++startA;
			while (startB < b.size() && b.at(startB) == QLatin1Char('0'))
				++startB;

			qsizetype endA = startA;
			qsizetype endB = startB;
			while (endA < a.size() && a.at(endA).isDigit())
				++endA;
			while (endB < b.size() && b.at(endB).isDigit())
				++endB;

			const qsizetype lengthA = endA - startA;
			const qsizetype lengthB = endB - startB;
			if (lengthA != lengthB)
				return lengthA < lengthB ? -1 : 1;

			/* Same length: digit by digit, which is now the same as by value. */
			for (qsizetype k = 0; k < lengthA; ++k) {
				const QChar da = a.at(startA + k);
				const QChar db = b.at(startB + k);
				if (da != db)
					return da < db ? -1 : 1;
			}

			/* Advance past the zeros as well, wherever the runs began. */
			while (i < a.size() && a.at(i).isDigit())
				++i;
			while (j < b.size() && b.at(j).isDigit())
				++j;
			continue;
		}

		const QChar la = ca.toCaseFolded();
		const QChar lb = cb.toCaseFolded();
		if (la != lb)
			return la < lb ? -1 : 1;

		++i;
		++j;
	}

	/* One is a prefix of the other: the shorter comes first. */
	const bool aDone = i >= a.size();
	const bool bDone = j >= b.size();
	if (aDone && bDone)
		return 0;
	return aDone ? -1 : 1;
}

} // namespace

bool naturalLess(const QString &a, const QString &b)
{
	const int natural = naturalCompare(a, b);
	if (natural != 0)
		return natural < 0;

	/* Equal but for case or leading zeros: fall back on the exact text, so the order is total. */
	return a < b;
}

void naturalSort(QStringList &names)
{
	std::sort(names.begin(), names.end(), naturalLess);
}

} // namespace closingtime
