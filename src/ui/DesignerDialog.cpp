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

#include "ui/DesignerDialog.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDropEvent>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QToolTip>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

#include "model/StyleLibrary.hpp"
#include "render/AnimatedLogo.hpp"
#include "render/FontResolution.hpp"
#include "render/RenderThread.hpp"
#include "render/RollTimeline.hpp"
#include "ui/AddSectionGallery.hpp"
#include "ui/DesignerWidgets.hpp"
#include "ui/FontDialog.hpp"
#include "ui/PreviewWidget.hpp"
#include "ui/SectionEditor.hpp"
#include "ui/StyleLibraryDialog.hpp"
#include "ui/ToolButtons.hpp"
#include "util/NaturalOrder.hpp"

namespace closingtime {

namespace {

QString moduleText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

/* Milliseconds of quiet before an edit triggers a re-render of the preview strip. */
constexpr int kPreviewDebounceMs = 250;

/*
 * Milliseconds of quiet before edits to a linked preset are written to the library file.
 *
 * Long enough that typing a font name is one write rather than a dozen, short enough that another
 * OBS window watching the file sees the change while the user is still looking at this one.
 */
constexpr int kLibraryWriteDebounceMs = 400;

/* Milliseconds of quiet before a run of small edits becomes its own undo step. */
constexpr int kEditBurstMs = 900;

/*
 * What a row of the section list carries, beside its display text.
 *
 * Set on the row of a sticky block that has children, and read by the list to tell a click on the
 * fold arrow from a click on the row. The list has to answer that itself -- a press is where a
 * drag starts, so there is nothing later to ask.
 */
constexpr int kBlockRowRole = Qt::UserRole + 1;
/* The section's type, which picks its badge. */
constexpr int kTypeRole = Qt::UserRole + 2;
/* The row's second line: "Bridged · 6 rows". */
constexpr int kDetailRole = Qt::UserRole + 3;
/* Whether the section is in the roll. */
constexpr int kVisibleRole = Qt::UserRole + 4;
/* 0 for a section of the roll, 1 for one inside a sticky block. */
constexpr int kDepthRole = Qt::UserRole + 5;
/* Spacers and Actions, which take no words and are drawn as thin markers between the rest. */
constexpr int kMarkerRole = Qt::UserRole + 6;
/* 0 when the row does not fold, 1 when its block is open, 2 when it is folded. */
constexpr int kFoldRole = Qt::UserRole + 7;
/* The section's own name, which is empty when it names itself; what an in-place rename starts from. */
constexpr int kLabelRole = Qt::UserRole + 8;
/* True on the last child of a block, whose guide line stops at its own row. */
constexpr int kLastChildRole = Qt::UserRole + 9;

/* Where each part of a row sits. Shared by the painting and by the list's own hit tests. */
struct RowGeometry {
	QRect fold;
	QRect badge;
	QRect text;
	QRect eye;
};

constexpr int kRowFoldWidth = 14;
constexpr int kRowIndent = 18;
constexpr int kRowEyeWidth = 22;

RowGeometry rowGeometry(const QRect &row, int depth, bool marker)
{
	RowGeometry geometry;
	int x = row.left() + 4 + depth * kRowIndent;

	geometry.fold = QRect(x, row.top(), kRowFoldWidth, row.height());
	x += kRowFoldWidth;

	const int badge = marker ? 16 : 22;
	geometry.badge = QRect(x + (22 - badge) / 2, row.center().y() - badge / 2, badge, badge);
	x += 22 + 8;

	geometry.eye = QRect(row.right() - 4 - kRowEyeWidth, row.top(), kRowEyeWidth, row.height());
	geometry.text = QRect(x, row.top(), std::max(10, geometry.eye.left() - 4 - x), row.height());
	return geometry;
}

/*
 * Draws a row of the section list: the section's badge, its name over a line of detail, and the
 * eye that shows or hides it. Spacers and Actions are thin rows with a dashed rule, read as
 * events in the roll rather than as content. A block's children hang off a guide line.
 *
 * Also the in-place editor for a rename, which reports through `onRename` rather than writing the
 * model: the document is what holds the name, and the list is only ever rebuilt from it.
 */
class SectionRowDelegate : public QStyledItemDelegate {
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	std::function<void(int row, const QString &name)> onRename;

	QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		const QFontMetrics metrics(option.font);
		if (index.data(kMarkerRole).toBool())
			return QSize(160, std::max(24, metrics.height() + 8));
		return QSize(160, std::max(40, metrics.height() * 2 + 10));
	}

	void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
	{
		painter->save();
		painter->setRenderHint(QPainter::Antialiasing);

		const QPalette &palette = option.palette;
		const bool selected = option.state & QStyle::State_Selected;
		const bool hovered = option.state & QStyle::State_MouseOver;
		const bool visible = index.data(kVisibleRole).toBool();
		const bool marker = index.data(kMarkerRole).toBool();
		const int depth = index.data(kDepthRole).toInt();
		const QRect row = option.rect.adjusted(2, 1, -2, -1);
		const RowGeometry geometry = rowGeometry(row, depth, marker);

		if (selected) {
			QColor fill = palette.color(QPalette::Highlight);
			fill.setAlpha(60);
			painter->setPen(QPen(palette.color(QPalette::Highlight), 1));
			painter->setBrush(fill);
			painter->drawRoundedRect(QRectF(row).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
		} else if (hovered) {
			painter->setPen(Qt::NoPen);
			painter->setBrush(palette.color(QPalette::AlternateBase));
			painter->drawRoundedRect(row, 5, 5);
		}

		QColor text = palette.color(QPalette::Text);
		QColor dim = text;
		dim.setAlphaF(0.6f);

		/* A child's guide line, down from its block's badge and across to its own. */
		if (depth > 0) {
			const int x = row.left() + 4 + kRowFoldWidth + 11;
			QColor guide = palette.color(QPalette::Mid);
			painter->setPen(QPen(guide, 1));
			const bool last = index.data(kLastChildRole).toBool();
			painter->drawLine(QPointF(x + 0.5, option.rect.top()),
					  QPointF(x + 0.5, last ? row.center().y() : option.rect.bottom() + 1));
			painter->drawLine(QPointF(x + 0.5, row.center().y() + 0.5),
					  QPointF(geometry.badge.left() - 3, row.center().y() + 0.5));
		}

		const int fold = index.data(kFoldRole).toInt();
		if (fold != 0) {
			const QPointF c = QRectF(geometry.fold).center();
			QPainterPath arrow;
			if (fold == 1) {
				arrow.moveTo(c.x() - 4, c.y() - 2);
				arrow.lineTo(c.x() + 4, c.y() - 2);
				arrow.lineTo(c.x(), c.y() + 3);
			} else {
				arrow.moveTo(c.x() - 2, c.y() - 4);
				arrow.lineTo(c.x() + 3, c.y());
				arrow.lineTo(c.x() - 2, c.y() + 4);
			}
			arrow.closeSubpath();
			painter->fillPath(arrow, dim);
		}

		painter->setOpacity(visible ? 1.0 : 0.45);
		paintTypeBadge(*painter, geometry.badge, static_cast<SectionType>(index.data(kTypeRole).toInt()));

		const QString name = index.data(Qt::DisplayRole).toString();
		const QString detail = visible ? index.data(kDetailRole).toString()
					       : moduleText("Designer.Detail.Hidden");
		const QFontMetrics metrics(option.font);

		if (marker) {
			const QString line = detail.isEmpty() ? name : QStringLiteral("%1 · %2").arg(name, detail);
			const QString shown = metrics.elidedText(line, Qt::ElideRight, geometry.text.width());
			painter->setPen(dim);
			painter->drawText(geometry.text, Qt::AlignLeft | Qt::AlignVCenter, shown);

			const int after = geometry.text.left() + metrics.horizontalAdvance(shown) + 8;
			if (after < geometry.text.right()) {
				QPen dashed(palette.color(QPalette::Mid), 1);
				dashed.setStyle(Qt::DashLine);
				painter->setPen(dashed);
				painter->drawLine(QPointF(after, row.center().y() + 0.5),
						  QPointF(geometry.text.right(), row.center().y() + 0.5));
			}
		} else {
			const int lineHeight = metrics.height();
			const int top = row.center().y() - lineHeight;
			painter->setPen(text);
			painter->drawText(QRect(geometry.text.left(), top, geometry.text.width(), lineHeight),
					  Qt::AlignLeft | Qt::AlignVCenter,
					  metrics.elidedText(name, Qt::ElideRight, geometry.text.width()));

			QFont small = option.font;
			small.setPointSizeF(std::max(6.5, small.pointSizeF() * 0.88));
			painter->setFont(small);
			painter->setPen(dim);
			painter->drawText(
				QRect(geometry.text.left(), top + lineHeight, geometry.text.width(), lineHeight),
				Qt::AlignLeft | Qt::AlignVCenter,
				QFontMetrics(small).elidedText(detail, Qt::ElideRight, geometry.text.width()));
		}

		painter->setOpacity(1.0);

		/* The eye shows on the row being pointed at or worked on, and always on a hidden one. */
		if (hovered || selected || !visible)
			paintEye(*painter, geometry.eye, visible, visible ? dim : QColor(240, 164, 58));

		painter->restore();
	}

	QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &, const QModelIndex &) const override
	{
		auto *edit = new QLineEdit(parent);
		edit->setFrame(true);
		return edit;
	}

	void setEditorData(QWidget *editor, const QModelIndex &index) const override
	{
		auto *edit = static_cast<QLineEdit *>(editor);
		const QString label = index.data(kLabelRole).toString();
		edit->setText(label.isEmpty() ? index.data(Qt::DisplayRole).toString() : label);
		edit->selectAll();
	}

	void setModelData(QWidget *editor, QAbstractItemModel *, const QModelIndex &index) const override
	{
		if (onRename)
			onRename(index.row(), static_cast<QLineEdit *>(editor)->text());
	}

	void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
				  const QModelIndex &index) const override
	{
		const RowGeometry geometry = rowGeometry(option.rect.adjusted(2, 1, -2, -1),
							 index.data(kDepthRole).toInt(),
							 index.data(kMarkerRole).toBool());
		/* Over the whole of the name and its second line, so nothing of the row shows round it. */
		const QRect row = option.rect.adjusted(2, 3, -2, -3);
		editor->setGeometry(geometry.text.left() - 3, row.top(),
				    geometry.eye.right() - geometry.text.left() + 3, row.height());
	}
};

/* A handful of starting points for a new roll, offered from the Roll menu. */
struct RollTemplate {
	const char *nameKey;
	std::function<QVector<Section>()> build;
};

Section withText(SectionType type, const QString &text, const QString &secondary = QString())
{
	Section section = Section::makeDefault(type);
	section.text = text;
	if (!secondary.isEmpty())
		section.secondaryText = secondary;
	return section;
}

Section withEntries(SectionType type, const QVector<QPair<QString, QString>> &rows)
{
	Section section = Section::makeDefault(type);
	section.entries.clear();
	for (const auto &[text, secondary] : rows) {
		Entry entry;
		entry.text = text;
		entry.secondaryText = secondary;
		section.entries.append(entry);
	}
	return section;
}

