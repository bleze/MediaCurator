#include "engine/ActionEngine.h"

#include <QString>

namespace Mc {

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

} // namespace Mc
