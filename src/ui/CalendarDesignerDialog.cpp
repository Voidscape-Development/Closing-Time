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

#include "ui/CalendarDesignerDialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDate>
#include <QFile>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

#include "model/CalendarPresets.hpp"
#include "render/RenderThread.hpp"
#include "ui/BackgroundControls.hpp"
#include "ui/CalendarImportDialog.hpp"
#include "ui/CollapsibleGroup.hpp"
#include "ui/SectionEditor.hpp"
#include "ui/ToolButtons.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

/* How many steps back the designer can go before the oldest is forgotten. */
constexpr int kUndoDepth = 100;

/* Opening widths of the editor and the preview, in pixels. */
const QList<int> kDefaultPaneSizes = {820, 520};

/*
 * One designer window per source. Keyed by the raw pointer purely for identity; the pointer is
 * never dereferenced, and each dialog holds a weak reference for actual access.
 */
QHash<obs_source_t *, CalendarDesignerDialog *> &calendarRegistry()
{
	static QHash<obs_source_t *, CalendarDesignerDialog *> registry;
	return registry;
}

QTime timeFromMinutes(int minutes)
{
	if (minutes < 0)
		return QTime(0, 0);
	return QTime(0, 0).addSecs((minutes % 1440) * 60);
}

int minutesFromTime(const QTime &time)
{
	return time.hour() * 60 + time.minute();
}

QTableWidget *makeTable(QWidget *parent, const QStringList &headers)
{
	auto *table = new QTableWidget(parent);
	table->setColumnCount(headers.size());
	table->setHorizontalHeaderLabels(headers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->verticalHeader()->setVisible(false);
	table->horizontalHeader()->setStretchLastSection(true);
	return table;
}

/* The row of add/duplicate/remove buttons every table here carries. */
QWidget *makeTableButtons(QWidget *parent, const QString &addTip, QToolButton **add, QToolButton **duplicate,
			  QToolButton **remove)
{
	auto *row = new QWidget(parent);
	auto *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);

	*add = makeGlyphButton(row, QStringLiteral("+"), addTip);
	layout->addWidget(*add);

	if (duplicate) {
		*duplicate = makeGlyphButton(row, QStringLiteral("⧉"), moduleText("Calendar.Duplicate"));
		layout->addWidget(*duplicate);
	}

	*remove = makeGlyphButton(row, QStringLiteral("−"), moduleText("Calendar.Remove"));
	layout->addWidget(*remove);

	layout->addStretch(1);
	return row;
}

QDoubleSpinBox *makeDouble(QWidget *parent, double min, double max, double step = 1.0, const QString &suffix = {})
{
	auto *box = new QDoubleSpinBox(parent);
	box->setRange(min, max);
	box->setSingleStep(step);
	box->setDecimals(1);
	if (!suffix.isEmpty())
		box->setSuffix(suffix);
	return box;
}

QSpinBox *makeInt(QWidget *parent, int min, int max, const QString &suffix = {})
{
	auto *box = new QSpinBox(parent);
	box->setRange(min, max);
	if (!suffix.isEmpty())
		box->setSuffix(suffix);
	return box;
}

/*
 * A tag written the way the table takes it: `Label` or `Label:glyph`.
 *
 * One field rather than a table of its own because a tag is two short words and a row of events is
 * where they are read: opening a sub-editor to type "Main:twitch" would cost more than it saves.
 */
QString tagsToText(const QVector<ChannelTag> &tags)
{
	QStringList parts;
	for (const ChannelTag &tag : tags) {
		if (tag.glyph == TagGlyph::None)
			parts.append(tag.label);
		else
			parts.append(QStringLiteral("%1:%2").arg(tag.label, QString::fromUtf8(tagGlyphId(tag.glyph))));
	}
	return parts.join(QStringLiteral(", "));
}

QVector<ChannelTag> tagsFromText(const QString &text)
{
	QVector<ChannelTag> tags;
	for (const QString &part : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
		const QString trimmed = part.trimmed();
		if (trimmed.isEmpty())
			continue;

		ChannelTag tag;
		const int split = trimmed.lastIndexOf(QLatin1Char(':'));
		if (split > 0) {
			const QString glyph = trimmed.mid(split + 1).trimmed();
			const TagGlyph parsed = tagGlyphFromId(glyph.toUtf8().constData(), TagGlyph::None);
			if (parsed != TagGlyph::None) {
				tag.label = trimmed.left(split).trimmed();
				tag.glyph = parsed;
				tags.append(tag);
				continue;
			}
		}

		tag.label = trimmed;
		tags.append(tag);
	}
	return tags;
}

/*
 * One source the Tools submenu offers. The reference is weak because the menu holds these for as
 * long as it is open.
 */
struct MenuEntry {
	QString name;
	OBSWeakSource source;
};

struct MenuCollector {
	QByteArray sourceId;
	QVector<MenuEntry> entries;
};

QVector<MenuEntry> sourcesOfType(const QByteArray &sourceId)
{
	MenuCollector collector{sourceId, {}};

	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *found = static_cast<MenuCollector *>(param);

			const char *id = obs_source_get_id(source);
			if (id && found->sourceId == id)
				found->entries.append(MenuEntry{QString::fromUtf8(obs_source_get_name(source)),
								OBSGetWeakRef(source)});

			return true;
		},
		&collector);

	std::sort(collector.entries.begin(), collector.entries.end(), [](const MenuEntry &a, const MenuEntry &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});

	return collector.entries;
}

void fillCalendarMenu(QMenu *menu, const QByteArray &sourceId)
{
	menu->clear();

	const QVector<MenuEntry> entries = sourcesOfType(sourceId);
	if (entries.isEmpty()) {
		menu->addAction(moduleText("CalendarDesigner.NoSources"))->setEnabled(false);
		return;
	}

	for (const MenuEntry &entry : entries) {
		QAction *open = menu->addAction(entry.name);
		QObject::connect(open, &QAction::triggered, menu, [source = entry.source] {
			OBSSourceAutoRelease strong = obs_weak_source_get_source(source);
			if (strong)
				openCalendarDesignerFor(strong);
		});
	}
}

} // namespace

/* --- the preview ---------------------------------------------------------------------------- */

CalendarPreviewWidget::CalendarPreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(320, 240);
	setCursor(Qt::PointingHandCursor);
}

void CalendarPreviewWidget::setBoard(const QImage &image, const QVector<CalendarHit> &boardHits, int boardPage)
{
	board = image;
	hits = boardHits;
	page = boardPage;
	update();
}

void CalendarPreviewWidget::setSelectedEvent(int index)
{
	if (selected == index)
		return;

	selected = index;
	update();
}

QRectF CalendarPreviewWidget::boardRect() const
{
	if (board.isNull())
		return QRectF();

	const double scale =
		std::min(static_cast<double>(width()) / board.width(), static_cast<double>(height()) / board.height());
	const QSizeF size(board.width() * scale, board.height() * scale);
	return QRectF((width() - size.width()) / 2.0, (height() - size.height()) / 2.0, size.width(), size.height());
}

void CalendarPreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.fillRect(rect(), QColor(24, 24, 28));

	if (board.isNull()) {
		painter.setPen(QColor(150, 150, 160));
		painter.drawText(rect(), Qt::AlignCenter, moduleText("CalendarDesigner.NoPreview"));
		return;
	}

	const QRectF target = boardRect();

	/*
	 * A checkerboard behind the board, because a schedule is usually drawn on a transparent
	 * canvas to sit over gameplay: without it a board with no background of its own is invisible
	 * against a dark pane, which reads as the preview being broken.
	 */
	painter.save();
	painter.setClipRect(target);
	const int square = 12;
	for (int y = static_cast<int>(target.top()); y < target.bottom(); y += square) {
		for (int x = static_cast<int>(target.left()); x < target.right(); x += square) {
			const bool dark = ((x / square) + (y / square)) % 2 == 0;
			painter.fillRect(QRect(x, y, square, square), dark ? QColor(40, 40, 46) : QColor(52, 52, 58));
		}
	}
	painter.restore();

	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	painter.drawImage(target, board);

	painter.setPen(QPen(QColor(120, 120, 140), 1));
	painter.drawRect(target.adjusted(0.5, 0.5, -0.5, -0.5));

	if (selected < 0)
		return;

	/* The selected block, outlined where the layout actually put it. */
	const double scale = target.width() / board.width();
	for (const CalendarHit &hit : hits) {
		if (hit.event != selected || hit.page != page)
			continue;

		const QRectF outline(target.left() + hit.rect.left() * scale, target.top() + hit.rect.top() * scale,
				     hit.rect.width() * scale, hit.rect.height() * scale);
		painter.setPen(QPen(QColor(255, 214, 102), 2));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(outline.adjusted(-1, -1, 1, 1));
	}
}

void CalendarPreviewWidget::mousePressEvent(QMouseEvent *event)
{
	const QRectF target = boardRect();
	if (board.isNull() || !target.contains(event->position())) {
		emit eventPicked(-1);
		return;
	}

	const double scale = board.width() / target.width();
	const QPointF at((event->position().x() - target.left()) * scale,
			 (event->position().y() - target.top()) * scale);

	/*
	 * Searched back to front, so the block drawn on top of an overlap is the one picked -- which
	 * is the one under the pointer as far as anybody looking at it is concerned.
	 */
	for (int i = hits.size() - 1; i >= 0; --i) {
		if (hits[i].page != page || hits[i].event < 0)
			continue;
		if (!hits[i].rect.contains(at))
			continue;

		emit eventPicked(hits[i].event);
		return;
	}

	emit eventPicked(-1);
}

/* --- the dialog ----------------------------------------------------------------------------- */

