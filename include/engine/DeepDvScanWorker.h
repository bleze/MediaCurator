#pragma once

#include <QAtomicInt>
#include <QObject>
#include <QString>

namespace Mc {

// Extracts the elementary video stream of a dual-layer Dolby Vision track (e.g.
// Profile 7) and analyzes it with dovi_tool to tell a Full (FEL) from a Minimal
// (MEL) Enhancement Layer — a distinction ffprobe's metadata never exposes at
// all (see DolbyVisionInfo/FfprobeScanner). Triggered explicitly via the "Deep
// Scan for FEL/MEL…" context menu action, never during a routine scan. Runs on
// its own QThread, same start/cancel/finished pattern as AnalyzeWorker.
class DeepDvScanWorker : public QObject
{
	Q_OBJECT
public:
	// videoOrdinal: this track's rank (0-based) among the file's video tracks in
	// ffprobe's stream order — used to find the matching track id in mkvmerge's
	// own (separate) numbering, since mkvextract addresses tracks by that id.
	explicit DeepDvScanWorker(qint64 fileId, QString filePath, int streamIndex, int videoOrdinal,
	                           QString mkvmergePath, QString mkvextractPath, QString doviToolPath,
	                           QObject* parent = nullptr);

	void cancel() { m_cancelled.storeRelaxed(1); }

public slots:
	void run();

signals:
	// elType is "FEL" or "MEL" on success, empty on failure (see errorMessage).
	void finished(qint64 fileId, int streamIndex, const QString& elType, const QString& errorMessage);

private:
	bool scanOne(QString& outElType, QString& outError);
	bool resolveMkvExtractTrackId(int& outTrackId, QString& outError);

	qint64     m_fileId;
	QString    m_filePath;
	int        m_streamIndex;
	int        m_videoOrdinal;
	QString    m_mkvmergePath;
	QString    m_mkvextractPath;
	QString    m_doviToolPath;
	QAtomicInt m_cancelled{0};
};

} // namespace Mc