const QVector<RollTemplate> &rollTemplates()
{
	static const QVector<RollTemplate> templates = {
		{"Designer.Template.Stream",
		 [] {
			 Section thanks = Section::makeDefault(SectionType::StickyBlock);
			 thanks.label = moduleText("Designer.Template.ThanksCard");
			 thanks.children.push_back(
				 withText(SectionType::Title, moduleText("Designer.Template.ThanksText")));
			 return QVector<Section>{
				 withText(SectionType::TitleWithSubtitle, moduleText("Designer.Template.ShowName"),
					  moduleText("Designer.Template.ShowTagline")),
				 withText(SectionType::Header, moduleText("Designer.Template.Team")),
				 withEntries(SectionType::Bridged, {{moduleText("Designer.Template.Host"),
								     moduleText("Designer.Template.Name")},
								    {moduleText("Designer.Template.Moderator"),
								     moduleText("Designer.Template.Name")}}),
				 Section::makeDefault(SectionType::SectionDivider),
				 withText(SectionType::Header, moduleText("Designer.Template.Supporters")),
				 withEntries(SectionType::MultiTextList,
					     {{moduleText("Designer.Template.Name"), QString()},
					      {moduleText("Designer.Template.Name"), QString()},
					      {moduleText("Designer.Template.Name"), QString()}}),
				 thanks,
			 };
		 }},
		{"Designer.Template.Film",
		 [] {
			 return QVector<Section>{
				 withText(SectionType::Title, moduleText("Designer.Template.ShowName")),
				 withText(SectionType::Header, moduleText("Designer.Template.Cast")),
				 withEntries(SectionType::Bridged, {{moduleText("Designer.Template.Role"),
								     moduleText("Designer.Template.Name")},
								    {moduleText("Designer.Template.Role"),
								     moduleText("Designer.Template.Name")}}),
				 withText(SectionType::Header, moduleText("Designer.Template.Crew")),
				 withEntries(SectionType::TitleSubtitleList, {{moduleText("Designer.Template.Role"),
									       moduleText("Designer.Template.Name")}}),
			 };
		 }},
		{"Designer.Template.Simple",
		 [] {
			 return QVector<Section>{
				 withText(SectionType::Title, moduleText("Designer.Template.ShowName")),
				 withEntries(SectionType::TextList,
					     {{moduleText("Designer.Template.Name"), QString()},
					      {moduleText("Designer.Template.Name"), QString()}}),
			 };
		 }},
	};
	return templates;
}

/* Every section with its view state cleared, for asking whether two rolls say the same thing. */
QVector<Section> withoutViewState(QVector<Section> sections)
{
	for (Section &section : sections)
		section.childrenCollapsed = false;
	return sections;
}

/* True when a key is going to a field that is being typed into, where Escape means "stop typing". */
bool isTypingInto(const QWidget *widget)
{
	return qobject_cast<const QLineEdit *>(widget) || qobject_cast<const QAbstractSpinBox *>(widget) ||
	       qobject_cast<const QPlainTextEdit *>(widget) || qobject_cast<const QTextEdit *>(widget) ||
	       qobject_cast<const QComboBox *>(widget);
}

/* How many steps back the designer can go before the oldest is forgotten. */
constexpr int kUndoDepth = 100;

/*
 * Opening widths of the section list, the editor and the preview, in pixels.
 *
 * The editor takes the lion's share: it is the pane the roll is actually built in, and the one
 * carrying tables whose columns have to be read across. The list needs only enough width for a
 * section's label and the preview only enough to show the shape of the roll -- and either can be
 * folded away entirely when the editor wants even that.
 */
const QList<int> kDefaultPaneSizes = {260, 720, 300};

/*
 * One designer window per source. Keyed by the raw pointer purely for identity; the
 * pointer is never dereferenced, and each dialog holds a weak reference for actual access.
 */
QHash<obs_source_t *, DesignerDialog *> &designerRegistry()
{
	static QHash<obs_source_t *, DesignerDialog *> registry;
	return registry;
}

QWidget *mainWindow()
{
	return static_cast<QWidget *>(obs_frontend_get_main_window());
}

/*
 * One source the Tools submenu offers. The reference is weak because the menu holds these for as
 * long as it is open: a strong one would keep a roll the user deleted mid-menu alive behind their
 * back, and the entry has to cope with the source going away regardless.
 */
struct MenuEntry {
	QString name;
	OBSWeakSource source;
};

struct MenuCollector {
	QByteArray sourceId;
	QVector<MenuEntry> entries;
};

/*
 * Every source of one type in the scene collection, whichever scene each sits in --
 * `obs_enum_sources` walks the collection's inputs rather than the current scene's items, which
 * is what makes a roll parked in a scene the user is not looking at reachable from here.
 */
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

	/*
	 * Listed by name, in natural order: libobs hands them over in creation order, which reads as no
	 * order at all, and a plain string sort puts "Credits 10" before "Credits 2".
	 */
	std::sort(collector.entries.begin(), collector.entries.end(),
		  [](const MenuEntry &a, const MenuEntry &b) { return naturalLess(a.name, b.name); });

	return collector.entries;
}

/* Rebuilds the Tools submenu from what is loaded right now. */
void fillDesignerMenu(QMenu *menu, const QByteArray &sourceId)
{
	menu->clear();

	const QVector<MenuEntry> entries = sourcesOfType(sourceId);
	if (entries.isEmpty()) {
		/* A disabled line rather than an empty menu, which reads as a broken one. */
		menu->addAction(moduleText("Designer.NoSources"))->setEnabled(false);
		return;
	}

	for (const MenuEntry &entry : entries) {
		QAction *open = menu->addAction(entry.name);

		/* Upgraded when picked, so a source destroyed while the menu is open opens nothing. */
		QObject::connect(open, &QAction::triggered, menu, [source = entry.source] {
			OBSSourceAutoRelease strong = obs_weak_source_get_source(source);
			if (strong)
				openDesignerFor(strong);
		});
	}
}

} // namespace

void openDesignerFor(obs_source_t *source)
{
	if (!source)
		return;

	const auto existing = designerRegistry().constFind(source);
	if (existing != designerRegistry().constEnd()) {
		existing.value()->show();
		existing.value()->raise();
		existing.value()->activateWindow();
		return;
	}

	auto *dialog = new DesignerDialog(source, mainWindow());
	dialog->show();
}

void openDesignerForAsync(obs_source_t *source)
{
	if (!source)
		return;

	/*
	 * A hotkey callback runs on the hotkey thread, and a window can only be opened on the UI
	 * one, so the request is queued rather than serviced where it lands. The weak reference is
	 * what makes a source destroyed in between the two ends of that queue a no-op.
	 */
	auto *weak = new OBSWeakSourceAutoRelease(obs_source_get_weak_source(source));
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const std::unique_ptr<OBSWeakSourceAutoRelease> held(
				static_cast<OBSWeakSourceAutoRelease *>(param));

			OBSSourceAutoRelease strong = obs_weak_source_get_source(*held);
			if (strong)
				openDesignerFor(strong);
		},
		weak, false);
}

void registerDesignerToolsMenu(const char *sourceId)
{
	/*
	 * The qaction form rather than the plain menu item, because what goes in the Tools menu is
	 * a submenu rather than something to click: a roll is picked by name from a list of every
	 * one in the collection, which is a menu's own job and does not need a dialog to do it.
	 */
	auto *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ToolsMenu.Designer")));
	if (!action)
		return;

	/* Outlives the plugin's own objects, so it hangs off the window rather than off anything here. */
	auto *menu = new QMenu(mainWindow());
	action->setMenu(menu);

	const QByteArray id(sourceId);
	QObject::connect(menu, &QMenu::aboutToShow, menu, [menu, id] { fillDesignerMenu(menu, id); });

	/*
	 * Filled once here as well, even though there is nothing to find at module load: an empty
	 * submenu is drawn as an unusable one by the macOS menu bar, which would leave the entry
	 * looking broken until something happened to open it. The placeholder is enough to keep it
	 * a submenu, and the first hover replaces it with what is actually loaded.
	 */
	fillDesignerMenu(menu, id);
}

void registerStyleLibraryToolsMenu()
{
	auto *action =
		static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("ToolsMenu.StyleLibrary")));
	if (!action)
		return;

	QObject::connect(action, &QAction::triggered, action, [] {
		/*
		 * Opened on no document: with no roll to publish from or link into, the window is the
		 * library alone. Parented to the main window and deleted on close, so it behaves like
		 * any other utility window rather than outliving the menu entry that opened it.
		 */
		auto *dialog = new StyleLibraryDialog(nullptr, mainWindow());
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->show();
	});
}

void closeDesignerFor(obs_source_t *source)
{
	if (!source)
		return;

	/*
	 * Sources are destroyed on the graphics thread, so the window cannot be touched here.
	 * The task closes the dialog only once its weak reference has gone dead, which also
	 * covers the case where a later source happens to reuse this address.
	 */
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			const auto it = designerRegistry().constFind(static_cast<obs_source_t *>(param));
			if (it == designerRegistry().constEnd())
				return;

			DesignerDialog *dialog = it.value();
			if (dialog->sourceIsGone())
				dialog->close();
		},
		source, false);
}

SectionListWidget::SectionListWidget(QWidget *parent) : QListWidget(parent)
{
	setDragDropMode(QAbstractItemView::InternalMove);
	setDefaultDropAction(Qt::MoveAction);
	setDropIndicatorShown(true);
	setSelectionMode(QAbstractItemView::SingleSelection);
	/* A row is renamed where it is read: double-click it, or F2. */
	setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	setMouseTracking(true);
	viewport()->setAttribute(Qt::WA_Hover);
	setFrameShape(QFrame::NoFrame);
	setSpacing(1);
}

void SectionListWidget::mousePressEvent(QMouseEvent *event)
{
	/*
	 * A press on a row's eye or a block's fold arrow does that and nothing else: it does not
	 * select the row, and it does not start a drag. Answered here rather than on release because a
	 * press is where the drag machinery starts from, and a fold that only happened when nothing was
	 * dragged would be a fold that sometimes did not happen.
	 */
	const QModelIndex index = indexAt(event->position().toPoint());

	if (event->button() == Qt::LeftButton && index.isValid()) {
		const RowGeometry geometry = rowGeometry(visualRect(index).adjusted(2, 1, -2, -1),
							 index.data(kDepthRole).toInt(),
							 index.data(kMarkerRole).toBool());
		const QPoint at = event->position().toPoint();

		if (geometry.eye.contains(at)) {
			emit visibilityToggled(index.row());
			return;
		}

		if (index.data(kBlockRowRole).toBool() && geometry.fold.adjusted(-4, 0, 4, 0).contains(at)) {
			emit blockFoldToggled(index.row());
			return;
		}
	}

	QListWidget::mousePressEvent(event);
}

void SectionListWidget::dropEvent(QDropEvent *event)
{
	const int from = currentRow();

	int to = indexAt(event->position().toPoint()).row();
	if (to < 0)
		to = count();
	else if (dropIndicatorPosition() == QAbstractItemView::BelowItem)
		++to;

	/*
	 * QListWidget is left out of it entirely. The base implementation would move the item
	 * itself, and the drag machinery would then remove the original row on a MoveAction,
	 * both of which fight the rebuild that follows from the document.
	 */
	event->setDropAction(Qt::IgnoreAction);
	event->accept();

	/* Dropping either side of where it already is changes nothing. */
	if (from < 0 || to == from || to == from + 1)
		return;

	emit rowMoved(from, to > from ? to - 1 : to);
}