CalendarDesignerDialog::CalendarDesignerDialog(obs_source_t *source, QWidget *parent)
	: QDialog(parent),
	  weakSource(OBSGetWeakRef(source)),
	  sink(std::make_shared<PreviewSink>())
{
	sink->dialog = this;

	setWindowTitle(QStringLiteral("%1 — %2").arg(moduleText("CalendarDesigner.Title"),
						     QString::fromUtf8(obs_source_get_name(source))));
	setWindowFlag(Qt::Window);
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1440, 900);

	calendarRegistry().insert(source, this);

	auto *layout = new QVBoxLayout(this);

	splitter = new QSplitter(Qt::Horizontal, this);
	splitter->setChildrenCollapsible(false);

	tabs = new QTabWidget(splitter);
	tabs->addTab(buildSchedulePane(), moduleText("CalendarDesigner.Tab.Schedule"));
	tabs->addTab(buildStructurePane(), moduleText("CalendarDesigner.Tab.Structure"));
	tabs->addTab(buildBoardPane(), moduleText("CalendarDesigner.Tab.Board"));
	tabs->addTab(buildLivePane(), moduleText("CalendarDesigner.Tab.Live"));
	tabs->addTab(buildElementsPane(), moduleText("CalendarDesigner.Tab.Elements"));
	tabs->addTab(buildStylePane(), moduleText("CalendarDesigner.Tab.Style"));
	splitter->addWidget(tabs);

	splitter->addWidget(buildPreviewPane());
	splitter->setSizes(kDefaultPaneSizes);
	layout->addWidget(splitter, 1);

	auto *bottom = new QHBoxLayout();

	undoButton = makeGlyphButton(this, QStringLiteral("↶"), moduleText("Designer.Undo"));
	redoButton = makeGlyphButton(this, QStringLiteral("↷"), moduleText("Designer.Redo"));
	bottom->addWidget(undoButton);
	bottom->addWidget(redoButton);

	auto *presetButton = new QPushButton(moduleText("CalendarDesigner.Presets"), this);
	auto *importButton = new QPushButton(moduleText("CalendarDesigner.Import"), this);
	auto *importJsonButton = new QPushButton(moduleText("CalendarDesigner.ImportJson"), this);
	auto *exportJsonButton = new QPushButton(moduleText("CalendarDesigner.ExportJson"), this);
	bottom->addWidget(presetButton);
	bottom->addWidget(importButton);
	bottom->addWidget(importJsonButton);
	bottom->addWidget(exportJsonButton);
	bottom->addStretch(1);

	auto *buttons =
		new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
	bottom->addWidget(buttons);
	layout->addLayout(bottom);

	connect(undoButton, &QToolButton::clicked, this, &CalendarDesignerDialog::undo);
	connect(redoButton, &QToolButton::clicked, this, &CalendarDesignerDialog::redo);
	connect(presetButton, &QPushButton::clicked, this, &CalendarDesignerDialog::applyPreset);
	connect(importButton, &QPushButton::clicked, this, &CalendarDesignerDialog::importDelimited);
	connect(importJsonButton, &QPushButton::clicked, this, &CalendarDesignerDialog::importJson);
	connect(exportJsonButton, &QPushButton::clicked, this, &CalendarDesignerDialog::exportJson);

	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		writeToSource();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &CalendarDesignerDialog::reject);
	connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { writeToSource(); });

	/*
	 * Debounced, for the reason the roll's preview is: an edit here is a keystroke, and
	 * rasterizing a board per character would keep the render thread busy producing pictures
	 * nobody sees.
	 */
	previewTimer = new QTimer(this);
	previewTimer->setSingleShot(true);
	previewTimer->setInterval(180);
	connect(previewTimer, &QTimer::timeout, this, &CalendarDesignerDialog::refreshPreview);

	loadFromSource();
	refreshUndoButtons();
	refreshPreview();
}

CalendarDesignerDialog::~CalendarDesignerDialog()
{
	sink->dialog = nullptr;

	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (source)
		calendarRegistry().remove(source);
	else {
		/* The source went first, so the entry is found by value rather than by key. */
		for (auto it = calendarRegistry().begin(); it != calendarRegistry().end(); ++it) {
			if (it.value() == this) {
				calendarRegistry().erase(it);
				break;
			}
		}
	}
}

bool CalendarDesignerDialog::sourceIsGone() const
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	return !source;
}

/* --- panes ---------------------------------------------------------------------------------- */

QWidget *CalendarDesignerDialog::buildSchedulePane()
{
	auto *pane = new QWidget(this);
	auto *layout = new QVBoxLayout(pane);

	eventTable = makeTable(pane, {moduleText("Calendar.Event.Title"), moduleText("Calendar.Event.Day"),
				      moduleText("Calendar.Event.Lane"), moduleText("Calendar.Event.Start"),
				      moduleText("Calendar.Event.End")});
	layout->addWidget(eventTable, 1);

	QToolButton *add = nullptr;
	QToolButton *duplicate = nullptr;
	QToolButton *remove = nullptr;
	layout->addWidget(makeTableButtons(pane, moduleText("Calendar.Event.Add"), &add, &duplicate, &remove));

	connect(add, &QToolButton::clicked, this, &CalendarDesignerDialog::addEvent);
	connect(duplicate, &QToolButton::clicked, this, &CalendarDesignerDialog::duplicateEvent);
	connect(remove, &QToolButton::clicked, this, &CalendarDesignerDialog::removeEvent);

	overlapWarning = new QLabel(pane);
	overlapWarning->setWordWrap(true);
	overlapWarning->setStyleSheet(QStringLiteral("color: #e0a030;"));
	layout->addWidget(overlapWarning);

	auto *scroll = new QScrollArea(pane);
	scroll->setWidgetResizable(true);
	auto *details = new QWidget(scroll);
	auto *form = new QFormLayout(details);

	eventTitle = new QLineEdit(details);
	eventSubtitle = new QLineEdit(details);
	eventDay = new QComboBox(details);
	eventLane = new QComboBox(details);
	eventCategory = new QComboBox(details);

	eventStart = new QTimeEdit(details);
	eventStart->setDisplayFormat(QStringLiteral("h:mm AP"));
	eventEnd = new QTimeEdit(details);
	eventEnd->setDisplayFormat(QStringLiteral("h:mm AP"));
	eventOpenEnded = new QCheckBox(moduleText("Calendar.Event.OpenEnded"), details);
	eventOpenEnded->setToolTip(moduleText("Calendar.Event.OpenEnded.Tip"));

	eventSlot = new QComboBox(details);
	eventEndSlot = new QComboBox(details);

	eventStatus = new QComboBox(details);
	for (EventStatus status : allEventStatuses())
		eventStatus->addItem(QString::fromUtf8(eventStatusName(status)),
				     QString::fromUtf8(eventStatusId(status)));

	eventBand = new QCheckBox(moduleText("Calendar.Event.Band"), details);
	eventBand->setToolTip(moduleText("Calendar.Event.Band.Tip"));
	eventContinuation = new QComboBox(details);
	eventContinuation->setToolTip(moduleText("Calendar.Event.Continuation.Tip"));

	eventLocation = new QLineEdit(details);
	eventZone = new QLineEdit(details);
	eventZone->setPlaceholderText(moduleText("Calendar.Event.Zone.Placeholder"));
	eventTags = new QLineEdit(details);
	eventTags->setToolTip(moduleText("Calendar.Event.Tags.Tip"));
	eventLogo = new QLineEdit(details);
	eventNotes = new QPlainTextEdit(details);
	eventNotes->setMaximumHeight(70);
	eventNotes->setToolTip(moduleText("Calendar.Event.Notes.Tip"));

	form->addRow(moduleText("Calendar.Event.Title"), eventTitle);
	form->addRow(moduleText("Calendar.Event.Subtitle"), eventSubtitle);
	form->addRow(moduleText("Calendar.Event.Day"), eventDay);
	form->addRow(moduleText("Calendar.Event.Lane"), eventLane);
	form->addRow(moduleText("Calendar.Event.Category"), eventCategory);
	form->addRow(moduleText("Calendar.Event.Start"), eventStart);
	form->addRow(moduleText("Calendar.Event.End"), eventEnd);
	form->addRow(QString(), eventOpenEnded);
	form->addRow(moduleText("Calendar.Event.Slot"), eventSlot);
	form->addRow(moduleText("Calendar.Event.EndSlot"), eventEndSlot);
	form->addRow(moduleText("Calendar.Event.Status"), eventStatus);
	form->addRow(QString(), eventBand);
	form->addRow(moduleText("Calendar.Event.Continuation"), eventContinuation);
	form->addRow(moduleText("Calendar.Event.Location"), eventLocation);
	form->addRow(moduleText("Calendar.Event.Zone"), eventZone);
	form->addRow(moduleText("Calendar.Event.Tags"), eventTags);
	form->addRow(moduleText("Calendar.Event.Logo"), eventLogo);
	form->addRow(moduleText("Calendar.Event.Notes"), eventNotes);

	scroll->setWidget(details);
	layout->addWidget(scroll, 1);

	connect(eventTable, &QTableWidget::itemSelectionChanged, this, [this] {
		refreshEventDetails();
		preview->setSelectedEvent(selectedEventRow());
	});

	/* Every field writes the whole document back, for the reason `commit` gives. */
	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		refreshEventTable();
		schedulePreviewRefresh();
	};

	connect(eventTitle, &QLineEdit::textEdited, this, edited);
	connect(eventSubtitle, &QLineEdit::textEdited, this, edited);
	connect(eventLocation, &QLineEdit::textEdited, this, edited);
	connect(eventZone, &QLineEdit::textEdited, this, edited);
	connect(eventTags, &QLineEdit::textEdited, this, edited);
	connect(eventLogo, &QLineEdit::textEdited, this, edited);
	connect(eventNotes, &QPlainTextEdit::textChanged, this, edited);
	connect(eventDay, &QComboBox::currentIndexChanged, this, edited);
	connect(eventLane, &QComboBox::currentIndexChanged, this, edited);
	connect(eventCategory, &QComboBox::currentIndexChanged, this, edited);
	connect(eventSlot, &QComboBox::currentIndexChanged, this, edited);
	connect(eventEndSlot, &QComboBox::currentIndexChanged, this, edited);
	connect(eventStatus, &QComboBox::currentIndexChanged, this, edited);
	connect(eventContinuation, &QComboBox::currentIndexChanged, this, edited);
	connect(eventStart, &QTimeEdit::timeChanged, this, edited);
	connect(eventEnd, &QTimeEdit::timeChanged, this, edited);
	connect(eventOpenEnded, &QCheckBox::toggled, this, edited);
	connect(eventBand, &QCheckBox::toggled, this, edited);

	return pane;
}

