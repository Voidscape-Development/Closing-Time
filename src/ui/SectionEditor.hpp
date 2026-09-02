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
#include <QWidget>

#include "model/CreditsModel.hpp"
#include "ui/BackgroundControls.hpp"
#include "ui/CollapsibleGroup.hpp"
#include "ui/FontPickerDialog.hpp"
#include "ui/StyleControls.hpp"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QToolButton;
class QVBoxLayout;

namespace closingtime {

/*
 * The four tabs the section's settings are dealt out into.
 *
 * The editor used to be one column of fifteen groups: what a section says, how its type is put
 * together, where it sits, five text styles, eight panels and a table -- everything about a section
 * at once, in a pane a third of a window wide. Folding the groups away made the column shorter
 * without making it any less of a column; the reader still had to scroll past the geometry to reach
 * the words.
 *
 * These four are the jobs somebody actually sits down to do, and each is a whole answer on its own:
 * fill the section in, place it, ink it, put something behind it. The order is the order the work
 * tends to happen in.
 */
enum class EditorTab { Content, Layout, Style, Background };

constexpr int kEditorTabCount = 4;

/*
 * Which of a divider's three piece stacks an editor is acting on.
 *
 * They are the same kind of list -- an end and a middle both hold `DividerPiece`s -- so one table
 * implementation serves all three and this is what says which one a call means. The order is the
 * order they are drawn in, which is also the order the tabs read in.
 */
enum class PieceSlot { LeftEnd, Center, RightEnd };

constexpr int kPieceSlotCount = 3;

/* The stack a slot names, on a section. */
const QVector<DividerPiece> &dividerPieces(const Section &section, PieceSlot slot);
QVector<DividerPiece> &dividerPieces(Section &section, PieceSlot slot);

/*
 * Everything one TextStyle carries: font, color and alignment, plus how the glyphs are
 * filled and what is drawn around them.
 */
class StyleEditor : public QWidget {
	Q_OBJECT

public:
	explicit StyleEditor(QWidget *parent = nullptr);

	void setStyle(const TextStyle &style);
	TextStyle style() const;

	/*
	 * Hides the rows a bridge has no say over -- font, size, weight, alignment, line spacing --
	 * leaving the fill, outline and shadow its ink is made of. A bridge is laid out in the row's
	 * own font whatever this editor holds (see Document::effectiveBridgeStyle), so showing those
	 * rows would offer settings that do nothing.
	 *
	 * What they hold is still carried straight through to style(), rather than read back off the
	 * hidden widgets, so a preset saved or edited from here keeps the font it already had
	 * instead of picking up whatever a hidden font box happened to resolve the family to.
	 */
	void setInkOnly(bool inkOnly);

	/*
	 * Rebinds the preset picker. `selected` is dropped when no preset carries that name,
	 * which is what makes deleting a preset unbind the editors that were showing it.
	 * `applySelectedStyle` writes the bound preset's values back into the fields; the
	 * editor a preset edit originated from passes false, because it already shows them.
	 */
	void setPresets(const QVector<StylePreset> &presets, const QString &selected, bool applySelectedStyle = true);

	/* Empty when the style is the section's own rather than a preset. */
	QString presetName() const { return selectedPreset; }

signals:
	void changed();
	/*
	 * Raised both by "save as preset" and by any field edit made while a preset is bound
	 * -- editing a bound style is how a preset, and with it every section following it,
	 * gets restyled. The owner writes the preset into the document and calls back through
	 * setPresets().
	 */
	void presetSaveRequested(const QString &name, const TextStyle &style);
	void presetDeleteRequested(const QString &name);

private:
	void writeFields(const TextStyle &style);

	/* Opens the picker, and keeps what it comes back with. */
	void pickFont();
	/* Puts the chosen family and face on the button's face. */
	void updateFontButton();
	void onPresetSelected();
	void applySelectedPreset(bool applySelectedStyle);
	void savePreset();
	void deletePreset();

	/* Shows the fill rows that apply to the selected fill and hides the rest. */
	void applyFillVisibility();
	void onFillChanged();

	/* Emits presetSaveRequested when a preset is bound and changed() when one is not. */
	void notifyEdited();

	QFormLayout *form = nullptr;

	QComboBox *presetBox = nullptr;
	QPushButton *savePresetButton = nullptr;
	QPushButton *deletePresetButton = nullptr;

