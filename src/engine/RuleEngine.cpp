#include "engine/RuleEngine.h"
#include "classifier/RegexClassifier.h"
#include "core/UserProfile.h"

#include <QHash>
#include <QSet>

namespace Mc {

RuleEngine::RuleEngine(const UserProfile* profile, QObject* parent)
	: QObject(parent), m_profile(profile)
{}

// Some MKV files use the ISO 639-2 bibliographic code (e.g. "ger", "fre", "cze")
// while our settings UI stores the terminological code (e.g. "deu", "fra", "ces").
// Normalize to terminological before comparing so both forms match.
static const QString normalizeLang(const QString& lang)
{
	static const QHash<QString, QString> b2t = {
		{"ger","deu"}, {"fre","fra"}, {"cze","ces"}, {"dut","nld"},
		{"chi","zho"}, {"rum","ron"}, {"gre","ell"}, {"slo","slk"},
		{"alb","sqi"}, {"arm","hye"}, {"baq","eus"}, {"bur","mya"},
		{"tib","bod"}, {"mac","mkd"}, {"geo","kat"}, {"per","fas"},
		{"ice","isl"}, {"may","msa"}, {"wel","cym"},
	};
	const QString lc = lang.toLower();
	return b2t.value(lc, lc);
}

bool RuleEngine::isInUnderstoodLanguage(const QString& lang) const
{
	if (!m_profile || lang.isEmpty()) return false;
	const QString norm = normalizeLang(lang);
	for (const QString& l : m_profile->understoodLanguages()) {
		if (normalizeLang(l) == norm)
			return true;
	}
	return false;
}

// Canonical format ID from stream metadata — shared by display and redundancy logic.
// static
QString RuleEngine::audioFormatId(const StreamRecord& s)
{
	const QString n = s.codecName.toLower();
	const QString p = s.codecProfile.toLower();
	const QString t = s.title.toLower();
	if (n == "truehd")
		return (p.contains("atmos") || t.contains("atmos")) ? "atmos" : "truehd";
	if (n == "dts") {
		if (p.contains("dts:x") || p.contains(":x") || t.contains("dts:x") || t.contains("dts-x"))
			return "dtsx";
		if (p.contains("ma") || t.contains("master audio") || t.contains("dts-hd ma"))
			return "dtshdma";
		if (p.contains("hra") || t.contains("hra"))
			return "dtshd_hra";
		return "dts";
	}
	if (n == "flac" || n.startsWith("pcm_")) return "flac";
	if (n == "eac3")  return "eac3";
	if (n == "ac3")   return "ac3";
	if (n == "aac")   return "aac";
	if (n == "mp3")   return "mp3";
	return n;
}

// Higher return value = higher quality (more preferred).
// Uses the user's profile order when available; falls back to a built-in default.
int RuleEngine::audioQualityTier(const StreamRecord& s) const
{
	const QString id = audioFormatId(s);
	if (m_profile) {
		const QStringList& order = m_profile->audioFormatOrder();
		const int pos = order.indexOf(id);
		if (pos >= 0) return order.size() - pos;  // index 0 = best = highest tier
		return -1;
	}
	// No profile — fall back to built-in defaults.
	const QStringList def = UserProfile::defaultAudioFormatOrder();
	const int pos = def.indexOf(id);
	return (pos >= 0) ? def.size() - pos : 0;
}

bool RuleEngine::isAudioFormatDisabled(const StreamRecord& s) const
{
	if (!m_profile) return false;
	return m_profile->disabledAudioFormats().contains(audioFormatId(s));
}

bool RuleEngine::isRedundantAudio(const StreamRecord& s, const QList<StreamRecord>& siblings) const
{
	// A default-flagged track is the author's explicit choice for this file — protect it
	// from codec-redundancy removal, UNLESS a higher-quality default track also exists for
	// the same language.  MP4 files routinely set default=1 on all audio tracks, making the
	// guard meaningless if applied unconditionally; MKV files typically have only one default
	// per language so the guard still holds there.
	if (s.isDefault) {
		const int myTier = audioQualityTier(s);
		bool hasBetterDefault = false;
		for (const StreamRecord& sib : siblings) {
			if (sib.id == s.id || sib.codecType != "audio") continue;
			if (normalizeLang(sib.language) != normalizeLang(s.language)) continue;
			if (!sib.isDefault)                             continue;
			if (isAudioFormatDisabled(sib))                 continue;
			// Fewer channels = spatial downgrade — not a "better" default.
			if (s.channels > 0 && sib.channels > 0 && sib.channels < s.channels) continue;
			if (audioQualityTier(sib) > myTier) { hasBetterDefault = true; break; }
		}
		if (!hasBetterDefault) return false;
		// A higher-quality default exists in the same language — fall through to the
		// standard redundancy check so lower-quality defaults can be removed.
	}

	const int  myTier     = audioQualityTier(s);
	const bool myDisabled = isAudioFormatDisabled(s);
	const int  myCh       = s.channels;

	bool hasEnabledAlt    = false;
	bool hasBetterEnabled = false;

	for (const StreamRecord& sib : siblings) {
		if (sib.id == s.id || sib.codecType != "audio") continue;
		if (normalizeLang(sib.language) != normalizeLang(s.language)) continue;
		if (isAudioFormatDisabled(sib))                 continue;

		// A sibling with fewer channels is a spatial downgrade — never treat it as a
		// valid replacement regardless of codec tier.  FLAC 2.0 does not make DD 5.1
		// redundant.  Only skip this guard when either side has unknown channel count (0).
		if (myCh > 0 && sib.channels > 0 && sib.channels < myCh) continue;

		hasEnabledAlt = true;
		if (audioQualityTier(sib) > myTier) {
			hasBetterEnabled = true;
		} else if (audioQualityTier(sib) == myTier && audioFormatId(sib) == audioFormatId(s)) {
			// Equal-tier duplicate of the same format: keep the default-flagged one,
			// or the earlier stream index when both are non-default.
			if (!s.isDefault && sib.isDefault)
				hasBetterEnabled = true;
			else if (!s.isDefault && !sib.isDefault && sib.streamIndex < s.streamIndex)
				hasBetterEnabled = true;
		}
	}

	// Disabled format: remove whenever any enabled alternative with >= channels exists.
	// Enabled format: remove only when a higher-priority enabled sibling with >= channels exists.
	return myDisabled ? hasEnabledAlt : hasBetterEnabled;
}

// ── Subtitle format redundancy ────────────────────────────────────────────────

// static
QString RuleEngine::subtitleFormatId(const StreamRecord& s)
{
	const QString n = s.codecName.toLower();
	if (n == "hdmv_pgs_subtitle" || n == "pgs") return QStringLiteral("pgs");
	if (n == "dvd_subtitle" || n == "dvdsub")   return QStringLiteral("vobsub");
	if (n == "ass" || n == "ssa")               return QStringLiteral("ass");
	if (n == "subrip" || n == "srt")            return QStringLiteral("srt");
	if (n == "webvtt" || n == "vtt")            return QStringLiteral("vtt");
	return n;
}

int RuleEngine::subtitleQualityTier(const StreamRecord& s) const
{
	const QString id = subtitleFormatId(s);
	if (m_profile) {
		const QStringList& order = m_profile->subtitleFormatOrder();
		const int pos = order.indexOf(id);
		if (pos >= 0) return order.size() - pos;
		return -1;
	}
	const QStringList def = UserProfile::defaultSubtitleFormatOrder();
	const int pos = def.indexOf(id);
	return (pos >= 0) ? def.size() - pos : 0;
}

bool RuleEngine::isSubtitleFormatDisabled(const StreamRecord& s) const
{
	if (!m_profile) return false;
	return m_profile->disabledSubtitleFormats().contains(subtitleFormatId(s));
}

bool RuleEngine::isRedundantSubtitle(const StreamRecord& s,
                                      const QList<StreamRecord>& siblings) const
{
	const int     myTier     = subtitleQualityTier(s);
	const bool    myDisabled = isSubtitleFormatDisabled(s);
	const bool    myIsSdh    = s.isHearingImpaired || s.trackType == "sdh";
	const QString myLang     = normalizeLang(s.language);

	bool hasEnabledAlt    = false;
	bool hasBetterEnabled = false;

	for (const StreamRecord& sib : siblings) {
		if (sib.id == s.id || sib.codecType != "subtitle") continue;
		if (normalizeLang(sib.language) != myLang) continue;
		// Only compare within the same content group: SDH vs regular and forced vs regular
		// are separate groups — a forced sub is not a replacement for a non-forced one.
		const bool sibIsSdh = sib.isHearingImpaired || sib.trackType == "sdh";
		if (sibIsSdh != myIsSdh) continue;
		if (sib.isForced != s.isForced) continue;
		if (isSubtitleFormatDisabled(sib)) continue;

		hasEnabledAlt = true;
		if (subtitleQualityTier(sib) > myTier) hasBetterEnabled = true;
	}

	return myDisabled ? hasEnabledAlt : hasBetterEnabled;
}

FileDecision RuleEngine::evaluateFile(const FileRecord& file, const QList<StreamRecord>& streams) const
{
	FileDecision fd;
	fd.file = file;

	static const RegexClassifier classifier;

	QList<StreamRecord> audioTracks, subtitleTracks;
	for (const auto& s : streams) {
		if (s.codecType == "audio")    audioTracks    << s;
		if (s.codecType == "subtitle") subtitleTracks << s;
	}

	// Pre-pass: channel-count heuristic — stereo alongside surround implies commentary
	QSet<qint64> heuristicCommentary;
	if (m_profile && m_profile->stereoAsCommentaryHeuristic()) {
		for (const StreamRecord& stereo : audioTracks) {
			if (stereo.channels < 1 || stereo.channels > 2) continue;

			// Skip tracks the classifier already identified as commentary
			const ClassificationResult stereoSig = classifier.classify(stereo.title, stereo.language, stereo.codecName);
			if (stereoSig.type == TrackType::Commentary && stereoSig.confidence >= 0.80) continue;

			for (const StreamRecord& surround : audioTracks) {
				if (surround.id == stereo.id || surround.channels < 6) continue;
				const bool langMatch = (normalizeLang(stereo.language) == normalizeLang(surround.language))
				                    || stereo.language.isEmpty()  || stereo.language  == "und"
				                    || surround.language.isEmpty() || surround.language == "und";
				if (langMatch) {
					heuristicCommentary.insert(stereo.id);
					break;
				}
			}
		}
	}

	// Pre-pass: subtitle SDH — build per-language presence maps.
	// Must use the SAME detection logic as the main loop (disposition + classifier)
	// so a track identified as SDH only by its title (no hearing_impaired disposition)
	// is not incorrectly placed into langsWithNonSdhSub, which would cause it to
	// satisfy its own "non-SDH alternative exists" guard and remove itself.
	// Commentary and signs subtitles are separate content buckets — they must not be
	// counted as regular-subtitle alternatives when deciding whether to remove an SDH
	// track. E.g. "English (Commentary #1)" must not make "English (SDH)" look redundant.
	QSet<QString> langsWithNonSdhSub, langsWithSdhSub;
	for (const StreamRecord& s : streams) {
		if (s.codecType != "subtitle") continue;
		const QString lang = normalizeLang(s.language);
		const ClassificationResult preCls = classifier.classify(s.title, s.language, s.codecName);
		const bool isCommentaryOrSigns =
		    (preCls.type == TrackType::Commentary && preCls.confidence >= 0.80)
		    || s.trackType == QLatin1String("commentary")
		    || (preCls.type == TrackType::Signs && preCls.confidence >= 0.90)
		    || s.trackType == QLatin1String("signs");
		if (isCommentaryOrSigns) continue;
		const bool isSdhStream = s.isHearingImpaired || s.trackType == "sdh"
		                         || preCls.type == TrackType::Sdh
		                         || preCls.type == TrackType::HearingImpaired;
		if (isSdhStream)
			langsWithSdhSub.insert(lang);
		else
			langsWithNonSdhSub.insert(lang);
	}

	for (const StreamRecord& s : streams) {
		TrackDecision td;
		td.stream   = s;
		td.decision = Decision::Keep;

		// Classify the track title into a semantic type
		const ClassificationResult cls = classifier.classify(s.title, s.language, s.codecName);

		if (s.codecType == "video") {
			const QString cn = s.codecName.toLower();
			if (m_profile && m_profile->removeMjpegCoverArt()
					&& (cn == QStringLiteral("mjpeg") || cn == QStringLiteral("png"))) {
				td.decision = Decision::Remove;
				td.reason   = QStringLiteral("Embedded cover-art (%1 video stream)").arg(s.codecName.toUpper());
			}
		}
		else if (s.codecType == "audio") {
			// Tier 0: commentary — title-classified or channel-count heuristic
			const bool isClassifiedCommentary = (cls.type == TrackType::Commentary && cls.confidence >= 0.80);
			const bool isHeuristicCommentary  = heuristicCommentary.contains(s.id);

			if (isClassifiedCommentary || isHeuristicCommentary) {
				const QString source = (isHeuristicCommentary && !isClassifiedCommentary)
				    ? QStringLiteral("Heuristic commentary (stereo alongside surround)")
				    : QStringLiteral("Commentary track");

				if (m_profile && !m_profile->keepCommentaryIfUnderstood()) {
					td.decision = Decision::Remove;
					td.reason   = source + QStringLiteral(" — policy: remove all commentary");
				} else if (m_profile && m_profile->keepCommentaryIfUnderstood()
				           && !isInUnderstoodLanguage(s.language)) {
					const bool langKnown = !s.language.isEmpty() && s.language != "und";
					if (langKnown) {
						td.decision = Decision::Remove;
						td.reason   = source + QStringLiteral(" in non-understood language '%1'").arg(s.language);
					}
				}
			}

			if (td.decision == Decision::Keep) {
				const bool isCommentary = isClassifiedCommentary || isHeuristicCommentary;

				// Tier 1: codec hierarchy — a higher-quality sibling in the same language
				// makes this track redundant. Commentary tracks are exempt: DD 2.0 alongside
				// TrueHD 7.1 is different content, not a redundant compatibility mix.
				if (!isCommentary && isRedundantAudio(s, audioTracks)) {
					td.decision = Decision::Remove;
					td.reason   = QStringLiteral("Redundant audio — higher-quality sibling exists for '%1'").arg(s.language);
				}
				// Tier 2: language policy
				else if (m_profile) {
					const bool isOriginal   = !file.originalLanguage.isEmpty() &&
					                          normalizeLang(s.language) == normalizeLang(file.originalLanguage) &&
					                          m_profile->alwaysKeepOriginalAudio();
					const bool isUnderstood = isInUnderstoodLanguage(s.language);
					const bool langKnown    = !s.language.isEmpty() && s.language != "und";

					if (!isOriginal && !isUnderstood && langKnown) {
						td.decision = Decision::Remove;
						td.reason   = QStringLiteral("Audio language '%1' not understood and not original").arg(s.language);
					} else if (isOriginal && isUnderstood) {
						td.reason = QStringLiteral("Original audio — understood language");
					} else if (isOriginal) {
						td.reason = QStringLiteral("Original audio language");
					} else if (isUnderstood) {
						td.reason = QStringLiteral("Understood language");
					}
				}
			}
		}
		else if (s.codecType == "subtitle") {
			if (m_profile) {
				// Classify-based overrides first
				const bool isCommentarySub = (cls.type == TrackType::Commentary && cls.confidence >= 0.80);
				const bool isSignsSub      = (cls.type == TrackType::Signs      && cls.confidence >= 0.80);

				// Signs & commentary subtitles follow same policy as regular subs
				// (they're only worth keeping if in an understood language)
				const bool isUnderstood = isInUnderstoodLanguage(s.language);
				const bool langUnknown  = s.language.isEmpty() || s.language == "und";
				// Forced subs are only protected when we actually understand the language
				// (or the language is untagged — benefit of the doubt).
				// A forced Italian sub is useless to an English-only viewer.
				// Accept both the disposition flag and a title-heuristic match.
				const bool isForcedStream = s.isForced
				                            || (cls.type == TrackType::Forced && cls.confidence >= 0.90);
				const bool isForced       = isForcedStream && m_profile->keepForcedSubtitlesAlways()
				                            && (isUnderstood || langUnknown);
				// SDH — behaviour depends on user preference mode
				const bool isSdhTrack   = s.isHearingImpaired
				                          || s.trackType == "sdh"
				                          || cls.type == TrackType::Sdh
				                          || cls.type == TrackType::HearingImpaired;
				bool isSdh = false;
				if (isUnderstood) {
					using M = UserProfile::SdhSubtitleMode;
					switch (m_profile->sdhSubtitleMode()) {
					case M::Keep:
						isSdh = isSdhTrack;
						break;
					case M::Remove:
						isSdh = false;
						break;
					case M::PreferNonSdh:
						// Protect SDH only when no regular track exists for this language
						isSdh = isSdhTrack && !langsWithNonSdhSub.contains(normalizeLang(s.language));
						break;
					case M::PreferSdh:
						isSdh = isSdhTrack;
						break;
					}
				}

				const bool isOrigSub    = !file.originalLanguage.isEmpty() &&
				                          s.language.compare(file.originalLanguage, Qt::CaseInsensitive) == 0 &&
				                          m_profile->keepOriginalLanguageSubtitle();

				// Remove or deprioritise an SDH track in an understood language.
				// (The default removal path only fires on !isUnderstood, so we need an
				// explicit check for understood-language SDH removal.)
				const bool removedSdhTrack = isUnderstood && isSdhTrack
				    && !s.isForced && !isCommentarySub && !isOrigSub
				    && langsWithNonSdhSub.contains(normalizeLang(s.language))
				    && (m_profile->sdhSubtitleMode() == UserProfile::SdhSubtitleMode::Remove
				        || m_profile->sdhSubtitleMode() == UserProfile::SdhSubtitleMode::PreferNonSdh);

				// PreferSdh mode: remove regular (non-SDH) track when SDH exists for same language
				const bool removedByPreferSdh = isUnderstood && !isSdhTrack
				    && !isCommentarySub && !s.isForced && !isOrigSub
				    && m_profile->sdhSubtitleMode() == UserProfile::SdhSubtitleMode::PreferSdh
				    && langsWithSdhSub.contains(normalizeLang(s.language));

				// Commentary subs: mirror the keepCommentaryIfUnderstood policy used for audio
				if (isCommentarySub) {
					if (!m_profile->keepCommentaryIfUnderstood()) {
						td.decision = Decision::Remove;
						td.reason   = QStringLiteral("Commentary subtitle — policy: remove all commentary");
					} else if (!isUnderstood && !langUnknown) {
						td.decision = Decision::Remove;
						td.reason   = QStringLiteral("Commentary subtitle in non-understood language '%1'").arg(s.language);
					}
				}
				else if (removedSdhTrack) {
					td.decision = Decision::Remove;
					td.reason   = (m_profile->sdhSubtitleMode() == UserProfile::SdhSubtitleMode::Remove)
					    ? QStringLiteral("SDH subtitle removed by policy for '%1'").arg(s.language)
					    : QStringLiteral("SDH subtitle superseded by regular track for '%1'").arg(s.language);
				}
				else if (removedByPreferSdh) {
					td.decision = Decision::Remove;
					td.reason   = QStringLiteral("Regular subtitle superseded by SDH track for '%1'").arg(s.language);
				}
				else if (!isForced && !isUnderstood && !isSdh && !isOrigSub) {
					td.decision = Decision::Remove;
					if (isSignsSub)
						td.reason = QStringLiteral("Signs/songs subtitle in non-understood language '%1'").arg(s.language);
					else
						td.reason = QStringLiteral("Subtitle language '%1' not in understood languages").arg(s.language);
				}

				// Format redundancy — only checked after language/SDH policy passes.
				// Removes lower-priority formats when a higher-priority one exists for the
				// same (language, SDH flag, forced flag) group.
				if (td.decision == Decision::Keep && isRedundantSubtitle(s, subtitleTracks)) {
					td.decision = Decision::Remove;
					td.reason   = QStringLiteral("Redundant subtitle format — preferred format kept for '%1'").arg(s.language);
				}

				if (td.decision == Decision::Keep) {
					if (isForced)          td.reason = QStringLiteral("Forced subtitle — kept by policy");
					else if (isOrigSub)    td.reason = QStringLiteral("Original-language subtitle — kept by policy");
					else if (isSdh)        td.reason = QStringLiteral("SDH subtitle — understood language");
					else if (isUnderstood) td.reason = QStringLiteral("Understood language");
				}
			}
		}

		fd.tracks.append(td);
	}

	return fd;
}

} // namespace Mc
