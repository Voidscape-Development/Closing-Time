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

#include <functional>
#include <memory>

#include "model/CreditsModel.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/FileDependencies.hpp"
#include "render/RollTimeline.hpp"

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
 * A layer's picture is rasterized once and moved; an animation is by definition not that. So an
 * animated logo is not painted into the picture it sits in at all. The spot it would have occupied
 * is left empty and the artwork is drawn over the picture afterwards, from its own texture -- by
 * the source on the graphics thread, or by the designer's preview widget on the UI thread. This is
 * what carries it across: where the hole is, what goes in it, and how the frames are timed.
 *
 * The hole can be in a section, which scrolls with the roll, or in a sticky block, which goes
 * wherever the block goes; `block` says which, and the logo is drawn after the picture it belongs
 * to and at that picture's opacity.
 *
 * The layout is unchanged by any of it. An animated logo occupies exactly the box its first frame
 * would have occupied as a still, so nothing above or below it moves when a PNG is swapped for a
 * GIF of the same artwork.
 */
struct AnimatedLogoPlacement {
	/*
	 * The hole, in strip space -- for a logo in a block too, where it is the spot in the block's
	 * slot. Consumers move it with whatever it belongs to: the scroll offset for a section, the
	 * block's own position less its slot's top for a block.
	 */
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
	/* The block it is drawn inside -- an index into RollLayout::blocks -- or -1 for the roll. */
	int block = -1;
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
	/*
	 * The block's picture, `rect.height() + margin * 2` tall at the strip's width. Filled only by
	 * RollRenderer::render, which rasterizes everything up front; a RollLayout leaves it null and
	 * carries the block as a layer instead, rasterized when it is wanted.
	 */
	QImage image;
	/* The layer the block's picture is, in RollLayout::layers. */
	int layer = -1;

	/* Everything about how it pins, copied off the section so the compositor reads no model. */
	StickyEntrance entrance = StickyEntrance::WithRoll;
	double fadeIn = 0.75;
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
	 * The backdrop itself is painted into the picture rather than carried here as a color to be
	 * drawn behind it. A flat quad would mean a second effect -- libobs draws solids and textures
	 * from different ones -- started inside the pass that is drawing the strip, and the panel is
	 * a rectangle the rasterizer can fill for nothing at rebuild time.
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

	/*
	 * How far the picture reaches above and below the slot's own top edge, for a block whose slot
	 * sits at `top`.
	 *
	 * The slot is where the block *belongs*; the picture is what is actually on screen, and it is
	 * `margin` taller at each end -- the backdrop's outset, and whatever the children paint outside
	 * their own boxes. Anything asking "is this block on the frame" has to ask about the picture,
	 * because the picture is what a viewer can see: a block judged gone by its slot alone leaves its
	 * last band of ink sitting at the top of the frame, and a roll that ends there ends over it.
	 */
	double pictureTop(double top) const { return top - margin; }
	double pictureBottom(double top) const { return top + rect.height() + margin; }

	/* True when none of the picture is left on a canvas this tall. */
	bool offFrame(double top, int canvasHeight) const
	{
		return pictureBottom(top) <= 0.0 || pictureTop(top) >= canvasHeight;
	}

	/* True when the whole picture has gone off the top, which is what "left the screen" means. */
	bool clearedFrame(double top) const { return pictureBottom(top) <= 0.0; }
};

/*
 * The whole roll rasterized up front: every section composited into tiles, every block's picture
 * filled in. What RollRenderer::render hands back, for the harness and for anything else that wants
 * the roll as pictures rather than as layers to draw from -- the source and the designer draw from a
 * RollLayout and rasterize only what they are about to show.
 */
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
	 * and always empty from `measure()`, which rasterizes nothing.
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
		 * centered in it and a stack of rules is taller than any one of them.
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
 * One picture the roll is drawn from, and where it goes.
 *
 * The roll used to be one tall picture, rasterized whole on every edit and kept whole on the GPU.
 * It is now a stack of these: a picture per section, which scrolls with the roll, and one per sticky
 * block, which goes wherever the timeline puts the block. Composited in order with premultiplied
 * over they make exactly the pixels one shared picture would have -- over is associative -- but
 * each can be rasterized, cached, uploaded and dropped on its own. Changing one name redraws one
 * section rather than the roll, and a ten-minute roll holds only the few sections near the frame.
 *
 * A picture is drawn in chunks of at most kChunkHeight rows, which are what is actually rasterized,
 * cached and uploaded: a list of four hundred names is one section and far too tall to be one
 * texture, or one upload.
 */
