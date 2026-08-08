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

#include "model/BridgeArt.hpp"
#include "model/EndingAction.hpp"

namespace closingtime {

/*
 * Every section the user can place in the roll. The enum values are persisted by their
 * string ids (see sectionTypeId), never by their numeric value, so the enum may be
 * reordered freely without breaking existing scene collections.
 */
enum class SectionType {
	Title,
	TitleWithLogo,
	LogoTitle,
	Header,
	HeaderWithLogo,
	LogoHeader,
	Bridged,
	TextList,
	LogoList,
	MultiTextList,
	MultiLogoList,
	Spacer,
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
};

struct TextStyle {
	QString family = QStringLiteral("Sans Serif");
	/*
	 * Sized in pixels rather than points: the strip is laid out in video pixels and has
	 * to come out identical no matter the DPI of the screen OBS is running on.
	 */
	int pixelSize = 32;
	bool bold = false;
	bool italic = false;
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
 */
struct StylePreset {
	QString name;
	TextStyle style;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * A logo reference. `path` is an absolute path to an image QImageReader can decode
 * (PNG/JPEG/SVG/etc). `maxHeight` bounds the drawn size; width follows from the aspect
 * ratio and is additionally clamped to the available column width at layout time.
 */
struct LogoRef {
	QString path;
	int maxHeight = 96;

	bool isEmpty() const { return path.isEmpty(); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * One entry inside a list-shaped section. Which fields matter depends on the owning
 * section's type:
 *   Bridged        -> text (left) and secondaryText (right), joined by the bridge string
 *   TextList       -> text
 *   MultiTextList  -> text, placed into columns in the section's fill order
 *   LogoList       -> logo
 *   MultiLogoList  -> logo, placed into columns in the section's fill order
 */
struct Entry {
	QString text;
	QString secondaryText;
	LogoRef logo;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

struct Section {
	SectionType type = SectionType::Title;

	/* Shown in the designer's section list only; never rendered. */
	QString label;

	/* Single-line content for Title/Header and their logo variants. */
	QString text;
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

	/* Multi-list sections only. */
	int columns = 2;
	int columnGap = 48;
	/*
	 * When true, entries fill left-to-right across a row before wrapping to the next
	 * row. When false, they fill top-to-bottom down each column before moving right.
	 */
	bool fillAcross = false;

	/* Styling. `secondaryStyle` is used for the right-hand side of Bridged sections. */
	TextStyle style;
	TextStyle secondaryStyle;
	bool useSecondaryStyle = false;

	/*
	 * Names of the document style presets this section follows, or empty to use the
	 * section's own `style`/`secondaryStyle`. A name that no longer resolves falls back to
	 * the section's own style as well, so deleting a preset degrades rather than breaks.
	 * The section's own style is never overwritten by a binding, which keeps binding and
	 * unbinding non-destructive in the same way changing a section's type is.
	 */
	QString stylePresetName;
	QString secondaryStylePresetName;

	/* Vertical padding above and below the section's content, in pixels. */
	int paddingTop = 16;
	int paddingBottom = 16;
	/* Horizontal inset applied to both edges, in pixels. */
	int marginX = 0;
	/* Spacer sections only: how tall the blank run is, in pixels. */
	int spacerHeight = 120;

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

	/* Adds `name`, or replaces the style of the preset already carrying that name. */
	void setStylePreset(const QString &name, const TextStyle &style);

	/* Removes the preset and unbinds every section that referenced it. */
	void removeStylePreset(const QString &name);

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
	static void defaults(obs_data_t *data);

	/* Serialises to a standalone JSON string for the designer's export button. */
	QString toJson() const;
	bool fromJson(const QString &json, QString *error = nullptr);
};

} // namespace closingtime
