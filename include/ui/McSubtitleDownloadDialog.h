#pragma once
#include <QDialog>
#include <QList>
#include <QMap>

class QTableWidget;
class QLabel;
class QPushButton;
class QThread;

namespace Mc {

class SubtitleDownloadWorker;
struct StreamRecord;

class McSubtitleDownloadDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSubtitleDownloadDialog(const QString& apiKey,
	                                   const QString& username,
	                                   const QString& password,
	                                   const QString& imdbId,
	                                   const QStringList& iso639_2Languages,
	                                   const QString& videoPath,
	                                   double durationSec,
	                                   const QStringList& editionTokens,
	                                   bool computeMovieHash,
	                                   const QList<StreamRecord>& existingStreams,
	                                   const QString& movieTitle,
	                                   QWidget* parent = nullptr);

	int downloadedCount() const { return m_downloaded; }

public slots:
	// Overridden so Cancel (or Esc / the window's close box) aborts an in-flight
	// download instead of destroying the QThread while it's still running — the
	// dialog actually closes once onAllDone confirms the worker has stopped.
	void reject() override;

signals:
	void downloadComplete(int downloaded);

private slots:
	void onDownload();
	void onLanguageStarted(const QString& lang6391);
	void onLanguageDone(const QString& lang6391, bool success, const QString& message);
	void onAllDone(int downloaded, int failed, const QString& statusMsg, int remaining,
	               bool quotaExceeded, int retryAfterSecs);
	void onStatusCellClicked(int row, int column);
	void onReferenceLookupStarted();
	void onReferenceLookupDone(bool found, const QString& label);

private:
	static QString languageDisplayName(const QString& iso6392);

	QTableWidget* m_table       = nullptr;
	QLabel*       m_statusLabel = nullptr;
	QPushButton*  m_downloadBtn = nullptr;
	QPushButton*  m_closeBtn    = nullptr;

	QString     m_apiKey, m_username, m_password;
	QString     m_imdbId;
	QStringList m_iso6392Languages;
	QString     m_videoPath;
	double      m_durationSec = 0.0;
	QStringList m_editionTokens;
	bool        m_computeMovieHash = false;
	QList<StreamRecord> m_existingStreams;

	QMap<QString, int>         m_rowByLang6391;
	QThread*                   m_thread = nullptr;
	SubtitleDownloadWorker*    m_worker = nullptr;
	int                        m_downloaded = 0;
	bool                       m_downloading    = false;
	bool                       m_closeRequested = false;
};

} // namespace Mc
