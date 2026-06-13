#include "engine/ActionEngine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

} // namespace Mc
