#include "scanner/NfoParser.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#ifdef Q_OS_WIN
#include <windows.h>
static FILETIME toFileTime(const QDateTime& dt)
{
	const qint64 ns100 = (dt.toMSecsSinceEpoch() + Q_INT64_C(11644473600000)) * 10000;
	FILETIME ft;
	ft.dwLowDateTime  = static_cast<DWORD>(ns100 & 0xFFFFFFFF);
	ft.dwHighDateTime = static_cast<DWORD>((ns100 >> 32) & 0xFFFFFFFF);
	return ft;
}
// Restore the folder's creation/modified timestamps after writing an .nfo file into
// it, so saving metadata doesn't bubble the folder up as "newest" in media libraries
// that sort by Date Modified. Mirrors preserveDirTimestamps() in OpenSubtitlesClient.cpp.
static void preserveDirTimestamps(const QString& dirPath, const QDateTime& origCreated, const QDateTime& origModified)
{
	HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(dirPath.utf16()),
	                       FILE_WRITE_ATTRIBUTES,
	                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                       nullptr, OPEN_EXISTING,
	                       FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	FILETIME ftCreated  = origCreated.isValid()  ? toFileTime(origCreated)  : FILETIME{};
	FILETIME ftModified = origModified.isValid() ? toFileTime(origModified) : FILETIME{};
	SetFileTime(h,
	            origCreated.isValid()  ? &ftCreated  : nullptr,
	            nullptr,
	            origModified.isValid() ? &ftModified : nullptr);
	CloseHandle(h);
}
#endif

namespace Mc {

QString NfoParser::nfoPathFor(const QString& videoPath)
{
	const QFileInfo fi(videoPath);
	return fi.dir().filePath(fi.completeBaseName() + ".nfo");
}

QString NfoParser::readImdbId(const QString& videoPath)
{
	static const QRegularExpression re(R"(\btt\d{7,8}\b)");

	auto scanFile = [&](const QString& path) -> QString {
		QFile f(path);
		if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
		const QString content = QString::fromUtf8(f.readAll());
		const auto m = re.match(content);
		return m.hasMatch() ? m.captured(0) : QString{};
	};

	const QString id = scanFile(nfoPathFor(videoPath));
	if (!id.isEmpty()) return id;

	const QDir dir(QFileInfo(videoPath).absolutePath());
	for (const QString& name : dir.entryList({"*.nfo"}, QDir::Files)) {
		const QString found = scanFile(dir.filePath(name));
		if (!found.isEmpty()) return found;
	}
	return {};
}

QSet<QString>& NfoParser::ownWrites()
{
	static QSet<QString> s;
	return s;
}

bool NfoParser::checkAndClearOwnWrite(const QString& nfoPath)
{
	return ownWrites().remove(nfoPath);
}

bool NfoParser::writeMovieNfo(const QString& videoPath,
							   const QString& imdbId,
							   const QString& title,
							   int year)
{
	const QString nfoPath = nfoPathFor(videoPath);
	// Register before the write so the watcher callback (if any) can skip it.
	ownWrites().insert(nfoPath);
	QFile file(nfoPath);

	// Capture the folder's timestamps before writing the .nfo file into it.
	const QFileInfo dirFi(QFileInfo(nfoPath).absolutePath());
	const QDateTime dirOrigCreated  = dirFi.birthTime();
	const QDateTime dirOrigModified = dirFi.lastModified();
	const QString   dirPath         = dirFi.absoluteFilePath();

	const QString uniqueIdTag =
		QStringLiteral("<uniqueid type=\"imdb\" default=\"true\">%1</uniqueid>").arg(imdbId);
	const QString idTag = QStringLiteral("<id>%1</id>").arg(imdbId);

	if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QString content = QString::fromUtf8(file.readAll());
		file.close();

		if (content.contains("<movie", Qt::CaseInsensitive)) {
			// Drop the old, non-standard <imdbid> tag written by earlier versions.
			static const QRegularExpression legacyImdbTagRe(
				R"(\s*<imdbid>.*?</imdbid>)", QRegularExpression::CaseInsensitiveOption);
			content.remove(legacyImdbTagRe);

			static const QRegularExpression uniqueIdTagRe(
				R"(<uniqueid\b[^>]*>.*?</uniqueid>)", QRegularExpression::CaseInsensitiveOption);
			static const QRegularExpression idTagRe(
				R"(<id>.*?</id>)", QRegularExpression::CaseInsensitiveOption);

			auto upsertTag = [&](const QRegularExpression& tagRe, const QString& newTag) {
				if (content.contains(tagRe)) {
					content.replace(tagRe, newTag);
				} else {
					const int closeIdx = content.lastIndexOf("</movie>", -1, Qt::CaseInsensitive);
					if (closeIdx >= 0)
						content.insert(closeIdx, "  " + newTag + "\n");
					else
						content += newTag + "\n";
				}
			};

			upsertTag(uniqueIdTagRe, uniqueIdTag);
			upsertTag(idTagRe, idTag);

			if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
				file.write(content.toUtf8());
#ifdef Q_OS_WIN
				preserveDirTimestamps(dirPath, dirOrigCreated, dirOrigModified);
#endif
				return true;
			}
			return false;
		}
	}

	// Create a fresh minimal Kodi-format NFO
	QString xml = QStringLiteral(
		"<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\n"
		"<movie>\n");
	if (!title.isEmpty())
		xml += QStringLiteral("  <title>%1</title>\n").arg(title.toHtmlEscaped());
	if (year > 0)
		xml += QStringLiteral("  <year>%1</year>\n").arg(year);
	xml += "  " + uniqueIdTag + "\n";
	xml += "  " + idTag + "\n";
	xml += QStringLiteral("</movie>\n");

	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return false;
	file.write(xml.toUtf8());
#ifdef Q_OS_WIN
	preserveDirTimestamps(dirPath, dirOrigCreated, dirOrigModified);
#endif
	return true;
}

QString NfoParser::titleFromFilename(const QString& filename)
{
	QString name = QFileInfo(filename).completeBaseName();

	name.replace('.', ' ');
	name.replace('_', ' ');

	// Keep everything up to and including the first release-year
	static const QRegularExpression yearRe(R"(\b(19|20)\d{2}\b)");
	const auto match = yearRe.match(name);
	if (match.hasMatch())
		name = name.left(match.capturedStart() + match.capturedLength());

	return name.simplified();
}

} // namespace Mc
