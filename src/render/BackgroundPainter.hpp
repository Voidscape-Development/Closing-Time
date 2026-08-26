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

#include <QBrush>
#include <QPainterPath>
#include <QRectF>

#include "model/Background.hpp"

class QPainter;

namespace closingtime {

class LogoCache;

/*
 * A gradient mapped over `box`.
 *
 * One mapping, used by everything that sweeps a gradient over something: the glyphs of a run of
 * text and the panel drawn behind them alike. `textFillBrush` is a thin wrapper over this, which is
 * what keeps a panel's sweep and a heading's sweep from being two implementations that agree today
 * and drift tomorrow -- and is what lets the designer paint a swatch of either from the same call.
 *
 * A linear gradient runs along `spec.angle`, clockwise from straight down, spanning exactly the
 * box's own extent along that axis. A radial one runs from the centre out to half the diagonal, so
 * the last stop lands on the corners rather than inside them.
 */
QBrush gradientBrush(const GradientSpec &spec, bool radial, const QRectF &box);

/*
 * The panel's outline as a path, corners and all.
 *
 * Exposed so a caller that needs the shape without the paint can have it -- the designer's swatch,
 * and any future clip -- and so the fill and the border are provably the same figure rather than a
 * rectangle and a rounded rectangle that happen to line up. A radius larger than the rectangle can
 * hold is scaled down here, all four together so the shape stays in proportion, which is what lets
 * one preset sit behind sections of different heights without being re-typed for each.
 */
QPainterPath backgroundPath(const BackgroundPanel &panel, const QRectF &rect);

/*
 * Paints `panel` behind `box`.
 *
 * `box` is the content's own rectangle; the outsets are applied here, so every caller hands over
 * the box it was already going to draw in and nothing has to remember to grow it first. Painting
 * happens strictly inside the panel's own path: an image is clipped to it, and the border is
 * stroked inside it, so the outset rectangle really is the outermost thing the panel touches.
 *
 * `images` may be null, in which case an image fill draws nothing and the rest of the panel is
 * drawn as usual -- the same bargain a missing logo strikes, and what a measure pass wants.
 *
 * Does nothing at all when `painter` is null, which is what makes the call safe to leave in the
 * shared measure-and-draw path that lays every section out.
 */
void paintBackgroundPanel(QPainter *painter, const BackgroundPanel &panel, const QRectF &box, LogoCache *images);

} // namespace closingtime
