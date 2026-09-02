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

#include <obs.h>

#include <QColor>
#include <QPair>
#include <QVector>

namespace closingtime {

/*
 * A gradient and its stops, with no opinion about what is being filled with it.
 *
 * It lives in a header of its own because two quite different things map a sweep over a box: the
 * glyphs of a TextStyle, and the panel a BackgroundPanel paints behind them. A background is
 * defined before the styles are -- the section carries both -- so leaving this in CreditsModel.hpp
 * would have meant either a circular include or a second copy of the same four fields.
 */

/* One color stop. `position` is 0.0 at the start of the gradient's axis and 1.0 at its end. */
struct GradientStop {
	double position = 0.0;
	QColor color = QColor(255, 255, 255);

	bool operator==(const GradientStop &other) const
	{
		return qFuzzyCompare(position + 1.0, other.position + 1.0) && color == other.color;
	}
	bool operator!=(const GradientStop &other) const { return !(*this == other); }
};

struct GradientSpec {
	/*
	 * Direction of a linear gradient, in degrees clockwise from straight down: 0 runs top
	 * to bottom, 90 left to right, 180 bottom to top. Ignored by a radial gradient, which
	 * has no axis to point.
	 */
	double angle = 0.0;
	QVector<GradientStop> stops = {GradientStop{0.0, QColor(255, 255, 255)},
				       GradientStop{1.0, QColor(140, 140, 140)}};

	/*
	 * The stops as QGradient wants them: sorted, clamped into 0..1, and padded out to at
	 * least two so a gradient left with one stop still paints that color rather than
	 * falling back to black. Everything that draws a gradient goes through this.
	 */
	QVector<QPair<qreal, QColor>> resolvedStops() const;

	bool operator==(const GradientSpec &other) const
	{
		return qFuzzyCompare(angle + 1.0, other.angle + 1.0) && stops == other.stops;
	}
	bool operator!=(const GradientSpec &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

} // namespace closingtime
