#pragma once
#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>

class QCloseEvent;
class QComboBox;
class QDialogButtonBox;
class QJsonArray;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QNetworkReply;
class QRegularExpressionMatch;
class QSplitter;

namespace Mc {

class ImdbSearchDialog : public QDialog {
	Q_OBJECT
public:
	// Custom done() codes used in batch mode.
	static constexpr int SkipFile    = 2;  // skip this file, continue batch
	static constexpr int CancelBatch = 3;  // abort the entire batch loop

	ImdbSearchDialog(const QString& videoPath,
	                  const QString& suggestedTitle,
	                  const QString& existingImdbId,
	                  const QString& tmdbApiKey,
	                  QWidget* parent = nullptr,
	                  const QString& existingPosterPath = {},
	                  const QString& existingFanartPath = {});
	~ImdbSearchDialog() override;

	QString    selectedImdbId()           const;
	QString    selectedTitle()            const;
	int        selectedYear()             const;
	QString    selectedPosterPath()       const;
	QByteArray selectedImageData()        const;
	QString    selectedFanartPath()       const;  // TMDB backdrop path; empty if none selected
	QString    selectedOriginalLanguage() const;  // ISO 639-1 from TMDB; empty if not available
	double     selectedVoteAverage()      const;
	int        selectedVoteCount()        const;
	int        selectedTmdbId()           const;  // TMDB movie/tv id; 0 if none selected
	QString    selectedOriginalTitle()    const;  // TMDB original_title / original_name
	QString    selectedReleaseDate()      const;  // release_date / first_air_date, YYYY-MM-DD
	// MediaTypes::* value derived from the selected result (movie/tv/documentary).
	// Empty if the user only typed an IMDb id without a TMDB result.
	QString    selectedMediaType()        const;

	// When set, auto-accepts if the search returns exactly one result with a valid
	// IMDb ID. Shows the dialog normally if the result is ambiguous or the fetch fails.
	void setAutoSelectSingle(bool on) { m_autoSelectSingle = on; }

	// ISO 639-2 codes (e.g. "eng", "dan") — used to request poster images in
	// languages the user understands. Call before exec().
	void setUnderstoodLanguages(const QStringList& langs) { m_understoodLanguages = langs; }

	// Adds "Skip" and "Cancel all" buttons and shows progress in the title.
	// Call before exec(). current/total are 1-based (e.g. "3 of 10").
	void setBatchMode(int current, int total);

private slots:
	void onSearch();
	void onResultSelectionChanged();

private:
	// Looks the title up directly by its known IMDb ID via TMDB's /find endpoint
	// (movie_results + tv_results). Preferred over a title search whenever an ID
	// is already on file, since a folder-name search can match the wrong entry.
	void searchByExistingImdbId();

	// Cancels the in-flight search/thumbnail/prefetch requests safely: handlers
	// are detached before abort() so their synchronous finished() can't mutate
	// the reply hashes mid-iteration.
	void abortResultRequests();
	// results items may include an injected "_mc_media_type" string ("movie"/"tv")
	// when sourced from /find or multi-search.
	void populateSearchResults(const QJsonArray& results, const QRegularExpressionMatch& yearMatch);

protected:
	void done(int result) override;
	void closeEvent(QCloseEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	bool eventFilter(QObject* obj, QEvent* ev) override;

private:
	QString m_videoPath;
	QString m_tmdbApiKey;

	QLineEdit*        m_searchEdit   = nullptr;
	QPushButton*      m_btnSearch    = nullptr;
	QListWidget*      m_resultsList  = nullptr;
	QLineEdit*        m_imdbIdEdit   = nullptr;
	QPushButton*      m_btnSave      = nullptr;
	QDialogButtonBox* m_btnBox       = nullptr;

	QNetworkAccessManager*      m_nam = nullptr;
	QNetworkReply*              m_searchReply  = nullptr;
	QNetworkReply*              m_extIdsReply  = nullptr;
	QHash<int, QNetworkReply*>  m_thumbReplyByRow;
	QHash<int, QNetworkReply*>  m_backdropReplyByRow;
	QHash<int, QNetworkReply*>  m_prefetchByRow;
	QHash<int, QString>         m_imdbIdByRow;

	// Poster gallery
	QSplitter*    m_splitter         = nullptr;
	QSplitter*    m_gallerySplitter  = nullptr;
	QComboBox*    m_langFilter       = nullptr;
	QListWidget*  m_posterGallery    = nullptr;

	// Fanart gallery
	QListWidget*  m_fanartGallery    = nullptr;

	QNetworkReply*                 m_imagesReply = nullptr;
	QHash<QNetworkReply*, QString> m_galleryThumbReplies;
	QHash<QString, QByteArray>     m_galleryThumbData;
	QList<QJsonObject>             m_allPosters;
	QByteArray                     m_galleryImageData;
	QString                        m_galleryFilter;

	QHash<QNetworkReply*, QString> m_fanartThumbReplies;
	QHash<QString, QByteArray>     m_fanartThumbData;
	QList<QJsonObject>             m_allBackdrops;
	QString                        m_selectedFanartPath;

	// kind: "movie" or "tv" — selects the TMDB images endpoint.
	void fetchPosterImages(int tmdbId, const QString& origLang, const QString& kind = QStringLiteral("movie"));
	void populateGallery();
	void populateFanartGallery();

	QStringList            m_understoodLanguages;
	QString                m_selectedTitle;
	QString                m_selectedPosterPath;
	QString                m_selectedMediaType;  // MediaTypes::* from last selection
	QHash<int, QByteArray> m_thumbDataByRow;
	QString                m_existingImdbId;
	QString                m_existingPosterPath;
	QString                m_existingFanartPath;
	int     m_selectedYear        = 0;
	bool    m_acceptAfterFetch    = false;
	bool    m_autoSelectSingle    = false;
	bool    m_batchMode           = false;
	bool    m_userHasSearched     = false;
	bool    m_userSelectedPoster  = false;
	bool    m_userSelectedFanart  = false;
};

} // namespace Mc
