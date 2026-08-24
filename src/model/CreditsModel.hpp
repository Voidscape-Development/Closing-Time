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

#include "model/BridgeArt.hpp"
#include "model/DividerArt.hpp"
#include "model/EndingAction.hpp"
#include "model/FontBundle.hpp"

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
	SectionDivider,
	Spacer,
	/*
	 * A run of sections that pins itself to a place on the canvas instead of scrolling past it.
	 * It holds other sections rather than content of its own; see `Section::children`.
	 */
	StickyBlock,
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
 * Which part of a sticky block is pinned, and to what.
 *
 * The pin is a pair of points: one on the block and one down the canvas. Saying "the middle of the
 * block, halfway down the frame" needs both halves, and a single number could only ever express
 * one of them -- which is why a block's top edge landing at a fixed distance from the top of the
 * frame and a block *centred* in the frame are two settings rather than one with a fudge in it.
 */
enum class StickyAnchor { Top, Center, Bottom };

const char *stickyAnchorId(StickyAnchor anchor);
StickyAnchor stickyAnchorFromId(const char *id, StickyAnchor fallback = StickyAnchor::Center);

/* Where on the block the pin sits: 0 at its top edge, 1 at its bottom. */
double stickyAnchorFraction(StickyAnchor anchor);

/*
 * What a sticky block does when its hold runs out.
 *
 *   EndAtHold      - the hold expiring *is* the end of the roll: the ending action fires and the
 *                    block stays where it is. The closing card that stays on screen.
 *   ResumeThenEnd  - the block carries on up and off the top, and the roll ends the way it always
 *                    has, once everything has left the frame.
 *   ResumeEndAtHold- the block carries on up and off, but the ending action fires at the moment
 *                    the hold ends rather than waiting for it to clear the frame.
 *
 * Three rather than one because they answer two independent questions -- does the block leave, and
 * what counts as the end of the roll -- and every combination of the two is something somebody
 * builds a roll around.
 */
enum class StickyRelease { EndAtHold, ResumeThenEnd, ResumeEndAtHold };

const char *stickyReleaseId(StickyRelease release);
StickyRelease stickyReleaseFromId(const char *id, StickyRelease fallback = StickyRelease::EndAtHold);

/* True when the block scrolls on after its hold rather than staying where it was pinned. */
bool stickyReleaseResumes(StickyRelease release);

/* True when the hold running out is what finishes the roll, rather than the strip clearing. */
bool stickyReleaseEndsAtHold(StickyRelease release);

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
 *             leaves a centred title stranded halfway across the frame from its own logo:
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
 *   Radial - the stops run outward from the centre of the block to its corners.
 *
 * Both gradients are mapped over the block of text being drawn -- one section's title, one
 * list entry, one side of a bridged row -- rather than over the canvas or the whole strip.
 * Mapping over the strip would make a stop's colour depend on how long the roll happens to
 * be; mapping per block means a run of names all share the same sweep as each other.
 */
enum class TextFill { Solid, LinearGradient, RadialGradient };

const char *textFillId(TextFill fill);
TextFill textFillFromId(const char *id, TextFill fallback = TextFill::Solid);

/* One colour stop. `position` is 0.0 at the start of the gradient's axis and 1.0 at its end. */
struct GradientStop {
	double position = 0.0;
	QColor color = QColor(255, 255, 255);

	bool operator==(const GradientStop &other) const
	{
		return qFuzzyCompare(position + 1.0, other.position + 1.0) && color == other.color;
	}
	bool operator!=(const GradientStop &other) const { return !(*this == other); }
};

struct GradientSpec {
	/*
	 * Direction of a linear gradient, in degrees clockwise from straight down: 0 runs top
	 * to bottom, 90 left to right, 180 bottom to top. Ignored by a radial gradient, which
	 * has no axis to point.
	 */
	double angle = 0.0;
	QVector<GradientStop> stops = {GradientStop{0.0, QColor(255, 255, 255)},
				       GradientStop{1.0, QColor(140, 140, 140)}};

	/*
	 * The stops as QGradient wants them: sorted, clamped into 0..1, and padded out to at
	 * least two so a gradient left with one stop still paints that colour rather than
	 * falling back to black. Everything that draws a gradient goes through this.
	 */
	QVector<QPair<qreal, QColor>> resolvedStops() const;

