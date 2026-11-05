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

#include <obs.h>

#include <QColor>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <vector>

#include "model/Background.hpp"
#include "model/BridgeArt.hpp"
#include "model/DividerArt.hpp"
#include "model/EndingAction.hpp"
#include "model/FontBundle.hpp"
#include "model/Gradient.hpp"

namespace closingtime {

/*
 * Every section the user can place in the roll. The enum values are persisted by their
 * string ids (see sectionTypeId), never by their numeric value, so the enum may be
 * reordered freely without breaking existing scene collections.
 */
enum class SectionType {
	Title,
	TitleWithSubtitle,
	TitleWithLogo,
	TitleWithSubtitleAndLogo,
	LogoTitle,
	Header,
	HeaderWithSubtitle,
	HeaderWithLogo,
	HeaderWithSubtitleAndLogo,
	LogoHeader,
	Bridged,
	TextList,
	TitleSubtitleList,
	LogoList,
	MultiTextList,
	MultiTitleSubtitleList,
	MultiLogoList,
	/*
	 * A list whose entries are a run of up to four lines, each in a style of its own -- a song
	 * title over who wrote it, who performed it and whose courtesy it is used by. The line under
	 * the title/subtitle pair that the two-line lists stop at. See `sectionStackLines`.
	 */
	StackedList,
	MultiStackedList,
	/*
	 * A list of cards: a picture with a title and a subtitle under it, the three of them one
	 * item. A headshot over a name and a role, a sponsor's mark over its tier. The caption
	 * belongs to its picture, which two sections -- one of logos, one of names -- cannot say.
	 */
	CardList,
	MultiCardList,
	/*
	 * A list of lines each led by a small picture sized to the text rather than to a height of
	 * its own: a platform's mark beside a handle, a flag beside a country.
	 */
	IconList,
	MultiIconList,
	/*
	 * Running prose: a dedication, a thank-you, the licensing lines at the foot of a roll. Breaks
	 * into paragraphs at blank lines and can be set justified. See `Section::justifyText`.
	 */
	Paragraph,
	/*
	 * A QR code drawn from the text it encodes, with an optional caption under it. Drawn as
	 * squares rather than decoded from a picture, so it is sharp at any size. See `Section::qrText`.
	 */
	QrCode,
	SectionDivider,
	Spacer,
	/*
	 * A run of sections that pins itself to a place on the canvas instead of scrolling past it.
	 * It holds other sections rather than content of its own; see `Section::children`.
	 */
	StickyBlock,
	/*
	 * A marker in the roll that runs actions when it crosses a line on the canvas. Takes no room and
	 * draws nothing; see `Section::actions`.
	 */
	Action,
	/*
	 * Where one page ends and the next begins, when the roll is played as pages rather than
	 * scrolled (see `PlaybackMode`). Takes no room and draws nothing, and means nothing to a roll
	 * that scrolls -- so a roll can be built once and played either way.
	 */
	PageBreak,
};

const char *sectionTypeId(SectionType type);
SectionType sectionTypeFromId(const char *id, SectionType fallback = SectionType::Title);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *sectionTypeName(SectionType type);

/* Every section type in the order the designer's "add section" menu should list them. */
const QVector<SectionType> &allSectionTypes();

/* True when the type renders text through a TextStyle at all. LogoTitle/LogoHeader do not. */
bool sectionUsesText(SectionType type);
/* True when the type places one or more logo images. */
bool sectionUsesLogos(SectionType type);
/* True when the type holds a list of entries rather than a single line. */
bool sectionUsesEntries(SectionType type);
/* True when the type spreads its entries over a configurable number of columns. */
bool sectionUsesColumns(SectionType type);
/*
 * True when the type stacks a subtitle under a title -- under each entry's title for the list
 * shapes, under the section's own for the single-heading ones. Either way it is what says that
 * `subtitleGap` and `subtitleFirst` have something to act on.
 */
bool sectionUsesSubtitles(SectionType type);
/*
 * True when the type carries two texts rather than one, whatever it does with them -- the two
 * sides of a bridged row, or a title and the subtitle under it. This is what decides that the
 * entry table has a second column, that a single heading has a subtitle field beside its text,
 * and that the secondary style is worth offering at all.
 */
bool sectionUsesSecondaryText(SectionType type);

/*
 * Which background slots a section of this type actually draws anything behind, in the order the
 * designer should offer them.
 *
 * Derived from the predicates above rather than tabulated beside them, for the same reason
 * `sectionUsesSecondaryText` is: "does it place logos" already answers "can it have a panel behind
 * a logo", and a second table would be a second place for the answer to drift. Every type gets the
 * Section slot -- a spacer's panel is a plain band of color in the middle of a roll, which is a
 * use rather than an oversight -- and a sticky block gets that one alone, since what it holds are
 * whole sections carrying slots of their own.
 */
QVector<BackgroundSlot> backgroundSlotsFor(SectionType type);

/*
 * What a list holds, which with a column count is the whole of what tells the list types apart:
 * single lines, title/subtitle pairs, logos, stacks of up to four lines, cards (a picture over a
 * pair), and lines each led by an icon.
 */
enum class SectionListContent { Text, Pairs, Logos, Stacks, Cards, Icons };

/* True for the two Stacked list types, whose entries are up to four lines rather than a pair. */
bool sectionStacksLines(SectionType type);
/* True for the two Card list types, whose entries are a picture with a pair of lines under it. */
bool sectionUsesCards(SectionType type);
/* True for the two Icon list types, whose entries are a line of text led by a small picture. */
bool sectionUsesIcons(SectionType type);

/* How many lines one entry of a Stacked list can hold. */
constexpr int kStackLines = 4;

/*
 * How much of a QR code can be lost and still be read: roughly 7, 15, 25 and 30 percent. More
 * recovery costs room, so the same text needs more, smaller squares at a higher level.
 */
enum class QrErrorCorrection { Low, Medium, Quartile, High };

const char *qrErrorCorrectionId(QrErrorCorrection level);
QrErrorCorrection qrErrorCorrectionFromId(const char *id, QrErrorCorrection fallback = QrErrorCorrection::Medium);

/*
 * How each dark square of a QR code is drawn.
 *
 *   Square    - the plain square every reader expects.
 *   Rounded   - squares with their corners rounded off.
 *   Dots      - a circle per square.
 *   Diamond   - a square turned on its corner.
 *   Connected - squares that flow into their neighbors, rounded only where a run ends: the
 *               "liquid" look, where the code reads as strokes rather than as a grid.
 *
 * A reader samples the middle of each square, so every shape keeps the middle dark; the shapes
 * differ only in how much of the square's edge they fill.
 */
enum class QrModuleShape { Square, Rounded, Dots, Diamond, Connected };

const char *qrModuleShapeId(QrModuleShape shape);
QrModuleShape qrModuleShapeFromId(const char *id, QrModuleShape fallback = QrModuleShape::Square);

/*
 * The shape of a finder pattern -- the three big "eyes" in the corners -- drawn as a frame
 * around a pupil. Each is chosen apart, which is most of what makes a styled code look styled.
 * Diamond is a pupil shape only: as a frame it is thin along the diagonals and is missed by ZXing,
 * the decoder behind most Android cameras, so a frame asked to be one is drawn square.
 */
enum class QrEyeShape { Square, Rounded, Circle, Diamond };

const char *qrEyeShapeId(QrEyeShape shape);
QrEyeShape qrEyeShapeFromId(const char *id, QrEyeShape fallback = QrEyeShape::Square);

/*
 * The most bytes of UTF-8 a QR code can carry at `level`: the capacity of the largest symbol
 * (version 40) in byte mode. Text past this is cut there when the code is drawn, rather than
 * the code failing to draw at all.
 */
int qrCapacityBytes(QrErrorCorrection level);

/*
 * A section type taken apart into the handful of plain answers it stands for.
 *
 * The nineteen shapes of heading and list are not nineteen different things: they are two
 * headings, a list and a bridged row, crossed with whether a subtitle is stacked under it, whether
 * a logo sits beside it, what a list holds, and how many columns it runs over. A picker that
 * offers all of them at once asks one hard question where this asks three easy ones.
 *
 * It lives here rather than in the designer because it is a property of the type table -- the
 * *editor's* decision is which base types to offer -- and because a mapping that has to be exactly
 * reversible is worth a test that can be written without a window on screen.
 */
struct SectionTypeSwitches {
	SectionType base = SectionType::Title;
	/* Headings only. */
	bool subtitle = false;
	bool logo = false;
	bool logoOnly = false;
	/* Lists only. */
	SectionListContent content = SectionListContent::Text;
	bool multiColumn = false;
};

/* The base type and the switches a type stands for. */
SectionTypeSwitches decomposeSectionType(SectionType type);

/* And back again: `composeSectionType(decomposeSectionType(t)) == t` for every type. */
SectionType composeSectionType(const SectionTypeSwitches &switches);

/*
 * Which part of a sticky block is pinned, and to what.
 *
 * The pin is a pair of points: one on the block and one down the canvas. Saying "the middle of the
 * block, halfway down the frame" needs both halves, and a single number could only ever express
 * one of them -- which is why a block's top edge landing at a fixed distance from the top of the
 * frame and a block *centered* in the frame are two settings rather than one with a fudge in it.
 */
enum class StickyAnchor { Top, Center, Bottom };

const char *stickyAnchorId(StickyAnchor anchor);
StickyAnchor stickyAnchorFromId(const char *id, StickyAnchor fallback = StickyAnchor::Center);

/* Where on the block the pin sits: 0 at its top edge, 1 at its bottom. */
double stickyAnchorFraction(StickyAnchor anchor);

/*
 * When a sticky block arrives on the frame.
 *
 *   WithRoll  - it travels up with the roll and stops when its slot reaches the anchor, which is
 *               what a pinned block has always done.
 *   AfterRoll - it is not on screen at all until the rest of the roll has gone off the top, and
 *               then fades up in place. The card that follows the credits rather than ending them:
 *               a beat of empty frame, and then "X will return".
 *
 * The layout is the same either way. The block still takes its slot in the roll -- see
 * `Section::children` -- so turning the entrance around moves nothing above or below it; all that
 * changes is whether the block is drawn while its slot is going past.
 */
enum class StickyEntrance { WithRoll, AfterRoll };

const char *stickyEntranceId(StickyEntrance entrance);
StickyEntrance stickyEntranceFromId(const char *id, StickyEntrance fallback = StickyEntrance::WithRoll);

/* True when the block waits for the roll to clear the frame before it shows itself at all. */
bool stickyEntranceWaitsForRoll(StickyEntrance entrance);

/*
 * What a sticky block does when its hold runs out.
 *
 *   EndAtHold      - the hold expiring *is* the end of the roll: the ending action fires and the
 *                    block stays where it is. The closing card that stays on screen.
 *   ResumeThenEnd  - the block carries on up and off the top, and the roll ends once it has taken
 *                    the last of itself with it.
 *   ResumeEndAtHold- the block carries on up and off, but the ending action fires at the moment
 *                    the hold ends rather than waiting for it to clear the frame.
 *   ResumeOnly     - the block carries on up and off and ends nothing. The roll finishes when
 *                    whatever else is in it says so, which is what lets one block follow another:
 *                    a thank-you card that leaves, and a second block that fades up behind it.
 *
 * Four rather than one because they answer two independent questions -- does the block leave, and
 * what does its hold ending mean for the roll -- and every combination of the two is something
 * somebody builds a roll around.
 */
enum class StickyRelease { EndAtHold, ResumeThenEnd, ResumeEndAtHold, ResumeOnly };

const char *stickyReleaseId(StickyRelease release);
StickyRelease stickyReleaseFromId(const char *id, StickyRelease fallback = StickyRelease::EndAtHold);

/* True when the block scrolls on after its hold rather than staying where it was pinned. */
bool stickyReleaseResumes(StickyRelease release);

/* True when the hold running out is what finishes the roll, rather than the strip clearing. */
bool stickyReleaseEndsAtHold(StickyRelease release);

/*
 * True when the block clearing the frame is what finishes the roll.
 *
 * The counterpart to `stickyReleaseEndsAtHold` for the mode that waits: between the two of them a
 * release either ends the roll at a moment of its own or leaves the ending to the rest of it.
 */
bool stickyReleaseEndsAtExit(StickyRelease release);

/*
 * How the roll is played.
 *
 *   Scroll - the strip travels up the frame, as a credit roll does.
 *   Pages  - the roll is cut into pages that each fill the frame, and each is shown, held and
 *            replaced by the next. A page ends where the frame is full or at a Page Break section,
 *            whichever comes first; see `Document::pageHold` and the settings beside it.
 *
 * The layout is the same either way. Pages are cut from the strip as it is laid out, so a roll is
 * built once and played either way, and switching between the two lays nothing out again.
 */
enum class PlaybackMode { Scroll, Pages };

const char *playbackModeId(PlaybackMode mode);
PlaybackMode playbackModeFromId(const char *id, PlaybackMode fallback = PlaybackMode::Scroll);

/*
 * How one page gives way to the next.
 *
 *   Cut       - the next page is simply there.
 *   Fade      - each page fades out to an empty frame and the next fades in from it.
 *   Crossfade - the next page fades in over the last as it fades out.
 *   Slide     - the pages travel up the frame, the next pushing the last off the top: a scroll that
 *               stops at every page.
 */
enum class PageTransition { Cut, Fade, Crossfade, Slide };

const char *pageTransitionId(PageTransition transition);
PageTransition pageTransitionFromId(const char *id, PageTransition fallback = PageTransition::Fade);

enum class HAlign { Left, Center, Right };

const char *hAlignId(HAlign align);
HAlign hAlignFromId(const char *id, HAlign fallback = HAlign::Center);

/* Which side of the text a logo sits on for the "... w/ Logo" section types. */
enum class LogoSide { Left, Right };

const char *logoSideId(LogoSide side);
LogoSide logoSideFromId(const char *id, LogoSide fallback = LogoSide::Left);

/*
 * How a "... w/ Logo" section arranges its logo against its text.
 *
 *   Edge    - the logo is pinned to the section's edge and the text is handed the whole of
 *             what is left. The text then aligns inside that entire column, which is what
 *             leaves a centered title stranded halfway across the frame from its own logo:
 *             `logoGap` sets the minimum separation between the two columns, never the
 *             distance actually drawn.
 *   Hug     - logo, gap and text are measured as one group and aligned as one, so the logo
 *             really does sit `logoGap` from the text wherever the pair ends up.
 *   Bridged - the logo caps one end of the row and the text the other, with the bridge
 *             running between them exactly as it does in a Bridged section.
 */
enum class LogoPlacement { Edge, Hug, Bridged };

const char *logoPlacementId(LogoPlacement placement);
LogoPlacement logoPlacementFromId(const char *id, LogoPlacement fallback = LogoPlacement::Edge);

/*
 * How a Bridged section's bridge covers the space between its two texts.
 *
 *   Fixed   - drawn once at its natural width, wherever the two columns leave room.
 *   Repeat  - tiled as many whole times as fit, so the leader reaches across the gap
 *             however long the two texts turn out to be.
 *   Stretch - drawn once, with the spacing between its characters widened until it spans
 *             the gap exactly. Keeps the character count the user typed.
 *
 * An art bridge reads all three the same way, counting tiles where a text bridge counts
 * copies of its string. The one exception is a bridge type that scales rather than spreads
 * (BridgeStretch::Scale, a continuous rule): it has nothing to count, so Repeat and Stretch
 * both cover the gap exactly and only Fixed leaves it at its natural width.
 */
enum class BridgeFill { Fixed, Repeat, Stretch };

const char *bridgeFillId(BridgeFill fill);
BridgeFill bridgeFillFromId(const char *id, BridgeFill fallback = BridgeFill::Fixed);

/*
 * How a Bridged section divides its width between the two texts.
 *
 *   Split   - the left column is a fixed share of the width (`bridgeSplit`), so the bridge
 *             begins at the same x on every row -- a tab stop. Long text wraps inside it.
 *   Natural - each side takes only the width its own text needs and the bridge absorbs
 *             everything left over, so the row reaches both edges but the bridge starts
 *             wherever the left text happens to end.
 */
enum class BridgeSizing { Split, Natural };

const char *bridgeSizingId(BridgeSizing sizing);
BridgeSizing bridgeSizingFromId(const char *id, BridgeSizing fallback = BridgeSizing::Split);

struct Section;

/*
 * The fill a section's bridge is really laid out with.
 *
 * An empty bridge has nothing to cover a gap with, so all three modes would come out looking the
 * same on screen while quietly measuring the text columns three different ways -- a "fill" setting
 * for something that draws nothing is exactly the kind of control that makes this editor hard to
 * read. So an empty bridge is always laid out as Fixed, reserving `bridgeMinGap` between the two
 * columns, and the designer hides the row rather than offering a choice that only moves text.
 *
 * Everything that measures or places a bridged row goes through this rather than reading
 * `bridgeFill` directly, the same guarantee `effectiveStyle` gives the text.
 */
BridgeFill effectiveBridgeFill(const Section &section);

/*
 * True when this section really stacks a subtitle under something, which is not a question the
 * type alone can answer any more: a bridged row does it only when `rowSubtitles` is on.
 *
 * `sectionUsesSubtitles` stays a property of the type, since that is what the layout switch and
 * the field visibility are written against; this is what anything asking about a particular
 * section -- the editor's `subtitleGap` and `subtitleFirst` rows, which shape any stack -- should
 * ask instead.
 */
bool sectionStacksSubtitles(const Section &section);

/*
 * Every section in `sections`, the children of any sticky block among them included, in the order
 * they are drawn.
 *
 * The document's list is no longer flat, and everything that walks it to ask a section something --
 * which fonts does this roll use, which preset does this section bind to -- has to reach a child or
 * quietly stop working for anything inside a block. One walk in one place is what keeps a new
 * question from being asked of half the roll.
 */
void visitSections(const QVector<Section> &sections, const std::function<void(const Section &)> &visit);
void visitSections(QVector<Section> &sections, const std::function<void(Section &)> &visit);

/*
 * What the glyphs themselves are painted with.
 *
 *   Solid  - TextStyle::color, exactly as text has always been drawn.
 *   Linear - the gradient's stops run along an axis across the block of text.
 *   Radial - the stops run outward from the center of the block to its corners.
 *
 * Both gradients are mapped over the block of text being drawn -- one section's title, one
 * list entry, one side of a bridged row -- rather than over the canvas or the whole strip.
 * Mapping over the strip would make a stop's color depend on how long the roll happens to
 * be; mapping per block means a run of names all share the same sweep as each other.
 */
enum class TextFill { Solid, LinearGradient, RadialGradient };

const char *textFillId(TextFill fill);
TextFill textFillFromId(const char *id, TextFill fallback = TextFill::Solid);

/* GradientStop and GradientSpec live in model/Gradient.hpp: a panel maps a sweep over its own box
 * exactly as a run of glyphs does, and a background is declared before the styles are. */

/*
 * A stroke drawn around the glyphs, `width` pixels of it outside the letterform. Drawn under
 * the fill, so the half of the stroke that falls inside the glyph is covered back over and
 * the outline reads as growing outward only.
 */
struct TextOutline {
	bool enabled = false;
	double width = 2.0;
	QColor color = QColor(0, 0, 0);