struct RollLayer {
	enum class Kind {
		/* A top-level section. Its origin is the top of the strip; it scrolls with the roll. */
		Section,
		/* A sticky block. Its origin is its slot's top edge, wherever the block currently is. */
		Block,
	};

	Kind kind = Kind::Section;
	/* Index into Document::sections. */
	int section = -1;
	/* For a block, the index into RollLayout::blocks. */
	int block = -1;
	/* The section's own top edge in strip space; a block's slot top. What it is laid out at. */
	int sectionTop = 0;

	/*
	 * The picture's top edge and height, relative to the layer's origin. A section's picture runs
	 * past its box by whatever it paints outside it -- shadows, a panel's outset, glyphs taller than
	 * their line -- and stops at the ends of the strip; a block's runs past its slot by its margin.
	 */
	int pictureTop = 0;
	int pictureHeight = 0;
	int width = 0;

	/*
	 * A hash of everything the picture's pixels depend on: the section with its styles and panels
	 * resolved through their presets, the canvas width, the fonts, the picture's bounds, and
	 * whether animated logos are left as holes. Two layers with the same key draw the same pixels,
	 * which is what lets a rebuilt roll keep every picture it did not change -- on the render thread
	 * and on the GPU both.
	 */
	quint64 key = 0;

	/*
	 * The height of every row of the section's list, as the layout measured them, so that painting
	 * one chunk of a long list skips the rows it cannot see without laying each of them out again.
	 * Null for a section with no list.
	 */
	std::shared_ptr<const QVector<int>> rowHeights;

	int chunkCount() const;
	/* A chunk's rows, relative to the picture's top edge. */
	int chunkTop(int chunk) const;
	int chunkHeight(int chunk) const;
};

/* Rows per chunk: the most any one rasterization, cache entry or upload covers. */
constexpr int kChunkHeight = 1024;

/*
 * Where a chunk lands at `scale`, in device pixels relative to the layer's origin scaled -- rows
 * [top, top + height). Chunk boundaries are rounded down at every scale, so the chunks of a picture
 * always meet exactly, whatever fraction of a pixel the scale puts them on.
 */
struct ChunkRows {
	int top = 0;
	int height = 0;
};

ChunkRows chunkRows(const RollLayer &layer, int chunk, qreal scale);

/* A cache key for one chunk of one layer at one scale. */
quint64 chunkKey(const RollLayer &layer, int chunk, qreal scale);

/*
 * A roll laid out: everything needed to draw any part of it at any moment, and none of the pixels.
 *
 * Built on the render thread, at about the cost of measuring the roll, and handed whole to the
 * thread that draws it. Immutable once built, and shared, because the source and the designer keep
 * drawing from one long after the job that built it has gone.
 */
struct RollLayout {
	/* The document as it was laid out, with its fonts resolved. Rasterizing reads it. */
	std::shared_ptr<const Document> document;
	int width = 0;
	/* The strip's height: every section and both leads. */
	int height = 0;

	/* Sections in document order, then blocks in block order -- the order they are composited in. */
	QVector<RollLayer> layers;
	QVector<StickyBlockPlacement> blocks;
	QVector<AnimatedLogoPlacement> animatedLogos;
	/* Collected only when asked for; see RollRenderer::layout. */
	LayoutBoxes boxes;

	/* True when animated logos are holes with placements, rather than baked in as stills. */
	bool animated = false;

	/*
	 * Everything every picture depends on besides its own section -- the canvas width, the fonts,
	 * the renderer's version -- as the bytes the keys were hashed with. Kept so the next layout can
	 * tell whether this one's keys still hold; see RollRenderer::layout.
	 */
	QByteArray context;

	/* How many keys were carried over from the previous layout rather than hashed; for the harness. */
	int reusedKeys = 0;