	/*
	 * The whole font choice behind one button, rather than a family dropdown with a bold and an
	 * italic box beside it. A dropdown can only offer families and the two boxes can only offer
	 * a weight and a slant, which between them cannot name Semibold, Book or Condensed Light --
	 * the picker the button opens lists the faces the family actually ships.
	 */
	QPushButton *fontButton = nullptr;
	QSpinBox *pixelSize = nullptr;
	ColorButton *colorButton = nullptr;
	QComboBox *alignment = nullptr;
	QDoubleSpinBox *lineSpacing = nullptr;

	QComboBox *fillBox = nullptr;
	GradientEditor *gradientEditor = nullptr;

	QGroupBox *outlineGroup = nullptr;
	QDoubleSpinBox *outlineWidth = nullptr;
	ColorButton *outlineColor = nullptr;

	QGroupBox *shadowGroup = nullptr;
	QSpinBox *shadowOffsetX = nullptr;
	QSpinBox *shadowOffsetY = nullptr;
	QSpinBox *shadowBlur = nullptr;
	ColorButton *shadowColor = nullptr;

	QVector<StylePreset> presets;
	QString selectedPreset;

	/* Kept alongside the widgets so a gradient's stops survive a trip through Solid. */
	GradientSpec gradient;

	/*
	 * The font, held here rather than read back off a widget: a button has no state of its own,
	 * and the face name is the part of the choice a family dropdown could never have carried.
	 */
	FontChoice chosenFont;

	/*
	 * The style the fields were last filled from. style() is built on top of it, which is what
	 * carries the rows an ink-only editor hides through unchanged.
	 */
	TextStyle loaded;
	bool inkOnly = false;

	bool loading = false;
};

/*
 * Editor for a single section. One instance is reused for every section; the rows that do
 * not apply to the selected type are hidden rather than rebuilt, which keeps focus and
 * scroll position stable as the user clicks down the section list.
 */
class SectionEditor : public QWidget {
	Q_OBJECT

public:
	explicit SectionEditor(QWidget *parent = nullptr);

	void setSection(const Section &section);
	Section section() const;

	/* Republishes the document's preset list into every style editor. */
	void setPresets(const QVector<StylePreset> &presets);
	/* And the same for the panels, into every background editor. */
	void setBackgroundPresets(const QVector<BackgroundPreset> &presets);

signals:
	/* Emitted whenever the edited section changes in a way that affects the render. */
	void changed();

	/* Forwarded from whichever StyleEditor raised them; see StyleEditor. */
	void presetSaveRequested(const QString &name, const TextStyle &style);
	void presetDeleteRequested(const QString &name);

	/* And from whichever BackgroundEditor did; see BackgroundEditor. */
	void backgroundPresetSaveRequested(const QString &name, const BackgroundPanel &panel);
	void backgroundPresetDeleteRequested(const QString &name);

private:
	void applyTypeVisibility(SectionType type);

	/*
	 * Adds a row to whichever group is being built, and remembers which form owns it.
	 *
	 * The editor is one column of settings split across several collapsible groups, so a row's
	 * visibility can no longer be set on "the form" -- there are several of them. Recording the
	 * owner as the row is added is what keeps `setRowVisible` a single call at the fifty-odd
	 * places that make one, rather than each of them having to know which group its row is in.
	 */
	void addRow(const QString &label, QWidget *field);
	void setRowVisible(QWidget *field, bool visible);

	/*
	 * Builds one tab and returns the layout its groups go into.
	 *
	 * Each tab scrolls on its own, so the header above the tab strip and the strip itself stay
	 * put however tall the settings under them get, and each tab keeps its own scroll position --
	 * coming back to Style leaves the reader where they left off in it rather than at the top of
	 * a column they have to find their place in again.
	 */
	QVBoxLayout *addTab(EditorTab tab, const QString &title);

	/* The layout a tab's groups are added to. */
	QVBoxLayout *tabLayout(EditorTab tab) const { return tabLayouts[static_cast<int>(tab)]; }

	/*
	 * Takes away the tabs with nothing on them and gives them back.
	 *
	 * A Spacer has no words, no styles and no panels, so three of the four would be empty panes
	 * inviting the reader to look for settings that are not there. Asked of the groups on each
	 * page rather than of the section type: applyTypeVisibility has just decided what applies, and
	 * counting what it left on screen cannot disagree with it the way a second list of conditions
	 * could.
	 */
	void refreshTabVisibility();