	bool operator==(const TextOutline &other) const
	{
		return enabled == other.enabled && qFuzzyCompare(width + 1.0, other.width + 1.0) &&
		       color == other.color;
	}
	bool operator!=(const TextOutline &other) const { return !(*this == other); }
};

/*
 * A drop shadow, offset from the text and optionally softened. `blur` is the radius the
 * shadow's edge is spread over, in pixels; 0 leaves it hard.
 */
struct TextShadow {
	bool enabled = false;
	double offsetX = 0.0;
	double offsetY = 4.0;
	double blur = 8.0;
	QColor color = QColor(0, 0, 0, 160);

	bool operator==(const TextShadow &other) const
	{
		return enabled == other.enabled && qFuzzyCompare(offsetX + 1.0, other.offsetX + 1.0) &&
		       qFuzzyCompare(offsetY + 1.0, other.offsetY + 1.0) &&
		       qFuzzyCompare(blur + 1.0, other.blur + 1.0) && color == other.color;
	}
	bool operator!=(const TextShadow &other) const { return !(*this == other); }
};

struct TextStyle {
	QString family = QStringLiteral("Sans Serif");
	/*
	 * Which face inside the family, by the name the family itself gives it: "Semibold",
	 * "Condensed Light Italic", "Book". A family is a set of faces and most of them cannot be
	 * reached by a weight and a slant -- there is no combination of the two that means Condensed,
	 * and a family with Light, Regular, Medium and Semibold has four answers to "not bold".
	 *
	 * Empty means the family's own default face, which is what every style written before the
	 * picker existed has. `bold` and `italic` below are kept in step with whatever is named here
	 * and are what a machine missing this exact face falls back to: see makeFont() in the
	 * renderer, which will not name a face the machine does not have.
	 */
	QString styleName;
	/*
	 * Sized in pixels rather than points: the strip is laid out in video pixels and has
	 * to come out identical no matter the DPI of the screen OBS is running on.
	 */
	int pixelSize = 32;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeOut = false;
	QColor color = QColor(255, 255, 255);
	HAlign align = HAlign::Center;
	/* Extra space between lines, as a multiplier of the font's natural line height. */
	double lineSpacing = 1.0;

