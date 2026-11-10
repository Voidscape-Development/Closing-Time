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

#include "ui/DividerEditor.hpp"

#include <obs-module.h>
#include <obs.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "model/DividerDesigns.hpp"
#include "model/StyleLibrary.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/RollRenderer.hpp"
#include "ui/DesignerWidgets.hpp"
#include "ui/StyleControls.hpp"
#include "ui/ToolButtons.hpp"

namespace closingtime {

const char *const kDividerElementMime = "application/x-closingtime-divider-element";
const char *const kDividerMoveMime = "application/x-closingtime-divider-move";

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString dividerDesignText(const DividerDesign &design)
{
	const QString key = QStringLiteral("Designer.DividerDesign.") + QString::fromLatin1(design.id);
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));
	return text == key ? QString::fromUtf8(design.name) : text;
}

/* The ink the little pictures are drawn in: the palette's text color, so they read in any theme. */
QColor pickerInk()
{
	return QApplication::palette().color(QPalette::Active, QPalette::WindowText);
}

/* Everything already drawn in `image`, recolored to `ink` while keeping its coverage. */
QImage inked(QImage image, const QColor &ink)
{
	QPainter painter(&image);
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(image.rect(), ink);
	return image;
}

constexpr qreal kIconScale = 2.0;

/*
 * A shape drawn into `box` in `ink`. A piece is drawn once at its own proportions, as large as the
 * box allows; a line is drawn the way it is used -- a thin band across the box, tiled where the shape
 * spreads and stretched where it scales -- because one dot is not what "Dots" looks like as a line.
 */
void drawShape(QPainter *target, DividerShape shape, const QRectF &box, bool asLine, const QColor &ink)
{
	const QString svg = dividerShapeSvg(shape);
	if (svg.isEmpty() || box.isEmpty())
		return;

	const qreal scale = std::max<qreal>(target->device() ? target->device()->devicePixelRatioF() : 1.0, 1.0);
	QImage image((box.size() * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
	if (image.isNull())
		return;
	image.fill(Qt::transparent);

	QSvgRenderer renderer(svg.toUtf8());
	const DividerShapeInfo &info = dividerShapeInfo(shape);
	{
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing);
		const qreal width = image.width();
		const qreal height = image.height();

		if (asLine) {
			if (info.stretch == DividerStretch::Scale) {
				const qreal band = std::max<qreal>(2.0, height * 0.3);
				renderer.render(&painter, QRectF(0, (height - band) / 2.0, width, band));
			} else {
				/*
				 * At least a few repeats, sized to fit, because a run is what a spreading line
				 * looks like -- one dot is a dot, and one ticked tile wider than the box is nothing.
				 */
				const qreal natural = std::max<qreal>(2.0, height * 0.3) * info.aspect;
				const int count = std::max(3, static_cast<int>(width / std::max<qreal>(natural, 1.0)));
				const qreal tile = width / count;
				const qreal band = std::min(height * 0.5, tile / info.aspect);
				const qreal top = (height - band) / 2.0;
				for (int i = 0; i < count; ++i)
					renderer.render(&painter, QRectF(i * tile, top, tile, band));
			}
		} else {
			qreal drawHeight = height - 2.0 * scale;
			qreal drawWidth = drawHeight * info.aspect;
			if (drawWidth > width - 2.0 * scale) {
				drawWidth = width - 2.0 * scale;
				drawHeight = drawWidth / info.aspect;
			}
			renderer.render(&painter, QRectF((width - drawWidth) / 2.0, (height - drawHeight) / 2.0,
							 drawWidth, drawHeight));
		}
	}

	image = inked(image, ink);
	image.setDevicePixelRatio(scale);
	target->drawImage(box.topLeft(), image);
}

/* The little picture for a word or an image, which have no shape in the table to draw. */
void drawKind(QPainter *painter, DividerElement::Kind kind, const QRectF &box, const QColor &ink)
{
	painter->save();
	painter->setRenderHint(QPainter::Antialiasing);
	if (kind == DividerElement::Kind::Text) {
		QFont font = painter->font();
		font.setPixelSize(std::max(9, qRound(box.height() * 0.62)));
		font.setBold(true);
		painter->setFont(font);
		painter->setPen(ink);
		painter->drawText(box, Qt::AlignCenter, QStringLiteral("Aa"));
	} else {
		const qreal side = std::min(box.width(), box.height()) * 0.8;
		const QRectF frame(box.center().x() - side * 0.6, box.center().y() - side / 2.0, side * 1.2, side);
		painter->setPen(QPen(ink, 1.3));
		painter->setBrush(Qt::NoBrush);
		painter->drawRoundedRect(frame, 1.5, 1.5);
		QPolygonF hill;
		hill << QPointF(frame.left() + 2, frame.bottom() - 2)
		     << QPointF(frame.left() + frame.width() * 0.35, frame.top() + side * 0.35)
		     << QPointF(frame.left() + frame.width() * 0.55, frame.bottom() - side * 0.3)
		     << QPointF(frame.left() + frame.width() * 0.7, frame.top() + side * 0.5)
		     << QPointF(frame.right() - 2, frame.bottom() - 2);
		painter->setPen(Qt::NoPen);
		painter->setBrush(ink);
		painter->drawPolygon(hill);
	}
	painter->restore();
}

/* An element's picture: its shape, its line, or the picture standing for a word or an image. */
void drawElement(QPainter *painter, const DividerElement &element, const QRectF &box, const QColor &ink)
{
	switch (element.kind) {
	case DividerElement::Kind::Shape:
		if (dividerShapeUsesFile(element.shape)) {
			painter->save();
			painter->setPen(ink);
			QFont font = painter->font();
			font.setPixelSize(10);
			font.setBold(true);
			painter->setFont(font);
			painter->drawText(box, Qt::AlignCenter, QStringLiteral("SVG"));
			painter->restore();
		} else {
			drawShape(painter, element.shape, box, false, ink);
		}
		break;
	case DividerElement::Kind::Line:
		if (dividerShapeIsEmpty(element.lineShape)) {
			/* Space: a bracket showing room held open with nothing in it. */
			painter->save();
			QPen pen(ink, 1.0, Qt::DashLine);
			painter->setPen(pen);
			const qreal y = box.center().y();
			painter->drawLine(QPointF(box.left() + 3, y), QPointF(box.right() - 3, y));
			painter->drawLine(QPointF(box.left() + 3, y - 4), QPointF(box.left() + 3, y + 4));
			painter->drawLine(QPointF(box.right() - 3, y - 4), QPointF(box.right() - 3, y + 4));
			painter->restore();
		} else if (dividerShapeUsesFile(element.lineShape)) {
			painter->save();
			painter->setPen(ink);
			painter->drawText(box, Qt::AlignCenter, QStringLiteral("SVG"));
			painter->restore();
		} else {
			drawShape(painter, element.lineShape, box, true, ink);
		}
		break;
	case DividerElement::Kind::Text:
	case DividerElement::Kind::Image:
		drawKind(painter, element.kind, box, ink);
		break;
	}
}

/* What an element is called, for a chip, a tile and the inspector's title. */
QString elementName(const DividerElement &element)
{
	switch (element.kind) {
	case DividerElement::Kind::Shape:
		return dividerShapeText(element.shape);
	case DividerElement::Kind::Line:
		return dividerShapeIsEmpty(element.lineShape) ? moduleText("Designer.DividerElement.Space")
							      : dividerShapeText(element.lineShape);
	case DividerElement::Kind::Text:
		return element.text.isEmpty() ? moduleText("Designer.DividerElement.Text")
					      : QStringLiteral("“%1”").arg(element.text);
	case DividerElement::Kind::Image:
		return moduleText("Designer.DividerElement.Image");
	}
	return QString();
}

QByteArray elementBytes(const DividerElement &element)
{
	OBSDataAutoRelease data = obs_data_create();
	element.save(data);
	return QByteArray(obs_data_get_json(data));
}

bool elementFromBytes(const QByteArray &bytes, DividerElement *element)
{
	OBSDataAutoRelease data = obs_data_create_from_json(bytes.constData());
	if (!data)
		return false;
	element->load(data);
	return true;
}

DividerElement lineOf(DividerShape shape)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Line;
	element.lineShape = shape;
	return element;
}

DividerElement pieceOf(DividerShape shape)
{
	DividerElement element;
	element.kind = DividerElement::Kind::Shape;
	element.shape = shape;
	return element;
}

/*
 * A divider drawn by the real renderer and kept, for the design picker. Keyed by the figure's own
 * saved form, so a built-in design is drawn once and a saved one again only when it changes.
 */
