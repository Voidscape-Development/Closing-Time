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

#include <QHash>
#include <QRectF>
#include <QString>

#include <functional>
#include <memory>

#include "model/CreditsModel.hpp"

class QPainter;
class QSvgRenderer;

namespace closingtime {

/*
 * Parsed SVG tiles, keyed by whatever the caller calls them.
 *
 * Parsing a few hundred bytes of markup is cheap, so unlike LogoCache this exists to avoid
 * re-parsing once per row or per piece rather than once per render: a cache is built for the
 * length of a measure or a render and let go afterwards, which also means a custom file edited
 * on disk is picked up by the next rebuild without anything having to notice.
 *
 * Both the bridge's tiles and the divider's parts come through here, so a leader and a rule are
 * loaded, cached and failed over identically rather than by two implementations that agree
 * today.
 */
class SvgArtCache {
public:
	SvgArtCache();
	~SvgArtCache();

	SvgArtCache(const SvgArtCache &) = delete;
	SvgArtCache &operator=(const SvgArtCache &) = delete;

	/*
	 * A built-in shape's tile, parsed from `markup` and cached under `id`. Null when the markup
	 * is empty or will not parse.
	 */
	QSvgRenderer *builtIn(const char *id, const QString &markup);

	/*
	 * Artwork from a file. Null when the path is empty or the file will not parse -- in which
	 * case the art simply is not drawn, the same way a missing logo leaves a placeholder rather
	 * than failing the strip.
	 */
	QSvgRenderer *fromFile(const QString &path);

private:
	QSvgRenderer *load(const QString &key, const QString &path, const QString &markup);

	QHash<QString, std::shared_ptr<QSvgRenderer>> cache;
};

/*
 * The artwork's width as a multiple of its height, taken from its own viewBox. Falls back to
 * `fallback` when the file declares neither a viewBox nor a default size.
 */
qreal svgArtAspect(QSvgRenderer *renderer, qreal fallback);

/*
 * Paints a silhouette through a text style's ink: its fill or gradient, its outline and its
 * shadow, exactly as the glyphs beside it are painted.
 *
 * `drawSilhouette` is called with a painter set up to receive the art in the same coordinates
 * the caller is working in; everything it draws is treated as one shape. Handing the whole of a
 * divider or a whole run of bridge tiles over in one call rather than piece by piece is what
 * keeps an outline from being painted over the neighboring piece's fill, and what keeps one
 * gradient running across the figure instead of restarting at every part of it.
 *
 * `bounds` is the region the art can reach, in the same coordinates; anything drawn outside it
 * is clipped by the buffer. `fillBox` is the block a gradient is mapped over, which callers set
 * to the whole figure rather than to the few pixels of a hairline: a sweep crammed into a run
 * of dots would show none of itself while the words beside it showed all of it.
 */
void paintInkedArt(QPainter *painter, const QRectF &bounds, const TextStyle &style, const QRectF &fillBox,
		   const std::function<void(QPainter *)> &drawSilhouette);

} // namespace closingtime
