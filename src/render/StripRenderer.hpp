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
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QVector>

#include "model/CreditsModel.hpp"

namespace closingtime {

/*
 * The brush a style's glyphs are filled with over `box`, which for a gradient is the block of
 * text the sweep is mapped across. Exposed so the designer's gradient preview is painted by
 * the same code that paints the roll, rather than by a second implementation of the mapping
 * that can drift away from it.
 */
QBrush textFillBrush(const TextStyle &style, const QRectF &box);

/*
 * Styles name a font by family, so a scene collection carried to another machine can end up
 * rendering in whatever Qt substitutes. These two report that rather than let it pass
 * silently: the designer surfaces the list under the preview, and the source logs it once.
 */

/*
 * True when `family` will actually be used rather than substituted. The generic families
 * Qt resolves against the platform default are always considered available, since there is
 * nothing for the user to install.
 */
bool fontFamilyAvailable(const QString &family);

/*
 * Families a document's visible text asks for that this machine cannot supply, deduplicated
 * and sorted. Preset bindings are resolved first, so only fonts that really get drawn count.
 */
QStringList missingFontFamilies(const Document &document);

/*
 * Decoded logo images keyed by "path|maxHeight".
 *
 * Deliberately not shared: the source owns one and each designer window owns another. They
 * are only ever touched from a render job, and render jobs run one at a time, so a cache
 * has a single user without needing a lock to say so.
 */
class LogoCache {
public:
	/*
	 * Returns the logo scaled so its height is at most `maxHeight`. Returns a null image
	 * when the path is empty or cannot be decoded; callers lay out a placeholder box in
	 * that case rather than failing the whole strip.
	 */
	QImage get(const QString &path, int maxHeight);

	void clear();

	/* Drops entries whose backing file changed on disk since it was decoded. */
	void invalidate(const QString &path);

private:
	struct CacheEntry {
		QImage image;
		qint64 fileSize = 0;
		qint64 modifiedMs = 0;
	};

	QHash<QString, CacheEntry> cache;
};

/*
 * One horizontal slice of the rendered strip. The strip is cut into tiles because a long
 * credit roll easily exceeds the maximum texture height a GPU will accept (commonly
 * 16384 px), and because uploading a 30 000 px tall texture in one piece wastes VRAM when
 * only a screenful is ever on display.
 */
struct StripTile {
	/* Y offset of this tile's top edge within the full strip, in pixels. */
	int top = 0;
	QImage image;
};

struct Strip {
	QVector<StripTile> tiles;
	int width = 0;
	int height = 0;

	bool isEmpty() const { return tiles.isEmpty() || height <= 0; }
};

/*
 * One rectangle the layout put something in, in strip space.
 *
 * Nothing in the roll is drawn from these -- they are what the designer's layout overlay draws
 * on top of the preview, so that a section landing somewhere unexpected can be read off the
 * screen instead of guessed at from the settings that produced it. A section box that turns out
 * to be half the canvas, or a text column that turns out to be the width of its own words rather
 * than the width of the section, says immediately which setting is doing it.
 *
 * They are collected during the measure pass, which every section goes through exactly once, so
 * the boxes cannot disagree with what was painted and a section straddling a tile seam is
 * reported once rather than once per tile.
 */
struct LayoutBox {
	enum class Kind {
		/* The section's share of the canvas width, over the full height it occupies. */
		Section,
		/* What is left of that box once the side margins and the padding are taken off. */
		Content,
		/* The column a run of text was laid out into -- not the ink, which may be narrower. */
		Text,
		Logo,
		Bridge,
		/*
		 * A Section Divider's own box, over the height its artwork occupies -- which is not
		 * the same as its content box, since a divider narrower than its section sits
		 * centred in it and a stack of rules is taller than any one of them.
		 */
		Divider,
	};

	Kind kind = Kind::Section;
	/* Index into Document::sections, so the overlay can pick the selected section out. */
	int section = -1;
	QRectF rect;
};

using LayoutBoxes = QVector<LayoutBox>;

/*
 * Lays a document out into a strip of tiles.
 *
 * Painting is into a QImage, which Qt supports off the GUI thread, so this runs on the
 * shared render thread (see RenderThread.hpp) rather than anywhere OBS needs to stay
 * responsive. The credits source hands the finished tiles to the graphics thread for
 * upload; the designer hands them back to the UI thread for the preview.
 */
class StripRenderer {
public:
	explicit StripRenderer(LogoCache *logos) : logos(logos) {}

	/* Maximum tile height in pixels. Tiles are also capped to the strip's own height. */
	static constexpr int kTileHeight = 2048;

	/*
	 * Rasterises the document. When `boxes` is given it is filled with the rectangles the
	 * layout placed things in, for the designer's overlay; the source passes nothing and
	 * pays for none of it.
	 */
	Strip render(const Document &document, LayoutBoxes *boxes = nullptr) const;

	/*
	 * Total content height in pixels, excluding lead-in and lead-out. Cheaper than a full
	 * render because nothing is rasterised; used by the designer to show roll duration
	 * while the user is still typing.
	 */
	int measure(const Document &document) const;

private:
	LogoCache *logos;
};

} // namespace closingtime