QIcon dividerFigureIcon(const Divider &figure, const QSize &size)
{
	static QHash<QByteArray, QIcon> cache;
	OBSDataAutoRelease keyData = obs_data_create();
	figure.save(keyData);
	const QByteArray key = QByteArray(obs_data_get_json(keyData)) + QByteArray::number(size.width());
	if (const auto found = cache.constFind(key); found != cache.cend())
		return *found;

	Section section = Section::makeDefault(SectionType::SectionDivider);
	applyDividerDesign(section, DividerDesign{"", "", figure});
	section.sectionWidth = 1.0;
	section.marginX = 0;
	section.paddingTop = 2;
	section.paddingBottom = 2;
	section.style.pixelSize = 26;
	section.style.bold = true;
	/*
	 * Drawn straight in the theme's text color, so the divider's own ink reads on the list it sits in
	 * while a part in a color of its own keeps that color -- a recolor afterwards would flatten both.
	 */
	section.style.color = pickerInk();

	Document document;
	document.width = qRound(size.width() * kIconScale);
	document.sections = {section};
	document.bundleFonts = false;

	static LogoCache logos;
	const RollRenderer renderer(&logos);
	const QImage drawn = renderer.paintReference(document);

	QImage image(size * kIconScale, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	if (!drawn.isNull()) {
		const QImage fitted = drawn.height() > image.height() ? drawn.scaled(image.size(), Qt::KeepAspectRatio,
										     Qt::SmoothTransformation)
								      : drawn;
		QPainter painter(&image);
		painter.drawImage(QPointF((image.width() - fitted.width()) / 2.0,
					  (image.height() - fitted.height()) / 2.0),
				  fitted);
	}

	QPixmap pixmap = QPixmap::fromImage(image);
	pixmap.setDevicePixelRatio(kIconScale);
	const QIcon icon(pixmap);
	cache.insert(key, icon);
	return icon;
}

void addAlignment(QComboBox *box)
{
	box->addItem(moduleText("Designer.Align.Left"), static_cast<int>(HAlign::Left));
	box->addItem(moduleText("Designer.Align.Center"), static_cast<int>(HAlign::Center));
	box->addItem(moduleText("Designer.Align.Right"), static_cast<int>(HAlign::Right));
}

void selectData(QComboBox *box, const QVariant &data)
{
	const int index = box->findData(data);
	if (index >= 0)
		box->setCurrentIndex(index);
}

/* Chip and tile metrics, in logical pixels. */
constexpr int kChipHeight = 58;
constexpr int kPieceChipWidth = 62;
constexpr int kLineChipMin = 74;
constexpr int kLineChipPerWeight = 46;
constexpr int kLineChipMax = 260;
constexpr int kChipGap = 6;
constexpr int kTrackMargin = 8;
constexpr int kHandleWidth = 9;
constexpr int kTileSize = 40;
constexpr int kTileGap = 4;
constexpr int kHeadingHeight = 18;

int chipWidth(const DividerElement &element)
{
	if (!element.isLine())
		return kPieceChipWidth;
	return std::clamp(kLineChipMin + qRound(element.weight * kLineChipPerWeight), kLineChipMin, kLineChipMax);
}

} // namespace

QString dividerShapeText(DividerShape shape)
{
	const QString key = QStringLiteral("Designer.DividerShape.") + QString::fromLatin1(dividerShapeId(shape));
	const QString text = QString::fromUtf8(obs_module_text(key.toUtf8().constData()));
	return text == key ? QString::fromUtf8(dividerShapeName(shape)) : text;
}

QIcon dividerShapeIcon(DividerShape shape, bool asLine)
{
	static QHash<int, QIcon> cache;
	const int key = static_cast<int>(shape) * 2 + (asLine ? 1 : 0);
	if (const auto found = cache.constFind(key); found != cache.cend())
		return *found;

	const QSize logical = asLine ? QSize(44, 18) : QSize(30, 18);
	QPixmap pixmap(logical * kIconScale);
	pixmap.setDevicePixelRatio(kIconScale);
	pixmap.fill(Qt::transparent);
	{
		QPainter painter(&pixmap);
		DividerElement element = asLine ? lineOf(shape) : pieceOf(shape);
		drawElement(&painter, element, QRectF(QPointF(0, 0), QSizeF(logical)), pickerInk());
	}
	const QIcon icon(pixmap);
	cache.insert(key, icon);
	return icon;
}

/* ---- DividerPreview ------------------------------------------------------------------------- */

DividerPreview::DividerPreview(QWidget *parent) : QWidget(parent)
{
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(moduleText("Designer.DividerPreview.Tip"));

	revealTimer = new QTimer(this);
	revealTimer->setInterval(16);
	connect(revealTimer, &QTimer::timeout, this, [this] {
		/* Long enough to read the shape of the sweep, short enough not to wait on. */
		constexpr double kPlayMs = 1100.0;
		const double elapsed = QDateTime::currentMSecsSinceEpoch() - revealStartedMs;
		/* Eased out, the way a pen slows into the end of a stroke. */
		const double t = std::clamp(elapsed / kPlayMs, 0.0, 1.0);
		revealProgress = 1.0 - (1.0 - t) * (1.0 - t);
		if (t >= 1.0) {
			revealProgress = 1.0;
			revealTimer->stop();
		}
		update();
	});
}

void DividerPreview::playReveal(DividerReveal mode)
{
	revealMode = mode;
	if (mode == DividerReveal::None) {
		revealProgress = 1.0;
		revealTimer->stop();
		update();
		return;
	}
	revealProgress = 0.0;
	revealStartedMs = QDateTime::currentMSecsSinceEpoch();
	revealTimer->start();
	update();
}

void DividerPreview::setImage(const QImage &image)
{
	picture = image;
	update();
}

QSize DividerPreview::sizeHint() const
{
	return QSize(400, 84);
}

void DividerPreview::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);

	/* A dark stage, since a roll is nearly always light ink over footage or black. */
	const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
	QPainterPath path;
	path.addRoundedRect(frame, 6, 6);
	painter.fillPath(path, QColor(24, 24, 30));
	painter.setPen(palette().color(QPalette::Mid));
	painter.drawPath(path);

	if (picture.isNull()) {
		painter.setPen(QColor(150, 150, 160));
		painter.drawText(rect(), Qt::AlignCenter, moduleText("Designer.DividerPreview.Empty"));
		return;
	}

	const QRectF area = frame.adjusted(10, 8, -10, -8);
	QSizeF size = picture.size();
	size.scale(area.size(), Qt::KeepAspectRatio);
	/* Never blown up past its own size: a thin rule shown thicker than it is would be a lie. */
	if (size.width() > picture.width())
		size = picture.size();
	const QRectF target(area.center().x() - size.width() / 2.0, area.center().y() - size.height() / 2.0,
			    size.width(), size.height());
	painter.setClipPath(path);

	/* A reveal playing: the same sweep the compositor makes, over the picture's own width. */
	if (revealMode != DividerReveal::None && revealProgress < 1.0) {
		const double width = target.width() * revealProgress;
		double from = target.left();
		if (revealMode == DividerReveal::FromCenter)
			from = target.center().x() - width / 2.0;
		else if (revealMode == DividerReveal::FromRight)
			from = target.right() - width;
		painter.setClipRect(QRectF(from, frame.top(), width, frame.height()), Qt::IntersectClip);
	}

	painter.drawImage(target, picture);
}

/* ---- DividerPalette ------------------------------------------------------------------------- */

