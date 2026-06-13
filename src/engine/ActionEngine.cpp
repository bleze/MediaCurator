#include "engine/ActionEngine.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace Mc {

// Maps our flag names to mkvmerge's --XXX-flag argument names
static QString mkvmergeFlagArg(const QString& flag)
{
	if (flag == QLatin1String("default"))     return QStringLiteral("--default-track-flag");
	if (flag == QLatin1String("forced"))      return QStringLiteral("--forced-display-flag");
	if (flag == QLatin1String("original"))    return QStringLiteral("--original-flag");
	return {};
}

// Maps our flag names to mkvpropedit's property names
static QString mkvpropEditProp(const QString& flag)
{
	if (flag == QLatin1String("default"))     return QStringLiteral("flag-default");
	if (flag == QLatin1String("forced"))      return QStringLiteral("flag-forced");
	if (flag == QLatin1String("original"))    return QStringLiteral("flag-original");
	return {};
}

ActionEngine::ActionEngine(const QString& mkvmergePath, QObject* parent)
	: QObject(parent), m_mkvmergePath(mkvmergePath)
{}

QStringList ActionEngine::buildCommand(const FileDecision& decision, const QString& outputPath) const
{
	QStringList keepAudio, keepSub, keepVideo;
	bool hasAudio = false, hasSub = false;
	bool anyVideoRemoved = false;

	for (const TrackDecision& td : decision.tracks) {
		if (td.stream.isExternal) continue;  // sidecar file — not in container, handled separately
		if (td.stream.codecType == "audio")    hasAudio = true;
		if (td.stream.codecType == "subtitle") hasSub   = true;
		if (td.stream.codecType == "video" && td.decision == Decision::Remove)
			anyVideoRemoved = true;
		if (td.decision == Decision::Remove) continue;
		if (td.stream.codecType == "audio")    keepAudio << QString::number(td.stream.streamIndex);
		if (td.stream.codecType == "subtitle") keepSub   << QString::number(td.stream.streamIndex);
		if (td.stream.codecType == "video")    keepVideo << QString::number(td.stream.streamIndex);
	}

	QStringList args = { "-o", outputPath };

	// Only emit --video-tracks when at least one video stream is being removed,
	// so existing jobs without the flag keep backward-compatible "keep all" semantics.
	if (anyVideoRemoved) {
		if (!keepVideo.isEmpty())
			args << "--video-tracks" << keepVideo.join(',');
		else
			args << "--no-video";
	}

	if (hasAudio) {
		if (!keepAudio.isEmpty())
			args << "--audio-tracks" << keepAudio.join(',');
		else
			args << "--no-audio";
	}

	if (hasSub) {
		if (!keepSub.isEmpty())
			args << "--subtitle-tracks" << keepSub.join(',');
		else
			args << "--no-subtitles";
	}

	// ISO files need a protocol prefix for mkvmerge (different from ffprobe's prefix)
	const QString& cont = decision.file.container;
	QString mkvInput = decision.file.path;
	if (cont == QLatin1String("iso-bluray"))
		mkvInput = QStringLiteral("bluray://") + decision.file.path;
	else if (cont == QLatin1String("iso-dvd"))
		mkvInput = QStringLiteral("dvd://") + decision.file.path;

	args << mkvInput;
	return args;
}

QStringList ActionEngine::buildFlagArgsForRemux(const QString& flagChangesJson)
{
	QStringList args;
	const QJsonArray changes = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	for (const QJsonValue& v : changes) {
		const QJsonObject o = v.toObject();
		const QString argName = mkvmergeFlagArg(o["flag"].toString());
		if (argName.isEmpty()) continue;
		// mkvmerge: --default-track-flag TRACKID:VALUE (0 or 1)
		args << argName
		     << QStringLiteral("%1:%2").arg(o["streamIndex"].toInt()).arg(o["value"].toBool() ? 1 : 0);
	}
	return args;
}

QStringList ActionEngine::buildPropEditArgs(const QString& filePath,
                                            const QString& flagChangesJson)
{
	// mkvpropedit format: file.mkv --edit track:@N --set flag-default=1 ...
	// track:@N is 1-indexed position across ALL tracks (video+audio+subtitle).
	// We use streamIndex+1 since ffprobe indices are 0-based and match track order in MKV.
	QStringList args;
	args << filePath;

	const QJsonArray changes = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	for (const QJsonValue& v : changes) {
		const QJsonObject o = v.toObject();
		const QString prop = mkvpropEditProp(o["flag"].toString());
		if (prop.isEmpty()) continue;
		const int trackPos = o["streamIndex"].toInt() + 1; // 0-based → 1-based
		args << QStringLiteral("--edit")
		     << QStringLiteral("track:@%1").arg(trackPos)
		     << QStringLiteral("--set")
		     << QStringLiteral("%1=%2").arg(prop).arg(o["value"].toBool() ? 1 : 0);
	}
	return args;
}

