#pragma once

#include <QObject>
#include <QStringList>
#include <QJsonObject>

namespace Mc {

/**
 * UserProfile — the user's curation policy.
 * Serialized as JSON and stored in the preferences table.
 */
class UserProfile : public QObject
{
	Q_OBJECT
public:
	explicit UserProfile(QObject* parent = nullptr);

	// Languages the user understands (ISO 639-2, e.g. "dan", "eng")
	QStringList understoodLanguages() const { return m_understoodLanguages; }
	void setUnderstoodLanguages(const QStringList& langs);

	// Always keep audio in the file's detected original language,
	// even if it's not in understoodLanguages. Prevents dubbing removal.
	bool alwaysKeepOriginalAudio() const { return m_alwaysKeepOriginalAudio; }
	void setAlwaysKeepOriginalAudio(bool v);

	// Keep commentary tracks if they are in an understood language
	bool keepCommentaryIfUnderstood() const { return m_keepCommentaryIfUnderstood; }
	void setKeepCommentaryIfUnderstood(bool v);

	// Skip proposing jobs where the only removals are subtitle tracks
	// (subtitle removal barely affects file size and remux time is the same)
	bool skipSubtitleOnlyJobs() const { return m_skipSubtitleOnlyJobs; }
	void setSkipSubtitleOnlyJobs(bool v);

	// Treat stereo (≤2ch) audio tracks as commentary when a surround (≥6ch) track exists
	// in the same language — catches commentary tracks without proper title/flag metadata
	bool stereoAsCommentaryHeuristic() const { return m_stereoAsCommentaryHeuristic; }
	void setStereoAsCommentaryHeuristic(bool v);

	// Keep forced subtitle tracks regardless of language
	bool keepForcedSubtitlesAlways() const { return m_keepForcedSubtitlesAlways; }
	void setKeepForcedSubtitlesAlways(bool v);

	// SDH / hearing-impaired subtitle preference
	enum class SdhSubtitleMode {
		Keep,         // Always keep SDH/HI tracks
		Remove,       // Always remove SDH/HI tracks
		PreferNonSdh, // Remove SDH when a regular track exists for same language (default)
		PreferSdh,    // Remove regular when an SDH track exists (deaf-viewer mode)
	};
	SdhSubtitleMode sdhSubtitleMode() const { return m_sdhSubtitleMode; }
	void setSdhSubtitleMode(SdhSubtitleMode mode);

	// Keep subtitles for original language even if not understood
	bool keepOriginalLanguageSubtitle() const { return m_keepOriginalLanguageSub; }
	void setKeepOriginalLanguageSubtitle(bool v);

	// Merge sidecar (external) subtitle files into the container whenever a remux
	// job runs anyway. When disabled, sidecar subtitles are left untouched on disk.
	bool mergeSidecarSubtitles() const { return m_mergeSidecarSubtitles; }
	void setMergeSidecarSubtitles(bool v);

	// Remove MJPEG video streams (embedded cover-art/thumbnail) from output
	bool removeMjpegCoverArt() const { return m_removeMjpegCoverArt; }
	void setRemoveMjpegCoverArt(bool v);

	// Write a .mc-log file alongside each processed file after remux succeeds
	bool writeJobLog() const { return m_writeJobLog; }
	void setWriteJobLog(bool v);

	// TMDB API key for poster/metadata enrichment (empty = feature disabled)
	QString tmdbApiKey() const { return m_tmdbApiKey; }
	void setTmdbApiKey(const QString& key);

	// OpenSubtitles.com credentials (opensubtitles.com REST API v1)
	QString openSubtitlesApiKey()  const { return m_openSubtitlesApiKey; }
	QString openSubtitlesUsername() const { return m_openSubtitlesUsername; }
	QString openSubtitlesPassword() const { return m_openSubtitlesPassword; }
	void setOpenSubtitlesApiKey(const QString& key);
	void setOpenSubtitlesUsername(const QString& username);
	void setOpenSubtitlesPassword(const QString& password);

	// Audio format priority — ordered list of format IDs, best first.
	// Each ID corresponds to a specific codec variant (e.g. "atmos", "dtshdma").
	// When multiple tracks of the same language exist, the highest-priority one is kept.
	QStringList audioFormatOrder() const { return m_audioFormatOrder; }
	void setAudioFormatOrder(const QStringList& order);

	// Format IDs the user does not want to keep.
	// A disabled track is removed when any enabled alternative exists in the same language.
	QStringList disabledAudioFormats() const { return m_disabledAudioFormats; }
	void setDisabledAudioFormats(const QStringList& ids);

	// All known format IDs in default priority order (best first).
	static QStringList defaultAudioFormatOrder();
	// Human-readable name for a format ID (e.g. "dtshdma" → "DTS-HD MA").
	static QString audioFormatDisplayName(const QString& id);

	// Subtitle format priority — ordered list of format IDs, best first.
	// When multiple formats exist for the same language + SDH/forced group, the
	// highest-priority one is kept and lower-priority ones are removed.
	QStringList subtitleFormatOrder() const { return m_subtitleFormatOrder; }
	void setSubtitleFormatOrder(const QStringList& order);

	// Subtitle format IDs the user does not want to keep.
	// A disabled format is removed when any enabled alternative exists in the same group.
	QStringList disabledSubtitleFormats() const { return m_disabledSubtitleFormats; }
	void setDisabledSubtitleFormats(const QStringList& ids);

	static QStringList defaultSubtitleFormatOrder();
	static QString subtitleFormatDisplayName(const QString& id);

	// Serialization
	[[nodiscard]] QJsonObject toJson() const;
	bool fromJson(const QJsonObject& json);

	void save();    // persist to DatabaseManager::preferences
	bool load();    // load from DatabaseManager::preferences

signals:
	void profileChanged();

private:
	QStringList m_understoodLanguages     = {"eng"};
	bool        m_alwaysKeepOriginalAudio = true;
	bool        m_keepCommentaryIfUnderstood = true;
	bool        m_skipSubtitleOnlyJobs = false;
	bool        m_stereoAsCommentaryHeuristic = true;
	bool        m_keepForcedSubtitlesAlways  = true;
	SdhSubtitleMode m_sdhSubtitleMode        = SdhSubtitleMode::PreferNonSdh;
	bool        m_keepOriginalLanguageSub    = false;
	bool        m_mergeSidecarSubtitles     = true;
	bool        m_removeMjpegCoverArt       = true;
	bool        m_writeJobLog               = false;
	QString     m_tmdbApiKey;
	QString     m_openSubtitlesApiKey;
	QString     m_openSubtitlesUsername;
	QString     m_openSubtitlesPassword;
	QStringList m_audioFormatOrder      = defaultAudioFormatOrder();
	QStringList m_disabledAudioFormats  = {};
	QStringList m_subtitleFormatOrder   = defaultSubtitleFormatOrder();
	QStringList m_disabledSubtitleFormats = {};
};

} // namespace Mc
