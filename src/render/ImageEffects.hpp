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

#include <QColor>
#include <QImage>

namespace closingtime {

/*
 * Raster helpers shared by everything that casts a shadow. Text builds its shadow from a glyph
 * path and bridge art from a rasterised tile, but both soften it the same way, so the passes
 * live here rather than once per caller.
 */

/*
 * Softens `image` in place with one box pass along a single axis, which is a handful of adds
 * per pixel; three passes each way is close enough to a Gaussian for a shadow edge. The image
 * must be premultiplied: that is what makes blurring the four channels together correct rather
 * than bleeding colour out of the edges.
 */
void boxBlur(QImage &image, int radius, bool vertical);

/* The three passes each way that a shadow is softened with. */
void blurImage(QImage &image, int radius);

/*
 * `image` recoloured to `colour`, keeping its alpha. Used to turn a rasterised silhouette into
 * shadow or outline ink without re-rendering the art that produced it.
 */
QImage tintedImage(const QImage &image, const QColor &colour);

} // namespace closingtime
