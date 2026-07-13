#pragma once

#include "core/DatabaseManager.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <algorithm>

namespace Mc {

enum class Decision {
	Keep,       // Track should be kept
	Remove,     // Track should be removed
	Unsure,     // Needs user review
};

/** A rule engine decision for a single stream track */
struct TrackDecision {
	StreamRecord    stream;
	Decision        decision = Decision::Keep;
	QString         reason;             // Human-readable explanation
	bool            userOverride = false; // User manually changed this decision
};

// Default fallback bitrates (bps) used when ffprobe reports none.
// Values represent AVERAGE bitrate over the full file duration, not peak/burst.
// Update these from Tools > Estimation Calibration Data after accumulating jobs.
namespace FallbackBps {
	constexpr double kAudio            =   192'000.0; // lossy audio (AC3, AAC, MP3, DTS core, E-AC3)
	constexpr double kDtsHd            = 3'500'000.0; // DTS-HD MA / DTS-HD HRA — 5.1 (6ch) reference
	constexpr double kDtsHdPerChannel  =   551'000.0; // kDtsHd / 6 — scale by actual channel count
	constexpr double kTrueHd           = 3'500'000.0; // TrueHD / Atmos — 5.1 (6ch) reference
	constexpr double kTrueHdPerChannel =   583'333.0; // kTrueHd / 6 — scale by actual channel count
	constexpr double kPcmDefault       = 4'608'000.0; // PCM fallback: 48 kHz x 24-bit x 4ch
	constexpr double kFlac             =   620'000.0; // FLAC (lossless, variable) — 2.0 (stereo) reference
	constexpr double kFlacPerChannel   =   310'000.0; // kFlac / 2 — scale by actual channel count
	constexpr double kPgsSubtitle      =    16'000.0; // PGS / VOBSUB (image-based bitmap; forced-only tracks run much lower, full/SDH tracks higher)
	constexpr double kTextSubtitle     =     5'000.0; // SRT / ASS / SSA / WebVTT
} // namespace FallbackBps

// Returns the effective bitrate used for size estimation:
// declared bitrate when ffprobe provides one, otherwise a FallbackBps constant.
// Lossless audio codecs (DTS-HD MA, TrueHD, PCM) commonly report no bitrate in MKV.
inline double effectiveBitrate(const StreamRecord& s) noexcept
{
	if (s.bitRate > 0) return static_cast<double>(s.bitRate);
	if (s.codecType == QLatin1String("audio")) {
		if (s.codecProfile.contains(QLatin1String("DTS-HD"), Qt::CaseInsensitive))
			return s.channels > 0 ? FallbackBps::kDtsHdPerChannel * s.channels : FallbackBps::kDtsHd;
		const QString cn = s.codecName.toLower();
		if (cn == QLatin1String("truehd"))
			return s.channels > 0 ? FallbackBps::kTrueHdPerChannel * s.channels : FallbackBps::kTrueHd;
		if (cn.startsWith(QLatin1String("pcm_"))) {
			if (s.sampleRate > 0 && s.channels > 0) {
				const int bits = cn.contains(QLatin1String("24")) ? 24
				               : cn.contains(QLatin1String("32")) ? 32 : 16;
				return static_cast<double>(s.sampleRate) * bits * s.channels;
			}
			return FallbackBps::kPcmDefault;
		}
		if (cn == QLatin1String("flac"))
			return s.channels > 0 ? FallbackBps::kFlacPerChannel * s.channels : FallbackBps::kFlac;
		return FallbackBps::kAudio;
	}
	if (s.codecType == QLatin1String("subtitle")) {
		const bool isImageSub = s.codecName.contains(QLatin1String("pgs"))
		                     || s.codecName == QLatin1String("dvd_subtitle");
		return isImageSub ? FallbackBps::kPgsSubtitle : FallbackBps::kTextSubtitle;
	}
	return 0.0;
}

// Fine-grained format identity for calibration — one entry per actual codec, so the
// calibration report can show a row per real format (AC3, AAC, DTS, DTS-HD MA, ...)
// instead of pre-collapsing several formats into one bucket. codecName alone very
// nearly works, except ffprobe reports the same codec_name "dts" for both plain DTS
// and DTS-HD MA/HRA — codecProfile is what actually tells them apart, so that's the
// one case needing a suffix. Empty means no fallback ever applies (e.g. video).
inline QString calibrationFormatKey(const QString& codecName, const QString& codecType,
                                     const QString& codecProfile)
{
	if (codecType != QLatin1String("audio") && codecType != QLatin1String("subtitle"))
		return {};
	QString key = codecName.toLower();
	if (codecType == QLatin1String("audio")
	        && codecProfile.contains(QLatin1String("DTS-HD"), Qt::CaseInsensitive))
		key += QStringLiteral("-hd");
	return key;
}

// Several calibrationFormatKey() formats can share one FallbackBps constant (e.g. ac3,
// aac, mp3, and plain dts all use the generic kAudio fallback) — this is the coarser
// level a suggested correction is actually actionable at, since there's only one
// constant in code to change. Must be kept in sync with effectiveBitrate()'s branching.
inline QString fallbackBpsKey(const QString& formatKey, const QString& codecType)
{
	if (codecType == QLatin1String("audio")) {
		if (formatKey.endsWith(QLatin1String("-hd")))    return QStringLiteral("dts-hd");
		if (formatKey == QLatin1String("truehd"))        return QStringLiteral("truehd");
		if (formatKey.startsWith(QLatin1String("pcm_"))) return QStringLiteral("pcm");
		if (formatKey == QLatin1String("flac"))          return QStringLiteral("flac");
		return QStringLiteral("audio-lossy");
	}
	if (codecType == QLatin1String("subtitle")) {
		const bool isImageSub = formatKey.contains(QLatin1String("pgs"))
		                     || formatKey == QLatin1String("dvd_subtitle");
		return isImageSub ? QStringLiteral("image-subtitle") : QStringLiteral("text-subtitle");
	}
	return {};
}

// Returns the FallbackBps value and constexpr name for a fallbackBpsKey() bucket —
// the single source of truth for both, shared by calibration storage and display.
inline double fallbackBpsForKey(const QString& key)
{
	if (key == QLatin1String("dts-hd"))          return FallbackBps::kDtsHdPerChannel;
	if (key == QLatin1String("truehd"))          return FallbackBps::kTrueHdPerChannel;
	if (key == QLatin1String("pcm"))             return FallbackBps::kPcmDefault;
	if (key == QLatin1String("flac"))            return FallbackBps::kFlacPerChannel;
	if (key == QLatin1String("audio-lossy"))     return FallbackBps::kAudio;
	if (key == QLatin1String("image-subtitle"))  return FallbackBps::kPgsSubtitle;
	if (key == QLatin1String("text-subtitle"))   return FallbackBps::kTextSubtitle;
	return 0.0;
}

inline QString fallbackBpsConstName(const QString& key)
{
	if (key == QLatin1String("dts-hd"))          return QStringLiteral("kDtsHdPerChannel");
	if (key == QLatin1String("truehd"))          return QStringLiteral("kTrueHdPerChannel");
	if (key == QLatin1String("pcm"))             return QStringLiteral("kPcmDefault");
	if (key == QLatin1String("flac"))            return QStringLiteral("kFlacPerChannel");
	if (key == QLatin1String("audio-lossy"))     return QStringLiteral("kAudio");
	if (key == QLatin1String("image-subtitle"))  return QStringLiteral("kPgsSubtitle");
	if (key == QLatin1String("text-subtitle"))   return QStringLiteral("kTextSubtitle");
	return {};
}

// Proportional savings estimate: removed tracks' share of total declared bitrate,
// applied to the known file size. The denominator is floored at the actual container
// bitrate (fileSize * 8 / duration) so video tracks that declare no bitrate don't
// collapse the denominator to just the audio tracks.
inline qint64 estimateSavingBytes(const QList<StreamRecord>& allStreams,
                                   const QSet<int>& removedStreamIndices,
                                   qint64 fileSizeBytes,
                                   double durationSec)
{
	if (fileSizeBytes <= 0) return 0;
	double totalBr = 0.0, removedBr = 0.0;
	for (const StreamRecord& s : allStreams) {
		const double br = effectiveBitrate(s);
		totalBr += br;
		if (removedStreamIndices.contains(s.streamIndex))
			removedBr += br;
	}
	if (durationSec > 0) {
		totalBr = qMax(totalBr, static_cast<double>(fileSizeBytes) * 8.0 / durationSec);
	} else {
		// Without duration the qMax floor can't fire. If no video stream has a
		// declared bitrate, totalBr is dominated by audio/sub fallbacks and the
		// ratio approaches 1.0, claiming the whole file as savings. Return 0 instead.
		const bool hasVideoBitrate = std::any_of(allStreams.cbegin(), allStreams.cend(),
		    [](const StreamRecord& s) {
		        return s.codecType == QLatin1String("video") && s.bitRate > 0;
		    });
		if (!hasVideoBitrate) return 0;
	}
	if (totalBr <= 0.0) return 0;
	return static_cast<qint64>(removedBr / totalBr * static_cast<double>(fileSizeBytes));
}

// Per-track estimate entry stored at job-creation time for calibration.
// declaredBitrate == 0 means ffprobe reported none and the fallback was used —
// that flag is the key for calibration queries: jobs where declaredBitrate == 0
// and actual savings diverge significantly reveal which fallbacks need tuning.
struct StreamEstimate {
	int     streamIndex     = -1;
	QString codecType;
	QString codecName;
	QString codecProfile;
	int     channels        = 0;
	int     sampleRate      = 0;
	qint64  declaredBitrate = 0; // 0 = fallback used
	qint64  estimatedBytes  = 0;
};

// Returns per-stream estimates for every track in removedStreamIndices.
// Uses the same totalBr denominator as estimateSavingBytes so the individual
// estimates sum to the overall estimate.
inline QList<StreamEstimate> perStreamEstimates(const QList<StreamRecord>& allStreams,
                                                  const QSet<int>& removedStreamIndices,
                                                  qint64 fileSizeBytes,
                                                  double durationSec)
{
	if (fileSizeBytes <= 0) return {};
	double totalBr = 0.0;
	for (const StreamRecord& s : allStreams)
		totalBr += effectiveBitrate(s);
	if (durationSec > 0) {
		totalBr = qMax(totalBr, static_cast<double>(fileSizeBytes) * 8.0 / durationSec);
	} else {
		const bool hasVideoBitrate = std::any_of(allStreams.cbegin(), allStreams.cend(),
		    [](const StreamRecord& s) {
		        return s.codecType == QLatin1String("video") && s.bitRate > 0;
		    });
		if (!hasVideoBitrate) return {};
	}
	if (totalBr <= 0.0) return {};

	QList<StreamEstimate> result;
	for (const StreamRecord& s : allStreams) {
		if (!removedStreamIndices.contains(s.streamIndex)) continue;
		StreamEstimate e;
		e.streamIndex     = s.streamIndex;
		e.codecType       = s.codecType;
		e.codecName       = s.codecName;
		e.codecProfile    = s.codecProfile;
		e.channels        = s.channels;
		e.sampleRate      = s.sampleRate;
		e.declaredBitrate = s.bitRate > 0 ? s.bitRate : 0;
		e.estimatedBytes  = static_cast<qint64>(effectiveBitrate(s) / totalBr * fileSizeBytes);
		result.append(e);
	}
	return result;
}

// Serialises perStreamEstimates() to a compact JSON array suitable for DB storage.
inline QString buildStreamEstimatesJson(const QList<StreamRecord>& allStreams,
                                         const QSet<int>& removedStreamIndices,
                                         qint64 fileSizeBytes,
                                         double durationSec)
{
	const auto estimates = perStreamEstimates(allStreams, removedStreamIndices, fileSizeBytes, durationSec);
	QJsonArray arr;
	for (const StreamEstimate& e : estimates) {
		QJsonObject obj;
		obj[QStringLiteral("streamIndex")]     = e.streamIndex;
		obj[QStringLiteral("codecType")]       = e.codecType;
		obj[QStringLiteral("codecName")]       = e.codecName;
		obj[QStringLiteral("codecProfile")]    = e.codecProfile;
		obj[QStringLiteral("channels")]        = e.channels;
		obj[QStringLiteral("sampleRate")]      = e.sampleRate;
		obj[QStringLiteral("declaredBitrate")] = e.declaredBitrate;
		obj[QStringLiteral("estimatedBytes")]  = e.estimatedBytes;
		arr.append(obj);
	}
	return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

/** All decisions for a single file */
struct FileDecision {
	FileRecord              file;
	QList<TrackDecision>    tracks;

	/** How many tracks are marked for removal */
	int removalCount() const {
		int n = 0;
		for (const auto& t : tracks)
			if (t.decision == Decision::Remove) ++n;
		return n;
	}

	qint64 estimatedSavingBytes() const {
		QSet<int> removed;
		QList<StreamRecord> all;
		for (const auto& t : tracks) {
			all << t.stream;
			if (t.decision == Decision::Remove)
				removed.insert(t.stream.streamIndex);
		}
		return Mc::estimateSavingBytes(all, removed, file.sizeBytes, file.durationSec);
	}

	QString streamEstimatesJson() const {
		QSet<int> removed;
		QList<StreamRecord> all;
		for (const auto& t : tracks) {
			all << t.stream;
			if (t.decision == Decision::Remove)
				removed.insert(t.stream.streamIndex);
		}
		return Mc::buildStreamEstimatesJson(all, removed, file.sizeBytes, file.durationSec);
	}
};

} // namespace Mc
