#include "engine/DeepDvScanWorker.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QUuid>

namespace Mc {

namespace {

// Polls waitForFinished() in short slices so a cancel() mid-run can kill the
// process promptly instead of blocking until it finishes on its own — matters
// here since mkvextract/dovi_tool can run for minutes on a large 4K track.
// Returns false on cancellation, on a failure to even launch (missing binary),
// or on a non-zero exit code.
bool runCancelable(QProcess& proc, const QAtomicInt& cancelled)
{
	if (!proc.waitForStarted(5000))
		return false;
	while (proc.state() != QProcess::NotRunning) {
		if (!proc.waitForFinished(200) && cancelled.loadRelaxed() != 0) {
			proc.kill();
			proc.waitForFinished(2000);
			return false;
		}
	}
	return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
}

} // namespace

DeepDvScanWorker::DeepDvScanWorker(qint64 fileId, QString filePath, int streamIndex, int videoOrdinal,
                                   QString mkvmergePath, QString mkvextractPath, QString doviToolPath,
                                   QObject* parent)
	: QObject(parent)
	, m_fileId(fileId)
	, m_filePath(std::move(filePath))
	, m_streamIndex(streamIndex)
	, m_videoOrdinal(videoOrdinal)
	, m_mkvmergePath(std::move(mkvmergePath))
	, m_mkvextractPath(std::move(mkvextractPath))
	, m_doviToolPath(std::move(doviToolPath))
{}

void DeepDvScanWorker::run()
{
	QString elType, error;
	scanOne(elType, error);
	emit finished(m_fileId, m_streamIndex, elType, error);
}

bool DeepDvScanWorker::resolveMkvExtractTrackId(int& outTrackId, QString& outError)
{
	QProcess identify;
	identify.start(m_mkvmergePath, {"-J", m_filePath});
	if (!runCancelable(identify, m_cancelled)) {
		outError = tr("mkvmerge -J failed to identify tracks");
		return false;
	}

	const QJsonDocument doc = QJsonDocument::fromJson(identify.readAllStandardOutput());
	const QJsonArray tracks = doc.object().value("tracks").toArray();

	int seen = 0;
	for (const QJsonValue& t : tracks) {
		const QJsonObject to = t.toObject();
		if (to.value("type").toString() != QLatin1String("video"))
			continue;
		if (seen == m_videoOrdinal) {
			outTrackId = to.value("id").toInt(-1);
			return outTrackId >= 0;
		}
		++seen;
	}
	outError = tr("Could not find video track #%1 in mkvmerge's track list").arg(m_videoOrdinal);
	return false;
}

bool DeepDvScanWorker::scanOne(QString& outElType, QString& outError)
{
	if (m_mkvmergePath.isEmpty() || m_mkvextractPath.isEmpty() || m_doviToolPath.isEmpty()) {
		outError = tr("mkvmerge, mkvextract, or dovi_tool could not be located");
		return false;
	}

	int trackId = -1;
	if (!resolveMkvExtractTrackId(trackId, outError))
		return false;

	if (m_cancelled.loadRelaxed() != 0) { outError = tr("Cancelled"); return false; }

	const QString workDir = QDir::temp().filePath(
		"mediacurator_dvscan_" + QUuid::createUuid().toString(QUuid::Id128));
	if (!QDir().mkpath(workDir)) {
		outError = tr("Could not create a temp working folder");
		return false;
	}
	const QString extractedPath = workDir + "/track.hevc";
	const QString rpuPath       = workDir + "/rpu.bin";

	bool ok = true;
	{
		QProcess extract;
		extract.start(m_mkvextractPath, {"tracks", m_filePath,
			QString("%1:%2").arg(trackId).arg(extractedPath)});
		ok = runCancelable(extract, m_cancelled);
		if (!ok) outError = tr("mkvextract failed to extract the video track");
	}

	if (ok && m_cancelled.loadRelaxed() != 0) { ok = false; outError = tr("Cancelled"); }

	if (ok) {
		QProcess extractRpu;
		extractRpu.start(m_doviToolPath, {"extract-rpu", extractedPath, "-o", rpuPath});
		ok = runCancelable(extractRpu, m_cancelled);
		if (!ok) outError = tr("dovi_tool extract-rpu failed");
	}

	if (ok) {
		// dovi_tool's own RPU parser computes el_type per frame from the NLQ
		// mapping data (dolby_vision::rpu::rpu_data_nlq::RpuDataNlq::el_type(),
		// confirmed by reading the source — MEL is a real zero-offset check on
		// the RPU, not a size/bitrate guess) and its `info --summary` prints
		// "Profile: 7 (FEL)" / "Profile: 7 (MEL)" (dovi_tool src/dovi/rpu_info.rs).
		// A file with per-frame variance would print both, joined by ", ".
		QProcess info;
		info.start(m_doviToolPath, {"info", "-i", rpuPath, "--summary"});
		ok = runCancelable(info, m_cancelled);
		const QString out = QString::fromUtf8(info.readAllStandardOutput());
		if (!ok) {
			outError = tr("dovi_tool info failed");
		} else {
			const bool hasFel = out.contains(QLatin1String("FEL"));
			const bool hasMel = out.contains(QLatin1String("MEL"));
			if (hasFel && !hasMel) outElType = QStringLiteral("FEL");
			else if (hasMel && !hasFel) outElType = QStringLiteral("MEL");
			else if (hasFel && hasMel) outElType = QStringLiteral("FEL"); // mixed across frames — report the more demanding one
			else { ok = false; outError = tr("Could not find FEL/MEL in dovi_tool's summary output"); }
		}
	}

	QDir(workDir).removeRecursively();
	return ok;
}

} // namespace Mc