	TextFill fill = TextFill::Solid;
	GradientSpec gradient;
	TextOutline outline;
	TextShadow shadow;

	/*
	 * True when the text needs more than a pen color to draw. The renderer keeps a fast
	 * path for the plain case, so this is what decides which one a style takes.
	 */
	bool hasEffects() const;

	/*
	 * Field-for-field equality.
	 *
	 * What it is for is noticing that a style did *not* move: a library reload that hands the
	 * document a style identical to the one it already had must not count as a change, or every
	 * poll would queue a rebuild of a strip that would come out the same.
	 */
	bool operator==(const TextStyle &other) const
	{
		return family == other.family && styleName == other.styleName && pixelSize == other.pixelSize &&
		       bold == other.bold && italic == other.italic && underline == other.underline &&
		       strikeOut == other.strikeOut && color == other.color && align == other.align &&
		       qFuzzyCompare(lineSpacing + 1.0, other.lineSpacing + 1.0) && fill == other.fill &&
		       gradient == other.gradient && outline == other.outline && shadow == other.shadow;
	}
	bool operator!=(const TextStyle &other) const { return !(*this == other); }

	/*
	 * How far past the text's own box the drawing can reach, in pixels. Nothing here
	 * changes the layout -- an outline or a shadow never moves a line or grows a section,
	 * the same way a CSS text-shadow does not -- so the renderer has to know how far it
	 * may paint outside the section it is drawing in order not to clip it at a tile edge.
	 */
	double effectBleed() const;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
	static void defaults(obs_data_t *data, int pixelSize, bool bold);
};

/*
 * A named TextStyle stored on the document. Sections bind to a preset by name, so
 * restyling every header in a roll is one edit rather than one edit per section.
 *
 * `linked` marks a preset that follows the machine-wide style library (see StyleLibrary):
 * the name is the binding, the library holds the style that is actually drawn, and `style`
 * here is the last copy this document saw. That copy is not redundant. A scene collection
 * carried to another machine arrives without the library, and a linked preset that resolved
 * to nothing would drop every section bound to it back to its own untouched style -- which
 * is to say the roll would arrive unstyled. Keeping the copy means the worst case is a roll
 * that renders exactly as it did when it was last saved.
 */
struct StylePreset {
	QString name;
	TextStyle style;
	bool linked = false;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const StylePreset &other) const = default;
};

/*
 * How an animated logo's frames are timed. Static artwork ignores all of it.
 *
 * The two questions are kept apart because they are genuinely independent: whether a sting
 * repeats is not the same question as when it begins, and a sponsor loop that starts as the
 * cell scrolls into frame is as reasonable a thing to ask for as one that has been running
 * since the roll was armed.
 */
/* Bounds on LogoPlayback::speed, shared by the loader and the designer's spinbox. */
constexpr double kMinLogoSpeed = 0.1;
constexpr double kMaxLogoSpeed = 8.0;

/*
 * How far a divider's parts may be pushed into each other, in pixels -- the far end of
 * Divider::lineGap and Divider::pieceGap once they are allowed to go negative. Shared by the loader
 * and the designer's spin boxes, so a document cannot arrive asking for an overlap the window has
 * no way to type.
 */
constexpr double kMaxDividerJoin = 2048.0;
/* How many lanes a divider may stack. A count read off a file decides how much gets drawn. */
constexpr int kMaxDividerLanes = 16;
/* How many elements one lane may hold, for the same reason. */
constexpr int kMaxDividerElements = 256;

struct LogoPlayback {
	/* Repeat for as long as the logo is on screen, rather than holding the last frame. */
	bool loop = true;
	/*
	 * Start the first frame when the logo's box first enters the canvas, rather than when
	 * the roll is armed. For a play-once sting this is the difference between the animation
	 * being visible and it having finished several minutes before its logo appears.
	 */
	bool startOnEnter = false;
	/* Multiplier on the file's own frame timing. 1.0 is the file's native rate. */
	double speed = 1.0;
	/*
	 * Recompute the drop shadow for every frame rather than casting the first frame's.
	 *
	 * Off by default because the shadow exists to hold a logo off the footage behind it, and
	 * a poster-frame shadow does that for the whole of any artwork that keeps its silhouette
	 * -- which is most of it. Artwork that changes shape frame to frame is what this is for,
	 * and it is opt-in because it costs a blur per frame at rebuild time.
	 */
	bool animatedShadow = false;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const LogoPlayback &other) const = default;
};

/*
 * A logo reference. `path` is an absolute path to an image QImageReader can decode
 * (PNG/JPEG/SVG/etc), or to an animation -- GIF, APNG or animated WebP. Video files are not
 * logos and are not decoded. `maxHeight` bounds the drawn size; width follows from
 * the aspect ratio and is additionally clamped to the available column width at layout time.
 *
 * Nothing here says whether the artwork is animated: that is a property of the file, read
 * when it is decoded, so a still PNG dropped into a slot that used to hold a GIF needs no
 * setting changed and a GIF dropped anywhere animates without one either.
 */
struct LogoRef {
	QString path;
	int maxHeight = 96;
	LogoPlayback playback;

	bool isEmpty() const { return path.isEmpty(); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const LogoRef &other) const = default;
};

/*
 * One entry inside a list-shaped section. Which fields matter depends on the owning
 * section's type:
 *   Bridged                -> text (left) and secondaryText (right), joined by the bridge string,
 *                             each with an optional subtitle stacked under it (subtitle and
 *                             secondarySubtitle) when the section asks for them
 *   TextList               -> text
 *   MultiTextList          -> text, placed into columns in the section's fill order
 *   TitleSubtitleList      -> text (title) and secondaryText (subtitle), stacked one over the other
 *   MultiTitleSubtitleList -> the same pair, placed into columns in the section's fill order
 *   LogoList               -> logo
 *   MultiLogoList          -> logo, placed into columns in the section's fill order
 *   StackedList            -> up to four lines: text, secondaryText, subtitle, secondarySubtitle,
 *                             top to bottom, each in a style of its own; MultiStackedList in columns
 *   CardList               -> logo, with text (title) and secondaryText (subtitle) under it;
 *                             MultiCardList in columns
 *   IconList               -> logo (the icon, sized to the text) and text; MultiIconList in columns
 */