	/* True when any group on a tab's page is still on screen. */
	bool tabHasVisibleGroup(EditorTab tab) const;

	/*
	 * The section type the picker and its switches add up to, and the reverse: which base type
	 * and switches stand for a given section type.
	 *
	 * The document still carries all twenty types under their own ids -- nothing about
	 * persistence changes here. What changes is that the reader picks a Title and then says
	 * whether it has a subtitle and whether it has a logo, instead of choosing between five
	 * kinds of title in a list of twenty.
	 */
	SectionType composedType() const;
	void showTypeAsSwitches(SectionType type);
	/* Shows the switches that apply to the base type, and re-reads what they now compose. */
	void onTypeSwitchChanged();
	/*
	 * True when any artwork this section places would animate. Read from file headers rather
	 * than from a decode, so it is cheap enough to ask on every section switch.
	 */
	bool sectionHasAnimatedArt(const Section &source) const;
	/* Shows or hides the playback controls, and says what the section's artwork is. */
	void refreshLogoPlayback();
	/* The playback settings the controls currently describe. */
	LogoPlayback currentLogoPlayback() const;
	/*
	 * `rowSubtitles` is passed rather than read off the checkbox because the table is rebuilt
	 * from a section being loaded as well as from a type being picked, and in the first case the
	 * widgets have not been written yet.
	 */
	void rebuildEntryTable(SectionType type, bool rowSubtitles);
	/*
	 * Re-columns the entry table for a new type, carrying the entries across. The table's shape
	 * follows the type, so a rebuild without this reads back as a table with nothing in it.
	 */
	void relayoutEntryTable(SectionType type);
	void readEntriesFromTable(Section *target) const;
	void writeEntriesToTable(const Section &source);

	/*
	 * The divider's center stack. Unlike the entry table these go through the model rather than
	 * shuffling cells: the rows carry combo boxes, which QTableWidget::takeItem knows nothing
	 * about, and rebuilding from a reordered vector cannot leave a row's widgets behind while
	 * its text moves.
	 */
	void writePiecesToTable(PieceSlot slot, const Section &source);
	void readPiecesFromTable(PieceSlot slot, Section *target) const;
	void addPiece(PieceSlot slot);
	void removeSelectedPieces(PieceSlot slot);
	void movePiece(PieceSlot slot, int delta);
	void browseForPieceFile(PieceSlot slot);
	/* Shows the fields that apply to the piece in `row` and hides the rest. */
	void applyPieceRowVisibility(PieceSlot slot, int row);
	/* True when any piece of any of the three stacks draws its artwork from a file. */
	bool dividerUsesFile() const;

	QTableWidget *pieceTable(PieceSlot slot) const { return pieceTables[static_cast<int>(slot)]; }

	/*
	 * Opens one of the editor's tables in a window of its own for as long as it is wanted there,
	 * and puts it back where it came from afterwards.
	 *
	 * The table itself moves rather than a copy of it: a roll of two hundred credits is typed
	 * into a pane a third of a designer wide, and the way to give that work room is a window,
	 * not a taller pane. Moving the widget keeps every signal, selection and half-typed cell
	 * exactly as it was, where a second table filled from the first would have to be kept in
	 * step with it and would hand back stale rows the moment it was not.
	 */
	void expandTable(QTableWidget *table, const QString &title);

	void addEntry();
	void removeSelectedEntries();
	void moveSelectedEntry(int delta);
	void importCsv();
	void browseForSectionLogo();
	void browseForEntryLogo();
	void browseForBridgeSvg();
	/* Points `target` at a file the user picks, for any of the divider's three artwork slots. */
	void browseForDividerSvg(QLineEdit *target);

	void emitChanged();

	/* The form rows are being added to at this point in the constructor. */
	QFormLayout *form = nullptr;
	/* Which form each row's field belongs to, for setRowVisible. */
	QHash<QWidget *, QFormLayout *> rowOwner;

	/*
	 * The handful of rows above the tab strip: what kind of section this is, what it is called,
	 * and whether it is drawn.
	 *
	 * They stay out of the tabs because they are not settings of one job -- the type decides what
	 * every tab holds, and the name is how the section is found again. A reader who had to leave
	 * the tab they were working in to flip one of these would lose their place to change something
	 * that governs all of them.
	 */
	QFormLayout *headerForm = nullptr;

