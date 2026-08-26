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
#include <QRectF>
#include <QString>
#include <QVector>

#include "model/Gradient.hpp"

namespace closingtime {

/*
 * What a panel is filled with.
 *
 * The two gradients are split the way TextFill splits them rather than being one fill with a shape
 * beside it, so a panel's fill and a run of glyphs' fill are the same list of answers in the same
 * order -- and so the sweep is mapped over the panel by the very code that maps one over a block of
 * text (see gradientBrush in render/BackgroundPainter.hpp).
 *
 * `None` is not "a transparent colour": it is the panel not being drawn at all, which is what
 * every slot on every section starts as and what every document written before panels existed
 * loads as. Keeping it a fill rather than a separate switch means one setting says whether there
 * is a panel and what it is made of, and turning a panel off leaves everything else about it --
 * its corners, its border, its image -- sitting where it was for when it is turned back on.
 */
enum class BackgroundFill { None, Color, LinearGradient, RadialGradient, Image };

const char *backgroundFillId(BackgroundFill fill);
BackgroundFill backgroundFillFromId(const char *id, BackgroundFill fallback = BackgroundFill::None);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *backgroundFillName(BackgroundFill fill);

/* Every fill in the order the designer's picker should list them. */
const QVector<BackgroundFill> &allBackgroundFills();

/*
 * What happens when an image and the panel it fills are not the same shape.
 *
 *   Cover   - scaled to fill the panel, whatever overflows cropped
 *   Contain - scaled to fit inside the panel, leaving the panel's own fill showing past it
 *   Stretch - scaled to the panel exactly, ignoring the image's proportions
 *   Tile    - drawn at its own size, repeated across the panel
 *
 * Cover is the default because a panel is a backdrop: an edge of bare panel where the proportions
 * happen not to match reads as a mistake, where losing the outer few percent of a photograph does
 * not. Tile is the one that ignores the panel's size entirely, which is what a seamless texture
 * wants and what anything with a subject in it does not.
 */
enum class BackgroundImageFit { Cover, Contain, Stretch, Tile };

const char *backgroundImageFitId(BackgroundImageFit fit);
BackgroundImageFit backgroundImageFitFromId(const char *id, BackgroundImageFit fallback = BackgroundImageFit::Cover);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *backgroundImageFitName(BackgroundImageFit fit);

/* Every fit in the order the designer's picker should list them. */
const QVector<BackgroundImageFit> &allBackgroundImageFits();

/*
 * A stroke drawn around the panel's edge.
 *
 * Drawn *inside* the panel's own bounds rather than straddling them, so the rectangle the outset
 * describes is the outermost thing the panel paints and a border can be made heavier without the
 * panel quietly growing into its neighbours. A rounded panel's border follows the same corners the
 * fill does, since it is the same path stroked rather than a second shape.
 */
struct BackgroundBorder {
	bool enabled = false;
	double width = 2.0;
	QColor color = QColor(255, 255, 255, 200);

	bool operator==(const BackgroundBorder &other) const
	{
		return enabled == other.enabled && qFuzzyCompare(width + 1.0, other.width + 1.0) &&
		       color == other.color;
	}
	bool operator!=(const BackgroundBorder &other) const { return !(*this == other); }
};

/* Bounds on a corner radius and an outset, shared by the loader and the designer's spin boxes. */
constexpr double kMaxBackgroundRadius = 4096.0;
constexpr double kMaxBackgroundOutset = 4096.0;
constexpr double kMaxBackgroundBorder = 512.0;

/*
 * A panel drawn behind something: a section, a heading, a logo, a row of a list.
 *
 * **A panel never takes part in layout.** It is painted behind content whose box has already been
 * decided, and its outset reaches outside that box the way a text shadow reaches outside a line of
 * type -- see TextStyle::effectBleed, which this is deliberately modelled on. Nothing about
 * switching a panel on moves a section, changes the height of the roll or alters its duration, so
 * a roll can be given cards and bands after it has been timed. The cost is the same cost a shadow
 * pays: the renderer has to be told how far outside its own box a section may paint, which is what
 * `bleed()` reports and what keeps a panel from being cut in half at a tile seam.
 *
 * The room *inside* a panel is the existing padding rather than a second set of numbers. A
 * section's `paddingTop`/`paddingBottom` and `marginX` already say how much air its content has,
 * they already take part in layout, and a panel drawn around the outside of them is the panel
 * anyone would draw by hand. The outset is for the other case -- a band that has to reach past the
 * section's box, out to the edges of the canvas -- and is per side because reaching past one edge
 * is a different want from reaching past all four.
 */
struct BackgroundPanel {
	BackgroundFill fill = BackgroundFill::None;

	QColor color = QColor(0, 0, 0, 160);
	GradientSpec gradient;

	/* Absolute path to an image QImageReader can decode. Animated files contribute their first
	 * frame only: the strip is rasterised once and scrolled, and a panel -- unlike a logo -- has
	 * no quad of its own to be drawn from. */
	QString imagePath;
	BackgroundImageFit imageFit = BackgroundImageFit::Cover;

