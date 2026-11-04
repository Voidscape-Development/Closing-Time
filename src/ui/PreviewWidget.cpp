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

#include "ui/PreviewWidget.hpp"

#include <obs-module.h>

#include <QCoreApplication>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

namespace closingtime {

namespace {

/* How often the pane repaints while animations are running, in milliseconds. */
constexpr int kAnimationIntervalMs = 33;

/* Checkerboard cell size, in widget pixels, used behind a transparent background. */
constexpr int kCheckerSize = 12;

/*
 * Clear space kept either side of the canvas, in widget pixels.
 *
 * The strip used to be drawn edge to edge, which left the canvas's own left and right edges
 * sitting exactly on the widget's border: the frame marking them had nothing on the far side of
 * it to mark them off against, so what it was outlining stopped being legible. A few pixels of
 * surround is the whole difference between a frame around the canvas and a border around a pane.
 */
constexpr int kCanvasInset = 10;

/* Laid over the part of the roll that has not reached the canvas yet. */
constexpr int kUpcomingScrimAlpha = 64;

/*
 * The layout overlay's colors, one per kind of box.
 *
 * Chosen to stay apart from each other rather than to be pretty: the point of the overlay is to
 * say at a glance which rectangle is which, over a roll whose own colors are the user's.
 */
QColor layoutBoxColor(LayoutBox::Kind kind)
{
	switch (kind) {
	case LayoutBox::Kind::Section:
		return QColor(90, 180, 255);
	case LayoutBox::Kind::Content:
		return QColor(120, 220, 150);
	case LayoutBox::Kind::Logo:
		return QColor(255, 170, 60);
	case LayoutBox::Kind::Bridge:
		return QColor(235, 225, 110);
	case LayoutBox::Kind::Divider:
		return QColor(180, 140, 255);
	case LayoutBox::Kind::Sticky:
		return QColor(255, 90, 90);
	case LayoutBox::Kind::Action:
		return QColor(80, 230, 230);
	case LayoutBox::Kind::Text:
	default:
		return QColor(255, 120, 200);
	}
}

void paintChecker(QPainter &painter, const QRect &rect)
{
	painter.fillRect(rect, QColor(48, 48, 48));

	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(62, 62, 62));
	for (int y = rect.top() / kCheckerSize; y <= rect.bottom() / kCheckerSize; ++y) {
		for (int x = rect.left() / kCheckerSize; x <= rect.right() / kCheckerSize; ++x) {
			if ((x + y) % 2 == 0)
				continue;
			painter.drawRect(
				QRect(x * kCheckerSize, y * kCheckerSize, kCheckerSize, kCheckerSize).intersected(rect));
		}
	}
}

/* What the pane keeps of pictures that have come back, in bytes, before the oldest go. */
constexpr qint64 kPreviewPictureBytes = 96LL * 1024 * 1024;

/* What the pane's own raster cache keeps on the render thread's side. */
constexpr qint64 kPreviewRasterCacheBytes = 64LL * 1024 * 1024;

} // namespace

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumWidth(240);
	setFocusPolicy(Qt::WheelFocus);
	/* For the outline that follows the pointer over the roll. */
	setMouseTracking(true);

	/*
	 * Faster than most logo artwork needs and slower than the compositor runs: the frame shown is
	 * looked up from the clock rather than stepped, so this interval decides how smooth the pane
	 * looks and nothing about how fast an animation plays.
	 */
	animationTimer.setInterval(kAnimationIntervalMs);
	connect(&animationTimer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
}

void PreviewWidget::setCaches(std::shared_ptr<LogoCache> logos, std::shared_ptr<AnimatedLogoCache> animations)
{
	/*
	 * Told rather than polled: a picture landing is a reason to repaint, and nothing else in the
	 * pane changes while one is being drawn. The notice comes from the render thread, so it is
	 * queued over to this one, and dropped if the pane has gone by the time it arrives.
	 */
	QPointer<PreviewWidget> guard(this);
	pipeline = ChunkPipeline::create(std::move(logos), std::move(animations), kPreviewRasterCacheBytes,
					 RenderPriority::Normal, [guard] {
						 QMetaObject::invokeMethod(
							 qApp,
							 [guard] {
								 if (guard)
									 guard->update();
							 },
							 Qt::QueuedConnection);
					 });
}

void PreviewWidget::setAnimationPlaying(bool playing)
{
	if (playing == animationPlaying)
		return;

	animationPlaying = playing;

	if (playing) {
		animationClock.restart();
		animationTimer.start();
	} else {
		animationTimer.stop();
	}

	update();
}