struct Entry {
	QString text;
	QString secondaryText;
	/*
	 * Bridged rows only: a second line under each side of the row, drawn when the section's
	 * `rowSubtitles` is on and there is something here to draw. A role under a name on the left,
	 * a company under a name on the right.
	 *
	 * They are fields of the entry rather than a reuse of `text`/`secondaryText` because a
	 * bridged row already spends both of those on the two sides of the row -- and keeping them
	 * whatever the section's type means a list switched to another shape and back keeps them,
	 * exactly as every other field on the model does.
	 */
	QString subtitle;
	QString secondarySubtitle;
	LogoRef logo;

	/*
	 * How many steps this entry is tabbed in from the rest of the list, each step
	 * `Section::indentStep` wide. Negative steps out the other way, for an entry hung back
	 * off the run above it.
	 *
	 * A step rather than a number of pixels because what an indent is for is a list that reads
	 * as having a shape -- a department over the names under it, a sub-heading inside a run --
	 * and steps of one size are what make several entries line up without each one being
	 * measured. The whole entry moves: its column is translated rather than narrowed, so a
	 * centered or right-aligned list steps by exactly as much as a left-aligned one and an
	 * indent means the same thing whatever the list is set to.
	 */
	int indent = 0;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const Entry &other) const = default;
};

/*
 * One element of a Section Divider: something set along the line.
 *
 * There are two sorts, and the whole of a divider is a row of them:
 *
 *   - A *line* stretches. It takes a share of whatever room the row has left over -- its
 *     `weight` against the other lines' -- and fills it with a rule, a taper, a run of dots, or
 *     nothing at all (a line of `DividerShape::None` is plain space).
 *   - Everything else is a *piece*, drawn once at its own size: a shape from the library, a word
 *     set in the section's style, or an image.
 *
 * That is all a divider needs. The fixed slots there used to be -- an end, an arm, a center, a
 * second end -- are just places in the row: an arrow capping a rule is `Arrow, Line`; a heading
 * leading one is `Text, Line`; diamonds at the thirds are `Line, Diamond, Line, Diamond, Line`.
 * Where a piece sits along the rule is the weights of the lines either side of it rather than a
 * setting of its own.
 *
 * Fields are kept whatever the kind, so switching an element from one kind to another and back
 * hands back what it held: a line remembers its rule while it is a diamond, a word its text while
 * it is a picture.
 */
struct DividerElement {
	enum class Kind {
		/* A shape from the library, or the user's own SVG when `shape` is Custom. */
		Shape,
		/* A word set in the section's own style -- `PART II`, `MMXXVI`. */
		Text,
		Image,
		/* Stretches to fill: see the note above. */
		Line,
	};

	Kind kind = Kind::Shape;

	/* Shape elements: which shape, and the file when it is Custom. */
	DividerShape shape = DividerShape::Diamond;
	QString svgPath;

	/* Line elements: what the line is drawn with (None is empty space), and the file when Custom. */
	DividerShape lineShape = DividerShape::Rule;
	QString lineSvgPath;

	/* Text elements. */
	QString text;
	/* Image elements. */
	LogoRef logo;

	/*
	 * A multiplier: on the size a shape's table row asks for beside a rule of its lane's thickness,
	 * or on the thickness itself for a line, so a run of dots can be heavier than the rule beside
	 * it. A word is sized by the style and an image by its own `maxHeight`, so neither reads this.
	 */
	double scale = 1.0;

	/*
	 * Degrees clockwise about the element's own center, for pieces. It keeps the room its untilted
	 * shape took along the line, so its neighbors stay still while an angle is dialed in; the
	 * divider grows to hold whatever height it sweeps out.
	 */
	double rotation = 0.0;
	/* Pixels below the lane's midline (negative is above), for any element. */
	double offsetY = 0.0;
	/*
	 * Drawn mirrored left to right. Artwork is flipped; a word and an image are never set
	 * backwards, so for them a flip turns the lean the other way instead. The mirrored half of a
	 * symmetric divider is this same flag set on the copies, which is why the two halves of a rule
	 * come out each other's reflection whatever they are made of.
	 */
	bool flip = false;

	/* Line elements: this line's share of the leftover room, against the other lines in its lane. */
	double weight = 1.0;
	/* Line elements: the shortest it may be drawn before the lane starts dropping optional pieces. */
	double minLength = 0.0;
	/*
	 * Line elements: run on under the pieces either side as far as their middles, rather than
	 * stopping short of them -- so a diamond sits *on* the line and an arrowhead closes it with no
	 * blunt end showing. Every tinted part of a divider is inked as one silhouette, so the overlap
	 * leaves no seam.
	 */
	bool join = false;

	/* Pieces: cut every other lane's lines around this one too, so one ornament breaks a stack of rules. */
	bool spanLanes = false;
	/* Pieces: left out when the lane is too narrow for its lines' minimum lengths. */
	bool optional = false;

	/*
	 * Drawn in a color of its own rather than the divider's ink -- a gold diamond on a white rule.
	 * The outline and shadow are still the divider's, so the element still belongs to the figure.
	 */
	bool ownColor = false;
	QColor color = QColor(255, 255, 255);

	bool isLine() const { return kind == Kind::Line; }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const DividerElement &other) const = default;
};

const char *dividerElementKindId(DividerElement::Kind kind);
DividerElement::Kind dividerElementKindFromId(const char *id,
					      DividerElement::Kind fallback = DividerElement::Kind::Shape);

/*
 * One line of a divider, with the row of elements set along it.
 *
 * A divider has one or more lanes, stacked top to bottom about its midline. Each is a complete row
 * of its own -- a different rule, a different thickness, different pieces, a different length --
 * so three lines can be three different lines. A piece that spans lanes is how one ornament breaks
 * all of them at once.
 */
struct DividerLane {
	QVector<DividerElement> elements;
	/* How thick this lane's lines are drawn, in pixels. A shape's size is a proportion of it. */
	double thickness = 4.0;
	/* How far in from each end of the divider this lane starts, in pixels: a stack tapers this way. */
	double inset = 0.0;
	/* A nudge on where the lane sits, in pixels; positive moves it down. */
	double offsetY = 0.0;
	/*
	 * Symmetric dividers only: the last element, when it is a piece, sits on the center line and is
	 * drawn once, rather than being drawn twice either side of it. On by default, which is what a
	 * diamond in the middle of a rule wants; off, a pair -- `Dot, Dot` -- meets at the middle.
	 */
	bool centerPiece = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const DividerLane &other) const = default;
};

/*
 * How a divider draws itself on as it scrolls into the frame.
 *
 *   None       - it is simply there, as every divider has always been.
 *   FromCenter - it grows outward from its own middle to both ends.
 *   FromLeft   - it is drawn on left to right, as if by a pen.
 *   FromRight  - the same, right to left.
 *
 * Driven by where the divider is on the frame rather than by a clock, so a reveal looks the same at
 * any scroll speed, paused, or scrubbed to by hand -- it is part of the roll's picture, not an event.
 */
enum class DividerReveal { None, FromCenter, FromLeft, FromRight };

const char *dividerRevealId(DividerReveal reveal);
DividerReveal dividerRevealFromId(const char *id, DividerReveal fallback = DividerReveal::None);

/*
 * Everything a Section Divider draws.
 *
 * `symmetric` is on by default and is the ordinary case: each lane's elements describe its left
 * half, outside in, and the right half is drawn as the same list reflected. Off, each lane's list
 * is the whole row, left to right, for a divider that points somewhere or is labeled at one end.
 * Nothing is lost by switching -- the lists are kept as written either way.
 */
struct Divider {
	/* Top to bottom. Never empty once loaded: there is always a lane to put something in. */
	QVector<DividerLane> lanes = {DividerLane{}};
	bool symmetric = true;

	/* Space between neighbouring lanes' lines, in pixels. */
	double laneGap = 6.0;
	/* Space between two pieces side by side. Negative pushes them into each other, up to halfway. */
	double pieceGap = 10.0;
	/*
	 * Space a line keeps clear of a piece beside it. Negative runs the line under the piece by
	 * that much, which with the single silhouette the art is inked as joins the two seamlessly.
	 */
	double lineGap = 12.0;

	/* How much of the section's content width the divider spans, 0.0 to 1.0, and where it sits. */
	double length = 1.0;
	HAlign align = HAlign::Center;

	/*
	 * Paint the user's own SVG files in the divider's ink rather than in the colors they were drawn
	 * with. The built-in shapes are always tinted -- they are drawn white so they can be.
	 */
	bool tint = true;

	/*
	 * The draw-on, if any. It begins when the divider's middle rises past `revealStart` of the way
	 * down the frame (1.0 is the bottom edge, so the default starts it just as it comes into view)
	 * and is complete once the divider has risen a further `revealLength` of the frame's height.
	 * Both are shares of the frame rather than pixels or seconds, so the reveal sits at the same
	 * place on any canvas and at any speed.
	 */
	DividerReveal reveal = DividerReveal::None;
	double revealStart = 0.95;
	double revealLength = 0.2;

