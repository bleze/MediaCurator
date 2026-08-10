#pragma once
#include <QDialog>
#include <QPointer>
#include <QStringList>

class QLabel;
class QListWidget;
class QPushButton;
class QThread;

namespace Mc {

class SceneNfoDownloadWorker;

// Explicit, per-file "Download Scene NFO…" flow (srrDB lookup) — see
// UserProfile::downloadSceneNfoEnabled(). Mirrors McSubtitleDownloadDialog's
// QThread+worker shape, but with an extra middle state: srrDB search can come
// back ambiguous (a file renamed by Radarr/Sonarr/Plex naming won't match its
// original scene release name), in which case the dialog shows a picker
// before the download phase can proceed.
class McSceneNfoDownloadDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSceneNfoDownloadDialog(const QString& videoPath, const QString& imdbId,
	                                   const QString& movieTitle, QWidget* parent = nullptr);
	~McSceneNfoDownloadDialog() override;

public slots:
	// Overridden so Cancel (or Esc / the window's close box) aborts an
	// in-flight search/download instead of closing out from under it — the
	// dialog actually closes once onDone() confirms the worker has stopped.
	void reject() override;

signals:
	// rawContent is the exact bytes downloaded from srrDB, undecoded — the
	// caller does both the DB write (via NfoParser::decodeSceneNfoBytes) and,
	// if enabled, the disk write (NfoParser::writeSceneNfoFile) itself.
	void downloadSucceeded(QByteArray rawContent);

private slots:
	void onCandidatesFound(QStringList labels, QStringList names);
	void onReleaseSelected(const QString& releaseName);
	void onDone(bool success, const QString& releaseName, const QByteArray& rawContent,
	            const QString& errorMessage);
	void onPickClicked();

private:
	void startSearch();

	QLabel*      m_statusLabel   = nullptr;
	QListWidget* m_candidateList = nullptr;
	QPushButton* m_pickBtn       = nullptr;
	QPushButton* m_closeBtn      = nullptr;

	QString m_videoPath;
	QString m_imdbId;

	// QPointer, not a raw pointer: the worker's QThread self-deletes via
	// connect(m_thread, finished, m_thread, deleteLater) once the download
	// completes, and the dialog can outlive that (e.g. left open on its final
	// "Downloaded ..." status text). A raw pointer would go dangling at that
	// point — this destructor's own isRunning() guard is exactly the bug that
	// hung a live MediaCurator.DMP capture in McSubtitleDownloadDialog before
	// the same fix was applied there.
	QPointer<QThread>                m_thread;
	QPointer<SceneNfoDownloadWorker> m_worker;

	QStringList m_candidateNames; // parallel to m_candidateList's rows
	bool m_finished       = false;
	bool m_closeRequested = false;
};

} // namespace Mc
