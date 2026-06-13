#include "core/UserProfile.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace Mc {

static const char* PREF_KEY = "user_profile_json";   // legacy DB key — read once for migration

UserProfile::UserProfile(QObject* parent)
	: QObject(parent)
{}

void UserProfile::setUnderstoodLanguages(const QStringList& langs)
{
	if (m_understoodLanguages != langs) {
		m_understoodLanguages = langs;
		emit profileChanged();
	}
}

void UserProfile::setAlwaysKeepOriginalAudio(bool v)
{
	if (m_alwaysKeepOriginalAudio != v) {
		m_alwaysKeepOriginalAudio = v;
		emit profileChanged();
	}
}

void UserProfile::setKeepCommentaryIfUnderstood(bool v)
{
	if (m_keepCommentaryIfUnderstood != v) {
		m_keepCommentaryIfUnderstood = v;
		emit profileChanged();
	}
}

void UserProfile::setSkipSubtitleOnlyJobs(bool v)
{
	if (m_skipSubtitleOnlyJobs != v) {
		m_skipSubtitleOnlyJobs = v;
		emit profileChanged();
	}
}

void UserProfile::setStereoAsCommentaryHeuristic(bool v)
{
	if (m_stereoAsCommentaryHeuristic != v) {
		m_stereoAsCommentaryHeuristic = v;
		emit profileChanged();
	}
}

void UserProfile::setKeepForcedSubtitlesAlways(bool v)
{
	if (m_keepForcedSubtitlesAlways != v) {
		m_keepForcedSubtitlesAlways = v;
		emit profileChanged();
	}
}

void UserProfile::setSdhSubtitleMode(SdhSubtitleMode mode)
{
	if (m_sdhSubtitleMode != mode) {
		m_sdhSubtitleMode = mode;
		emit profileChanged();
	}
}

static QString sdhModeToString(UserProfile::SdhSubtitleMode mode)
{
	using M = UserProfile::SdhSubtitleMode;
	switch (mode) {
	case M::Keep:         return QStringLiteral("keep");
	case M::Remove:       return QStringLiteral("remove");
	case M::PreferSdh:    return QStringLiteral("prefer_sdh");
	case M::PreferNonSdh: return QStringLiteral("prefer_non_sdh");
	}
	return QStringLiteral("prefer_non_sdh");
}

static UserProfile::SdhSubtitleMode sdhModeFromString(const QString& s)
{
	if (s == QStringLiteral("keep"))       return UserProfile::SdhSubtitleMode::Keep;
	if (s == QStringLiteral("remove"))     return UserProfile::SdhSubtitleMode::Remove;
	if (s == QStringLiteral("prefer_sdh")) return UserProfile::SdhSubtitleMode::PreferSdh;
	return UserProfile::SdhSubtitleMode::PreferNonSdh;
}

void UserProfile::setKeepOriginalLanguageSubtitle(bool v)
{
	if (m_keepOriginalLanguageSub != v) {
		m_keepOriginalLanguageSub = v;
		emit profileChanged();
	}
}

void UserProfile::setRemoveMjpegCoverArt(bool v)
{
	if (m_removeMjpegCoverArt != v) {
		m_removeMjpegCoverArt = v;
		emit profileChanged();
	}
}

void UserProfile::setWriteJobLog(bool v)
{
	if (m_writeJobLog != v) {
		m_writeJobLog = v;
		emit profileChanged();
	}
}

void UserProfile::setTmdbApiKey(const QString& key)
{
	if (m_tmdbApiKey != key) {
		m_tmdbApiKey = key;
		emit profileChanged();
	}
}

void UserProfile::setOpenSubtitlesApiKey(const QString& key)
{
	if (m_openSubtitlesApiKey != key) {
		m_openSubtitlesApiKey = key;
		emit profileChanged();
	}
}

void UserProfile::setOpenSubtitlesUsername(const QString& username)
{
	if (m_openSubtitlesUsername != username) {
		m_openSubtitlesUsername = username;
		emit profileChanged();
	}
}

void UserProfile::setOpenSubtitlesPassword(const QString& password)
{
	if (m_openSubtitlesPassword != password) {
		m_openSubtitlesPassword = password;
		emit profileChanged();
	}
}