QWidget *CalendarDesignerDialog::buildStructurePane()
{
	auto *pane = new QScrollArea(this);
	pane->setWidgetResizable(true);

	auto *body = new QWidget(pane);
	auto *layout = new QVBoxLayout(body);

	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		refreshReferenceCombos();
		refreshEventTable();
		schedulePreviewRefresh();
	};

	/*
	 * Four tables rather than four panes: days, lanes, slots and categories are all short lists of
	 * short rows, and putting them one under another means the whole shape of a board is on one
	 * screen -- which is what somebody adding a lane is actually checking.
	 */
	const auto addTable = [&](const QString &title, QTableWidget **table, const QStringList &headers,
				  const QString &addTip, auto onAdd, auto onRemove) {
		auto *group = new CollapsibleGroup(title, body);
		*table = makeTable(group->content(), headers);
		(*table)->setMaximumHeight(180);
		group->addWidget(*table);

		QToolButton *add = nullptr;
		QToolButton *remove = nullptr;
		group->addWidget(makeTableButtons(group->content(), addTip, &add, nullptr, &remove));

		connect(add, &QToolButton::clicked, this, onAdd);
		connect(remove, &QToolButton::clicked, this, onRemove);
		connect(*table, &QTableWidget::cellChanged, this, edited);

		layout->addWidget(group);
	};

	addTable(
		moduleText("Calendar.Days"), &dayTable,
		{moduleText("Calendar.Day.Label"), moduleText("Calendar.Day.Date"), moduleText("Calendar.Day.Sub")},
		moduleText("Calendar.Day.Add"),
		[this] {
			beginUndoStep();
			CalendarDay day;
			day.id = document.makeDayId();
			day.date = document.days.isEmpty() ? QDate::currentDate()
							   : document.days.last().date.addDays(1);
			document.days.append(day);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		},
		[this] {
			const int row = dayTable->currentRow();
			if (row < 0 || row >= document.days.size())
				return;
			beginUndoStep();
			document.days.remove(row);
			document.syncDays();
			refreshStructureTables();
			refreshReferenceCombos();
			refreshEventTable();
			schedulePreviewRefresh();
		});

	addTable(
		moduleText("Calendar.Lanes"), &laneTable,
		{moduleText("Calendar.Lane.Name"), moduleText("Calendar.Lane.Sub"), moduleText("Calendar.Lane.Logo"),
		 moduleText("Calendar.Lane.Overlap"), moduleText("Calendar.Lane.OpenEnded"),
		 moduleText("Calendar.Lane.Minutes")},
		moduleText("Calendar.Lane.Add"),
		[this] {
			beginUndoStep();
			CalendarLane lane;
			lane.id = document.makeLaneId();
			lane.name = moduleText("Calendar.Lane.New");
			document.lanes.append(lane);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		},
		[this] {
			const int row = laneTable->currentRow();
			if (row < 0 || row >= document.lanes.size())
				return;
			beginUndoStep();
			document.lanes.remove(row);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		});

	addTable(
		moduleText("Calendar.Slots"), &slotTable,
		{moduleText("Calendar.Slot.Name"), moduleText("Calendar.Slot.Start"), moduleText("Calendar.Slot.End"),
		 moduleText("Calendar.Slot.Weight")},
		moduleText("Calendar.Slot.Add"),
		[this] {
			beginUndoStep();
			CalendarSlot slot;
			slot.id = document.makeSlotId();
			slot.name = QStringLiteral("WAVE %1").arg(
				QChar::fromLatin1('A' + static_cast<char>(document.timeSlots.size() % 26)));
			document.timeSlots.append(slot);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		},
		[this] {
			const int row = slotTable->currentRow();
			if (row < 0 || row >= document.timeSlots.size())
				return;
			beginUndoStep();
			document.timeSlots.remove(row);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		});

	addTable(
		moduleText("Calendar.Categories"), &categoryTable,
		{moduleText("Calendar.Category.Name"), moduleText("Calendar.Category.Color"),
		 moduleText("Calendar.Category.Legend")},
		moduleText("Calendar.Category.Add"),
		[this] {
			beginUndoStep();
			CalendarCategory category;
			category.id = document.makeCategoryId();
			category.name = moduleText("Calendar.Category.New");
			category.style.usePanel = true;
			category.style.panel.fill = BackgroundFill::Color;
			category.style.panel.color = QColor(60, 110, 200);
			category.style.panel.setRadius(6.0);
			document.categories.append(category);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		},
		[this] {
			const int row = categoryTable->currentRow();
			if (row < 0 || row >= document.categories.size())
				return;
			beginUndoStep();
			document.categories.remove(row);
			refreshStructureTables();
			refreshReferenceCombos();
			schedulePreviewRefresh();
		});

	layout->addStretch(1);
	pane->setWidget(body);
	return pane;
}

QWidget *CalendarDesignerDialog::buildBoardPane()
{
	auto *pane = new QScrollArea(this);
	pane->setWidgetResizable(true);

	auto *body = new QWidget(pane);
	auto *layout = new QVBoxLayout(body);

	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		schedulePreviewRefresh();
	};

	const auto watch = [this, edited](QWidget *widget) {
		if (auto *box = qobject_cast<QComboBox *>(widget))
			connect(box, &QComboBox::currentIndexChanged, this, edited);
		else if (auto *check = qobject_cast<QCheckBox *>(widget))
			connect(check, &QCheckBox::toggled, this, edited);
		else if (auto *spin = qobject_cast<QDoubleSpinBox *>(widget))
			connect(spin, &QDoubleSpinBox::valueChanged, this, edited);
		else if (auto *intSpin = qobject_cast<QSpinBox *>(widget))
			connect(intSpin, &QSpinBox::valueChanged, this, edited);
		else if (auto *line = qobject_cast<QLineEdit *>(widget))
			connect(line, &QLineEdit::textEdited, this, edited);
	};

	auto *shape = new CollapsibleGroup(moduleText("Calendar.Group.Shape"), body);
	auto *shapeForm = new QFormLayout();

	layoutBox = new QComboBox(body);
	for (CalendarLayout value : allCalendarLayouts())
		layoutBox->addItem(QString::fromUtf8(calendarLayoutName(value)),
				   QString::fromUtf8(calendarLayoutId(value)));

	axisBox = new QComboBox(body);
	axisBox->addItem(moduleText("Calendar.Axis.Clock"), QStringLiteral("clock"));
	axisBox->addItem(moduleText("Calendar.Axis.Slots"), QStringLiteral("slots"));
	axisBox->setToolTip(moduleText("Calendar.Axis.Tip"));

	orientationBox = new QComboBox(body);
	orientationBox->addItem(moduleText("Calendar.Orientation.Down"), QStringLiteral("time_down"));
	orientationBox->addItem(moduleText("Calendar.Orientation.Across"), QStringLiteral("time_across"));

	daysAsColumns = new QCheckBox(moduleText("Calendar.DaysAsColumns"), body);
	daysAsColumns->setToolTip(moduleText("Calendar.DaysAsColumns.Tip"));

	marginX = makeDouble(body, 0.0, 1000.0, 4.0, QStringLiteral(" px"));
	marginY = makeDouble(body, 0.0, 1000.0, 4.0, QStringLiteral(" px"));

	shapeForm->addRow(moduleText("Calendar.Layout"), layoutBox);
	shapeForm->addRow(moduleText("Calendar.Axis"), axisBox);
	shapeForm->addRow(moduleText("Calendar.Orientation"), orientationBox);
	shapeForm->addRow(QString(), daysAsColumns);
	shapeForm->addRow(moduleText("Calendar.MarginX"), marginX);
	shapeForm->addRow(moduleText("Calendar.MarginY"), marginY);
	shape->addLayout(shapeForm);
	layout->addWidget(shape);

	auto *sizes = new CollapsibleGroup(moduleText("Calendar.Group.Sizes"), body);
	auto *sizeForm = new QFormLayout();

	pixelsPerHour = makeDouble(body, 4.0, 1000.0, 5.0, QStringLiteral(" px"));
	slotSize = makeDouble(body, 4.0, 1000.0, 5.0, QStringLiteral(" px"));
	laneSize = makeDouble(body, 8.0, 2000.0, 5.0, QStringLiteral(" px"));
	laneGap = makeDouble(body, 0.0, 200.0, 1.0, QStringLiteral(" px"));
	dayGap = makeDouble(body, 0.0, 400.0, 2.0, QStringLiteral(" px"));

	sizeForm->addRow(moduleText("Calendar.PixelsPerHour"), pixelsPerHour);
	sizeForm->addRow(moduleText("Calendar.SlotSize"), slotSize);
	sizeForm->addRow(moduleText("Calendar.LaneSize"), laneSize);
	sizeForm->addRow(moduleText("Calendar.LaneGap"), laneGap);
	sizeForm->addRow(moduleText("Calendar.DayGap"), dayGap);
	sizes->addLayout(sizeForm);
	layout->addWidget(sizes);

	auto *furniture = new CollapsibleGroup(moduleText("Calendar.Group.Furniture"), body);
	auto *furnitureForm = new QFormLayout();

	gutterBox = new QComboBox(body);
	gutterBox->addItem(moduleText("Calendar.Gutter.Leading"), QStringLiteral("leading"));
	gutterBox->addItem(moduleText("Calendar.Gutter.Trailing"), QStringLiteral("trailing"));
	gutterBox->addItem(moduleText("Calendar.Gutter.Both"), QStringLiteral("both"));
	gutterBox->addItem(moduleText("Calendar.Gutter.None"), QStringLiteral("none"));

	gutterWidth = makeDouble(body, 0.0, 500.0, 4.0, QStringLiteral(" px"));
	gutter24Hour = new QCheckBox(moduleText("Calendar.Gutter24Hour"), body);
	gutterStep = makeInt(body, 5, 240, QStringLiteral(" min"));

	showLaneHeaders = new QCheckBox(moduleText("Calendar.ShowLaneHeaders"), body);
	laneHeaderSize = makeDouble(body, 0.0, 500.0, 4.0, QStringLiteral(" px"));
	showDayHeaders = new QCheckBox(moduleText("Calendar.ShowDayHeaders"), body);
	dayHeaderHeight = makeDouble(body, 0.0, 500.0, 4.0, QStringLiteral(" px"));
	dayFormat = new QLineEdit(body);
	dayFormat->setToolTip(moduleText("Calendar.DayFormat.Tip"));
	dateFormat = new QLineEdit(body);
	dateFormat->setToolTip(moduleText("Calendar.DayFormat.Tip"));

	showTimeLines = new QCheckBox(moduleText("Calendar.ShowTimeLines"), body);
	showLaneLines = new QCheckBox(moduleText("Calendar.ShowLaneLines"), body);

	furnitureForm->addRow(moduleText("Calendar.Gutter"), gutterBox);
	furnitureForm->addRow(moduleText("Calendar.GutterWidth"), gutterWidth);
	furnitureForm->addRow(QString(), gutter24Hour);
	furnitureForm->addRow(moduleText("Calendar.GutterStep"), gutterStep);
	furnitureForm->addRow(QString(), showLaneHeaders);
	furnitureForm->addRow(moduleText("Calendar.LaneHeaderSize"), laneHeaderSize);
	furnitureForm->addRow(QString(), showDayHeaders);
	furnitureForm->addRow(moduleText("Calendar.DayHeaderHeight"), dayHeaderHeight);
	furnitureForm->addRow(moduleText("Calendar.DayFormat"), dayFormat);
	furnitureForm->addRow(moduleText("Calendar.DateFormat"), dateFormat);
	furnitureForm->addRow(QString(), showTimeLines);
	furnitureForm->addRow(QString(), showLaneLines);
	furniture->addLayout(furnitureForm);
	layout->addWidget(furniture);

	auto *blocks = new CollapsibleGroup(moduleText("Calendar.Group.Blocks"), body);
	auto *blockForm = new QFormLayout();

	showBlockTimes = new QCheckBox(moduleText("Calendar.ShowBlockTimes"), body);
	showBlockSubtitles = new QCheckBox(moduleText("Calendar.ShowBlockSubtitles"), body);
	showBlockTags = new QCheckBox(moduleText("Calendar.ShowBlockTags"), body);
	showBlockLocation = new QCheckBox(moduleText("Calendar.ShowBlockLocation"), body);
	showBlockLogos = new QCheckBox(moduleText("Calendar.ShowBlockLogos"), body);

	blockForm->addRow(QString(), showBlockTimes);
	blockForm->addRow(QString(), showBlockSubtitles);
	blockForm->addRow(QString(), showBlockTags);
	blockForm->addRow(QString(), showBlockLocation);
	blockForm->addRow(QString(), showBlockLogos);
	blocks->addLayout(blockForm);
	layout->addWidget(blocks);

	auto *overflow = new CollapsibleGroup(moduleText("Calendar.Group.Overflow"), body);
	auto *overflowForm = new QFormLayout();

	overflowBox = new QComboBox(body);
	for (OverflowMode mode : allOverflowModes())
		overflowBox->addItem(QString::fromUtf8(overflowModeName(mode)),
				     QString::fromUtf8(overflowModeId(mode)));
	overflowBox->setToolTip(moduleText("Calendar.Overflow.Tip"));

	pageDwell = makeDouble(body, 1.0, 600.0, 1.0, QStringLiteral(" s"));
	pageByDay = new QCheckBox(moduleText("Calendar.PageByDay"), body);
	pageByDay->setToolTip(moduleText("Calendar.PageByDay.Tip"));
	scrollSpeed = makeDouble(body, 1.0, 500.0, 5.0, QStringLiteral(" px/s"));

	overflowForm->addRow(moduleText("Calendar.Overflow"), overflowBox);
	overflowForm->addRow(moduleText("Calendar.PageDwell"), pageDwell);
	overflowForm->addRow(QString(), pageByDay);
	overflowForm->addRow(moduleText("Calendar.ScrollSpeed"), scrollSpeed);
	overflow->addLayout(overflowForm);
	layout->addWidget(overflow);

	auto *listGroup = new CollapsibleGroup(moduleText("Calendar.Group.UpNext"), body);
	auto *listForm = new QFormLayout();

	upNextCount = makeInt(body, 1, 64);
	upNextRowHeight = makeDouble(body, 20.0, 500.0, 4.0, QStringLiteral(" px"));
	upNextTimeWidth = makeDouble(body, 0.0, 800.0, 10.0, QStringLiteral(" px"));
	upNextIncludesPast = new QCheckBox(moduleText("Calendar.UpNextPast"), body);

	listForm->addRow(moduleText("Calendar.UpNextCount"), upNextCount);
	listForm->addRow(moduleText("Calendar.UpNextRowHeight"), upNextRowHeight);
	listForm->addRow(moduleText("Calendar.UpNextTimeWidth"), upNextTimeWidth);
	listForm->addRow(QString(), upNextIncludesPast);
	listGroup->addLayout(listForm);
	layout->addWidget(listGroup);

	auto *zoneGroup = new CollapsibleGroup(moduleText("Calendar.Group.Zone"), body);
	auto *zoneForm = new QFormLayout();

	zoneMode = new QComboBox(body);
	zoneMode->addItem(moduleText("Calendar.Zone.Automatic"), QStringLiteral("automatic"));
	zoneMode->addItem(moduleText("Calendar.Zone.Fixed"), QStringLiteral("fixed"));
	zoneMode->setToolTip(moduleText("Calendar.Zone.Tip"));
	zoneName = new QLineEdit(body);
	zoneName->setPlaceholderText(QStringLiteral("America/New_York"));

	zoneForm->addRow(moduleText("Calendar.Zone"), zoneMode);
	zoneForm->addRow(moduleText("Calendar.ZoneName"), zoneName);
	zoneGroup->addLayout(zoneForm);
	layout->addWidget(zoneGroup);

	for (QWidget *widget : std::initializer_list<QWidget *>{layoutBox,
								axisBox,
								orientationBox,
								daysAsColumns,
								marginX,
								marginY,
								pixelsPerHour,
								slotSize,
								laneSize,
								laneGap,
								dayGap,
								gutterBox,
								gutterWidth,
								gutter24Hour,
								gutterStep,
								showLaneHeaders,
								laneHeaderSize,
								showDayHeaders,
								dayHeaderHeight,
								dayFormat,
								dateFormat,
								showTimeLines,
								showLaneLines,
								showBlockTimes,
								showBlockSubtitles,
								showBlockTags,
								showBlockLocation,
								showBlockLogos,
								overflowBox,
								pageDwell,
								pageByDay,
								scrollSpeed,
								upNextCount,
								upNextRowHeight,
								upNextTimeWidth,
								upNextIncludesPast,
								zoneMode,
								zoneName})
		watch(widget);

	layout->addStretch(1);
	pane->setWidget(body);
	return pane;
}