DividerPalette::DividerPalette(QWidget *parent) : QWidget(parent)
{
	setMouseTracking(true);
	setAcceptDrops(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

	groupTitles = {moduleText("Designer.DividerPalette.Lines"), moduleText("Designer.DividerPalette.Shapes"),
		       moduleText("Designer.DividerPalette.Other")};

	for (DividerShape shape : dividerShapesForRole(DividerRoleArm)) {
		if (!dividerShapeUsesFile(shape))
			tiles.append(Tile{lineOf(shape), dividerShapeText(shape), 0, QRect()});
	}
	tiles.append(Tile{lineOf(DividerShape::None), moduleText("Designer.DividerElement.Space"), 0, QRect()});
	tiles.append(Tile{lineOf(DividerShape::Custom), moduleText("Designer.DividerElement.LineSvg"), 0, QRect()});

	for (DividerShape shape : dividerShapesForRole(DividerRolePiece)) {
		if (!dividerShapeIsEmpty(shape))
			tiles.append(Tile{pieceOf(shape), dividerShapeText(shape), 1, QRect()});
	}

	DividerElement text;
	text.kind = DividerElement::Kind::Text;
	text.text = moduleText("Designer.DividerElement.DefaultText");
	tiles.append(Tile{text, moduleText("Designer.DividerElement.Text"), 2, QRect()});

	DividerElement image;
	image.kind = DividerElement::Kind::Image;
	tiles.append(Tile{image, moduleText("Designer.DividerElement.Image"), 2, QRect()});
}

int DividerPalette::layoutTiles(int width) const
{
	headings.clear();
	const int usable = std::max(width, kTileSize);
	int y = 0;
	int group = -1;
	int x = 0;

	for (Tile &tile : tiles) {
		if (tile.group != group) {
			if (group >= 0)
				y += kTileSize + kTileGap + 2;
			group = tile.group;
			headings.append(QRect(0, y, usable, kHeadingHeight));
			y += kHeadingHeight;
			x = 0;
		}
		if (x > 0 && x + kTileSize > usable) {
			x = 0;
			y += kTileSize + kTileGap;
		}
		tile.rect = QRect(x, y, kTileSize, kTileSize);
		x += kTileSize + kTileGap;
	}
	return y + kTileSize + 2;
}

int DividerPalette::heightForWidth(int width) const
{
	return layoutTiles(width);
}

QSize DividerPalette::sizeHint() const
{
	return QSize(480, layoutTiles(480));
}

int DividerPalette::tileAt(const QPoint &pos) const
{
	for (int i = 0; i < tiles.size(); ++i) {
		if (tiles.at(i).rect.contains(pos))
			return i;
	}
	return -1;
}

void DividerPalette::paintEvent(QPaintEvent *)
{
	layoutTiles(width());

	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	QFont small = font();
	small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
	small.setBold(true);
	painter.setFont(small);
	painter.setPen(palette().color(QPalette::PlaceholderText));
	for (int i = 0; i < headings.size(); ++i)
		painter.drawText(headings.at(i).adjusted(2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
				 groupTitles.value(i).toUpper());

	const QColor ink = palette().color(QPalette::WindowText);
	for (int i = 0; i < tiles.size(); ++i) {
		const Tile &tile = tiles.at(i);
		const QRectF box = QRectF(tile.rect).adjusted(0.5, 0.5, -0.5, -0.5);
		painter.setPen(palette().color(QPalette::Mid));
		painter.setBrush(i == hovered ? subtleFill(palette())
					      : palette().color(QPalette::Base));
		painter.drawRoundedRect(box, 5, 5);
		drawElement(&painter, tile.element, box.adjusted(5, 7, -5, -7), ink);
	}

	/* A chip dragged here is about to be taken off the divider: say so over the whole palette. */
	if (removing) {
		QColor wash = palette().color(QPalette::Highlight);
		wash.setAlpha(60);
		painter.fillRect(rect(), wash);
		QFont bold = font();
		bold.setBold(true);
		painter.setFont(bold);
		painter.setPen(palette().color(QPalette::HighlightedText));
		QColor plate = palette().color(QPalette::Highlight);
		const QRect label = QRect(0, 0, width(), 28).translated(0, height() / 2 - 14);
		painter.fillRect(label, plate);
		painter.drawText(label, Qt::AlignCenter, moduleText("Designer.DividerPalette.DropToRemove"));
	}
}

bool DividerPalette::event(QEvent *event)
{
	if (event->type() == QEvent::ToolTip) {
		auto *help = static_cast<QHelpEvent *>(event);
		const int tile = tileAt(help->pos());
		if (tile >= 0) {
			QToolTip::showText(help->globalPos(),
					   QStringLiteral("<b>%1</b><br>%2")
						   .arg(tiles.at(tile).name.toHtmlEscaped(),
							moduleText("Designer.DividerPalette.TileTip")),
					   this, tiles.at(tile).rect);
		} else {
			QToolTip::hideText();
		}
		return true;
	}
	return QWidget::event(event);
}

void DividerPalette::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	pressed = tileAt(event->position().toPoint());
	pressPos = event->position().toPoint();
}

void DividerPalette::mouseMoveEvent(QMouseEvent *event)
{
	const int over = tileAt(event->position().toPoint());
	if (over != hovered) {
		hovered = over;
		setCursor(over >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
		update();
	}

	if (pressed < 0 || !(event->buttons() & Qt::LeftButton) ||
	    (event->position().toPoint() - pressPos).manhattanLength() < QApplication::startDragDistance())
		return;

	const Tile tile = tiles.at(pressed);
	pressed = -1;

	auto *mime = new QMimeData();
	mime->setData(QString::fromLatin1(kDividerElementMime), elementBytes(tile.element));

	QPixmap face(QSize(kTileSize, kTileSize) * kIconScale);
	face.setDevicePixelRatio(kIconScale);
	face.fill(Qt::transparent);
	{
		QPainter painter(&face);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(palette().color(QPalette::Highlight));
		painter.setBrush(palette().color(QPalette::Base));
		painter.drawRoundedRect(QRectF(0.5, 0.5, kTileSize - 1, kTileSize - 1), 5, 5);
		drawElement(&painter, tile.element, QRectF(5, 7, kTileSize - 10, kTileSize - 14),
			    palette().color(QPalette::WindowText));
	}

	auto *drag = new QDrag(this);
	drag->setMimeData(mime);
	drag->setPixmap(face);
	drag->setHotSpot(QPoint(kTileSize / 2, kTileSize / 2));
	drag->exec(Qt::CopyAction);
}

void DividerPalette::mouseReleaseEvent(QMouseEvent *event)
{
	/* A click rather than a drag adds the tile after whatever is selected. */
	if (pressed >= 0 && tileAt(event->position().toPoint()) == pressed)
		emit activated(tiles.at(pressed).element);
	pressed = -1;
}

void DividerPalette::leaveEvent(QEvent *)
{
	hovered = -1;
	update();
}

void DividerPalette::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat(QString::fromLatin1(kDividerMoveMime))) {
		removing = true;
		update();
		event->acceptProposedAction();
	}
}

void DividerPalette::dragLeaveEvent(QDragLeaveEvent *)
{
	removing = false;
	update();
}

void DividerPalette::dropEvent(QDropEvent *event)
{
	removing = false;
	update();
	bool ok = false;
	const int index = event->mimeData()->data(QString::fromLatin1(kDividerMoveMime)).toInt(&ok);
	if (!ok)
		return;
	event->acceptProposedAction();
	emit removeRequested(index);
}

/* ---- DividerTrack --------------------------------------------------------------------------- */

DividerTrack::DividerTrack(QWidget *parent) : QWidget(parent)
{
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(moduleText("Designer.DividerTrack.Tip"));
}

void DividerTrack::setLane(const QVector<DividerElement> &laneElements, bool isSymmetric, bool laneCenterPiece)
{
	elements = laneElements;
	symmetric = isSymmetric;
	centerPiece = laneCenterPiece;
	if (selectedIndex >= elements.size())
		selectedIndex = elements.isEmpty() ? -1 : static_cast<int>(elements.size()) - 1;
	layoutChips();
	updateGeometry();
	update();
}

void DividerTrack::setSelected(int index)
{
	selectedIndex = index >= 0 && index < elements.size() ? index : -1;
	update();
}

void DividerTrack::layoutChips()
{
	chips.clear();
	axisX = -1;

	int x = kTrackMargin;
	for (int i = 0; i < elements.size(); ++i) {
		const int w = chipWidth(elements.at(i));
		chips.append(Chip{QRect(x, kTrackMargin + 12, w, kChipHeight), i, false});
		x += w + kChipGap;
	}

	if (!symmetric || elements.isEmpty())
		return;

	/*
	 * The reflection, faded, past the mirror line. A piece written last that sits on the axis is the
	 * axis: the line runs through it and it is not drawn again.
	 */
	const bool onAxis = centerPiece && !elements.last().isLine();
	if (onAxis) {
		axisX = chips.last().rect.center().x();
	} else {
		axisX = x - kChipGap / 2 + 2;
		x += 4;
	}

	const int mirrored = onAxis ? static_cast<int>(elements.size()) - 1 : static_cast<int>(elements.size());
	for (int i = mirrored - 1; i >= 0; --i) {
		const int w = chipWidth(elements.at(i));
		chips.append(Chip{QRect(x, kTrackMargin + 12, w, kChipHeight), i, true});
		x += w + kChipGap;
	}
}

QSize DividerTrack::sizeHint() const
{
	int right = kTrackMargin * 2 + 200;
	for (const Chip &chip : chips)
		right = std::max(right, chip.rect.right() + kTrackMargin);
	return QSize(right, kChipHeight + kTrackMargin * 2 + 14);
}

QSize DividerTrack::minimumSizeHint() const
{
	return QSize(200, sizeHint().height());
}

int DividerTrack::chipAt(const QPoint &pos, bool includeGhosts) const
{
	for (int i = 0; i < chips.size(); ++i) {
		if ((includeGhosts || !chips.at(i).ghost) && chips.at(i).rect.contains(pos))
			return i;
	}
	return -1;
}

bool DividerTrack::onHandle(int chip, const QPoint &pos) const
{
	if (chip < 0 || chips.at(chip).ghost || !elements.at(chips.at(chip).source).isLine())
		return false;
	return pos.x() >= chips.at(chip).rect.right() - kHandleWidth;
}

int DividerTrack::slotAt(int x) const
{
	int slot = 0;
	for (const Chip &chip : chips) {
		if (chip.ghost)
			break;
		if (x > chip.rect.center().x())
			slot = chip.source + 1;
	}
	return slot;
}

void DividerTrack::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const QColor ink = palette().color(QPalette::WindowText);
	const QColor highlight = palette().color(QPalette::Highlight);

	/* The lane drawn as a groove the chips sit in. */
	const QRectF groove = QRectF(rect()).adjusted(1, 9, -1, -1);
	painter.setPen(palette().color(QPalette::Mid));
	painter.setBrush(subtleFill(palette(), 0.05));
	painter.drawRoundedRect(groove, 7, 7);

	if (elements.isEmpty()) {
		painter.setPen(palette().color(QPalette::PlaceholderText));
		painter.drawText(groove, Qt::AlignCenter, moduleText("Designer.DividerTrack.Empty"));
	}

	QFont small = font();
	small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.82));

	for (int i = 0; i < chips.size(); ++i) {
		const Chip &chip = chips.at(i);
		const DividerElement &element = elements.at(chip.source);
		const bool selectedChip = chip.source == selectedIndex;

		painter.setOpacity(chip.ghost ? 0.35 : 1.0);
		const QRectF box = QRectF(chip.rect).adjusted(0.5, 0.5, -0.5, -0.5);
		painter.setPen(QPen(selectedChip && !chip.ghost ? highlight : palette().color(QPalette::Mid),
				    selectedChip && !chip.ghost ? 2.0 : 1.0));
		painter.setBrush(palette().color(QPalette::Base));
		painter.drawRoundedRect(box, 6, 6);

		/* A word or a piece in a color of its own carries a swatch of it. */
		if (element.ownColor) {
			painter.setPen(Qt::NoPen);
			painter.setBrush(element.color);
			painter.drawEllipse(QRectF(box.right() - 11, box.top() + 4, 7, 7));
		}

		const QRectF art =
			QRectF(box.left() + 6, box.top() + 6, box.width() - 12 - (element.isLine() ? 6 : 0), 26);
		if (chip.ghost) {
			/* The reflection is drawn reflected, which is what makes the half read as half. */
			painter.save();
			painter.translate(art.center());
			painter.scale(-1, 1);
			painter.translate(-art.center());
			drawElement(&painter, element, art, ink);
			painter.restore();
		} else {
			drawElement(&painter, element, art, ink);
		}

		painter.setFont(small);
		painter.setPen(ink);
		const QRectF label(box.left() + 3, box.bottom() - 21, box.width() - 6 - (element.isLine() ? 6 : 0), 18);
		QString name = elementName(element);
		if (element.isLine())
			name += QStringLiteral(" · %1×").arg(QString::number(element.weight, 'g', 3));
		painter.drawText(label, Qt::AlignCenter,
				 painter.fontMetrics().elidedText(name, Qt::ElideRight, qRound(label.width())));

		/* A line's right edge is a grip: dragged, it gives the line more or less of the room. */
		if (element.isLine() && !chip.ghost) {
			painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
			const qreal gx = box.right() - kHandleWidth / 2.0 - 1;
			for (int dy = -8; dy <= 8; dy += 4)
				painter.drawPoint(QPointF(gx, box.center().y() + dy));
			painter.drawLine(QPointF(box.right() - kHandleWidth, box.top() + 6),
					 QPointF(box.right() - kHandleWidth, box.bottom() - 6));
		}
	}
	painter.setOpacity(1.0);

	/* The mirror line, labeled, so the faded half reads as a reflection rather than as disabled chips. */
	if (axisX >= 0) {
		QPen pen(highlight, 1.4, Qt::DashLine);
		painter.setPen(pen);
		painter.drawLine(QPointF(axisX, 4), QPointF(axisX, height() - 4));
		painter.setFont(small);
		const QString mirror = moduleText("Designer.DividerTrack.Mirror");
		const int textWidth = painter.fontMetrics().horizontalAdvance(mirror) + 8;
		const QRectF tag(axisX - textWidth / 2.0, 0, textWidth, 14);
		painter.setPen(Qt::NoPen);
		painter.setBrush(highlight);
		painter.drawRoundedRect(tag, 3, 3);
		painter.setPen(palette().color(QPalette::HighlightedText));
		painter.drawText(tag, Qt::AlignCenter, mirror);
	}

	/* Where a drop would land. */
	if (dropSlot >= 0) {
		int x = kTrackMargin - kChipGap / 2;
		for (const Chip &chip : chips) {
			if (chip.ghost)
				break;
			if (chip.source < dropSlot)
				x = chip.rect.right() + kChipGap / 2 + 1;
		}
		painter.setPen(Qt::NoPen);
		painter.setBrush(highlight);
		painter.drawRoundedRect(QRectF(x - 2, kTrackMargin + 8, 4, kChipHeight + 8), 2, 2);
	}
}

