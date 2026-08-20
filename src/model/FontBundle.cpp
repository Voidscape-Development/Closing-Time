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

#include "util/FontFiles.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

namespace closingtime {

QString BundledFont::hash() const
{
	if (cachedHash.isEmpty() && !data.isEmpty())
		cachedHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());

	return cachedHash;
}

void BundledFont::save(obs_data_t *data) const
{
	obs_data_set_string(data, "family", family.toUtf8().constData());
	obs_data_set_string(data, "file", fileName.toUtf8().constData());
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

QVector<BundledFont> collectBundledFonts(const QStringList &families, QStringList *skipped)
{
	QVector<BundledFont> bundle;
	qint64 total = 0;

	for (const QString &family : families) {
		bool carried = false;
		bool found = false;

		for (const QString &path : fontFilesForFamily(family)) {
			found = true;

			QFileInfo info(path);
			if (info.size() > kMaxBundledFontBytes || total + info.size() > kMaxFontBundleBytes)
				continue;

			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
				continue;

			BundledFont font;
			font.family = family;
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
			skipped->append(family);
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