void PreviewWidget::setLayout(RollLayoutPtr newLayout, const QColor &newBackground)
{
	layout = std::move(newLayout);
	background = newBackground;

	/*
	 * A rebuild replaces the placements the frames are looked up from, so playback starts again
	 * from the top rather than from wherever a since-discarded decode had reached.
	 */
	if (animationPlaying)
		animationClock.restart();

	/*
	 * The pictures already here stay: they are kept by what they show, so every section the edit
	 * did not change is drawn from what the pane already has while the changed one is redrawn.
	 */
	scroll = std::min(scroll, maxScroll());
	update();
}

void PreviewWidget::setLayoutBoxesVisible(bool visible)
{
	if (visible == layoutBoxesVisible)
		return;

	layoutBoxesVisible = visible;
	update();
}

void PreviewWidget::setHighlightedSection(int index)
{
	if (index == highlightedSection)
		return;

	highlightedSection = index;
	update();
}

void PreviewWidget::setZoom(Zoom newZoom)
{
	if (newZoom == zoom)
		return;

	/* Kept on the same part of the roll across the change, measured in roll pixels. */
	const qreal stripTop = scroll / scaleFactor();
	zoom = newZoom;
	pan = maxPan() / 2;
	scroll = std::clamp(static_cast<int>(stripTop * scaleFactor()), 0, maxScroll());
	update();
}

void PreviewWidget::showPlaybackFrame(const RollFrame &frame)
{
	playbackActive = true;
	playbackFrame = frame;
	update();
}

void PreviewWidget::clearPlayback()
{
	if (!playbackActive)
		return;

	playbackActive = false;
	update();
}

void PreviewWidget::scrollToStripY(int stripY)
{
	scroll = std::clamp(static_cast<int>(stripY * scaleFactor()), 0, maxScroll());
	update();
}

int PreviewWidget::canvasWidth() const
{
	return layout && layout->document ? std::max(1, layout->document->width) : 1920;
}

int PreviewWidget::canvasHeight() const
{
	return layout && layout->document ? std::max(1, layout->document->height) : 1080;
}

int PreviewWidget::canvasScreenWidth() const
{
	if (zoom == Zoom::Actual)
		return canvasWidth();
	return std::max(1, width() - kCanvasInset * 2);
}

int PreviewWidget::maxPan() const
{
	if (zoom != Zoom::Actual)
		return 0;
	return std::max(0, canvasScreenWidth() - (width() - kCanvasInset * 2));
}

int PreviewWidget::canvasLeft() const
{
	return kCanvasInset - std::clamp(pan, 0, maxPan());
}

qreal PreviewWidget::scaleFactor() const
{
	return static_cast<qreal>(canvasScreenWidth()) / static_cast<qreal>(canvasWidth());
}

qreal PreviewWidget::rasterScale() const
{
	return scaleFactor() * devicePixelRatioF();
}

qreal PreviewWidget::onAirHeight() const
{
	return std::min<qreal>(canvasHeight() * scaleFactor(), height());
}

/*
 * The roll stops scrolling once its last pixel has reached the bottom of the framed canvas, not
 * once it has reached the bottom of the pane.
 *
 * The frame is the only part of the pane that says anything about what goes to air; everything
 * below it is content that has not got there yet. Stopping at the pane's own bottom edge left the
 * end of the roll parked in that dimmed run and refused to bring it any further, so the last
 * sections could be looked at but never seen in the frame they are being designed for -- and the
 * taller the pane, the more of the roll that was true of.
 */
int PreviewWidget::maxScroll() const
{
	const int scaledHeight = layout ? static_cast<int>(layout->height * scaleFactor()) : 0;
	return std::max(0, scaledHeight - static_cast<int>(onAirHeight()));
}

QSize PreviewWidget::sizeHint() const
{
	return QSize(480, 640);
}

void PreviewWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	scroll = std::min(scroll, maxScroll());
}

void PreviewWidget::wheelEvent(QWheelEvent *event)
{
	const int delta = event->angleDelta().y();
	if (delta == 0) {
		QWidget::wheelEvent(event);
		return;
	}

	/* Sideways, at 100%, with a horizontal wheel or with shift held. */
	const int sideways = event->angleDelta().x() != 0               ? event->angleDelta().x()
			     : (event->modifiers() & Qt::ShiftModifier) ? delta
									: 0;
	if (zoom == Zoom::Actual && sideways != 0) {
		pan = std::clamp(pan - sideways, 0, maxPan());
		update();
		event->accept();
		return;
	}

	scroll = std::clamp(scroll - delta, 0, maxScroll());
	update();
	event->accept();
}