void DividerTrack::mousePressEvent(QMouseEvent *event)
{
	setFocus(Qt::MouseFocusReason);
	if (event->button() != Qt::LeftButton)
		return;

	const QPoint pos = event->position().toPoint();
	const int chip = chipAt(pos, true);
	pressedChip = chip;
	pressPos = pos;
	resizing = false;

	if (chip < 0) {
		selectedIndex = -1;
		emit selectionChanged(-1);
		update();
		return;
	}

	if (onHandle(chip, pos)) {
		resizing = true;
		resizeStartWeight = elements.at(chips.at(chip).source).weight;
	}

	selectedIndex = chips.at(chip).source;
	emit selectionChanged(selectedIndex);
	update();
}

void DividerTrack::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint pos = event->position().toPoint();

	if (!(event->buttons() & Qt::LeftButton)) {
		const int over = chipAt(pos, false);
		setCursor(onHandle(over, pos) ? Qt::SizeHorCursor : over >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
		return;
	}

	if (pressedChip < 0)
		return;

	const int source = chips.at(pressedChip).source;

	if (resizing) {
		/* Half a step of weight per chip-width step, in tenths, so a drag lands on round numbers. */
		const double delta = (pos.x() - pressPos.x()) / static_cast<double>(kLineChipPerWeight);
		const double weight = std::clamp(std::round((resizeStartWeight + delta) * 10.0) / 10.0, 0.0, 100.0);
		if (weight != elements.at(source).weight) {
			elements[source].weight = weight;
			layoutChips();
			updateGeometry();
			update();
			emit weightChanged(source, weight);
		}
		return;
	}

	if (chips.at(pressedChip).ghost || (pos - pressPos).manhattanLength() < QApplication::startDragDistance())
		return;

	/* Picked up: dropped back on the track it moves, dropped on the palette it is taken off. */
	const QRect chipRect = chips.at(pressedChip).rect;
	auto *mime = new QMimeData();
	mime->setData(QString::fromLatin1(kDividerMoveMime), QByteArray::number(source));
	mime->setData(QString::fromLatin1(kDividerElementMime), elementBytes(elements.at(source)));

	const qreal dpr = devicePixelRatioF();
	QPixmap face(chipRect.size() * dpr);
	face.setDevicePixelRatio(dpr);
	face.fill(Qt::transparent);
	render(&face, QPoint(), QRegion(chipRect), QWidget::DrawChildren);

	auto *drag = new QDrag(this);
	drag->setMimeData(mime);
	drag->setPixmap(face);
	drag->setHotSpot(pressPos - chipRect.topLeft());
	pressedChip = -1;
	drag->exec(Qt::MoveAction);
}

void DividerTrack::mouseReleaseEvent(QMouseEvent *)
{
	pressedChip = -1;
	resizing = false;
}

void DividerTrack::keyPressEvent(QKeyEvent *event)
{
	const bool command = event->modifiers() & Qt::ControlModifier;

	switch (event->key()) {
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		if (selectedIndex >= 0)
			emit removeRequested(selectedIndex);
		return;
	case Qt::Key_Left:
	case Qt::Key_Right: {
		if (selectedIndex < 0 && !elements.isEmpty()) {
			setSelected(0);
			emit selectionChanged(0);
			return;
		}
		const int step = event->key() == Qt::Key_Left ? -1 : 1;
		const int target = selectedIndex + step;
		if (target < 0 || target >= elements.size())
			return;
		if (command) {
			/* Ctrl+arrow carries the selected chip along instead of moving the selection. */
			emit moveRequested(selectedIndex, step > 0 ? target + 1 : target);
		} else {
			setSelected(target);
			emit selectionChanged(target);
		}
		return;
	}
	case Qt::Key_D:
		if (command && selectedIndex >= 0) {
			emit duplicateRequested(selectedIndex);
			return;
		}
		break;
	default:
		break;
	}
	QWidget::keyPressEvent(event);
}

void DividerTrack::dragEnterEvent(QDragEnterEvent *event)
{
	if (event->mimeData()->hasFormat(QString::fromLatin1(kDividerElementMime))) {
		event->acceptProposedAction();
		dropSlot = slotAt(event->position().toPoint().x());
		update();
	}
}

void DividerTrack::dragMoveEvent(QDragMoveEvent *event)
{
	if (!event->mimeData()->hasFormat(QString::fromLatin1(kDividerElementMime)))
		return;
	event->acceptProposedAction();
	const int slot = slotAt(event->position().toPoint().x());
	if (slot != dropSlot) {
		dropSlot = slot;
		update();
	}
}

void DividerTrack::dragLeaveEvent(QDragLeaveEvent *)
{
	dropSlot = -1;
	update();
}

void DividerTrack::dropEvent(QDropEvent *event)
{
	const int slot = slotAt(event->position().toPoint().x());
	dropSlot = -1;
	update();

	const QMimeData *mime = event->mimeData();
	bool moving = false;
	const int from = mime->data(QString::fromLatin1(kDividerMoveMime)).toInt(&moving);

	/* A chip from this very track moves; one from anywhere else -- the palette, another lane -- is added. */
	if (moving && event->source() == this) {
		event->setDropAction(Qt::MoveAction);
		event->accept();
		emit moveRequested(from, slot);
		return;
	}

	DividerElement element;
	if (!elementFromBytes(mime->data(QString::fromLatin1(kDividerElementMime)), &element))
		return;
	event->setDropAction(Qt::CopyAction);
	event->accept();
	emit insertRequested(slot, element);
}

/* ---- DividerEditor -------------------------------------------------------------------------- */

