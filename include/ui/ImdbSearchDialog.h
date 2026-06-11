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
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QNetworkReply;
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
	                  QWidget* parent = nullptr);
	~ImdbSearchDialog() override;

	QString    selectedImdbId()           const;
	QString    selectedTitle()            const;
	int        selectedYear()             const;
	QString    selectedPosterPath()       const;
	QByteArray selectedImageData()        const;
	QString    selectedOriginalLanguage() const;  // ISO 639-1 from TMDB; empty if not available
	double     selectedVoteAverage()      const;
	int        selectedVoteCount()        const;

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

protected:
	void done(int result) override;
	void closeEvent(QCloseEvent* event) override;
	void keyPressEvent(QKeyEvent* event) override;
	bool eventFilter(QObject* obj, QEvent* ev) override;

private:
	void setStatusText(const QString& text, bool isError = false);

	QString m_videoPath;
	QString m_tmdbApiKey;

	QLineEdit*        m_searchEdit   = nullptr;
	QPushButton*      m_btnSearch    = nullptr;
	QLabel*           m_statusLabel  = nullptr;
	QListWidget*      m_resultsList  = nullptr;
	QLineEdit*        m_imdbIdEdit   = nullptr;
	QPushButton*      m_btnSave      = nullptr;
	QDialogButtonBox* m_btnBox       = nullptr;

	QNetworkAccessManager*      m_nam = nullptr;
	QNetworkReply*              m_searchReply  = nullptr;
	QNetworkReply*              m_extIdsReply  = nullptr;
	QHash<int, QNetworkReply*>  m_thumbReplyByRow;
	QHash<int, QNetworkReply*>  m_prefetchByRow;
	QHash<int, QString>         m_imdbIdByRow;

	// Poster gallery
	QSplitter*    m_splitter         = nullptr;
	QComboBox*    m_langFilter       = nullptr;
	QListWidget*  m_posterGallery    = nullptr;
	QLabel*       m_galleryStatus    = nullptr;

	QNetworkReply*                 m_imagesReply = nullptr;
	QHash<QNetworkReply*, QString> m_galleryThumbReplies;
	QHash<QString, QByteArray>     m_galleryThumbData;
	QList<QJsonObject>             m_allPosters;
	QByteArray                     m_galleryImageData;
	QString                        m_galleryFilter;

	void fetchPosterImages(int tmdbId, const QString& origLang);
	void populateGallery();

	QStringList            m_understoodLanguages;
	QString                m_selectedTitle;
	QString                m_selectedPosterPath;
	QHash<int, QByteArray> m_thumbDataByRow;
	int     m_selectedYear      = 0;
	bool    m_acceptAfterFetch  = false;
	bool    m_autoSelectSingle  = false;
	bool    m_batchMode         = false;
	bool    m_userHasSearched   = false;
};

} // namespace Mc