DesignerDialog::DesignerDialog(obs_source_t *source, QWidget *parent) : QDialog(parent)
{
	weakSource = OBSGetWeakRef(source);
	designerRegistry().insert(source, this);
	sink->dialog = this;

	/* The [*] is where Qt puts the asterisk while there are changes not yet applied. */
	setWindowTitle(QStringLiteral("%1 - %2[*]")
			       .arg(moduleText("Designer.Title"), QString::fromUtf8(obs_source_get_name(source))));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(1280, 800);

	setStyleSheet(designerStyleSheet());

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(8, 6, 8, 8);
	outer->setSpacing(6);

	/*
	 * --- top: the toolbar ---
	 *
	 * Undo and redo where people look for them, the roll's running time and anything that will not
	 * come out as designed, the roll-wide tools in one menu, and saving. Saving is up here rather
	 * than in a footer because it is the thing the whole window is working towards, and the dot
	 * beside it says whether there is anything to save.
	 */
	auto *toolbar = new QHBoxLayout();
	toolbar->setSpacing(6);
	outer->addLayout(toolbar);

	const QColor iconColor = palette().color(QPalette::ButtonText);
	undoButton = new QToolButton(this);
	undoButton->setIcon(undoIcon(false, iconColor));
	undoButton->setAutoRaise(true);
	undoButton->setToolTip(moduleText("Designer.Undo.Tip"));
	undoButton->setShortcut(QKeySequence::Undo);
	redoButton = new QToolButton(this);
	redoButton->setIcon(undoIcon(true, iconColor));
	redoButton->setAutoRaise(true);
	redoButton->setToolTip(moduleText("Designer.Redo.Tip"));
	/*
	 * These take Ctrl+Z / Ctrl+Shift+Z away from the text fields' own undo. That is the
	 * intent: one stack covering the whole document beats two that disagree about what the
	 * last change was, and an undo step here already restores the text along with it.
	 */
	redoButton->setShortcut(QKeySequence::Redo);
	toolbar->addWidget(undoButton);
	toolbar->addWidget(redoButton);

	auto *separator = new QFrame(this);
	separator->setFrameShape(QFrame::VLine);
	separator->setFrameShadow(QFrame::Sunken);
	toolbar->addWidget(separator);

	durationPill = new QLabel(this);
	durationPill->setObjectName(QStringLiteral("designerPill"));
	durationPill->setToolTip(moduleText("Designer.Duration.Tip"));
	toolbar->addWidget(durationPill);

	warningChip = new QToolButton(this);
	warningChip->setObjectName(QStringLiteral("designerWarning"));
	warningChip->setCursor(Qt::PointingHandCursor);
	warningChip->hide();
	toolbar->addWidget(warningChip);

	toolbar->addStretch(1);

	auto *rollButton = new QToolButton(this);
	rollButton->setObjectName(QStringLiteral("designerMenuButton"));
	rollButton->setText(moduleText("Designer.RollMenu") + QStringLiteral(" ▾"));
	rollButton->setPopupMode(QToolButton::InstantPopup);
	rollButton->setToolTip(moduleText("Designer.RollMenu.Tip"));
	auto *rollMenu = new QMenu(rollButton);
	QAction *libraryAction = rollMenu->addAction(moduleText("Designer.StyleLibrary"));
	libraryAction->setToolTip(moduleText("Designer.StyleLibrary.Tip"));
	QAction *fontsAction = rollMenu->addAction(moduleText("Designer.Fonts"));
	fontsAction->setToolTip(moduleText("Designer.Fonts.Tip"));
	rollMenu->addSeparator();
	QMenu *templateMenu = rollMenu->addMenu(moduleText("Designer.Template"));
	for (int index = 0; index < rollTemplates().size(); ++index) {
		QAction *action = templateMenu->addAction(moduleText(rollTemplates().at(index).nameKey));
		connect(action, &QAction::triggered, this, [this, index] { startFromTemplate(index); });
	}
	rollMenu->addSeparator();
	QAction *importAction = rollMenu->addAction(moduleText("Designer.ImportJson"));
	QAction *exportAction = rollMenu->addAction(moduleText("Designer.ExportJson"));
	rollMenu->setToolTipsVisible(true);
	rollButton->setMenu(rollMenu);
	toolbar->addWidget(rollButton);

	auto *separator2 = new QFrame(this);
	separator2->setFrameShape(QFrame::VLine);
	separator2->setFrameShadow(QFrame::Sunken);
	toolbar->addWidget(separator2);

	dirtyMark = new QWidget(this);
	{
		auto *dirtyLayout = new QHBoxLayout(dirtyMark);
		dirtyLayout->setContentsMargins(0, 0, 4, 0);
		dirtyLayout->setSpacing(4);
		auto *dot = new QLabel(dirtyMark);
		dot->setPixmap(changedDotIcon().pixmap(QSize(12, 12)));
		auto *text = new QLabel(moduleText("Designer.Unsaved"), dirtyMark);
		styleAsHint(text);
		dirtyLayout->addWidget(dot);
		dirtyLayout->addWidget(text);
	}
	dirtyMark->setToolTip(moduleText("Designer.Unsaved.Tip"));
	dirtyMark->hide();
	toolbar->addWidget(dirtyMark);

	auto *cancelButton = new QPushButton(moduleText("Designer.Cancel"), this);
	cancelButton->setFlat(true);
	applyButton = new QPushButton(moduleText("Designer.Apply"), this);
	applyButton->setToolTip(moduleText("Designer.Apply.Tip"));
	doneButton = new QPushButton(moduleText("Designer.Done"), this);
	doneButton->setObjectName(QStringLiteral("designerPrimary"));
	doneButton->setToolTip(moduleText("Designer.Done.Tip"));
	/*
	 * None of them is the default button: Enter belongs to whatever is being typed into, and a
	 * window that closed on an Enter pressed in the wrong field would be exactly the kind of
	 * surprise this one is meant to be free of. Ctrl+Enter is Done.
	 */
	for (QPushButton *button : {cancelButton, applyButton, doneButton}) {
		button->setAutoDefault(false);
		button->setDefault(false);
		toolbar->addWidget(button);
	}

	splitter = new QSplitter(Qt::Horizontal, this);
	/* Folding a pane away is the collapse button's job now, not something a drag can do. */
	splitter->setChildrenCollapsible(false);

	/* --- left: the section list, in roll order --- */
	listPane = new QWidget(splitter);
	auto *listLayout = new QVBoxLayout(listPane);
	listLayout->setContentsMargins(0, 0, 4, 0);
	listLayout->setSpacing(6);

	auto *listHeader = new QHBoxLayout();
	sectionsLabel = new QLabel(moduleText("Designer.Sections"), listPane);
	sectionsLabel->setObjectName(QStringLiteral("designerPaneTitle"));
	collapseButton = new QToolButton(listPane);
	collapseButton->setAutoRaise(true);
	collapseButton->setArrowType(Qt::LeftArrow);
	collapseButton->setToolTip(moduleText("Designer.Sections.Collapse"));
	listHeader->addWidget(sectionsLabel, 1);
	listHeader->addWidget(collapseButton);
	listLayout->addLayout(listHeader);

	/* Finds a section in a long roll by what it is called or what kind it is. */
	filterEdit = new QLineEdit(listPane);
	filterEdit->setPlaceholderText(moduleText("Designer.Filter"));
	filterEdit->setClearButtonEnabled(true);
	listLayout->addWidget(filterEdit);

	sectionList = new SectionListWidget(listPane);
	auto *rowDelegate = new SectionRowDelegate(sectionList);
	rowDelegate->onRename = [this](int row, const QString &name) {
		/* Queued: the list is still closing its editor, and the rename rebuilds the list. */
		QMetaObject::invokeMethod(this, [this, row, name] { renameRow(row, name); }, Qt::QueuedConnection);
	};
	sectionList->setItemDelegate(rowDelegate);
	sectionList->setContextMenuPolicy(Qt::CustomContextMenu);
	listLayout->addWidget(sectionList, 1);

	listButtonRow = new QWidget(listPane);
	auto *listButtons = new QHBoxLayout(listButtonRow);
	listButtons->setContentsMargins(0, 0, 0, 0);

	/*
	 * One clear way to add a section, opening a gallery of what each kind looks like. Everything
	 * else a row can have done to it lives on the row itself: its right-click menu, the ⋯ button
	 * beside this one, and keyboard shortcuts.
	 */
	addButton = new QToolButton(listButtonRow);
	addButton->setObjectName(QStringLiteral("designerAddSection"));
	addButton->setText(moduleText("Designer.AddSection"));
	addButton->setToolTip(moduleText("Designer.AddSection.Tip"));
	addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	addButton->setCursor(Qt::PointingHandCursor);
	listButtons->addWidget(addButton, 1);

	auto *moreButton = new QToolButton(listButtonRow);
	moreButton->setObjectName(QStringLiteral("designerMenuButton"));
	moreButton->setText(QStringLiteral("⋯"));
	moreButton->setToolTip(moduleText("Designer.MoreActions"));
	moreButton->setPopupMode(QToolButton::InstantPopup);
	listButtons->addWidget(moreButton);
	listLayout->addWidget(listButtonRow);

	gallery = new AddSectionGallery(this);
	connect(gallery, &AddSectionGallery::typeChosen, this, &DesignerDialog::addSectionOfType);
	connect(addButton, &QToolButton::clicked, this, &DesignerDialog::openAddGallery);

	splitter->addWidget(listPane);

	/*
	 * --- middle: the editor for whichever section is selected ---
	 *
	 * Added straight to the splitter rather than wrapped in a scroll area: the editor is tabbed
	 * and scrolls each of its pages itself, which is what keeps its header and its tab strip on
	 * screen while a long page is scrolled. See SectionEditor::addTab.
	 */
	editor = new SectionEditor(splitter);
	splitter->addWidget(editor);

	/* --- right: live preview of the whole strip --- */
	previewPane = new QWidget(splitter);
	auto *previewLayout = new QVBoxLayout(previewPane);
	previewLayout->setContentsMargins(4, 0, 0, 0);
	previewLayout->setSpacing(6);

	/*
	 * A header of its own, so the preview folds away to a button the way the section list does.
	 * The button sits at the pane's inner edge -- the side it folds towards -- and stays on
	 * screen when it does, since it is the one way back.
	 */
	auto *previewHeader = new QHBoxLayout();
	previewCollapseButton = new QToolButton(previewPane);
	previewCollapseButton->setAutoRaise(true);
	previewCollapseButton->setArrowType(Qt::RightArrow);
	previewCollapseButton->setToolTip(moduleText("Designer.Preview.Collapse"));
	previewLabel = new QLabel(moduleText("Designer.Preview"), previewPane);
	previewLabel->setObjectName(QStringLiteral("designerPaneTitle"));
	previewHeader->addWidget(previewCollapseButton);
	previewHeader->addWidget(previewLabel, 1);
	previewLayout->addLayout(previewHeader);

	previewBody = new QWidget(previewPane);
	auto *previewBodyLayout = new QVBoxLayout(previewBody);
	previewBodyLayout->setContentsMargins(0, 0, 0, 0);
	previewBodyLayout->setSpacing(6);
	previewLayout->addWidget(previewBody, 1);

	/* The view's own controls, over the view: zoom, the layout overlay, and animation. */
	auto *viewBar = new QHBoxLayout();
	viewBar->setSpacing(4);
	zoomControl = new SegmentedControl(previewBody);
	zoomControl->addSegment(moduleText("Designer.Preview.Fit"), moduleText("Designer.Preview.Fit.Tip"));
	zoomControl->addSegment(moduleText("Designer.Preview.Actual"), moduleText("Designer.Preview.Actual.Tip"));
	viewBar->addWidget(zoomControl);
	viewBar->addStretch();

	/*
	 * Off by default: this is a view of the layout rather than of the roll, wanted only while
	 * a section is not landing where its settings say it should.
	 */
	layoutBoxesButton = new QToolButton(previewBody);
	layoutBoxesButton->setText(moduleText("Designer.LayoutBoxes"));
	layoutBoxesButton->setToolTip(moduleText("Designer.LayoutBoxes.Tip"));
	layoutBoxesButton->setCheckable(true);
	layoutBoxesButton->setAutoRaise(true);
	viewBar->addWidget(layoutBoxesButton);

	/*
	 * Off by default, like the overlay beside it and for the same reason: this pane is what a roll
	 * is written in, and something moving in the corner of it while a name is being typed is a
	 * distraction rather than a feature. With it off every animated logo shows its first frame,
	 * which is the frame the layout was measured from.
	 */
	animateButton = new QToolButton(previewBody);
	animateButton->setText(moduleText("Designer.PlayAnimations"));
	animateButton->setToolTip(moduleText("Designer.PlayAnimations.Tip"));
	animateButton->setCheckable(true);
	animateButton->setAutoRaise(true);
	animateButton->setEnabled(false);
	viewBar->addWidget(animateButton);
	previewBodyLayout->addLayout(viewBar);

	preview = new PreviewWidget(previewBody);
	preview->setToolTip(moduleText("Designer.Preview.Tip"));
	preview->setCaches(logos, animations);
	previewBodyLayout->addWidget(preview, 1);
	/* A click on a section in the pane selects it in the list, so a long roll is edited from where it is seen. */
	connect(preview, &PreviewWidget::sectionClicked, this, [this](int section) {
		const int row = rowOf(SectionPath{-1, section});
		if (row >= 0)
			sectionList->setCurrentRow(row);
	});

	/*
	 * The transport: the roll played in the preview at its own speed, with sticky blocks pinning
	 * and holding and logos animating, as it will go to air. The scrubber marks every Action.
	 */
	auto *transport = new QHBoxLayout();
	transport->setSpacing(6);
	playButton = new QToolButton(previewBody);
	playButton->setIcon(transportIcon(TransportIcon::Play, iconColor));
	playButton->setToolTip(moduleText("Designer.Preview.Play.Tip"));
	playButton->setAutoRaise(true);
	stopButton = new QToolButton(previewBody);
	stopButton->setIcon(transportIcon(TransportIcon::Stop, iconColor));
	stopButton->setToolTip(moduleText("Designer.Preview.Stop.Tip"));
	stopButton->setAutoRaise(true);
	stopButton->setEnabled(false);
	scrubber = new TimelineScrubber(previewBody);
	timecodeLabel = new QLabel(previewBody);
	styleAsHint(timecodeLabel);
	QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	timecodeLabel->setFont(mono);
	transport->addWidget(playButton);
	transport->addWidget(stopButton);
	transport->addWidget(scrubber, 1);
	transport->addWidget(timecodeLabel);
	previewBodyLayout->addLayout(transport);

	playTimer = new QTimer(this);
	playTimer->setInterval(33);
	connect(playTimer, &QTimer::timeout, this, [this] {
		playTime += playClock.restart() / 1000.0;
		const double length = playbackLength();
		if (length > 0.0 && playTime >= length) {
			playTime = length;
			setPlaying(false);
		}
		showPlaybackAt(playTime);
	});
	connect(playButton, &QToolButton::clicked, this, [this] { setPlaying(!playing); });
	connect(stopButton, &QToolButton::clicked, this, &DesignerDialog::stopPlayback);
	connect(scrubber, &TimelineScrubber::seeked, this, [this](double seconds) {
		playTime = seconds;
		if (playing)
			playClock.restart();
		showPlaybackAt(seconds);
	});

	splitter->addWidget(previewPane);

	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 4);
	splitter->setStretchFactor(2, 2);
	/*
	 * Wider than the stretch factors alone would open it: the list carries a section's own
	 * label, and a roll's sections are told apart by reading them rather than by counting them.
	 */
	splitter->setSizes(kDefaultPaneSizes);
	outer->addWidget(splitter, 1);

	toast = new UndoToast(this);

	/*
	 * --- actions on a section ---
	 *
	 * One set, reached from the right-click menu, the ⋯ button and the keyboard alike. The ones
	 * whose keys a text field would also want (Delete, H, Alt+arrows) only listen while the list
	 * has the focus; the rest work from anywhere in the window.
	 */
	const auto makeAction = [this](const char *key, const QKeySequence &shortcut, QWidget *scope,
				       Qt::ShortcutContext context, auto slot) {
		auto *action = new QAction(moduleText(key), this);
		action->setShortcut(shortcut);
		action->setShortcutContext(context);
		scope->addAction(action);
		connect(action, &QAction::triggered, this, slot);
		return action;
	};

	addSectionAction = makeAction("Designer.AddSection.Action", QKeySequence::New, this, Qt::WindowShortcut,
				      &DesignerDialog::openAddGallery);
	renameAction = makeAction("Designer.Rename", QKeySequence(Qt::Key_F2), this, Qt::WindowShortcut,
				  &DesignerDialog::renameSelected);
	duplicateAction = makeAction("Designer.Duplicate", QKeySequence(Qt::CTRL | Qt::Key_D), this, Qt::WindowShortcut,
				     &DesignerDialog::duplicateSection);
	hideAction = makeAction("Designer.ToggleInRoll", QKeySequence(Qt::Key_H), sectionList,
				Qt::WidgetWithChildrenShortcut, [this] { toggleRowVisibility(currentRow); });
	moveUpAction = makeAction("Designer.MoveUp", QKeySequence(Qt::ALT | Qt::Key_Up), sectionList,
				  Qt::WidgetWithChildrenShortcut, [this] { moveSection(-1); });
	moveDownAction = makeAction("Designer.MoveDown", QKeySequence(Qt::ALT | Qt::Key_Down), sectionList,
				    Qt::WidgetWithChildrenShortcut, [this] { moveSection(1); });
	removeAction = makeAction("Designer.Remove", QKeySequence::Delete, sectionList, Qt::WidgetWithChildrenShortcut,
				  &DesignerDialog::removeSection);

	/* Shown on the menus, which a shortcut on a hidden widget's action would not otherwise say. */
	for (QAction *action :
	     {addSectionAction, renameAction, duplicateAction, hideAction, moveUpAction, moveDownAction, removeAction})
		action->setShortcutVisibleInContextMenu(true);

	auto *moreMenu = new QMenu(moreButton);
	moreMenu->addAction(renameAction);
	moreMenu->addAction(duplicateAction);
	moreMenu->addAction(hideAction);
	moreMenu->addSeparator();
	moreMenu->addAction(moveUpAction);
	moreMenu->addAction(moveDownAction);
	moreMenu->addSeparator();
	moreMenu->addAction(removeAction);
	moreButton->setMenu(moreMenu);
	connect(sectionList, &QWidget::customContextMenuRequested, this, &DesignerDialog::showSectionMenu);

	const auto addShortcut = [this](const QKeySequence &keys, QWidget *scope, Qt::ShortcutContext context,
					auto slot) {
		auto *shortcut = new QShortcut(keys, scope);
		shortcut->setContext(context);
		connect(shortcut, &QShortcut::activated, this, slot);
	};

	addShortcut(QKeySequence::Find, this, Qt::WindowShortcut, [this] {
		setSectionsCollapsed(false);
		filterEdit->setFocus(Qt::ShortcutFocusReason);
		filterEdit->selectAll();
	});
	addShortcut(QKeySequence::Save, this, Qt::WindowShortcut, [this] { applyChanges(); });
	addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this, Qt::WindowShortcut, [this] {
		applyChanges();
		accept();
	});
	addShortcut(QKeySequence(Qt::CTRL | Qt::Key_Enter), this, Qt::WindowShortcut, [this] {
		applyChanges();
		accept();
	});
	for (int tab = 0; tab < kEditorTabCount; ++tab) {
		addShortcut(QKeySequence(Qt::CTRL | (Qt::Key_1 + tab)), this, Qt::WindowShortcut,
			    [this, tab] { editor->showTab(static_cast<EditorTab>(tab)); });
	}
	/* Space plays and pauses -- but only from the list and the preview, never from a text field. */
	for (QWidget *scope : {static_cast<QWidget *>(sectionList), static_cast<QWidget *>(previewPane)})
		addShortcut(QKeySequence(Qt::Key_Space), scope, Qt::WidgetWithChildrenShortcut,
			    [this] { setPlaying(!playing); });

	connect(filterEdit, &QLineEdit::textChanged, this, &DesignerDialog::applyRowVisibility);
	connect(sectionList, &SectionListWidget::visibilityToggled, this, &DesignerDialog::toggleRowVisibility);
	connect(zoomControl, &SegmentedControl::currentIndexChanged, this, [this](int index) {
		preview->setZoom(index == 1 ? PreviewWidget::Zoom::Actual : PreviewWidget::Zoom::Fit);
	});
	connect(warningChip, &QToolButton::clicked, this, [this] {
		if (fontWarnings)
			openFonts();
		else
			QToolTip::showText(warningChip->mapToGlobal(QPoint(0, warningChip->height())),
					   warnings.join(QStringLiteral("\n")), warningChip);
	});
	connect(libraryAction, &QAction::triggered, this, &DesignerDialog::openStyleLibrary);
	connect(fontsAction, &QAction::triggered, this, &DesignerDialog::openFonts);
	connect(importAction, &QAction::triggered, this, &DesignerDialog::importJson);
	connect(exportAction, &QAction::triggered, this, &DesignerDialog::exportJson);
	connect(cancelButton, &QPushButton::clicked, this, &DesignerDialog::reject);
	connect(applyButton, &QPushButton::clicked, this, &DesignerDialog::applyChanges);
	connect(doneButton, &QPushButton::clicked, this, [this] {
		applyChanges();
		accept();
	});

	refreshTimer = new QTimer(this);
	refreshTimer->setSingleShot(true);
	refreshTimer->setInterval(kPreviewDebounceMs);

	editBurstTimer = new QTimer(this);
	editBurstTimer->setSingleShot(true);
	editBurstTimer->setInterval(kEditBurstMs);

	libraryWriteTimer = new QTimer(this);
	libraryWriteTimer->setSingleShot(true);
	libraryWriteTimer->setInterval(kLibraryWriteDebounceMs);

	connect(layoutBoxesButton, &QToolButton::toggled, this,
		[this](bool on) { preview->setLayoutBoxesVisible(on); });
	connect(animateButton, &QToolButton::toggled, this, [this](bool on) { preview->setAnimationPlaying(on); });
	connect(refreshTimer, &QTimer::timeout, this, &DesignerDialog::refreshPreview);
	connect(editBurstTimer, &QTimer::timeout, this, [this] { editBurstOpen = false; });
	connect(libraryWriteTimer, &QTimer::timeout, this, &DesignerDialog::flushLibraryEdits);
	connect(collapseButton, &QToolButton::clicked, this, [this] { setSectionsCollapsed(!sectionsCollapsed); });
	connect(previewCollapseButton, &QToolButton::clicked, this, [this] { setPreviewCollapsed(!previewCollapsed); });
	connect(undoButton, &QToolButton::clicked, this, &DesignerDialog::undo);
	connect(redoButton, &QToolButton::clicked, this, &DesignerDialog::redo);
	connect(sectionList, &QListWidget::currentRowChanged, this, &DesignerDialog::onSelectionChanged);
	connect(sectionList, &SectionListWidget::rowMoved, this, &DesignerDialog::moveSectionTo);
	connect(sectionList, &SectionListWidget::blockFoldToggled, this, &DesignerDialog::toggleBlockFold);
	connect(editor, &SectionEditor::changed, this, &DesignerDialog::onSectionEdited);
	connect(editor, &SectionEditor::presetSaveRequested, this, &DesignerDialog::savePreset);
	connect(editor, &SectionEditor::presetDeleteRequested, this, &DesignerDialog::deletePreset);
	connect(editor, &SectionEditor::backgroundPresetSaveRequested, this, &DesignerDialog::saveBackgroundPreset);
	connect(editor, &SectionEditor::backgroundPresetDeleteRequested, this, &DesignerDialog::deleteBackgroundPreset);

	/*
	 * A library edited in another OBS window, imported from elsewhere, or written by hand shows up
	 * here without anything being reopened. Watching the file rather than polling it is what makes
	 * a shared library feel shared: the edit lands in the preview as it is made.
	 */
	libraryWatcher = new QFileSystemWatcher(this);
	librarySerial = StyleLibrary::instance().serial();
	if (const QString path = StyleLibrary::instance().filePath(); !path.isEmpty()) {
		libraryWatcher->addPath(QFileInfo(path).absolutePath());
		if (QFileInfo::exists(path))
			libraryWatcher->addPath(path);
	}
	connect(libraryWatcher, &QFileSystemWatcher::fileChanged, this, &DesignerDialog::reloadStyleLibrary);
	connect(libraryWatcher, &QFileSystemWatcher::directoryChanged, this, &DesignerDialog::reloadStyleLibrary);

	loadFromSource();
	refreshUndoButtons();
}

