#pragma once
#include <QDialog>
#include <QMap>

class QTableWidget;
class QPushButton;
class QThread;

namespace Mc {

class SubtitleDownloadWorker;

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
	                                   QWidget* parent = nullptr);

	int downloadedCount() const { return m_downloaded; }

signals:
	void downloadComplete(int downloaded);

private slots:
	void onDownload();
	void onLanguageStarted(const QString& lang6391);
	void onLanguageDone(const QString& lang6391, bool success, const QString& message);
	void onAllDone(int downloaded, int failed, const QString& statusMsg);

private:
	static QString languageDisplayName(const QString& iso6392);

	QTableWidget* m_table       = nullptr;
	QPushButton*  m_downloadBtn = nullptr;
	QPushButton*  m_closeBtn    = nullptr;

	QString     m_apiKey, m_username, m_password;
	QString     m_imdbId;
	QStringList m_iso6392Languages;
	QString     m_videoPath;

	QMap<QString, int>         m_rowByLang6391;
	QThread*                   m_thread = nullptr;
	SubtitleDownloadWorker*    m_worker = nullptr;
	int                        m_downloaded = 0;
};

} // namespace Mc