int PreviewWidget::sectionAt(const QPointF &position) const
{
	if (!layout || playbackActive)
		return -1;

	const qreal scale = scaleFactor();
	const QPointF at((position.x() - canvasLeft()) / scale, (position.y() + scroll) / scale);

	/*
	 * A marker has no height to point inside, so a point a few pixels either side of its line picks
	 * it -- ahead of the sections, since it always sits on the edge of one of them.
	 */
	constexpr qreal kMarkerReach = 5.0;
	for (const ActionCuePlacement &cue : layout->cues) {
		if (std::abs((markerTop(cue) - at.y()) * scale) <= kMarkerReach)
			return cue.section;
	}

	for (const LayoutBox &box : layout->boxes) {
		if ((box.kind == LayoutBox::Kind::Section || box.kind == LayoutBox::Kind::Sticky) &&
		    box.rect.contains(at))
			return box.section;
	}

	return -1;
}

QRectF PreviewWidget::sectionScreenRect(int section) const
{
	if (!layout || section < 0)
		return QRectF();

	QRectF united;
	for (const LayoutBox &box : layout->boxes) {
		if (box.section == section &&
		    (box.kind == LayoutBox::Kind::Section || box.kind == LayoutBox::Kind::Sticky))
			united = united.isNull() ? box.rect : united.united(box.rect);
	}
	if (united.isNull())
		return QRectF();

	const qreal scale = scaleFactor();
	return QRectF(canvasLeft() + united.x() * scale, united.y() * scale - scroll, united.width() * scale,
		      united.height() * scale);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *event)
{
	const int under = sectionAt(event->position());
	if (under != hoveredSection) {
		hoveredSection = under;
		setCursor(under >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
		update();
	}
	QWidget::mouseMoveEvent(event);
}

void PreviewWidget::leaveEvent(QEvent *event)
{
	if (hoveredSection >= 0) {
		hoveredSection = -1;
		update();
	}
	QWidget::leaveEvent(event);
}

/*
 * A click selects the section under it. The layout already knows where every section is -- the
 * overlay draws exactly those boxes -- so finding it is a lookup, and a section in a long roll is
 * picked out of the pane rather than hunted for in the list.
 */
void PreviewWidget::mousePressEvent(QMouseEvent *event)
{
	if (!layout || event->button() != Qt::LeftButton) {
		QWidget::mousePressEvent(event);
		return;
	}

	const int section = sectionAt(event->position());
	if (section >= 0) {
		emit sectionClicked(section);
		event->accept();
		return;
	}

	QWidget::mousePressEvent(event);
}

DrawList PreviewWidget::composeView(double clipBottomPixels) const
{
	const qreal dpr = devicePixelRatioF();

	ComposeView view;
	view.scale = rasterScale();
	view.stripTop = -scroll * dpr;
	view.clipTop = 0.0;
	view.clipBottom = clipBottomPixels * dpr;
	/* The pane is the roll end to end, so a block is shown in the slot it holds in the running order. */
	view.blocks = ComposeView::Blocks::InSlots;

	RollFrame frame;
	const double elapsedMs =
		animationPlaying && animationClock.isValid() ? static_cast<double>(animationClock.elapsed()) : 0.0;
	for (const AnimatedLogoPlacement &placement : layout->animatedLogos)
		frame.logoElapsedMs.append(elapsedMs *
					   std::clamp(placement.playback.speed, kMinLogoSpeed, kMaxLogoSpeed));

	return composeFrame(*layout, frame, view);
}

void PreviewWidget::requestPictures(const DrawList &visible, const DrawList &ahead)
{
	if (!pipeline)
		return;

	const qreal scale = rasterScale();
	std::vector<ChunkRequest> wanted;

	/* What is in the pane first, then a pane's worth below it, which is where a scroll goes next. */
	const auto want = [&](const DrawList &list, double urgency) {
		for (const DrawSource &chunk : chunksIn(list)) {
			const quint64 key = chunkKey(layout->layers.at(chunk.layer), chunk.chunk, scale);
			if (!pictures.contains(key))
				wanted.push_back(ChunkRequest{key, layout, chunk.layer, chunk.chunk, scale, urgency});
		}
	};
	want(visible, 0.0);
	want(ahead, 1.0);

	pipeline->want(std::move(wanted));
}

void PreviewWidget::collectPictures()
{
	if (!pipeline || !pipeline->hasArrived())
		return;

	for (auto &[key, image] : pipeline->takeArrived()) {
		if (image.isNull() || pictures.contains(key))
			continue;
		pictureBytes += image.sizeInBytes();
		pictures.insert(key, Picture{std::move(image), paintCount});
	}
}

void PreviewWidget::trimPictures()
{
	while (pictureBytes > kPreviewPictureBytes && !pictures.isEmpty()) {
		auto oldest = pictures.begin();
		for (auto it = pictures.begin(); it != pictures.end(); ++it) {
			if (it->lastPainted < oldest->lastPainted)
				oldest = it;
		}
		/* Never what is on screen now: that would only be asked for again on the next paint. */
		if (oldest->lastPainted == paintCount)
			break;
		pictureBytes -= oldest->image.sizeInBytes();
		pictures.erase(oldest);
	}
}

/*
 * What the preview is showing, and what the dashed frame means:
 *
 * The pane is the whole roll laid out end to end, drawn at whatever scale fits the canvas across
 * it, with the wheel moving through it. The dashed frame is the canvas itself -- one screenful,
 * held at the top of the pane -- so the roll runs up through it exactly the way it will on air.
 * Everything below the frame is content that has not reached the canvas yet, and it is dimmed to
 * say so: the frame marks a real boundary rather than an outline whose relationship to the
 * pixels around it has to be guessed at.
 *
 * The scale is taken from the canvas's width rather than the pane's, which is what puts the
 * canvas's own left and right edges inside the pane instead of on top of its border.
 */
void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const qreal scale = scaleFactor();
	const QRect canvasColumn(canvasLeft(), 0, canvasScreenWidth(), height());

	/* Anything outside the canvas is not part of the roll, so it is left as plain surround. */
	painter.fillRect(rect(), palette().window());

	if (playbackActive) {
		paintPlayback(painter, canvasColumn);
		return;
	}

	if (background.alpha() >= 255) {
		painter.fillRect(canvasColumn, background);
	} else {
		/* A checkerboard makes it obvious which parts of the roll are transparent. */
		paintChecker(painter, canvasColumn);
		if (background.alpha() > 0)
			painter.fillRect(canvasColumn, background);
	}

	if (layout && !layout->isEmpty()) {
		++paintCount;
		collectPictures();

		const DrawList visible = composeView(height());
		requestPictures(visible, composeView(height() * 2.0));

		const qreal scaleNow = rasterScale();
		const qreal dpr = devicePixelRatioF();

		painter.save();
		painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
		/* The draws are in device pixels; the painter is in widget pixels. */
		painter.scale(1.0 / dpr, 1.0 / dpr);
		paintDrawList(
			painter, visible,
			[&](const DrawSource &source) -> QImage {
				if (source.kind != DrawSource::Kind::Chunk)
					return logoImage(*layout, source);

				const auto it = pictures.find(
					chunkKey(layout->layers.at(source.layer), source.chunk, scaleNow));
				if (it == pictures.end())
					return QImage();
				it->lastPainted = paintCount;
				return it->image;
			},
			QPointF(canvasColumn.left() * dpr, 0.0));
		painter.restore();

		trimPictures();
	}

	const qreal framedHeight = onAirHeight();
	const QRectF onAir(canvasColumn.left() + 0.5, 0.5, canvasColumn.width() - 1.0, framedHeight - 1.0);

	/* Dimmed rather than hidden: this is still the content being edited, just not on air yet. */
	if (framedHeight < height())
		painter.fillRect(QRectF(canvasColumn.left(), framedHeight, canvasColumn.width(),
					height() - framedHeight),
				 QColor(0, 0, 0, kUpcomingScrimAlpha));

	/* Under the canvas frame, which has to stay readable over whatever the overlay draws. */
	if (layoutBoxesVisible)
		paintLayoutBoxes(painter, canvasColumn, scale);
	paintActionMarkers(painter, canvasColumn, scale);
	paintSelection(painter);

	painter.setBrush(Qt::NoBrush);

	/* The canvas runs the full height of the pane; only the screenful at the top is on air. */
	painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
	painter.drawLine(QPointF(canvasColumn.left() + 0.5, 0), QPointF(canvasColumn.left() + 0.5, height()));
	painter.drawLine(QPointF(canvasColumn.right() + 0.5, 0), QPointF(canvasColumn.right() + 0.5, height()));

	QPen pen(QColor(255, 200, 80, 200));
	pen.setStyle(Qt::DashLine);
	pen.setWidth(1);
	painter.setPen(pen);
	painter.drawRect(onAir);
}