void DesignerDialog::reloadStyleLibrary()
{
	StyleLibrary &library = StyleLibrary::instance();

	/*
	 * Watching a file that is replaced rather than written in place -- which is what a save
	 * through QSaveFile does -- drops the watch with it, so the path is re-added on every
	 * notification. The directory watch is what catches a library created after this window
	 * opened.
	 */
	const QString path = library.filePath();
	if (!path.isEmpty() && QFileInfo::exists(path) && !libraryWatcher->files().contains(path))
		libraryWatcher->addPath(path);

	library.load();
	if (library.serial() == librarySerial)
		return;

	librarySerial = library.serial();

	if (!document.refreshLinkedPresets())
		return;

	/*
	 * Not an undo step. This is not an edit the user made here, and putting it on the stack would
	 * mean Ctrl+Z appearing to undo a change made in another window -- which it could not do, since
	 * the library would still hold the new style and the next reload would bring it straight back.
	 */
	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::flushLibraryEdits()
{
	if (pendingLibraryEdits.isEmpty() && pendingLibraryBackgroundEdits.isEmpty())
		return;

	StyleLibrary &library = StyleLibrary::instance();
	for (auto it = pendingLibraryEdits.cbegin(); it != pendingLibraryEdits.cend(); ++it)
		library.set(it.key(), it.value());
	for (auto it = pendingLibraryBackgroundEdits.cbegin(); it != pendingLibraryBackgroundEdits.cend(); ++it)
		library.setBackground(it.key(), it.value());

	pendingLibraryEdits.clear();
	pendingLibraryBackgroundEdits.clear();
	/* Our own writes must not read back as somebody else's change on the next reload. */
	librarySerial = library.serial();
}

void DesignerDialog::openFonts()
{
	/* A family typed into the section editor and not yet committed is one this window is about. */
	commitCurrentSection();

	FontDialog dialog(&document, this);
	connect(&dialog, &FontDialog::documentAboutToChange, this, [this] { beginUndoStep(); });
	connect(&dialog, &FontDialog::documentChanged, this, [this] { schedulePreviewRefresh(); });

	dialog.exec();
}

void DesignerDialog::openStyleLibrary()
{
	commitCurrentSection();

	/* The manager reads the library, so anything still queued here belongs in it first. */
	flushLibraryEdits();

	StyleLibraryDialog dialog(&document, this);
	connect(&dialog, &StyleLibraryDialog::documentAboutToChange, this, [this] { beginUndoStep(); });
	connect(&dialog, &StyleLibraryDialog::documentChanged, this, [this] {
		editor->setPresets(document.stylePresets);
		editor->setBackgroundPresets(document.backgroundPresets);
		schedulePreviewRefresh();
	});

	dialog.exec();

	/* The library's own serial moved if anything was published, imported or deleted in there. */
	librarySerial = StyleLibrary::instance().serial();
}

DesignerDialog::~DesignerDialog()
{
	/*
	 * An edit to a shared style is not this window's to discard: Cancel drops the roll's own
	 * changes, but "change it everywhere" was answered about the library, and the library is not
	 * what Cancel is about.
	 */
	flushLibraryEdits();

	/* Any render still in flight now has nowhere to deliver to, and quietly drops itself. */
	sink->dialog = nullptr;

	for (auto it = designerRegistry().begin(); it != designerRegistry().end(); ++it) {
		if (it.value() == this) {
			designerRegistry().erase(it);
			break;
		}
	}
}

bool DesignerDialog::sourceIsGone() const
{
	return obs_weak_source_expired(weakSource);
}

void DesignerDialog::loadFromSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	OBSDataAutoRelease settings = obs_source_get_settings(source);
	document.load(settings);

	/*
	 * A new roll starts with a heading, a heading for a run of credits, and the run itself -- the
	 * shape most rolls take -- rather than one lone title to work out the rest from. The Roll menu
	 * offers fuller starting points.
	 */
	if (document.sections.isEmpty()) {
		document.sections.append(Section::makeDefault(SectionType::Title));
		document.sections.append(Section::makeDefault(SectionType::Header));
		document.sections.append(Section::makeDefault(SectionType::TextList));
	}

	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	refreshSectionList(0);
	markSaved();
	refreshPreview();
}

