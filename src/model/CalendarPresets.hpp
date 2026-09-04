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
#include <QVector>

#include "model/CalendarModel.hpp"

namespace closingtime {

/*
 * The boards a new calendar can start from.
 *
 * A preset is a set of answers to the questions the layout asks -- which shape, which axis, which
 * way round, how big, in what colors -- and not a layout of its own. That is what keeps the five of
 * them honest: a fix to how a lane header is measured improves all five, because there is only one
 * lane header.
 *
 * The sample schedule is optional and separate for the same reason a template's placeholder text is:
 * a new board wants something on it to edit, and a board being restyled wants its own schedule left
 * alone.
 */
struct CalendarPresetInfo {
	/* Stable id, used by the designer's picker and by nothing that persists. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;
	/* One line on what the board is for. */
	const char *description;
};

const QVector<CalendarPresetInfo> &allCalendarPresets();

/*
 * Applies a preset to `document`.
 *
 * `includeSample` replaces the schedule -- days, lanes, slots, categories and events -- with sample
 * content shaped like the board the preset is for. Without it only the layout, the styling and the
 * furniture are set, so an existing schedule can be re-dressed without being retyped.
 *
 * Returns false for an id nothing knows, leaving the document untouched.
 */
bool applyCalendarPreset(const QString &id, CalendarDocument *document, bool includeSample);

} // namespace closingtime