void UserProfile::setAudioFormatOrder(const QStringList& order)
{
	if (m_audioFormatOrder != order) {
		m_audioFormatOrder = order;
		emit profileChanged();
	}
}

void UserProfile::setDisabledAudioFormats(const QStringList& ids)
{
	if (m_disabledAudioFormats != ids) {
		m_disabledAudioFormats = ids;
		emit profileChanged();
	}
}

void UserProfile::setSubtitleFormatOrder(const QStringList& order)
{
	if (m_subtitleFormatOrder != order) {
		m_subtitleFormatOrder = order;
		emit profileChanged();
	}
}

void UserProfile::setDisabledSubtitleFormats(const QStringList& ids)
{
	if (m_disabledSubtitleFormats != ids) {
		m_disabledSubtitleFormats = ids;
		emit profileChanged();
	}
}

// static
QStringList UserProfile::defaultSubtitleFormatOrder()
{
	// Text formats first: player-rendered with TTF, scalable to any display resolution.
	// Image formats last: fixed-resolution bitmaps (PGS = 1080p BD, VobSub = 480p DVD).
	return {"srt", "ass", "vtt", "pgs", "vobsub"};
}

// static
QString UserProfile::subtitleFormatDisplayName(const QString& id)
{
	static const QHash<QString, QString> names = {
		{"pgs",    "PGS (Blu-ray image)"},
		{"vobsub", "VobSub (DVD image)"},
		{"ass",    "ASS / SSA (styled text)"},
		{"srt",    "SRT (plain text)"},
		{"vtt",    "WebVTT"},
	};
	return names.value(id, id);
}

// static
QStringList UserProfile::defaultAudioFormatOrder()
{
	return {"atmos", "truehd", "dtsx", "dtshdma", "dtshd_hra",
			"flac", "eac3", "dts", "ac3", "aac", "mp3"};
}

// static
QString UserProfile::audioFormatDisplayName(const QString& id)
{
	static const QHash<QString, QString> names = {
		{"atmos",     "Dolby Atmos"},
		{"truehd",    "TrueHD"},
		{"dtsx",      "DTS:X"},
		{"dtshdma",   "DTS-HD MA"},
		{"dtshd_hra", "DTS-HD HRA"},
		{"flac",      "FLAC / PCM"},
		{"eac3",      "DD+"},
		{"dts",       "DTS"},
		{"ac3",       "DD (AC3)"},
		{"aac",       "AAC"},
		{"mp3",       "MP3"},
	};
	return names.value(id, id);
}

// ── Serialization ─────────────────────────────────────────────────────────────

QJsonObject UserProfile::toJson() const
{
	QJsonObject o;
	o["understood_languages"]           = QJsonArray::fromStringList(m_understoodLanguages);
	o["always_keep_original_audio"]     = m_alwaysKeepOriginalAudio;
	o["keep_commentary_if_understood"]    = m_keepCommentaryIfUnderstood;
	o["skip_subtitle_only_jobs"]          = m_skipSubtitleOnlyJobs;
	o["stereo_as_commentary_heuristic"]   = m_stereoAsCommentaryHeuristic;
	o["keep_forced_subtitles_always"]   = m_keepForcedSubtitlesAlways;
	o["sdh_subtitle_mode"]              = sdhModeToString(m_sdhSubtitleMode);
	o["keep_original_language_subtitle"]= m_keepOriginalLanguageSub;
	o["remove_mjpeg_cover_art"]         = m_removeMjpegCoverArt;
	o["audio_format_order"]         = QJsonArray::fromStringList(m_audioFormatOrder);
	o["disabled_audio_formats"]     = QJsonArray::fromStringList(m_disabledAudioFormats);
	o["subtitle_format_order"]      = QJsonArray::fromStringList(m_subtitleFormatOrder);
	o["disabled_subtitle_formats"]  = QJsonArray::fromStringList(m_disabledSubtitleFormats);
	o["write_job_log"]                  = m_writeJobLog;
	o["tmdb_api_key"]                   = m_tmdbApiKey;
	o["opensubtitles_api_key"]          = m_openSubtitlesApiKey;
	o["opensubtitles_username"]         = m_openSubtitlesUsername;
	o["opensubtitles_password"]         = m_openSubtitlesPassword;
	return o;
}