void DesignerDialog::writeToSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	if (!source)
		return;

	/*
	 * The properties window owns canvas size, speed and the ending action; the designer
	 * owns content. Re-reading the live settings before writing means edits made in the
	 * properties window while this dialog was open are not clobbered on Apply.
	 */
	OBSDataAutoRelease settings = obs_source_get_settings(source);

	Document merged;
	merged.load(settings);
	merged.sections = document.sections;
	merged.stylePresets = document.stylePresets;
	merged.backgroundPresets = document.backgroundPresets;
	merged.bundleFonts = document.bundleFonts;
	merged.fontSubstitutions = document.fontSubstitutions;
	merged.bundledFonts = document.bundledFonts;

	/*
	 * Applying is where the roll's fonts are collected, because it is the one moment the content
	 * is settled and the window is not being typed into: a font added to a heading two keystrokes
	 * ago is in the bundle by the time the source is told about it, and a family that has not
	 * changed costs a comparison rather than a walk of the machine's font directories.
	 */
	merged.refreshFontBundle();
	document.bundledFonts = merged.bundledFonts;

	OBSDataAutoRelease updated = obs_data_create();
	merged.save(updated);
	obs_source_update(source, updated);

	markSaved();
}

void DesignerDialog::applyChanges()
{
	commitCurrentSection();
	flushLibraryEdits();
	writeToSource();
}

bool DesignerDialog::isDirty()
{
	commitCurrentSection();

	const DocumentSnapshot now = snapshot();
	const bool sameExtras =
		now.stylePresets == savedState.stylePresets && now.backgroundPresets == savedState.backgroundPresets &&
		now.bundleFonts == savedState.bundleFonts && now.fontSubstitutions == savedState.fontSubstitutions;
	if (!sameExtras)
		return true;

	/* Shared data compares in an instant; only a real difference pays for the view state to be cleared. */
	if (now.sections == savedState.sections)
		return false;
	return withoutViewState(now.sections) != withoutViewState(savedState.sections);
}

void DesignerDialog::refreshDirty()
{
	dirty = isDirty();
	dirtyMark->setVisible(dirty);
	setWindowModified(dirty);
}

void DesignerDialog::markSaved()
{
	commitCurrentSection();
	savedState = snapshot();
	refreshDirty();
}

void DesignerDialog::reject()
{
	/*
	 * A source deleted under the window has nowhere to save to, so there is nothing to ask; and
	 * a roll with nothing unapplied has nothing to lose.
	 */
	if (sourceIsGone() || !isDirty()) {
		QDialog::reject();
		return;
	}

	QMessageBox box(this);
	box.setWindowTitle(moduleText("Designer.Title"));
	box.setIcon(QMessageBox::Question);
	OBSSourceAutoRelease source = obs_weak_source_get_source(weakSource);
	box.setText(moduleText("Designer.SaveChanges").arg(QString::fromUtf8(obs_source_get_name(source))));
	box.setInformativeText(moduleText("Designer.SaveChanges.Detail"));
	QPushButton *save = box.addButton(moduleText("Designer.SaveChanges.Save"), QMessageBox::AcceptRole);
	QPushButton *discard = box.addButton(moduleText("Designer.SaveChanges.Discard"), QMessageBox::DestructiveRole);
	QPushButton *keep = box.addButton(moduleText("Designer.SaveChanges.Keep"), QMessageBox::RejectRole);
	box.setDefaultButton(save);
	box.setEscapeButton(keep);
	box.exec();

	if (box.clickedButton() == save) {
		applyChanges();
		QDialog::accept();
	} else if (box.clickedButton() == discard) {
		QDialog::reject();
	}
}

void DesignerDialog::keyPressEvent(QKeyEvent *event)
{
	/*
	 * Escape in a field being typed into stops the typing and goes back to the list; it is not
	 * a request to close the window. Escape from anywhere else still closes it, by way of reject.
	 */
	if (event->key() == Qt::Key_Escape && isTypingInto(focusWidget())) {
		sectionList->setFocus(Qt::OtherFocusReason);
		event->accept();
		return;
	}

	QDialog::keyPressEvent(event);
}

double DesignerDialog::playbackLength() const
{
	const double finish = shownTimeline.finishTime();
	if (std::isfinite(finish))
		return finish;

	const double travel = shownTimeline.travelTime();
	if (std::isfinite(travel))
		return travel;

	/* A roll that holds for ever: as far as the last thing that happens, and a little more. */
	double last = 0.0;
	for (const RollTimeline::BlockSchedule &block : shownTimeline.schedules()) {
		for (const double moment : {block.enter, block.pin, block.holdEnd, block.release})
			if (std::isfinite(moment))
				last = std::max(last, moment);
	}
	return last + 10.0;
}

void DesignerDialog::setPlaying(bool play)
{
	if (play == playing)
		return;

	playing = play;

	if (playing) {
		/* From the top again once it has run to the end. */
		if (playTime >= playbackLength() - 0.01)
			playTime = 0.0;
		playClock.restart();
		playTimer->start();
		showPlaybackAt(playTime);
	} else {
		playTimer->stop();
	}

	playButton->setIcon(transportIcon(playing ? TransportIcon::Pause : TransportIcon::Play,
					  palette().color(QPalette::ButtonText)));
	playButton->setToolTip(moduleText(playing ? "Designer.Preview.Pause.Tip" : "Designer.Preview.Play.Tip"));
	stopButton->setEnabled(playing || preview->isShowingPlayback());
}

void DesignerDialog::showPlaybackAt(double seconds)
{
	if (!shownLayout)
		return;

	preview->showPlaybackFrame(shownTimeline.evaluate(seconds));
	scrubber->setPosition(seconds);
	timecodeLabel->setText(QStringLiteral("%1 / %2").arg(formatClock(seconds), formatClock(playbackLength())));
	stopButton->setEnabled(true);
}

void DesignerDialog::stopPlayback()
{
	setPlaying(false);
	playTime = 0.0;
	preview->clearPlayback();
	scrubber->setPosition(0.0);
	timecodeLabel->setText(QStringLiteral("%1 / %2").arg(formatClock(0.0), formatClock(playbackLength())));
	stopButton->setEnabled(false);
}

Section *DesignerDialog::sectionAt(const SectionPath &path)
{
	return const_cast<Section *>(std::as_const(*this).sectionAt(path));
}