QString ActionEngine::filterFlagChangesForRemux(const QString& flagChangesJson,
                                                 const QStringList& commandArgs,
                                                 const QList<StreamRecord>& streams)
{
	if (flagChangesJson.isEmpty()) return {};

	QSet<int> keptAudio, keptSub, keptVideo;
	bool hasAudioFilter = false, hasSubFilter = false, hasVideoFilter = false;

	for (int i = 0; i + 1 < commandArgs.size(); ++i) {
		const QString& a = commandArgs[i];
		if (a == QLatin1String("--audio-tracks")) {
			hasAudioFilter = true;
			for (const QString& p : commandArgs[i + 1].split(','))
				keptAudio.insert(p.toInt());
			++i;
		} else if (a == QLatin1String("--subtitle-tracks")) {
			hasSubFilter = true;
			for (const QString& p : commandArgs[i + 1].split(','))
				keptSub.insert(p.toInt());
			++i;
		} else if (a == QLatin1String("--video-tracks")) {
			hasVideoFilter = true;
			for (const QString& p : commandArgs[i + 1].split(','))
				keptVideo.insert(p.toInt());
			++i;
		} else if (a == QLatin1String("--no-audio")) {
			hasAudioFilter = true;    // keptAudio stays empty → all audio removed
		} else if (a == QLatin1String("--no-subtitles")) {
			hasSubFilter = true;      // keptSub stays empty → all subs removed
		} else if (a == QLatin1String("--no-video")) {
			hasVideoFilter = true;    // keptVideo stays empty → all video removed
		}
	}

	if (!hasAudioFilter && !hasSubFilter && !hasVideoFilter) return flagChangesJson;

	QSet<int> removedIndices;
	for (const StreamRecord& s : streams) {
		if (s.isExternal) continue;  // sidecar — not a container track, no mkvmerge flag to filter
		if      (s.codecType == QLatin1String("audio")    && hasAudioFilter && !keptAudio.contains(s.streamIndex))
			removedIndices.insert(s.streamIndex);
		else if (s.codecType == QLatin1String("subtitle") && hasSubFilter   && !keptSub.contains(s.streamIndex))
			removedIndices.insert(s.streamIndex);
		else if (s.codecType == QLatin1String("video")    && hasVideoFilter && !keptVideo.contains(s.streamIndex))
			removedIndices.insert(s.streamIndex);
	}

	if (removedIndices.isEmpty()) return flagChangesJson;

	const QJsonArray changes = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	QJsonArray filtered;
	for (const QJsonValue& v : changes) {
		if (!removedIndices.contains(v.toObject()[QLatin1String("streamIndex")].toInt()))
			filtered.append(v);
	}
	if (filtered.size() == changes.size()) return flagChangesJson;
	return filtered.isEmpty() ? QString{} : QJsonDocument(filtered).toJson(QJsonDocument::Compact);
}

QString ActionEngine::filterInternalFlagChanges(const QString& flagChangesJson,
                                                  const QList<StreamRecord>& streams)
{
	if (flagChangesJson.isEmpty()) return {};
	QSet<int> externalIdxs;
	for (const StreamRecord& s : streams)
		if (s.isExternal) externalIdxs.insert(s.streamIndex);
	if (externalIdxs.isEmpty()) return flagChangesJson;

	const QJsonArray input = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	QJsonArray output;
	for (const QJsonValue& v : input) {
		if (!externalIdxs.contains(v.toObject()[QLatin1String("streamIndex")].toInt()))
			output.append(v);
	}
	if (output.size() == input.size()) return flagChangesJson;
	return output.isEmpty() ? QString{} : QString::fromUtf8(QJsonDocument(output).toJson(QJsonDocument::Compact));
}

QStringList ActionEngine::buildSidecarArgsForRemux(const QList<StreamRecord>& streams,
                                                     const QString& flagChangesJson)
{
	// Build map: streamIndex → {flag → value} for any pending flag overrides.
	QHash<int, QHash<QString, bool>> changesByStream;
	for (const QJsonValue& v : QJsonDocument::fromJson(flagChangesJson.toUtf8()).array()) {
		const QJsonObject o = v.toObject();
		changesByStream[o[QLatin1String("streamIndex")].toInt()]
		               [o[QLatin1String("flag")].toString()] = o[QLatin1String("value")].toBool();
	}

	QStringList args;
	for (const StreamRecord& s : streams) {
		if (!s.isExternal || s.externalPath.isEmpty()) continue;

		// Include all sidecar subtitles; apply pending flag changes where present.
		const auto& ch = changesByStream.value(s.streamIndex);
		const bool wantDefault = ch.value(QLatin1String("default"), s.isDefault);
		const bool wantForced  = ch.value(QLatin1String("forced"),  s.isForced);

		// Per-file mkvmerge options targeting track 0 (the only track in a sidecar)
		args << QStringLiteral("--default-track-flag")  << QStringLiteral("0:%1").arg(wantDefault ? 1 : 0);
		args << QStringLiteral("--forced-display-flag") << QStringLiteral("0:%1").arg(wantForced  ? 1 : 0);
		if (!s.language.isEmpty())
			args << QStringLiteral("--language") << QStringLiteral("0:%1").arg(s.language);
		args << s.externalPath;
	}
	return args;
}

QString ActionEngine::computeRenamedSidecarPath(const QString& currentPath, bool wantForced)
{
	const QFileInfo fi(currentPath);
	QStringList     parts = fi.completeBaseName().split(QLatin1Char('.'));
	parts.removeAll(QStringLiteral("forced"));
	if (wantForced) parts.append(QStringLiteral("forced"));
	return fi.absolutePath() + QLatin1Char('/')
	       + parts.join(QLatin1Char('.')) + QLatin1Char('.') + fi.suffix();
}


} // namespace Mc