	bool isEmpty() const { return height <= 0 || layers.isEmpty(); }

	/* What the timeline needs, from the layout and the document's own playback settings. */
	TimelineInputs timelineInputs() const;
};

using RollLayoutPtr = std::shared_ptr<const RollLayout>;

/*
 * Lays documents out and rasterizes them.
 *
 * Painting is into a QImage, which Qt supports off the GUI thread, so this runs on the shared render
 * thread (see RenderThread.hpp) rather than anywhere OBS needs to stay responsive.
 */
class RollRenderer {
public:
	/*
	 * `animations` may be null, and a renderer without one draws every logo as a still: an
	 * animated file contributes its first frame, baked into the picture exactly as artwork always
	 * was. That is the fallback the test harness and any caller with nowhere to draw an overlay
	 * quad want, and it means animation is something a consumer opts into by being able to
	 * honor it rather than something the renderer assumes of everybody.
	 */
	explicit RollRenderer(LogoCache *logos, AnimatedLogoCache *animations = nullptr)
		: logos(logos),
		  animations(animations)
	{
	}

	/* Height of the tiles RollRenderer::render composites into. Tiles are also capped to the strip. */
	static constexpr int kTileHeight = 1024;

	/*
	 * Lays the document out without rasterizing anything. `boxes` asks for the layout overlay's
	 * rectangles as well, which only the designer wants.
	 *
	 * `cancelled`, when given, is asked between sections, and an abandoned layout comes back null:
	 * a roll edited again while the last edit was still being laid out has no use for the result.
	 *
	 * The document's fonts are resolved on the way in -- its bundle registered, its stand-ins
	 * applied to whatever this machine still lacks -- so what is drawn later is in the fonts the
	 * roll asks for wherever they can be had. See render/FontResolution.hpp.
	 *
	 * `previous`, when given, is the layout this one replaces. A section it holds unchanged, at the
	 * same picture bounds, keeps the key it had there rather than being hashed again -- which is most
	 * of what laying out a long roll costs, and an edit changes one section.
	 */
	RollLayoutPtr layout(const Document &document, bool boxes = false, const std::function<bool()> &cancelled = {},
			     const RollLayout *previous = nullptr) const;

	/*
	 * Rasterizes one chunk of one layer, at `scale` device pixels per roll pixel. `files`, when
	 * given, receives every file the picture was drawn from -- see FileDependencies.hpp.
	 *
	 * The picture comes back cut down to its ink: `offset()` says where its top-left corner sits in
	 * the chunk's full rows (see chunkRows), and everything outside it is transparent. A chunk with
	 * nothing in it is a single transparent pixel.
	 *
	 * The layout must have been made by a renderer with the same caches, or at least the same
	 * answer to whether it has an animation cache: a layout that left holes for animated logos
	 * rasterized by one that has no animations to fill them would lose the logos.
	 */
	QImage rasterize(const RollLayout &layout, int layer, int chunk, qreal scale = 1.0,
			 FileStamps *files = nullptr) const;

	/* Every chunk of one layer, stitched into one picture at scale 1. */
	QImage rasterizeLayer(const RollLayout &layout, int layer) const;

	/*
	 * Lays out and rasterizes the whole roll: sections composited into tiles, blocks' pictures
	 * filled in. When `boxes` is given it is filled with the layout overlay's rectangles.
	 */
	Strip render(const Document &document, LayoutBoxes *boxes = nullptr) const;

	/*
	 * The roll painted the way it used to be: every section into one shared picture, in order,
	 * with no layers. Kept for the harness, which compares it against the layered roll -- the two
	 * must agree to within rounding wherever pictures overlap, and exactly everywhere else.
	 */
	QImage paintReference(const Document &document) const;

	/*
	 * Total content height in pixels, excluding lead-in and lead-out. Cheaper than a full
	 * render because nothing is rasterized; used by the designer to show roll duration
	 * while the user is still typing. Fonts are resolved exactly as they are for a render,
	 * so the duration is measured against the metrics the roll will really be laid out with.
	 */
	int measure(const Document &document) const;

private:
	LogoCache *logos;
	AnimatedLogoCache *animations;
};

} // namespace closingtime
