#include "engine/ActionEngine.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QString>

#include <optional>

namespace Mc {

// Maps our flag names to mkvmerge's --XXX-flag argument names
static QString mkvmergeFlagArg(const QString& flag)
{
	if (flag == QLatin1String("default"))     return QStringLiteral("--default-track-flag");
	if (flag == QLatin1String("forced"))      return QStringLiteral("--forced-display-flag");
	if (flag == QLatin1String("original"))    return QStringLiteral("--original-flag");
	if (flag == QLatin1String("language"))    return QStringLiteral("--language");
	return {};
}

// Maps our flag names to mkvpropedit's property names
static QString mkvpropEditProp(const QString& flag)
{
	if (flag == QLatin1String("default"))     return QStringLiteral("flag-default");
	if (flag == QLatin1String("forced"))      return QStringLiteral("flag-forced");
	if (flag == QLatin1String("original"))    return QStringLiteral("flag-original");
	if (flag == QLatin1String("language"))    return QStringLiteral("language");
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
	bool coverArtRemoved = false;

	for (const TrackDecision& td : decision.tracks) {
		if (td.stream.isExternal) continue;  // sidecar file — not in container, handled separately
		if (td.stream.codecType == "audio")    hasAudio = true;
		if (td.stream.codecType == "subtitle") hasSub   = true;
		if (td.stream.codecType == "video" && td.decision == Decision::Remove) {
			anyVideoRemoved = true;
			const QString cn = td.stream.codecName.toLower();
			if (cn == QLatin1String("mjpeg") || cn == QLatin1String("png"))
				coverArtRemoved = true;
		}
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

	// A Matroska "cover.jpg"-style embedded image is stored as a container
	// ATTACHMENT, not a track — ffprobe synthesizes a fake attached_pic video
	// stream for it (which is what coverArtRemoved above just matched and is
	// what --video-tracks/--no-video was built from), but mkvmerge's attachment
	// handling is completely separate from track selection. Without an explicit
	// --attachments/--no-attachments flag here, mkvmerge's default of copying
	// every attachment silently carries the "removed" cover art straight through.
	// Only image-typed attachments are stripped — fonts attached for styled
	// (ASS/SSA) subtitles must survive, or subtitle rendering breaks.
	if (coverArtRemoved && cont != QLatin1String("iso-bluray") && cont != QLatin1String("iso-dvd")) {
		const QJsonArray attachments = identifyAttachments(mkvInput);
		QStringList keepAttachmentIds;
		bool anyImageAttachment = false;
		for (const QJsonValue& v : attachments) {
			const QJsonObject a = v.toObject();
			if (a.value("content_type").toString().startsWith(QLatin1String("image/")))
				anyImageAttachment = true;
			else
				keepAttachmentIds << QString::number(a.value("id").toInt());
		}
		if (anyImageAttachment) {
			if (keepAttachmentIds.isEmpty())
				args << "--no-attachments";
			else
				args << "--attachments" << keepAttachmentIds.join(',');
		}
	}

	args << mkvInput;
	return args;
}

QJsonArray ActionEngine::identifyAttachments(const QString& filePath) const
{
	QProcess proc;
	proc.start(m_mkvmergePath, {"-J", filePath});
	if (!proc.waitForFinished(10000)) {
		proc.kill();
		return {};
	}
	if (proc.exitCode() > 1) // mkvmerge: 0=ok, 1=warnings (still valid), 2=error
		return {};

	const QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
	return doc.object().value(QLatin1String("attachments")).toArray();
}

QStringList ActionEngine::buildFlagArgsForRemux(const QString& flagChangesJson)
{
	QStringList args;
	const QJsonArray changes = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	for (const QJsonValue& v : changes) {
		const QJsonObject o = v.toObject();
		const QString flag    = o["flag"].toString();
		const QString argName = mkvmergeFlagArg(flag);
		if (argName.isEmpty()) continue;
		const QString value = (flag == QLatin1String("language"))
		    ? o["value"].toString()
		    : QString::number(o["value"].toBool() ? 1 : 0);
		// mkvmerge: --default-track-flag TRACKID:VALUE (0/1) or --language TRACKID:eng
		args << argName << QStringLiteral("%1:%2").arg(o["streamIndex"].toInt()).arg(value);
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
		const QString flag = o["flag"].toString();
		const QString prop = mkvpropEditProp(flag);
		if (prop.isEmpty()) continue;
		const int     trackPos = o["streamIndex"].toInt() + 1; // 0-based → 1-based
		const QString value = (flag == QLatin1String("language"))
		    ? o["value"].toString()
		    : QString::number(o["value"].toBool() ? 1 : 0);
		args << QStringLiteral("--edit")
		     << QStringLiteral("track:@%1").arg(trackPos)
		     << QStringLiteral("--set")
		     << QStringLiteral("%1=%2").arg(prop, value);
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

QString ActionEngine::serializeStreamSnapshot(const QList<StreamRecord>& streams)
{
	QJsonArray arr;
	for (const StreamRecord& s : streams) {
		QJsonObject o;
		o["idx"]        = s.streamIndex;
		o["type"]       = s.codecType;
		o["codec"]      = s.codecName;
		o["profile"]    = s.codecProfile;
		o["lang"]       = s.language;
		o["title"]      = s.title;
		o["ch"]         = s.channels;
		o["w"]          = s.width;
		o["h"]          = s.height;
		o["hdr"]        = s.hdrFormat;
		if (s.maxCll > 0) o["max_cll"] = s.maxCll;
		if (s.maxFall > 0) o["max_fall"] = s.maxFall;
		if (!s.masteringDisplay.isEmpty()) o["mastering"] = s.masteringDisplay;
		o["default"]    = s.isDefault;
		o["forced"]     = s.isForced;
		o["original"]   = s.isOriginal;
		o["commentary"] = s.isCommentary;
		o["hi"]         = s.isHearingImpaired;
		o["ext"]        = s.isExternal;
		o["extPath"]    = s.externalPath;
		arr.append(o);
	}
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QList<StreamRecord> ActionEngine::deserializeStreamSnapshot(const QString& json)
{
	QList<StreamRecord> result;
	const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
	for (const auto& v : arr) {
		const QJsonObject o = v.toObject();
		StreamRecord s;
		s.streamIndex       = o["idx"].toInt();
		s.codecType         = o["type"].toString();
		s.codecName         = o["codec"].toString();
		s.codecProfile      = o["profile"].toString();
		s.language          = o["lang"].toString();
		s.title             = o["title"].toString();
		s.channels          = o["ch"].toInt();
		s.width             = o["w"].toInt();
		s.height            = o["h"].toInt();
		s.hdrFormat         = o["hdr"].toString();
		s.maxCll            = o["max_cll"].toInt();
		s.maxFall           = o["max_fall"].toInt();
		s.masteringDisplay  = o["mastering"].toString();
		s.isDefault         = o["default"].toBool();
		s.isForced          = o["forced"].toBool();
		s.isOriginal        = o["original"].toBool();
		s.isCommentary      = o["commentary"].toBool();
		s.isHearingImpaired = o["hi"].toBool();
		s.isExternal        = o["ext"].toBool();
		s.externalPath      = o["extPath"].toString();
		result << s;
	}
	return result;
}

QList<StreamRecord> ActionEngine::computeKeptStreams(
	const QList<StreamRecord>& all,
	const QString& commandArgsJson)
{
	if (commandArgsJson.isEmpty())
		return all;

	// mkvmerge args contain --audio-tracks N,M and --subtitle-tracks N,M
	// where N,M are stream indices to *include*.  Video tracks are always kept.
	// If neither flag is present for a type, all tracks of that type are kept.
	const QJsonArray arr = QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	const QStringList args = [&]{
		QStringList s;
		for (const auto& v : arr) s << v.toString();
		return s;
	}();

	// Parse include-sets per type
	auto parseSet = [&](const QString& flag) -> std::optional<QSet<int>> {
		for (int i = 0; i < args.size() - 1; ++i) {
			if (args[i] == flag) {
				QSet<int> set;
				for (const QString& tok : args[i + 1].split(',', Qt::SkipEmptyParts))
					set.insert(tok.trimmed().toInt());
				return set;
			}
		}
		return std::nullopt;
	};

	const auto audioInclude    = parseSet("--audio-tracks");
	const auto subtitleInclude = parseSet("--subtitle-tracks");
	const auto videoInclude    = parseSet("--video-tracks");
	const bool noAudio     = args.contains(QStringLiteral("--no-audio"));
	const bool noSubtitles = args.contains(QStringLiteral("--no-subtitles"));
	const bool noVideo     = args.contains(QStringLiteral("--no-video"));

	QList<StreamRecord> kept;
	for (const StreamRecord& s : all) {
		if (s.isExternal) { kept << s; continue; }  // sidecar — not in container, mkvmerge args don't apply
		if (s.codecType == "video") {
			if (noVideo) continue;
			// When --video-tracks is absent, keep all video (backward-compatible default).
			if (videoInclude && !videoInclude->contains(s.streamIndex)) continue;
			kept << s;
		} else if (s.codecType == "audio") {
			if (noAudio) continue;
			if (!audioInclude || audioInclude->contains(s.streamIndex))
				kept << s;
		} else if (s.codecType == "subtitle") {
			if (noSubtitles) continue;
			if (!subtitleInclude || subtitleInclude->contains(s.streamIndex))
				kept << s;
		} else {
			kept << s; // data/attachment tracks always kept
		}
	}
	return kept;
}

QString ActionEngine::insertLanguageIntoSidecarPath(const QString& currentPath,
                                                     const QString& videoBaseName,
                                                     const QString& langCode)
{
	const QFileInfo fi(currentPath);
	const QString   subBase   = fi.completeBaseName();
	const QString   remainder = subBase.mid(videoBaseName.length()); // "" | ".forced" | ".sdh" …
	return fi.absolutePath() + QLatin1Char('/')
	       + videoBaseName + QLatin1Char('.') + langCode + remainder
	       + QLatin1Char('.') + fi.suffix();
}


} // namespace Mc