const Section *DesignerDialog::sectionAt(const SectionPath &path) const
{
	if (path.index < 0)
		return nullptr;

	if (path.parent < 0)
		return path.index < document.sections.size() ? &document.sections.at(path.index) : nullptr;

	if (path.parent >= document.sections.size())
		return nullptr;

	const std::vector<Section> &children = document.sections.at(path.parent).children;
	return static_cast<size_t>(path.index) < children.size() ? &children[static_cast<size_t>(path.index)] : nullptr;
}

QVector<DesignerDialog::SectionPath> DesignerDialog::pathsInOrder() const
{
	QVector<SectionPath> paths;
	paths.reserve(document.sections.size());

	for (int index = 0; index < document.sections.size(); ++index) {
		paths.append(SectionPath{-1, index});

		const std::vector<Section> &children = document.sections.at(index).children;
		for (int child = 0; child < static_cast<int>(children.size()); ++child)
			paths.append(SectionPath{index, child});
	}

	return paths;
}

int DesignerDialog::rowOf(const SectionPath &path) const
{
	for (int row = 0; row < rowPaths.size(); ++row) {
		if (rowPaths.at(row).parent == path.parent && rowPaths.at(row).index == path.index)
			return row;
	}
	return -1;
}

DesignerDialog::SectionPath DesignerDialog::insertSection(const SectionPath &path, const Section &section)
{
	/*
	 * A sticky block never goes inside another one -- the model says so and the loader enforces
	 * it -- so one dropped into a block lands after it instead of vanishing into it.
	 */
	if (path.parent >= 0 && section.type == SectionType::StickyBlock) {
		const int after = std::min(path.parent + 1, static_cast<int>(document.sections.size()));
		document.sections.insert(after, section);
		return SectionPath{-1, after};
	}

	if (path.parent < 0) {
		const int at = std::clamp(path.index, 0, static_cast<int>(document.sections.size()));
		document.sections.insert(at, section);
		return SectionPath{-1, at};
	}

	if (path.parent >= document.sections.size())
		return SectionPath{};

	std::vector<Section> &children = document.sections[path.parent].children;
	const int at = std::clamp(path.index, 0, static_cast<int>(children.size()));
	children.insert(children.begin() + at, section);
	return SectionPath{path.parent, at};
}

void DesignerDialog::removeSectionAt(const SectionPath &path)
{
	if (!sectionAt(path))
		return;

	if (path.parent < 0) {
		document.sections.removeAt(path.index);
		return;
	}

	std::vector<Section> &children = document.sections[path.parent].children;
	children.erase(children.begin() + path.index);
}

void DesignerDialog::highlightCurrentSection()
{
	if (!currentPath.isValid()) {
		preview->setHighlightedSection(-1);
		return;
	}

	/* Named after what the preview outlines, which for a child is its whole block. */
	const int index = highlightFor(currentPath);
	const Section *outlined = sectionAt(SectionPath{-1, index});
	preview->setHighlightedSection(index, outlined ? outlined->displayLabel() : QString());
}

int DesignerDialog::highlightFor(const SectionPath &path) const
{
	/*
	 * The overlay and the preview know sections by their top-level index -- a child is drawn as
	 * part of its block's picture and has no box of its own out there -- so selecting a child
	 * highlights the block it belongs to.
	 */
	return path.parent >= 0 ? path.parent : path.index;
}

void DesignerDialog::fillRowItem(QListWidgetItem *item, const SectionPath &path, const Section &section) const
{
	const bool block = path.parent < 0 && section.type == SectionType::StickyBlock;
	const int held = block ? static_cast<int>(section.children.size()) : 0;

	item->setText(section.displayLabel());
	item->setToolTip(QString::fromUtf8(sectionTypeName(section.type)));
	item->setFlags(item->flags() | Qt::ItemIsEditable);
	item->setData(kTypeRole, static_cast<int>(section.type));
	item->setData(kVisibleRole, section.visible);
	item->setData(kDepthRole, path.parent >= 0 ? 1 : 0);
	item->setData(kMarkerRole, section.type == SectionType::Spacer || section.type == SectionType::Action);
	item->setData(kLabelRole, section.label);

	/*
	 * A block says how many sections it holds, which is the one thing about it a folded row cannot
	 * show for itself, and carries the arrow that folds it. A block holding nothing has nothing to
	 * fold.
	 */
	QString detail = describeSection(section);
	/* An unnamed spacer already says its height in the name it gives itself. */
	if (section.type == SectionType::Spacer && section.label.isEmpty())
		detail.clear();
	if (block)
		detail = moduleText("Designer.Detail.Holding").arg(detail).arg(held);
	item->setData(kDetailRole, detail);
	item->setData(kBlockRowRole, block && held > 0);
	item->setData(kFoldRole, block && held > 0 ? (section.childrenCollapsed ? 2 : 1) : 0);

	if (path.parent >= 0) {
		const Section *owner = sectionAt(SectionPath{-1, path.parent});
		item->setData(kLastChildRole, owner && path.index + 1 == static_cast<int>(owner->children.size()));
	}
}

void DesignerDialog::toggleBlockFold(int row)
{
	if (row < 0 || row >= rowPaths.size())
		return;

	const SectionPath path = rowPaths.at(row);
	Section *section = sectionAt(path);
	if (path.parent >= 0 || !section || section->type != SectionType::StickyBlock || section->children.empty())
		return;

	/* Whatever is half-typed into the editor belongs to the document before the list is rebuilt. */
	commitCurrentSection();

	section->childrenCollapsed = !section->childrenCollapsed;

	/*
	 * No undo step and no re-render: this is what the list is showing, not what the roll says.
	 * The selection follows a child that has just been folded away up to the block itself, which
	 * refreshSectionList does for any hidden row it is handed.
	 */
	refreshSectionList(currentRow);
}

void DesignerDialog::refreshSectionList(int selectRow)
{
	const QSignalBlocker blocker(sectionList);

	sectionList->clear();
	rowPaths = pathsInOrder();

	for (const SectionPath &path : rowPaths) {
		const Section *section = sectionAt(path);
		if (!section)
			continue;

		fillRowItem(new QListWidgetItem(sectionList), path, *section);
	}

	sectionsLabel->setText(moduleText("Designer.Sections.Count").arg(document.sections.size()));
	applyRowVisibility();

	int row = std::clamp(selectRow, -1, static_cast<int>(rowPaths.size()) - 1);

	/*
	 * Never a row nothing can see: a selection folded away leaves the editor working on a
	 * section the list is not showing, so it moves up to the block that folded it.
	 */
	if (row >= 0 && row < sectionList->count() && sectionList->item(row)->isHidden() &&
	    rowPaths.at(row).parent >= 0)
		row = rowOf(SectionPath{-1, rowPaths.at(row).parent});
	sectionList->setCurrentRow(row);
	currentRow = row;
	currentPath = row >= 0 ? rowPaths.at(row) : SectionPath{};
	/* Set here as well as on selection: this path moves the row with the list's signals blocked. */
	highlightCurrentSection();

	if (const Section *section = sectionAt(currentPath))
		editor->setSection(*section);

	editor->setEnabled(row >= 0);
}

void DesignerDialog::applyRowVisibility()
{
	const QString needle = filterEdit->text().trimmed();
	const bool filtering = !needle.isEmpty();

	const auto matches = [&needle](const QListWidgetItem *item) {
		return item->text().contains(needle, Qt::CaseInsensitive) ||
		       item->data(kDetailRole).toString().contains(needle, Qt::CaseInsensitive) ||
		       item->toolTip().contains(needle, Qt::CaseInsensitive);
	};

	for (int row = 0; row < sectionList->count() && row < rowPaths.size(); ++row) {
		QListWidgetItem *item = sectionList->item(row);
		const SectionPath &path = rowPaths.at(row);

		bool show = true;
		if (filtering) {
			/* A block is shown when anything in it matches, so a match is never left without its context. */
			show = matches(item);
			if (!show && path.parent < 0) {
				for (int child = row + 1;
				     child < rowPaths.size() && rowPaths.at(child).parent == path.index; ++child)
					show = show || matches(sectionList->item(child));
			}
		} else if (path.parent >= 0) {
			/* A folded block's children stay in the list, and out of sight; see toggleBlockFold. */
			const Section *block = sectionAt(SectionPath{-1, path.parent});
			show = !(block && block->childrenCollapsed);
		}

		item->setHidden(!show);
	}

	/* A drop is placed by the row it follows, which a filtered list cannot be trusted to show. */
	sectionList->setDragEnabled(!filtering);
}

DesignerDialog::DocumentSnapshot DesignerDialog::snapshot() const
{
	return DocumentSnapshot{document.sections,
				document.stylePresets,
				document.backgroundPresets,
				document.bundleFonts,
				document.bundledFonts,
				document.fontSubstitutions,
				currentRow};
}

void DesignerDialog::beginUndoStep()
{
	editBurstOpen = false;
	editBurstTimer->stop();

	/* A toast's Undo means the edit it was about; once anything else has happened, it would not. */
	toast->hide();

	undoStack.append(snapshot());
	if (undoStack.size() > kUndoDepth)
		undoStack.removeFirst();

	/* Any new edit invalidates the branch that was undone away from. */
	redoStack.clear();
	refreshUndoButtons();
}

void DesignerDialog::beginEditUndoStep()
{
	if (editBurstOpen) {
		editBurstTimer->start();
		return;
	}

	beginUndoStep();
	editBurstOpen = true;
	editBurstTimer->start();
}

void DesignerDialog::restore(const DocumentSnapshot &state)
{
	editBurstOpen = false;
	editBurstTimer->stop();

	document.sections = state.sections;
	document.stylePresets = state.stylePresets;
	document.backgroundPresets = state.backgroundPresets;
	document.bundleFonts = state.bundleFonts;
	document.bundledFonts = state.bundledFonts;
	document.fontSubstitutions = state.fontSubstitutions;

	/* Cleared first so refreshSectionList cannot write the editor back into a stale row. */
	currentPath = SectionPath{};
	currentRow = -1;
	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	refreshSectionList(-1);
	refreshSectionList(std::min(state.currentIndex, static_cast<int>(rowPaths.size()) - 1));
	refreshPreview();
}

void DesignerDialog::undo()
{
	if (undoStack.isEmpty())
		return;

	commitCurrentSection();
	toast->hide();
	redoStack.append(snapshot());
	restore(undoStack.takeLast());
	refreshUndoButtons();
}

void DesignerDialog::redo()
{
	if (redoStack.isEmpty())
		return;

	commitCurrentSection();
	undoStack.append(snapshot());
	restore(redoStack.takeLast());
	refreshUndoButtons();
}

void DesignerDialog::setSectionsCollapsed(bool collapsed)
{
	if (collapsed == sectionsCollapsed)
		return;

	sectionsCollapsed = collapsed;

	sectionsLabel->setVisible(!collapsed);
	filterEdit->setVisible(!collapsed);
	sectionList->setVisible(!collapsed);
	listButtonRow->setVisible(!collapsed);

	collapseButton->setArrowType(collapsed ? Qt::RightArrow : Qt::LeftArrow);
	collapseButton->setToolTip(moduleText(collapsed ? "Designer.Sections.Expand" : "Designer.Sections.Collapse"));

	setPaneFolded(listPane, collapseButton, &listExpandedWidth, collapsed);
}

void DesignerDialog::setPreviewCollapsed(bool collapsed)
{
	if (collapsed == previewCollapsed)
		return;

	previewCollapsed = collapsed;

	previewLabel->setVisible(!collapsed);
	previewBody->setVisible(!collapsed);

	/* Pointing the way the pane would go: out to the right to fold, back in to reopen. */
	previewCollapseButton->setArrowType(collapsed ? Qt::LeftArrow : Qt::RightArrow);
	previewCollapseButton->setToolTip(
		moduleText(collapsed ? "Designer.Preview.Expand" : "Designer.Preview.Collapse"));

	setPaneFolded(previewPane, previewCollapseButton, &previewExpandedWidth, collapsed);
}