DividerEditor::DividerEditor(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(8);

	/* --- Design and symmetry -------------------------------------------------------------- */

	{
		auto *row = new QHBoxLayout();
		auto *label = new QLabel(moduleText("Designer.DividerDesign"), this);
		design = new QComboBox(this);
		design->setToolTip(moduleText("Designer.DividerDesign.Tip"));
		const QSize iconSize(180, 30);
		design->setIconSize(iconSize);
		design->setMaxVisibleItems(12);
		label->setBuddy(design);
		row->addWidget(label);
		row->addWidget(design, 1);

		designMenuButton = new QToolButton(this);
		designMenuButton->setText(QStringLiteral("\u22ef"));
		designMenuButton->setToolTip(moduleText("Designer.DividerSaved.Menu"));
		designMenuButton->setPopupMode(QToolButton::InstantPopup);
		designMenu = new QMenu(designMenuButton);
		designMenuButton->setMenu(designMenu);
		connect(designMenu, &QMenu::aboutToShow, this, &DividerEditor::rebuildDesignMenu);
		row->addWidget(designMenuButton);

		symmetric = new QCheckBox(moduleText("Designer.DividerSymmetric"), this);
		symmetric->setToolTip(moduleText("Designer.DividerSymmetric.Tip"));
		row->addWidget(symmetric);
		layout->addLayout(row);

		connect(design, &QComboBox::activated, this, [this](int index) {
			const QString data = design->itemData(index).toString();
			Divider figure;
			if (data.startsWith(QStringLiteral("builtin:"))) {
				const QByteArray id = data.mid(8).toLatin1();
				const DividerDesign *chosen = findDividerDesign(id.constData());
				if (!chosen)
					return;
				figure = chosen->figure;
			} else if (data.startsWith(QStringLiteral("saved:"))) {
				if (!StyleLibrary::instance().findDivider(data.mid(6), &figure))
					return;
			} else {
				return;
			}
			Section holder;
			holder.divider = model;
			applyDividerDesign(holder, DividerDesign{"", "", figure});
			model = holder.divider;
			lane = 0;
			element = -1;
			edited();
		});
		connect(symmetric, &QCheckBox::toggled, this, [this](bool on) {
			if (loading)
				return;
			model.symmetric = on;
			edited();
		});
	}

	preview = new DividerPreview(this);
	layout->addWidget(preview);

	/* --- What can be added ---------------------------------------------------------------- */

	palette = new DividerPalette(this);
	layout->addWidget(palette);
	connect(palette, &DividerPalette::activated, this, [this](const DividerElement &added) {
		insertElement(element >= 0 ? element + 1 : static_cast<int>(currentLane().elements.size()), added);
	});
	connect(palette, &DividerPalette::removeRequested, this, &DividerEditor::removeElement);

	/* --- Lanes and the track -------------------------------------------------------------- */

	{
		auto *row = new QHBoxLayout();
		row->setSpacing(4);
		laneTabs = new QTabBar(this);
		laneTabs->setExpanding(false);
		laneTabs->setDrawBase(false);
		laneTabs->setToolTip(moduleText("Designer.DividerLanes.Tip"));
		row->addWidget(laneTabs, 1);

		auto *add = makeGlyphButton(this, QStringLiteral("+"), moduleText("Designer.DividerLaneAdd"));
		auto *copy = makeLabeledButton(this, moduleText("Designer.DividerLaneDuplicate"),
					       moduleText("Designer.DividerLaneDuplicate.Tip"));
		removeLaneButton = makeGlyphButton(this, QStringLiteral("−"), moduleText("Designer.DividerLaneRemove"));
		laneUpButton = makeArrowButton(this, Qt::UpArrow, moduleText("Designer.DividerLaneUp"));
		laneDownButton = makeArrowButton(this, Qt::DownArrow, moduleText("Designer.DividerLaneDown"));
		for (QToolButton *button : {add, copy, removeLaneButton, laneUpButton, laneDownButton})
			row->addWidget(button);
		layout->addLayout(row);

		connect(add, &QToolButton::clicked, this, &DividerEditor::addLane);
		connect(copy, &QToolButton::clicked, this, &DividerEditor::duplicateLane);
		connect(removeLaneButton, &QToolButton::clicked, this, &DividerEditor::removeLane);
		connect(laneUpButton, &QToolButton::clicked, this, [this] { moveLane(-1); });
		connect(laneDownButton, &QToolButton::clicked, this, [this] { moveLane(1); });
		connect(laneTabs, &QTabBar::currentChanged, this, [this](int index) {
			if (loading || index < 0)
				return;
			lane = index;
			element = -1;
			rebuildTrack();
			refreshInspector();
		});
	}

	track = new DividerTrack(this);
	auto *scroller = new QScrollArea(this);
	scroller->setWidget(track);
	scroller->setWidgetResizable(true);
	scroller->setFrameShape(QFrame::NoFrame);
	scroller->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroller->setFixedHeight(track->sizeHint().height() + 14);
	layout->addWidget(scroller);

	connect(track, &DividerTrack::selectionChanged, this, [this](int index) {
		element = index;
		refreshInspector();
	});
	connect(track, &DividerTrack::moveRequested, this, &DividerEditor::moveElement);
	connect(track, &DividerTrack::insertRequested, this, &DividerEditor::insertElement);
	connect(track, &DividerTrack::removeRequested, this, &DividerEditor::removeElement);
	connect(track, &DividerTrack::duplicateRequested, this, &DividerEditor::duplicateElement);
	connect(track, &DividerTrack::weightChanged, this, [this](int index, double weight) {
		if (index < 0 || index >= currentLane().elements.size())
			return;
		currentLane().elements[index].weight = weight;
		element = index;
		edited(false);
	});

	/* --- The selected element ------------------------------------------------------------- */

	auto *panes = new QHBoxLayout();
	panes->setSpacing(8);

	inspector = new QGroupBox(this);
	inspectorForm = new QFormLayout(inspector);
	inspectorForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	emptyHint = new QLabel(moduleText("Designer.DividerInspector.Empty"), inspector);
	emptyHint->setWordWrap(true);
	emptyHint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
	inspectorForm->addRow(emptyHint);

	pieceBox = new QComboBox(inspector);
	pieceBox->setIconSize(QSize(30, 18));
	pieceBox->setMaxVisibleItems(16);
	for (DividerShape shape : dividerShapesForRole(DividerRolePiece)) {
		if (dividerShapeIsEmpty(shape))
			continue;
		pieceBox->addItem(dividerShapeIcon(shape, false), dividerShapeText(shape),
				  QVariant::fromValue(QPoint(static_cast<int>(DividerElement::Kind::Shape),
							     static_cast<int>(shape))));
	}
	pieceBox->insertSeparator(pieceBox->count());
	pieceBox->addItem(moduleText("Designer.DividerElement.Text"),
			  QVariant::fromValue(QPoint(static_cast<int>(DividerElement::Kind::Text), 0)));
	pieceBox->addItem(moduleText("Designer.DividerElement.Image"),
			  QVariant::fromValue(QPoint(static_cast<int>(DividerElement::Kind::Image), 0)));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Piece"), pieceBox);

	lineBox = new QComboBox(inspector);
	lineBox->setIconSize(QSize(44, 18));
	for (DividerShape shape : dividerShapesForRole(DividerRoleArm))
		lineBox->addItem(dividerShapeIcon(shape, true),
				 dividerShapeUsesFile(shape) ? moduleText("Designer.DividerElement.LineSvg")
							     : dividerShapeText(shape),
				 static_cast<int>(shape));
	lineBox->addItem(moduleText("Designer.DividerElement.Space"), static_cast<int>(DividerShape::None));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Line"), lineBox);

	textEdit = new QLineEdit(inspector);
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Text"), textEdit);

	fileRow = new QWidget(inspector);
	{
		auto *row = new QHBoxLayout(fileRow);
		row->setContentsMargins(0, 0, 0, 0);
		fileEdit = new QLineEdit(fileRow);
		auto *browse = new QToolButton(fileRow);
		browse->setText(QStringLiteral("..."));
		row->addWidget(fileEdit, 1);
		row->addWidget(browse);
		connect(browse, &QToolButton::clicked, this, &DividerEditor::browseForFile);
	}
	inspectorForm->addRow(moduleText("Designer.DividerInspector.File"), fileRow);

	sizeBox = new QDoubleSpinBox(inspector);
	sizeBox->setRange(0.05, 64.0);
	sizeBox->setDecimals(2);
	sizeBox->setSingleStep(0.1);
	sizeBox->setSuffix(QStringLiteral(" ×"));
	sizeBox->setToolTip(moduleText("Designer.DividerInspector.Size.Tip"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Size"), sizeBox);

	imageHeight = new QSpinBox(inspector);
	imageHeight->setRange(1, 4096);
	imageHeight->setSuffix(QStringLiteral(" px"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.ImageHeight"), imageHeight);

	weightBox = new QDoubleSpinBox(inspector);
	weightBox->setRange(0.0, 100.0);
	weightBox->setDecimals(1);
	weightBox->setSingleStep(0.5);
	weightBox->setSuffix(QStringLiteral(" ×"));
	weightBox->setToolTip(moduleText("Designer.DividerInspector.Weight.Tip"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Weight"), weightBox);

	minLengthBox = new QSpinBox(inspector);
	minLengthBox->setRange(0, 16384);
	minLengthBox->setSuffix(QStringLiteral(" px"));
	minLengthBox->setToolTip(moduleText("Designer.DividerInspector.MinLength.Tip"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.MinLength"), minLengthBox);

	joinBox = new QCheckBox(moduleText("Designer.DividerInspector.Join"), inspector);
	joinBox->setToolTip(moduleText("Designer.DividerInspector.Join.Tip"));
	inspectorForm->addRow(QString(), joinBox);

	rotationBox = new QDoubleSpinBox(inspector);
	rotationBox->setRange(-180.0, 180.0);
	rotationBox->setWrapping(true);
	rotationBox->setDecimals(1);
	rotationBox->setSingleStep(15.0);
	rotationBox->setSuffix(QStringLiteral("°"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Rotation"), rotationBox);

	offsetBox = new QSpinBox(inspector);
	offsetBox->setRange(-4096, 4096);
	offsetBox->setSuffix(QStringLiteral(" px"));
	offsetBox->setToolTip(moduleText("Designer.DividerInspector.Offset.Tip"));
	inspectorForm->addRow(moduleText("Designer.DividerInspector.Offset"), offsetBox);

	flipBox = new QCheckBox(moduleText("Designer.DividerInspector.Flip"), inspector);
	inspectorForm->addRow(QString(), flipBox);

	colorRow = new QWidget(inspector);
	{
		auto *row = new QHBoxLayout(colorRow);
		row->setContentsMargins(0, 0, 0, 0);
		ownColorBox = new QCheckBox(moduleText("Designer.DividerInspector.OwnColor"), colorRow);
		ownColorBox->setToolTip(moduleText("Designer.DividerInspector.OwnColor.Tip"));
		colorButton = new ColorButton(colorRow);
		row->addWidget(ownColorBox);
		row->addWidget(colorButton);
		row->addStretch(1);
	}
	inspectorForm->addRow(QString(), colorRow);

	spanBox = new QCheckBox(moduleText("Designer.DividerInspector.Span"), inspector);
	spanBox->setToolTip(moduleText("Designer.DividerInspector.Span.Tip"));
	inspectorForm->addRow(QString(), spanBox);

	optionalBox = new QCheckBox(moduleText("Designer.DividerInspector.Optional"), inspector);
	optionalBox->setToolTip(moduleText("Designer.DividerInspector.Optional.Tip"));
	inspectorForm->addRow(QString(), optionalBox);

	{
		auto *row = new QHBoxLayout();
		auto *duplicate = makeLabeledButton(inspector, moduleText("Designer.DuplicatePiece"),
						    moduleText("Designer.DuplicatePiece.Tip"));
		auto *remove = makeLabeledButton(inspector, moduleText("Designer.RemovePiece"));
		row->addStretch(1);
		row->addWidget(duplicate);
		row->addWidget(remove);
		inspectorForm->addRow(row);
		connect(duplicate, &QToolButton::clicked, this, [this] { duplicateElement(element); });
		connect(remove, &QToolButton::clicked, this, [this] { removeElement(element); });
	}

	panes->addWidget(inspector, 3, Qt::AlignTop);

	/* --- The lane and the whole divider --------------------------------------------------- */

	auto *right = new QVBoxLayout();
	laneGroup = new QGroupBox(this);
	laneForm = new QFormLayout(laneGroup);

	thicknessBox = new QSpinBox(laneGroup);
	thicknessBox->setRange(1, 1024);
	thicknessBox->setSuffix(QStringLiteral(" px"));
	thicknessBox->setToolTip(moduleText("Designer.DividerThickness.Tip"));
	laneForm->addRow(moduleText("Designer.DividerThickness"), thicknessBox);

	insetBox = new QSpinBox(laneGroup);
	insetBox->setRange(0, 16384);
	insetBox->setSuffix(QStringLiteral(" px"));
	insetBox->setToolTip(moduleText("Designer.DividerLaneInset.Tip"));
	laneForm->addRow(moduleText("Designer.DividerLaneInset"), insetBox);

	laneOffsetBox = new QSpinBox(laneGroup);
	laneOffsetBox->setRange(-4096, 4096);
	laneOffsetBox->setSuffix(QStringLiteral(" px"));
	laneForm->addRow(moduleText("Designer.DividerInspector.Offset"), laneOffsetBox);

	centerPieceBox = new QCheckBox(moduleText("Designer.DividerCenterPiece"), laneGroup);
	centerPieceBox->setToolTip(moduleText("Designer.DividerCenterPiece.Tip"));
	laneForm->addRow(QString(), centerPieceBox);
	right->addWidget(laneGroup);

	auto *spacing = new QGroupBox(moduleText("Designer.DividerSpacing"), this);
	spacingForm = new QFormLayout(spacing);

	lengthBox = new QSpinBox(spacing);
	lengthBox->setRange(1, 100);
	lengthBox->setSuffix(QStringLiteral(" %"));
	lengthBox->setToolTip(moduleText("Designer.DividerLength.Tip"));
	spacingForm->addRow(moduleText("Designer.DividerLength"), lengthBox);

	alignBox = new QComboBox(spacing);
	addAlignment(alignBox);
	spacingForm->addRow(moduleText("Designer.DividerAlign"), alignBox);

	lineGapBox = new QSpinBox(spacing);
	lineGapBox->setRange(-static_cast<int>(kMaxDividerJoin), 4096);
	lineGapBox->setSuffix(QStringLiteral(" px"));
	lineGapBox->setToolTip(moduleText("Designer.DividerGap.Tip"));
	spacingForm->addRow(moduleText("Designer.DividerGap"), lineGapBox);

	pieceGapBox = new QSpinBox(spacing);
	pieceGapBox->setRange(-static_cast<int>(kMaxDividerJoin), 4096);
	pieceGapBox->setSuffix(QStringLiteral(" px"));
	pieceGapBox->setToolTip(moduleText("Designer.DividerPieceGap.Tip"));
	spacingForm->addRow(moduleText("Designer.DividerPieceGap"), pieceGapBox);

	laneGapBox = new QSpinBox(spacing);
	laneGapBox->setRange(0, 4096);
	laneGapBox->setSuffix(QStringLiteral(" px"));
	spacingForm->addRow(moduleText("Designer.DividerLaneGap"), laneGapBox);

	tintBox = new QCheckBox(moduleText("Designer.DividerTint"), spacing);
	spacingForm->addRow(QString(), tintBox);
	right->addWidget(spacing);

	/*
	 * The draw-on. Where it starts and how long it takes are shares of the frame rather than
	 * seconds, because what is being placed is a moment in the picture -- "as it comes in at the
	 * bottom", "by the time it is a quarter of the way up" -- which holds at any scroll speed.
	 */
	auto *revealGroup = new QGroupBox(moduleText("Designer.DividerReveal"), this);
	revealGroup->setToolTip(moduleText("Designer.DividerReveal.Tip"));
	revealForm = new QFormLayout(revealGroup);
	{
		auto *row = new QWidget(revealGroup);
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		revealBox = new QComboBox(row);
		revealBox->addItem(moduleText("Designer.DividerReveal.None"), static_cast<int>(DividerReveal::None));
		revealBox->addItem(moduleText("Designer.DividerReveal.Center"),
				   static_cast<int>(DividerReveal::FromCenter));
		revealBox->addItem(moduleText("Designer.DividerReveal.Left"),
				   static_cast<int>(DividerReveal::FromLeft));
		revealBox->addItem(moduleText("Designer.DividerReveal.Right"),
				   static_cast<int>(DividerReveal::FromRight));
		revealPlay = makeGlyphButton(row, QStringLiteral("▶"), moduleText("Designer.DividerReveal.Play"));
		rowLayout->addWidget(revealBox, 1);
		rowLayout->addWidget(revealPlay);
		revealForm->addRow(moduleText("Designer.DividerReveal.Mode"), row);
	}
	revealStartBox = new QSpinBox(revealGroup);
	revealStartBox->setRange(0, 150);
	revealStartBox->setSuffix(QStringLiteral(" %"));
	revealStartBox->setToolTip(moduleText("Designer.DividerReveal.Start.Tip"));
	revealForm->addRow(moduleText("Designer.DividerReveal.Start"), revealStartBox);
	revealLengthBox = new QSpinBox(revealGroup);
	revealLengthBox->setRange(0, 200);
	revealLengthBox->setSuffix(QStringLiteral(" %"));
	revealLengthBox->setToolTip(moduleText("Designer.DividerReveal.Length.Tip"));
	revealForm->addRow(moduleText("Designer.DividerReveal.Length"), revealLengthBox);
	right->addWidget(revealGroup);
	connect(revealPlay, &QToolButton::clicked, this, [this] { preview->playReveal(model.reveal); });
	right->addStretch(1);

	panes->addLayout(right, 2);
	layout->addLayout(panes);
	layout->addStretch(1);

	/* --- Every field writes straight into the model ---------------------------------------- */

	const auto onElement = [this](auto apply) {
		return [this, apply](auto &&...) {
			if (loading)
				return;
			DividerElement *selected = currentElement();
			if (!selected)
				return;
			apply(*selected);
			edited();
		};
	};

	connect(pieceBox, &QComboBox::currentIndexChanged, this, onElement([this](DividerElement &e) {
			const QPoint data = pieceBox->currentData().toPoint();
			e.kind = static_cast<DividerElement::Kind>(data.x());
			if (e.kind == DividerElement::Kind::Shape)
				e.shape = static_cast<DividerShape>(data.y());
			/* A word given its kind with nothing to say would draw nothing and look broken. */
			if (e.kind == DividerElement::Kind::Text && e.text.isEmpty())
				e.text = moduleText("Designer.DividerElement.DefaultText");
		}));
	connect(lineBox, &QComboBox::currentIndexChanged, this, onElement([this](DividerElement &e) {
			e.lineShape = static_cast<DividerShape>(lineBox->currentData().toInt());
		}));
	connect(textEdit, &QLineEdit::textChanged, this,
		onElement([this](DividerElement &e) { e.text = textEdit->text(); }));
	connect(fileEdit, &QLineEdit::textChanged, this, onElement([this](DividerElement &e) {
			if (e.kind == DividerElement::Kind::Image)
				e.logo.path = fileEdit->text();
			else if (e.isLine())
				e.lineSvgPath = fileEdit->text();
			else
				e.svgPath = fileEdit->text();
		}));
	connect(sizeBox, &QDoubleSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.scale = sizeBox->value(); }));
	connect(imageHeight, &QSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.logo.maxHeight = imageHeight->value(); }));
	connect(weightBox, &QDoubleSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.weight = weightBox->value(); }));
	connect(minLengthBox, &QSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.minLength = minLengthBox->value(); }));
	connect(joinBox, &QCheckBox::toggled, this,
		onElement([this](DividerElement &e) { e.join = joinBox->isChecked(); }));
	connect(rotationBox, &QDoubleSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.rotation = rotationBox->value(); }));
	connect(offsetBox, &QSpinBox::valueChanged, this,
		onElement([this](DividerElement &e) { e.offsetY = offsetBox->value(); }));
	connect(flipBox, &QCheckBox::toggled, this,
		onElement([this](DividerElement &e) { e.flip = flipBox->isChecked(); }));
	connect(ownColorBox, &QCheckBox::toggled, this,
		onElement([this](DividerElement &e) { e.ownColor = ownColorBox->isChecked(); }));
	connect(colorButton, &ColorButton::colorChanged, this, onElement([this](DividerElement &e) {
			e.color = colorButton->color();
			/* Picking a color is asking for it. */
			e.ownColor = true;
		}));
	connect(spanBox, &QCheckBox::toggled, this,
		onElement([this](DividerElement &e) { e.spanLanes = spanBox->isChecked(); }));
	connect(optionalBox, &QCheckBox::toggled, this,
		onElement([this](DividerElement &e) { e.optional = optionalBox->isChecked(); }));

	const auto onLane = [this](auto apply) {
		return [this, apply](auto &&...) {
			if (loading)
				return;
			apply(currentLane());
			edited();
		};
	};
	connect(thicknessBox, &QSpinBox::valueChanged, this,
		onLane([this](DividerLane &l) { l.thickness = thicknessBox->value(); }));
	connect(insetBox, &QSpinBox::valueChanged, this,
		onLane([this](DividerLane &l) { l.inset = insetBox->value(); }));
	connect(laneOffsetBox, &QSpinBox::valueChanged, this,
		onLane([this](DividerLane &l) { l.offsetY = laneOffsetBox->value(); }));
	connect(centerPieceBox, &QCheckBox::toggled, this,
		onLane([this](DividerLane &l) { l.centerPiece = centerPieceBox->isChecked(); }));

	const auto onDivider = [this](auto apply) {
		return [this, apply](auto &&...) {
			if (loading)
				return;
			apply(model);
			edited(false);
		};
	};
	connect(lengthBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.length = lengthBox->value() / 100.0; }));
	connect(alignBox, &QComboBox::currentIndexChanged, this,
		onDivider([this](Divider &d) { d.align = static_cast<HAlign>(alignBox->currentData().toInt()); }));
	connect(lineGapBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.lineGap = lineGapBox->value(); }));
	connect(pieceGapBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.pieceGap = pieceGapBox->value(); }));
	connect(laneGapBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.laneGap = laneGapBox->value(); }));
	connect(tintBox, &QCheckBox::toggled, this, onDivider([this](Divider &d) { d.tint = tintBox->isChecked(); }));
	connect(revealBox, &QComboBox::currentIndexChanged, this, [this] {
		if (loading)
			return;
		model.reveal = static_cast<DividerReveal>(revealBox->currentData().toInt());
		edited(false);
		/* Picking a reveal plays it, so the choice is seen rather than imagined. */
		preview->playReveal(model.reveal);
	});
	connect(revealStartBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.revealStart = revealStartBox->value() / 100.0; }));
	connect(revealLengthBox, &QSpinBox::valueChanged, this,
		onDivider([this](Divider &d) { d.revealLength = revealLengthBox->value() / 100.0; }));

	previewTimer = new QTimer(this);
	previewTimer->setSingleShot(true);
	previewTimer->setInterval(60);
	connect(previewTimer, &QTimer::timeout, this, &DividerEditor::renderPreview);

	setDivider(plainDivider());
}

