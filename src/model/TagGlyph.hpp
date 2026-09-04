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

namespace closingtime {

/*
 * The mark drawn beside a channel tag or a chip.
 *
 * The same bargain the bridge tiles strike: a small table of SVG drawn in a box one unit tall and
 * painted white, so the renderer can use it as a stencil for whatever ink the text beside it is
 * set in. A tag therefore picks up the color of its own line rather than carrying one, and adding a
 * platform is a row here rather than a change to the renderer.
 *
 * `Custom` takes its art from a file, which is what covers every platform, sponsor and in-house
 * mark the table does not have. The shapes here are deliberately generic silhouettes of the shape
 * of each platform's mark rather than reproductions of anyone's logo artwork: what they are for is
 * telling one row of a schedule from another at a glance.
 *
 * Persisted by string id (tagGlyphId), never by ordinal.
 */
enum class TagGlyph { None, Twitch, YouTube, Kick, Website, Location, Custom };

const char *tagGlyphId(TagGlyph glyph);
TagGlyph tagGlyphFromId(const char *id, TagGlyph fallback = TagGlyph::None);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *tagGlyphName(TagGlyph glyph);

/* Every glyph in the order the designer's picker should list them. */
const QVector<TagGlyph> &allTagGlyphs();

/*
 * The complete SVG document for a glyph's mark, or an empty string for None and Custom.
 *
 * Wrapped here rather than in the table so the viewBox is derived from the aspect rather than
 * written out twice and left to drift, exactly as `bridgeTypeSvg` does it.
 */
QString tagGlyphSvg(TagGlyph glyph);

/* The mark's width as a multiple of its height. */
double tagGlyphAspect(TagGlyph glyph);

} // namespace closingtime