	QTabWidget *tabs = nullptr;
	/* The layout each page's groups are added to, in EditorTab order. */
	QVBoxLayout *tabLayouts[kEditorTabCount] = {};
	/* Where each page's trailing spacer sits in its layout; see the note beside the one it added. */
	int tabStretchIndex[kEditorTabCount] = {};
	/*
	 * The tab the reader last chose, which is not always the one on screen: a tab going away under
	 * them takes the selection with it, and this is what brings them back to it when the next
	 * section has one again.
	 */
	int desiredTab = 0;
	/* Set while tabs are being taken away and given back, so that shuffle is not read as a choice. */
	bool restoringTab = false;

	CollapsibleGroup *contentGroup = nullptr;
	CollapsibleGroup *typeSettingsGroup = nullptr;
	CollapsibleGroup *placementGroup = nullptr;
	QFormLayout *contentForm = nullptr;
	QFormLayout *typeSettingsForm = nullptr;
	QFormLayout *placementForm = nullptr;

	QComboBox *typeBox = nullptr;
	/*
	 * The switches that turn a base type into one of the document's own. A heading with a
	 * subtitle and a logo is a Title, a subtitle and a logo -- three plain answers -- rather than
	 * "Title w/ Subtitle & Logo" picked out of a list of ten headings.
	 */
	QCheckBox *typeSubtitle = nullptr;
	QCheckBox *typeLogo = nullptr;
	QCheckBox *typeLogoOnly = nullptr;
	QComboBox *typeListContent = nullptr;
	/* One line saying what the selected type is for, under the picker. */
	QLabel *typeHelp = nullptr;

	QLineEdit *labelEdit = nullptr;
	QCheckBox *visibleBox = nullptr;
	QPlainTextEdit *textEdit = nullptr;
	/* The second line of a "... w/ Subtitle" heading; a list's subtitles live in the entry table. */
	QPlainTextEdit *subtitleEdit = nullptr;

	QLineEdit *logoPath = nullptr;
	QToolButton *logoBrowse = nullptr;
	QSpinBox *logoHeight = nullptr;
	/*
	 * Playback for the section's animated artwork.
	 *
	 * One set of controls for the whole section rather than one per logo. The document holds
	 * playback on each LogoRef -- a list can carry a different setting per entry, and one
	 * hand-written or imported that way is honored -- but a Multi-List of Logos is a grid of
	 * sponsor marks that read as one block, and a loop switch on each of twelve cells is a
	 * column of checkboxes nobody wants to fill in. Writing the section's settings to every logo
	 * it holds is what these do.
	 */
	QCheckBox *logoLoop = nullptr;
	QCheckBox *logoStartOnEnter = nullptr;
	QDoubleSpinBox *logoSpeed = nullptr;
	QCheckBox *logoAnimatedShadow = nullptr;
	QComboBox *logoPlacement = nullptr;
	QComboBox *logoSide = nullptr;
	QSpinBox *logoGap = nullptr;

	QComboBox *bridgeType = nullptr;
	QLineEdit *bridgeEdit = nullptr;
	QLineEdit *bridgeSvgPath = nullptr;
	QToolButton *bridgeSvgBrowse = nullptr;
	QSpinBox *bridgeThickness = nullptr;
	QSpinBox *bridgeOffset = nullptr;
	QSpinBox *bridgeGap = nullptr;
	/* Empty bridges only: how much room the gap keeps when nothing is drawn in it. */
	QSpinBox *bridgeMinGap = nullptr;
	QCheckBox *bridgeTint = nullptr;
	QComboBox *bridgeFill = nullptr;
	QComboBox *bridgeSizing = nullptr;
	QSpinBox *bridgeSplit = nullptr;
	QComboBox *bridgeRowAlign = nullptr;
	QCheckBox *bridgeSpanEmpty = nullptr;
	/* Bridged rows only: draw a second line under each side of every row. */
	QCheckBox *rowSubtitles = nullptr;

	QCheckBox *dividerMirrorEnds = nullptr;
	/* Runs the rule through the ends and the center rather than stopping at them. */
	QCheckBox *dividerConnect = nullptr;
	QComboBox *dividerArm = nullptr;
	QLineEdit *dividerArmSvgPath = nullptr;
	QSpinBox *dividerThickness = nullptr;
	QSpinBox *dividerGap = nullptr;
	QSpinBox *dividerPieceGap = nullptr;
	QSpinBox *dividerRules = nullptr;
	QSpinBox *dividerRuleGap = nullptr;
	QSpinBox *dividerRuleInset = nullptr;
	QCheckBox *dividerTint = nullptr;

