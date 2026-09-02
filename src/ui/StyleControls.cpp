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

#include "ui/StyleControls.hpp"

#include <obs-module.h>

#include <QColorDialog>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPainter>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "render/StripRenderer.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Checkerboard cell size, in widget pixels, used behind a gradient carrying alpha. */
constexpr int kCheckerSize = 8;

/* Columns of the stop table. */
enum StopColumn { StopPosition = 0, StopColor = 1, StopColumnCount = 2 };

/*
 * Tallest the stop table is allowed to get before it starts scrolling, in pixels. It sizes
 * itself to the stops it actually holds up to this, so a sweep built from six or seven stops is
 * read down the list rather than through a scroll bar four rows tall.
 */
constexpr int kMaxStopTableHeight = 320;

/* Breathing room around a stop's own controls, so a row is not the exact height of a spin box. */
constexpr int kStopRowPadding = 4;

/* Slack added to the position column, so the spin box is not drawn hard against the grid line. */
constexpr int kStopColumnPadding = 8;

/* Checkerboard cell size behind a swatch carrying alpha. */
constexpr int kSwatchCheckerSize = 5;

/* Set on each cell widget so a focus event can say which stop it belongs to. */
constexpr const char *kStopRowProperty = "closingtime_stop_row";

} // namespace

/* --------------------------------------------------------------------- ColorButton */

ColorButton::ColorButton(QWidget *parent) : QPushButton(parent)
{
	refresh();
	connect(this, &QPushButton::clicked, this, &ColorButton::pick);
}

void ColorButton::setColor(const QColor &color)
{
	current = color;
	refresh();
}

void ColorButton::pick()
{
	/*
	 * Parented to the window rather than to this button. A dialog is a child of whatever it is
	 * parented to for style purposes as well as for stacking, so hanging one off a widget hands
	 * it that widget's styling -- which is how the color dialog came to be painted in the very
	 * color it was being opened to change.
	 */
	const QColor picked = QColorDialog::getColor(current, window(), dialogTitle, QColorDialog::ShowAlphaChannel);
	if (!picked.isValid() || picked == current)
		return;

	current = picked;
	refresh();

	emit colorChanged();
}

void ColorButton::refresh()
{
	setText(current.name(QColor::HexArgb));
	update();
}

/*
 * The swatch is painted rather than set as a stylesheet background.
 *
 * A stylesheet does not stop at the widget it is set on: it reaches everything beneath that
 * widget in the object tree, and a dialog parented to a widget is beneath it for styling as much
 * as for stacking. So the color dialog this button opens was being painted in the very color it
 * had been opened to change -- and so was every control inside it. Painting the swatch here, and
 * hanging the dialog off the window instead, leaves nothing behind to cascade.
 *
 * It also stays crisp at whatever DPI OBS is running at, which was the reason the stylesheet was
 * preferred to an icon in the first place: a painted rectangle has no fixed resolution to lose.
 */
void ColorButton::paintEvent(QPaintEvent *event)
{
	/* The frame, the focus ring and the pressed state, all as the running theme draws them. */
	QPushButton::paintEvent(event);

	QStyleOptionButton option;
	initStyleOption(&option);

	const QRect swatch = style()->subElementRect(QStyle::SE_PushButtonContents, &option, this);
	if (swatch.isEmpty())
		return;

	QPainter painter(this);

	/* Colors here carry alpha, so the swatch needs something to be transparent against. */
	if (current.alpha() < 255) {
		painter.fillRect(swatch, QColor(48, 48, 48));
		for (int y = 0; y * kSwatchCheckerSize < swatch.height(); ++y) {
			for (int x = 0; x * kSwatchCheckerSize < swatch.width(); ++x) {
				if ((x + y) % 2 == 0)
					continue;

				painter.fillRect(QRect(swatch.left() + x * kSwatchCheckerSize,
						       swatch.top() + y * kSwatchCheckerSize, kSwatchCheckerSize,
						       kSwatchCheckerSize)
							 .intersected(swatch),
						 QColor(62, 62, 62));
			}
		}
	}

	painter.fillRect(swatch, current);

	/*
	 * The hex code goes back on top of the fill, because the theme's own text color is no
	 * longer readable against it. Composited over the checkerboard, a color with alpha reads
	 * lighter than its own lightness says, so the test is made against what is actually shown.
	 */
	const QColor shown = QColor::fromRgbF(current.redF() * current.alphaF() + 0.22 * (1.0 - current.alphaF()),
					      current.greenF() * current.alphaF() + 0.22 * (1.0 - current.alphaF()),
					      current.blueF() * current.alphaF() + 0.22 * (1.0 - current.alphaF()));
	painter.setPen(shown.lightness() > 127 ? Qt::black : Qt::white);
	painter.drawText(swatch, Qt::AlignCenter, text());
}