	bool operator==(const GradientSpec &other) const
	{
		return qFuzzyCompare(angle + 1.0, other.angle + 1.0) && stops == other.stops;
	}
	bool operator!=(const GradientSpec &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

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
	 * True when the text needs more than a pen colour to draw. The renderer keeps a fast
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

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One item in the middle of a Section Divider, between the two arms.
 *
 * The centre is a list rather than a single choice because the shapes that turn up there in
 * practice are compounds: a diamond with a dot either side of it, a word flanked by two
 * ornaments, a monogram between a pair of curls. Offering one slot would mean either shipping
 * every one of those combinations as its own tile, or drawing them by hand in a file; a list of
 * small pieces builds them out of parts the plugin already has and keeps each one adjustable.
 *
 * An empty list is the ordinary case, not a degenerate one: it is the unbroken rule.
 */
struct DividerPiece {
	enum class Kind {
		/* A shape from the built-in library, or a file when `shape` is Custom. */
		Ornament,
		/* A word set in the section's own style -- `PART II`, `MMXXVI`. */
		Text,
		Logo,
	};

	Kind kind = Kind::Ornament;

	/* Ornament pieces only. */
	DividerShape shape = DividerShape::Diamond;
	/* Ornament pieces whose shape is Custom: absolute path to the artwork. */
	QString svgPath;
	/*
	 * Ornament pieces only: a multiplier on the height the shape's table row asks for, so a
	 * run of three diamonds can have a larger one in the middle without a second Diamond in
	 * the library. Text takes its size from the section's style and a logo from its own
	 * `maxHeight`, both of which are already a size the user set, so neither reads this.
	 */
	double scale = 1.0;

	/* Text pieces only. */
	QString text;
	/* Logo pieces only. */
	LogoRef logo;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

const char *dividerPieceKindId(DividerPiece::Kind kind);
DividerPiece::Kind dividerPieceKindFromId(const char *id, DividerPiece::Kind fallback = DividerPiece::Kind::Ornament);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *dividerPieceKindName(DividerPiece::Kind kind);

/* Every piece kind in the order the designer's picker should list them. */
const QVector<DividerPiece::Kind> &allDividerPieceKinds();

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
	 * colours it was authored with. The built-in tiles are always painted this way -- they
	 * are drawn white precisely so they can be -- so this only has a say over a user's file.
	 */
	bool bridgeTint = true;
	BridgeFill bridgeFill = BridgeFill::Fixed;
	BridgeSizing bridgeSizing = BridgeSizing::Split;
	/*
	 * Split sizing only: the left column's share of the space the two texts divide between
	 * them, 0.0 to 1.0. At the default 0.5 with a Fixed bridge this is an even split with
	 * the bridge centred -- the layout Bridged sections have always had.
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

	/*
	 * Section Divider sections only.
	 *
	 * A divider is composed rather than drawn from one piece of artwork: an end cap, an arm
	 * running inward from it, whatever the centre stack holds, then the same again mirrored.
	 * `dividerThickness` is the one number that sizes all of it -- every shape's table row
	 * declares its own height as a multiple of the rule it belongs to -- so a divider stays in
	 * proportion when it is made heavier rather than needing each part resized in turn.
	 *
	 * The artwork is inked exactly as a bridge is: the section's own style, or `bridgeStyle`
	 * when `useBridgeStyle` is set. That is deliberate reuse rather than a field left lying
	 * around -- "colour the art separately from the text" is the same want in both places, and
	 * a divider whose text is white while its rule carries the title's gold sweep is precisely
	 * what the override is for.
	 */
	/*
	 * The left-hand end, outermost piece first.
	 *
	 * A list of the same `DividerPiece` the centre holds rather than a single shape, because an
	 * end is the same kind of thing a middle is: something drawn once at its own size, and just
	 * as often a compound -- a diamond outside an arrowhead, a year set against the rule. Making
	 * them the same list means a shape offered in one place is offered in the other, a piece can
	 * be resized where it sits, and a word or a mark can cap a rule exactly as it can break one.
	 *
	 * Empty is an end with nothing on it, which is the ordinary case for a plain rule.
	 */
	QVector<DividerPiece> dividerCap;
	/*
	 * The right-hand end, used only when `dividerMirrorEnds` is off. Kept whether or not it is
	 * in use, so mirroring can be switched off and back on without losing what was set.
	 *
	 * Written in the same order as the left-hand one -- outermost piece first -- and drawn
	 * flipped, so an end reads the same way whichever list it came from.
	 */
	QVector<DividerPiece> dividerEndCap;
	/*
	 * Draw the right-hand end as the left one flipped. On by default, and true of every
	 * ornamental rule that is not an arrow pointing somewhere: it is what stops the two ends of
	 * the same divider from drifting apart as the design is worked on.
	 */
	bool dividerMirrorEnds = true;

	DividerShape dividerArm = DividerShape::Rule;
	/* Custom arms only: absolute path to the artwork tiled along each arm. */
	QString dividerArmSvg;

	/*
	 * How tall the rule itself is drawn, in pixels. Every other part is a multiple of it.
	 */
	double dividerThickness = 4.0;
	/*
	 * Space left clear where an arm meets something, in pixels: the cap outside it and the
	 * centre stack inside it. One number rather than two, because a divider whose gaps differ
	 * at the two ends of the same arm reads as a mistake rather than as a setting.
	 */
	double dividerGap = 12.0;
	/* Space between consecutive pieces of the centre stack, in pixels. */
	double dividerPieceGap = 10.0;

	/* What sits between the two arms, in order, left to right. Empty is an unbroken rule. */
	QVector<DividerPiece> dividerCentre;

	/*
	 * How many rules run in parallel, stacked about the divider's midline, and the vertical
	 * space between them in pixels. The centre stack is drawn once over the whole stack rather
	 * than once per rule -- three lines broken by one ornament is the deco figure; three
	 * complete dividers touching is not.
	 */
	int dividerRules = 1;
	double dividerRuleGap = 6.0;
	/*
	 * How much shorter each rule is than the one nearer the midline, in pixels, so a stack of
	 * three tapers to a wedge. At 0 the rules are all the same length. An even-numbered stack
	 * has no middle rule to measure from, so its two innermost are inset half of this each and
	 * the figure stays symmetric either way.
	 */
	double dividerRuleInset = 0.0;

	/*
	 * Custom artwork only: paint the files in the section's own fill rather than in the colours
	 * they were authored with. The built-in shapes are always painted this way -- they are drawn
	 * white precisely so they can be -- so this only has a say over a user's files, and covers
	 * all three slots at once because a divider whose cap is tinted and whose centre is not is
	 * not a design anyone reaches for on purpose.
	 */
	bool dividerTint = true;

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

	/* Vertical padding above and below the section's content, in pixels. */
	int paddingTop = 16;
	int paddingBottom = 16;
	/* Horizontal inset applied to both edges of the section's own box, in pixels. */
	int marginX = 0;
	/*
	 * The share of the canvas width the section's box occupies, 0.0 to 1.0, and where that
	 * box sits within the canvas.
	 *
	 * `marginX` insets both edges of the box equally, so on its own it can only ever centre
	 * the content: a margin large enough to push a block to one side pushes it in from the
	 * other by just as much. These two are what let a section be narrower than the canvas and
	 * then be placed against an edge, with the margin still holding it clear of that edge.
	 *
	 * At the defaults -- the full width, centred -- the box is the canvas and the layout is
	 * exactly what it was before either setting existed.
	 */
	double sectionWidth = 1.0;
	HAlign sectionAlign = HAlign::Center;
	/* Spacer sections only: how tall the blank run is, in pixels. */
	int spacerHeight = 120;

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
	 * A panel drawn behind the block while it is pinned, so the roll running past underneath
	 * does not read through its lettering.
	 *
	 * Off by default: a closing card set over the last of the credits is a perfectly good look,
	 * and a backdrop that appeared without being asked for would be the plugin making that
	 * decision. `stickyBackdropPadding` grows it past the block's own bounds on every side.
	 */
	bool stickyBackdrop = false;
	QColor stickyBackdropColor = QColor(0, 0, 0, 180);
	double stickyBackdropPadding = 24.0;

	bool visible = true;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);

	/* Builds a section of `type` with defaults appropriate to that type. */
	static Section makeDefault(SectionType type);

	/* The text shown for this section in the designer's list pane. */
	QString displayLabel() const;
};

struct Document {
	QVector<Section> sections;

	/* Named styles sections can bind to. Ordered as the designer lists them. */
	QVector<StylePreset> stylePresets;

	/* Canvas geometry, in pixels. Also the source's reported width/height. */
	int width = 1920;
	int height = 1080;

	QColor background = QColor(0, 0, 0, 0);

	/* Scroll rate in pixels per second of the credits strip travelling upward. */
	double scrollSpeed = 90.0;

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
	 * leader be coloured separately without any of a bridged row's geometry moving, since every
	 * width, baseline and height in the row is measured from the fields the merge leaves alone.
	 */
	TextStyle effectiveBridgeStyle(const Section &section) const;

	/* Adds `name`, or replaces the style of the preset already carrying that name. */
	void setStylePreset(const QString &name, const TextStyle &style);

	/* Removes the preset and unbinds every section that referenced it. */
	void removeStylePreset(const QString &name);

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
	 * whose fonts may not be passed on -- most commercial licences say so -- and for the one whose
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

	/* Serialises to a standalone JSON string for the designer's export button. */
	QString toJson() const;
	bool fromJson(const QString &json, QString *error = nullptr);
};

} // namespace closingtime