QWidget *CalendarDesignerDialog::buildLivePane()
{
	auto *pane = new QScrollArea(this);
	pane->setWidgetResizable(true);

	auto *body = new QWidget(pane);
	auto *layout = new QVBoxLayout(body);

	auto *note = new QLabel(moduleText("Calendar.Live.Note"), body);
	note->setWordWrap(true);
	layout->addWidget(note);

	auto *form = new QFormLayout();

	nowLine = new QCheckBox(moduleText("Calendar.Live.NowLine"), body);
	nowLineLabel = new QLineEdit(body);
	nowLineLabel->setPlaceholderText(moduleText("Calendar.Live.NowLineLabel.Placeholder"));

	dimFinished = new QCheckBox(moduleText("Calendar.Live.DimFinished"), body);
	finishedOpacity = makeDouble(body, 0.0, 1.0, 0.05);
	finishedOpacity->setDecimals(2);

	highlightCurrent = new QCheckBox(moduleText("Calendar.Live.HighlightCurrent"), body);
	dropFinished = new QCheckBox(moduleText("Calendar.Live.DropFinished"), body);
	dropGrace = makeInt(body, 0, 1440, QStringLiteral(" min"));
	dropGrace->setToolTip(moduleText("Calendar.Live.Grace.Tip"));

	refreshSeconds = makeInt(body, 1, 3600, QStringLiteral(" s"));
	refreshSeconds->setToolTip(moduleText("Calendar.Live.Refresh.Tip"));

	form->addRow(QString(), nowLine);
	form->addRow(moduleText("Calendar.Live.NowLineLabel"), nowLineLabel);
	form->addRow(QString(), dimFinished);
	form->addRow(moduleText("Calendar.Live.FinishedOpacity"), finishedOpacity);
	form->addRow(QString(), highlightCurrent);
	form->addRow(QString(), dropFinished);
	form->addRow(moduleText("Calendar.Live.Grace"), dropGrace);
	form->addRow(moduleText("Calendar.Live.Refresh"), refreshSeconds);
	layout->addLayout(form);

	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		schedulePreviewRefresh();
	};

	connect(nowLine, &QCheckBox::toggled, this, edited);
	connect(nowLineLabel, &QLineEdit::textEdited, this, edited);
	connect(dimFinished, &QCheckBox::toggled, this, edited);
	connect(finishedOpacity, &QDoubleSpinBox::valueChanged, this, edited);
	connect(highlightCurrent, &QCheckBox::toggled, this, edited);
	connect(dropFinished, &QCheckBox::toggled, this, edited);
	connect(dropGrace, &QSpinBox::valueChanged, this, edited);
	connect(refreshSeconds, &QSpinBox::valueChanged, this, edited);

	layout->addStretch(1);
	pane->setWidget(body);
	return pane;
}

QWidget *CalendarDesignerDialog::buildElementsPane()
{
	auto *pane = new QWidget(this);
	auto *layout = new QVBoxLayout(pane);

	elementTable = makeTable(pane, {moduleText("Calendar.Element.Name"), moduleText("Calendar.Element.Type")});
	elementTable->setMaximumHeight(180);
	layout->addWidget(elementTable);

	QToolButton *add = nullptr;
	QToolButton *remove = nullptr;
	layout->addWidget(makeTableButtons(pane, moduleText("Calendar.Element.Add"), &add, nullptr, &remove));
	connect(add, &QToolButton::clicked, this, &CalendarDesignerDialog::addElement);
	connect(remove, &QToolButton::clicked, this, &CalendarDesignerDialog::removeElement);

	auto *scroll = new QScrollArea(pane);
	scroll->setWidgetResizable(true);
	auto *body = new QWidget(scroll);
	auto *bodyLayout = new QVBoxLayout(body);
	auto *form = new QFormLayout();

	elementType = new QComboBox(body);
	for (ElementType type : allElementTypes())
		elementType->addItem(QString::fromUtf8(elementTypeName(type)), QString::fromUtf8(elementTypeId(type)));

	elementLabel = new QLineEdit(body);
	elementAnchor = new QComboBox(body);
	for (ElementAnchor anchor : allElementAnchors())
		elementAnchor->addItem(QString::fromUtf8(elementAnchorName(anchor)),
				       QString::fromUtf8(elementAnchorId(anchor)));
	elementAnchor->setToolTip(moduleText("Calendar.Element.Anchor.Tip"));

	elementX = makeDouble(body, -4000.0, 4000.0, 4.0, QStringLiteral(" px"));
	elementY = makeDouble(body, -4000.0, 4000.0, 4.0, QStringLiteral(" px"));
	elementWidth = makeDouble(body, 0.0, 8000.0, 10.0, QStringLiteral(" px"));
	elementHeight = makeDouble(body, 0.0, 8000.0, 10.0, QStringLiteral(" px"));
	elementHeight->setToolTip(moduleText("Calendar.Element.Height.Tip"));

	elementText = new QPlainTextEdit(body);
	elementText->setMaximumHeight(90);
	elementText->setToolTip(moduleText("Calendar.Element.Text.Tip"));

	elementImage = new QLineEdit(body);
	elementClockFormat = new QLineEdit(body);
	elementClockFormat->setToolTip(moduleText("Calendar.Element.ClockFormat.Tip"));
	elementClockLocal = new QCheckBox(moduleText("Calendar.Element.ClockLocal"), body);
	elementClockLabel = new QLineEdit(body);

	elementLegendSource = new QComboBox(body);
	elementLegendSource->addItem(moduleText("Calendar.Legend.Categories"), QStringLiteral("categories"));
	elementLegendSource->addItem(moduleText("Calendar.Legend.Lanes"), QStringLiteral("lanes"));
	elementLegendSource->addItem(moduleText("Calendar.Legend.Statuses"), QStringLiteral("statuses"));
	elementLegendColumns = makeInt(body, 1, 12);

	form->addRow(moduleText("Calendar.Element.Type"), elementType);
	form->addRow(moduleText("Calendar.Element.Name"), elementLabel);
	form->addRow(moduleText("Calendar.Element.Anchor"), elementAnchor);
	form->addRow(moduleText("Calendar.Element.X"), elementX);
	form->addRow(moduleText("Calendar.Element.Y"), elementY);
	form->addRow(moduleText("Calendar.Element.Width"), elementWidth);
	form->addRow(moduleText("Calendar.Element.Height"), elementHeight);
	form->addRow(moduleText("Calendar.Element.Text"), elementText);
	form->addRow(moduleText("Calendar.Element.Image"), elementImage);
	form->addRow(moduleText("Calendar.Element.ClockFormat"), elementClockFormat);
	form->addRow(QString(), elementClockLocal);
	form->addRow(moduleText("Calendar.Element.ClockLabel"), elementClockLabel);
	form->addRow(moduleText("Calendar.Element.LegendSource"), elementLegendSource);
	form->addRow(moduleText("Calendar.Element.LegendColumns"), elementLegendColumns);
	bodyLayout->addLayout(form);

	auto *styleGroup = new CollapsibleGroup(moduleText("Calendar.Element.Style"), body);
	elementStyle = new StyleEditor(styleGroup->content());
	styleGroup->addWidget(elementStyle);
	bodyLayout->addWidget(styleGroup);

	auto *panelGroup = new CollapsibleGroup(moduleText("Calendar.Element.Panel"), body);
	elementPanel = new BackgroundEditor(panelGroup->content());
	panelGroup->addWidget(elementPanel);
	bodyLayout->addWidget(panelGroup);

	bodyLayout->addStretch(1);
	scroll->setWidget(body);
	layout->addWidget(scroll, 1);

	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		refreshElementList();
		schedulePreviewRefresh();
	};

	connect(elementTable, &QTableWidget::itemSelectionChanged, this,
		&CalendarDesignerDialog::refreshElementDetails);
	connect(elementType, &QComboBox::currentIndexChanged, this, edited);
	connect(elementLabel, &QLineEdit::textEdited, this, edited);
	connect(elementAnchor, &QComboBox::currentIndexChanged, this, edited);
	connect(elementX, &QDoubleSpinBox::valueChanged, this, edited);
	connect(elementY, &QDoubleSpinBox::valueChanged, this, edited);
	connect(elementWidth, &QDoubleSpinBox::valueChanged, this, edited);
	connect(elementHeight, &QDoubleSpinBox::valueChanged, this, edited);
	connect(elementText, &QPlainTextEdit::textChanged, this, edited);
	connect(elementImage, &QLineEdit::textEdited, this, edited);
	connect(elementClockFormat, &QLineEdit::textEdited, this, edited);
	connect(elementClockLocal, &QCheckBox::toggled, this, edited);
	connect(elementClockLabel, &QLineEdit::textEdited, this, edited);
	connect(elementLegendSource, &QComboBox::currentIndexChanged, this, edited);
	connect(elementLegendColumns, &QSpinBox::valueChanged, this, edited);
	connect(elementStyle, &StyleEditor::changed, this, edited);
	connect(elementPanel, &BackgroundEditor::changed, this, edited);

	return pane;
}

