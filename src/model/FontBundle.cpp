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

#include "model/FontBundle.hpp"

#include <obs.hpp>

#include "util/FontFiles.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace closingtime {

QString BundledFont::hash() const
{
	if (cachedHash.isEmpty() && !data.isEmpty())
		cachedHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

	return cachedHash;
}

bool BundledFont::supplies(const QString &wantedFamily, const QString &styleName) const
{
	if (family.compare(wantedFamily, Qt::CaseInsensitive) != 0)
		return false;

	/*
	 * A bundle written before faces were recorded says nothing about which of them it holds, so
	 * it answers for the whole family rather than for none of it -- the behavior every such
	 * bundle already has.
	 */
	if (styleNames.isEmpty())
		return true;

	for (const QString &declared : styleNames) {
		if (declared.compare(styleName, Qt::CaseInsensitive) == 0)
			return true;
	}

	return false;
}

void BundledFont::save(obs_data_t *data) const
{
	obs_data_set_string(data, "family", family.toUtf8().constData());
	obs_data_set_string(data, "file", fileName.toUtf8().constData());

	OBSDataArrayAutoRelease faces = obs_data_array_create();
	for (const QString &styleName : styleNames) {
		OBSDataAutoRelease face = obs_data_create();
		obs_data_set_string(face, "style_name", styleName.toUtf8().constData());
		obs_data_array_push_back(faces, face);
	}
	obs_data_set_array(data, "faces", faces);

	/*
	 * obs_data holds text, not bytes, so the file goes in base64 -- a third larger than the
	 * font, and the only shape a scene collection's JSON can carry it in at all.
	 */
	obs_data_set_string(data, "data", this->data.toBase64().constData());
}

void BundledFont::load(obs_data_t *data)
{
	family = QString::fromUtf8(obs_data_get_string(data, "family"));
	fileName = QString::fromUtf8(obs_data_get_string(data, "file"));

	styleNames.clear();
	OBSDataArrayAutoRelease faces = obs_data_get_array(data, "faces");
	const size_t faceCount = obs_data_array_count(faces);
	for (size_t i = 0; i < faceCount; ++i) {
		OBSDataAutoRelease face = obs_data_array_item(faces, i);
		styleNames.append(QString::fromUtf8(obs_data_get_string(face, "style_name")));
	}

	const QByteArray encoded(obs_data_get_string(data, "data"));
	/*
	 * A hand-edited or truncated collection is a broken bundle rather than a broken document:
	 * the entry loads empty, nothing is registered from it, and the family reports as missing
	 * exactly as it would if it had never been bundled.
	 */
	const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
	this->data = decoded ? *decoded : QByteArray();
	cachedHash.clear();
}

void FontSubstitution::save(obs_data_t *data) const
{
	obs_data_set_string(data, "from", from.toUtf8().constData());
	obs_data_set_string(data, "to", to.toUtf8().constData());
}

void FontSubstitution::load(obs_data_t *data)
{
	from = QString::fromUtf8(obs_data_get_string(data, "from"));
	to = QString::fromUtf8(obs_data_get_string(data, "to"));
}

QVector<BundledFont> collectBundledFonts(const QVector<FontUse> &fonts, QStringList *skipped)
{
	QVector<BundledFont> bundle;
	qint64 total = 0;

	for (const FontUse &use : fonts) {
		bool carried = false;
		bool found = false;

		/*
		 * Each candidate's own faces, read before anything is carried, so the files the roll is
		 * actually set in can be taken first. The `name` table is a few kilobytes at the front
		 * of the file; reading it is nothing beside reading the file itself, which is what
		 * happens to the ones that are kept.
		 */
		struct Candidate {
			QString path;
			QStringList styleNames;
			bool wanted = false;
		};

		QVector<Candidate> candidates;

		for (const QString &path : fontFilesForFamily(use.family)) {
			found = true;

			Candidate candidate;
			candidate.path = path;

			for (const FontFace &face : fontFacesInFile(path)) {
				if (face.family.compare(use.family, Qt::CaseInsensitive) != 0)
					continue;

				candidate.styleNames.append(face.styleName);

				for (const QString &styleName : use.styleNames) {
					if (face.styleName.compare(styleName, Qt::CaseInsensitive) == 0)
						candidate.wanted = true;
				}
			}

			candidates.append(candidate);
		}

		/*
		 * Stable, so the walk's order survives among the files that are equally wanted and a
		 * family small enough to carry whole comes out exactly as it did before.
		 */
		std::stable_partition(candidates.begin(), candidates.end(),
				      [](const Candidate &candidate) { return candidate.wanted; });

		for (const Candidate &candidate : candidates) {
			QFileInfo info(candidate.path);
			if (info.size() > kMaxBundledFontBytes || total + info.size() > kMaxFontBundleBytes)
				continue;

			QFile file(candidate.path);
			if (!file.open(QIODevice::ReadOnly))
				continue;

			BundledFont font;
			font.family = use.family;
			font.styleNames = candidate.styleNames;
			font.fileName = info.fileName();
			font.data = file.readAll();
			if (font.isEmpty())
				continue;

			total += font.data.size();
			bundle.append(font);
			carried = true;
		}

		/*
		 * Only a family whose files were all found and all refused counts as skipped. One
		 * with no file at all is a family this machine does not have either, which the
		 * missing-font report already covers and which nothing here could have fixed.
		 */
		if (found && !carried && skipped)
			skipped->append(use.family);
	}

	return bundle;
}

qint64 fontBundleBytes(const QVector<BundledFont> &fonts)
{
	qint64 total = 0;
	for (const BundledFont &font : fonts)
		total += font.data.size();

	return total;
}

} // namespace closingtime
