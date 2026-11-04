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

#include "render/RollCompositor.hpp"

#include <limits>

#include <QPainter>
#include <QtMath>

#include <algorithm>

namespace closingtime {

namespace {

/*
 * Adds a draw, cut to the view's rows. The source rectangle is cut by the same share as the target,
 * so a picture drawn at another size than its own -- a logo frame fitted to its box -- is cut in the
 * right place too.
 */
void addDraw(DrawList *list, const ComposeView &view, const DrawSource &source, const QRectF &target,
	     const QRectF &sourceRect, double alpha, double clipLeft = -std::numeric_limits<double>::infinity(),
	     double clipRight = std::numeric_limits<double>::infinity())
{
	if (target.isEmpty() || sourceRect.isEmpty() || alpha <= 0.0)
		return;

	const double top = std::max(target.top(), view.clipTop);
	const double bottom = std::min(target.bottom(), view.clipBottom);
	if (bottom <= top)
		return;

	/* Cut across as well when asked -- a divider part way through drawing itself on. */
	const double left = std::max(target.left(), clipLeft);
	const double right = std::min(target.right(), clipRight);
	if (right <= left)
		return;

	const double perRow = sourceRect.height() / target.height();
	const double perColumn = sourceRect.width() / target.width();
	DrawItem item;
	item.source = source;
	item.target = QRectF(left, top, right - left, bottom - top);
	item.sourceRect = QRectF(sourceRect.left() + (left - target.left()) * perColumn,
				 sourceRect.top() + (top - target.top()) * perRow, (right - left) * perColumn,
				 (bottom - top) * perRow);
	item.alpha = std::min(1.0, alpha);
	list->append(item);
}

/*
 * The columns of the view a layer may be drawn into, as {clipLeft, clipRight}: everything, unless it
 * is a divider drawing itself on, which shows only the span drawn so far -- worked out from where its
 * middle is on the canvas. Only a view that is the canvas has a "so far": the roll laid end to end
 * shows every divider whole. Returns false when nothing of it is drawn yet.
 */
bool revealClip(const RollLayer &layer, const ComposeView &view, double *clipLeft, double *clipRight)
{
	*clipLeft = -std::numeric_limits<double>::infinity();
	*clipRight = std::numeric_limits<double>::infinity();
	if (!layer.reveal.active() || view.canvasHeight <= 0.0 || view.blocks != ComposeView::Blocks::Timeline)
		return true;

	const double anchorY = view.stripTop + layer.reveal.anchor * view.scale;
	const double progress = layer.reveal.progress(anchorY, view.canvasHeight);
	if (progress <= 0.0)
		return false;
	if (progress < 1.0) {
		const QPair<double, double> span = layer.reveal.drawnSpan(progress);
		*clipLeft = span.first * view.scale;
		*clipRight = span.second * view.scale;
	}
	return true;
}

/* Every chunk of one layer, with the layer's origin at `originTop` view pixels. */
void emitLayer(DrawList *list, const RollLayout &layout, int layerIndex, double originTop, const ComposeView &view,
	       double alpha)
{
	const RollLayer &layer = layout.layers.at(layerIndex);
	const double width = qCeil(layer.width * view.scale);

	double clipLeft = 0.0;
	double clipRight = 0.0;
	if (!revealClip(layer, view, &clipLeft, &clipRight))
		return;

	for (int chunk = 0; chunk < layer.chunkCount(); ++chunk) {
		const ChunkRows rows = chunkRows(layer, chunk, view.scale);
		const double top = originTop + rows.top;
		/* Cheap rejection before any rectangle is built: most of a long roll is off the view. */
		if (top >= view.clipBottom || top + rows.height <= view.clipTop)
			continue;

		DrawSource source;
		source.kind = DrawSource::Kind::Chunk;
		source.layer = layerIndex;
		source.chunk = chunk;
		addDraw(list, view, source, QRectF(0.0, top, width, rows.height), QRectF(0.0, 0.0, width, rows.height),
			alpha, clipLeft, clipRight);
	}
}

/* One animated logo, its shadow under it, with its owner's origin at `originTop`. */
void emitLogo(DrawList *list, const RollLayout &layout, const RollFrame &frame, int index, double originTop,
	      double originStripTop, const ComposeView &view, double alpha)
{
	const AnimatedLogoPlacement &placement = layout.animatedLogos.at(index);
	if (!placement.animation || placement.animation->frames.isEmpty())
		return;

	const double elapsed = index < frame.logoElapsedMs.size() ? frame.logoElapsedMs.at(index) : 0.0;
	const int frameIndex = logoFrameAt(*placement.animation, elapsed, placement.playback.loop);
	if (frameIndex < 0 || frameIndex >= placement.animation->frames.size())
		return;

	/* An animated image in a divider drawing itself on is cut with the rest of it. */
	double clipLeft = -std::numeric_limits<double>::infinity();
	double clipRight = std::numeric_limits<double>::infinity();
	if (placement.block < 0) {
		for (const RollLayer &layer : layout.layers) {
			if (layer.kind == RollLayer::Kind::Section && layer.section == placement.section &&
			    layer.reveal.active()) {
				if (!revealClip(layer, view, &clipLeft, &clipRight))
					return;
				break;
			}
		}
	}

	const qreal scale = view.scale;
	/* A logo is placed in strip space; its owner's origin is where that space starts, here. */
	const double top = originTop + (placement.rect.top() - originStripTop) * scale;

	if (frameIndex < placement.shadowFrames.size()) {
		const QImage &shadow = placement.shadowFrames.at(frameIndex);
		DrawSource source;
		source.kind = DrawSource::Kind::LogoShadow;
		source.logo = index;
		source.frame = frameIndex;
		const QPointF at = placement.rect.topLeft() + placement.shadowOffset;
		addDraw(list, view, source,
			QRectF(at.x() * scale, top + placement.shadowOffset.y() * scale, shadow.width() * scale,
			       shadow.height() * scale),
			QRectF(QPointF(0, 0), shadow.size()), alpha, clipLeft, clipRight);
	}

	const QImage &image = placement.animation->frames.at(frameIndex).image;
	DrawSource source;
	source.kind = DrawSource::Kind::LogoFrame;
	source.logo = index;
	source.frame = frameIndex;
	addDraw(list, view, source,
		QRectF(placement.rect.left() * scale, top, placement.rect.width() * scale,
		       placement.rect.height() * scale),
		QRectF(QPointF(0, 0), image.size()), alpha, clipLeft, clipRight);
}

} // namespace

ComposeView ComposeView::canvas(const RollFrame &frame, int canvasHeight)
{
	ComposeView view;
	view.scale = 1.0;
	view.stripTop = canvasHeight - frame.offset;
	view.clipTop = 0.0;
	view.clipBottom = canvasHeight;
	view.blocks = Blocks::Timeline;
	view.canvasHeight = canvasHeight;
	return view;
}

DrawList composeFrame(const RollLayout &layout, const RollFrame &frame, const ComposeView &view)
{
	DrawList list;
	if (layout.isEmpty())
		return list;

	/* The roll itself: every section, in order, then the logos playing in its holes. */
	for (int index = 0; index < layout.layers.size(); ++index) {
		if (layout.layers.at(index).kind == RollLayer::Kind::Section)
			emitLayer(&list, layout, index, view.stripTop, view, 1.0);
	}

	for (int index = 0; index < layout.animatedLogos.size(); ++index) {
		if (layout.animatedLogos.at(index).block < 0)
			emitLogo(&list, layout, frame, index, view.stripTop, 0.0, view, 1.0);
	}

	/*
	 * Then the blocks, over everything: a pinned card is what the rest of the roll runs behind.
	 * Each is followed by its own logos, at its own opacity, so a card fading up brings its
	 * artwork up with it.
	 */
	for (int block = 0; block < layout.blocks.size(); ++block) {
		const StickyBlockPlacement &placement = layout.blocks.at(block);
		if (placement.layer < 0)
			continue;

		double top = 0.0;
		double alpha = 1.0;
		if (view.blocks == ComposeView::Blocks::InSlots) {
			top = view.stripTop + placement.rect.top() * view.scale;
		} else {
			if (block >= frame.blocks.size() || !frame.blocks.at(block).showing)
				continue;
			top = frame.blocks.at(block).top * view.scale;
			alpha = frame.blocks.at(block).alpha;
		}

		emitLayer(&list, layout, placement.layer, top, view, alpha);

		for (int index = 0; index < layout.animatedLogos.size(); ++index) {
			if (layout.animatedLogos.at(index).block == block)
				emitLogo(&list, layout, frame, index, top, placement.rect.top(), view, alpha);
		}
	}

	return list;
}

bool fitToPicture(const DrawItem &item, QPoint offset, QSize size, QRectF *target, QRectF *source)
{
	const QRectF picture{QPointF(offset), QSizeF(size)};
	const QRectF visible = item.sourceRect.intersected(picture);
	if (visible.isEmpty())
		return false;

	const double perX = item.target.width() / item.sourceRect.width();
	const double perY = item.target.height() / item.sourceRect.height();

	*target = QRectF(item.target.left() + (visible.left() - item.sourceRect.left()) * perX,
			 item.target.top() + (visible.top() - item.sourceRect.top()) * perY, visible.width() * perX,
			 visible.height() * perY);
	*source = visible.translated(-QPointF(offset));
	return true;
}

QVector<DrawSource> chunksIn(const DrawList &list)
{
	QVector<DrawSource> chunks;
	for (const DrawItem &item : list) {
		if (item.source.kind == DrawSource::Kind::Chunk && !chunks.contains(item.source))
			chunks.append(item.source);
	}
	return chunks;
}

QImage logoImage(const RollLayout &layout, const DrawSource &source)
{
	if (source.logo < 0 || source.logo >= layout.animatedLogos.size())
		return QImage();

	const AnimatedLogoPlacement &placement = layout.animatedLogos.at(source.logo);
	if (source.kind == DrawSource::Kind::LogoShadow)
		return source.frame >= 0 && source.frame < placement.shadowFrames.size()
			       ? placement.shadowFrames.at(source.frame)
			       : QImage();

	if (source.kind == DrawSource::Kind::LogoFrame && placement.animation && source.frame >= 0 &&
	    source.frame < placement.animation->frames.size())
		return placement.animation->frames.at(source.frame).image;

	return QImage();
}

void paintDrawList(QPainter &painter, const DrawList &list, const DrawImageLookup &image, QPointF origin)
{
	const double opacity = painter.opacity();

	for (const DrawItem &item : list) {
		const QImage picture = image(item.source);
		if (picture.isNull())
			continue;

		QRectF target = item.target;
		QRectF source = item.sourceRect;
		if (item.source.kind == DrawSource::Kind::Chunk &&
		    !fitToPicture(item, picture.offset(), picture.size(), &target, &source))
			continue;

		painter.setOpacity(opacity * item.alpha);
		painter.drawImage(target.translated(origin), picture, source);
	}

	painter.setOpacity(opacity);
}

QImage RasterCache::find(quint64 key)
{
	const auto it = index.constFind(key);
	if (it == index.constEnd())
		return QImage();

	const std::list<Entry>::iterator entry = *it;
	if (!fileStampsCurrent(entry->files)) {
		/* A logo or a piece of artwork changed on disk since this was drawn: it is wrong now. */
		used -= entry->image.sizeInBytes();
		entries.erase(entry);
		index.erase(it);
		return QImage();
	}

	entries.splice(entries.begin(), entries, entry);
	return entry->image;
}

void RasterCache::insert(quint64 key, const QImage &image, const FileStamps &files)
{
	if (image.isNull())
		return;

	const auto existing = index.constFind(key);
	if (existing != index.constEnd()) {
		used -= (*existing)->image.sizeInBytes();
		entries.erase(*existing);
		index.erase(existing);
	}

	entries.push_front(Entry{key, image, files});
	index.insert(key, entries.begin());
	used += image.sizeInBytes();
	trim();
}

void RasterCache::clear()
{
	entries.clear();
	index.clear();
	used = 0;
}

void RasterCache::trim()
{
	/* Never down to nothing: the entry just added is the one somebody is about to draw. */
	while (used > budget && entries.size() > 1) {
		const Entry &oldest = entries.back();
		used -= oldest.image.sizeInBytes();
		index.remove(oldest.key);
		entries.pop_back();
	}
}

QImage RasterCache::chunk(const RollRenderer &renderer, const RollLayout &layout, int layer, int chunkIndex,
			  qreal scale)
{
	if (layer < 0 || layer >= layout.layers.size())
		return QImage();

	const quint64 key = chunkKey(layout.layers.at(layer), chunkIndex, scale);
	QImage image = find(key);
	if (!image.isNull())
		return image;

	FileStamps files;
	image = renderer.rasterize(layout, layer, chunkIndex, scale, &files);
	insert(key, image, files);
	return image;
}

} // namespace closingtime