QWidget *CalendarDesignerDialog::buildStylePane()
{
	auto *pane = new QScrollArea(this);
	pane->setWidgetResizable(true);

	auto *body = new QWidget(pane);
	auto *layout = new QVBoxLayout(body);

	auto *note = new QLabel(moduleText("Calendar.Style.Note"), body);
	note->setWordWrap(true);
	layout->addWidget(note);

	const auto edited = [this] {
		if (loading)
			return;
		beginUndoStep();
		commit();
		schedulePreviewRefresh();
	};

	const auto addStyle = [&](const QString &title, StyleEditor **editor) {
		auto *group = new CollapsibleGroup(title, body);
		*editor = new StyleEditor(group->content());
		group->addWidget(*editor);
		group->setExpanded(false);
		layout->addWidget(group);
		connect(*editor, &StyleEditor::changed, this, edited);
	};

	addStyle(moduleText("Calendar.Style.Title"), &titleStyle);
	addStyle(moduleText("Calendar.Style.Subtitle"), &subtitleStyle);
	addStyle(moduleText("Calendar.Style.Meta"), &metaStyle);

	auto *panelGroup = new CollapsibleGroup(moduleText("Calendar.Style.Panel"), body);
	blockPanel = new BackgroundEditor(panelGroup->content());
	panelGroup->addWidget(blockPanel);
	panelGroup->setExpanded(false);
	layout->addWidget(panelGroup);
	connect(blockPanel, &BackgroundEditor::changed, this, edited);

	auto *fitGroup = new CollapsibleGroup(moduleText("Calendar.Style.Fit"), body);
	auto *fitForm = new QFormLayout();
	textFit = new QComboBox(body);
	for (TextFit fit : allTextFits())
		textFit->addItem(QString::fromUtf8(textFitName(fit)), QString::fromUtf8(textFitId(fit)));
	textFit->setToolTip(moduleText("Calendar.Style.Fit.Tip"));
	minPixelSize = makeInt(body, 4, 200, QStringLiteral(" px"));
	fitForm->addRow(moduleText("Calendar.Style.Fit"), textFit);
	fitForm->addRow(moduleText("Calendar.Style.MinSize"), minPixelSize);
	fitGroup->addLayout(fitForm);
	layout->addWidget(fitGroup);

	connect(textFit, &QComboBox::currentIndexChanged, this, edited);
	connect(minPixelSize, &QSpinBox::valueChanged, this, edited);

	addStyle(moduleText("Calendar.Style.Gutter"), &gutterStyle);
	addStyle(moduleText("Calendar.Style.Lane"), &laneStyle);
	addStyle(moduleText("Calendar.Style.Day"), &dayStyle);

	layout->addStretch(1);
	pane->setWidget(body);
	return pane;
}

QWidget *CalendarDesignerDialog::buildPreviewPane()
{
	auto *pane = new QWidget(this);
	auto *layout = new QVBoxLayout(pane);

	preview = new CalendarPreviewWidget(pane);
	layout->addWidget(preview, 1);

	connect(preview, &CalendarPreviewWidget::eventPicked, this, [this](int index) {
		if (index < 0 || index >= document.events.size())
			return;

		tabs->setCurrentIndex(0);
		eventTable->selectRow(index);
		preview->setSelectedEvent(index);
	});

	auto *controls = new QHBoxLayout();

	/*
	 * A board's live features are the hardest thing here to judge, because judging them means
	 * waiting. Being able to say "show me this at ten past seven" is what makes a now-line or a
	 * dropped event something that can be designed rather than discovered on air.
	 */
	previewAtEnabled = new QCheckBox(moduleText("CalendarDesigner.PreviewAt"), pane);
	previewAtEnabled->setToolTip(moduleText("CalendarDesigner.PreviewAt.Tip"));
	previewAt = new QDateTimeEdit(QDateTime::currentDateTime(), pane);
	previewAt->setDisplayFormat(QStringLiteral("ddd d MMM h:mm AP"));
	previewAt->setEnabled(false);

	previewPage = makeInt(pane, 1, 99);
	previewPage->setPrefix(moduleText("CalendarDesigner.Page"));

	controls->addWidget(previewAtEnabled);
	controls->addWidget(previewAt, 1);
	controls->addWidget(previewPage);
	layout->addLayout(controls);

	previewSummary = new QLabel(pane);
	previewSummary->setWordWrap(true);
	layout->addWidget(previewSummary);

	connect(previewAtEnabled, &QCheckBox::toggled, this, [this](bool on) {
		previewAt->setEnabled(on);
		if (on)
			previewAt->setDateTime(QDateTime::currentDateTime());
		schedulePreviewRefresh();
	});
	connect(previewAt, &QDateTimeEdit::dateTimeChanged, this, [this] { schedulePreviewRefresh(); });
	connect(previewPage, &QSpinBox::valueChanged, this, [this] { schedulePreviewRefresh(); });

	return pane;
}

/* --- document ------------------------------------------------------------------------------- */

void CalendarDesignerDialog::loadFromSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	document.load(settings);
	document.syncDays();

	refreshFields();
}

void CalendarDesignerDialog::writeToSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	/*
	 * The properties window owns the canvas; the designer owns everything else. Re-reading the
	 * live settings before writing means a canvas resized in the properties window while this was
	 * open is not clobbered on Apply.
	 */
	OBSDataAutoRelease settings = obs_source_get_settings(source);

	CalendarDocument merged = document;
	CalendarDocument current;
	current.load(settings);
	merged.width = current.width;
	merged.height = current.height;
	merged.background = current.background;

	/*
	 * Applying is where the board's fonts are collected: the one moment the content is settled and
	 * the window is not being typed into.
	 */
	merged.refreshFontBundle();
	document.bundledFonts = merged.bundledFonts;

	OBSDataAutoRelease updated = obs_data_create();
	merged.save(updated);
	obs_source_update(source, updated);
}