	/*
	 * Each lane's elements as they are actually drawn, left to right: the list itself when the
	 * divider is not symmetric, the list and its reflection when it is.
	 */
	QVector<DividerElement> drawnRow(const DividerLane &lane) const;

	/* True when any element of any lane holds a word with something in it. */
	bool hasText() const;
	/* The first word any lane holds, or an empty string. What a divider is named after in a list. */
	QString firstText() const;
	/* Every image path the divider draws. */
	QStringList imagePaths() const;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	/*
	 * Builds the divider an older document describes, from the keys it was written with: two end
	 * stacks, an arm, a center stack, a rule count. See model/Divider.cpp.
	 */
	static Divider fromLegacy(obs_data_t *sectionData);

	bool operator==(const Divider &other) const = default;
};

/* The plainest divider there is: one lane with one line across it. */
Divider plainDivider();

/*
 * Everything that makes a QR code look like more than a grid of squares. All of it is drawing:
 * none of it changes what the code encodes or how big it is, so a styled code is the same code.
 */
struct QrDesign {
	QrModuleShape moduleShape = QrModuleShape::Square;
	/*
	 * How much of its square each module fills, 0.5 to 1.0. Under 1.0 the modules stand apart with
	 * the plate showing between them, which suits dots and diamonds; a reader still finds them, as
	 * long as the middle of each square stays dark.
	 */
	double moduleScale = 1.0;

	QrEyeShape eyeFrame = QrEyeShape::Square;
	QrEyeShape eyePupil = QrEyeShape::Square;
	/* Draw the eyes in colors of their own rather than the modules' fill. */
	bool eyeColors = false;
	QColor eyeFrameColor = QColor(0, 0, 0);
	QColor eyePupilColor = QColor(0, 0, 0);

	/*
	 * What the modules -- and the eyes, unless they have colors of their own -- are filled with.
	 * A gradient is mapped over the whole code, so the sweep runs across it as one picture.
	 */
	TextFill fill = TextFill::Solid;
	GradientSpec gradient;

	/* Rounds the corners of the plate the code is drawn on, in pixels. */
	double plateRadius = 0.0;

	/*
	 * A picture in the middle of the code -- a channel's mark, a sponsor's -- and how wide it is
	 * as a share of the code. The modules behind it are cleared, which the code survives by
	 * spending its error correction on them; that is why a code with a logo is always encoded at
	 * High, whatever level is set, and why the logo is held to at most 30 % of the code's width.
	 */
	LogoRef logo;
	double logoSize = 0.22;
	/* Clears the modules behind the logo, with a square's margin, rather than drawing it over them. */
	bool logoClear = true;

	bool hasLogo() const { return !logo.isEmpty(); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	bool operator==(const QrDesign &other) const = default;
};

/* A ready-made design, for the designer's picker. `id` is never shown; `name` is the English name. */
struct QrLook {
	const char *id;
	const char *name;
	QrModuleShape moduleShape;
	double moduleScale;
	QrEyeShape eyeFrame;
	QrEyeShape eyePupil;
	double plateRadius;
};

const QVector<QrLook> &qrLooks();

/* The look `design` matches in every field a look sets, or null. */
const QrLook *matchingQrLook(const QrDesign &design);
void applyQrLook(QrDesign &design, const QrLook &look);

struct Section {
	SectionType type = SectionType::Title;

	/* Shown in the designer's section list only; never rendered. */
	QString label;

	/* Single-line content for Title/Header and their subtitle and logo variants. */
	QString text;
	/*
	 * The second line of a "... w/ Subtitle" heading, drawn under `text` exactly as a
	 * title/subtitle list draws an entry's `secondaryText` under its `text` -- same styles, same
	 * `subtitleGap`, same `subtitleFirst`, same helper in the renderer.
	 *
	 * It is a field of its own rather than a one-entry list because a heading is not a list: a
	 * list would put an entry table in the editor for a section that can only ever hold one pair,
	 * and would leave a second entry to be silently drawn by a type with nowhere to put it. Kept
	 * whatever the type, so a heading switched to something else and back keeps its subtitle, the
	 * same way every other field survives a change of type.
	 */
	QString secondaryText;
	LogoRef logo;
	LogoSide logoSide = LogoSide::Left;
	LogoPlacement logoPlacement = LogoPlacement::Edge;
	/*
	 * Space between the logo and the text for the "... w/ Logo" types, in pixels. Under
	 * Edge placement this is only a minimum between the two columns; under Hug it is the
	 * distance drawn, and under Bridged it is the padding at each end of the bridge.
	 */
	int logoGap = 24;

	/* List content. */
	QVector<Entry> entries;
	/* Vertical gap between consecutive entries (or rows, for multi-lists), in pixels. */
	int entryGap = 8;

	/*
	 * Subtitle-carrying types only: the gap between the two lines of one pair, in pixels. Kept
	 * separate from `entryGap` because the two say opposite things -- this one binds a pair
	 * together, that one holds consecutive pairs apart -- and a list where both are the same
	 * number reads as a single run of alternating lines rather than as a list of pairs.
	 */
	int subtitleGap = 4;
	/*
	 * Subtitle-carrying types only: draw the subtitle above the title rather than below it. The
	 * fields keep their meaning either way -- the title is still `text` (or an entry's `text`)
	 * and the subtitle still `secondaryText`, styled by `style` and `secondaryStyle`
	 * respectively -- so flipping this is purely a placement change and never moves content
	 * between fields.
	 */
	bool subtitleFirst = false;

	/*
	 * Bridged sections only: what the separator between the two texts is drawn from. Text is
	 * the load-time fallback, so a document written before the art types existed keeps the
	 * string bridge it was built against; new sections are handed a Dots bridge.
	 */
	BridgeType bridgeType = BridgeType::Text;
	/* Text bridges only: the string laid across the gap. */
	QString bridge = QStringLiteral(" . . . . . . ");
	/* Custom bridges only: absolute path to the SVG whose art is tiled across the gap. */
	QString bridgeSvg;
	/*
	 * Art bridges only: how tall the art is drawn, in pixels. A tile's width follows from its
	 * own proportions, so this one number sizes the whole thing.
	 */
	double bridgeThickness = 4.0;
	/*
	 * Art bridges only: how far above the row's baseline the art sits, in pixels. At 0 it
	 * rests on the baseline the way a run of leader dots does; raising it lifts a rule up
	 * through the middle of the text.
	 */
	double bridgeOffset = 0.0;
	/*
	 * Empty bridges only: the space the bridge takes up when there is nothing in it, in pixels.
	 *
	 * Every other type has a natural width of its own -- a string's advance, a tile's aspect --
	 * and that width is what a Fixed bridge reserves between the two columns. An empty bridge
	 * has none, so without this a Natural-sized row would set its two texts hard against each
	 * other. It is a minimum rather than an exact gap for the same reason every other bridge's
	 * natural width is: whatever the columns leave over past it still belongs to the bridge.
	 */
	double bridgeMinGap = 24.0;
	/*
	 * Art bridges only: space left at each end of the art, in pixels, so a leader does not
	 * run into the words it joins. A text bridge carries its own spacing in the string the
	 * user typed, which is why this is confined to art -- applying it there too would move
	 * every bridged row in every document written before it existed.
	 */
	double bridgeGap = 8.0;
	/*
	 * Custom bridges only: paint the file's art in the section's own fill rather than in the
	 * colors it was authored with. The built-in tiles are always painted this way -- they
	 * are drawn white precisely so they can be -- so this only has a say over a user's file.
	 */
	bool bridgeTint = true;
	BridgeFill bridgeFill = BridgeFill::Fixed;
	BridgeSizing bridgeSizing = BridgeSizing::Split;
	/*
	 * Split sizing only: the left column's share of the space the two texts divide between
	 * them, 0.0 to 1.0. At the default 0.5 with a Fixed bridge this is an even split with
	 * the bridge centered -- the layout Bridged sections have always had.
	 */
	double bridgeSplit = 0.5;
	/*
	 * Where a row that does not span the full width sits. Only reachable with Natural
	 * sizing and a Fixed bridge; every other combination fills the width by construction.
	 */
	HAlign bridgeRowAlign = HAlign::Center;
	/*
	 * When set, a side with no text gives its column up to the bridge, so a row carrying
	 * only one of the two texts runs the leader out to the far edge -- a heading row in an
	 * otherwise bridged list. Applies only to a filling bridge, since a fixed one has
	 * nothing to cover the freed space with, and only bites under Split sizing, where the
	 * column is reserved whether or not there is anything in it.
	 */
	bool bridgeSpanEmpty = false;

	/*
	 * Bridged sections only: draw a second line under each side of every row, from the entry's
	 * `subtitle` and `secondarySubtitle`.
	 *
	 * A switch rather than a section type of its own, because a bridged row with subtitles is the
	 * same content laid out the same way with one more line under each side -- and because the
	 * subtitles are optional per row: a side with nothing in it draws nothing and takes no height,
	 * so a list where only some of the rows carry a role under the name needs no second section.
	 *
	 * Switching it off leaves the text where it is, like every other non-destructive choice here.
	 * The stack itself is shaped by `subtitleGap` and `subtitleFirst`, which mean here exactly
	 * what they mean for a title/subtitle list: the pair's own spacing, and which line is on top.
	 */
	bool rowSubtitles = false;

