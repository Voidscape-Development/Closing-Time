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

#include <QString>
#include <QStringList>

namespace closingtime {

/*
 * Natural order, the way a person sorts names: runs of digits compare by their value rather than
 * character by character, so "Scene 2" comes before "Scene 10", and letters compare without regard
 * to case. Two names that differ only in case, or in leading zeros, still come out in a fixed order
 * rather than as equals, so a sort is the same every time.
 *
 * Written out rather than left to QCollator: its numeric mode depends on which collation backend Qt
 * was built with, and on a build without ICU it is silently ignored -- which would put "Scene 10"
 * before "Scene 2" on some machines and not others.
 */
bool naturalLess(const QString &a, const QString &b);

/* Sorts `names` into natural order, in place. */
void naturalSort(QStringList &names);

} // namespace closingtime