/* -------------------------------------------------------------------- GradientPreview */

GradientPreview::GradientPreview(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(28);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void GradientPreview::setSpec(TextFill newFill, const GradientSpec &newSpec)
{
	fill = newFill;
	spec = newSpec;
	update();
}

void GradientPreview::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const QRect box = rect().adjusted(0, 0, -1, -1);

	/* Stops carry alpha, so the checkerboard is what makes a transparent one readable. */
	painter.fillRect(box, QColor(48, 48, 48));
	for (int y = 0; y <= box.height() / kCheckerSize; ++y) {
		for (int x = 0; x <= box.width() / kCheckerSize; ++x) {
			if ((x + y) % 2 == 0)
				continue;
			painter.fillRect(QRect(box.left() + x * kCheckerSize, box.top() + y * kCheckerSize,
					       kCheckerSize, kCheckerSize)
						 .intersected(box),
					 QColor(62, 62, 62));
		}
	}

	/*
	 * Painted through the renderer's own brush, so what the swatch shows is the mapping the
	 * roll will use -- angle and all -- rather than a second guess at it.
	 */
	TextStyle style;
	style.fill = fill;
	style.gradient = spec;

	painter.fillRect(box, textFillBrush(style, QRectF(box)));

	painter.setPen(QColor(0, 0, 0, 120));
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(box);
}

/* --------------------------------------------------------------------- GradientEditor */

GradientEditor::GradientEditor(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	preview = new GradientPreview(this);
	layout->addWidget(preview);

	angleRow = new QWidget(this);
	auto *angleLayout = new QHBoxLayout(angleRow);
	angleLayout->setContentsMargins(0, 0, 0, 0);
	angleLayout->addWidget(new QLabel(moduleText("Designer.GradientAngle"), angleRow));
	angle = new QSpinBox(angleRow);
	angle->setRange(0, 359);
	angle->setWrapping(true);
	angle->setSuffix(QStringLiteral("°"));
	angle->setToolTip(moduleText("Designer.GradientAngle.Tip"));
	angleLayout->addWidget(angle, 1);
	layout->addWidget(angleRow);

	table = new QTableWidget(this);
	table->setColumnCount(StopColumnCount);
	table->setHorizontalHeaderLabels(
		{moduleText("Designer.GradientStop.Position"), moduleText("Designer.GradientStop.Color")});
	table->verticalHeader()->setVisible(false);
	table->horizontalHeader()->setStretchLastSection(true);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	/*
	 * Two columns, one of them fixed to what a percentage needs and the other taking the rest:
	 * there is never anything off to the side to scroll to, and a bar along the bottom would
	 * only eat into the height the rows are sized against.
	 */
	table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	/* Height follows the stop count -- see updateTableHeight -- rather than being fixed here. */
	layout->addWidget(table);

	auto *buttons = new QHBoxLayout();
	auto *addButton = new QPushButton(moduleText("Designer.GradientStop.Add"), this);
	removeButton = new QPushButton(moduleText("Designer.GradientStop.Remove"), this);
	buttons->addWidget(addButton);
	buttons->addWidget(removeButton);
	buttons->addStretch();
	layout->addLayout(buttons);

	connect(angle, &QSpinBox::valueChanged, this, [this] {
		if (loading)
			return;
		current.angle = angle->value();
		notifyEdited();
	});
	connect(addButton, &QPushButton::clicked, this, &GradientEditor::addStop);
	connect(removeButton, &QPushButton::clicked, this, &GradientEditor::removeSelectedStop);
	connect(table, &QTableWidget::itemSelectionChanged, this, [this] {
		/*
		 * hasSelection() rather than selectedItems(), which is always empty here: every
		 * cell holds a widget, so the table carries no QTableWidgetItems to return.
		 * The last two stops are what a gradient needs to be one, so they stay put.
		 */
		removeButton->setEnabled(current.stops.size() > 2 && table->selectionModel()->hasSelection());
	});

	setGradient(current);
}