	/* Section Divider sections only: everything it draws. See Divider. */
	Divider divider;

	/* Multi-list sections only. */
	int columns = 2;
	int columnGap = 48;
	/*
	 * When true, entries fill left-to-right across a row before wrapping to the next
	 * row. When false, they fill top-to-bottom down each column before moving right.
	 */
	bool fillAcross = false;

	/*
	 * Styling. `secondaryStyle` is the second text a section carries: the right-hand side of a
	 * Bridged section, or the subtitle under a heading or a list entry. Left off, both are drawn
	 * in the primary style.
	 */
	TextStyle style;
	TextStyle secondaryStyle;
	bool useSecondaryStyle = false;

	/*
	 * The bridge's own ink, when `useBridgeStyle` is set. A bridge otherwise takes the whole of
	 * the section's primary style, which is what makes a leader belong to the row rather than
	 * sit on it -- and is exactly what gets in the way when the leader is the thing meant to
	 * stand out: yellow dots under white names, a rule carrying a sweep the words do not.
	 *
	 * Only the *ink* is taken from here -- the fill, the gradient, the outline and the shadow.
	 * Font, size, alignment and line spacing stay the row's own, so a text bridge is still set in
	 * the face the words either side of it are and nothing about a bridged row's geometry moves
	 * when this is switched on. See Document::effectiveBridgeStyle.
	 */
	TextStyle bridgeStyle;
	bool useBridgeStyle = false;

	/*
	 * The two subtitles of a bridged row, when `rowSubtitles` is on.
	 *
	 * One style each rather than one shared between them, because the two sides of a bridged row
	 * are already styled apart -- that is what `secondaryStyle` is for -- and a subtitle that
	 * could not follow the line it belongs under would be the one part of the row unable to.
	 *
	 * Unlike `secondaryStyle` these carry no switch of their own. There is nothing sensible for
	 * an off position to mean: a subtitle set in exactly the style of the line above it is not a
	 * subtitle, it is a second line of the same text, so `makeDefault` hands out a smaller size
	 * and the styles are simply used whenever the subtitles are drawn at all.
	 */
	TextStyle rowSubtitleStyle;
	TextStyle rowSecondarySubtitleStyle;
	/*
	 * A Stacked list has four lines to style rather than two, and spends these two on its third
	 * and fourth: `style`, `secondaryStyle`, `rowSubtitleStyle`, `rowSecondarySubtitleStyle`,
	 * top to bottom. The same four styles in the same order the four texts of an entry are kept
	 * in, so switching a bridged list with subtitles to a stacked one keeps every line in the
	 * style it was drawn in.
	 */

	/*
	 * Names of the document style presets this section follows, or empty to use the
	 * section's own `style`/`secondaryStyle`/`bridgeStyle`. A name that no longer resolves falls
	 * back to the section's own style as well, so deleting a preset degrades rather than breaks.
	 * The section's own style is never overwritten by a binding, which keeps binding and
	 * unbinding non-destructive in the same way changing a section's type is.
	 *
	 * A preset bound to the bridge contributes its ink and nothing else, the same way the
	 * section's own `bridgeStyle` does.
	 */
	QString stylePresetName;
	QString secondaryStylePresetName;
	QString bridgeStylePresetName;
	QString rowSubtitleStylePresetName;
	QString rowSecondarySubtitleStylePresetName;

	/*
	 * The panels drawn behind this section and behind the things it draws, one entry per slot
	 * that has been given one.
	 *
	 * Held as the slots it *has* rather than as all eight always, which buys three things. A
	 * section with no panels costs nothing to copy onto the undo stack and writes nothing into the
	 * scene collection, which is every section in every roll written before this existed. A slot
	 * can be told apart from a slot deliberately set to draw nothing, which is what makes an
	 * alternate entry panel able to leave every other row bare (see BackgroundSlot::EntryAlt).
	 * And an order is preserved, so the designer lists them the way they were added.
	 *
	 * Never more than one entry per slot: `backgroundEntry` replaces rather than appends, and the
	 * loader drops a duplicate a hand-written document might carry.
	 */
	QVector<SectionBackground> backgrounds;

	/* The panel in `slot`, or a default one that draws nothing. Never null, never dangling. */
	const BackgroundPanel &background(BackgroundSlot slot) const;
	/* The preset `slot` follows, or an empty string. */
	QString backgroundPresetName(BackgroundSlot slot) const;
	/* True when the slot has been given an entry at all, whether or not that entry draws. */
	bool hasBackground(BackgroundSlot slot) const;

	/* The slot's entry, created carrying a panel that draws nothing if it was not there. */
	SectionBackground &backgroundEntry(BackgroundSlot slot);
	/* Removes the slot's entry outright, which is not the same as setting its fill to None. */
	void clearBackground(BackgroundSlot slot);

	/* Vertical padding above and below the section's content, in pixels. */
	int paddingTop = 16;
	int paddingBottom = 16;
	/* Horizontal inset applied to both edges of the section's own box, in pixels. */
	int marginX = 0;
	/*
	 * Everything the section draws, slid sideways by this many pixels: negative to the left,
	 * positive to the right.
	 *
	 * A translation rather than a narrowing, and applied to the whole content rather than to the
	 * text alone -- a heading and the logo beside it shift together, keeping the arrangement they
	 * were given. That is what the setting is for: a section deliberately off its own center,
	 * where `marginX` insets both edges equally and `sectionAlign` only ever picks one of three
	 * places. Content pushed past the edge of the section's box is drawn there rather than
	 * wrapped back inside it; the offset is a nudge, and a nudge large enough to leave the canvas
	 * is one the preview shows immediately.
	 *
	 * Read for every type but the two with no content box of their own: a spacer draws nothing,
	 * and a sticky block spans the canvas while each of its children carries a box -- and an
	 * offset -- of its own.
	 */
	int contentOffsetX = 0;
	/*
	 * How wide one step of `Entry::indent` is, in pixels. Per section, so the tabs of a list are
	 * one decision about that list rather than a number typed again into every row of it.
	 */
	int indentStep = 24;
	/*
	 * The share of the canvas width the section's box occupies, 0.0 to 1.0, and where that
	 * box sits within the canvas.
	 *
	 * `marginX` insets both edges of the box equally, so on its own it can only ever center
	 * the content: a margin large enough to push a block to one side pushes it in from the
	 * other by just as much. These two are what let a section be narrower than the canvas and
	 * then be placed against an edge, with the margin still holding it clear of that edge.
	 *
	 * At the defaults -- the full width, centered -- the box is the canvas and the layout is
	 * exactly what it was before either setting existed.
	 */
	double sectionWidth = 1.0;
	HAlign sectionAlign = HAlign::Center;
	/* Spacer sections only: how tall the blank run is, in pixels. */
	int spacerHeight = 120;

	/*
	 * Paragraph sections only.
	 *
	 * The text is `text`, broken into paragraphs wherever it holds a blank line; a single line
	 * break is a break inside a paragraph, which is how an address or a verse is set. Paragraphs
	 * are held `paragraphGap` apart, which is what makes them read as paragraphs rather than as
	 * one long run with a short line in it.
	 *
	 * `justifyText` stretches every line but each paragraph's last to the full width of the
	 * column, which is how a block of legal text is set. The last line still follows the style's
	 * alignment, so a justified block can end flush left or centered.
	 */
	int paragraphGap = 16;
	bool justifyText = false;

	/*
	 * QR Code sections only.
	 *
	 * `qrText` is what the code encodes -- a link, usually -- and `text` is the optional caption
	 * set under it in the section's style, `logoGap` below it. The code is drawn `qrSize` pixels
	 * square at most: each square is a whole number of pixels so the edges stay crisp, which can
	 * leave it a few pixels smaller than asked for.
	 *
	 * It is drawn on a plate of `qrBackground` that runs `qrQuietZone` squares out past the code
	 * on every side. A reader needs that margin of plain light around the code to find it; four
	 * squares is what the standard asks for, and a plate of nothing at all leaves the code to
	 * whatever the roll is over. Dark on light is what every reader reads; light on dark is
	 * read by most phones but not all.
	 */
	QString qrText;
	int qrSize = 240;
	int qrQuietZone = 4;
	QrErrorCorrection qrErrorCorrection = QrErrorCorrection::Medium;
	QColor qrForeground = QColor(0, 0, 0);
	QColor qrBackground = QColor(255, 255, 255);
	/* How the code is drawn: module and eye shapes, fill, rounded plate, a logo in the middle. */
	QrDesign qrDesign;

	/*
	 * Icon list sections only: how tall each icon is drawn, as a multiple of the height of the
	 * text's capital letters. At 1.0 an icon stands exactly as tall as a capital beside it; it is
	 * centered on the capitals either way, so a larger icon grows above and below the line evenly.
	 */
	double iconScale = 1.4;

