#pragma once
#include <QByteArray>
#include <QDialog>
#include <QHash>
#include <QNetworkAccessManager>

class QCloseEvent;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QNetworkReply;

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

	// When set, exec() runs the dialog invisibly (opacity 0) and auto-accepts if the
	// search returns exactly one result. Falls back to opaque+visible if ambiguous.
	void setAutoSelectSingle(bool on) { m_autoSelectSingle = on; if (on) setWindowOpacity(0.0); }

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
	QHash<int, QNetworkReply*>  m_thumbReplyByRow;   // row → in-flight thumbnail reply
	QHash<int, QNetworkReply*>  m_prefetchByRow;     // row → in-flight external_ids prefetch
	QHash<int, QString>         m_imdbIdByRow;       // row → cached IMDb ID

	QString                m_selectedTitle;
	QString                m_selectedPosterPath;
	QHash<int, QByteArray> m_thumbDataByRow;   // row → raw image bytes
	int     m_selectedYear      = 0;
	bool    m_acceptAfterFetch  = false;
	bool    m_autoSelectSingle  = false;
	bool    m_batchMode         = false;
	bool    m_userHasSearched   = false;  // true once the user manually triggers a search
};

} // namespace Mc