void GradientEditor::setGradient(const GradientSpec &spec)
{
	loading = true;

	current = spec;
	sortStops();
	angle->setValue(((qRound(current.angle) % 360) + 360) % 360);
	rebuildTable();

	loading = false;
}

/*
 * Only called where the row order is allowed to change under the user -- loading a style and
 * adding a stop. Sorting on every position edit would renumber the rows mid-drag as a stop
 * passed its neighbor, which moves the spin box out from under the cursor.
 */
void GradientEditor::sortStops()
{
	std::stable_sort(current.stops.begin(), current.stops.end(),
			 [](const GradientStop &a, const GradientStop &b) { return a.position < b.position; });
}

void GradientEditor::setFill(TextFill newFill)
{
	fill = newFill;
	angleRow->setVisible(fill == TextFill::LinearGradient);
	preview->setSpec(fill, current);
}

void GradientEditor::rebuildTable()
{
	table->setRowCount(current.stops.size());
	/* The rows the old selection named are gone, whether or not the count changed. */
	table->clearSelection();

	for (int row = 0; row < current.stops.size(); ++row) {
		const GradientStop &stop = current.stops.at(row);

		auto *position = new QSpinBox(table);
		position->setRange(0, 100);
		position->setSuffix(QStringLiteral(" %"));
		position->setValue(qRound(stop.position * 100.0));
		connect(position, &QSpinBox::valueChanged, this, [this] {
			if (loading)
				return;
			readTable();
			notifyEdited();
		});
		table->setCellWidget(row, StopPosition, position);

		auto *color = new ColorButton(table);
		/*
		 * The swatch takes the height the row is given rather than asking for one of its own.
		 * A push button asks for more height than a spin box, and letting it have that made
		 * every row taller than the value it exists to sit beside -- which cost a stop or two
		 * off the bottom of the table and gained nothing that was any easier to read.
		 */
		color->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
		color->setMinimumHeight(0);
		color->setColor(stop.color);
		color->setDialogTitle(moduleText("Designer.GradientStop.Color"));
		connect(color, &ColorButton::colorChanged, this, [this] {
			if (loading)
				return;
			readTable();
			notifyEdited();
		});
		table->setCellWidget(row, StopColor, color);

		for (QWidget *cell : {static_cast<QWidget *>(position), static_cast<QWidget *>(color)}) {
			cell->setProperty(kStopRowProperty, row);
			cell->installEventFilter(this);
		}

		/*
		 * Rows are sized from the position spin box and nothing else. A cell widget is
		 * stretched to whatever the row happens to be, so this one number decides both
		 * whether the spin box is drawn at its own size and how tall the swatch beside it
		 * comes out -- and the spin box, being the control with a value to read, is the one
		 * worth sizing the row against.
		 */
		table->setRowHeight(row, position->sizeHint().height() + kStopRowPadding);
	}

	/*
	 * resizeColumnToContents measures items, and every cell here holds a widget instead, so the
	 * position column has to be sized from the spin box itself or it comes out the width of its
	 * own header and clips the value it is showing.
	 */
	if (table->rowCount() > 0) {
		const QWidget *position = table->cellWidget(0, StopPosition);
		table->setColumnWidth(StopPosition, position->sizeHint().width() + kStopColumnPadding);
	}
	updateTableHeight();
	removeButton->setEnabled(false);
	preview->setSpec(fill, current);
}

/*
 * The table is exactly as tall as the stops it holds, up to a cap. A fixed height sized for four
 * stops turned every sweep past that into a four-row window scrolled through one row at a time,
 * while a two-stop gradient -- the common case -- was given empty rows it had no use for.
 */
void GradientEditor::updateTableHeight()
{
	int total = table->horizontalHeader()->sizeHint().height() + table->frameWidth() * 2;
	for (int row = 0; row < table->rowCount(); ++row)
		total += table->rowHeight(row);

	const int height = std::min(total, kMaxStopTableHeight);
	table->setMinimumHeight(height);
	table->setMaximumHeight(height);
}