	/*
	 * Sticky Ending Block sections only.
	 *
	 * The block is laid into the roll like any other section and scrolls up with it. When its
	 * slot reaches the anchor it detaches and stays there while the rest of the roll goes on
	 * past behind it -- which is why the renderer leaves a hole where it would have been and
	 * hands the block out as a picture of its own, the same bargain an animated logo strikes.
	 *
	 * `children` is what it holds. Any section type may go inside except another sticky block:
	 * pinning something to something already pinned is a second kind of position with a second
	 * set of rules, and one level is what the feature is for. The loader drops any that turn up,
	 * so a hand-written document cannot smuggle one in either.
	 *
	 * Held as std::vector rather than QVector because this is a member of the very type it holds:
	 * the standard says a vector may be declared over an incomplete type, and Qt's containers
	 * make no such promise.
	 */
	std::vector<Section> children;

	/*
	 * When the block shows itself: on its way past with the roll, or only once the roll has gone.
	 *
	 * Defaulted to the entrance blocks have always had, so a document written before there was a
	 * choice loads as the roll its author saw.
	 */
	StickyEntrance stickyEntrance = StickyEntrance::WithRoll;
	/*
	 * How long the block takes to fade up, in seconds, for an entrance that waits: a block that
	 * arrives with the roll is already visible by the time it pins, and has nothing to fade.
	 *
	 * Zero is a cut rather than a fade, which is a use rather than a degenerate case -- a card that
	 * snaps on after a beat of empty frame reads very differently from one that swells up.
	 */
	double stickyFadeIn = 0.75;

	/* Which point of the block is pinned, and where down the canvas that point lands. */
	StickyAnchor stickyAnchor = StickyAnchor::Center;
	/*
	 * How far down the canvas the anchor sits, 0.0 at the top edge and 1.0 at the bottom. A
	 * share of the height rather than a pixel offset, so a roll designed at 1080 still pins
	 * where it was meant to when the canvas is resized under it.
	 */
	double stickyCanvasPosition = 0.5;
	/* A nudge on the pinned position, in pixels. Positive moves the block down. */
	double stickyOffset = 0.0;

	/* How long the block holds once it has pinned, in seconds. */
	double stickyHold = 5.0;
	/*
	 * Hold until something else stops the roll -- a hotkey, a scene change -- rather than for a
	 * measured time. A block that never lets go can never end the roll either, so the designer
	 * says as much next to the release setting rather than leaving it to be discovered on air.
	 */
	bool stickyHoldForever = false;

	StickyRelease stickyRelease = StickyRelease::EndAtHold;

	/*
	 * Action sections only.
	 *
	 * An Action section is a marker with no height: it sits between two sections, travels with the
	 * roll -- or with its block, inside a sticky block -- and when it crosses `actionLine` it runs
	 * every action in `actions`, each after its own delay. Taking no room is what lets one be dropped
	 * anywhere in a finished roll without moving anything or changing how long the roll runs.
	 *
	 * The line is a share of the canvas height, 0.0 at the top and 1.0 at the bottom, like a sticky
	 * block's pin, so it means the same place after a canvas resize. The default is the bottom edge:
	 * the marker crosses it the moment it comes into frame.
	 *
	 * Each action's `delay` is roll time, not wall time -- a paused roll holds its actions back with
	 * it. Filter and visibility changes are put back when the roll restarts, loops or is reset; see
	 * ActionJournal.
	 */
	QVector<EndingActionConfig> actions;
	double actionLine = 1.0;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	/* Builds a section of `type` with defaults appropriate to that type. */
	static Section makeDefault(SectionType type);

	/* The text shown for this section in the designer's list pane. */
	QString displayLabel() const;

	/*
	 * Sticky blocks only, and the designer's business alone: whether the block's children are
	 * folded away in the section list.
	 *
	 * Deliberately neither saved nor loaded. It is view state, like a settings group's fold, and
	 * a roll should open showing what it holds. It lives on the section rather than beside the
	 * list because it then travels with the block through every move, duplicate and undo step
	 * for free -- where a set of folded row numbers kept next to the list would have to be
	 * remapped by hand at each of them, and would fold the wrong block the first time that was
	 * missed.
	 */
	bool childrenCollapsed = false;

	/*
	 * Every field, view state included -- which only ever differs inside the designer, since
	 * `childrenCollapsed` is not saved. Defaulted so that a field added later is compared without
	 * anyone having to remember to add it here.
	 */
	bool operator==(const Section &other) const = default;
};

struct Document {
	QVector<Section> sections;

	/* Named styles sections can bind to. Ordered as the designer lists them. */
	QVector<StylePreset> stylePresets;

	/*
	 * Named panels sections can bind to, in their own collection alongside the text styles.
	 *
	 * Two collections rather than one preset carrying both, because a panel and a typeface are
	 * not one decision: the card behind a sponsor logo has no font to set, and a heading style is
	 * wanted behind several different panels as often as not. It also keeps the two namespaces
	 * apart, so a background called "Card" and a style called "Card" are two things.
	 */
	QVector<BackgroundPreset> backgroundPresets;

	/* Canvas geometry, in pixels. Also the source's reported width/height. */
	int width = 1920;
	int height = 1080;

	QColor background = QColor(0, 0, 0, 0);

	/* Scroll rate in pixels per second of the credits strip traveling upward. */
	double scrollSpeed = 90.0;

	/* Scrolled, or played as pages. See PlaybackMode. */
	PlaybackMode playbackMode = PlaybackMode::Scroll;

	/*
	 * Play the roll in `fitSeconds` rather than at a speed: the scroll speed -- or, played as pages,
	 * how long each page holds -- is worked out so the roll finishes exactly then. Credits are cut to
	 * a piece of music far more often than to a number of pixels a second.
	 *
	 * What is fitted is the roll itself, from the moment it starts moving to the moment it is over:
	 * the start delay comes before it, and a card's hold and fade are counted in it, since they are
	 * part of how long the roll takes. A roll that never ends is fitted to one pass -- the strip's
	 * travel for a loop or for a roll held open by a card that holds for ever. When the time cannot
	 * be met -- the cards' own holds already take longer -- the roll plays as fast as it can.
	 */
	bool fitDuration = false;
	double fitSeconds = 60.0;

	/*
	 * Fade the roll out over this many pixels at the top of the frame and in over this many at the
	 * bottom, rather than cutting it off at the edges. Zero is a hard edge. Drawing only: nothing is
	 * laid out or rasterized again for it. A sticky card fades with the roll while it travels with it,
	 * and is left at full strength while it is held, since the roll then runs behind it.
	 */
	int edgeFadeTop = 0;
	int edgeFadeBottom = 0;

	/*
	 * Played as pages: how long each page holds, how it gives way to the next and how long that
	 * takes, where on the frame a page that does not fill it sits, and how much of the frame is kept
	 * clear at the top and bottom -- which is also what decides how much fits on a page.
	 */
	double pageHold = 4.0;
	PageTransition pageTransition = PageTransition::Fade;
	double pageTransitionTime = 0.6;
	StickyAnchor pageAnchor = StickyAnchor::Center;
	int pageMargin = 48;

	/*
	 * Park the roll at `scrollPosition` instead of advancing it, so a position in the middle of
	 * a long roll can be looked at in the OBS canvas without waiting for the roll to scroll
	 * there. Playback is suspended outright while this is set: the roll does not move, the
	 * ending action cannot fire, and the start/pause hotkeys have nothing to act on.
	 *
	 * It saves with the scene collection like everything else here, which is what makes it an
	 * editing aid the properties window warns about rather than a mode that quietly resets.
	 */
	bool manualScroll = false;
	/*
	 * Where the roll is parked while `manualScroll` is set, as a percentage of the distance it
	 * travels in full -- one canvas height plus the strip, the same distance playback covers. At
	 * 0 the roll is at its start position with nothing on screen yet and at 100 it has cleared
	 * the frame, so both ends are deliberately empty: a percentage of the travel is the only
	 * measure of position that survives an edit to the content or a change of scroll speed.
	 */
	double scrollPosition = 0.0;

	/*
	 * Blank space before the first section and after the last, in pixels. Lead-in is
	 * measured from the bottom edge of the canvas, so a lead-in of 0 means the first
	 * section is already touching the bottom edge when the roll starts.
	 */
	int leadIn = 0;
	int leadOut = 0;

	bool loop = false;
	bool startOnShow = true;
	/* Seconds to wait after the roll is armed before the strip starts moving. */
	double startDelay = 0.0;

	EndingActionConfig endingAction;

	/* Null when no preset carries that name, including for an empty name. */
	const TextStyle *findStylePreset(const QString &name) const;

	/*
	 * The styles a section's text is actually drawn with, once preset bindings are
	 * resolved. Everything that lays out or paints text goes through these rather than
	 * reading Section::style directly.
	 */
	const TextStyle &effectiveStyle(const Section &section) const;
	const TextStyle &effectiveSecondaryStyle(const Section &section) const;

	/*
	 * The styles the two subtitles of a bridged row are drawn with, once their preset bindings
	 * are resolved. Like every other effective-style accessor, a name that no longer resolves
	 * falls back to the section's own copy rather than failing.
	 */
	const TextStyle &effectiveRowSubtitleStyle(const Section &section) const;
	const TextStyle &effectiveRowSecondarySubtitleStyle(const Section &section) const;