void DesignerDialog::setPaneFolded(QWidget *pane, QToolButton *button, int *rememberedWidth, bool folded)
{
	const int index = splitter->indexOf(pane);
	if (index < 0)
		return;

	QList<int> sizes = splitter->sizes();

	/*
	 * The pane is pinned to the width of its own button rather than hidden outright: the button
	 * is what reopens it, so it has to stay on screen and stay where it was.
	 */
	if (folded) {
		*rememberedWidth = sizes.value(index);
		/* Re-laid out first, so the width asked for is the folded one and not the stale one. */
		pane->layout()->activate();
		pane->setMaximumWidth(std::max(button->sizeHint().width(), pane->layout()->minimumSize().width()));
		return;
	}

	pane->setMaximumWidth(QWIDGETSIZE_MAX);

	const int width = *rememberedWidth > 0 ? *rememberedWidth : kDefaultPaneSizes.value(index);
	const int editorIndex = splitter->indexOf(editor);
	if (index == editorIndex || editorIndex < 0 || sizes.size() != splitter->count()) {
		splitter->setSizes(kDefaultPaneSizes);
		return;
	}

	/*
	 * Taken out of the middle rather than shared around, so opening one pane leaves the other
	 * where the user last put it. The editor is the pane with room to spare -- and never let
	 * below nothing, which the splitter would only hand back at the next resize anyway.
	 */
	sizes[editorIndex] = std::max(0, sizes.value(editorIndex) - (width - sizes.value(index)));
	sizes[index] = width;
	splitter->setSizes(sizes);
}

void DesignerDialog::refreshUndoButtons()
{
	undoButton->setEnabled(!undoStack.isEmpty());
	redoButton->setEnabled(!redoStack.isEmpty());
}

void DesignerDialog::onSelectionChanged()
{
	commitCurrentSection();

	/* Moving to another section ends the run of edits made on the last one. */
	editBurstOpen = false;
	editBurstTimer->stop();

	const int row = sectionList->currentRow();
	currentRow = row;
	currentPath = row >= 0 && row < rowPaths.size() ? rowPaths.at(row) : SectionPath{};
	editor->setEnabled(row >= 0);
	highlightCurrentSection();

	if (const Section *section = sectionAt(currentPath))
		editor->setSection(*section);
}

void DesignerDialog::commitCurrentSection()
{
	Section *section = sectionAt(currentPath);
	if (!section)
		return;

	/*
	 * The editor knows nothing about children -- a sticky block's contents are edited as their
	 * own rows -- so what it hands back carries none, and the block's own are put back over the
	 * top of it. Writing the editor's copy through unchanged would empty the block on the first
	 * keystroke made anywhere on its form.
	 */
	std::vector<Section> children = std::move(section->children);
	*section = editor->section();
	section->children = std::move(children);
}

void DesignerDialog::onSectionEdited()
{
	beginEditUndoStep();
	commitCurrentSection();

	const Section *section = sectionAt(currentPath);
	if (section && currentRow >= 0 && currentRow < sectionList->count()) {
		const QSignalBlocker blocker(sectionList);
		fillRowItem(sectionList->item(currentRow), currentPath, *section);
	}

	schedulePreviewRefresh();
}

void DesignerDialog::duplicateSection()
{
	commitCurrentSection();

	const Section *section = sectionAt(currentPath);
	if (!section)
		return;

	beginUndoStep();
	/* Copied first: inserting into the container can move the section the pointer names. */
	Section copy = *section;
	/*
	 * Named after the name the list already shows rather than the stored label, so a section that
	 * never had one still gives the copy something the reader recognizes. The rename that follows
	 * starts from it; Enter keeps it.
	 */
	copy.label = moduleText("Designer.NameSection.CopySuffix").arg(section->displayLabel());
	const SectionPath added = insertSection(SectionPath{currentPath.parent, currentPath.index + 1}, copy);
	refreshSectionList(-1);
	refreshSectionList(rowOf(added));
	schedulePreviewRefresh();
	renameSelected();
}

void DesignerDialog::removeSection()
{
	commitCurrentSection();

	const Section *section = sectionAt(currentPath);
	if (!section)
		return;

	const QString name = section->displayLabel();
	beginUndoStep();

	const int removedRow = currentRow;
	/* Drop the stale selection first so the editor cannot write back into the gap. */
	const SectionPath removed = currentPath;
	currentPath = SectionPath{};
	currentRow = -1;
	removeSectionAt(removed);

	refreshSectionList(-1);
	refreshSectionList(std::min(removedRow, static_cast<int>(rowPaths.size()) - 1));
	schedulePreviewRefresh();

	/*
	 * No "are you sure?" first: undo covers a delete, and what a misclick needs is to be told what
	 * happened and handed the way back, there and then.
	 */
	toast->showMessage(moduleText("Designer.Removed").arg(name), [this] { undo(); });
}

void DesignerDialog::addSectionOfType(SectionType type)
{
	commitCurrentSection();
	beginUndoStep();

	/*
	 * Next to whatever is selected, in the same container: a section added while a child of a
	 * sticky block is selected joins that block, which is the only reading of "add one here" that
	 * does not surprise anyone.
	 */
	const SectionPath at = currentPath.isValid() ? SectionPath{currentPath.parent, currentPath.index + 1}
						     : SectionPath{-1, static_cast<int>(document.sections.size())};

	const SectionPath added = insertSection(at, Section::makeDefault(type));
	refreshSectionList(-1);
	refreshSectionList(rowOf(added));
	schedulePreviewRefresh();

	/*
	 * The one moment a name is worth asking for is the moment the section is made, so the new row
	 * opens for renaming in place -- Enter keeps the name it gives itself, and Escape leaves the
	 * section added with that name.
	 */
	renameSelected();
}

void DesignerDialog::openAddGallery()
{
	setSectionsCollapsed(false);

	const Section *after = sectionAt(currentPath);
	gallery->popup(addButton,
		       after ? moduleText("Designer.Gallery.InsertsBelow").arg(after->displayLabel()) : QString());
}

void DesignerDialog::renameSelected()
{
	if (currentRow < 0 || currentRow >= sectionList->count())
		return;

	setSectionsCollapsed(false);

	/* Deferred so a menu or popup that started this has gone, and the list can take the focus. */
	QTimer::singleShot(0, this, [this] {
		if (currentRow < 0 || currentRow >= sectionList->count())
			return;
		QListWidgetItem *item = sectionList->item(currentRow);
		sectionList->setFocus();
		sectionList->scrollToItem(item);
		sectionList->editItem(item);
	});
}

void DesignerDialog::renameRow(int row, const QString &typed)
{
	if (row < 0 || row >= rowPaths.size())
		return;

	const SectionPath path = rowPaths.at(row);
	Section *section = sectionAt(path);
	if (!section)
		return;

	commitCurrentSection();
	section = sectionAt(path);

	QString name = typed.trimmed();

	/*
	 * Accepting the name a section gives itself keeps it naming itself, so it goes on following
	 * what it holds rather than freezing today's words into a label.
	 */
	Section unnamed = *section;
	unnamed.label.clear();
	if (name == unnamed.displayLabel())
		name.clear();

	if (name == section->label)
		return;

	beginUndoStep();
	section->label = name;

	const QSignalBlocker blocker(sectionList);
	fillRowItem(sectionList->item(row), path, *section);
	if (row == currentRow)
		editor->setSection(*section);
	schedulePreviewRefresh();
}

void DesignerDialog::toggleRowVisibility(int row)
{
	if (row < 0 || row >= rowPaths.size())
		return;

	commitCurrentSection();

	const SectionPath path = rowPaths.at(row);
	Section *section = sectionAt(path);
	if (!section)
		return;

	beginUndoStep();
	section->visible = !section->visible;

	{
		const QSignalBlocker blocker(sectionList);
		fillRowItem(sectionList->item(row), path, *section);
	}
	if (row == currentRow)
		editor->setSection(*section);
	schedulePreviewRefresh();
}

void DesignerDialog::showSectionMenu(const QPoint &position)
{
	const int row = sectionList->indexAt(position).row();
	if (row >= 0 && row != currentRow)
		sectionList->setCurrentRow(row);

	const bool onRow = row >= 0;
	for (QAction *action : {renameAction, duplicateAction, hideAction, moveUpAction, moveDownAction, removeAction})
		action->setEnabled(onRow);

	if (const Section *section = sectionAt(currentPath))
		hideAction->setText(moduleText(section->visible ? "Designer.HideFromRoll" : "Designer.ShowInRoll"));

	QMenu menu(this);
	menu.addAction(renameAction);
	menu.addAction(duplicateAction);
	menu.addAction(hideAction);
	menu.addSeparator();
	menu.addAction(moveUpAction);
	menu.addAction(moveDownAction);
	menu.addSeparator();
	menu.addAction(addSectionAction);
	menu.addSeparator();
	menu.addAction(removeAction);
	menu.exec(sectionList->viewport()->mapToGlobal(position));

	for (QAction *action : {renameAction, duplicateAction, hideAction, moveUpAction, moveDownAction, removeAction})
		action->setEnabled(true);
	hideAction->setText(moduleText("Designer.ToggleInRoll"));
}

void DesignerDialog::startFromTemplate(int index)
{
	if (index < 0 || index >= rollTemplates().size())
		return;

	commitCurrentSection();
	beginUndoStep();

	const RollTemplate &chosen = rollTemplates().at(index);
	document.sections = chosen.build();
	currentPath = SectionPath{};
	currentRow = -1;
	refreshSectionList(-1);
	refreshSectionList(0);
	schedulePreviewRefresh();

	toast->showMessage(moduleText("Designer.Template.Started").arg(moduleText(chosen.nameKey)), [this] { undo(); });
}

void DesignerDialog::moveSection(int delta)
{
	commitCurrentSection();

	/*
	 * Within the section's own container. The buttons are for ordering a run, and a section that
	 * hopped into or out of a sticky block on its way up the list would be doing something the
	 * arrow it was clicked on never said it would; dragging is what moves a section between
	 * containers, where the drop says plainly where it landed.
	 */
	const SectionPath target{currentPath.parent, currentPath.index + delta};
	if (!sectionAt(currentPath) || !sectionAt(target))
		return;

	beginUndoStep();

	if (currentPath.parent < 0) {
		document.sections.swapItemsAt(currentPath.index, target.index);
	} else {
		std::vector<Section> &children = document.sections[currentPath.parent].children;
		std::swap(children[static_cast<size_t>(currentPath.index)],
			  children[static_cast<size_t>(target.index)]);
	}

	refreshSectionList(-1);
	refreshSectionList(rowOf(target));
	schedulePreviewRefresh();
}