bool GradientEditor::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::FocusIn && !selecting) {
		const int row = rowForCell(watched);
		if (row >= 0)
			selectStopRow(row);
	}

	return QWidget::eventFilter(watched, event);
}

int GradientEditor::rowForCell(const QObject *cell) const
{
	const QVariant stored = cell->property(kStopRowProperty);
	if (!stored.isValid())
		return -1;

	const int row = stored.toInt();
	if (row < 0 || row >= table->rowCount())
		return -1;

	/*
	 * setCellWidget() only deletes the widget it replaces on the next trip through the event
	 * loop, so a rebuilt table leaves orphans behind that still carry a row number and still
	 * have this filter on them. Anything the table no longer holds does not name a stop.
	 */
	for (int column = 0; column < StopColumnCount; ++column)
		if (table->cellWidget(row, column) == cell)
			return row;

	return -1;
}

int GradientEditor::selectedStopRow() const
{
	const QModelIndexList rows = table->selectionModel()->selectedRows();
	return rows.isEmpty() ? -1 : rows.constFirst().row();
}

/*
 * Selecting through the selection model rather than with QTableWidget::selectRow(), which is
 * what crashed here. selectRow() also moves the current cell to the first column, and the view
 * answers a current cell change by focusing that cell's widget -- QAbstractItemView::edit()
 * hands focus straight to a persistent editor. The view has its own FocusIn filter on those
 * widgets that moves the current cell back to whichever one just took focus, so with a swatch
 * focused the two chase each other: current cell to the spin box, focus to the spin box,
 * current cell back to the swatch, focus back to the swatch. Every hop is delivered as a
 * nested event, so nothing unwinds and the stack runs out. Leaving the current cell alone
 * breaks the cycle -- the view still tracks it through its own filter, and the Remove button
 * reads the selection instead.
 */
void GradientEditor::selectStopRow(int row)
{
	if (selectedStopRow() == row)
		return;

	const QModelIndex first = table->model()->index(row, 0);
	const QModelIndex last = table->model()->index(row, StopColumnCount - 1);

	selecting = true;
	table->selectionModel()->select(QItemSelection(first, last),
					QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
	selecting = false;
}

void GradientEditor::readTable()
{
	QVector<GradientStop> stops;
	stops.reserve(table->rowCount());

	for (int row = 0; row < table->rowCount(); ++row) {
		const auto *position = qobject_cast<QSpinBox *>(table->cellWidget(row, StopPosition));
		const auto *color = qobject_cast<ColorButton *>(table->cellWidget(row, StopColor));
		if (!position || !color)
			continue;

		stops.append(GradientStop{position->value() / 100.0, color->color()});
	}

	current.stops = stops;
}

void GradientEditor::addStop()
{
	readTable();

	/*
	 * The new stop lands in the widest gap the sweep has, carrying the color already shown
	 * there, so adding one changes nothing on screen until it is moved or recolored.
	 */
	const QVector<QPair<qreal, QColor>> resolved = current.resolvedStops();

	qreal position = 0.5;
	QColor color(255, 255, 255);
	qreal widest = -1.0;

	for (int i = 1; i < resolved.size(); ++i) {
		const qreal gap = resolved.at(i).first - resolved.at(i - 1).first;
		if (gap <= widest)
			continue;

		widest = gap;
		position = (resolved.at(i - 1).first + resolved.at(i).first) / 2.0;

		const QColor &before = resolved.at(i - 1).second;
		const QColor &after = resolved.at(i).second;
		color = QColor::fromRgbF((before.redF() + after.redF()) / 2.0, (before.greenF() + after.greenF()) / 2.0,
					 (before.blueF() + after.blueF()) / 2.0,
					 (before.alphaF() + after.alphaF()) / 2.0);
	}

	current.stops.append(GradientStop{position, color});
	sortStops();

	rebuildTable();
	notifyEdited();
}

void GradientEditor::removeSelectedStop()
{
	const int row = selectedStopRow();
	if (row < 0 || row >= current.stops.size() || current.stops.size() <= 2)
		return;

	readTable();
	current.stops.removeAt(row);

	rebuildTable();
	notifyEdited();
}

void GradientEditor::notifyEdited()
{
	preview->setSpec(fill, current);
	emit changed();
}

} // namespace closingtime
