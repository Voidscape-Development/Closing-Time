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

#include <QString>
#include <QVector>

#include "model/CreditsModel.hpp"

namespace closingtime::test {

/* --- documents --------------------------------------------------------------------------- */

/*
 * A document holding one section, at a fixed canvas size and with no lead-in or lead-out, so a
 * measured height is the section's own and nothing has to be subtracted back off it.
 */
Document documentWith(const Section &section);

/* The same, for several sections in order. */
Document documentWith(const QVector<Section> &sections);

/* One section of every type, defaults throughout, with artwork wherever a type places some. */
Document everySectionType();

/* --- sections ---------------------------------------------------------------------------- */

/*
 * `Section::makeDefault` with the padding taken off, for the many checks that are about a
 * section's content and would otherwise have to subtract its padding from every measurement.
 */
Section unpadded(SectionType type);

/* Points a section at the generated test logo, at `maxHeight` pixels. */
Section &withLogo(Section &section, int maxHeight = 110);

/* --- assets ------------------------------------------------------------------------------ */

/*
 * A small PNG written into a temporary directory the first time it is asked for, and reused for
 * the rest of the run.
 *
 * Generated rather than committed: a binary in the tree is one more thing to keep in step with
 * the tests that read it, and the only properties any check here depends on are that it decodes,
 * that it is square, and that its ink reaches its own edges.
 */
QString testLogoPath();

/*
 * A two-frame animated GIF, written into the same temporary directory as the still.
 *
 * Generated rather than committed for the reasons the still is, and by hand rather than through
 * Qt because Qt reads GIFs and does not write them. It is 16x16 and its two frames are two flat
 * colours, which is all any check here needs: the frame count, the frame timing, and the fact
 * that the two differ.
 */
QString testAnimatedLogoPath();

/* Frames in the generated animation, and how long each is held. */
constexpr int kTestAnimationFrames = 2;
constexpr int kTestAnimationFrameMs = 100;

/*
 * A file called `logo.mp4`, in the same temporary directory as the rest.
 *
 * Its contents are never read and are not a video: logos are pictures here, so what the checks
 * care about is that a file *named* like a video is turned away with an explanation rather than
 * decoded, offered playback settings, or drawn.
 */
QString videoLogoPath();

/* Points a section's artwork -- its own and its entries' -- at the animated logo. */
Section &withAnimatedLogo(Section &section, int maxHeight = 110);

/*
 * A path no file will ever be at. Every renderer path that takes artwork has to survive one, and
 * naming it is clearer at the call site than another string literal that looks like a real path.
 */
QString missingLogoPath();

/* --- scenes ------------------------------------------------------------------------------ */

/*
 * A named document, rendered to a PNG by `--artifacts` and available to any suite that wants to
 * assert against it.
 *
 * Scenes are the cheap way to look at a graphical change: add one here, run with `--artifacts`,
 * and there is a picture of it next to every other. Keep them small and legible -- a scene that
 * shows one thing is worth more than one that shows nine.
 */
struct Scene {
	QString name;
	QString description;
	Document document;
};

const QVector<Scene> &scenes();

} // namespace closingtime::test