void DesignerDialog::moveSectionTo(int from, int to)
{
	commitCurrentSection();

	const int count = rowPaths.size();
	if (from < 0 || from >= count || to < 0 || to >= count || from == to)
		return;

	const SectionPath fromPath = rowPaths.at(from);
	const Section *moved = sectionAt(fromPath);
	if (!moved)
		return;

	const Section section = *moved;

	beginUndoStep();
	removeSectionAt(fromPath);

	/*
	 * Where it lands is decided by the row it now follows, which is the rule the list is already
	 * showing: drop something under a sticky block, or under one of that block's own rows, and it
	 * joins the block; drop it under anything else and it is top-level. Reading the drop off what
	 * precedes it is what lets one drag both put a section into a block and take it back out,
	 * without a second gesture to learn.
	 */
	const QVector<SectionPath> after = pathsInOrder();
	const int predecessorRow = std::min(to, static_cast<int>(after.size())) - 1;

	SectionPath at{-1, 0};
	if (predecessorRow >= 0 && predecessorRow < after.size()) {
		const SectionPath &before = after.at(predecessorRow);
		const Section *precedes = sectionAt(before);

		if (before.parent >= 0) {
			at = SectionPath{before.parent, before.index + 1};
		} else if (precedes && precedes->type == SectionType::StickyBlock && !precedes->childrenCollapsed) {
			at = SectionPath{before.index, 0};
		} else if (precedes && precedes->type == SectionType::StickyBlock) {
			/*
			 * Folded, the block is one row that stands for all of it, so the gap under that
			 * row is the gap under the block -- not the way into a fold the user has just
			 * put away. Unfolding it is how something is dropped inside it.
			 */
			at = SectionPath{-1, before.index + 1};
		} else {
			at = SectionPath{-1, before.index + 1};
		}
	}

	const SectionPath landed = insertSection(at, section);
	refreshSectionList(-1);
	refreshSectionList(rowOf(landed));
	schedulePreviewRefresh();
}

bool DesignerDialog::shouldEditLinkedPreset(const QString &name, QHash<QString, bool> *choices)
{
	if (StyleLibrary::instance().alwaysEditLinked())
		return true;

	const auto answered = choices->constFind(name);
	if (answered != choices->constEnd())
		return *answered;

	QMessageBox box(this);
	box.setWindowTitle(moduleText("Library.EditLinked.Title"));
	box.setText(moduleText("Library.EditLinked").arg(name));
	box.setIcon(QMessageBox::Question);

	QPushButton *everywhere = box.addButton(moduleText("Library.EditLinked.Everywhere"), QMessageBox::YesRole);
	QPushButton *fork = box.addButton(moduleText("Library.EditLinked.Fork"), QMessageBox::NoRole);
	/* Forking is the default because it is the one that cannot surprise anybody else. */
	box.setDefaultButton(fork);

	auto *remember = new QCheckBox(moduleText("Library.EditLinked.Remember"), &box);
	box.setCheckBox(remember);

	box.exec();

	const bool editLibrary = box.clickedButton() == everywhere;
	choices->insert(name, editLibrary);

	/*
	 * Only "change everywhere" is worth remembering across windows. Someone who wants to be asked
	 * again after choosing to fork has nothing to turn back on, whereas a remembered fork would
	 * quietly make the library read-only from the editor with no way back to it.
	 */
	if (remember->isChecked() && editLibrary)
		StyleLibrary::instance().setAlwaysEditLinked(true);

	return editLibrary;
}

void DesignerDialog::savePreset(const QString &name, const TextStyle &style)
{
	/*
	 * An edit step rather than a structural one: this also fires on every keystroke and
	 * spinbox tick made while a preset is bound.
	 */
	beginEditUndoStep();

	/*
	 * A preset that follows the library is shared with every other roll on the machine, so an
	 * edit to one has to say which of the two it means: restyle everything, or take a copy for
	 * this roll. Asked once per preset per window -- see shouldEditLinkedPreset.
	 */
	for (StylePreset &preset : document.stylePresets) {
		if (preset.name != name || !preset.linked)
			continue;

		if (shouldEditLinkedPreset(name, &linkedEditChoices)) {
			pendingLibraryEdits.insert(name, style);
			libraryWriteTimer->start();
		} else {
			/* The document's own copy from here on; the library carries on as it was. */
			preset.linked = false;
		}
		break;
	}

	document.setStylePreset(name, style);

	/* Re-publishing the list is what binds the editor that raised this to the preset. */
	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::deletePreset(const QString &name)
{
	beginUndoStep();
	document.removeStylePreset(name);

	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::saveBackgroundPreset(const QString &name, const BackgroundPanel &panel)
{
	/* An edit step rather than a structural one, for the reason savePreset gives. */
	beginEditUndoStep();

	/*
	 * A panel that follows the library is shared with every other roll on the machine, so the
	 * same question is asked here that an edit to a linked style asks -- out of a memory of its
	 * own, since a panel and a style may share a name and are not the same thing to answer for.
	 */
	for (BackgroundPreset &preset : document.backgroundPresets) {
		if (preset.name != name || !preset.linked)
			continue;

		if (shouldEditLinkedPreset(name, &linkedBackgroundEditChoices)) {
			pendingLibraryBackgroundEdits.insert(name, panel);
			libraryWriteTimer->start();
		} else {
			/* The document's own copy from here on; the library carries on as it was. */
			preset.linked = false;
		}
		break;
	}

	document.setBackgroundPreset(name, panel);

	/* Re-publishing the list is what binds the editor that raised this to the preset. */
	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::deleteBackgroundPreset(const QString &name)
{
	beginUndoStep();
	document.removeBackgroundPreset(name);

	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	commitCurrentSection();
	schedulePreviewRefresh();
}

void DesignerDialog::importJson()
{
	const QString path = QFileDialog::getOpenFileName(this, moduleText("Designer.ImportJson"), QString(),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		QMessageBox::warning(this, moduleText("Designer.ImportJson"), file.errorString());
		return;
	}

	Document loaded;
	QString error;
	if (!loaded.fromJson(QString::fromUtf8(file.readAll()), &error)) {
		QMessageBox::warning(this, moduleText("Designer.ImportJson"), error);
		return;
	}

	beginUndoStep();

	/* Only the content is taken; canvas and playback settings stay as configured here. */
	document.sections = loaded.sections;
	document.stylePresets = loaded.stylePresets;
	document.backgroundPresets = loaded.backgroundPresets;
	currentPath = SectionPath{};
	currentRow = -1;
	editor->setPresets(document.stylePresets);
	editor->setBackgroundPresets(document.backgroundPresets);
	refreshSectionList(document.sections.isEmpty() ? -1 : 0);
	refreshPreview();
}

void DesignerDialog::exportJson()
{
	commitCurrentSection();

	const QString path = QFileDialog::getSaveFileName(this, moduleText("Designer.ExportJson"),
							  QStringLiteral("credits.json"),
							  QStringLiteral("JSON (*.json)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		QMessageBox::warning(this, moduleText("Designer.ExportJson"), file.errorString());
		return;
	}

	file.write(document.toJson().toUtf8());
}

void DesignerDialog::schedulePreviewRefresh()
{
	refreshTimer->start();
}

void DesignerDialog::refreshPreview()
{
	refreshDirty();

	/* Overtakes whatever layout is in flight: it gives up between sections when it sees this. */
	const uint64_t generation = ++sink->generation;

	if (previewInFlight) {
		previewAgain = true;
		return;
	}
	previewInFlight = true;

	/*
	 * The whole document goes to the render thread by value, so the user can keep editing while
	 * it is laid out. Only the layout is made here; the pane asks for the pictures it shows, at its
	 * own scale, as it shows them.
	 */
	postRenderJob([rendered = document, cache = logos, animationCache = animations, target = sink, generation,
		       previous = shownLayout] {
		const RollRenderer renderer(cache.get(), animationCache.get());
		/*
		 * The overlay's boxes are collected on every layout rather than only while it is showing,
		 * so switching it on draws what is already on screen instead of waiting on another one.
		 */
		RollLayoutPtr layout = renderer.layout(
			rendered, true, [target, generation] { return target->generation.load() != generation; },
			previous.get());

		QMetaObject::invokeMethod(
			qApp,
			[rendered, layout = std::move(layout), target] {
				if (target->dialog)
					target->dialog->applyPreview(rendered, layout);
			},
			Qt::QueuedConnection);
	});
}

void DesignerDialog::applyPreview(const Document &rendered, const RollLayoutPtr &layout)
{
	previewInFlight = false;

	/* An abandoned layout shows nothing; the edit that overtook it is laid out below. */
	if (!layout) {
		if (previewAgain) {
			previewAgain = false;
			refreshPreview();
		}
		return;
	}

	shownLayout = layout;
	preview->setLayout(layout, rendered.background);
	highlightCurrentSection();

	/*
	 * How long the roll runs is the timeline's answer, which counts what the strip's height alone
	 * cannot: a card holding at the end, a card that waits for the roll and then fades up. The
	 * transport plays the same timeline.
	 */
	shownTimeline = RollTimeline(layout->timelineInputs());
	const RollTimeline &timeline = shownTimeline;

	QString running;
	if (rendered.loop && std::isfinite(timeline.travelTime()))
		running = moduleText("Designer.DurationLoops").arg(formatClock(timeline.travelTime()));
	else if (std::isfinite(timeline.finishTime()))
		running = moduleText("Designer.Duration").arg(formatClock(timeline.finishTime()));
	else
		running = moduleText("Designer.DurationForever");

	durationPill->setText(
		QStringLiteral("%1  ·  %2")
			.arg(running, moduleText("Designer.StripHeight").arg(QLocale().toString(layout->height))));

	scrubber->setDuration(playbackLength());
	scrubber->setMarkers(timeline.cueTimes());
	if (!playing && !preview->isShowingPlayback())
		timecodeLabel->setText(QStringLiteral("%1 / %2").arg(formatClock(0.0), formatClock(playbackLength())));
	else
		showPlaybackAt(std::min(playTime, playbackLength()));

	warnings.clear();
	fontWarnings = false;

	/*
	 * Only the families nothing has been done about are a warning. One the roll carries its own
	 * file for is not missing at all by the time this is asked -- the render registered it -- and
	 * one with a stand-in recorded is reported separately, because a deliberate substitution is
	 * worth saying and is not a problem to go and fix.
	 */
	const QStringList unresolvedFonts = unresolvedFontFamilies(rendered);
	if (!unresolvedFonts.isEmpty()) {
		warnings.append(moduleText("Designer.MissingFonts").arg(unresolvedFonts.join(QStringLiteral(", "))));
		fontWarnings = true;
	}

	QStringList substituted;
	for (const QString &family : missingFontFamilies(rendered)) {
		const QString substitute = rendered.fontSubstitute(family);
		if (!substitute.isEmpty())
			substituted.append(QStringLiteral("%1 → %2").arg(family, substitute));
	}

	if (!substituted.isEmpty()) {
		warnings.append(moduleText("Designer.SubstitutedFonts").arg(substituted.join(QStringLiteral(", "))));
		fontWarnings = true;
	}

	/*
	 * Reported from the layout rather than from the document, because it is an answer only the
	 * decoder has: how long a file turned out to be once every frame of it had been read.
	 */
	const bool truncated = std::any_of(layout->animatedLogos.cbegin(), layout->animatedLogos.cend(),
					   [](const AnimatedLogoPlacement &placement) {
						   return placement.animation && placement.animation->truncated;
					   });
	if (truncated)
		warnings.append(moduleText("Designer.LogoTruncated").arg(kMaxLogoDurationMs / 1000));

	warningChip->setVisible(!warnings.isEmpty());
	warningChip->setText(warnings.size() == 1 ? moduleText("Designer.Issue")
						  : moduleText("Designer.Issues").arg(warnings.size()));
	warningChip->setToolTip(
		warnings.join(QStringLiteral("\n")) +
		(fontWarnings ? QStringLiteral("\n\n") + moduleText("Designer.Issues.FixFonts") : QString()));

	animateButton->setEnabled(preview->hasAnimatedLogos());
	if (!preview->hasAnimatedLogos() && animateButton->isChecked())
		animateButton->setChecked(false);

	/* Edits made while this render was out are picked up now, against the live document. */
	if (previewAgain) {
		previewAgain = false;
		refreshPreview();
	}
}

} // namespace closingtime