void PreviewWidget::paintSelection(QPainter &painter) const
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setBrush(Qt::NoBrush);

	if (hoveredSection >= 0 && hoveredSection != highlightedSection) {
		const QRectF hovered = sectionScreenRect(hoveredSection);
		if (!hovered.isEmpty()) {
			QPen pen(QColor(255, 255, 255, 110), 1);
			pen.setStyle(Qt::DashLine);
			painter.setPen(pen);
			painter.drawRoundedRect(hovered.adjusted(1.5, 0.5, -1.5, -0.5), 3, 3);
		}
	}

	/* The overlay already draws the selected section at full strength, so this is for the plain view. */
	if (!layoutBoxesVisible && highlightedSection >= 0) {
		const QRectF selected = sectionScreenRect(highlightedSection);
		if (!selected.isEmpty()) {
			QColor accent = palette().color(QPalette::Highlight).lighter(130);
			painter.setPen(QPen(accent, 1.5));
			painter.drawRoundedRect(selected.adjusted(1.5, 0.5, -1.5, -0.5), 3, 3);
		}
	}

	painter.restore();
}

void PreviewWidget::paintPlayback(QPainter &painter, const QRect &canvasColumn)
{
	const qreal framedHeight = onAirHeight();
	const QRect onAir(canvasColumn.left(), 0, canvasColumn.width(), qRound(framedHeight));

	if (background.alpha() >= 255) {
		painter.fillRect(onAir, background);
	} else {
		paintChecker(painter, onAir);
		if (background.alpha() > 0)
			painter.fillRect(onAir, background);
	}

	if (layout && !layout->isEmpty()) {
		++paintCount;
		collectPictures();

		const qreal scaleNow = rasterScale();
		const qreal dpr = devicePixelRatioF();

		/* The canvas as the source draws it at this moment, scaled down to the pane. */
		ComposeView view = ComposeView::canvas(playbackFrame, canvasHeight());
		view.scale = scaleNow;
		view.stripTop *= scaleNow;
		view.clipTop = 0.0;
		view.clipBottom = framedHeight * dpr;
		const DrawList visible = composeFrame(*layout, playbackFrame, view);
		requestPictures(visible, visible);

		painter.save();
		painter.setClipRect(onAir);
		painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
		painter.scale(1.0 / dpr, 1.0 / dpr);
		paintDrawList(
			painter, visible,
			[&](const DrawSource &source) -> QImage {
				if (source.kind != DrawSource::Kind::Chunk)
					return logoImage(*layout, source);

				const auto it = pictures.find(
					chunkKey(layout->layers.at(source.layer), source.chunk, scaleNow));
				if (it == pictures.end())
					return QImage();
				it->lastPainted = paintCount;
				return it->image;
			},
			QPointF(canvasColumn.left() * dpr, 0.0));
		painter.restore();

		trimPictures();
	}

	QPen pen(QColor(255, 200, 80, 200));
	pen.setStyle(Qt::DashLine);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(QRectF(onAir).adjusted(0.5, 0.5, -0.5, -0.5));

	if (onAir.bottom() + 24 < height()) {
		QColor hint = palette().color(QPalette::WindowText);
		hint.setAlphaF(0.55);
		painter.setPen(hint);
		painter.drawText(QRect(0, onAir.bottom() + 8, width(), 40),
				 Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
				 QString::fromUtf8(obs_module_text("Designer.Preview.PlayingHint")));
	}
}

