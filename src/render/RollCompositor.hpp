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
#include <QImage>
#include <QRectF>
#include <QVector>

#include <functional>
#include <list>

#include "render/RollRenderer.hpp"
#include "render/RollTimeline.hpp"

class QPainter;

namespace closingtime {

/*
 * What gets drawn, where, in what order and how strongly -- decided once, for whoever draws.
 *
 * The source draws a roll on the GPU and the designer draws one with QPainter, and they used to
 * decide separately what that meant: which tiles, the animated logos over them, the sticky blocks
 * over those, each clipped and placed by arithmetic written twice. A disagreement between the two
 * is exactly a designer that shows something the roll will not do. Now both ask this for a list of
 * draws and only execute it, each in its own way.
 */

/* One picture a draw takes its pixels from. */
struct DrawSource {
	enum class Kind {
		/* One chunk of one layer, rasterized at the view's scale. */
		Chunk,
		/* One frame of an animated logo. */
		LogoFrame,
		/* The shadow cast by one frame of an animated logo whose shadow follows its frames. */
		LogoShadow,
	};

	Kind kind = Kind::Chunk;
	int layer = -1;
	int chunk = -1;
	int logo = -1;
	int frame = -1;

	bool operator==(const DrawSource &other) const = default;
};

struct DrawItem {
	DrawSource source;
	/* Where it lands, in view pixels. */
	QRectF target;
	/* The part of the picture that lands there, in the picture's own pixels. */
	QRectF sourceRect;
	/* Opacity, applied on top of the picture's own alpha. */
	double alpha = 1.0;
	/*
	 * Faded out toward the view's top and bottom edges when the view asks for an edge fade: set for
	 * everything in the roll, and for a sticky card while it moves with the roll -- coming up or
	 * climbing off -- but not while it is held, when the roll runs behind it rather than it being part
	 * of the roll.
	 */
	bool edgeFade = false;
};

using DrawList = QVector<DrawItem>;

/* How the roll is being looked at. */
struct ComposeView {
	/* View pixels per roll pixel. Pictures must have been rasterized at the same scale. */
	qreal scale = 1.0;
	/* Where the strip's top edge lands, in view pixels. */
	double stripTop = 0.0;
	/* The rows of the view anything may be drawn into, in view pixels. Everything else is cut. */
	double clipTop = 0.0;
	double clipBottom = 0.0;

	enum class Blocks {
		/* Where the timeline puts them, in canvas pixels -- the roll as it plays. */
		Timeline,
		/* Back in the slots they leave in the roll -- the roll end to end, as it is written. */
		InSlots,
	};
	Blocks blocks = Blocks::Timeline;

	/*
	 * How tall the canvas is, in view pixels, when the view *is* the canvas at a moment. A divider's
	 * draw-on is measured against it, so a view that is not a canvas -- the roll end to end -- leaves
	 * it at zero and shows every divider whole.
	 */
	double canvasHeight = 0.0;

	/*
	 * How far in from the top and bottom of the view the roll fades to nothing, in view pixels, and
	 * where the view's edges are. Zero is a hard edge. Only drawn by whoever executes the list -- see
	 * edgeFadeAt -- since a fade is a property of each pixel, not of each picture.
	 */
	double fadeTop = 0.0;
	double fadeBottom = 0.0;
	double fadeEdgeTop = 0.0;
	double fadeEdgeBottom = 0.0;

	bool fades() const { return fadeTop > 0.0 || fadeBottom > 0.0; }

	/* The canvas at a moment: the strip where the timeline has it, cut at the canvas's edges. */
	static ComposeView canvas(const RollFrame &frame, int canvasHeight);
};

/*
 * How strongly the roll is drawn at view row `y`: 0 at the view's edges, rising to 1 over the fade's
 * height, eased so the fade has no visible start. The GPU shader computes exactly this per pixel.
 */
double edgeFadeAt(const ComposeView &view, double y);

/*
 * The draws that make up one view of the roll, back to front: every section's chunks in document
 * order, the animated logos in the sections, and then each block that is showing with its own
 * animated logos over it, at the block's opacity.
 *
 * `frame` supplies where the blocks are (in Timeline mode) and how far into its playback each
 * animated logo is; in InSlots mode only the latter is read. Nothing is emitted that falls wholly
 * outside the clip.
 */
DrawList composeFrame(const RollLayout &layout, const RollFrame &frame, const ComposeView &view);

/*
 * Fits a draw to a picture that covers only part of what the draw expects -- a chunk cut down to its
 * ink, which covers `size` at `offset` in the chunk's full rectangle. Returns false when none of the
 * draw falls on the picture. `target` and `source` come back as the draw to make of the picture as it
 * is.
 */
bool fitToPicture(const DrawItem &item, QPoint offset, QSize size, QRectF *target, QRectF *source);

/* The chunks a draw list uses, each once, in the order they are first drawn. */
QVector<DrawSource> chunksIn(const DrawList &list);

/* The picture behind a draw, or a null image when there is none to hand. */
using DrawImageLookup = std::function<QImage(const DrawSource &)>;

/*
 * Executes a draw list with QPainter, offset by `origin`. A draw whose picture cannot be found is
 * skipped rather than failing the frame.
 */
void paintDrawList(QPainter &painter, const DrawList &list, const DrawImageLookup &image, QPointF origin = {});

/*
 * The same, with the view's edge fade applied to every draw that takes one: those are painted into a
 * buffer the size of `area` (in view pixels, before `origin`) and faded there, the rest straight on.
 * Order is kept -- a card drawn after the roll is still drawn over it.
 */
void paintDrawListFaded(QPainter &painter, const DrawList &list, const DrawImageLookup &image, const ComposeView &view,
			const QRect &area, QPointF origin = {});

/* The image an animated-logo draw refers to, straight out of the layout. */
QImage logoImage(const RollLayout &layout, const DrawSource &source);

/*
 * Rasterized chunks, kept between frames and between rebuilds, and dropped least recently used
 * first once they come to more than a budget of bytes.
 *
 * Keyed by chunkKey, so a picture outlives the layout it was drawn for: a rebuild that did not
 * change a section finds that section's chunks already here. An entry is handed back only while
 * every file it was drawn from is as it was -- see FileDependencies.hpp.
 *
 * Not thread-safe: each consumer owns one and touches it only from the render thread, the same
 * arrangement the logo caches have.
 */
class RasterCache {
public:
	explicit RasterCache(qint64 budgetBytes) : budget(budgetBytes) {}

	/* The chunk if it is held and still current, or a null image. */
	QImage find(quint64 key);
	void insert(quint64 key, const QImage &image, const FileStamps &files);
	void clear();

	qint64 bytes() const { return used; }

	/* Looks the chunk up, rasterizing and keeping it when it is not here. */
	QImage chunk(const RollRenderer &renderer, const RollLayout &layout, int layer, int chunk, qreal scale);

private:
	struct Entry {
		quint64 key = 0;
		QImage image;
		FileStamps files;
	};

	void trim();

	qint64 budget = 0;
	qint64 used = 0;
	/* Most recently used at the front. */
	std::list<Entry> entries;
	QHash<quint64, std::list<Entry>::iterator> index;
};

} // namespace closingtime