	/*
	 * Applied to everything the panel paints, fill and border together, 0.0 to 1.0.
	 *
	 * Over the whole panel rather than folded into the colour's own alpha, because an image has
	 * no alpha to fold it into and "the same card, half as present" should be one number wherever
	 * the panel came from -- including when it came from a preset shared with another section.
	 */
	double opacity = 1.0;

	/* How far past the content's box the panel reaches on each side, in pixels. Negative insets. */
	double outsetLeft = 0.0;
	double outsetTop = 0.0;
	double outsetRight = 0.0;
	double outsetBottom = 0.0;

	/*
	 * Corner radii in pixels, clockwise from the top left.
	 *
	 * Four rather than one because the shapes that need them are asymmetric by nature: a header
	 * rounded at the top and square at the bottom reads as sitting on the section below it, and a
	 * tab needs exactly that. A radius larger than the panel can hold is scaled down at paint time
	 * rather than clamped here, so the same panel put behind a taller section keeps its shape.
	 */
	double radiusTopLeft = 0.0;
	double radiusTopRight = 0.0;
	double radiusBottomRight = 0.0;
	double radiusBottomLeft = 0.0;

	BackgroundBorder border;

	/* True when the panel would paint anything at all. */
	bool isVisible() const;

	/*
	 * How far outside the box it is given the panel paints, in pixels -- the largest outset, and
	 * never less than zero however far the outsets inset. The border is drawn inside the panel,
	 * so it adds nothing here.
	 */
	double bleed() const;

	/* `box` grown by the outsets, which is the rectangle the panel actually paints. */
	QRectF outsetRect(const QRectF &box) const;

	/* Sets all four corners at once, which is what the designer's single-radius shortcut writes. */
	void setRadius(double radius);
	/* True when all four corners carry the same radius. */
	bool hasUniformRadius() const;

	bool operator==(const BackgroundPanel &other) const;
	bool operator!=(const BackgroundPanel &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * Which of the things a section draws a panel sits behind.
 *
 * A slot rather than a field per component. Eight named `BackgroundPanel` members on Section would
 * be eight copies of the same save/load, eight branches in the editor and eight places for a new
 * panel setting to be forgotten; a slot means persistence, the editor and the renderer each say it
 * once and ask which slot they are dealing with.
 *
 *   Section  - behind everything the section draws, over its whole height including its padding
 *   Title    - behind the primary text block: a heading, or the left side of a bridged row
 *   Subtitle - behind the secondary text: the line under a heading, or a row's right side
 *   Logo     - behind each logo the section places, one panel per logo
 *   Entry    - behind each entry of a list, or each cell of a multi-column one
 *   EntryAlt - behind every *other* entry instead, for a striped list; see below
 *   Bridge   - behind the leader joining the two sides of a bridged row
 *   Divider  - behind a Section Divider's artwork, over the box that artwork occupies
 *
 * `EntryAlt` is the one slot whose *absence* means something different from its being empty. A
 * list with no alternate set draws `Entry` behind every row; a list that has an alternate draws it
 * behind the odd-numbered ones, and an alternate deliberately left on `None` is how every other
 * row is left bare. That is why a section holds the slots it has been given rather than all eight
 * always -- see Section::backgrounds.
 */
enum class BackgroundSlot { Section, Title, Subtitle, Logo, Entry, EntryAlt, Bridge, Divider };

const char *backgroundSlotId(BackgroundSlot slot);
BackgroundSlot backgroundSlotFromId(const char *id, BackgroundSlot fallback = BackgroundSlot::Section);

/* Untranslated display name, used as the fallback when no locale string exists. */
const char *backgroundSlotName(BackgroundSlot slot);

/* Every slot in the order the designer should present them. */
const QVector<BackgroundSlot> &allBackgroundSlots();

/*
 * One slot's panel on a section, and the preset it follows.
 *
 * `presetName` binds exactly the way a section's `stylePresetName` binds: the named preset is what
 * is drawn, `panel` is kept untouched underneath it, and a name that no longer resolves falls back
 * to `panel` rather than failing. Binding and unbinding are therefore non-destructive in both
 * directions, like every other binding here.
 */
struct SectionBackground {
	BackgroundSlot slot = BackgroundSlot::Section;
	BackgroundPanel panel;
	QString presetName;

	bool operator==(const SectionBackground &other) const
	{
		return slot == other.slot && panel == other.panel && presetName == other.presetName;
	}
	bool operator!=(const SectionBackground &other) const { return !(*this == other); }

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

/*
 * A named panel on the document, which sections bind to by name.
 *
 * The text-style twin of this is StylePreset, and the two are deliberately separate types in
 * separate collections rather than one preset carrying both. A panel and a typeface are not the
 * same decision made twice: the card behind a sponsor logo has no font, and a heading style is
 * wanted behind three different panels as often as not. Keeping them apart also keeps the
 * namespaces apart, so a background called "Card" and a text style called "Card" are two things
 * and neither shadows the other.
 *
 * `linked` means the same thing it means on a StylePreset: the preset follows the machine-wide
 * library, and the copy held here is the last one this document saw -- which is what a scene
 * collection opened on a machine with no library file renders from.
 */
struct BackgroundPreset {
	QString name;
	BackgroundPanel panel;
	bool linked = false;

	void save(obs_data_t *data) const;
	void load(obs_data_t *data);
};

} // namespace closingtime
