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

#include "model/TagGlyph.hpp"

#include <cstring>
#include <iterator>

namespace closingtime {

namespace {

struct TagGlyphInfo {
	TagGlyph glyph;
	/* Persisted id, and the key the renderer's tile cache is keyed on. */
	const char *id;
	/* Untranslated display name, used as the fallback when no locale string exists. */
	const char *name;
	/*
	 * The shapes the mark is made of, drawn in a box one unit tall and `aspect` units wide, in
	 * white so the renderer can use them as a stencil for the ink of the text beside them.
	 *
	 * Simplified silhouettes rather than anyone's logo artwork: the job of a mark on a schedule
	 * row is to tell that row apart from the one under it at a glance, and a recognizable shape
	 * does that without carrying a trademark into the plugin's own source. A production that
	 * wants the real mark points a Custom glyph at the file it has been given a license for.
	 */
	const char *svg;
	double aspect;
};

/*
 * The built-in marks, in the order the designer lists them.
 *
 * Adding one is a row here and nothing else: the renderer measures whatever the table hands it,
 * scales it to the line's own height and paints it through the line's ink.
 */
const TagGlyphInfo kTagGlyphs[] = {
	{TagGlyph::None, "none", "None", "", 0.0},

	/* A speech-bubble screen with two aerials: the shape a stream tag has carried for a decade. */
	{TagGlyph::Twitch, "twitch", "Stream",
	 R"(<path d="M0.12 0.06 L0.88 0.06 L0.88 0.62 L0.66 0.84 L0.5 0.84 L0.34 1 L0.34 0.84 L0.12 0.84 Z" fill="#ffffff"/>)"
	 R"(<rect x="0.38" y="0.26" width="0.09" height="0.32" fill="#000000"/>)"
	 R"(<rect x="0.58" y="0.26" width="0.09" height="0.32" fill="#000000"/>)",
	 1.0},

	/* A rounded screen with a play triangle knocked out of it. */
	{TagGlyph::YouTube, "youtube", "Video",
	 R"(<rect x="0" y="0.12" width="1.4" height="0.76" rx="0.22" fill="#ffffff"/>)"
	 R"(<path d="M0.56 0.32 L0.92 0.5 L0.56 0.68 Z" fill="#000000"/>)",
	 1.4},

	/* A blocky K, for the platforms whose mark is a letterform. */
	{TagGlyph::Kick, "kick", "Channel",
	 R"(<rect x="0.08" y="0.06" width="0.22" height="0.88" fill="#ffffff"/>)"
	 R"(<path d="M0.36 0.5 L0.72 0.06 L1.02 0.06 L0.66 0.5 L1.02 0.94 L0.72 0.94 Z" fill="#ffffff"/>)",
	 1.1},

	/* A globe: a circle with a meridian and a parallel across it. */
	{TagGlyph::Website, "website", "Website",
	 R"(<circle cx="0.5" cy="0.5" r="0.44" fill="none" stroke="#ffffff" stroke-width="0.1"/>)"
	 R"(<ellipse cx="0.5" cy="0.5" rx="0.2" ry="0.44" fill="none" stroke="#ffffff" stroke-width="0.08"/>)"
	 R"(<rect x="0.06" y="0.45" width="0.88" height="0.1" fill="#ffffff"/>)",
	 1.0},

	/* A map pin, for the tag that names a room rather than a channel. */
	{TagGlyph::Location, "location", "Location",
	 R"(<path d="M0.5 0.02 C0.24 0.02 0.08 0.22 0.08 0.44 C0.08 0.72 0.5 1 0.5 1 )"
	 R"(C0.5 1 0.92 0.72 0.92 0.44 C0.92 0.22 0.76 0.02 0.5 0.02 Z" fill="#ffffff"/>)"
	 R"(<circle cx="0.5" cy="0.42" r="0.15" fill="#000000"/>)",
	 1.0},

	/* Art from a file, measured from its own viewBox rather than from `aspect`. */
	{TagGlyph::Custom, "custom", "Custom Image", "", 1.0},
};

const TagGlyphInfo &tagGlyphInfo(TagGlyph glyph)
{
	for (const auto &info : kTagGlyphs) {
		if (info.glyph == glyph)
			return info;
	}
	return kTagGlyphs[0];
}

} // namespace

const char *tagGlyphId(TagGlyph glyph)
{
	return tagGlyphInfo(glyph).id;
}

TagGlyph tagGlyphFromId(const char *id, TagGlyph fallback)
{
	if (!id)
		return fallback;

	for (const auto &info : kTagGlyphs) {
		if (strcmp(info.id, id) == 0)
			return info.glyph;
	}
	return fallback;
}

const char *tagGlyphName(TagGlyph glyph)
{
	return tagGlyphInfo(glyph).name;
}

const QVector<TagGlyph> &allTagGlyphs()
{
	static const QVector<TagGlyph> glyphs = [] {
		QVector<TagGlyph> result;
		result.reserve(static_cast<int>(std::size(kTagGlyphs)));
		for (const auto &info : kTagGlyphs)
			result.append(info.glyph);
		return result;
	}();
	return glyphs;
}

QString tagGlyphSvg(TagGlyph glyph)
{
	const TagGlyphInfo &info = tagGlyphInfo(glyph);
	if (!info.svg || info.svg[0] == '\0' || info.aspect <= 0.0)
		return QString();

	return QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %1 1\">%2</svg>")
		.arg(QString::number(info.aspect, 'g', 6), QString::fromLatin1(info.svg));
}

double tagGlyphAspect(TagGlyph glyph)
{
	const double aspect = tagGlyphInfo(glyph).aspect;
	return aspect > 0.0 ? aspect : 1.0;
}

} // namespace closingtime
