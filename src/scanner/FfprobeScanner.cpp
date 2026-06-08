#include "scanner/FfprobeScanner.h"

#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

namespace Mc {

FfprobeScanner::FfprobeScanner(const QString& ffprobePath, QObject* parent)
	: QObject(parent)
	, m_ffprobePath(ffprobePath)
{}

// ── Public API ────────────────────────────────────────────────────────────────

QString FfprobeScanner::validateFfprobe() const
{
	QProcess proc;
	proc.start(m_ffprobePath, {"-version"});
	if (!proc.waitForFinished(5000))
		return {};

	const QString out = QString::fromUtf8(proc.readAllStandardOutput());
	const QString err = QString::fromUtf8(proc.readAllStandardError());
	const QString combined = out + err;

	// First line is "ffprobe version X.Y.Z ..."
	const int nl = combined.indexOf('\n');
	return nl > 0 ? combined.left(nl).trimmed() : combined.trimmed();
}

FfprobeScanner::ScanResult FfprobeScanner::scanFile(const QString& filePath, qint64 scanRunId) const
{
	const QFileInfo fi(filePath);
	const QString ext = fi.suffix().toLower();

	QByteArray json;
	QString isoType;

	if (ext == QLatin1String("iso")) {
		// Blu-ray disc images use "bluray:" prefix for ffprobe; DVD uses "dvd://"
		json = invokeFfprobe(QStringLiteral("bluray:") + filePath);
		if (!json.isEmpty()) {
			isoType = QStringLiteral("iso-bluray");
		} else {
			json = invokeFfprobe(QStringLiteral("dvd://") + filePath);
			if (!json.isEmpty())
				isoType = QStringLiteral("iso-dvd");
		}
		if (json.isEmpty())
			return {{}, {}, QStringLiteral("ffprobe could not read ISO (tried bluray: and dvd://): ") + filePath, false};
	} else {
		json = invokeFfprobe(filePath);
		if (json.isEmpty())
			return {{}, {}, QStringLiteral("ffprobe produced no output for: ") + filePath, false};
	}

	auto result = parseJsonOutput(json, filePath, scanRunId);
	if (result.success) {
		result.file.mtimeMs   = fi.lastModified().toMSecsSinceEpoch();
		result.file.createdMs = fi.birthTime().toMSecsSinceEpoch();
		if (!isoType.isEmpty()) {
			result.file.container  = isoType;
			result.file.sizeBytes  = fi.size(); // use actual ISO file size, not parsed content size
		}
	}
	return result;
}

// ── Private ───────────────────────────────────────────────────────────────────

QByteArray FfprobeScanner::invokeFfprobe(const QString& filePath) const
{
	QProcess proc;
	const QStringList args = {
		"-v", "quiet",
		"-print_format", "json",
		"-show_format",
		"-show_streams",
		"-show_chapters",
		filePath
	};

	proc.start(m_ffprobePath, args);
	if (!proc.waitForFinished(30000)) {
		qWarning() << "FfprobeScanner: timeout scanning" << filePath;
		proc.kill();
		return {};
	}

	if (proc.exitCode() != 0) {
		qWarning() << "FfprobeScanner: ffprobe exited" << proc.exitCode()
				   << "for" << filePath
				   << "—" << QString::fromUtf8(proc.readAllStandardError()).trimmed();
		return {};
	}

	return proc.readAllStandardOutput();
}

FfprobeScanner::ScanResult FfprobeScanner::parseJsonOutput(
	const QByteArray& json, const QString& filePath, qint64 scanRunId) const
{
	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
	if (doc.isNull()) {
		return {{}, {}, QStringLiteral("JSON parse error: ") + parseError.errorString(), false};
	}

	const QJsonObject root = doc.object();
	const QJsonObject format = root.value("format").toObject();
	const QJsonArray  streams = root.value("streams").toArray();

	// ── Build FileRecord ──────────────────────────────────────────────────────
	FileRecord file;
	file.path     = filePath;
	file.filename = QFileInfo(filePath).fileName();
	file.sizeBytes = format.value("size").toString("0").toLongLong();
	file.durationSec  = format.value("duration").toString("0").toDouble();
	file.overallBitrate = format.value("bit_rate").toString("0").toLongLong();
	file.scanTime  = QDateTime::currentSecsSinceEpoch();
	file.scanRunId = scanRunId;

	// Container format — use format_name, strip alternatives (e.g. "matroska,webm" → "mkv")
	const QString formatName = format.value("format_name").toString();
	if (formatName.contains("matroska") || formatName.contains("webm"))
		file.container = "mkv";
	else if (formatName.contains("mp4") || formatName.contains("m4v"))
		file.container = "mp4";
	else if (formatName.contains("avi"))
		file.container = "avi";
	else if (formatName.contains("asf") || formatName.contains("wmv"))
		file.container = "wmv";
	else if (formatName.contains("mpeg") || formatName.contains("m2ts") || formatName.contains("mpegts"))
		file.container = "m2ts";
	else
		file.container = formatName.split(',').first().trimmed();

	// ── Build StreamRecords ───────────────────────────────────────────────────
	QList<StreamRecord> streamList;
	QString detectedOriginalLang;

	for (const QJsonValue& sv : streams) {
		const QJsonObject so = sv.toObject();
		StreamRecord s = parseStreamObject(so);

		// Keep only video, audio, subtitle streams
		if (s.codecType != "video" && s.codecType != "audio" && s.codecType != "subtitle")
			continue;

		streamList.append(s);

		// Heuristic: first audio track is often original language
		if (s.codecType == "audio" && detectedOriginalLang.isEmpty() && !s.language.isEmpty())
			detectedOriginalLang = s.language;
	}

	file.originalLanguage = detectedOriginalLang;

	ScanResult result;
	result.file    = file;
	result.streams = streamList;
	result.success = true;
	return result;
}

StreamRecord FfprobeScanner::parseStreamObject(const QJsonObject& obj)
{
	StreamRecord s;

	s.streamIndex = obj.value("index").toInt();
	s.codecType   = obj.value("codec_type").toString();
	s.codecName   = obj.value("codec_name").toString().toLower();

	// Language — check tags object
	const QJsonObject tags = obj.value("tags").toObject();
	s.language = normalizeLanguage(
		tags.value("language").toString(
			tags.value("LANGUAGE").toString()
		)
	);
	s.title = tags.value("title").toString(
				  tags.value("TITLE").toString()
			  );

	// Disposition flags
	const QJsonObject disp = obj.value("disposition").toObject();
	s.isDefault         = disp.value("default").toInt() != 0;
	s.isForced          = disp.value("forced").toInt() != 0;
	s.isHearingImpaired = disp.value("hearing_impaired").toInt() != 0;
	s.isVisualImpaired  = disp.value("visual_impaired").toInt() != 0;

	s.bitRate = obj.value("bit_rate").toString("0").toLongLong();

	if (s.codecType == "audio") {
		s.channels    = obj.value("channels").toInt();
		s.sampleRate  = obj.value("sample_rate").toString("0").toInt();
		s.codecProfile = obj.value("profile").toString();
	}
	else if (s.codecType == "video") {
		s.width       = obj.value("width").toInt();
		s.height      = obj.value("height").toInt();
		s.pixelFormat = obj.value("pix_fmt").toString();
		s.codecProfile = obj.value("profile").toString();
		s.codecLevel   = obj.value("level").toString();

		// Frame rate — prefer avg_frame_rate over r_frame_rate
		s.frameRate = obj.value("avg_frame_rate").toString(
						  obj.value("r_frame_rate").toString()
					  );

		// HDR detection — iterate ALL side_data entries and pick highest priority.
		// DV files have multiple entries: "Mastering display metadata" (HDR10 base)
		// followed by "DOVI configuration record". Breaking on first match misses DV.
		static const QHash<QString, int> hdrPriority = {
			{"DolbyVision", 4}, {"HDR10+", 3}, {"HDR10", 2}, {"HLG", 1}
		};
		int bestPri = 0;
		const QJsonArray sideDataList = obj.value("side_data_list").toArray();
		for (const QJsonValue& sdv : sideDataList) {
			const QString hdr = detectHdrFormat(tags, sdv.toObject());
			const int pri = hdrPriority.value(hdr, 0);
			if (pri > bestPri) {
				s.hdrFormat = hdr;
				bestPri = pri;
			}
		}

		// Fallback: color_transfer field
		if (s.hdrFormat.isEmpty()) {
			const QString transfer = obj.value("color_transfer").toString();
			if (transfer == "smpte2084")
				s.hdrFormat = "HDR10";
			else if (transfer == "arib-std-b67")
				s.hdrFormat = "HLG";
		}

		// Last resort: some encoders embed DV profile in the stream codec profile
		if (s.hdrFormat.isEmpty()) {
			const QString prof = s.codecProfile.toLower();
			if (prof.startsWith("dvhe") || prof.startsWith("dvav") || prof.contains("dolby vision"))
				s.hdrFormat = "DolbyVision";
		}
	}

	// Store full tags as extra_json for future use
	const QJsonDocument tagDoc(tags);
	s.extraJson = QString::fromUtf8(tagDoc.toJson(QJsonDocument::Compact));

	return s;
}

QString FfprobeScanner::detectHdrFormat(const QJsonObject& /*tags*/, const QJsonObject& sideData)
{
	const QString sdType = sideData.value("side_data_type").toString();

	// Dolby Vision: ffprobe reports "DOVI configuration record" (older builds may
	// say "Dolby Vision configuration record" or "Dolby Vision RPU Data")
	if (sdType.contains("DOVI", Qt::CaseInsensitive) ||
		sdType.contains("Dolby Vision", Qt::CaseInsensitive))
		return "DolbyVision";

	// HDR10+: "HDR Dynamic Metadata SMPTE2094-40" or contains "HDR10+"
	if (sdType.contains("HDR10+", Qt::CaseInsensitive) ||
		sdType.contains("SMPTE2094-40", Qt::CaseInsensitive))
		return "HDR10+";

	// HDR10 base layer: mastering display or content light level metadata
	if (sdType.contains("mastering display", Qt::CaseInsensitive) ||
		sdType.contains("content light level", Qt::CaseInsensitive))
		return "HDR10";

	return {};
}

QString FfprobeScanner::normalizeLanguage(const QString& lang)
{
	// Normalize common variants to ISO 639-2/B
	static const QHash<QString, QString> map = {
		{"iw",  "heb"},  // old Hebrew code
		{"in",  "ind"},  // old Indonesian code
		{"ji",  "yid"},  // old Yiddish code
		{"he",  "heb"},
		{"id",  "ind"},
		{"zh",  "zho"},
		{"cn",  "zho"},
		{"no",  "nor"},
		{"nb",  "nor"},
		{"nn",  "nor"},
		// Some files use 2-letter codes for common languages
		{"en",  "eng"},
		{"de",  "deu"},
		{"fr",  "fra"},
		{"es",  "spa"},
		{"it",  "ita"},
		{"ja",  "jpn"},
		{"ko",  "kor"},
		{"pt",  "por"},
		{"da",  "dan"},
		{"sv",  "swe"},
		{"nl",  "nld"},
		{"fi",  "fin"},
		{"pl",  "pol"},
		{"ru",  "rus"},
		{"ar",  "ara"},
		{"tr",  "tur"},
		{"cs",  "ces"},
		{"hu",  "hun"},
		{"ro",  "ron"},
		{"el",  "ell"},
		{"hr",  "hrv"},
		{"sk",  "slk"},
		{"bg",  "bul"},
		{"uk",  "ukr"},
	};

	if (lang.isEmpty())
		return {};

	const QString lower = lang.toLower();
	return map.value(lower, lower);
}

} // namespace Mc