bool UserProfile::fromJson(const QJsonObject& json)
{
	if (json.isEmpty()) return false;

	if (json.contains("understood_languages")) {
		QStringList langs;
		for (const auto& v : json["understood_languages"].toArray())
			langs << v.toString();
		m_understoodLanguages = langs;
	}
	m_alwaysKeepOriginalAudio    = json["always_keep_original_audio"].toBool(true);
	m_keepCommentaryIfUnderstood      = json["keep_commentary_if_understood"].toBool(true);
	m_skipSubtitleOnlyJobs            = json["skip_subtitle_only_jobs"].toBool(false);
	m_stereoAsCommentaryHeuristic     = json["stereo_as_commentary_heuristic"].toBool(true);
	m_keepForcedSubtitlesAlways  = json["keep_forced_subtitles_always"].toBool(true);
	if (json.contains("sdh_subtitle_mode")) {
		m_sdhSubtitleMode = sdhModeFromString(json["sdh_subtitle_mode"].toString());
	} else if (json.contains("keep_sdh_subtitles")) {
		// Backward compat: old bool → enum
		m_sdhSubtitleMode = json["keep_sdh_subtitles"].toBool(true)
			? SdhSubtitleMode::Keep
			: SdhSubtitleMode::Remove;
	} else {
		m_sdhSubtitleMode = SdhSubtitleMode::PreferNonSdh;
	}
	m_keepOriginalLanguageSub    = json["keep_original_language_subtitle"].toBool(false);
	m_removeMjpegCoverArt        = json["remove_mjpeg_cover_art"].toBool(true);
	m_writeJobLog                = json["write_job_log"].toBool(false);
	m_tmdbApiKey                 = json["tmdb_api_key"].toString();
	m_openSubtitlesApiKey        = json["opensubtitles_api_key"].toString();
	m_openSubtitlesUsername      = json["opensubtitles_username"].toString();
	m_openSubtitlesPassword      = json["opensubtitles_password"].toString();

	if (json.contains("audio_format_order")) {
		QStringList order;
		for (const auto& v : json["audio_format_order"].toArray())
			order << v.toString();
		// Ensure any new formats added in later versions are appended at end
		for (const QString& id : defaultAudioFormatOrder())
			if (!order.contains(id)) order.append(id);
		m_audioFormatOrder = order;
	}
	if (json.contains("disabled_audio_formats")) {
		QStringList ids;
		for (const auto& v : json["disabled_audio_formats"].toArray())
			ids << v.toString();
		m_disabledAudioFormats = ids;
	}
	if (json.contains("subtitle_format_order")) {
		QStringList order;
		for (const auto& v : json["subtitle_format_order"].toArray())
			order << v.toString();
		for (const QString& id : defaultSubtitleFormatOrder())
			if (!order.contains(id)) order.append(id);
		m_subtitleFormatOrder = order;
	}
	if (json.contains("disabled_subtitle_formats")) {
		QStringList ids;
		for (const auto& v : json["disabled_subtitle_formats"].toArray())
			ids << v.toString();
		m_disabledSubtitleFormats = ids;
	}
	return true;
}

void UserProfile::save()
{
	AppSettings::instance().setProfileSection(toJson());
}

bool UserProfile::load()
{
	// Primary: settings.json "profile" section (new format).
	const QJsonObject profileJson = AppSettings::instance().profileSection();
	if (!profileJson.isEmpty() && fromJson(profileJson))
		return true;

	// Migration: old settings.json was a flat UserProfile object (pre-AppSettings
	// restructure). AppSettings already read the file; if the root contains a known
	// UserProfile key it's the old format — migrate it in place.
	const QJsonObject raw = AppSettings::instance().rawRoot();
	if (raw.contains(QStringLiteral("understood_languages"))) {
		if (fromJson(raw)) {
			save();   // re-saves under the "profile" key in new format
			return true;
		}
	}

	// Legacy: old DB-based storage (earliest format, before JSON file).
	const QString stored = DatabaseManager::instance().getPref(PREF_KEY);
	if (!stored.isEmpty()) {
		const QJsonDocument doc = QJsonDocument::fromJson(stored.toUtf8());
		if (!doc.isNull() && fromJson(doc.object())) {
			save();
			return true;
		}
	}

	return false;
}

} // namespace Mc