DividerLane &DividerEditor::currentLane()
{
	if (model.lanes.isEmpty())
		model.lanes.append(DividerLane{});
	lane = std::clamp(lane, 0, static_cast<int>(model.lanes.size()) - 1);
	return model.lanes[lane];
}

DividerElement *DividerEditor::currentElement()
{
	DividerLane &current = currentLane();
	if (element < 0 || element >= current.elements.size())
		return nullptr;
	return &current.elements[element];
}

void DividerEditor::setDivider(const Divider &divider)
{
	model = divider;
	if (model.lanes.isEmpty())
		model.lanes.append(DividerLane{});
	lane = std::clamp(lane, 0, static_cast<int>(model.lanes.size()) - 1);
	if (element >= model.lanes.at(lane).elements.size())
		element = -1;

	rebuildLaneTabs();
	rebuildTrack();
	refreshInspector();
	refreshDesign();
	refreshPreview();
}

void DividerEditor::setPreviewSource(std::function<Section()> source)
{
	previewSource = std::move(source);
	refreshPreview();
}

void DividerEditor::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	refreshPreview();
}

void DividerEditor::refreshPreview()
{
	if (previewTimer)
		previewTimer->start();
}

void DividerEditor::selectElement(int laneIndex, int index)
{
	lane = std::clamp(laneIndex, 0, static_cast<int>(model.lanes.size()) - 1);
	element = index >= 0 && index < model.lanes.at(lane).elements.size() ? index : -1;
	rebuildLaneTabs();
	rebuildTrack();
	refreshInspector();
}

