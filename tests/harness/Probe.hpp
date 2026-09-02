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

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <functional>
#include <initializer_list>

#include "model/CreditsModel.hpp"
#include "render/StripRenderer.hpp"

/*
 * Everything a check needs to ask about a rendered document.
 *
 * Two kinds of question, and the difference matters when a test is written. The **layout boxes**
 * say where the layout *put* something -- the column a run of text was given, the rectangle a logo
 * was fitted into. The **ink** says where pixels actually landed. A column is not its contents: a
 * centered title in a wide column inks the middle of it, and a check that means "the leader reaches
 * the words" has to ask about ink, while one that means "the column is the full content width" has
 * to ask about boxes. Asking the wrong one is the most common way a check here passes while the
 * thing it names is broken.
 */

namespace closingtime::test {

/* --- rendering --------------------------------------------------------------------------- */

/* The document's height in pixels, without rasterizing anything. */
int measure(const Document &document);

/* Every rectangle the layout reported, in strip space. */
LayoutBoxes layout(const Document &document);

/* Just the boxes of one kind, in the order the layout reported them. */
QVector<QRectF> boxesOf(const Document &document, LayoutBox::Kind kind);

/*
 * One box of a kind, or a null rectangle when there are fewer than `index + 1` of them. Returning
 * null rather than indexing past the end keeps a broken build reporting failures instead of
 * aborting halfway through the run.
 */
QRectF boxOf(const Document &document, LayoutBox::Kind kind, int index = 0);

/* The rendered strip, and the same thing flattened into one image for probing or saving. */
Strip renderStrip(const Document &document);

/*
 * The same, with animation support switched on.
 *
 * Kept apart from renderStrip because the difference is the point: without an animation cache an
 * animated logo is baked into the strip as its first frame, and with one it is left as a hole with
 * a placement reported beside it. Suites that are not about animation use the plain call and get
 * the behavior the renderer has always had.
 */
Strip renderAnimatedStrip(const Document &document);
QImage renderImage(const Document &document);

/*
 * The tiles of an already-rendered strip, assembled into one image.
 *
 * Separate from renderImage because a check about animation has a Strip in hand rather than a
 * document: what it is asking about -- the hole an animated logo leaves -- only exists in a strip
 * rendered with animation support, and re-rendering to look at it would render a different one.
 */
QImage flatten(const Strip &strip);

/* --- ink --------------------------------------------------------------------------------- */

/*
 * The bounding box of every non-transparent pixel, in the coordinates of the image it was measured
 * from. Empty when nothing was drawn at all.
 */
struct Ink {
	int left = 0;
	int right = -1;
	int top = 0;
	int bottom = -1;

	bool isEmpty() const { return right < left || bottom < top; }
	int width() const { return isEmpty() ? 0 : right - left + 1; }
	int height() const { return isEmpty() ? 0 : bottom - top + 1; }
};

Ink inkOf(const QImage &image);

/*
 * Ink restricted to a region -- the rows one line of a stacked pair occupies, the columns to one
 * side of a leader. This is how a single line's ink gets measured without the logo beside it, or
 * the section above it, answering for it.
 */
Ink inkOf(const QImage &image, const QRect &within);

/* Convenience: the ink of one layout box, measured over exactly the rows and columns it covers. */
Ink inkOf(const QImage &image, const QRectF &box);

/* True when any pixel of that column carries alpha. */
bool inksColumn(const QImage &image, int x);

/* --- structure --------------------------------------------------------------------------- */

/*
 * How many boxes the layout placed outside the content area of the section that owns them.
 * Zero is the invariant; the count is returned rather than a bool so a failure says how badly.
 *
 * Horizontal and vertical both, since a section that overflows either way is drawing into its
 * neighbor or off the canvas.
 */
int boxesOutsideContent(const Document &document);

/* Tiles have to be contiguous and cover the strip exactly; returns an empty string when they do. */
QString tilingProblem(const Strip &strip);

/* --- sweeps ------------------------------------------------------------------------------ */

/*
 * A configuration matrix walked as one call rather than as eight nested loops.
 *
 * Each axis is a named list of mutations applied to a copy of the base section; `body` is handed
 * every combination together with a label naming it, and pushes that label as a Context so a
 * failure out of thousands says which combination raised it.
 */
struct Axis {
	QString name;
	QVector<QPair<QString, std::function<void(Section &)>>> values;
};

/*
 * Builds an axis from a list of named values and a setter, which is what most axes are.
 *
 * The value type is given explicitly -- `axis<HAlign>("align", {...}, ...)` -- because a braced
 * list is not a deduced context, so writing it out is the difference between one clear call and a
 * page of compiler output about deduction failing.
 */
template<typename T, typename Setter>
Axis axis(const QString &name, std::initializer_list<QPair<QString, T>> values, Setter setter)
{
	Axis result;
	result.name = name;
	for (const auto &value : values) {
		const T held = value.second;
		result.values.append({value.first, [setter, held](Section &section) {
					      setter(section, held);
				      }});
	}
	return result;
}

void sweep(const Section &base, const QVector<Axis> &axes, const std::function<void(const Section &)> &body);

/* How many combinations a set of axes describes, for a check that the matrix is the size meant. */
int sweepSize(const QVector<Axis> &axes);

/* --- artifacts --------------------------------------------------------------------------- */

/*
 * Writes a PNG into the run's artifact directory, or does nothing when the run was not asked for
 * artifacts. Named so it is safe to call unconditionally from a suite: a normal run pays a render
 * it was doing anyway and no disk at all.
 *
 * There are deliberately no golden images to compare against. Text rasterizes differently across
 * font versions, hinting settings and platforms, so a committed PNG would fail on machines where
 * nothing is wrong -- these are for looking at, and the checks that must hold are measurements.
 */
void saveArtifact(const QString &name, const QImage &image);
void saveArtifact(const QString &name, const Document &document);

} // namespace closingtime::test