void CalendarDesignerDialog::commit()
{
	if (loading)
		return;

	/* Board. */
	document.layout =
		calendarLayoutFromId(layoutBox->currentData().toString().toUtf8().constData(), CalendarLayout::Grid);
	document.axisMode =
		timeAxisModeFromId(axisBox->currentData().toString().toUtf8().constData(), TimeAxisMode::Clock);
	document.orientation = gridOrientationFromId(orientationBox->currentData().toString().toUtf8().constData(),
						     GridOrientation::TimeDown);
	document.grid.daysAsColumns = daysAsColumns->isChecked();
	document.marginX = marginX->value();
	document.marginY = marginY->value();

	document.grid.pixelsPerHour = pixelsPerHour->value();
	document.grid.slotSize = slotSize->value();
	document.grid.laneSize = laneSize->value();
	document.grid.laneGap = laneGap->value();
	document.grid.dayGap = dayGap->value();

	document.grid.gutter =
		gutterSideFromId(gutterBox->currentData().toString().toUtf8().constData(), GutterSide::Leading);
	document.grid.gutterWidth = gutterWidth->value();
	document.grid.gutter24Hour = gutter24Hour->isChecked();
	document.grid.gutterStep = gutterStep->value();
	document.grid.showLaneHeaders = showLaneHeaders->isChecked();
	document.grid.laneHeaderSize = laneHeaderSize->value();
	document.grid.showDayHeaders = showDayHeaders->isChecked();
	document.grid.dayHeaderHeight = dayHeaderHeight->value();
	document.grid.dayFormat = dayFormat->text();
	document.grid.dateFormat = dateFormat->text();
	document.grid.showTimeLines = showTimeLines->isChecked();
	document.grid.showLaneLines = showLaneLines->isChecked();

	document.grid.showBlockTimes = showBlockTimes->isChecked();
	document.grid.showBlockSubtitles = showBlockSubtitles->isChecked();
	document.grid.showBlockTags = showBlockTags->isChecked();
	document.grid.showBlockLocation = showBlockLocation->isChecked();
	document.grid.showBlockLogos = showBlockLogos->isChecked();

	document.overflow.mode =
		overflowModeFromId(overflowBox->currentData().toString().toUtf8().constData(), OverflowMode::Fit);
	document.overflow.pageDwell = pageDwell->value();
	document.overflow.pageByDay = pageByDay->isChecked();
	document.overflow.scrollSpeed = scrollSpeed->value();

	document.upNextCount = upNextCount->value();
	document.upNextRowHeight = upNextRowHeight->value();
	document.upNextTimeWidth = upNextTimeWidth->value();
	document.upNextIncludesPast = upNextIncludesPast->isChecked();

	document.zoneMode =
		clockZoneModeFromId(zoneMode->currentData().toString().toUtf8().constData(), ClockZoneMode::Automatic);
	document.timeZone = zoneName->text().trimmed();

	/* Live. */
	document.live.nowLine = nowLine->isChecked();
	document.live.nowLineLabel = nowLineLabel->text();
	document.live.dimFinished = dimFinished->isChecked();
	document.live.finishedOpacity = finishedOpacity->value();
	document.live.highlightCurrent = highlightCurrent->isChecked();
	document.live.dropFinished = dropFinished->isChecked();
	document.live.dropGraceMinutes = dropGrace->value();
	document.live.refreshSeconds = refreshSeconds->value();

	/* Style. */
	document.blockStyle.useTitleStyle = true;
	document.blockStyle.titleStyle = titleStyle->style();
	document.blockStyle.titlePresetName = titleStyle->presetName();
	document.blockStyle.useSubtitleStyle = true;
	document.blockStyle.subtitleStyle = subtitleStyle->style();
	document.blockStyle.subtitlePresetName = subtitleStyle->presetName();
	document.blockStyle.useMetaStyle = true;
	document.blockStyle.metaStyle = metaStyle->style();
	document.blockStyle.metaPresetName = metaStyle->presetName();
	document.blockStyle.usePanel = true;
	document.blockStyle.panel = blockPanel->panel();
	document.blockStyle.panelPresetName = blockPanel->presetName();
	document.blockStyle.useFit = true;
	document.blockStyle.fit =
		textFitFromId(textFit->currentData().toString().toUtf8().constData(), TextFit::Shrink);
	document.blockStyle.minPixelSize = minPixelSize->value();

	document.grid.gutterStyle = gutterStyle->style();
	document.grid.gutterPresetName = gutterStyle->presetName();
	document.grid.laneStyle = laneStyle->style();
	document.grid.lanePresetName = laneStyle->presetName();
	document.grid.dayStyle = dayStyle->style();
	document.grid.dayPresetName = dayStyle->presetName();

	/* Structure tables. */
	for (int row = 0; row < dayTable->rowCount() && row < document.days.size(); ++row) {
		CalendarDay &day = document.days[row];
		day.label = dayTable->item(row, 0) ? dayTable->item(row, 0)->text() : QString();
		const QString date = dayTable->item(row, 1) ? dayTable->item(row, 1)->text().trimmed() : QString();
		day.date = QDate::fromString(date, Qt::ISODate);
		day.subLabel = dayTable->item(row, 2) ? dayTable->item(row, 2)->text() : QString();
	}

	for (int row = 0; row < laneTable->rowCount() && row < document.lanes.size(); ++row) {
		CalendarLane &lane = document.lanes[row];
		lane.name = laneTable->item(row, 0) ? laneTable->item(row, 0)->text() : QString();
		lane.subLabel = laneTable->item(row, 1) ? laneTable->item(row, 1)->text() : QString();
		lane.logo.path = laneTable->item(row, 2) ? laneTable->item(row, 2)->text() : QString();
		const QString overlap = laneTable->item(row, 3) ? laneTable->item(row, 3)->text() : QString();
		lane.overlap = laneOverlapFromId(overlap.toLower().toUtf8().constData(), LaneOverlap::Split);
		const QString rule = laneTable->item(row, 4) ? laneTable->item(row, 4)->text() : QString();
		lane.openEnded = openEndedRuleFromId(rule.toLower().toUtf8().constData(), OpenEndedRule::UntilNext);
		lane.openEndedMinutes = laneTable->item(row, 5) ? std::max(1, laneTable->item(row, 5)->text().toInt())
								: 60;
	}

	for (int row = 0; row < slotTable->rowCount() && row < document.timeSlots.size(); ++row) {
		CalendarSlot &slot = document.timeSlots[row];
		slot.name = slotTable->item(row, 0) ? slotTable->item(row, 0)->text() : QString();
		slot.startMinutes = slotTable->item(row, 1) ? parseScheduleTime(slotTable->item(row, 1)->text()) : -1;
		slot.endMinutes = slotTable->item(row, 2) ? parseScheduleTime(slotTable->item(row, 2)->text()) : -1;
		const double weight = slotTable->item(row, 3) ? slotTable->item(row, 3)->text().toDouble() : 1.0;
		slot.weight = weight > 0.0 ? weight : 1.0;
	}

	for (int row = 0; row < categoryTable->rowCount() && row < document.categories.size(); ++row) {
		CalendarCategory &category = document.categories[row];
		category.name = categoryTable->item(row, 0) ? categoryTable->item(row, 0)->text() : QString();

		const QString color = categoryTable->item(row, 1) ? categoryTable->item(row, 1)->text().trimmed()
								  : QString();
		if (QColor(color).isValid()) {
			category.style.usePanel = true;
			if (category.style.panel.fill == BackgroundFill::None)
				category.style.panel.fill = BackgroundFill::Color;
			category.style.panel.color = QColor(color);
		}

		const QString legend = categoryTable->item(row, 2) ? categoryTable->item(row, 2)->text().trimmed()
								   : QString();
		category.inLegend = legend.compare(QStringLiteral("no"), Qt::CaseInsensitive) != 0;
	}

	/* The selected event's details. */
	if (CalendarEvent *event = selectedEvent()) {
		event->title = eventTitle->text();
		event->subtitle = eventSubtitle->text();
		event->dayId = eventDay->currentData().toString();
		event->laneId = eventLane->currentData().toString();
		event->categoryId = eventCategory->currentData().toString();
		event->startMinutes = minutesFromTime(eventStart->time());
		event->endMinutes = eventOpenEnded->isChecked() ? -1 : minutesFromTime(eventEnd->time());
		event->slotId = eventSlot->currentData().toString();
		event->endSlotId = eventEndSlot->currentData().toString();
		event->status = eventStatusFromId(eventStatus->currentData().toString().toUtf8().constData(),
						  EventStatus::Auto);
		event->band = eventBand->isChecked();
		event->continuationOf = eventContinuation->currentData().toString();
		event->location = eventLocation->text();
		event->timeZone = eventZone->text().trimmed();
		event->tags = tagsFromText(eventTags->text());
		event->logo.path = eventLogo->text();
		event->notes = eventNotes->toPlainText();
	}

	/* The selected element's details. */
	if (CalendarElement *element = selectedElement()) {
		element->type = elementTypeFromId(elementType->currentData().toString().toUtf8().constData(),
						  ElementType::Text);
		element->label = elementLabel->text();
		element->anchor = elementAnchorFromId(elementAnchor->currentData().toString().toUtf8().constData(),
						      ElementAnchor::TopLeft);
		element->x = elementX->value();
		element->y = elementY->value();
		element->width = elementWidth->value();
		element->height = elementHeight->value();

		/*
		 * A ticker's several messages and a text element's one body are the same field on screen:
		 * one line per message, which is what a list of short strings wants to be typed as.
		 */
		const QString body = elementText->toPlainText();
		if (element->type == ElementType::Ticker)
			element->messages = body.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
		else
			element->text = body;

		element->image.path = elementImage->text();
		element->clockFormat = elementClockFormat->text();
		element->clockUsesLocalZone = elementClockLocal->isChecked();
		element->clockZoneLabel = elementClockLabel->text();
		element->legendSource = legendSourceFromId(
			elementLegendSource->currentData().toString().toUtf8().constData(), LegendSource::Categories);
		element->legendColumns = elementLegendColumns->value();
		element->textStyle = elementStyle->style();
		element->textPresetName = elementStyle->presetName();
		element->panel = elementPanel->panel();
		element->panelPresetName = elementPanel->presetName();
	}

	document.syncDays();
}

void CalendarDesignerDialog::refreshFields()
{
	loading = true;

	layoutBox->setCurrentIndex(
		std::max(0, layoutBox->findData(QString::fromUtf8(calendarLayoutId(document.layout)))));
	axisBox->setCurrentIndex(std::max(0, axisBox->findData(QString::fromUtf8(timeAxisModeId(document.axisMode)))));
	orientationBox->setCurrentIndex(
		std::max(0, orientationBox->findData(QString::fromUtf8(gridOrientationId(document.orientation)))));
	daysAsColumns->setChecked(document.grid.daysAsColumns);
	marginX->setValue(document.marginX);
	marginY->setValue(document.marginY);

	pixelsPerHour->setValue(document.grid.pixelsPerHour);
	slotSize->setValue(document.grid.slotSize);
	laneSize->setValue(document.grid.laneSize);
	laneGap->setValue(document.grid.laneGap);
	dayGap->setValue(document.grid.dayGap);

	gutterBox->setCurrentIndex(
		std::max(0, gutterBox->findData(QString::fromUtf8(gutterSideId(document.grid.gutter)))));
	gutterWidth->setValue(document.grid.gutterWidth);
	gutter24Hour->setChecked(document.grid.gutter24Hour);
	gutterStep->setValue(document.grid.gutterStep);
	showLaneHeaders->setChecked(document.grid.showLaneHeaders);
	laneHeaderSize->setValue(document.grid.laneHeaderSize);
	showDayHeaders->setChecked(document.grid.showDayHeaders);
	dayHeaderHeight->setValue(document.grid.dayHeaderHeight);
	dayFormat->setText(document.grid.dayFormat);
	dateFormat->setText(document.grid.dateFormat);
	showTimeLines->setChecked(document.grid.showTimeLines);
	showLaneLines->setChecked(document.grid.showLaneLines);

	showBlockTimes->setChecked(document.grid.showBlockTimes);
	showBlockSubtitles->setChecked(document.grid.showBlockSubtitles);
	showBlockTags->setChecked(document.grid.showBlockTags);
	showBlockLocation->setChecked(document.grid.showBlockLocation);
	showBlockLogos->setChecked(document.grid.showBlockLogos);

	overflowBox->setCurrentIndex(
		std::max(0, overflowBox->findData(QString::fromUtf8(overflowModeId(document.overflow.mode)))));
	pageDwell->setValue(document.overflow.pageDwell);
	pageByDay->setChecked(document.overflow.pageByDay);
	scrollSpeed->setValue(document.overflow.scrollSpeed);

	upNextCount->setValue(document.upNextCount);
	upNextRowHeight->setValue(document.upNextRowHeight);
	upNextTimeWidth->setValue(document.upNextTimeWidth);
	upNextIncludesPast->setChecked(document.upNextIncludesPast);

	zoneMode->setCurrentIndex(
		std::max(0, zoneMode->findData(QString::fromUtf8(clockZoneModeId(document.zoneMode)))));
	zoneName->setText(document.timeZone);

	nowLine->setChecked(document.live.nowLine);
	nowLineLabel->setText(document.live.nowLineLabel);
	dimFinished->setChecked(document.live.dimFinished);
	finishedOpacity->setValue(document.live.finishedOpacity);
	highlightCurrent->setChecked(document.live.highlightCurrent);
	dropFinished->setChecked(document.live.dropFinished);
	dropGrace->setValue(document.live.dropGraceMinutes);
	refreshSeconds->setValue(document.live.refreshSeconds);

	/*
	 * The style editors are shown the *resolved* base style rather than the raw layer, so a board
	 * that has never been styled opens showing what it is actually drawing rather than a blank
	 * 32px default that is not on screen anywhere.
	 */
	const ResolvedBlockStyle base = document.resolveStyle(document.blockStyle);
	titleStyle->setPresets(document.stylePresets, document.blockStyle.titlePresetName, false);
	titleStyle->setStyle(base.titleStyle);
	subtitleStyle->setPresets(document.stylePresets, document.blockStyle.subtitlePresetName, false);
	subtitleStyle->setStyle(base.subtitleStyle);
	metaStyle->setPresets(document.stylePresets, document.blockStyle.metaPresetName, false);
	metaStyle->setStyle(base.metaStyle);
	blockPanel->setPresets(document.backgroundPresets, document.blockStyle.panelPresetName, false);
	blockPanel->setPanel(base.panel);

	textFit->setCurrentIndex(std::max(0, textFit->findData(QString::fromUtf8(textFitId(base.fit)))));
	minPixelSize->setValue(base.minPixelSize);

	gutterStyle->setPresets(document.stylePresets, document.grid.gutterPresetName, false);
	gutterStyle->setStyle(document.grid.gutterStyle);
	laneStyle->setPresets(document.stylePresets, document.grid.lanePresetName, false);
	laneStyle->setStyle(document.grid.laneStyle);
	dayStyle->setPresets(document.stylePresets, document.grid.dayPresetName, false);
	dayStyle->setStyle(document.grid.dayStyle);

	loading = false;

	refreshStructureTables();
	refreshReferenceCombos();
	refreshEventTable();
	refreshElementList();
}

/* --- tables --------------------------------------------------------------------------------- */