void DividerEditor::edited(bool rebuild)
{
	if (rebuild)
		rebuildTrack();
	rebuildLaneTabs();
	refreshInspector();
	refreshDesign();
	refreshPreview();
	emit changed();
}

void DividerEditor::rebuildLaneTabs()
{
	const bool was = loading;
	loading = true;
	const QSignalBlocker blocker(laneTabs);
	while (laneTabs->count() > model.lanes.size())
		laneTabs->removeTab(laneTabs->count() - 1);
	while (laneTabs->count() < model.lanes.size())
		laneTabs->addTab(QString());
	for (int i = 0; i < model.lanes.size(); ++i)
		laneTabs->setTabText(i, moduleText("Designer.DividerLane").arg(i + 1));
	laneTabs->setCurrentIndex(lane);
	removeLaneButton->setEnabled(model.lanes.size() > 1);
	laneUpButton->setEnabled(lane > 0);
	laneDownButton->setEnabled(lane < model.lanes.size() - 1);
	loading = was;
}

void DividerEditor::rebuildTrack()
{
	const DividerLane &current = currentLane();
	track->setLane(current.elements, model.symmetric, current.centerPiece);
	track->setSelected(element);
}

void DividerEditor::refreshInspector()
{
	const bool was = loading;
	loading = true;

	const DividerElement *selected = currentElement();
	const bool has = selected != nullptr;
	const DividerElement e = has ? *selected : DividerElement{};
	const bool isLine = has && e.isLine();
	const bool isPiece = has && !e.isLine();
	const bool isShape = has && e.kind == DividerElement::Kind::Shape;
	const bool isText = has && e.kind == DividerElement::Kind::Text;
	const bool isImage = has && e.kind == DividerElement::Kind::Image;
	const bool usesFile = isImage || (isShape && dividerShapeUsesFile(e.shape)) ||
			      (isLine && dividerShapeUsesFile(e.lineShape));

	inspector->setTitle(has ? moduleText("Designer.DividerInspector.Title").arg(elementName(e))
				: moduleText("Designer.DividerInspector.None"));

	inspectorForm->setRowVisible(emptyHint, !has);
	inspectorForm->setRowVisible(pieceBox, isPiece);
	inspectorForm->setRowVisible(lineBox, isLine);
	inspectorForm->setRowVisible(textEdit, isText);
	inspectorForm->setRowVisible(fileRow, usesFile);
	inspectorForm->setRowVisible(sizeBox, isShape || isLine);
	inspectorForm->setRowVisible(imageHeight, isImage);
	inspectorForm->setRowVisible(weightBox, isLine);
	inspectorForm->setRowVisible(minLengthBox, isLine);
	inspectorForm->setRowVisible(joinBox, isLine);
	inspectorForm->setRowVisible(rotationBox, isPiece);
	inspectorForm->setRowVisible(offsetBox, has);
	inspectorForm->setRowVisible(flipBox, has && !isText);
	inspectorForm->setRowVisible(colorRow, has && !isImage);
	inspectorForm->setRowVisible(spanBox, isPiece && model.lanes.size() > 1);
	inspectorForm->setRowVisible(optionalBox, isPiece);
	for (int row = 0; row < inspectorForm->rowCount(); ++row) {
		QLayoutItem *item = inspectorForm->itemAt(row, QFormLayout::SpanningRole);
		if (item && item->layout())
			inspectorForm->setRowVisible(row, has);
	}

	if (has) {
		if (isPiece) {
			const QPoint key(static_cast<int>(e.kind), isShape ? static_cast<int>(e.shape) : 0);
			selectData(pieceBox, QVariant::fromValue(key));
		}
		selectData(lineBox, static_cast<int>(e.lineShape));
		if (textEdit->text() != e.text)
			textEdit->setText(e.text);
		const QString file = isImage ? e.logo.path : isLine ? e.lineSvgPath : e.svgPath;
		if (fileEdit->text() != file)
			fileEdit->setText(file);
		sizeBox->setValue(e.scale);
		sizeBox->setToolTip(moduleText(isLine ? "Designer.DividerInspector.LineSize.Tip"
						      : "Designer.DividerInspector.Size.Tip"));
		imageHeight->setValue(e.logo.maxHeight);
		weightBox->setValue(e.weight);
		minLengthBox->setValue(qRound(e.minLength));
		joinBox->setChecked(e.join);
		rotationBox->setValue(e.rotation > 180.0 ? e.rotation - 360.0 : e.rotation);
		offsetBox->setValue(qRound(e.offsetY));
		flipBox->setChecked(e.flip);
		ownColorBox->setChecked(e.ownColor);
		colorButton->setColor(e.color);
		spanBox->setChecked(e.spanLanes);
		optionalBox->setChecked(e.optional);
	}

	const DividerLane &current = currentLane();
	laneGroup->setTitle(moduleText("Designer.DividerLaneSettings").arg(lane + 1));
	thicknessBox->setValue(std::max(1, qRound(current.thickness)));
	insetBox->setValue(qRound(current.inset));
	laneOffsetBox->setValue(qRound(current.offsetY));
	centerPieceBox->setChecked(current.centerPiece);
	laneForm->setRowVisible(centerPieceBox,
				model.symmetric && !current.elements.isEmpty() && !current.elements.last().isLine());

	symmetric->setChecked(model.symmetric);
	lengthBox->setValue(std::clamp(qRound(model.length * 100.0), 1, 100));
	selectData(alignBox, static_cast<int>(model.align));
	alignBox->setEnabled(model.length < 1.0);
	lineGapBox->setValue(qRound(model.lineGap));
	pieceGapBox->setValue(qRound(model.pieceGap));
	laneGapBox->setValue(qRound(model.laneGap));
	spacingForm->setRowVisible(laneGapBox, model.lanes.size() > 1);

	bool anyFile = false;
	for (const DividerLane &each : model.lanes) {
		for (const DividerElement &item : each.elements) {
			anyFile = anyFile ||
				  (item.kind == DividerElement::Kind::Shape && dividerShapeUsesFile(item.shape)) ||
				  (item.isLine() && dividerShapeUsesFile(item.lineShape));
		}
	}
	tintBox->setChecked(model.tint);
	spacingForm->setRowVisible(tintBox, anyFile);

	selectData(revealBox, static_cast<int>(model.reveal));
	revealStartBox->setValue(qRound(model.revealStart * 100.0));
	revealLengthBox->setValue(qRound(model.revealLength * 100.0));
	const bool revealing = model.reveal != DividerReveal::None;
	revealForm->setRowVisible(revealStartBox, revealing);
	revealForm->setRowVisible(revealLengthBox, revealing);
	revealPlay->setEnabled(revealing);

	loading = was;
}

