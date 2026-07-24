#include "engine/TrackFlagService.h"

#include "core/DatabaseManager.h"
#include "core/DriveActivityMonitor.h"
#include "core/ExternalTools.h"
#include "engine/ActionEngine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>

namespace Mc {

TrackFlagService& TrackFlagService::instance()
{
	static TrackFlagService s;
	return s;
}

void TrackFlagService::apply(qint64 fileId, int streamIndex, const QString& flag,
                              const QVariant& value, std::function<void(bool ok)> onDone)
{
	m_pending[fileId].append(PendingChange{ streamIndex, flag, value, std::move(onDone) });
	if (!m_running.contains(fileId))
		runNext(fileId);
}

void TrackFlagService::runNext(qint64 fileId)
{
	if (m_running.contains(fileId))
		return;

	QList<PendingChange> batch = m_pending.take(fileId);
	if (batch.isEmpty())
		return;

	auto& db = DatabaseManager::instance();

	// Never touch a file that's mid-remux.
	if (const auto activeJob = db.activeJobForFile(fileId)) {
		if (activeJob->status == QLatin1String("running")) {
			for (const PendingChange& c : batch)
				if (c.onDone) c.onDone(false);
			return;
		}
	}

	const auto fileOpt = db.fileById(fileId);
	if (!fileOpt) {
		for (const PendingChange& c : batch)
			if (c.onDone) c.onDone(false);
		return;
	}

	const QList<StreamRecord> streams = db.streamsForFile(fileId);
	QHash<int, const StreamRecord*> streamByIndex;
	for (const StreamRecord& s : streams) streamByIndex.insert(s.streamIndex, &s);

	// Original-flag exclusivity: only one audio track may carry FlagOriginal.
	// If this batch sets a new track as original, clear it from whichever
	// other audio track currently has it — synthesized, no caller callback.
	for (const PendingChange& c : batch) {
		if (c.flag != QLatin1String("original") || !c.value.toBool()) continue;
		for (const StreamRecord& s : streams) {
			if (s.codecType == QLatin1String("audio") && s.isOriginal && s.streamIndex != c.streamIndex) {
				batch.append(PendingChange{ s.streamIndex, QStringLiteral("original"), false, {} });
			}
		}
		break; // at most one "set original" per batch is expected
	}

	// A sidecar subtitle isn't a track inside the container yet — there's nothing for
	// mkvpropedit to edit. Its default/forced state lives only in the DB until it's
	// actually muxed in (ActionEngine::buildSidecarArgsForRemux reads it from there).
	QList<PendingChange> internalBatch, externalBatch;
	for (const PendingChange& c : batch) {
		const StreamRecord* s = streamByIndex.value(c.streamIndex, nullptr);
		if (s && s->isExternal) externalBatch.append(c);
		else                    internalBatch.append(c);
	}

	for (const PendingChange& c : externalBatch) {
		const bool ok = (c.flag != QLatin1String("language"))
		    && db.updateStreamFlag(fileId, c.streamIndex, c.flag, c.value.toBool());
		if (c.onDone) c.onDone(ok);
	}

	if (internalBatch.isEmpty()) {
		runNext(fileId);
		return;
	}

	QJsonArray changes;
	for (const PendingChange& c : internalBatch) {
		QJsonObject o;
		o["streamIndex"] = c.streamIndex;
		o["flag"]        = c.flag;
		o["value"]       = QJsonValue::fromVariant(c.value);
		changes.append(o);
	}
	const QString json = QString::fromUtf8(QJsonDocument(changes).toJson(QJsonDocument::Compact));

	const QStringList args        = ActionEngine::buildPropEditArgs(fileOpt->path, json);
	const QString     mkvpropedit = ExternalTools::instance().mkvpropeditPath();

	DriveActivityMonitor::touchPath(fileOpt->path);
	auto* proc = new QProcess(this);
	m_running.insert(fileId, proc);
	proc->setProcessChannelMode(QProcess::MergedChannels);
	connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
	        this, [this, proc, fileId, internalBatch](int exitCode, QProcess::ExitStatus status) {
		const bool ok = (status == QProcess::NormalExit && exitCode == 0);
		proc->deleteLater();
		m_running.remove(fileId);

		if (ok) {
			auto& db = DatabaseManager::instance();
			for (const PendingChange& c : internalBatch) {
				if (c.flag == QLatin1String("language"))
					db.updateStreamLanguageInternal(fileId, c.streamIndex, c.value.toString());
				else
					db.updateStreamFlag(fileId, c.streamIndex, c.flag, c.value.toBool());
			}
		}
		for (const PendingChange& c : internalBatch)
			if (c.onDone) c.onDone(ok);

		runNext(fileId);
	});
	proc->start(mkvpropedit, args);
}

} // namespace Mc