void CalendarDesignerDialog::refreshEventTable()
{
	const int selected = selectedEventRow();

	const bool wasLoading = loading;
	loading = true;

	eventTable->setRowCount(document.events.size());

	for (int row = 0; row < document.events.size(); ++row) {
		const CalendarEvent &event = document.events[row];
		const EventSpan span = document.eventSpan(event);

		const CalendarDay *day = document.findDay(event.dayId);
		const CalendarLane *lane = document.findLane(event.laneId);

		const auto set = [this, row](int column, const QString &text) {
			auto *item = new QTableWidgetItem(text);
			item->setFlags(item->flags() & ~Qt::ItemIsEditable);
			eventTable->setItem(row, column, item);
		};

		set(0, event.displayLabel());
		set(1,
		    day ? (day->label.isEmpty() && day->date.isValid() ? day->date.toString(Qt::ISODate) : day->label)
			: QString());
		set(2, event.band ? moduleText("Calendar.Event.BandShort") : (lane ? lane->name : QString()));
		set(3, span.valid ? timeFromMinutes(span.startMinutes).toString(QStringLiteral("h:mm AP")) : QString());
		set(4, span.valid && !span.openEnded
			       ? timeFromMinutes(span.endMinutes).toString(QStringLiteral("h:mm AP"))
			       : QString());
	}

	if (selected >= 0 && selected < eventTable->rowCount())
		eventTable->selectRow(selected);

	loading = wasLoading;

	/* The collision report, which the board itself deliberately does not hide. */
	const QVector<OverlapReport> reports = document.overlaps();
	if (reports.isEmpty()) {
		overlapWarning->clear();
	} else {
		QStringList names;
		for (const OverlapReport &report : reports) {
			if (report.firstEvent < 0 || report.firstEvent >= document.events.size())
				continue;
			names.append(QStringLiteral("%1 / %2").arg(document.events[report.firstEvent].displayLabel(),
								   document.events[report.secondEvent].displayLabel()));
		}
		overlapWarning->setText(moduleText("Calendar.Overlaps").arg(names.join(QStringLiteral(", "))));
	}

	refreshEventDetails();
}

void CalendarDesignerDialog::refreshEventDetails()
{
	CalendarEvent *selected = selectedEvent();
	const bool enabled = selected != nullptr;

	for (QWidget *widget : std::initializer_list<QWidget *>{
		     eventTitle, eventSubtitle, eventDay, eventLane, eventCategory, eventStart, eventEnd,
		     eventOpenEnded, eventSlot, eventEndSlot, eventStatus, eventBand, eventContinuation, eventLocation,
		     eventZone, eventTags, eventLogo, eventNotes})
		widget->setEnabled(enabled);

	if (!enabled)
		return;

	loading = true;

	eventTitle->setText(selected->title);
	eventSubtitle->setText(selected->subtitle);
	eventDay->setCurrentIndex(std::max(0, eventDay->findData(selected->dayId)));
	eventLane->setCurrentIndex(std::max(0, eventLane->findData(selected->laneId)));
	eventCategory->setCurrentIndex(std::max(0, eventCategory->findData(selected->categoryId)));
	eventStart->setTime(timeFromMinutes(std::max(0, selected->startMinutes)));
	eventEnd->setTime(timeFromMinutes(std::max(0, selected->endMinutes)));
	eventOpenEnded->setChecked(selected->endMinutes < 0);
	eventEnd->setEnabled(selected->endMinutes >= 0);
	eventSlot->setCurrentIndex(std::max(0, eventSlot->findData(selected->slotId)));
	eventEndSlot->setCurrentIndex(std::max(0, eventEndSlot->findData(selected->endSlotId)));
	eventStatus->setCurrentIndex(
		std::max(0, eventStatus->findData(QString::fromUtf8(eventStatusId(selected->status)))));
	eventBand->setChecked(selected->band);
	eventContinuation->setCurrentIndex(std::max(0, eventContinuation->findData(selected->continuationOf)));
	eventLocation->setText(selected->location);
	eventZone->setText(selected->timeZone);
	eventTags->setText(tagsToText(selected->tags));
	eventLogo->setText(selected->logo.path);
	eventNotes->setPlainText(selected->notes);

	loading = false;
}

void CalendarDesignerDialog::refreshStructureTables()
{
	const bool wasLoading = loading;
	loading = true;

	dayTable->setRowCount(document.days.size());
	for (int row = 0; row < document.days.size(); ++row) {
		const CalendarDay &day = document.days[row];
		dayTable->setItem(row, 0, new QTableWidgetItem(day.label));
		dayTable->setItem(
			row, 1, new QTableWidgetItem(day.date.isValid() ? day.date.toString(Qt::ISODate) : QString()));
		dayTable->setItem(row, 2, new QTableWidgetItem(day.subLabel));
	}

	laneTable->setRowCount(document.lanes.size());
	for (int row = 0; row < document.lanes.size(); ++row) {
		const CalendarLane &lane = document.lanes[row];
		laneTable->setItem(row, 0, new QTableWidgetItem(lane.name));
		laneTable->setItem(row, 1, new QTableWidgetItem(lane.subLabel));
		laneTable->setItem(row, 2, new QTableWidgetItem(lane.logo.path));
		laneTable->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(laneOverlapId(lane.overlap))));
		laneTable->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(openEndedRuleId(lane.openEnded))));
		laneTable->setItem(row, 5, new QTableWidgetItem(QString::number(lane.openEndedMinutes)));
	}

	slotTable->setRowCount(document.timeSlots.size());
	for (int row = 0; row < document.timeSlots.size(); ++row) {
		const CalendarSlot &slot = document.timeSlots[row];
		slotTable->setItem(row, 0, new QTableWidgetItem(slot.name));
		slotTable->setItem(
			row, 1,
			new QTableWidgetItem(
				slot.startMinutes >= 0
					? timeFromMinutes(slot.startMinutes).toString(QStringLiteral("h:mm AP"))
					: QString()));
		slotTable->setItem(
			row, 2,
			new QTableWidgetItem(
				slot.endMinutes >= 0
					? timeFromMinutes(slot.endMinutes).toString(QStringLiteral("h:mm AP"))
					: QString()));
		slotTable->setItem(row, 3, new QTableWidgetItem(QString::number(slot.weight)));
	}

	categoryTable->setRowCount(document.categories.size());
	for (int row = 0; row < document.categories.size(); ++row) {
		const CalendarCategory &category = document.categories[row];
		categoryTable->setItem(row, 0, new QTableWidgetItem(category.name));

		const QColor color = document.resolveStyle(category.style).panel.color;
		auto *swatch = new QTableWidgetItem(color.name(QColor::HexRgb));
		swatch->setBackground(color);
		categoryTable->setItem(row, 1, swatch);

		categoryTable->setItem(
			row, 2, new QTableWidgetItem(category.inLegend ? QStringLiteral("yes") : QStringLiteral("no")));
	}

	loading = wasLoading;
}

void CalendarDesignerDialog::refreshReferenceCombos()
{
	const bool wasLoading = loading;
	loading = true;

	const auto fill = [](QComboBox *box, const QString &emptyLabel, const QVector<QPair<QString, QString>> &items) {
		const QString current = box->currentData().toString();
		box->clear();
		box->addItem(emptyLabel, QString());
		for (const auto &item : items)
			box->addItem(item.second, item.first);

		const int index = box->findData(current);
		box->setCurrentIndex(index >= 0 ? index : 0);
	};

	QVector<QPair<QString, QString>> days;
	for (const CalendarDay &day : document.days) {
		days.append({day.id,
			     day.label.isEmpty() && day.date.isValid() ? day.date.toString(Qt::ISODate) : day.label});
	}

	QVector<QPair<QString, QString>> lanes;
	for (const CalendarLane &lane : document.lanes)
		lanes.append({lane.id, lane.name});

	QVector<QPair<QString, QString>> categories;
	for (const CalendarCategory &category : document.categories)
		categories.append({category.id, category.name});

	QVector<QPair<QString, QString>> slotItems;
	for (const CalendarSlot &slot : document.timeSlots)
		slotItems.append({slot.id, slot.name});

	QVector<QPair<QString, QString>> events;
	for (const CalendarEvent &event : document.events)
		events.append({event.id, event.displayLabel()});

	fill(eventDay, moduleText("Calendar.None"), days);
	fill(eventLane, moduleText("Calendar.None"), lanes);
	fill(eventCategory, moduleText("Calendar.None"), categories);
	fill(eventSlot, moduleText("Calendar.None"), slotItems);
	fill(eventEndSlot, moduleText("Calendar.None"), slotItems);
	fill(eventContinuation, moduleText("Calendar.None"), events);

	loading = wasLoading;
}

void CalendarDesignerDialog::refreshElementList()
{
	const int selected = elementTable->currentRow();

	const bool wasLoading = loading;
	loading = true;

	elementTable->setRowCount(document.elements.size());
	for (int row = 0; row < document.elements.size(); ++row) {
		const CalendarElement &element = document.elements[row];

		auto *name = new QTableWidgetItem(element.displayLabel());
		name->setFlags(name->flags() & ~Qt::ItemIsEditable);
		elementTable->setItem(row, 0, name);

		auto *type = new QTableWidgetItem(QString::fromUtf8(elementTypeName(element.type)));
		type->setFlags(type->flags() & ~Qt::ItemIsEditable);
		elementTable->setItem(row, 1, type);
	}

	if (selected >= 0 && selected < elementTable->rowCount())
		elementTable->selectRow(selected);

	loading = wasLoading;
	refreshElementDetails();
}

void CalendarDesignerDialog::refreshElementDetails()
{
	CalendarElement *element = selectedElement();
	const bool enabled = element != nullptr;

	for (QWidget *widget : std::initializer_list<QWidget *>{
		     elementType, elementLabel, elementAnchor, elementX, elementY, elementWidth, elementHeight,
		     elementText, elementImage, elementClockFormat, elementClockLocal, elementClockLabel,
		     elementLegendSource, elementLegendColumns, elementStyle, elementPanel})
		widget->setEnabled(enabled);

	if (!enabled)
		return;

	loading = true;

	elementType->setCurrentIndex(
		std::max(0, elementType->findData(QString::fromUtf8(elementTypeId(element->type)))));
	elementLabel->setText(element->label);
	elementAnchor->setCurrentIndex(
		std::max(0, elementAnchor->findData(QString::fromUtf8(elementAnchorId(element->anchor)))));
	elementX->setValue(element->x);
	elementY->setValue(element->y);
	elementWidth->setValue(element->width);
	elementHeight->setValue(element->height);
	elementText->setPlainText(element->type == ElementType::Ticker ? element->messages.join(QLatin1Char('\n'))
								       : element->text);
	elementImage->setText(element->image.path);
	elementClockFormat->setText(element->clockFormat);
	elementClockLocal->setChecked(element->clockUsesLocalZone);
	elementClockLabel->setText(element->clockZoneLabel);
	elementLegendSource->setCurrentIndex(
		std::max(0, elementLegendSource->findData(QString::fromUtf8(legendSourceId(element->legendSource)))));
	elementLegendColumns->setValue(element->legendColumns);

	elementStyle->setPresets(document.stylePresets, element->textPresetName, false);
	elementStyle->setStyle(element->textStyle);
	elementPanel->setPresets(document.backgroundPresets, element->panelPresetName, false);
	elementPanel->setPanel(element->panel);

	loading = false;
}

/* --- editing -------------------------------------------------------------------------------- */

int CalendarDesignerDialog::selectedEventRow() const
{
	return eventTable ? eventTable->currentRow() : -1;
}

CalendarEvent *CalendarDesignerDialog::selectedEvent()
{
	const int row = selectedEventRow();
	if (row < 0 || row >= document.events.size())
		return nullptr;
	return &document.events[row];
}