/*
 * Draws the layout's own rectangles over the roll: each section's box, the content area left
 * inside its margins and padding, and every block of text, logo and bridge that was placed.
 *
 * This is a debugging view of the layout rather than a part of the design, so it says as much as
 * it can while getting in the way as little as it can: hairlines rather than filled shapes, a
 * section box dashed so it reads as a bound rather than as something drawn, and everything
 * outside the selected section taken down to a quarter strength so the section being edited is
 * the one that stands out. Boxes are drawn back to front -- the section's own box first, its
 * contents over the top -- so a text column sitting exactly on its section's edge is still
 * visible.
 */
void PreviewWidget::paintLayoutBoxes(QPainter &painter, const QRect &canvasColumn, qreal scale) const
{
	if (!layout || layout->boxes.isEmpty())
		return;

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, false);
	painter.setBrush(Qt::NoBrush);

	const auto draw = [&](LayoutBox::Kind kind) {
		for (const LayoutBox &box : layout->boxes) {
			if (box.kind != kind)
				continue;

			const QRectF mapped(canvasColumn.left() + box.rect.x() * scale, box.rect.y() * scale - scroll,
					    box.rect.width() * scale, box.rect.height() * scale);
			if (mapped.bottom() < 0 || mapped.top() > height())
				continue;

			const bool dim = highlightedSection >= 0 && box.section != highlightedSection;

			QColor color = layoutBoxColor(kind);
			color.setAlpha(dim ? 60 : 230);

			QPen boxPen(color, 1);
			/*
			 * A sticky block's slot is dashed for the reason a section box is: it is a
			 * bound rather than something drawn -- and doubly so here, since the block is
			 * drawn somewhere else entirely the moment it pins.
			 */
			if (kind == LayoutBox::Kind::Section || kind == LayoutBox::Kind::Sticky)
				boxPen.setStyle(Qt::DashLine);
			else if (kind == LayoutBox::Kind::Content)
				boxPen.setStyle(Qt::DotLine);

			painter.setPen(boxPen);
			/* Half-pixel inset so a one-pixel pen lands on the boundary, not either side of it. */
			painter.drawRect(mapped.adjusted(0.5, 0.5, -0.5, -0.5));
		}
	};

	draw(LayoutBox::Kind::Section);
	draw(LayoutBox::Kind::Content);
	draw(LayoutBox::Kind::Text);
	draw(LayoutBox::Kind::Logo);
	draw(LayoutBox::Kind::Bridge);
	draw(LayoutBox::Kind::Divider);
	draw(LayoutBox::Kind::Sticky);

	painter.restore();
}