	/*
	 * The divider's three piece stacks, in PieceSlot order. One table each rather than one table
	 * and a selector, because they are the same kind of list and the tabs are what say so -- and
	 * because the right-hand end's tab can simply be taken away while the ends are mirrored.
	 */
	QGroupBox *dividerPiecesGroup = nullptr;
	QTabWidget *pieceTabs = nullptr;
	QTableWidget *pieceTables[kPieceSlotCount] = {};
	QToolButton *pieceFileButtons[kPieceSlotCount] = {};

	QSpinBox *columns = nullptr;
	QSpinBox *columnGap = nullptr;
	QComboBox *fillOrder = nullptr;
	QSpinBox *entryGap = nullptr;
	QSpinBox *indentStep = nullptr;
	QSpinBox *subtitleGap = nullptr;
	QComboBox *subtitleOrder = nullptr;

	QSpinBox *paddingTop = nullptr;
	QSpinBox *paddingBottom = nullptr;
	QSpinBox *marginX = nullptr;
	QSpinBox *contentOffsetX = nullptr;
	QSpinBox *sectionWidth = nullptr;
	QComboBox *sectionAlign = nullptr;
	QSpinBox *spacerHeight = nullptr;

	/* Sticky Ending Block sections only. */
	QComboBox *stickyAnchor = nullptr;
	QSpinBox *stickyCanvasPosition = nullptr;
	QSpinBox *stickyOffset = nullptr;
	QDoubleSpinBox *stickyHold = nullptr;
	QCheckBox *stickyHoldForever = nullptr;
	QComboBox *stickyRelease = nullptr;
	QLabel *stickyForeverWarning = nullptr;

	/*
	 * The style groups, which fold away like the three above them. The two that a section can
	 * switch off carry their checkbox in the fold header; see CollapsibleGroup for why the two
	 * readings are kept apart.
	 */
	CollapsibleGroup *styleGroup = nullptr;
	StyleEditor *primaryStyle = nullptr;
	CollapsibleGroup *secondaryGroup = nullptr;
	StyleEditor *secondaryStyle = nullptr;
	/* Shown for the two shapes that draw a bridge, and only ever edits that bridge's ink. */
	CollapsibleGroup *bridgeStyleGroup = nullptr;
	StyleEditor *bridgeStyle = nullptr;
	/* The two subtitles of a bridged row, shown only while that row draws them. */
	CollapsibleGroup *rowSubtitleStyleGroup = nullptr;
	StyleEditor *rowSubtitleStyle = nullptr;
	CollapsibleGroup *rowSecondarySubtitleStyleGroup = nullptr;
	StyleEditor *rowSecondarySubtitleStyle = nullptr;

	/*
	 * One folding group per background slot, built from the slot table rather than named one at a
	 * time: eight slots carrying the same eleven controls is a loop, and writing it out eight
	 * times would be eight places for a new panel setting to be forgotten. Which of them are on
	 * screen follows `backgroundSlotsFor` -- see applyTypeVisibility.
	 */
	QHash<BackgroundSlot, CollapsibleGroup *> backgroundGroups;
	QHash<BackgroundSlot, BackgroundEditor *> backgroundEditors;

	QGroupBox *entriesGroup = nullptr;
	QTableWidget *entryTable = nullptr;
	/* Hidden for the entry types that have no logo path to set. */
	QToolButton *setLogoButton = nullptr;

	QVector<StylePreset> presets;
	QVector<BackgroundPreset> backgroundPresets;
	/*
	 * Set only for the duration of a forwarded preset signal, so the round trip back
	 * through setPresets() does not rewrite the fields of the editor being typed into.
	 */
	StyleEditor *presetOrigin = nullptr;
	BackgroundEditor *backgroundPresetOrigin = nullptr;

	Section current;
	/* The type the entry table's columns currently stand for; see relayoutEntryTable. */
	SectionType tableType = SectionType::Title;
	/*
	 * Whether those columns include the two subtitle ones. Kept beside `tableType` and for the
	 * same reason: the columns on screen are what a read has to be taken through, and the
	 * checkbox has already moved on by the time the toggle is handled.
	 */
	bool tableRowSubtitles = false;
	bool loading = false;
};

} // namespace closingtime
