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

#include "model/Gradient.hpp"

#include <obs.hpp>

#include <algorithm>

namespace closingtime {

QVector<QPair<qreal, QColor>> GradientSpec::resolvedStops() const
{
	QVector<QPair<qreal, QColor>> resolved;
	resolved.reserve(std::max<qsizetype>(2, stops.size()));

	for (const GradientStop &stop : stops)
		resolved.append({std::clamp(stop.position, 0.0, 1.0), stop.color});

	std::stable_sort(resolved.begin(), resolved.end(),
			 [](const QPair<qreal, QColor> &a, const QPair<qreal, QColor> &b) {
				 return a.first < b.first;
			 });

	/*
	 * QGradient paints black wherever it has no stop to interpolate from, so a spec that
	 * has been edited down to one stop -- or none -- is padded out rather than allowed to
	 * blank the text out from under the user mid-edit.
	 */
	if (resolved.isEmpty())
		resolved.append({0.0, QColor(255, 255, 255)});
	if (resolved.size() == 1)
		resolved.append({1.0, resolved.first().second});

	return resolved;
}

void GradientSpec::save(obs_data_t *data) const
{
	obs_data_set_double(data, "angle", angle);

	OBSDataArrayAutoRelease array = obs_data_array_create();
	for (const GradientStop &stop : stops) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_double(item, "position", stop.position);
		obs_data_set_int(item, "color", static_cast<long long>(stop.color.rgba()));
		obs_data_array_push_back(array, item);
	}
	obs_data_set_array(data, "stops", array);
}

void GradientSpec::load(obs_data_t *data)
{
	angle = obs_data_get_double(data, "angle");

	OBSDataArrayAutoRelease array = obs_data_get_array(data, "stops");
	if (!array)
		return;

	stops.clear();
	const size_t count = obs_data_array_count(array);
	stops.reserve(static_cast<int>(count));
	for (size_t i = 0; i < count; ++i) {
		OBSDataAutoRelease item = obs_data_array_item(array, i);
		GradientStop stop;
		stop.position = std::clamp(obs_data_get_double(item, "position"), 0.0, 1.0);
		stop.color = QColor::fromRgba(static_cast<QRgb>(obs_data_get_int(item, "color")));
		stops.append(stop);
	}
}

} // namespace closingtime