double PreviewWidget::markerTop(const ActionCuePlacement &cue) const
{
	if (layout && cue.block >= 0 && cue.block < layout->blocks.size())
		return layout->blocks.at(cue.block).rect.top() + cue.top;
	return cue.top;
}

/*
 * Draws every Action section as a dashed line across the canvas, labeled with what it does.
 *
 * Drawn whether or not the layout overlay is on: a marker draws nothing into the roll, so without
 * this the only sign of one in the preview would be the absence of anything at all, and an action
 * nobody can see is one nobody can place. The line is where the marker is in the roll, not the line
 * it fires on -- that one is a place on the frame, and the preview is the whole roll end to end.
 */
void PreviewWidget::paintActionMarkers(QPainter &painter, const QRect &canvasColumn, qreal scale) const
{
	if (!layout || layout->cues.isEmpty())
		return;

	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, false);

	QFont font = painter.font();
	font.setPointSizeF(std::max(7.0, font.pointSizeF() * 0.85));
	painter.setFont(font);
	const QFontMetrics metrics(font);

	for (const ActionCuePlacement &cue : layout->cues) {
		const qreal y = std::round(markerTop(cue) * scale - scroll) + 0.5;
		if (y < -metrics.height() || y > height() + metrics.height())
			continue;

		const bool dim = highlightedSection >= 0 && cue.section != highlightedSection;
		QColor color = layoutBoxColor(LayoutBox::Kind::Action);
		color.setAlpha(dim ? 110 : 255);

		QPen pen(color, 1);
		pen.setStyle(Qt::DashLine);
		painter.setPen(pen);
		painter.drawLine(QPointF(canvasColumn.left(), y), QPointF(canvasColumn.right(), y));

		QString label = QStringLiteral("▶ ");
		if (cue.actions.isEmpty())
			label += QString::fromUtf8(sectionTypeName(SectionType::Action));
		else if (cue.actions.size() == 1)
			label += cue.actions.front().summary();
		else
			label +=
				QStringLiteral("%1 +%2").arg(cue.actions.front().summary()).arg(cue.actions.size() - 1);

		const QString text = metrics.elidedText(label, Qt::ElideRight, std::max(40, canvasColumn.width() - 12));
		const QRectF tag(canvasColumn.left() + 4, y - metrics.height() - 2, metrics.horizontalAdvance(text) + 8,
				 metrics.height() + 2);
		painter.fillRect(tag, QColor(0, 0, 0, dim ? 110 : 180));
		painter.setPen(color);
		painter.drawText(tag.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
	}

	painter.restore();
}

} // namespace closingtime