CalendarElement *CalendarDesignerDialog::selectedElement()
{
	if (!elementTable)
		return nullptr;

	const int row = elementTable->currentRow();
	if (row < 0 || row >= document.elements.size())
		return nullptr;
	return &document.elements[row];
}

void CalendarDesignerDialog::addEvent()
{
	beginUndoStep();

	CalendarEvent event;
	event.id = document.makeEventId();
	event.title = moduleText("Calendar.Event.New");
	event.dayId = document.days.isEmpty() ? QString() : document.days.first().id;
	event.laneId = document.lanes.isEmpty() ? QString() : document.lanes.first().id;

	/* A new event starts where the last one in that lane ended, which is usually where it goes. */
	int latest = 10 * 60;
	for (const CalendarEvent &other : document.events) {
		if (other.dayId != event.dayId || other.laneId != event.laneId)
			continue;
		const EventSpan span = document.eventSpan(other);
		if (span.valid)
			latest = std::max(latest, span.endMinutes);
	}
	event.startMinutes = std::min(latest, 23 * 60);
	event.endMinutes = std::min(latest + 60, 24 * 60);

	document.events.append(event);
	document.syncDays();

	refreshReferenceCombos();
	refreshEventTable();
	eventTable->selectRow(document.events.size() - 1);
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::duplicateEvent()
{
	const int row = selectedEventRow();
	if (row < 0 || row >= document.events.size())
		return;

	beginUndoStep();

	CalendarEvent copy = document.events[row];
	copy.id = document.makeEventId();
	document.events.insert(row + 1, copy);

	refreshReferenceCombos();
	refreshEventTable();
	eventTable->selectRow(row + 1);
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::removeEvent()
{
	const int row = selectedEventRow();
	if (row < 0 || row >= document.events.size())
		return;

	beginUndoStep();

	const QString removed = document.events[row].id;
	document.events.remove(row);

	/* A continuation pointing at an event that has gone degrades to an ordinary block. */
	for (CalendarEvent &event : document.events) {
		if (event.continuationOf == removed)
			event.continuationOf.clear();
	}

	refreshReferenceCombos();
	refreshEventTable();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::addElement()
{
	beginUndoStep();

	CalendarElement element = CalendarElement::makeDefault(ElementType::Text);
	element.id = document.makeElementId();
	document.elements.append(element);

	refreshElementList();
	elementTable->selectRow(document.elements.size() - 1);
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::removeElement()
{
	const int row = elementTable->currentRow();
	if (row < 0 || row >= document.elements.size())
		return;

	beginUndoStep();
	document.elements.remove(row);
	refreshElementList();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::applyPreset()
{
	QDialog dialog(this);
	dialog.setWindowTitle(moduleText("CalendarDesigner.Presets"));

	auto *layout = new QVBoxLayout(&dialog);

	auto *list = new QComboBox(&dialog);
	for (const CalendarPresetInfo &info : allCalendarPresets())
		list->addItem(QString::fromUtf8(info.name), QString::fromUtf8(info.id));

	auto *description = new QLabel(&dialog);
	description->setWordWrap(true);

	const auto describe = [list, description] {
		const QString id = list->currentData().toString();
		for (const CalendarPresetInfo &info : allCalendarPresets()) {
			if (id == QString::fromUtf8(info.id))
				description->setText(QString::fromUtf8(info.description));
		}
	};
	QObject::connect(list, &QComboBox::currentIndexChanged, &dialog, describe);
	describe();

	auto *sample = new QCheckBox(moduleText("CalendarDesigner.Preset.Sample"), &dialog);
	sample->setToolTip(moduleText("CalendarDesigner.Preset.Sample.Tip"));
	sample->setChecked(document.events.isEmpty());

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	layout->addWidget(list);
	layout->addWidget(description);
	layout->addWidget(sample);
	layout->addWidget(buttons);

	if (dialog.exec() != QDialog::Accepted)
		return;

	beginUndoStep();
	applyCalendarPreset(list->currentData().toString(), &document, sample->isChecked());
	refreshFields();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::importDelimited()
{
	CalendarImportDialog dialog(document, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const CalendarImportDialog::Result result = dialog.result();
	if (result.events.isEmpty())
		return;

	beginUndoStep();

	if (result.replaceExisting)
		document.events.clear();

	document.days += result.days;
	document.lanes += result.lanes;
	document.categories += result.categories;

	/* Ids from the importer are only unique within the import, so they are reissued here. */
	for (CalendarEvent event : result.events) {
		event.id = document.makeEventId();
		document.events.append(event);
	}

	document.syncDays();
	refreshFields();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::importJson()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("CalendarDesigner.ImportJson"), QString(),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, moduleText("CalendarDesigner.ImportJson"), file.errorString());
		return;
	}

	CalendarDocument loaded;
	QString error;
	if (!loaded.fromJson(QString::fromUtf8(file.readAll()), &error)) {
		QMessageBox::warning(this, moduleText("CalendarDesigner.ImportJson"), error);
		return;
	}

	beginUndoStep();

	/* The canvas belongs to the properties window, so an imported board keeps this one's. */
	const int width = document.width;
	const int height = document.height;
	const QColor background = document.background;

	document = loaded;
	document.width = width;
	document.height = height;
	document.background = background;
	document.syncDays();

	refreshFields();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::exportJson()
{
	const QString path = QFileDialog::getSaveFileName(this, moduleText("CalendarDesigner.ExportJson"), QString(),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		QMessageBox::warning(this, moduleText("CalendarDesigner.ExportJson"), file.errorString());
		return;
	}

	QTextStream stream(&file);
	stream << document.toJson();
}

/* --- preview -------------------------------------------------------------------------------- */

QDateTime CalendarDesignerDialog::previewTime() const
{
	if (previewAtEnabled && previewAtEnabled->isChecked() && previewAt)
		return previewAt->dateTime();
	return QDateTime::currentDateTime();
}

void CalendarDesignerDialog::schedulePreviewRefresh()
{
	if (previewTimer)
		previewTimer->start();
}

void CalendarDesignerDialog::refreshPreview()
{
	if (renderInFlight) {
		renderAgain = true;
		return;
	}

	renderInFlight = true;

	const CalendarDocument snapshot = document;
	const QDateTime at = previewTime();
	auto keeper = sink;

	postRenderJob([this, snapshot, at, keeper] {
		CalendarRenderer renderer(&logos);
		CalendarBoard board = renderer.render(snapshot, at);

		/*
		 * Back to the UI thread through the sink, which the destructor clears: a window closed
		 * while a render was in flight takes the callback with it rather than being written into.
		 */
		QMetaObject::invokeMethod(
			QCoreApplication::instance(),
			[keeper, board = std::move(board), snapshot] {
				if (keeper->dialog)
					keeper->dialog->applyPreview(board, snapshot);
			},
			Qt::QueuedConnection);
	});
}

void CalendarDesignerDialog::applyPreview(const CalendarBoard &board, const CalendarDocument &rendered)
{
	renderInFlight = false;

	if (renderAgain) {
		renderAgain = false;
		schedulePreviewRefresh();
	}

	previewPage->setMaximum(std::max(1, static_cast<int>(board.pages.size())));
	const int page = std::clamp(previewPage->value() - 1, 0, static_cast<int>(board.pages.size()) - 1);

	QImage flat(std::max(1, board.width), std::max(1, board.height), QImage::Format_ARGB32);
	flat.fill(Qt::transparent);

	if (!board.pages.isEmpty()) {
		QPainter painter(&flat);
		for (const StripTile &tile : board.pages[page].tiles)
			painter.drawImage(QPointF(0.0, tile.top), tile.image);

		/* And the free layer over it, exactly as the source draws it. */
		if (!board.overlay.isNull())
			painter.drawImage(QPointF(0.0, 0.0), board.overlay);
	}

	preview->setBoard(flat, board.hits, page);

	QStringList notes;
	notes.append(moduleText("CalendarDesigner.Summary")
			     .arg(board.placedEvents)
			     .arg(static_cast<int>(std::lround(board.boardWidth)))
			     .arg(static_cast<int>(std::lround(board.boardHeight))));

	if (board.pages.size() > 1)
		notes.append(moduleText("CalendarDesigner.Pages").arg(board.pages.size()));
	if (board.scale < 0.999)
		notes.append(moduleText("CalendarDesigner.Scaled").arg(std::lround(board.scale * 100.0)));
	if (board.clipped)
		notes.append(moduleText("CalendarDesigner.Clipped"));

	const int unplaced = rendered.events.size() - board.placedEvents;
	if (unplaced > 0)
		notes.append(moduleText("CalendarDesigner.Unplaced").arg(unplaced));

	previewSummary->setText(notes.join(QStringLiteral("  ·  ")));
}

/* --- undo ----------------------------------------------------------------------------------- */

void CalendarDesignerDialog::beginUndoStep()
{
	undoStack.append(document);
	if (undoStack.size() > kUndoDepth)
		undoStack.removeFirst();

	redoStack.clear();
	refreshUndoButtons();
}

void CalendarDesignerDialog::undo()
{
	if (undoStack.isEmpty())
		return;

	redoStack.append(document);
	document = undoStack.takeLast();

	refreshFields();
	refreshUndoButtons();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::redo()
{
	if (redoStack.isEmpty())
		return;

	undoStack.append(document);
	document = redoStack.takeLast();

	refreshFields();
	refreshUndoButtons();
	schedulePreviewRefresh();
}

void CalendarDesignerDialog::refreshUndoButtons()
{
	undoButton->setEnabled(!undoStack.isEmpty());
	redoButton->setEnabled(!redoStack.isEmpty());
}

/* --- ways in -------------------------------------------------------------------------------- */

void openCalendarDesignerFor(obs_source_t *source)
{
	if (!source)
		return;

	const auto existing = calendarRegistry().constFind(source);
	if (existing != calendarRegistry().constEnd()) {
		existing.value()->show();
		existing.value()->raise();
		existing.value()->activateWindow();
		return;
	}

	auto *dialog = new CalendarDesignerDialog(source, mainWindow());
	dialog->show();
}

void openCalendarDesignerForAsync(obs_source_t *source)
{
	if (!source)
		return;

	/* Weak, so a source destroyed between the hotkey and the queued call opens nothing. */
	OBSWeakSource weak = OBSGetWeakRef(source);
	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[weak] {
			OBSSourceAutoRelease strong = obs_weak_source_get_source(weak);
			if (strong)
				openCalendarDesignerFor(strong);
		},
		Qt::QueuedConnection);
}

void registerCalendarDesignerToolsMenu(const char *sourceId)
{
	auto *action = static_cast<QAction *>(
		obs_frontend_add_tools_menu_qaction(obs_module_text("ToolsMenu.CalendarDesigner")));
	if (!action)
		return;

	auto *menu = new QMenu(mainWindow());
	action->setMenu(menu);

	const QByteArray id(sourceId);
	QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, id] { fillCalendarMenu(menu, id); });

	/* Filled once here too: an empty submenu is drawn as an unusable one by the macOS menu bar. */
	fillCalendarMenu(menu, id);
}

void closeCalendarDesignerFor(obs_source_t *source)
{
	const auto found = calendarRegistry().constFind(source);
	if (found == calendarRegistry().constEnd())
		return;

	found.value()->close();
}

} // namespace closingtime
