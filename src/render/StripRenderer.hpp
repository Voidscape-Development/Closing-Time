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
#include "render/AnimatedLogo.hpp"

namespace closingtime {

/*
 * The brush a style's glyphs are filled with over `box`, which for a gradient is the block of
 * text the sweep is mapped across. Exposed so the designer's gradient preview is painted by
 * the same code that paints the roll, rather than by a second implementation of the mapping
 * that can drift away from it.
 */
QBrush textFillBrush(const TextStyle &style, const QRectF &box);

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

/*
 * An animated logo the layout placed, and everything needed to draw it.
 *
 * The strip is rasterised once and scrolled; an animation is by definition not that. So an
 * animated logo is not painted into the strip at all. The tile it would have occupied is left
 * empty and the artwork is drawn over the strip afterwards, from its own texture -- by the source
 * on the graphics thread, or by the designer's preview widget on the UI thread. This is what
 * carries it across: where the hole is, what goes in it, and how the frames are timed.
 *
 * The layout is unchanged by any of it. An animated logo occupies exactly the box its first frame
 * would have occupied as a still, so nothing above or below it moves when a PNG is swapped for a
 * GIF of the same artwork.
 */
struct AnimatedLogoPlacement {
	/* The hole, in strip space. Consumers add the scroll offset and draw here. */
	QRectF rect;
	LogoAnimationPtr animation;
	LogoPlayback playback;
	/*
	 * The drop shadow, one image per frame, when the logo asked for a shadow that follows the
	 * animation. Empty in the ordinary case, where the strip has already baked the first
	 * frame's shadow into the tile behind the hole and there is nothing per-frame to draw.
	 *
	 * Blurring every frame is done here, once per rebuild on the render thread, rather than per
	 * frame on the compositor's: a shadow is a blur, and a blur is the most expensive thing in
	 * this renderer.
	 */
	QVector<QImage> shadowFrames;
	/* Where a shadow frame's top-left goes, relative to `rect`'s. */
	QPointF shadowOffset;
	/* Index into Document::sections, for the designer's overlay and its highlighting. */
	int section = -1;
};

/*
 * A sticky block the layout placed, and everything needed to draw it.
 *
 * The strip is one tall picture that scrolls; a block that stops scrolling while the rest of it
 * carries on is by definition not part of that picture. So a sticky block is not painted into the
 * strip at all -- the same bargain an animated logo strikes, and for the same reason. The slot it
 * occupies is left empty and the block is carried here as a picture of its own, drawn over the
 * strip by whoever is compositing: the source on the graphics thread, the designer's preview on
 * the UI thread.
 *
 * The layout does not move for any of it. The block occupies exactly the slot its content would
 * have occupied inline, so everything above and below it sits where it always did, and the empty
 * slot goes on scrolling after the block has detached from it.
 */
struct StickyBlockPlacement {
	/* The slot, in strip space. Consumers add the scroll offset to find where it would be. */
	QRectF rect;
	/* The block itself, rasterised at the strip's width, exactly `rect` in size. */
	QImage image;

	/* Everything about how it pins, copied off the section so the compositor reads no model. */
	StickyAnchor anchor = StickyAnchor::Center;
	double canvasPosition = 0.5;
	double offset = 0.0;
	double hold = 5.0;
	bool holdForever = false;
	StickyRelease release = StickyRelease::EndAtHold;

	/*
	 * How far past the slot the picture reaches at top and bottom, in pixels: enough for the
	 * shadows a child can cast outside its own box, and for the backdrop's padding when the block
	 * carries one. The picture is `rect.height() + margin * 2` tall and its top edge belongs
	 * `margin` pixels above wherever the block is drawn.
	 *
	 * The backdrop itself is painted into the picture rather than carried here as a colour to be
	 * drawn behind it. A flat quad would mean a second effect -- libobs draws solids and textures
	 * from different ones -- started inside the pass that is drawing the strip, and the panel is
	 * a rectangle the rasteriser can fill for nothing at rebuild time.
	 */
	int margin = 0;

	/* Index into Document::sections, for the designer's overlay and its highlighting. */
	int section = -1;

	/*
	 * Where the block's top edge sits in canvas space once it has pinned, for a block of this
	 * height on a canvas this tall. The one piece of arithmetic both the source and the preview
	 * need, kept here so they cannot disagree about where "pinned" is.
	 */
	double pinnedTop(int canvasHeight) const
	{
		return canvasPosition * canvasHeight + offset - stickyAnchorFraction(anchor) * rect.height();
	}
};

struct Strip {
	QVector<StripTile> tiles;
	/*
	 * Collected during the measure pass, which visits each section exactly once, so a logo that
	 * straddles a tile seam is reported once rather than once per tile it touches.
	 */
	QVector<AnimatedLogoPlacement> animatedLogos;
	/*
	 * Collected in the same pass, and for the same reason: a block straddling a tile seam is
	 * reported once rather than once per tile. Empty for a roll with no sticky blocks in it,
	 * and always empty from `measure()`, which rasterises nothing.
	 */
	QVector<StickyBlockPlacement> stickyBlocks;
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
		 * The slot a sticky block occupies in the roll. Worth its own kind rather than being
		 * another Section box: the block is drawn somewhere else entirely once it pins, and
		 * the overlay showing where its slot *was* is the whole point of drawing it.
		 */
		Sticky,
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
	/*
	 * `animations` may be null, and a renderer without one draws every logo as a still: an
	 * animated file contributes its first frame, baked into the strip exactly as artwork always
	 * was. That is the fallback the test harness and any caller with nowhere to draw an overlay
	 * quad want, and it means animation is something a consumer opts into by being able to
	 * honour it rather than something the renderer assumes of everybody.
	 */
	explicit StripRenderer(LogoCache *logos, AnimatedLogoCache *animations = nullptr)
		: logos(logos),
		  animations(animations)
	{
	}

	/* Maximum tile height in pixels. Tiles are also capped to the strip's own height. */
	static constexpr int kTileHeight = 2048;

	/*
	 * Rasterises the document. When `boxes` is given it is filled with the rectangles the
	 * layout placed things in, for the designer's overlay; the source passes nothing and
	 * pays for none of it.
	 *
	 * The document's fonts are resolved on the way in -- its bundle registered, its stand-ins
	 * applied to whatever this machine still lacks -- so what comes out is drawn in the fonts
	 * the roll asks for wherever they can be had. See render/FontResolution.hpp.
	 */
	Strip render(const Document &document, LayoutBoxes *boxes = nullptr) const;

	/*
	 * Total content height in pixels, excluding lead-in and lead-out. Cheaper than a full
	 * render because nothing is rasterised; used by the designer to show roll duration
	 * while the user is still typing. Fonts are resolved exactly as they are for a render,
	 * so the duration is measured against the metrics the roll will really be laid out with.
	 */
	int measure(const Document &document) const;

private:
	LogoCache *logos;
	AnimatedLogoCache *animations;
};

} // namespace closingtime