void DividerEditor::rebuildDesignPicker()
{
	const QSignalBlocker blocker(design);
	const QSize iconSize = design->iconSize();
	pickerSerial = StyleLibrary::instance().serial();

	design->clear();
	design->addItem(moduleText("Designer.DividerDesign.Own"), QString());
	for (const DividerDesign &each : dividerDesigns())
		design->addItem(dividerFigureIcon(each.figure, iconSize), dividerDesignText(each),
				QStringLiteral("builtin:") + QString::fromLatin1(each.id));

	/*
	 * The dividers saved on this machine, under a heading of their own so a saved one is never
	 * mistaken for the built-in design it may have started from.
	 */
	const QVector<DividerPreset> saved = StyleLibrary::instance().dividers();
	if (!saved.isEmpty()) {
		design->insertSeparator(design->count());
		design->addItem(moduleText("Designer.DividerSaved.Heading"), QStringLiteral("heading"));
		/* A heading, not a choice: disabled through the item's own flags. */
		design->setItemData(design->count() - 1, 0, Qt::UserRole - 1);
		for (const DividerPreset &preset : saved)
			design->addItem(dividerFigureIcon(preset.figure, iconSize), preset.name,
					QStringLiteral("saved:") + preset.name);
	}
}

void DividerEditor::refreshDesign()
{
	if (StyleLibrary::instance().serial() != pickerSerial)
		rebuildDesignPicker();

	/* A built-in design first, then a saved one: the same figure is named after what ships with the plugin. */
	QString key;
	Section probe;
	probe.divider = model;
	if (const DividerDesign *matched = matchingDividerDesign(probe)) {
		key = QStringLiteral("builtin:") + QString::fromLatin1(matched->id);
	} else {
		for (const DividerPreset &preset : StyleLibrary::instance().dividers()) {
			if (sameDividerFigure(model, preset.figure)) {
				key = QStringLiteral("saved:") + preset.name;
				break;
			}
		}
	}

	const QSignalBlocker blocker(design);
	const int index = design->findData(key);
	design->setCurrentIndex(index >= 0 ? index : 0);
}

void DividerEditor::rebuildDesignMenu()
{
	designMenu->clear();
	designMenu->addAction(moduleText("Designer.DividerSaved.Save"), this, &DividerEditor::saveDesign);

	const QVector<DividerPreset> saved = StyleLibrary::instance().dividers();
	QMenu *rename = designMenu->addMenu(moduleText("Designer.DividerSaved.Rename"));
	QMenu *remove = designMenu->addMenu(moduleText("Designer.DividerSaved.Delete"));
	rename->setEnabled(!saved.isEmpty());
	remove->setEnabled(!saved.isEmpty());
	for (const DividerPreset &preset : saved) {
		const QString name = preset.name;
		rename->addAction(name, this, [this, name] { renameSavedDesign(name); });
		remove->addAction(name, this, [this, name] { deleteSavedDesign(name); });
	}
}

void DividerEditor::saveDesign()
{
	/* Offer the name of the saved divider this one still is, so saving over it again is one Enter. */
	QString suggestion;
	const QString current = design->currentData().toString();
	if (current.startsWith(QStringLiteral("saved:")))
		suggestion = current.mid(6);

	bool ok = false;
	const QString name = QInputDialog::getText(this, moduleText("Designer.DividerSaved.Save.Title"),
						   moduleText("Designer.DividerSaved.Save.Prompt"), QLineEdit::Normal,
						   suggestion, &ok)
				     .trimmed();
	if (!ok || name.isEmpty())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	if (library.containsDivider(name) &&
	    QMessageBox::question(this, moduleText("Designer.DividerSaved.Save.Title"),
				  moduleText("Designer.DividerSaved.Replace").arg(name)) != QMessageBox::Yes)
		return;

	library.setDivider(name, model);
	refreshDesign();
}

void DividerEditor::renameSavedDesign(const QString &name)
{
	bool ok = false;
	const QString to = QInputDialog::getText(this, moduleText("Designer.DividerSaved.Rename.Title"),
						 moduleText("Designer.DividerSaved.Save.Prompt"), QLineEdit::Normal,
						 name, &ok)
				   .trimmed();
	if (!ok || to.isEmpty() || to == name)
		return;

	StyleLibrary &library = StyleLibrary::instance();
	if (library.containsDivider(to)) {
		QMessageBox::information(this, moduleText("Designer.DividerSaved.Rename.Title"),
					 moduleText("Designer.DividerSaved.Taken").arg(to));
		return;
	}
	library.renameDivider(name, to);
	refreshDesign();
}

void DividerEditor::deleteSavedDesign(const QString &name)
{
	if (QMessageBox::question(this, moduleText("Designer.DividerSaved.Delete.Title"),
				  moduleText("Designer.DividerSaved.Delete.Confirm").arg(name)) != QMessageBox::Yes)
		return;
	StyleLibrary::instance().removeDivider(name);
	refreshDesign();
}

void DividerEditor::renderPreview()
{
	if (!previewSource || !isVisible()) {
		if (!previewSource)
			preview->setImage(QImage());
		return;
	}

	Section section = previewSource();
	section.type = SectionType::SectionDivider;
	section.divider = model;
	section.visible = true;
	section.paddingTop = 6;
	section.paddingBottom = 6;
	section.backgrounds.clear();

	Document document;
	document.sections = {section};
	document.bundleFonts = false;

	static LogoCache logos;
	const RollRenderer renderer(&logos);
	QImage drawn = renderer.paintReference(document);
	if (drawn.isNull()) {
		preview->setImage(QImage());
		return;
	}

	/* Cropped to what was drawn, so the preview shows the divider rather than the empty canvas around it. */
	int left = drawn.width(), right = -1, top = drawn.height(), bottom = -1;
	for (int y = 0; y < drawn.height(); ++y) {
		const QRgb *line = reinterpret_cast<const QRgb *>(drawn.constScanLine(y));
		for (int x = 0; x < drawn.width(); ++x) {
			if (qAlpha(line[x]) > 8) {
				left = std::min(left, x);
				right = std::max(right, x);
				top = std::min(top, y);
				bottom = std::max(bottom, y);
			}
		}
	}
	if (right < left) {
		preview->setImage(QImage());
		return;
	}
	preview->setImage(drawn.copy(QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-4, -4, 4, 4)));
}

void DividerEditor::insertElement(int at, const DividerElement &added)
{
	DividerLane &current = currentLane();
	if (current.elements.size() >= kMaxDividerElements)
		return;
	at = std::clamp(at, 0, static_cast<int>(current.elements.size()));
	current.elements.insert(at, added);
	element = at;
	edited();
	track->setFocus();
}

void DividerEditor::removeElement(int index)
{
	DividerLane &current = currentLane();
	if (index < 0 || index >= current.elements.size())
		return;
	current.elements.removeAt(index);
	element = current.elements.isEmpty() ? -1 : std::min(index, static_cast<int>(current.elements.size()) - 1);
	edited();
}

void DividerEditor::duplicateElement(int index)
{
	DividerLane &current = currentLane();
	if (index < 0 || index >= current.elements.size() || current.elements.size() >= kMaxDividerElements)
		return;
	current.elements.insert(index + 1, current.elements.at(index));
	element = index + 1;
	edited();
}

void DividerEditor::moveElement(int from, int to)
{
	DividerLane &current = currentLane();
	if (from < 0 || from >= current.elements.size())
		return;
	to = std::clamp(to, 0, static_cast<int>(current.elements.size()));
	/* `to` is a slot counted before the move, so one past the moved element is where it already is. */
	const int target = to > from ? to - 1 : to;
	if (target == from)
		return;
	current.elements.move(from, target);
	element = target;
	edited();
}

void DividerEditor::addLane()
{
	if (model.lanes.size() >= kMaxDividerLanes)
		return;
	DividerLane added;
	added.thickness = currentLane().thickness;
	DividerElement line;
	line.kind = DividerElement::Kind::Line;
	added.elements = {line};
	model.lanes.insert(lane + 1, added);
	lane += 1;
	element = 0;
	edited();
}

void DividerEditor::duplicateLane()
{
	if (model.lanes.size() >= kMaxDividerLanes)
		return;
	model.lanes.insert(lane + 1, currentLane());
	lane += 1;
	edited();
}

void DividerEditor::removeLane()
{
	if (model.lanes.size() <= 1)
		return;
	model.lanes.removeAt(lane);
	lane = std::min(lane, static_cast<int>(model.lanes.size()) - 1);
	element = -1;
	edited();
}

void DividerEditor::moveLane(int delta)
{
	const int target = lane + delta;
	if (target < 0 || target >= model.lanes.size())
		return;
	model.lanes.swapItemsAt(lane, target);
	lane = target;
	edited();
}

void DividerEditor::browseForFile()
{
	DividerElement *selected = currentElement();
	if (!selected)
		return;
	const bool image = selected->kind == DividerElement::Kind::Image;
	const QString filter =
		image ? moduleText("Designer.LogoFilter") + QStringLiteral(" (%1)").arg(imageLogoPatterns())
		      : moduleText("Designer.BridgeSvgFilter") + QStringLiteral(" (*.svg *.svgz)");
	const QString path = QFileDialog::getOpenFileName(
		this, moduleText(image ? "Designer.ChooseLogo" : "Designer.ChooseDividerSvg"), fileEdit->text(),
		filter);
	if (!path.isEmpty())
		fileEdit->setText(path);
}

} // namespace closingtime
