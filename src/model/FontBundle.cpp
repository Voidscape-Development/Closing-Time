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
#include <plugin-support.h>

#include "util/FontFiles.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHash>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

namespace closingtime {

namespace {

/*
 * Every bundled font this process has decoded, by SHA-256, with the compressed text it is written
 * as.
 *
 * A source's settings are loaded on every update(), and libobs sends one per frame of a slider
 * drag -- on the thread compositing the program. Decoding a font is base64 and zlib over
 * megabytes, which for a roll carrying a couple of them was most of a hundred milliseconds a frame.
 * The settings say which font they hold by its hash, so a font already decoded is handed back as
 * the same bytes, and two rolls carrying the same font share one copy.
 *
 * Fonts nobody holds any more are let go whenever a new one arrives.
 */
struct StoredFont {
	QByteArray data;
	/* What `save` writes, made once: compressing a font is not free either. */
	QByteArray encoded;
};

struct FontStore {
	std::mutex mutex;
	QHash<QString, StoredFont> fonts;
	std::atomic<int> decodes{0};
};

FontStore &fontStore()
{
	static FontStore store;
	return store;
}

QString sha256(const QByteArray &bytes)
{
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/* Remembers a decoded font, letting go of any the store alone is still holding. Callers hold the mutex. */
void keepLocked(FontStore &store, const QString &hash, const StoredFont &font)
{
	for (auto it = store.fonts.begin(); it != store.fonts.end();) {
		/* Detached means the store's is the only copy left: no document carries it. */
		if (it.key() != hash && it->data.isDetached())
			it = store.fonts.erase(it);
		else
			++it;
	}
	store.fonts.insert(hash, font);
}

} // namespace

int fontStoreSize()
{
	FontStore &store = fontStore();
	std::lock_guard<std::mutex> lock(store.mutex);
	return static_cast<int>(store.fonts.size());
}

int fontStoreDecodes()
{
	return fontStore().decodes.load();
}

bool BundledFont::operator==(const BundledFont &other) const
{
	if (family != other.family || styleNames != other.styleNames || fileName != other.fileName)
		return false;
	if (data.constData() == other.data.constData() && data.size() == other.data.size())
		return true;
	return data == other.data;
}

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
	 * obs_data holds text, not bytes, so the file goes in base64 -- the only shape a scene
	 * collection's JSON can carry it in at all -- compressed first, which more than pays back the
	 * third base64 adds for the outline data a font is mostly made of. Named by its hash, so a
	 * reader that already holds the font never decodes it.
	 */
	const QString name = hash();
	QByteArray encoded;
	{
		FontStore &store = fontStore();
		std::lock_guard<std::mutex> lock(store.mutex);
		const auto it = store.fonts.constFind(name);
		if (it != store.fonts.constEnd() && !it->encoded.isEmpty()) {
			encoded = it->encoded;
		} else {
			encoded = qCompress(this->data, 9).toBase64();
			keepLocked(store, name, StoredFont{this->data, encoded});
		}
	}

	obs_data_set_string(data, "encoding", "zlib");
	obs_data_set_string(data, "sha256", name.toLatin1().constData());
	obs_data_set_string(data, "data", encoded.constData());
}

void BundledFont::load(obs_data_t *data, bool *legacy)
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

	cachedHash.clear();
	this->data.clear();

	const QString name = QString::fromLatin1(obs_data_get_string(data, "sha256"));
	const bool compressed = std::strcmp(obs_data_get_string(data, "encoding"), "zlib") == 0;
	if (legacy && !compressed)
		*legacy = true;

	FontStore &store = fontStore();

	/* The common case, every frame of a drag: the font is already decoded, and is handed straight back. */
	if (!name.isEmpty()) {
		std::lock_guard<std::mutex> lock(store.mutex);
		const auto it = store.fonts.constFind(name);
		if (it != store.fonts.constEnd()) {
			this->data = it->data;
			cachedHash = name;
			return;
		}
	}

	/*
	 * A hand-edited or truncated collection is a broken bundle rather than a broken document:
	 * the entry loads empty, nothing is registered from it, and the family reports as missing
	 * exactly as it would if it had never been bundled.
	 */
	const QByteArray encoded(obs_data_get_string(data, "data"));
	const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded)
		return;

	QByteArray bytes = compressed ? qUncompress(*decoded) : *decoded;
	if (bytes.isEmpty())
		return;

	/* A hash that does not match what it names is a damaged entry; the bytes are what is trusted. */
	const QString actual = sha256(bytes);
	if (!name.isEmpty() && actual != name)
		obs_log(LOG_WARNING, "bundled font '%s' did not match the hash it was saved with",
			fileName.toUtf8().constData());

	++store.decodes;
	this->data = bytes;
	cachedHash = actual;

	std::lock_guard<std::mutex> lock(store.mutex);
	keepLocked(store, actual, StoredFont{bytes, compressed ? encoded : QByteArray()});
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