	/*
	 * The style a section's bridge is drawn with, once its own preset binding is resolved.
	 *
	 * Returned by value rather than by reference because this is a merge rather than a choice:
	 * the bridge keeps the row's font, size, alignment and line spacing and takes only the ink
	 * -- fill, gradient, outline and shadow -- from the bridge style. That split is what lets a
	 * leader be colored separately without any of a bridged row's geometry moving, since every
	 * width, baseline and height in the row is measured from the fields the merge leaves alone.
	 */
	TextStyle effectiveBridgeStyle(const Section &section) const;

	/* Adds `name`, or replaces the style of the preset already carrying that name. */
	void setStylePreset(const QString &name, const TextStyle &style);

	/* Removes the preset and unbinds every section that referenced it. */
	void removeStylePreset(const QString &name);

	/* --- background presets ---------------------------------------------------------------- */

	/* Null when no background preset carries that name, including for an empty name. */
	const BackgroundPanel *findBackgroundPreset(const QString &name) const;

	/*
	 * The panel a section's slot is actually drawn with, once its preset binding is resolved.
	 * Everything that paints a panel goes through this rather than reading Section::backgrounds
	 * directly, so a binding cannot be bypassed by forgetting to resolve it at one paint site.
	 *
	 * A name that no longer resolves falls back to the section's own copy, the same way a text
	 * style binding does, so deleting a preset degrades rather than breaks.
	 */
	const BackgroundPanel &effectiveBackground(const Section &section, BackgroundSlot slot) const;

	/* Adds `name`, or replaces the panel of the background preset already carrying that name. */
	void setBackgroundPreset(const QString &name, const BackgroundPanel &panel);

	/* Removes the background preset and unbinds every slot on every section that referenced it. */
	void removeBackgroundPreset(const QString &name);

	/*
	 * Adds `name` as a background preset that follows the machine-wide library, taking its
	 * current panel from the library. Does nothing when the library holds no background of that
	 * name.
	 */
	bool linkBackgroundPreset(const QString &name);

	/*
	 * Copies the current library panel into every background preset here marked `linked`, and
	 * returns true when any of them moved. The background twin of `refreshLinkedPresets`, and it
	 * exists for exactly the same reason: the renderer reads the document's own presets and never
	 * learns that a library exists.
	 */
	bool refreshLinkedBackgroundPresets();

	/*
	 * Adds `name` as a preset that follows the machine-wide library, taking its current style
	 * from the library. Does nothing when the library holds no preset of that name.
	 */
	bool linkStylePreset(const QString &name);

	/*
	 * Follows any rename the library has recorded: a linked preset whose name has been changed in
	 * the library is renamed here to match, and every section binding that named it is rewritten.
	 * Returns true when anything moved.
	 *
	 * This is what keeps a rename from quietly unbinding every roll on the machine. A binding is a
	 * name, so without it a renamed preset would leave each document pointing at a name the
	 * library no longer has -- still rendering, from the copy it carries, but no longer following
	 * the library, which is the whole reason it was linked.
	 *
	 * Two cases are deliberately left alone. A preset that is *not* linked is the document's own
	 * and has nothing to do with a library preset that happens to share its name. And a rename
	 * whose new name is already taken by another preset in this document is skipped rather than
	 * forced, because merging two presets is not a rename: the section bindings would land on a
	 * style the user never chose for them. The link stays under the old name, still rendering from
	 * its copy, and migrates by itself if the clash is ever resolved.
	 */
	bool applyLibraryRenames();

	/*
	 * Copies the current library style into every preset here marked `linked`, and returns true
	 * when any of them moved.
	 *
	 * This, rather than a lookup at paint time, is how a library edit reaches a roll: the
	 * document is brought up to date once when the library changes, and everything downstream --
	 * the renderer, the measure pass, the designer's fields -- keeps reading the document's own
	 * presets and never learns that a library exists. A linked preset the library has since lost
	 * keeps the last style it was given rather than reverting, so removing a preset from the
	 * library on one machine does not unstyle the rolls that were bound to it.
	 */
	bool refreshLinkedPresets();

	/* --- fonts ---------------------------------------------------------------------------- */

	/*
	 * Carry the files behind this roll's fonts inside the document, so it renders the same on a
	 * machine that does not have them installed. See `refreshFontBundle`.
	 *
	 * On by default, which is the setting that makes a scene collection self-contained without
	 * anybody having to know that fonts are a problem before they hit it. Off is for the roll
	 * whose fonts may not be passed on -- most commercial licenses say so -- and for the one whose
	 * collection has to stay small.
	 */
	bool bundleFonts = true;

	/* The files themselves. Written by `refreshFontBundle`, registered by the render layer. */
	QVector<BundledFont> bundledFonts;

	/*
	 * Stand-ins for families that could not be carried. Applied only to a family this machine
	 * actually lacks, so a substitution is a fallback rather than an override: install the real
	 * font and the roll goes back to using it with nothing to undo here.
	 */
	QVector<FontSubstitution> fontSubstitutions;

	/*
	 * Every family the roll's visible text is actually set in, deduplicated and sorted.
	 *
	 * Resolved through preset bindings, and asked of a Section Divider rather than assumed of it:
	 * reporting a font for a roll whose every divider is pure artwork would send the user hunting
	 * for a substitution that never happened. A `bridgeStyle` contributes nothing because a bridge
	 * takes only ink from it and keeps the row's own font (see `effectiveBridgeStyle`).
	 */
	QStringList usedFontFamilies() const;

	/*
	 * The same, each family carrying the faces of it the roll names -- sorted, deduplicated, and
	 * with an empty entry standing for the family's default face.
	 *
	 * This is what `usedFontFamilies` is built on, and what the bundle needs in order to carry
	 * the right file: a family is several files, the roll uses some of them, and only the
	 * document knows which.
	 */
	QVector<FontUse> usedFonts() const;

	/* The stand-in recorded for `family`, or an empty string when there is none. */
	QString fontSubstitute(const QString &family) const;

	/* Records a stand-in, replacing any already held for `from`. An empty `to` removes it. */
	void setFontSubstitute(const QString &from, const QString &to);

	/*
	 * Rewrites every style set in one of `families` to the stand-in recorded for it, and returns
	 * true when anything moved. Presets are rewritten too, since a bound section is drawn from one.
	 *
	 * Which families are missing is a question only the render layer can answer, so it is asked
	 * there and the answer passed in -- the model has no business knowing what this machine has
	 * installed, and this is what keeps a substitution from applying on the machine that has the
	 * real font.
	 */
	bool applyFontSubstitutions(const QStringList &families);

	/*
	 * Brings `bundledFonts` in line with the families the roll uses, and returns true when it
	 * changed. Clears the bundle outright when `bundleFonts` is off.
	 *
	 * Walking the machine's font directories is a second's work the first time, so this is called
	 * from the designer -- when a document is applied, and from the font window -- and never from
	 * the render path, which only ever registers what it is handed.
	 *
	 * A family already carried is left alone unless `recollect` says to read it again, so an
	 * ordinary Apply touches the disk only for a family that has just appeared. Either way, a
	 * family whose file cannot be found *here* keeps whatever the document is already carrying
	 * for it: this is the machine that does not have the font, which is the one the bundle exists
	 * for, and re-reading is no reason to throw it away.
	 *
	 * `skipped`, when given, receives the families whose file was found but too large to carry.
	 */
	bool refreshFontBundle(QStringList *skipped = nullptr, bool recollect = false);

	void save(obs_data_t *data) const;

	/*
	 * `migrated`, when given, comes back true if loading brought the document up to date against
	 * the style library -- a preset renamed there and the bindings that named it rewritten, or a
	 * linked style whose copy here was stale. It is what tells the source that `data` is now the
	 * old shape of this document and has to be written back, so the migration survives a restart
	 * rather than being redone from the same stale settings on every load.
	 */
	void load(obs_data_t *data, bool *migrated = nullptr);
	static void defaults(obs_data_t *data);

	/* Serializes to a standalone JSON string for the designer's export button. */
	QString toJson() const;
	bool fromJson(const QString &json, QString *error = nullptr);

	/*
	 * The version of the format `save` writes and `load` understands. 1 is everything written
	 * before versions were recorded; 2 carries bundled fonts compressed and named by their hash.
	 * A document from a newer plugin still loads -- every reader here ignores what it does not
	 * know -- and says so in the log.
	 */
	static constexpr int kSchemaVersion = 2;

	/*
	 * This document with everything that only moves the finished roll taken out: scroll speed,
	 * looping, starting, the scrub position, the ending action. What is left is everything the roll
	 * is laid out and drawn from -- lead-in and lead-out included, since they are part of the strip.
	 *
	 * Built by blanking the playback fields rather than by copying the content ones, so a field
	 * added later counts as content by default: wrong in that direction it costs a redundant layout,
	 * wrong in the other it would leave a stale roll on screen.
	 */
	Document contentOnly() const;

	/* True when the two would lay out and draw the same roll. Cheap: a comparison, not a serialization. */
	bool sameContent(const Document &other) const;

	bool operator==(const Document &other) const = default;
};

} // namespace closingtime
