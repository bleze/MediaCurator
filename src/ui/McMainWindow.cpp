#include "ui/McMainWindow.h"
#include "core/AppSettings.h"
#include "ui/ImdbSearchDialog.h"
#include "ui/McFilterPanel.h"
#include "ui/McHighscoreBand.h"
#include "ui/McHighscoreDialog.h"
#include "ui/McManageFoldersDialog.h"
#include "ui/SvgIcon.h"
#include "ui/McCardDelegate.h"
#include "ui/McFileCardDelegate.h"
#include "ui/McFileListModel.h"
#include "ui/McJobListModel.h"
#include "ui/McLanguageFlags.h"
#include "ui/McTrackContextMenu.h"
#include "ui/McJobPanel.h"
#include "ui/McLegendDialog.h"
#include "ui/McOnboardingDialog.h"
#include "ui/McPreviewDialog.h"
#include "ui/McJobReviewDialog.h"
#include "ui/McSettingsDialog.h"
#include "engine/ActionEngine.h"
#include "engine/AnalyzeWorker.h"
#include "engine/TrackFlagService.h"
#include "engine/HighscoreClient.h"
#include "engine/SimulateWorker.h"
#include "ui/McWhatIfDialog.h"
#include "engine/JobQueue.h"
#include "engine/OpenSubtitlesClient.h"
#include "ui/McSubtitleDownloadDialog.h"
#include "ui/McCalibrationDialog.h"
#include "engine/PosterManager.h"
#include "engine/SubtitleManager.h"
#include "engine/RuleEngine.h"
#include "engine/TrackDecision.h"
#include "scanner/NfoParser.h"
#include "scanner/OriginalLanguageDetector.h"
#include "scanner/ScanWorker.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"
#include "core/LibraryLoader.h"
#include "core/StorageGroupSettings.h"
#include "core/UpdateChecker.h"
#include "core/UserProfile.h"

#ifdef Q_OS_WIN
#include <shobjidl.h>
#include <windows.h>
#endif

#include <QAbstractItemView>
#include <QComboBox>
#include <QDateTime>
#include <QLocale>
#include <QShowEvent>
#include <QPaintEvent>
#include <QSplashScreen>
#include <QTimer>
#include <functional>
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QClipboard>
#include <QIcon>
#include <QLineEdit>
#include <QCursor>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProcess>
#include <QProgressBar>
#include <QGuiApplication>
#include <QWindow>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStatusBar>
#include <QColor>
#include <QDebug>
#include <QPainter>
#include <QPalette>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>
#include <QVBoxLayout>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QSvgRenderer>

namespace {

// Best title to pre-fill the TMDB search dialog for a given file.
// If the file sits alone in its folder the folder name is almost always cleaner
// than the encoded filename.  When multiple movies share a folder, prefer the
// title embedded in the container tags; fall back to parsing the filename.
static QString smartSuggestedTitle(const Mc::FileRecord& file)
{
	const QDir folder = QFileInfo(file.path).dir();
	static const QStringList kVideoExts{
	    "*.mkv","*.mp4","*.avi","*.m4v","*.mov","*.wmv","*.ts","*.m2ts","*.mpg","*.mpeg"};
	const int videoCount = folder.entryList(kVideoExts, QDir::Files).count();
	if (videoCount == 1)
		return folder.dirName();
	if (!file.containerTitle.isEmpty()) {
		const QString stemmed = QFileInfo(file.path).completeBaseName();
		if (file.containerTitle.compare(stemmed, Qt::CaseInsensitive) != 0)
			return file.containerTitle;
	}
	return Mc::NfoParser::titleFromFilename(file.filename);
}

// Short card-style label for the status bar while a job is running.
static QString jobStatusLabel(const Mc::JobDisplayRecord& rec)
{
	if (!rec.displayTitle.isEmpty()) {
		return rec.displayYear > 0
		    ? QStringLiteral("%1 (%2)").arg(rec.displayTitle).arg(rec.displayYear)
		    : rec.displayTitle;
	}
	const QString stemmed = QFileInfo(rec.filePath).completeBaseName();
	if (!rec.containerTitle.isEmpty()
	    && rec.containerTitle.compare(stemmed, Qt::CaseInsensitive) != 0)
		return rec.containerTitle;
	const QString folder = QFileInfo(rec.filePath).dir().dirName();
	return folder.isEmpty() ? rec.filename : folder;
}

// Streams that will actually remain on disk once already-decided removals are applied —
// a pending job's kept-tracks list if one exists, otherwise a library-level forced
// removal that hasn't been turned into a job yet. Subtitle coverage checks must run
// against this set, not the raw file contents, otherwise a track the user has toggled
// off (e.g. an unwanted PGS CC) still counts as covering that language and blocks
// downloading a replacement.
static QList<Mc::StreamRecord> streamsRemainingForFile(qint64 fileId,
                                                        const QList<Mc::StreamRecord>& allStreams,
                                                        Mc::McFileListModel* listModel)
{
	if (const auto job = Mc::DatabaseManager::instance().activeJobForFile(fileId))
		return Mc::McJobListModel::computeKeptStreams(allStreams, job->commandArgsJson);

	const QSet<int> forcedRemovals = listModel->forcedRemovalsFor(fileId);
	if (forcedRemovals.isEmpty())
		return allStreams;

	QList<Mc::StreamRecord> kept;
	for (const auto& s : allStreams)
		if (!forcedRemovals.contains(s.streamIndex))
			kept << s;
	return kept;
}


// Convert TMDB's ISO 639-1 two-letter code to the ISO 639-2/T three-letter code
// used in media file stream tags.  Falls back to the input if unknown.
static QString tmdbLangToIso6392(const QString& iso1)
{
	static const QHash<QString, QString> m = {
		{"en","eng"}, {"fr","fra"}, {"de","deu"}, {"es","spa"}, {"it","ita"},
		{"pt","por"}, {"ja","jpn"}, {"zh","zho"}, {"ko","kor"}, {"ru","rus"},
		{"ar","ara"}, {"nl","nld"}, {"pl","pol"}, {"sv","swe"}, {"nb","nor"},
		{"da","dan"}, {"fi","fin"}, {"tr","tur"}, {"cs","ces"}, {"sk","slk"},
		{"hu","hun"}, {"ro","ron"}, {"el","ell"}, {"he","heb"}, {"th","tha"},
		{"id","ind"}, {"vi","vie"}, {"uk","ukr"}, {"hi","hin"}, {"bn","ben"},
	};
	const QString lc = iso1.toLower();
	return (lc.length() == 2) ? m.value(lc, lc) : lc;  // pass 3-letter codes through unchanged
}

// Menu toggle widget that draws its own hover (full row) and checked indicator (inset pill).
// Not Q_OBJECT — uses std::function callback instead of signals.
class McQueueToggle final : public QWidget
{
public:
	std::function<void(bool)> onToggled;

	McQueueToggle(const QIcon& icon, const QString& text,
	              QColor hoverColor, QColor checkedColor, QColor checkedHoverColor,
	              QWidget* parent = nullptr)
	    : QWidget(parent)
	    , m_icon(icon), m_text(text)
	    , m_hoverColor(hoverColor)
	    , m_checkedColor(checkedColor)
	    , m_checkedHoverColor(checkedHoverColor)
	{
		setAutoFillBackground(true);
		setAttribute(Qt::WA_Hover);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	}

	void setChecked(bool on) { if (m_checked != on) { m_checked = on; update(); } }
	bool isChecked() const { return m_checked; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRect r = rect();
		const bool enabled = isEnabled();

		// Full-row hover fill — always drawn first so the pill sits on top of it
		if (m_hovered && enabled)
			p.fillRect(r, m_hoverColor);

		// Checked pill: inset 1px top/bottom, 2px left/right — brighter when hovered
		if (m_checked) {
			p.setPen(Qt::NoPen);
			p.setBrush(m_hovered && enabled ? m_checkedHoverColor : m_checkedColor);
			if (!enabled) p.setOpacity(0.4);
			p.drawRoundedRect(QRectF(r).adjusted(2.0, 1.0, -2.0, -1.0), 3.0, 3.0);
			p.setOpacity(1.0);
		}

		// Icon (16×16, left-aligned with 8px left padding)
		constexpr int kIconW   = 16;
		constexpr int kLeftPad = 8;
		constexpr int kGap     = 5;
		const QRect iconRect(kLeftPad, (r.height() - kIconW) / 2, kIconW, kIconW);
		if (!enabled) p.setOpacity(0.4);
		m_icon.paint(&p, iconRect);

		// Text
		p.setPen(palette().color(enabled ? QPalette::Active : QPalette::Disabled, QPalette::Text));
		p.setFont(font());
		const QRect textRect = r.adjusted(kLeftPad + kIconW + kGap, 0, -kLeftPad, 0);
		p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_text);
		if (!enabled) p.setOpacity(1.0);
	}

	void enterEvent(QEnterEvent*) override { m_hovered = true;  update(); }
	void leaveEvent(QEvent*)      override { m_hovered = false; update(); }

	void changeEvent(QEvent* e) override
	{
		QWidget::changeEvent(e);
		if (e->type() == QEvent::EnabledChange) update();
	}

	void mousePressEvent(QMouseEvent* e) override
	{
		if (!isEnabled()) return;
		if (e->button() == Qt::LeftButton) {
			m_checked = !m_checked;
			update();
			if (onToggled) onToggled(m_checked);
		}
	}

	QSize sizeHint() const override
	{
		const int w = 8 + 16 + 5 + fontMetrics().horizontalAdvance(m_text) + 8;
		return { w, fontMetrics().height() + 8 };
	}

private:
	QIcon    m_icon;
	QString  m_text;
	QColor   m_hoverColor;
	QColor   m_checkedColor;
	QColor   m_checkedHoverColor;
	bool     m_hovered = false;
	bool     m_checked = false;
};

// Repaint the full widget tree synchronously so the first visible frame is
// already drawn — QListView delegates and other children don't paint until
// explicitly repainted while the top-level window is still invisible.
static void flushWidgetRepaints(QWidget* root)
{
	if (!root)
		return;
	root->ensurePolished();
	root->repaint();
	for (QObject* child : root->children()) {
		if (auto* w = qobject_cast<QWidget*>(child))
			flushWidgetRepaints(w);
	}
}

} // anonymous namespace

namespace Mc {

McMainWindow::McMainWindow(QWidget* parent)
	: QMainWindow(parent)
{
	// Keep the native HWND off-screen until dismissSplash() has painted the
	// backing store — otherwise Windows shows a white client area at restored
	// geometry while the constructor blocks on library load.
	setAttribute(Qt::WA_DontShowOnScreen, true);
	setAttribute(Qt::WA_OpaquePaintEvent, true);

	setWindowTitle("MediaCurator");
	setMinimumSize(900, 600);

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	const bool firstLaunch = !s.contains("mainWindow/geometry");
	if (!firstLaunch) {
		restoreGeometry(s.value("mainWindow/geometry").toByteArray());
		restoreState(s.value("mainWindow/state").toByteArray());
		// Never start hidden/minimized — dismissSplash() must be able to showNormal().
		setWindowState(windowState() & ~Qt::WindowMinimized);
	}
	// Splitter state is restored after setupUi() creates m_splitter

	m_profile  = new UserProfile(this);
	m_profile->load();

	m_jobQueue = new JobQueue(this);
	m_jobQueue->setWriteJobLog(m_profile->writeJobLog());
	m_jobQueue->setMergeSidecarSubtitles(m_profile->mergeSidecarSubtitles());
	m_jobQueue->setUseLocalStaging(m_profile->useLocalStaging());
	m_jobQueue->setLocalStagingDir(m_profile->localStagingDir());
	m_jobQueue->setDetectSubtitleLanguage(m_profile->detectSidecarSubtitleLanguage());

	connect(m_profile, &UserProfile::profileChanged, this, [this]() {
		m_jobQueue->setWriteJobLog(m_profile->writeJobLog());
		m_jobQueue->setMergeSidecarSubtitles(m_profile->mergeSidecarSubtitles());
		m_jobQueue->setUseLocalStaging(m_profile->useLocalStaging());
		m_jobQueue->setLocalStagingDir(m_profile->localStagingDir());
		m_jobQueue->setDetectSubtitleLanguage(m_profile->detectSidecarSubtitleLanguage());
		PosterManager::instance().setTmdbApiKey(m_profile->tmdbApiKey());
		const bool tmdbConfigured = !m_profile->tmdbApiKey().isEmpty();
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->setTmdbConfigured(tmdbConfigured);
		m_jobPanel->setTmdbConfigured(tmdbConfigured);
		SubtitleManager::instance().setCredentials(m_profile->openSubtitlesApiKey(),
		                                            m_profile->openSubtitlesUsername(),
		                                            m_profile->openSubtitlesPassword());
		SubtitleManager::instance().setEnabled(m_profile->autoDownloadSubtitles());
		SubtitleManager::instance().setUnderstoodLanguages(m_profile->understoodLanguages());
		SubtitleManager::instance().setDetectSubtitleLanguage(m_profile->detectSidecarSubtitleLanguage());
		SubtitleManager::instance().setEditionTokens(m_profile->editionTokens());
		SubtitleManager::instance().setComputeMovieHash(m_profile->computeSubtitleMovieHash());
	});

	m_savedJobPanelHeight = AppSettings::instance().value("mainWindow/jobPanelHeight", 0).toInt();
	m_jobPanelPinned     = AppSettings::instance().value("mainWindow/queueHidden",    false).toBool();
	m_highscoreBandPinned = AppSettings::instance().value("mainWindow/highscoreHidden", false).toBool();

	setupActions();
	setupUi();
	m_jobPanel->setVisible(false);   // hidden until we know there are jobs
	{
		const int saved = AppSettings::instance().value("library/sortOrder", McFilterPanel::SortByName).toInt();
		const int idx   = m_filterPanel->sortCombo()->findData(saved);
		if (idx > 0)
			m_filterPanel->sortCombo()->setCurrentIndex(idx);
	}
	if (const QByteArray sp = s.value("mainWindow/splitter").toByteArray(); !sp.isEmpty()) {
		m_splitter->restoreState(sp);
		m_splitterRestored = true;
	} else {
		m_splitter->setSizes({ 600, 600 });
	}

	if (firstLaunch) {
		if (QScreen* screen = QGuiApplication::primaryScreen()) {
			const QRect avail = screen->availableGeometry();
			setGeometry(avail.adjusted(50, 50, -50, -50));
		}
	}
	setupToolBar();
	setupMenuBar();
	setupStatusBar();

	connect(m_jobQueue, &JobQueue::fileRescanned,
	        m_listModel, &McFileListModel::reloadFile);
	connect(m_jobQueue, &JobQueue::fileRescanned,
	        this, [this](qint64 fid) {
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->invalidateSizeCacheFor(fid);
		PosterManager::instance().enqueue(fid);
		SubtitleManager::instance().enqueue(fid);
	});
	// After a track-mismatch re-analyze rescan completes, auto-analyze just that file
	// so the user doesn't have to manually click Analyze Library.
	connect(m_jobQueue, &JobQueue::fileNeedsReanalysis,
	        this, [this](qint64 fileId) {
		if (m_analyzeThread) return;  // full analysis running — it'll cover this file
		const bool created = analyzeSingleFile(fileId);
		m_jobPanel->refresh();
		m_listModel->refreshJobFilter();
		if (created) {
			updateJobPanelVisibility(/*forceShow=*/true);
			m_jobPanel->scrollToFileJob(fileId);
		}
	});

	m_analyzeRefreshTimer = new QTimer(this);
	m_analyzeRefreshTimer->setSingleShot(true);
	m_analyzeRefreshTimer->setInterval(300);
	connect(m_analyzeRefreshTimer, &QTimer::timeout, this, [this] {
		m_jobPanel->refresh();
		m_listModel->refreshJobFilter();
	});

	connect(m_jobQueue, &JobQueue::jobStarted, this, [this](qint64 jobId) {
		const auto rec = DatabaseManager::instance().jobDisplayRecordById(jobId);
		if (rec)
			m_runningJobNames.insert(jobId, jobStatusLabel(*rec));
		m_runningJobProgress.insert(jobId, 0);
		m_queuedAtStart = DatabaseManager::instance().queuedJobCount();
		updateRemuxStatusBar();
	});

	connect(m_jobQueue, &JobQueue::progressChanged, this, [this](qint64 jobId, int pct) {
		m_runningJobProgress.insert(jobId, pct);
		updateRemuxStatusBar();
	});

	// A single-job cancel (JobQueue::cancelJob) sends one job back to "queued"
	// without emitting allFinished (other jobs may still be running), so this is
	// the only place that job's entry gets dropped from the running-jobs maps.
	// A bulk cancel() also emits jobRequeued for each job it stops, but that's
	// immediately followed by allFinished(), which does its own full clear —
	// this handler still runs first and is harmless in that case.
	connect(m_jobQueue, &JobQueue::jobRequeued, this, [this](qint64 jobId) {
		m_runningJobNames.remove(jobId);
		m_runningJobProgress.remove(jobId);
		if (!m_runningJobNames.isEmpty()) {
			updateRemuxStatusBar();
		} else {
			m_progressBar->setRange(0, 100);
			m_progressBar->setVisible(false);
			m_progressBar->setValue(0);
			m_statusLabel->setText(tr("Stopped — %1 job(s) queued")
			                       .arg(DatabaseManager::instance().queuedJobCount()));
#ifdef Q_OS_WIN
			clearTaskbarProgress();
#endif
		}
	});

	connect(m_jobQueue, &JobQueue::jobFinished, this, [this](qint64 jobId, bool success, qint64) {
		updateSavedLabel();
		const QString failedName = m_runningJobNames.value(jobId);
		m_runningJobNames.remove(jobId);
		m_runningJobProgress.remove(jobId);

		if (!success) {
			m_progressBar->setRange(0, 100);
			m_progressBar->setVisible(false);
			m_progressBar->setValue(0);
			// A track-mismatch review already reported this job as failed via this
			// same signal when the review dialog was raised (JobQueue::onRemuxJobFinished).
			// Resolving that review (accept/re-analyze/discard) re-emits jobFinished for
			// bookkeeping, but by then the name has already been consumed above — skip
			// re-showing the message so it doesn't render as "Job failed for ''".
			if (!failedName.isEmpty()) {
				m_statusLabel->setText(tr("Job failed for '%1'").arg(failedName));
#ifdef Q_OS_WIN
				if (m_taskbar) {
					m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_ERROR);
				}
#endif
			}
		} else if (m_jobQueue->isPaused()) {
			m_progressBar->setRange(0, 100);
			m_progressBar->setVisible(false);
			m_progressBar->setValue(0);
			// More jobs are waiting but the queue is paused; allFinished won't
			// fire until the user resumes and the last job completes.
			m_statusLabel->setText(tr("Paused — click Start to resume"));
#ifdef Q_OS_WIN
			clearTaskbarProgress();
#endif
		} else if (!m_runningJobNames.isEmpty()) {
			updateRemuxStatusBar();
		} else {
			m_progressBar->setRange(0, 100);
			m_progressBar->setVisible(false);
			m_progressBar->setValue(0);
		}
	});

	if (HighscoreClient::instance().isEnabled()) {
		m_highscoreDebounce = new QTimer(this);
		m_highscoreDebounce->setSingleShot(true);
		m_highscoreDebounce->setInterval(5000);   // coalesces bursts (e.g. a bulk job run)
		connect(m_highscoreDebounce, &QTimer::timeout, this, &McMainWindow::submitHighscoreIfDue);

		connect(m_jobQueue, &JobQueue::jobFinished, this, [this](qint64, bool success, qint64) {
			if (success)
				m_highscoreDebounce->start();
		});

		connect(&HighscoreClient::instance(), &HighscoreClient::leaderboardReady,
		        this, &McMainWindow::onHighscoreLeaderboardReady);
		connect(&HighscoreClient::instance(), &HighscoreClient::scoreSubmitted,
		        this, [](bool ok) { if (ok) HighscoreClient::instance().fetchLeaderboard(); });
	}

	connect(m_jobQueue, &JobQueue::reviewRequested, this,
	        [this](qint64 jobId, const QString& filename, const QString& warningText,
	               const QString& cmdArgs,
	               const QList<StreamRecord>& srcStreams,
	               const QList<StreamRecord>& outStreams) {
		auto* dlg = new McJobReviewDialog(jobId, filename, warningText,
		                                  cmdArgs, srcStreams, outStreams, this);
		connect(dlg, &QDialog::finished, this, [this, jobId](int result) {
			if (result == QDialog::Accepted)
				m_jobQueue->commitReview(jobId);
			else if (result == 2)
				m_jobQueue->rejectReview(jobId, true);
			else
				m_jobQueue->rejectReview(jobId, false);
		});
		dlg->show();
	});

	connect(m_jobQueue, &JobQueue::allFinished, this, [this] {
		m_progressBar->setVisible(false);
		m_progressBar->setValue(0);
		m_runningJobNames.clear();
		m_runningJobProgress.clear();
		updateSavedLabel();
		const int queued = DatabaseManager::instance().queuedJobCount();
		if (queued > 0) {
			m_statusLabel->setText(tr("Stopped — %1 job(s) queued").arg(queued));
		} else {
			const int total    = m_listModel->totalCount();
			const int proposed = DatabaseManager::instance().totalJobCount();
			QString msg = tr("All jobs finished — %1 files in library").arg(total);
			if (proposed > 0)
				msg += tr(", %1 proposed").arg(proposed);
			m_statusLabel->setText(msg);
		}
#ifdef Q_OS_WIN
		clearTaskbarProgress();
#endif
	});

	connect(m_jobQueue, &JobQueue::warning,
	        this, [this](const QString& msg) { m_statusLabel->setText(msg); });

	// ── Poster manager ────────────────────────────────────────────────────────
	auto& pm = PosterManager::instance();
	pm.start(m_profile->tmdbApiKey());
	connect(&pm, &PosterManager::posterReady,
	        m_listModel, &McFileListModel::onPosterReady);
	connect(&pm, &PosterManager::fanartReady,
	        m_listModel, &McFileListModel::onFanartReady);
	connect(&pm, &PosterManager::posterReady, this, [this](qint64, const QString&) {
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->scheduleArtworkPrefetch();
	});
	connect(&pm, &PosterManager::fanartReady, this, [this](qint64, const QString&, const QImage&) {
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->scheduleArtworkPrefetch();
	});
	connect(&pm, &PosterManager::imdbIdSaved,
	        m_listModel, &McFileListModel::onImdbIdSaved);
	connect(&pm, &PosterManager::tmdbDataReady,
	        m_listModel, &McFileListModel::onTmdbDataReady);
	connect(&pm, &PosterManager::batchProgressChanged, this, [this](int done, int total) {
		m_posterProgressBar->setRange(0, total);
		m_posterProgressBar->setValue(done);
		m_posterProgressBar->setVisible(true);
		m_btnCancelPosterRefresh->setVisible(true);
		m_btnCancelPosterRefresh->setEnabled(true);
		m_btnCancelPosterRefresh->setText(tr("Cancel Poster Refresh"));
		m_statusLabel->setText(tr("Refreshing posters… %1 of %2").arg(done).arg(total));
	});
	connect(&pm, &PosterManager::batchFinished, this, [this](int done, int total, bool cancelled) {
		m_posterProgressBar->setVisible(false);
		m_btnCancelPosterRefresh->setVisible(false);
		m_statusLabel->setText(cancelled
		    ? tr("Poster refresh cancelled after %1 of %2").arg(done).arg(total)
		    : tr("Refreshed %1 poster(s)").arg(total));
	});

	// ── Subtitle manager ──────────────────────────────────────────────────────
	auto& sm = SubtitleManager::instance();
	sm.start(m_profile->openSubtitlesApiKey(), m_profile->openSubtitlesUsername(),
	         m_profile->openSubtitlesPassword(), m_profile->autoDownloadSubtitles(),
	         m_profile->understoodLanguages());
	sm.setDetectSubtitleLanguage(m_profile->detectSidecarSubtitleLanguage());
	sm.setEditionTokens(m_profile->editionTokens());
	sm.setComputeMovieHash(m_profile->computeSubtitleMovieHash());
	connect(&sm, &SubtitleManager::subtitlesReady, this, [this](qint64 fileId, int downloaded) {
		m_listModel->reloadFile(fileId);
		m_jobPanel->refresh();
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		m_statusLabel->setText(
			tr("Downloaded %1 subtitle(s) for %2").arg(downloaded)
				.arg(fileOpt ? fileOpt->filename : QString::number(fileId)));
	});
	connect(&sm, &SubtitleManager::quotaExhausted, this, [this](qint64 resumeAtEpoch) {
		const QDateTime resumeAt = QDateTime::fromSecsSinceEpoch(resumeAtEpoch);
		m_statusLabel->setText(
			tr("OpenSubtitles rate limit reached — automatic subtitle downloads paused until %1.")
				.arg(QLocale::system().toString(resumeAt, QLocale::ShortFormat)));
	});
	connect(&sm, &SubtitleManager::queueActiveChanged, this, [this](bool active) {
		m_btnCancelSubtitles->setVisible(active);
		if (active) {
			m_btnCancelSubtitles->setEnabled(true);
			m_btnCancelSubtitles->setText(tr("Cancel Subtitle Downloads"));
		}
	});

	// ── Update checker ────────────────────────────────────────────────────────
	connect(&UpdateChecker::instance(), &UpdateChecker::updateAvailable,
	        this, &McMainWindow::onUpdateAvailable);
	connect(&UpdateChecker::instance(), &UpdateChecker::upToDate,
	        this, &McMainWindow::onUpdateUpToDate);
	connect(&UpdateChecker::instance(), &UpdateChecker::checkFailed,
	        this, &McMainWindow::onUpdateCheckFailed);
	connect(&UpdateChecker::instance(), &UpdateChecker::downloadProgress,
	        this, &McMainWindow::onUpdateDownloadProgress);
	connect(&UpdateChecker::instance(), &UpdateChecker::downloadFailed,
	        this, &McMainWindow::onUpdateDownloadFailed);
	connect(&UpdateChecker::instance(), &UpdateChecker::installerLaunched,
	        this, &McMainWindow::onUpdateInstallerLaunched);

	startLibraryLoader();
}

McMainWindow::~McMainWindow()
{
#ifdef Q_OS_WIN
	if (m_taskbar) { m_taskbar->Release(); m_taskbar = nullptr; }
	if (m_nativeBgBrush) {
		DeleteObject(static_cast<HBRUSH>(m_nativeBgBrush));
		m_nativeBgBrush = nullptr;
	}
#endif
}

void McMainWindow::setupUi()
{
	m_splitter = new QSplitter(Qt::Vertical, this);
	auto* splitter = m_splitter;
	splitter->setChildrenCollapsible(false);

	// ── File list (top) ───────────────────────────────────────────────────────
	m_listModel = new McFileListModel(this);

	m_listView = new QListView(this);
	m_listView->setModel(m_listModel);
	auto* fileDelegate = new McFileCardDelegate(m_listView);
	connect(fileDelegate, &McFileCardDelegate::playRequested,
	        this, [this](const QModelIndex& idx) {
		launchInVlc(idx.data(McFileListModel::FileRole).value<FileRecord>().path);
	});
	connect(fileDelegate, &McFileCardDelegate::imdbPageRequested,
	        this, [this](const QModelIndex& idx) {
		const QString id = idx.data(McFileListModel::ImdbRole).toString();
		if (!id.isEmpty())
			QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.imdb.com/title/%1/").arg(id)));
	});
	connect(fileDelegate, &McFileCardDelegate::streamToggleRequested,
	        this, [this](const QModelIndex& idx, int streamIndex) {
		const qint64 fileId = idx.data(McFileListModel::FileRole).value<FileRecord>().id;
		QApplication::setOverrideCursor(Qt::WaitCursor);
		m_listModel->toggleForcedRemoval(fileId, streamIndex);
		QApplication::restoreOverrideCursor();
	});

	m_listView->setItemDelegate(fileDelegate);
	fileDelegate->setTmdbConfigured(!m_profile->tmdbApiKey().isEmpty());
	fileDelegate->setMultiGroupBadgeEnabled(StorageGroupSettings::multipleGroupsInUse());
	fileDelegate->setFanartOpacity(
	    AppSettings::instance().value("library/fanartOpacity", 5).toInt() / 100.0);

	// When stream layout changes, the delegate's size cache may hold stale heights.
	// Only clear it for roles that actually affect card height (streams / overrides).
	// Title, year, rating, and poster path never change the badge row count or height.
	connect(m_listModel, &QAbstractItemModel::dataChanged, this,
	        [this](const QModelIndex& topLeft, const QModelIndex&, const QList<int>& roles) {
		if (!topLeft.isValid()) return;
		const bool affectsHeight = roles.contains(McFileListModel::StreamsRole)
		    || roles.contains(McFileListModel::OverridesRole);
		if (!affectsHeight) return;
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate())) {
			const auto file = topLeft.data(McFileListModel::FileRole).value<FileRecord>();
			d->invalidateSizeCacheFor(file.id);
		}
	});

	// Double-click on the poster column → open IMDb search dialog
	connect(m_listView, &QAbstractItemView::doubleClicked, this,
	        [this](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		const QPoint pos    = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		const QRect  iRect  = m_listView->visualRect(idx);
		const auto*  cardDelegate = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate());
		const int    posterW = cardDelegate ? cardDelegate->posterColumnWidth() : McFileCardDelegate::kPosterW;
		if (pos.x() > iRect.left() + posterW) return;
		const FileRecord file     = idx.data(McFileListModel::FileRole).value<FileRecord>();
		const QString    suggested = smartSuggestedTitle(file);
		const QString    existing  = NfoParser::readImdbId(file.path);
		const QString    exPoster  = idx.data(McFileListModel::PosterRole).toString();
		const QString    exFanart  = idx.data(McFileListModel::FanartRole).toString();
		ImdbSearchDialog dlg(file.path, suggested, existing, m_profile->tmdbApiKey(), this, exPoster, exFanart);
		dlg.setUnderstoodLanguages(m_profile->understoodLanguages());
		if (existing.isEmpty()) dlg.setAutoSelectSingle(true);
		if (dlg.exec() == QDialog::Accepted) {
			const QString id = dlg.selectedImdbId();
			if (!id.isEmpty()) {
				NfoParser::writeMovieNfo(file.path, id, dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(file.id, dlg.selectedPosterPath(), dlg.selectedImageData(), id,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount(),
				                                 dlg.selectedFanartPath());
				if (dlg.selectedVoteAverage() > 0) {
					m_listModel->setRatingForFile(file.id, dlg.selectedVoteAverage());
					m_jobPanel->setRatingForFile(file.id, dlg.selectedVoteAverage());
				}
				const QString origLang = tmdbLangToIso6392(dlg.selectedOriginalLanguage());
				if (!origLang.isEmpty())
					DatabaseManager::instance().updateFileOriginalLanguage(file.id, origLang);
				const QString title = dlg.selectedTitle();
				if (!title.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(file.id, title, dlg.selectedYear());
				m_listView->viewport()->repaint();
			}
		}
	});

	m_listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_listView->setMouseTracking(true);
	m_listView->viewport()->setMouseTracking(true);
	m_listView->setUniformItemSizes(false);
	m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	// AlwaysOn, not the AsNeeded default: row heights depend on viewport width
	// (badge wrapping), so if the scrollbar's own presence toggles with total
	// content height, its width changes the viewport width, which triggers
	// another resize → another relayout → possibly toggles the scrollbar again.
	// Pinning it removes that feedback loop entirely (see McCardDelegate's
	// resize-relayout handling).
	m_listView->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
	m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_listView->setAlternatingRowColors(true);
	m_listView->setSpacing(0);
	// Fixed, not Adjust: with setUniformItemSizes(false), Adjust mode forces a full
	// doItemsLayout() (re-querying every row's sizeHint) on every single resize
	// tick during a live drag — badly janky with thousands of cards. McCardDelegate
	// now handles post-resize relayout itself (debounced, visible-rows-only; see
	// McCardDelegate::relayoutForResize), so Qt's own per-tick relayout is redundant.
	m_listView->setResizeMode(QListView::Fixed);
	m_listView->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(m_listView, &QListView::customContextMenuRequested,
	        this, [this](const QPoint& pos) {
		const QModelIndex idx = m_listView->indexAt(pos);
		if (!idx.isValid()) return;
		const FileRecord file = idx.data(McFileListModel::FileRole).value<FileRecord>();

		// Detect whether the right-click landed on a specific track badge — same
		// pattern as McJobPanel's badge context menu (customContextMenuRequested
		// gives pos in list-view widget coordinates; hitTestBadgeStream and
		// visualRect() both work in viewport coordinates).
		{
			const QPoint vpPos = m_listView->viewport()->mapFrom(m_listView, pos);
			QList<StreamRecord> streams;
			int hitStreamIdx = -1;
			if (auto* del = qobject_cast<McCardDelegate*>(m_listView->itemDelegate())) {
				streams = idx.data(McFileListModel::StreamsRole).value<QList<StreamRecord>>();
				const bool hasImdb = !idx.data(McFileListModel::ImdbRole).toString().isEmpty();
				hitStreamIdx = del->hitTestBadgeStream(vpPos, m_listView->visualRect(idx),
				                                        streams, m_listView->font(),
				                                        hasImdb, file.originalLanguage);
			}
			const StreamRecord* hitStream = nullptr;
			for (const StreamRecord& s : streams) {
				if (s.streamIndex == hitStreamIdx) { hitStream = &s; break; }
			}
			if (hitStream) {
				QMenu menu(this);
				const StreamRecord streamCopy = *hitStream;
				buildTrackFlagMenu(menu, streamCopy, file.originalLanguage, devicePixelRatioF(), /*showFlagRowsForExternal=*/false,
					[this, file, hitStreamIdx](const QString& flag, bool value) {
						TrackFlagService::instance().apply(file.id, hitStreamIdx, flag, value,
							[this, file](bool ok) {
								if (!ok) {
									m_statusLabel->setText(
										tr("Failed to update track flag for %1").arg(file.filename));
									return;
								}
								m_listModel->reloadFile(file.id);
								const bool created = analyzeSingleFile(file.id);
								m_jobPanel->refresh();
								m_listModel->refreshJobFilter();
								if (created) {
									updateJobPanelVisibility(/*forceShow=*/true);
									m_jobPanel->scrollToFileJob(file.id);
								}
							});
					},
					[this, file, streamCopy](const QString& code) {
						setSubtitleLanguage(file, streamCopy, code);
					});
				menu.exec(m_listView->viewport()->mapToGlobal(pos));
				return;
			}
		}

		QSet<int> selRows;
		int firstSelRow = idx.row();
		for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes()) {
			selRows.insert(si.row());
			firstSelRow = qMin(firstSelRow, si.row());
		}
		const int selCount = selRows.size();

		QMenu menu(this);

		auto* analyzeAction = menu.addAction(svgIcon(":/icons/manage_search.svg"),
		                                      tr("&Analyze File"));
		connect(analyzeAction, &QAction::triggered, this, [this, file] {
			const bool created = analyzeSingleFile(file.id);
			m_jobPanel->refresh();
			m_listModel->refreshJobFilter();
			if (created) {
				updateJobPanelVisibility(/*forceShow=*/true);
				m_jobPanel->scrollToFileJob(file.id);
				m_statusLabel->setText(tr("Proposed job for %1").arg(file.filename));
			} else {
				const auto existing = DatabaseManager::instance().activeJobForFile(file.id);
				if (!existing || existing->jobType == QLatin1String("tag_edit"))
					m_statusLabel->setText(tr("No removals found for %1").arg(file.filename));
			}
		});

		auto* previewAction = menu.addAction(svgIcon(":/icons/visibility.svg"),
		                                      tr("&Preview Tracks…"));
		previewAction->setEnabled(selCount == 1);
		connect(previewAction, &QAction::triggered,
		        this, [this, file] { onShowPreview(file.id); });

		menu.addSeparator();

		// Ignore / Unignore — collect all selected file IDs for batch operation.
		QList<qint64> selectedFileIds;
		{
			QSet<int> seen;
			for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes()) {
				if (seen.contains(si.row())) continue;
				seen.insert(si.row());
				const qint64 fid = si.data(McFileListModel::FileRole).value<FileRecord>().id;
				if (fid > 0) selectedFileIds << fid;
			}
			if (!selectedFileIds.contains(file.id)) selectedFileIds.prepend(file.id);
		}
		if (file.ignored) {
			const QString unignoreLabel = selectedFileIds.size() > 1
			    ? tr("&Unignore %1 Files").arg(selectedFileIds.size())
			    : tr("&Unignore File");
			auto* unignoreAction = menu.addAction(svgIcon(":/icons/visibility_off.svg"), unignoreLabel);
			connect(unignoreAction, &QAction::triggered, this, [this, selectedFileIds, firstSelRow] {
				auto& db = DatabaseManager::instance();
				for (qint64 fid : selectedFileIds) db.setFileIgnored(fid, false);
				m_listModel->setIgnoredBatch(selectedFileIds, false);
				m_statusLabel->setText(tr("Unignored %1 file(s)").arg(selectedFileIds.size()));
				QTimer::singleShot(0, this, [this, firstSelRow] {
					const int n = m_listModel->rowCount();
					if (n > 0) {
						const QModelIndex next = m_listModel->index(qMin(firstSelRow, n - 1), 0);
						m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
						m_listView->scrollTo(next, QAbstractItemView::EnsureVisible);
					}
				});
			});
		} else {
			const QString ignoreLabel = selectedFileIds.size() > 1
			    ? tr("&Ignore %1 Files").arg(selectedFileIds.size())
			    : tr("&Ignore File");
			auto* ignoreAction = menu.addAction(svgIcon(":/icons/block.svg"), ignoreLabel);
			connect(ignoreAction, &QAction::triggered, this, [this, selectedFileIds, firstSelRow] {
				auto& db = DatabaseManager::instance();
				for (qint64 fid : selectedFileIds) db.setFileIgnored(fid, true);
				m_listModel->setIgnoredBatch(selectedFileIds, true);
				m_statusLabel->setText(tr("Ignored %1 file(s) — switch to \"Ignored files\" filter to manage them")
				    .arg(selectedFileIds.size()));
				QTimer::singleShot(0, this, [this, firstSelRow] {
					const int n = m_listModel->rowCount();
					if (n > 0) {
						const QModelIndex next = m_listModel->index(qMin(firstSelRow, n - 1), 0);
						m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
						m_listView->scrollTo(next, QAbstractItemView::EnsureVisible);
					}
				});
			});
		}

		menu.addSeparator();

		const QString refreshPosterLabel = selectedFileIds.size() > 1
		    ? tr("Refresh %1 &Posters").arg(selectedFileIds.size())
		    : tr("Refresh &Poster");
		auto* refreshPosterAction = menu.addAction(svgIcon(":/icons/refresh.svg"), refreshPosterLabel);
		connect(refreshPosterAction, &QAction::triggered, this, [selectedFileIds] {
			auto& pm = PosterManager::instance();
			if (selectedFileIds.size() > 1)
				pm.refreshBatch(selectedFileIds);
			else
				pm.refresh(selectedFileIds.first());
		});

		auto* dlSubsAction = menu.addAction(svgIcon(":/icons/translate.svg"),
		                                     tr("&Download Subtitles…"));
		dlSubsAction->setEnabled(selCount == 1);
		connect(dlSubsAction, &QAction::triggered, this, [this, file] {
			// Check API key
			if (m_profile->openSubtitlesApiKey().isEmpty()) {
				QMessageBox::information(this, tr("Download Subtitles"),
					tr("No OpenSubtitles API key configured.\n"
					   "Go to Settings → OpenSubtitles to add your key."));
				return;
			}

			// Resolve IMDB ID (poster_cache first, then NFO file fallback)
			QString imdbId;
			if (const auto pr = DatabaseManager::instance().posterForFile(file.id))
				imdbId = pr->imdbId;
			if (imdbId.isEmpty())
				imdbId = NfoParser::readImdbId(file.path);
			if (imdbId.isEmpty()) {
				QMessageBox::information(this, tr("Download Subtitles"),
					tr("No IMDb ID found for \"%1\".\n"
					   "Use \"Edit Movie Metadata\" to identify the movie first.").arg(file.filename));
				return;
			}

			// Find which understood languages have no subtitle coverage among the tracks
			// that will actually remain — a track already toggled off for removal
			// doesn't count as covering its language.
			const auto streams  = DatabaseManager::instance().streamsForFile(file.id);
			const auto remaining = streamsRemainingForFile(file.id, streams, m_listModel);
			const QStringList missingIso6392 =
				Mc::missingSubtitleLanguages(remaining, m_profile->understoodLanguages());

			if (missingIso6392.isEmpty()) {
				QMessageBox::information(this, tr("Download Subtitles"),
					tr("All subtitle languages are already covered for \"%1\".").arg(file.filename));
				return;
			}

			const qint64  fileId   = file.id;
			const QString filePath = file.path;
			const QString movieTitle = file.displayTitle.isEmpty()
			    ? QFileInfo(filePath).completeBaseName()
			    : (file.displayYear > 0
			        ? QStringLiteral("%1 (%2)").arg(file.displayTitle).arg(file.displayYear)
			        : file.displayTitle);
			auto* dlg = new Mc::McSubtitleDownloadDialog(
				m_profile->openSubtitlesApiKey(),
				m_profile->openSubtitlesUsername(),
				m_profile->openSubtitlesPassword(),
				imdbId, missingIso6392, filePath, file.durationSec,
				m_profile->editionTokens(), m_profile->computeSubtitleMovieHash(),
				streams, movieTitle, this);
			dlg->setAttribute(Qt::WA_DeleteOnClose);
			connect(dlg, &Mc::McSubtitleDownloadDialog::downloadComplete, this,
				[this, fileId, filePath](int downloaded) {
					if (downloaded == 0) return;
					// Refresh sidecar streams in DB so the card reflects new files.
					auto& db = DatabaseManager::instance();
					const auto existing = db.streamsForFile(fileId);
					QList<StreamRecord> containerStreams;
					for (const auto& s : existing)
						if (!s.isExternal) containerStreams << s;
					const auto sidecars = ScanWorker::scanSidecarSubtitles(
						filePath, ScanWorker::nextSidecarStreamIndex(containerStreams),
						m_profile->detectSidecarSubtitleLanguage());
					auto allStreams = containerStreams;
					allStreams.append(sidecars);
					db.insertStreams(fileId, allStreams);
					m_listModel->reloadFile(fileId);
					m_listView->viewport()->repaint();
					m_statusLabel->setText(
						tr("Downloaded %1 subtitle(s) for %2").arg(downloaded)
							.arg(QFileInfo(filePath).fileName()));
				});
			dlg->open();
		});

		// Collect selected files so right-clicking on a multi-selection gives a batch action.
		QList<FileRecord> imdbFiles;
		{
			QSet<int> seen;
			for (const QModelIndex& si : m_listView->selectionModel()->selectedIndexes()) {
				if (seen.contains(si.row())) continue;
				seen.insert(si.row());
				imdbFiles << si.data(McFileListModel::FileRole).value<FileRecord>();
			}
			bool alreadyIn = false;
			for (const auto& f : imdbFiles) if (f.id == file.id) { alreadyIn = true; break; }
			if (!alreadyIn) imdbFiles.prepend(file);
		}
		const QString imdbLabel = imdbFiles.size() > 1
		    ? tr("Edit &Movie Metadata (%1 files)…").arg(imdbFiles.size())
		    : tr("Edit &Movie Metadata…");
		auto* imdbAction = menu.addAction(svgIcon(":/icons/link.svg"), imdbLabel);
		connect(imdbAction, &QAction::triggered, this, [this, imdbFiles, firstSelRow] {
			const int total = imdbFiles.size();
			for (int i = 0; i < total; ++i) {
				const FileRecord& f = imdbFiles[i];
				const QString suggested = smartSuggestedTitle(f);
				QString existing;
				QString exPoster;
				QString exFanart;
				if (const auto pr = DatabaseManager::instance().posterForFile(f.id)) {
					existing = pr->imdbId;
					exPoster = pr->imagePath;
					exFanart = pr->fanartPath;
				}
				if (existing.isEmpty()) existing = NfoParser::readImdbId(f.path);
				ImdbSearchDialog dlg(f.path, suggested, existing, m_profile->tmdbApiKey(), this, exPoster, exFanart);
				dlg.setUnderstoodLanguages(m_profile->understoodLanguages());
				if (existing.isEmpty()) dlg.setAutoSelectSingle(true);
				if (total > 1) dlg.setBatchMode(i + 1, total);
				const int result = dlg.exec();
				if (result == ImdbSearchDialog::CancelBatch) break;
				if (result != QDialog::Accepted) continue;
				const QString imdbId = dlg.selectedImdbId();
				if (imdbId.isEmpty()) continue;
				NfoParser::writeMovieNfo(f.path, imdbId, dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(f.id, dlg.selectedPosterPath(), dlg.selectedImageData(), imdbId,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount(),
				                                 dlg.selectedFanartPath());
				if (dlg.selectedVoteAverage() > 0) {
					m_listModel->setRatingForFile(f.id, dlg.selectedVoteAverage());
					m_jobPanel->setRatingForFile(f.id, dlg.selectedVoteAverage());
				}
				const QString origLang = tmdbLangToIso6392(dlg.selectedOriginalLanguage());
				if (!origLang.isEmpty())
					DatabaseManager::instance().updateFileOriginalLanguage(f.id, origLang);
				const QString title = dlg.selectedTitle();
				if (!title.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(f.id, title, dlg.selectedYear());
			}
			m_listView->viewport()->repaint();
			// onPosterReady() calls applyFilter() (beginResetModel) synchronously when
			// the "missing IMDb" filter is active, clearing the scroll position.
			// Restore selection to the nearest surviving row if it was wiped.
			if (m_listView->selectionModel()->selectedIndexes().isEmpty()) {
				const int n = m_listModel->rowCount();
				if (n > 0) {
					const QModelIndex next = m_listModel->index(qMin(firstSelRow, n - 1), 0);
					m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
					m_listView->scrollTo(next);
				}
			}
		});

		menu.addSeparator();

		auto* openFolderAction = menu.addAction(svgIcon(":/icons/folder_open.svg"),
		                                         tr("Open &Containing Folder"));
		connect(openFolderAction, &QAction::triggered, this, [file] {
			QDesktopServices::openUrl(
				QUrl::fromLocalFile(QFileInfo(file.path).absolutePath()));
		});

		menu.addSeparator();

		const QString removeLabel = imdbFiles.size() > 1
		    ? tr("Remove %1 Files from Library…").arg(imdbFiles.size())
		    : tr("Remove from Library…");
		auto* removeAction = menu.addAction(svgIcon(":/icons/delete.svg"), removeLabel);
		connect(removeAction, &QAction::triggered, this, [this, imdbFiles, firstSelRow] {
			const int n = imdbFiles.size();
			const QString title = n > 1
			    ? tr("Remove %1 Files").arg(n)
			    : tr("Remove \"%1\"").arg(imdbFiles.first().filename);
			const QString body = n > 1
			    ? tr("%1 files will be removed from MediaCurator. "
			         "You can re-add them by scanning the folder again.").arg(n)
			    : tr("\"%1\" will be removed from MediaCurator. "
			         "You can re-add it by scanning the folder again.").arg(imdbFiles.first().filename);

			QMessageBox dlg(this);
			dlg.setWindowTitle(title);
			dlg.setText(body);
			dlg.setIcon(QMessageBox::Question);
			auto* deleteBtn = dlg.addButton(tr("Delete File from Disk"), QMessageBox::DestructiveRole);
			auto* removeBtn = dlg.addButton(tr("Remove from Library Only"), QMessageBox::AcceptRole);
			dlg.addButton(QMessageBox::Cancel);
			dlg.setDefaultButton(QMessageBox::Cancel);
			dlg.exec();

			if (dlg.clickedButton() != deleteBtn && dlg.clickedButton() != removeBtn)
				return;

			const bool deleteFromDisk = (dlg.clickedButton() == deleteBtn);
			auto& db = DatabaseManager::instance();
			int removed = 0;
			for (const FileRecord& f : imdbFiles) {
				db.deleteJobsForFile(f.id);
				if (!db.deleteFile(f.id)) continue;
				if (deleteFromDisk) QFile::remove(f.path);
				m_listModel->removeEntry(f.id);
				// Targeted removal — m_jobPanel->refresh() would re-query and re-derive
				// the entire jobs table (posters, streams, kept-track diffs for every
				// job ever run), which is the "feels like a fresh load" stall this avoids.
				m_jobPanel->removeJobsForFile(f.id);
				++removed;
			}
			m_statusLabel->setText(deleteFromDisk
			    ? tr("Deleted %1 file(s) from disk and library").arg(removed)
			    : tr("Removed %1 file(s) from library").arg(removed));
			const int remaining = m_listModel->rowCount();
			if (remaining > 0) {
				const QModelIndex next = m_listModel->index(qMin(firstSelRow, remaining - 1), 0);
				m_listView->selectionModel()->setCurrentIndex(next, QItemSelectionModel::ClearAndSelect);
				m_listView->scrollTo(next);
			}
		});

		menu.exec(m_listView->viewport()->mapToGlobal(pos));
	});

	// ── Filter bar ────────────────────────────────────────────────────────────
	m_filterPanel = new McFilterPanel(this);

	connect(m_filterPanel, &McFilterPanel::filterTextChanged,
	        this, [this](const QString& text) {
		// When clearing, remember the selected card so we can scroll back to it
		qint64 anchorId = -1;
		if (text.isEmpty()) {
			const auto sel = m_listView->selectionModel()->selectedIndexes();
			if (!sel.isEmpty())
				anchorId = sel.first().data(McFileListModel::FileRole).value<FileRecord>().id;
		}
		m_listModel->setFilterText(text);
		if (text.isEmpty() && anchorId >= 0) {
			for (int row = 0; row < m_listModel->rowCount(); ++row) {
				const QModelIndex idx = m_listModel->index(row);
				if (idx.data(McFileListModel::FileRole).value<FileRecord>().id == anchorId) {
					m_listView->setCurrentIndex(idx);
					m_listView->scrollTo(idx, QAbstractItemView::PositionAtCenter);
					break;
				}
			}
		}
	});
	connect(m_filterPanel, &McFilterPanel::filterStatusChanged,
	        this, [this](int status) {
		m_listModel->setStatusFilter(status);
	});
	connect(m_filterPanel, &McFilterPanel::quickFiltersChanged,
	        m_listModel, &McFileListModel::setQuickFilters);
	connect(m_filterPanel, &McFilterPanel::sortOrderChanged,
	        m_listModel, &McFileListModel::setSortOrder);
	connect(m_filterPanel, &McFilterPanel::ratingFilterChanged,
	        m_listModel, &McFileListModel::setRatingFilter);
	connect(m_filterPanel, &McFilterPanel::storageGroupFilterChanged,
	        m_listModel, &McFileListModel::setStorageGroupFilter);

	auto* topWidget = new QWidget(this);
	auto* topLayout = new QVBoxLayout(topWidget);
	topLayout->setContentsMargins(0, 0, 0, 0);
	topLayout->setSpacing(0);

	if (HighscoreClient::instance().isEnabled()) {
		m_highscoreBand = new McHighscoreBand(topWidget);
		m_highscoreBand->setVisible(false);   // hidden until we have data
		topLayout->addWidget(m_highscoreBand);
		connect(m_highscoreBand, &McHighscoreBand::clicked, this, [this] {
			if (!m_highscoreDialog) {
				m_highscoreDialog = new McHighscoreDialog(
				    m_lastHighscoreEntries,
				    AppSettings::instance().value("highscore/playerName").toString(),
				    this);
				m_highscoreDialog->setAttribute(Qt::WA_DeleteOnClose);
				connect(m_highscoreDialog, &QObject::destroyed, this,
				        [this] { m_highscoreDialog = nullptr; });
				connect(m_highscoreDialog, &McHighscoreDialog::refreshRequested,
				        this, [] { HighscoreClient::instance().fetchLeaderboard(); });
			}
			m_highscoreDialog->show();
			m_highscoreDialog->raise();
			m_highscoreDialog->activateWindow();
		});
	}

	topLayout->addWidget(m_filterPanel);
	topLayout->addWidget(m_listView, 1);

	splitter->addWidget(topWidget);

	// ── Job panel (bottom) ────────────────────────────────────────────────────
	m_jobPanel = new McJobPanel(this);
	m_jobPanel->setJobQueue(m_jobQueue);
	m_jobPanel->setTmdbConfigured(!m_profile->tmdbApiKey().isEmpty());
	m_jobPanel->setMultiGroupBadgeEnabled(StorageGroupSettings::multipleGroupsInUse());
	m_jobPanel->setFanartOpacity(
	    AppSettings::instance().value("library/fanartOpacity", 5).toInt() / 100.0);
	m_jobPanel->setMinimumHeight(120);
	splitter->addWidget(m_jobPanel);

	// Sync filter-bar combo widths so they align column-by-column across the splitter.
	// Status combos: both already have their minimum widths set (with different padding
	// for the pill delegate), so take the max.
	// Sort combos: neither has an explicit minimum, so measure via AdjustToContents first.
	{
		const auto syncCombos = [](QComboBox* a, QComboBox* b) {
			const auto pol = QComboBox::AdjustToContents;
			const auto polR = QComboBox::AdjustToContentsOnFirstShow;
			a->setSizeAdjustPolicy(pol);
			b->setSizeAdjustPolicy(pol);
			const int w = qMax(a->sizeHint().width(), b->sizeHint().width());
			a->setMinimumWidth(w);
			b->setMinimumWidth(w);
			a->setSizeAdjustPolicy(polR);
			b->setSizeAdjustPolicy(polR);
		};
		syncCombos(m_filterPanel->statusCombo(), m_jobPanel->statusCombo());
		syncCombos(m_filterPanel->sortCombo(),   m_jobPanel->sortCombo());
	}

	connect(m_jobPanel, &McJobPanel::jobsChanged,
	        this, [this](int) { updateJobPanelVisibility(); });

	connect(m_jobPanel, &McJobPanel::previewRequested,
	        this, &McMainWindow::onShowPreview);

	connect(m_jobPanel, &McJobPanel::playRequested,
	        this, &McMainWindow::launchInVlc);

	connect(m_jobPanel, &McJobPanel::editImdbLinkRequested,
	        this, [this](qint64 fileId) {
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;
		// Prefer DB-saved id so the dialog shows the correct existing link even
		// when no .nfo file is present on disk.
		QString existingId;
		QString exPoster;
		QString exFanart;
		if (const auto pr = DatabaseManager::instance().posterForFile(fileId)) {
			existingId = pr->imdbId;
			exPoster   = pr->imagePath;
			exFanart   = pr->fanartPath;
		}
		if (existingId.isEmpty())
			existingId = NfoParser::readImdbId(fileOpt->path);
		ImdbSearchDialog dlg(fileOpt->path,
		                     smartSuggestedTitle(*fileOpt),
		                     existingId,
		                     m_profile->tmdbApiKey(), this, exPoster, exFanart);
		dlg.setUnderstoodLanguages(m_profile->understoodLanguages());
		if (existingId.isEmpty()) dlg.setAutoSelectSingle(true);
		if (dlg.exec() == QDialog::Accepted) {
			const QString id = dlg.selectedImdbId();
			if (!id.isEmpty()) {
				NfoParser::writeMovieNfo(fileOpt->path, id,
				                        dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(fileId, dlg.selectedPosterPath(), dlg.selectedImageData(), id,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount(),
				                                 dlg.selectedFanartPath());
			if (dlg.selectedVoteAverage() > 0) {
					m_listModel->setRatingForFile(fileId, dlg.selectedVoteAverage());
					m_jobPanel->setRatingForFile(fileId, dlg.selectedVoteAverage());
				}
				const QString origLang = tmdbLangToIso6392(dlg.selectedOriginalLanguage());
				if (!origLang.isEmpty())
					DatabaseManager::instance().updateFileOriginalLanguage(fileId, origLang);
				const QString title = dlg.selectedTitle();
				if (!title.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(fileId, title, dlg.selectedYear());
				m_listView->viewport()->repaint();
			}
		}
	});

	// Batch IMDb link editing: process all selected files in order.
	// Files with 1 search result auto-accept silently; ambiguous files show the
	// dialog with Skip / Cancel-all buttons so the user can exit the loop at any time.
	connect(m_jobPanel, &McJobPanel::editImdbLinksRequested,
	        this, [this](const QList<qint64>& fileIds) {
		const int total = fileIds.size();
		for (int i = 0; i < total; ++i) {
			const qint64 fileId = fileIds[i];
			const auto fileOpt = DatabaseManager::instance().fileById(fileId);
			if (!fileOpt) continue;

			QString existingId;
			QString exPoster;
			QString exFanart;
			if (const auto pr = DatabaseManager::instance().posterForFile(fileId)) {
				existingId = pr->imdbId;
				exPoster   = pr->imagePath;
				exFanart   = pr->fanartPath;
			}
			if (existingId.isEmpty())
				existingId = NfoParser::readImdbId(fileOpt->path);

			ImdbSearchDialog dlg(fileOpt->path,
			                     smartSuggestedTitle(*fileOpt),
			                     existingId,
			                     m_profile->tmdbApiKey(), this, exPoster, exFanart);
			dlg.setUnderstoodLanguages(m_profile->understoodLanguages());
			if (existingId.isEmpty()) dlg.setAutoSelectSingle(true);
			dlg.setBatchMode(i + 1, total);

			const int result = dlg.exec();
			if (result == ImdbSearchDialog::CancelBatch) break;
			if (result != QDialog::Accepted) continue;  // Skip or user cancelled

			const QString id = dlg.selectedImdbId();
			if (!id.isEmpty()) {
				NfoParser::writeMovieNfo(fileOpt->path, id,
				                        dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(fileId, dlg.selectedPosterPath(), dlg.selectedImageData(), id,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount(),
				                                 dlg.selectedFanartPath());
			if (dlg.selectedVoteAverage() > 0) {
					m_listModel->setRatingForFile(fileId, dlg.selectedVoteAverage());
					m_jobPanel->setRatingForFile(fileId, dlg.selectedVoteAverage());
				}
				const QString origLang = tmdbLangToIso6392(dlg.selectedOriginalLanguage());
				if (!origLang.isEmpty())
					DatabaseManager::instance().updateFileOriginalLanguage(fileId, origLang);
				const QString title = dlg.selectedTitle();
				if (!title.isEmpty())
					DatabaseManager::instance().updateDisplayTitle(fileId, title, dlg.selectedYear());
			}
		}
		m_listView->viewport()->repaint();
		m_jobPanel->repaintCards();
	});

	connect(m_jobPanel, &McJobPanel::refreshPosterRequested,
	        this, [](qint64 fileId) {
		PosterManager::instance().refresh(fileId);
	});

	connect(m_jobPanel, &McJobPanel::refreshPosterBatchRequested,
	        this, [](const QList<qint64>& fileIds) {
		PosterManager::instance().refreshBatch(fileIds);
	});

	connect(m_jobPanel, &McJobPanel::downloadSubtitlesRequested,
	        this, [this](qint64 fileId) {
		const auto fileOpt = DatabaseManager::instance().fileById(fileId);
		if (!fileOpt) return;
		const FileRecord& file = *fileOpt;

		if (m_profile->openSubtitlesApiKey().isEmpty()) {
			QMessageBox::information(this, tr("Download Subtitles"),
				tr("No OpenSubtitles API key configured.\n"
				   "Go to Settings → OpenSubtitles to add your key."));
			return;
		}

		QString imdbId;
		if (const auto pr = DatabaseManager::instance().posterForFile(file.id))
			imdbId = pr->imdbId;
		if (imdbId.isEmpty())
			imdbId = NfoParser::readImdbId(file.path);
		if (imdbId.isEmpty()) {
			QMessageBox::information(this, tr("Download Subtitles"),
				tr("No IMDb ID found for \"%1\".\n"
				   "Use \"Edit Movie Metadata\" to identify the movie first.").arg(file.filename));
			return;
		}

		const auto streams  = DatabaseManager::instance().streamsForFile(file.id);
		const auto remaining = streamsRemainingForFile(fileId, streams, m_listModel);
		const QStringList missingIso6392 =
			Mc::missingSubtitleLanguages(remaining, m_profile->understoodLanguages());

		if (missingIso6392.isEmpty()) {
			QMessageBox::information(this, tr("Download Subtitles"),
				tr("All subtitle languages are already covered for \"%1\".").arg(file.filename));
			return;
		}

		const QString filePath = file.path;
		const QString movieTitle = file.displayTitle.isEmpty()
		    ? QFileInfo(filePath).completeBaseName()
		    : (file.displayYear > 0
		        ? QStringLiteral("%1 (%2)").arg(file.displayTitle).arg(file.displayYear)
		        : file.displayTitle);
		auto* dlg = new Mc::McSubtitleDownloadDialog(
			m_profile->openSubtitlesApiKey(),
			m_profile->openSubtitlesUsername(),
			m_profile->openSubtitlesPassword(),
			imdbId, missingIso6392, filePath, file.durationSec,
			m_profile->editionTokens(), m_profile->computeSubtitleMovieHash(),
			streams, movieTitle, this);
		dlg->setAttribute(Qt::WA_DeleteOnClose);
		connect(dlg, &Mc::McSubtitleDownloadDialog::downloadComplete, this,
			[this, fileId, filePath](int downloaded) {
				if (downloaded == 0) return;
				auto& db = DatabaseManager::instance();
				const auto existing = db.streamsForFile(fileId);
				QList<StreamRecord> containerStreams;
				for (const auto& s : existing)
					if (!s.isExternal) containerStreams << s;
				const auto sidecars = ScanWorker::scanSidecarSubtitles(
					filePath, ScanWorker::nextSidecarStreamIndex(containerStreams),
					m_profile->detectSidecarSubtitleLanguage());
				auto allStreams = containerStreams;
				allStreams.append(sidecars);
				db.insertStreams(fileId, allStreams);
				m_jobPanel->refresh();
				m_statusLabel->setText(
					tr("Downloaded %1 subtitle(s) for %2").arg(downloaded)
						.arg(QFileInfo(filePath).fileName()));
			});
		dlg->open();
	});

#ifdef QT_DEBUG
	connect(m_jobPanel, &McJobPanel::debugReviewRequested,
	        this, [this](qint64 jobId) {
		m_jobQueue->debugTriggerReview(jobId);
	});
#endif

	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({ 450, 160 });

	setCentralWidget(splitter);
	applyDarkBackgrounds();
}

void McMainWindow::applyDarkBackgrounds()
{
	// QListView's viewport defaults to a light Base on Windows until the first
	// delegate paint — fill every ancestor with the themed Window/Base colors so
	// the gap between splash.finish() and the first card paint isn't a white flash.
	const QColor win  = palette().color(QPalette::Window);
	const QColor base = palette().color(QPalette::Base);
	const QColor alt  = palette().color(QPalette::AlternateBase);

	auto fill = [](QWidget* w, const QColor& window, const QColor& baseColor,
	               const QColor& altColor) {
		if (!w) return;
		w->setAutoFillBackground(true);
		QPalette pal = w->palette();
		pal.setColor(QPalette::Window, window);
		pal.setColor(QPalette::Base, baseColor);
		pal.setColor(QPalette::AlternateBase, altColor);
		w->setPalette(pal);
	};

	fill(this, win, base, alt);
	fill(m_splitter, win, base, alt);
	fill(m_filterPanel, win, base, alt);
	fill(m_listView, win, base, alt);
	if (m_listView && m_listView->viewport())
		fill(m_listView->viewport(), base, base, alt);
	fill(m_jobPanel, win, base, alt);
	if (auto* mb = menuBar())
		fill(mb, win, base, alt);
	if (auto* sb = statusBar())
		fill(sb, win, base, alt);
	for (auto* tb : findChildren<QToolBar*>())
		fill(tb, win, base, alt);
}

void McMainWindow::attachSplash(QSplashScreen* splash, const QIcon& appIcon)
{
	m_splash      = splash;
	m_startupIcon = appIcon;
	// Queued so this runs on the first app.exec() iteration, not during main().
	QMetaObject::invokeMethod(this, &McMainWindow::dismissSplash, Qt::QueuedConnection);
}

void McMainWindow::dismissSplash()
{
	if (m_splashDismissed)
		return;
	m_splashDismissed = true;

	if (windowState() & Qt::WindowMinimized)
		setWindowState(windowState() & ~Qt::WindowMinimized);

	// Paint the full UI while still off-screen so the first visible frame is
	// already the dark themed surface, not the Win32 default white client area.
	if (!internalWinId())
		winId();
	setNativeWindowBackground();
	flushWidgetRepaints(this);
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
#ifdef Q_OS_WIN
	RedrawWindow(reinterpret_cast<HWND>(winId()), nullptr, nullptr,
	             RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
#endif

	setAttribute(Qt::WA_DontShowOnScreen, false);
	showNormal();
	ensureOnScreen();

	if (m_splash) {
		m_splash->hide();
		m_splash = nullptr;
	}

	raise();
	activateWindow();

	if (!m_startupIcon.isNull())
		setWindowIcon(m_startupIcon);
	if (QWindow* wh = windowHandle())
		wh->setIcon(m_startupIcon);

	QMetaObject::invokeMethod(this, &McMainWindow::completeStartup, Qt::QueuedConnection);
}

void McMainWindow::ensureOnScreen()
{
	QScreen* screen = nullptr;
	if (QWindow* wh = windowHandle())
		screen = wh->screen();
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return;

	const QRect avail = screen->availableGeometry();
	QRect geo = frameGeometry();

	const bool offScreen = !avail.intersects(geo)
	    || geo.width() < minimumWidth() / 2
	    || geo.height() < minimumHeight() / 2;

	if (offScreen) {
		const int w = qBound(minimumWidth(), width(), avail.width() - 80);
		const int h = qBound(minimumHeight(), height(), avail.height() - 80);
		const int x = avail.x() + (avail.width() - w) / 2;
		const int y = avail.y() + (avail.height() - h) / 2;
		setGeometry(x, y, w, h);
	}
}

void McMainWindow::paintEvent(QPaintEvent* event)
{
	// Native Win32 HWND defaults to white until Qt children paint — fill immediately.
	QPainter p(this);
	p.fillRect(event->rect(), palette().color(QPalette::Window));
	QMainWindow::paintEvent(event);
}

bool McMainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
#ifdef Q_OS_WIN
	if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
		const auto* msg = static_cast<MSG*>(message);
		if (msg->message == WM_ERASEBKGND) {
			const QColor bg = palette().color(QPalette::Window);
			if (bg.lightness() < 128) {
				HBRUSH brush = CreateSolidBrush(RGB(bg.red(), bg.green(), bg.blue()));
				RECT rect;
				GetClientRect(msg->hwnd, &rect);
				FillRect(reinterpret_cast<HDC>(msg->wParam), &rect, brush);
				DeleteObject(brush);
				if (result)
					*result = 1;
				return true;
			}
		}
	}
#endif
	return QMainWindow::nativeEvent(eventType, message, result);
}

void McMainWindow::setNativeWindowBackground()
{
#ifdef Q_OS_WIN
	if (!internalWinId())
		return;
	const QColor bg = palette().color(QPalette::Window);
	const COLORREF cref = RGB(bg.red(), bg.green(), bg.blue());
	if (m_nativeBgBrush)
		DeleteObject(static_cast<HBRUSH>(m_nativeBgBrush));
	m_nativeBgBrush = CreateSolidBrush(cref);
	SetClassLongPtr(reinterpret_cast<HWND>(winId()), GCLP_HBRBACKGROUND,
	                reinterpret_cast<LONG_PTR>(m_nativeBgBrush));
#endif
}

void McMainWindow::completeStartup()
{
	if (m_startupCompleteDone)
		return;
	m_startupCompleteDone = true;

	updateJobPanelVisibility(/*forceShow=*/true);

	if (!AppSettings::instance().value("onboarding/seen", false).toBool()) {
		McOnboardingDialog dlg(this);
		dlg.exec();
		AppSettings::instance().setValue("onboarding/seen", true);
	}

	QTimer::singleShot(2000, this, [] { UpdateChecker::instance().check(/*silent=*/true); });

	if (HighscoreClient::instance().isEnabled())
		HighscoreClient::instance().fetchLeaderboard();

	startBackgroundLibraryLoad();
}

void McMainWindow::startBackgroundLibraryLoad()
{
	if (m_backgroundLoadStarted || !m_loadThread)
		return;
	m_backgroundLoadStarted = true;
	m_loadThread->start();
}

void McMainWindow::setupActions()
{
	m_actScanFolder = new QAction(tr("Add Folder…"), this);
	m_actScanFolder->setShortcut(QKeySequence("Ctrl+O"));
	m_actScanFolder->setToolTip(tr("Add a folder to the library and scan it (Ctrl+O)"));
	m_actScanFolder->setIcon(svgIcon(":/icons/folder_open.svg"));
	connect(m_actScanFolder, &QAction::triggered, this, &McMainWindow::onScanFolder);

	m_actScanLibrary = new QAction(tr("Scan Library"), this);
	m_actScanLibrary->setShortcut(QKeySequence("Ctrl+Shift+O"));
	m_actScanLibrary->setToolTip(tr("Rescan all library folders (Ctrl+Shift+O)"));
	m_actScanLibrary->setIcon(svgIcon(":/icons/refresh.svg"));
	connect(m_actScanLibrary, &QAction::triggered, this, &McMainWindow::onScanLibrary);

	m_actQuickScan = new QAction(tr("Quick Scan"), this);
	m_actQuickScan->setShortcut(QKeySequence("Ctrl+Shift+Q"));
	m_actQuickScan->setToolTip(tr("Only look for newly added movie folders — skips anything already "
	                              "in the library, so it finishes much faster than Scan Library "
	                              "(Ctrl+Shift+Q)"));
	m_actQuickScan->setIcon(svgIcon(":/icons/refresh.svg"));
	connect(m_actQuickScan, &QAction::triggered, this, &McMainWindow::onQuickScan);

	m_actLibraryFolders = new QAction(tr("Manage Library Folders…"), this);
	m_actLibraryFolders->setToolTip(tr("View and remove folders from the library database"));
	m_actLibraryFolders->setIcon(svgIcon(":/icons/delete.svg"));
	connect(m_actLibraryFolders, &QAction::triggered, this, &McMainWindow::onRemoveFolder);

	m_actAnalyze = new QAction(tr("Analyze Library"), this);
	m_actAnalyze->setShortcut(QKeySequence("Ctrl+Shift+A"));
	m_actAnalyze->setToolTip(tr("Run rule engine across all files and propose jobs (Ctrl+Shift+A)"));
	m_actAnalyze->setIcon(svgIcon(":/icons/manage_search.svg"));
	connect(m_actAnalyze, &QAction::triggered, this, &McMainWindow::onAnalyzeLibrary);

	m_actQuickAnalyze = new QAction(tr("Quick Analyze"), this);
	m_actQuickAnalyze->setToolTip(tr("Only analyze files that have never been analyzed before — skips "
	                                 "anything that already has a job, so a policy change won't be "
	                                 "picked up here (use Analyze Library for that)"));
	m_actQuickAnalyze->setIcon(svgIcon(":/icons/manage_search.svg"));
	connect(m_actQuickAnalyze, &QAction::triggered, this, &McMainWindow::onQuickAnalyze);

	m_actSimulate = new QAction(tr("Simulate Policy…"), this);
	m_actSimulate->setShortcut(QKeySequence("Ctrl+Shift+W"));
	m_actSimulate->setToolTip(tr("Preview what the current policy would remove — no jobs are created (Ctrl+Shift+W)"));
	m_actSimulate->setIcon(svgIcon(":/icons/visibility.svg"));
	connect(m_actSimulate, &QAction::triggered, this, &McMainWindow::onSimulate);

	m_actSettings = new QAction(tr("Settings…"), this);
	m_actSettings->setToolTip(tr("Edit language preferences and external tool paths"));
	m_actSettings->setIcon(svgIcon(":/icons/settings.svg"));
	connect(m_actSettings, &QAction::triggered, this, &McMainWindow::onSettings);

	m_actRefresh = new QAction(tr("Refresh"), this);
	m_actRefresh->setShortcut(QKeySequence::Refresh);
	m_actRefresh->setToolTip(tr("Reload library from database (F5)"));
	m_actRefresh->setIcon(svgIcon(":/icons/refresh.svg"));
	connect(m_actRefresh, &QAction::triggered, this, &McMainWindow::onRefreshView);

	m_actToggleQueue = new QAction(tr("Job Queue"), this);
	m_actToggleQueue->setCheckable(true);
	m_actToggleQueue->setToolTip(tr("Show / hide the job queue panel"));
	m_actToggleQueue->setIcon(svgIcon(":/icons/playlist_add_check.svg"));
	connect(m_actToggleQueue, &QAction::toggled, this, [this](bool checked) {
		m_jobPanelPinned = !checked;
		AppSettings::instance().setValue("mainWindow/queueHidden", m_jobPanelPinned);
		updateJobPanelVisibility();
	});

	if (HighscoreClient::instance().isEnabled()) {
		m_actToggleHighscore = new QAction(tr("Leaderboard"), this);
		m_actToggleHighscore->setCheckable(true);
		m_actToggleHighscore->setToolTip(tr("Show / hide the leaderboard band"));
		m_actToggleHighscore->setIcon(svgIcon(":/icons/star.svg"));
		connect(m_actToggleHighscore, &QAction::toggled, this, [this](bool checked) {
			m_highscoreBandPinned = !checked;
			AppSettings::instance().setValue("mainWindow/highscoreHidden", m_highscoreBandPinned);
			updateHighscoreVisibility();
		});
	}

}

void McMainWindow::setupToolBar()
{
	auto* tb = addToolBar(tr("Main"));
	tb->setObjectName("mainToolBar");   // needed for saveState/restoreState
	tb->setMovable(false);
	tb->setIconSize({ 24, 24 });
	tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

	{
		const QColor h = tb->palette().color(QPalette::Highlight);
		const auto rgbaStr = [&](int alpha) {
			return QStringLiteral("rgba(%1,%2,%3,%4)").arg(h.red()).arg(h.green()).arg(h.blue()).arg(alpha);
		};
		tb->setStyleSheet(
		    "QToolButton {"
		    "  padding: 4px 8px;"
		    "  border-radius: 4px;"
		    "  border: none;"
		    "}"
		    "QToolButton:hover {"
		    "  background: rgba(128,128,128,50);"
		    "}"
		    "QToolButton:pressed {"
		    "  background: rgba(128,128,128,90);"
		    "}"
		    "QToolButton:checked { background: " + rgbaStr(140) + "; }"
		    "QToolButton:checked:hover { background: " + rgbaStr(170) + "; }");
	}

	tb->addAction(m_actScanFolder);
	tb->addSeparator();
	tb->addAction(m_actScanLibrary);
	tb->addAction(m_actQuickScan);
	tb->addSeparator();
	tb->addAction(m_actAnalyze);
	tb->addAction(m_actQuickAnalyze);
	tb->addSeparator();
	tb->addAction(m_actSimulate);
	tb->addSeparator();
	tb->addAction(m_actToggleQueue);
	if (m_actToggleHighscore)
		tb->addAction(m_actToggleHighscore);
	tb->addSeparator();
	tb->addAction(m_actRefresh);

	// Settings pushed to the far right via a spacer widget
	auto* spacer = new QWidget(this);
	spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	tb->addWidget(spacer);
	tb->addAction(m_actSettings);
}

void McMainWindow::setupMenuBar()
{
	// File menu
	QMenu* fileMenu = menuBar()->addMenu(tr("&File"));
	fileMenu->addAction(m_actScanFolder);
	fileMenu->addSeparator();
	fileMenu->addAction(m_actScanLibrary);
	fileMenu->addAction(m_actQuickScan);
	fileMenu->addSeparator();
	fileMenu->addAction(m_actLibraryFolders);
	fileMenu->addSeparator();
	auto* quitAction = new QAction(svgIcon(":/icons/logout.svg"), tr("&Quit"), this);
	quitAction->setShortcut(QKeySequence::Quit);
	connect(quitAction, &QAction::triggered, this, &QWidget::close);
	fileMenu->addAction(quitAction);

	// View menu
	QMenu* viewMenu = menuBar()->addMenu(tr("&View"));
	viewMenu->addAction(m_actRefresh);
	viewMenu->addSeparator();

	// Job Queue toggle — custom widget draws full-row hover + inset pill when checked,
	// matching the toolbar toggle style without native platform checkmark rendering.
	{
		const QColor h   = palette().color(QPalette::Highlight);
		const QColor hov = h.lighter(175);

		auto* wa     = new QWidgetAction(viewMenu);
		auto* toggle = new McQueueToggle(
		    svgIcon(":/icons/playlist_add_check.svg"),
		    tr("Job Queue"),
		    hov,
		    QColor(h.red(), h.green(), h.blue(), 140),
		    QColor(h.red(), h.green(), h.blue(), 180),
		    viewMenu);
		toggle->setChecked(m_actToggleQueue->isChecked());

		toggle->onToggled = [this, viewMenu](bool on) {
			m_actToggleQueue->setChecked(on);
			viewMenu->hide();
		};

		m_menuQueueBtn = toggle;
		wa->setDefaultWidget(toggle);
		viewMenu->addAction(wa);
	}

	// Leaderboard toggle — same McQueueToggle widget, reused as-is.
	if (m_actToggleHighscore) {
		const QColor h   = palette().color(QPalette::Highlight);
		const QColor hov = h.lighter(175);

		auto* wa     = new QWidgetAction(viewMenu);
		auto* toggle = new McQueueToggle(
		    svgIcon(":/icons/star.svg"),
		    tr("Leaderboard"),
		    hov,
		    QColor(h.red(), h.green(), h.blue(), 140),
		    QColor(h.red(), h.green(), h.blue(), 180),
		    viewMenu);
		toggle->setChecked(m_actToggleHighscore->isChecked());

		toggle->onToggled = [this, viewMenu](bool on) {
			m_actToggleHighscore->setChecked(on);
			viewMenu->hide();
		};

		m_menuHighscoreBtn = toggle;
		wa->setDefaultWidget(toggle);
		viewMenu->addAction(wa);
	}

	// Tools menu
	QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
	toolsMenu->addAction(m_actAnalyze);
	toolsMenu->addAction(m_actQuickAnalyze);
	toolsMenu->addSeparator();
	toolsMenu->addAction(m_actSimulate);
	toolsMenu->addSeparator();
	toolsMenu->addAction(m_actSettings);
#ifdef QT_DEBUG
	toolsMenu->addSeparator();
	auto* calibAct = new QAction(tr("[Debug] Estimation Calibration Data…"), this);
	connect(calibAct, &QAction::triggered, this, [this] {
		auto* dlg = new Mc::McCalibrationDialog(this);
		dlg->exec();
	});
	toolsMenu->addAction(calibAct);
#endif

	// Help menu
	QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
	auto* onboardingAction = new QAction(svgIcon(":/icons/chat.svg"), tr("&Getting Started…"), this);
	connect(onboardingAction, &QAction::triggered, this, [this] {
		McOnboardingDialog dlg(this);
		dlg.exec();
	});
	helpMenu->addAction(onboardingAction);
	helpMenu->addSeparator();
	auto* legendAction = new QAction(svgIcon(":/icons/legend_toggle.svg"), tr("&Legend…"), this);
	connect(legendAction, &QAction::triggered, this, [this] {
		McLegendDialog dlg(this);
		dlg.exec();
	});
	helpMenu->addAction(legendAction);
	helpMenu->addSeparator();
	m_actCheckUpdates = new QAction(svgIcon(":/icons/system_update.svg"), tr("Check for &Updates…"), this);
	connect(m_actCheckUpdates, &QAction::triggered, this, &McMainWindow::onCheckForUpdates);
	helpMenu->addAction(m_actCheckUpdates);
	helpMenu->addSeparator();
	auto* donateMenuAction = new QAction(svgIcon(":/icons/dollar.svg"), tr("&Donate…"), this);
	connect(donateMenuAction, &QAction::triggered, this, &McMainWindow::onDonate);
	helpMenu->addAction(donateMenuAction);
	helpMenu->addSeparator();
	auto* aboutAction = new QAction(tr("&About MediaCurator…"), this);
	aboutAction->setIcon(QApplication::windowIcon());
	connect(aboutAction, &QAction::triggered, this, &McMainWindow::onAbout);
	helpMenu->addAction(aboutAction);

	// Flat hover on all menus — matches the combobox dropdown hover colour.
	const QColor hov = palette().color(QPalette::Highlight).lighter(175);
	const QString baseMenuStyle = QStringLiteral(
	    "QMenu::item:selected { background: %1; }").arg(hov.name());
	fileMenu->setStyleSheet(baseMenuStyle);
	viewMenu->setStyleSheet(baseMenuStyle);
	toolsMenu->setStyleSheet(baseMenuStyle);
	helpMenu->setStyleSheet(baseMenuStyle);

	// Donate button — far right corner of the menu bar
	auto* donateBtn = new QPushButton(svgIcon(":/icons/dollar.svg"), tr("Donate"), this);
	donateBtn->setFlat(true);
	donateBtn->setToolTip(tr("Support MediaCurator"));
	connect(donateBtn, &QPushButton::clicked, this, &McMainWindow::onDonate);
	menuBar()->setCornerWidget(donateBtn, Qt::TopRightCorner);
}

void McMainWindow::setupStatusBar()
{
	m_statusLabel = new QLabel(tr("Ready"), this);
	statusBar()->addWidget(m_statusLabel, 1);

	m_progressBar = new QProgressBar(this);
	m_progressBar->setMaximumWidth(200);
	m_progressBar->setVisible(false);
	statusBar()->addPermanentWidget(m_progressBar);

	m_btnCancelScan = new QPushButton(tr("Cancel Scan"), this);
	m_btnCancelScan->setVisible(false);
	connect(m_btnCancelScan, &QPushButton::clicked, this, [this] {
		for (auto& g : m_scanGroups)
			g.pendingRoots.clear();
		for (auto it = m_scanGroups.begin(); it != m_scanGroups.end(); ++it) {
			if (it->worker)
				it->worker->cancel();
		}
		m_btnCancelScan->setEnabled(false);
		m_btnCancelScan->setText(tr("Cancelling…"));
	});
	statusBar()->addPermanentWidget(m_btnCancelScan);

	m_btnCancelAnalyze = new QPushButton(tr("Cancel Analyze"), this);
	m_btnCancelAnalyze->setVisible(false);
	connect(m_btnCancelAnalyze, &QPushButton::clicked, this, [this] {
		if (m_analyzeWorker) m_analyzeWorker->cancel();
		m_btnCancelAnalyze->setEnabled(false);
		m_btnCancelAnalyze->setText(tr("Cancelling…"));
	});
	statusBar()->addPermanentWidget(m_btnCancelAnalyze);

	m_btnCancelSubtitles = new QPushButton(tr("Cancel Subtitle Downloads"), this);
	m_btnCancelSubtitles->setVisible(false);
	connect(m_btnCancelSubtitles, &QPushButton::clicked, this, [this] {
		SubtitleManager::instance().cancelAll();
		m_btnCancelSubtitles->setEnabled(false);
		m_btnCancelSubtitles->setText(tr("Cancelling…"));
	});
	statusBar()->addPermanentWidget(m_btnCancelSubtitles);

	m_posterProgressBar = new QProgressBar(this);
	m_posterProgressBar->setMaximumWidth(200);
	m_posterProgressBar->setVisible(false);
	statusBar()->addPermanentWidget(m_posterProgressBar);

	m_btnCancelPosterRefresh = new QPushButton(tr("Cancel Poster Refresh"), this);
	m_btnCancelPosterRefresh->setVisible(false);
	connect(m_btnCancelPosterRefresh, &QPushButton::clicked, this, [this] {
		PosterManager::instance().cancelBatch();
		m_btnCancelPosterRefresh->setEnabled(false);
		m_btnCancelPosterRefresh->setText(tr("Cancelling…"));
	});
	statusBar()->addPermanentWidget(m_btnCancelPosterRefresh);

	m_savedLabel = new QLabel(this);
	m_savedLabel->setVisible(false);
	statusBar()->addPermanentWidget(m_savedLabel);
}

void McMainWindow::keyPressEvent(QKeyEvent* event)
{
	// On Windows, Ctrl+F4 maps to QKeySequence::Close. Route it through close()
	// so the active-job guard in closeEvent fires the same as Alt+F4/close button.
	if (event->matches(QKeySequence::Close)) {
		close();
		return;
	}
	QMainWindow::keyPressEvent(event);
}

void McMainWindow::closeEvent(QCloseEvent* event)
{
	if (m_closeHandled) {
		// Windows can re-deliver WM_QUERYENDSESSION/WM_CLOSE while this window is
		// still unwinding after we already accepted the close once (a session-end
		// broadcast can arrive here too once the "Shut Down After" path issues its
		// own shutdown command). Re-running the teardown below on an already-torn-
		// down window is a use-after-free, so just accept and get out.
		event->accept();
		return;
	}

	if (m_jobQueue->hasActiveJob()) {
		QMessageBox msg(this);
		msg.setWindowTitle(tr("Job Running"));
		msg.setText(tr("A job is currently processing.\n\n"
		               "Quiting immediately will interrupt it and may leave a temporary file on disk."));
		auto* pauseBtn = msg.addButton(tr("Quit After"), QMessageBox::AcceptRole);
		auto* shutdownBtn = msg.addButton(tr("Shut Down After"), QMessageBox::ActionRole);
		auto* closeBtn = msg.addButton(tr("Quit Now"), QMessageBox::DestructiveRole);
		/*auto* cancelBtn =*/ msg.addButton(tr("Cancel"), QMessageBox::RejectRole);
		msg.setDefaultButton(pauseBtn);
		msg.exec();

		if (msg.clickedButton() == pauseBtn || msg.clickedButton() == shutdownBtn) {
			// Pause the queue so no new job starts, then close automatically
			// once the current job finishes cleanly.
			m_shutdownOnClose = (msg.clickedButton() == shutdownBtn);
			m_jobQueue->pause();
			// jobFinished fires mid-stack inside JobQueue, before it runs the
			// finished job's own post-processing (rescan, sidecar cleanup,
			// dispatchJobs). Calling close() synchronously from here would tear
			// down MainWindow's threads/managers — and fire the shutdown command
			// — while JobQueue is still using them further down its call stack.
			// Defer to the next event-loop tick instead. Guard against stacking
			// more than one of these if the dialog is triggered again before the
			// job finishes.
			if (!m_closeOnJobFinishPending) {
				m_closeOnJobFinishPending = true;
				connect(m_jobQueue, &JobQueue::jobFinished, this, [this]() {
					m_closeOnJobFinishPending = false;
					QTimer::singleShot(0, this, &McMainWindow::close);
				}, Qt::SingleShotConnection);
			}
			event->ignore();
			return;
		}
		if (msg.clickedButton() != closeBtn) {
			event->ignore();
			return;
		}
		m_jobQueue->cancel();
	}

	// Stop the library loader if it is still paging through the database.
	// cancel() sets an atomic flag; the worker checks it between every file emit.
	// DB pages are fast (< 50 ms each), so 3 s is more than enough.
	if (m_loadThread && m_loadThread->isRunning()) {
		if (m_loader) m_loader->cancel();
		m_loadThread->quit();
		if (!m_loadThread->wait(3000)) {
			m_loadThread->terminate();
			m_loadThread->wait(500);
		}
		// Thread has stopped — safe to delete loader from main thread now.
		delete m_loader;     m_loader     = nullptr;
		delete m_loadThread; m_loadThread = nullptr;
	}

	stopAllScanWorkers(/*waitForThreads=*/true);

	PosterManager::instance().stop();
	SubtitleManager::instance().stop();

	if (m_jobPanel->isVisible()) {
		const QList<int> sz = m_splitter->sizes();
		if (sz.size() > 1 && sz[1] > 0)
			m_savedJobPanelHeight = sz[1];
	}

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("mainWindow/geometry", saveGeometry());
	s.setValue("mainWindow/state",    saveState());
	s.setValue("mainWindow/splitter", m_splitter->saveState());

	AppSettings::instance().setValue("mainWindow/jobPanelHeight", m_savedJobPanelHeight);
	AppSettings::instance().setValue("mainWindow/queueHidden",    m_jobPanelPinned);
	AppSettings::instance().setValue("mainWindow/highscoreHidden", m_highscoreBandPinned);
	AppSettings::instance().setValue("library/sortOrder",         m_filterPanel->sortCombo()->currentData().toInt());

	// The actual OS shutdown command is issued from main(), after app.exec()
	// returns and this window has been fully destroyed — not here. Firing
	// "shutdown /t 0" from inside closeEvent starts the OS session-end sequence
	// while we're still unwinding (threads/managers still stopping above), which
	// can race our own teardown and deliver a second WM_QUERYENDSESSION into a
	// half-destroyed window. shutdownRequested() reports m_shutdownOnClose so
	// main() knows to run it once we're actually gone.
	m_closeHandled = true;
	event->accept();
}

void McMainWindow::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	if (!m_firstShowDone)
		m_firstShowDone = true;
	else
		updateJobPanelVisibility();
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void McMainWindow::onScanFolder()
{
	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();
	const QString hint = roots.isEmpty() ? QString() : roots.last();

	const QString raw = QFileDialog::getExistingDirectory(
		this, tr("Add Media Folder"), hint,
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
	);
	if (raw.isEmpty()) return;
	const QString folder = StorageGroupSettings::normalizedRoot(raw);

	if (!roots.contains(folder))
		roots << folder;
	AppSettings::instance().setValue("scan/roots", roots);

	if (!isScanning()) {
		m_newFilesFound.clear();
		m_scannedSoFar = 0;
	}
	enqueueScanRoot(folder, /*quickScan=*/false);
}

void McMainWindow::onScanLibrary()
{
	if (isScanning()) {
		QMessageBox::information(this, tr("Scan in progress"),
			tr("A scan is already running. Please wait for it to finish."));
		return;
	}

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	if (roots.isEmpty()) {
		onScanFolder();
		return;
	}

	startScanRoots(roots, /*quickScan=*/false);
}

void McMainWindow::onQuickScan()
{
	if (isScanning()) {
		QMessageBox::information(this, tr("Scan in progress"),
			tr("A scan is already running. Please wait for it to finish."));
		return;
	}

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	if (roots.isEmpty()) {
		onScanFolder();
		return;
	}

	startScanRoots(roots, /*quickScan=*/true);
}

void McMainWindow::onRemoveFolder()
{
	McManageFoldersDialog dlg(this);

	// Route Add Folder through the existing scan infrastructure — the dialog
	// is just a view and emits a signal rather than owning its own worker.
	connect(&dlg, &McManageFoldersDialog::folderAdded, this, [this](const QString& path) {
		if (!isScanning()) {
			m_newFilesFound.clear();
			m_scannedSoFar = 0;
		}
		enqueueScanRoot(path, /*quickScan=*/false);
	});

	dlg.exec();

	const bool multiGroup = StorageGroupSettings::multipleGroupsInUse();
	if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
		d->setMultiGroupBadgeEnabled(multiGroup);
	m_jobPanel->setMultiGroupBadgeEnabled(multiGroup);
	// Folder→group reassignment may have changed which groups currently have
	// folders — refresh each panel's storage-group chip row to match live,
	// no restart required.
	m_filterPanel->refreshStorageGroups();
	m_jobPanel->refreshStorageGroups();

	if (dlg.anyRemoved() || dlg.anyAdded())
		onRefreshView();
}

bool McMainWindow::isScanning() const
{
	for (auto it = m_scanGroups.cbegin(); it != m_scanGroups.cend(); ++it) {
		if (it->thread && it->thread->isRunning())
			return true;
	}
	return false;
}

void McMainWindow::startScanRoots(const QStringList& roots, bool quickScan)
{
	m_scanGroups.clear();
	m_newFilesFound.clear();
	m_scannedSoFar = 0;
	m_updatedSoFar = 0;
	m_removedSoFar = 0;

	const QHash<int, QStringList> byGroup = StorageGroupSettings::partitionRootsByGroup(roots);
	for (auto it = byGroup.cbegin(); it != byGroup.cend(); ++it) {
		if (it.value().isEmpty())
			continue;
		ScanGroupState state;
		state.quickScan = quickScan;
		state.pendingRoots = it.value();
		const QString first = state.pendingRoots.takeFirst();
		m_scanGroups.insert(it.key(), state);
		createScanWorkerForGroup(it.key(), first, quickScan);
	}
}

void McMainWindow::enqueueScanRoot(const QString& root, bool quickScan)
{
	const QString norm  = StorageGroupSettings::normalizedRoot(root);
	const int     group = StorageGroupSettings::groupForRoot(norm);

	auto it = m_scanGroups.find(group);
	if (it == m_scanGroups.end()) {
		ScanGroupState state;
		state.quickScan = quickScan;
		m_scanGroups.insert(group, state);
		createScanWorkerForGroup(group, norm, quickScan);
		return;
	}

	if (it->thread && it->thread->isRunning()) {
		it->pendingRoots << norm;
		updateScanStatusLabel();
		return;
	}

	createScanWorkerForGroup(group, norm, quickScan);
}

void McMainWindow::createScanWorkerForGroup(int groupId, const QString& folderPath, bool quickScan)
{
	const QString exeDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
	const QString ffprobePath = exeDir + "/tools/windows/ffprobe.exe";
#elif defined(Q_OS_MACOS)
	const QString ffprobePath = exeDir + "/tools/macos/ffprobe";
#else
	const QString ffprobePath = exeDir + "/tools/linux/ffprobe";
#endif

	ScanGroupState& state = m_scanGroups[groupId];
	state.quickScan = quickScan;

	auto* thread = new QThread(this);
	auto* worker = new ScanWorker(ffprobePath);
	worker->setRootPath(folderPath);
	worker->setQuickScan(quickScan);
	worker->setDetectSubtitleLanguage(m_profile->detectSidecarSubtitleLanguage());
	worker->moveToThread(thread);

	state.thread = thread;
	state.worker = worker;
	state.progressCurrent = 0;
	state.currentFile.clear();

	connect(thread, &QThread::started, worker, &ScanWorker::run);
	connect(worker, &ScanWorker::finished, thread, &QThread::quit);
	connect(worker, &ScanWorker::finished, worker, &QObject::deleteLater);
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);

	connect(worker, &ScanWorker::progress, this,
	        [this, groupId](int current, int total, const QString& file) {
		onScanProgressForGroup(groupId, current, total, file);
	});
	connect(worker, &ScanWorker::finished, this,
	        [this, groupId](int scanned, int added, int updated, int failed, int skipped, int removed,
	                        QStringList newFiles) {
		onScanFinishedForGroup(groupId, scanned, added, updated, failed, skipped, removed,
		                       std::move(newFiles));
	});
	connect(worker, &ScanWorker::fileProcessed,  m_listModel, &McFileListModel::applyFileUpdate);
	connect(worker, &ScanWorker::imdbIdFound,    m_listModel, &McFileListModel::onImdbIdSaved);
	connect(worker, &ScanWorker::fileRemoved,    m_listModel, &McFileListModel::removeEntry);
	connect(worker, &ScanWorker::posterEnqueueRequested, this,
	        [](qint64 id) { PosterManager::instance().enqueue(id); }, Qt::QueuedConnection);
	connect(worker, &ScanWorker::subtitleEnqueueRequested, this,
	        [](qint64 id) { SubtitleManager::instance().enqueue(id); }, Qt::QueuedConnection);
	connect(worker, &ScanWorker::posterEnqueueBatchRequested, this,
	        [](const Mc::FileIdList& ids) { PosterManager::instance().enqueueBatch(ids); },
	        Qt::QueuedConnection);
	connect(worker, &ScanWorker::subtitleEnqueueBatchRequested, this,
	        [](const Mc::FileIdList& ids) { SubtitleManager::instance().enqueueBatch(ids); },
	        Qt::QueuedConnection);

	setScanningState(true);
	thread->start();
	updateScanStatusLabel();
}

void McMainWindow::onScanProgressForGroup(int groupId, int current, int total,
                                          const QString& currentFile)
{
	auto it = m_scanGroups.find(groupId);
	if (it == m_scanGroups.end())
		return;

	it->progressCurrent = current;
	it->currentFile     = currentFile;

	const int sessionCurrent = m_scannedSoFar + current;
	m_progressBar->setMaximum(total);
	m_progressBar->setValue(current);

	if (m_scanGroups.size() == 1) {
		if (total > 0)
			m_statusLabel->setText(tr("Scanning %1/%2: %3")
			                           .arg(sessionCurrent).arg(total).arg(currentFile));
		else
			m_statusLabel->setText(tr("Scanning (%1 found): %2")
			                           .arg(sessionCurrent).arg(currentFile));
	} else {
		updateScanStatusLabel();
	}
}

void McMainWindow::updateScanStatusLabel()
{
	int activeGroups = 0;
	QStringList parts;
	for (auto it = m_scanGroups.cbegin(); it != m_scanGroups.cend(); ++it) {
		if (!it->thread || !it->thread->isRunning())
			continue;
		++activeGroups;
		const QString file = it->currentFile.isEmpty() ? tr("starting…") : it->currentFile;
		parts << tr("Storage %1: %2").arg(it.key()).arg(file);
	}

	if (activeGroups == 0) {
		m_statusLabel->setText(tr("Scanning…"));
		return;
	}
	if (activeGroups == 1 && !parts.isEmpty())
		m_statusLabel->setText(tr("Scanning — %1").arg(parts.first()));
	else
		m_statusLabel->setText(tr("Scanning %1 storage groups — %2")
		                           .arg(activeGroups).arg(parts.join(QStringLiteral("; "))));
}

void McMainWindow::onScanFinishedForGroup(int groupId, int scanned, int /*added*/, int updated,
                                          int /*failed*/, int /*skipped*/, int removed,
                                          QStringList newFiles)
{
	auto it = m_scanGroups.find(groupId);
	if (it == m_scanGroups.end())
		return;

	it->thread = nullptr;
	it->worker = nullptr;
	m_newFilesFound << newFiles;
	m_scannedSoFar += scanned;
	m_updatedSoFar += updated;
	m_removedSoFar += removed;

	if (!it->pendingRoots.isEmpty()) {
		const QString next = it->pendingRoots.takeFirst();
		updateScanStatusLabel();
		createScanWorkerForGroup(groupId, next, it->quickScan);
		return;
	}

	m_scanGroups.erase(it);

	if (!m_scanGroups.isEmpty()) {
		updateScanStatusLabel();
		return;
	}

	setScanningState(false);

	if (m_newFilesFound.isEmpty()) {
		QMessageBox::information(this, tr("Scan Complete"), tr("No new files found."));
	} else {
		QMessageBox box(QMessageBox::Information, tr("Scan Complete"),
			tr("Found %n new file(s).", "", m_newFilesFound.size()), QMessageBox::Ok, this);
		box.setDetailedText(m_newFilesFound.join('\n'));
		box.exec();
	}

	// A scan only invalidates existing jobs when a known file changed or a file
	// was removed (see ScanWorker::deletePendingJobsForFile) — newly discovered
	// files have no jobs yet. Skipping this for a pure "found new files" scan
	// (the common Quick Scan case) avoids a full, unpaged reload and re-render
	// of every job card just to reflect a change that never happened.
	if (m_updatedSoFar > 0 || m_removedSoFar > 0)
		m_jobPanel->refresh();
	updateSavedLabel();

	const int libTotal = m_listModel->totalCount();
	const int shown    = m_listModel->fileCount();
	const int jobCount = DatabaseManager::instance().totalJobCount();
	AppSettings::instance().setValue("library/lastFileCount", libTotal);

	QString text = shown < libTotal
	    ? tr("%1 of %2 files in library").arg(shown).arg(libTotal)
	    : tr("%1 files in library").arg(libTotal);
	if (jobCount > 0)
		text += tr(", %1 in queue").arg(jobCount);
	m_statusLabel->setText(text);
}

void McMainWindow::stopAllScanWorkers(bool waitForThreads)
{
	for (auto it = m_scanGroups.begin(); it != m_scanGroups.end(); ++it) {
		it->pendingRoots.clear();
		if (it->worker)
			it->worker->cancel();
	}

	if (!waitForThreads)
		return;

	for (auto it = m_scanGroups.begin(); it != m_scanGroups.end(); ++it) {
		if (!it->thread || !it->thread->isRunning())
			continue;
		if (!it->thread->wait(8000))
			it->thread->terminate();
		it->thread->wait(1000);
	}
	m_scanGroups.clear();
}

void McMainWindow::onRefreshView()
{
	m_listModel->reload();
	m_jobPanel->refresh();          // emits jobsChanged → updateJobPanelVisibility
	updateSavedLabel();
	updateActionStates();
	if (m_progressBar->isVisible()) return;
	const int shown    = m_listModel->fileCount();
	const int total    = m_listModel->totalCount();
	const int jobCount = DatabaseManager::instance().allJobsForPanel().size();

	QString text = shown < total
	    ? tr("%1 of %2 files").arg(shown).arg(total)
	    : tr("%1 files in library").arg(total);
	if (jobCount > 0)
		text += tr(", %1 in queue").arg(jobCount);
	m_statusLabel->setText(text);
}

void McMainWindow::startLibraryLoader()
{
	static constexpr int kFirstPageSize = 50;  // larger initial sync page for snappier perceived library load

	auto& db = DatabaseManager::instance();

	// Jobs left in 'running' state from a previous crash/force-close: mark failed
	// so the model shows them correctly and the user can retry them.
	db.recoverRunningJobs();

	// Poster/fanart paths must be in the model before the first cards paint —
	// otherwise every card flashes a grey placeholder until LibraryLoader's
	// metaReady arrives on a background thread.
	QHash<qint64, QString> posters, imdbs, fanarts;
	QHash<qint64, double> ratings;
	db.loadPosterMeta(posters, imdbs, ratings, fanarts);
	m_listModel->initMeta(posters, imdbs, db.proposedJobFileIds(), ratings, fanarts);

	// ── First page: library + queue (synchronous, splash still visible) ───────
	// Must use the same sort order the model itself sorts by (persisted setting,
	// already applied above) — otherwise the "first page" is alphabetically-first,
	// not viewport-first, and doesn't match what the model actually shows on top.
	const auto firstFiles = db.allFilesPaged(0, kFirstPageSize, m_listModel->sortOrder());
	{
		if (!firstFiles.isEmpty()) {
			QList<qint64> ids;
			ids.reserve(firstFiles.size());
			for (const auto& f : firstFiles) ids << f.id;
			const auto streams = db.streamsForFiles(ids);
			if (m_listView)
				m_listView->setUpdatesEnabled(false);
			for (const auto& f : firstFiles)
				m_listModel->applyFileUpdate(f, streams.value(f.id));
			if (m_listView)
				m_listView->setUpdatesEnabled(true);
		}
		m_jobPanel->refreshPaged(kFirstPageSize);   // uses batch streams — fast
	}

	if (auto* cardDelegate = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
		cardDelegate->prefetchVisibleArtwork();

	// Promote visible first-page items to the front of the poster/fanart queue.
	// Job queue first, then library — library ends up at the very front since each prepend wins.
	// Reverse order within each list so item[0] lands at the absolute front.
	{
		auto& pm = PosterManager::instance();
		const auto jobFileIds = m_jobPanel->visibleFileIds();
		for (int i = static_cast<int>(jobFileIds.size()) - 1; i >= 0; --i)
			pm.enqueue(jobFileIds.at(i));
		for (int i = static_cast<int>(firstFiles.size()) - 1; i >= 0; --i)
			pm.enqueue(firstFiles.at(i).id);
	}

	updateActionStates();
	updateSavedLabel();

	// Keep the splash painted while the constructor blocks the main thread.
	QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

	// ── Loading progress bar ─────────────────────────────────────────────────
	// Query the total once — a fast COUNT(*) before the background thread starts.
	const int dbTotal = db.fileCount();

	// ── Status bar: live file count + live job count ───────────────────────
	// Both are fast COUNT(*) queries — done here after dbTotal so we share the call.
	{
		const int totalJobs = db.totalJobCount();
		QString text = tr("%1 files in library").arg(dbTotal);
		if (totalJobs > 0)
			text += tr(", %1 in queue").arg(totalJobs);
		m_statusLabel->setText(text);
	}
	if (dbTotal > kFirstPageSize) {
		m_progressBar->setRange(0, dbTotal);
		m_progressBar->setValue(kFirstPageSize);
		m_progressBar->setVisible(true);
	}

	// ── Background thread: meta + remaining files + full queue refresh ────────
	// Use the actual number of files loaded synchronously as the start offset —
	// not kFirstPageSize — so LibraryLoader's total count is correct when the
	// DB has fewer files than one full page (e.g. empty DB → offset 0, total 0).
	m_loadThread = new QThread;   // no parent — we control lifetime explicitly
	m_loader     = new LibraryLoader(qMin(kFirstPageSize, dbTotal), m_listModel->sortOrder());
	m_loader->moveToThread(m_loadThread);

	// Normal-completion cleanup: loader emits finished → thread quits → both
	// schedule their own deletion.  Null member pointers once deleted so
	// closeEvent() does not try to double-cancel.
	connect(m_loadThread, &QThread::started,         m_loader,     &LibraryLoader::run);
	connect(m_loader,     &LibraryLoader::finished,  m_loadThread, &QThread::quit);
	connect(m_loader,     &LibraryLoader::finished,  m_loader,     &QObject::deleteLater);
	connect(m_loadThread, &QThread::finished,        m_loadThread, &QObject::deleteLater);
	connect(m_loader,     &QObject::destroyed,       this, [this]{ m_loader     = nullptr; });
	connect(m_loadThread, &QObject::destroyed,       this, [this]{ m_loadThread = nullptr; });

	// Capture local aliases so the lambdas below don't capture members that
	// may be nulled out before the queued signal is delivered.
	auto* loader = m_loader;
	connect(loader, &LibraryLoader::metaReady, this,
	        [this](const QHash<qint64, QString>& posters,
	               const QHash<qint64, QString>& imdbIds,
	               const QSet<qint64>& filesWithJobs,
	               const QHash<qint64, double>& ratings,
	               const QHash<qint64, QString>& fanartPaths) {
		m_listModel->initMeta(posters, imdbIds, filesWithJobs, ratings, fanartPaths);
		if (auto* cardDelegate = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			cardDelegate->prefetchVisibleArtwork();
	});

	connect(loader, &LibraryLoader::pageReady, this,
	        [this](const QList<Mc::FileRecord>& files, const Mc::FileStreamMap& streams) {
		if (files.isEmpty()) return;
		if (m_listView)
			m_listView->setUpdatesEnabled(false);
		for (const auto& f : files)
			m_listModel->applyFileUpdate(f, streams.value(f.id));
		if (m_listView) {
			m_listView->setUpdatesEnabled(true);
			if (auto* cardDelegate = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
				cardDelegate->scheduleArtworkPrefetch();
			else
				m_listView->viewport()->update();
		}
	});

	if (dbTotal > kFirstPageSize) {
		connect(loader, &LibraryLoader::pageReady, this,
		        [this](const QList<Mc::FileRecord>&, const Mc::FileStreamMap&) {
			m_progressBar->setValue(m_listModel->totalCount());
		});
	}

	connect(loader, &LibraryLoader::finished, this, [this](int total) {
		// Hide the loading bar before touching status text.
		m_progressBar->setVisible(false);
		m_progressBar->setValue(0);

		m_listModel->recomputeFolderCounts();
		m_jobPanel->refresh();
		updateJobPanelVisibility(/*forceShow=*/true);
		updateActionStates();
		updateSavedLabel();
		const int jobTotal = DatabaseManager::instance().totalJobCount();
		AppSettings::instance().setValue("library/lastFileCount", total);
		// Don't overwrite the status label if a remux job is actively running.
		if (!m_jobQueue->hasActiveJob()) {
			const int shown = m_listModel->fileCount();
			QString text = shown < total
			    ? tr("%1 of %2 files").arg(shown).arg(total)
			    : tr("%1 files in library").arg(total);
			if (jobTotal > 0)
				text += tr(", %1 in queue").arg(jobTotal);
			m_statusLabel->setText(text);
		}
	});

	// Thread start is deferred to startBackgroundLibraryLoad() after the window is shown.
}

bool McMainWindow::analyzeSingleFile(qint64 fileId)
{
	auto& db = DatabaseManager::instance();

	const auto fileOpt = db.fileById(fileId);
	if (!fileOpt) return false;

	FileRecord f = *fileOpt;
	const auto streams = db.streamsForFile(f.id);

	if (f.originalLanguage.isEmpty()) {
		const QString lang = OriginalLanguageDetector::detect(f, streams);
		if (!lang.isEmpty()) {
			db.updateFileOriginalLanguage(f.id, lang);
			f.originalLanguage = lang;
		}
	}

	RuleEngine   engine(m_profile);
	ActionEngine actions(ExternalTools::instance().mkvmergePath());

	FileDecision decision = engine.evaluateFile(f, streams);

	// Apply any manual badge overrides the user toggled in the file card.
	const QSet<int> forcedRemovals = m_listModel->forcedRemovalsFor(f.id);
	for (auto& td : decision.tracks) {
		if (forcedRemovals.contains(td.stream.streamIndex)
		        && td.decision == Decision::Keep) {
			td.decision      = Decision::Remove;
			td.reason        = tr("Manually marked for removal");
			td.userOverride  = true;
		}
	}

	// A file with no internal tracks to remove may still need a remux just to
	// absorb an external (sidecar) subtitle into the container — mkvpropedit
	// can't add tracks, so this is the only way that ever happens.
	QList<StreamRecord> unmuxedSidecars;
	if (m_profile->mergeSidecarSubtitles()) {
		for (const StreamRecord& s : streams)
			if (s.isExternal && !s.externalPath.isEmpty()) unmuxedSidecars << s;
	}

	if (decision.removalCount() == 0 && unmuxedSidecars.isEmpty()) return false;

	int audioRemoved = 0, subRemoved = 0;
	for (const auto& td : decision.tracks) {
		if (td.decision != Decision::Remove) continue;
		if (td.stream.codecType == "audio")    ++audioRemoved;
		if (td.stream.codecType == "subtitle") ++subRemoved;
	}

	if (m_profile->skipSubtitleOnlyJobs() && audioRemoved == 0) return false;

	QStringList parts;
	if (audioRemoved > 0) parts << tr("%1 audio").arg(audioRemoved);
	if (subRemoved   > 0) parts << tr("%1 subtitle").arg(subRemoved);
	const QString summary = parts.isEmpty()
		? (unmuxedSidecars.size() == 1
		       ? tr("Mux in external subtitle")
		       : tr("Mux in %1 external subtitles").arg(unmuxedSidecars.size()))
		: tr("Remove ") + parts.join(", ");

	// Build human-readable description of removed tracks
	QStringList descLines;
	for (const auto& td : decision.tracks) {
		if (td.decision != Decision::Remove) continue;
		const StreamRecord& s = td.stream;
		QString line = QStringLiteral("  [%1] %2").arg(s.codecType.toUpper(), s.codecName);
		if (!s.language.isEmpty() && s.language != "und")
			line += QStringLiteral(" (%1)").arg(s.language);
		if (!s.title.isEmpty())
			line += QStringLiteral(" \"%1\"").arg(s.title);
		if (!td.reason.isEmpty())
			line += QStringLiteral(" — %1").arg(td.reason);
		descLines << line;
	}
	for (const StreamRecord& s : unmuxedSidecars) {
		QString line = QStringLiteral("  [SUBTITLE] %1").arg(s.codecName);
		if (!s.language.isEmpty() && s.language != QLatin1String("und"))
			line += QStringLiteral(" (%1)").arg(s.language);
		line += tr(" — external, will be muxed in");
		descLines << line;
	}
	const QString descriptionText = descLines.join('\n');

	const QString     outputPath      = f.path + ".tmp";
	const QStringList args            = actions.buildCommand(decision, outputPath);
	QJsonArray        arr;
	for (const QString& a : args) arr.append(a);

	QString inheritedFlagChanges;
	if (const auto existingJob = db.activeJobForFile(f.id)) {
		if (existingJob->jobType != QLatin1String("tag_edit")) return false;
		inheritedFlagChanges = existingJob->flagChangesJson;
		db.deleteJob(existingJob->id);
	}

	const qint64 estimatedSavings = decision.estimatedSavingBytes();

	JobRecord job;
	job.fileId              = f.id;
	job.status              = "proposed";
	job.jobType             = "remux";
	job.commandArgsJson     = QJsonDocument(arr).toJson(QJsonDocument::Compact);
	job.summary             = summary;
	job.descriptionText     = descriptionText;
	job.savedBytes          = estimatedSavings;
	job.estimatedSavedBytes = estimatedSavings;
	job.streamEstimatesJson = decision.streamEstimatesJson();
	job.flagChangesJson     = inheritedFlagChanges;
	// originalStreamsJson and sidecarDeletionsJson are (re)computed from live data
	// right before the job actually runs (JobQueue::startJob) — a proposed job can
	// sit in the queue long enough for the on-disk sidecar layout to change, so a
	// snapshot frozen at proposal time would go stale.
	(void)db.insertJob(job);
	return true;
}

void McMainWindow::setSubtitleLanguage(const FileRecord& file, const StreamRecord& stream,
                                        const QString& langCode)
{
	auto& db = DatabaseManager::instance();

	if (stream.isExternal) {
		if (stream.externalPath.isEmpty()) return;
		const QString videoBaseName = QFileInfo(file.path).completeBaseName();
		const QString newPath = ActionEngine::insertLanguageIntoSidecarPath(
			stream.externalPath, videoBaseName, langCode);
		if (!QFile::rename(stream.externalPath, newPath)) {
			m_statusLabel->setText(tr("Failed to rename %1")
				.arg(QFileInfo(stream.externalPath).fileName()));
			return;
		}
		// Patch just this one stream row directly rather than doing a full ffprobe
		// rescan — a rescan unconditionally deletes any proposed/queued job for this
		// file (JobQueue::rescanFile), which would destroy the very job we might be
		// trying to update via syncExternalStreamLanguage below.
		db.updateStreamExternalInfo(file.id, stream.streamIndex, langCode, newPath);
		m_listModel->reloadFile(file.id);
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->invalidateSizeCacheFor(file.id);
		m_jobPanel->syncExternalStreamLanguage(file.id, stream.streamIndex, langCode, newPath);
		m_statusLabel->setText(tr("Set subtitle language to %1 for %2")
			.arg(McLanguageFlags::displayName(langCode), file.filename));
		return;
	}

	// Embedded track: apply immediately (mkvpropedit + DB), then re-analyze — the
	// user isn't watching a proposed job's decisions the way they are in the Queue.
	const qint64 fileId      = file.id;
	const QString filename   = file.filename;
	const int    streamIndex = stream.streamIndex;
	TrackFlagService::instance().apply(fileId, streamIndex, QStringLiteral("language"), langCode,
		[this, fileId, filename, langCode](bool ok) {
			if (!ok) {
				m_statusLabel->setText(tr("Failed to set subtitle language for %1").arg(filename));
				return;
			}
			m_listModel->reloadFile(fileId);
			const bool created = analyzeSingleFile(fileId);
			m_jobPanel->refresh();
			m_listModel->refreshJobFilter();
			if (created) {
				updateJobPanelVisibility(/*forceShow=*/true);
				m_jobPanel->scrollToFileJob(fileId);
			}
			m_statusLabel->setText(tr("Set subtitle language to %1 for %2")
				.arg(McLanguageFlags::displayName(langCode), filename));
		});
}

void McMainWindow::onAnalyzeLibrary()
{
	if (m_analyzeThread) return;   // already running

	auto& db = DatabaseManager::instance();
	QList<FileRecord> files;
	for (const auto& f : db.allFiles())
		if (!f.ignored) files << f;
	if (files.isEmpty()) {
		m_statusLabel->setText(tr("No files in library to analyze."));
		return;
	}

	// Pre-fetch forced removals from the UI model — must happen on the main thread
	QHash<qint64, QSet<int>> forcedRemovals;
	for (const auto& file : files) {
		const auto fr = m_listModel->forcedRemovalsFor(file.id);
		if (!fr.isEmpty())
			forcedRemovals.insert(file.id, fr);
	}

	m_analyzeJobCount = 0;
	m_progressBar->setMaximum(files.size());
	m_progressBar->setValue(0);
	m_progressBar->setVisible(true);
	m_actScanFolder->setEnabled(false);
	m_actAnalyze->setEnabled(false);
	m_actQuickAnalyze->setEnabled(false);
	m_btnCancelAnalyze->setEnabled(true);
	m_btnCancelAnalyze->setText(tr("Cancel Analyze"));
	m_btnCancelAnalyze->setVisible(true);

	m_analyzeThread = new QThread(this);
	m_analyzeWorker = new AnalyzeWorker(files, m_profile,
	                                    ExternalTools::instance().mkvmergePath(),
	                                    std::move(forcedRemovals));
	m_analyzeWorker->moveToThread(m_analyzeThread);

	connect(m_analyzeThread, &QThread::started,                m_analyzeWorker, &AnalyzeWorker::run);
	connect(m_analyzeWorker, &AnalyzeWorker::finished, m_analyzeThread,         &QThread::quit);
	connect(m_analyzeWorker, &AnalyzeWorker::finished, m_analyzeWorker,         &QObject::deleteLater);
	connect(m_analyzeThread, &QThread::finished, this, [this] {
		m_analyzeThread->deleteLater();
		m_analyzeThread = nullptr;
	});

	connect(m_analyzeWorker, &AnalyzeWorker::progress,   this, &McMainWindow::onAnalyzeProgress);
	connect(m_analyzeWorker, &AnalyzeWorker::jobProposed, this, &McMainWindow::onAnalyzeJobProposed);
	connect(m_analyzeWorker, &AnalyzeWorker::finished,    this, &McMainWindow::onAnalyzeFinished);

	m_analyzeThread->start();
}

void McMainWindow::onQuickAnalyze()
{
	if (m_analyzeThread) return;   // already running

	auto& db = DatabaseManager::instance();
	QList<FileRecord> files;
	for (const auto& f : db.filesWithoutAnyJob())
		if (!f.ignored) files << f;
	if (files.isEmpty()) {
		m_statusLabel->setText(tr("No unanalyzed files found — everything already has a job."));
		return;
	}

	// Pre-fetch forced removals from the UI model — must happen on the main thread
	QHash<qint64, QSet<int>> forcedRemovals;
	for (const auto& file : files) {
		const auto fr = m_listModel->forcedRemovalsFor(file.id);
		if (!fr.isEmpty())
			forcedRemovals.insert(file.id, fr);
	}

	m_analyzeJobCount = 0;
	m_progressBar->setMaximum(files.size());
	m_progressBar->setValue(0);
	m_progressBar->setVisible(true);
	m_actScanFolder->setEnabled(false);
	m_actAnalyze->setEnabled(false);
	m_actQuickAnalyze->setEnabled(false);
	m_btnCancelAnalyze->setEnabled(true);
	m_btnCancelAnalyze->setText(tr("Cancel Analyze"));
	m_btnCancelAnalyze->setVisible(true);

	m_analyzeThread = new QThread(this);
	m_analyzeWorker = new AnalyzeWorker(files, m_profile,
	                                    ExternalTools::instance().mkvmergePath(),
	                                    std::move(forcedRemovals));
	m_analyzeWorker->moveToThread(m_analyzeThread);

	connect(m_analyzeThread, &QThread::started,                m_analyzeWorker, &AnalyzeWorker::run);
	connect(m_analyzeWorker, &AnalyzeWorker::finished, m_analyzeThread,         &QThread::quit);
	connect(m_analyzeWorker, &AnalyzeWorker::finished, m_analyzeWorker,         &QObject::deleteLater);
	connect(m_analyzeThread, &QThread::finished, this, [this] {
		m_analyzeThread->deleteLater();
		m_analyzeThread = nullptr;
	});

	connect(m_analyzeWorker, &AnalyzeWorker::progress,   this, &McMainWindow::onAnalyzeProgress);
	connect(m_analyzeWorker, &AnalyzeWorker::jobProposed, this, &McMainWindow::onAnalyzeJobProposed);
	connect(m_analyzeWorker, &AnalyzeWorker::finished,    this, &McMainWindow::onAnalyzeFinished);

	m_analyzeThread->start();
}

void McMainWindow::onAnalyzeProgress(int current, int total, const QString& filename)
{
	m_progressBar->setValue(current);
	m_statusLabel->setText(tr("Analyzing %1/%2: %3").arg(current).arg(total).arg(filename));
}

void McMainWindow::onAnalyzeJobProposed(qint64 /*fileId*/)
{
	const bool isFirst = (m_analyzeJobCount == 0);
	++m_analyzeJobCount;
	if (isFirst) {
		// Show the queue immediately on the first hit rather than waiting for
		// the debounce timer or the end of analysis.
		m_jobPanel->refresh();
		m_listModel->refreshJobFilter();
		updateJobPanelVisibility(/*forceShow=*/true);
	}
	// Debounce: collapse subsequent bursts into one refresh 300 ms after the last.
	m_analyzeRefreshTimer->start();
}

void McMainWindow::onAnalyzeFinished(int /*analyzed*/, int created)
{
	m_analyzeWorker = nullptr;
	m_progressBar->setVisible(false);
	m_btnCancelAnalyze->setVisible(false);
	m_actScanFolder->setEnabled(true);
	updateActionStates();

	m_jobPanel->refresh();
	m_listModel->refreshJobFilter();
	updateSavedLabel();
	if (created > 0)
		updateJobPanelVisibility(/*forceShow=*/true);
	m_statusLabel->setText(tr("Analyze complete — %1 job(s) proposed").arg(created));
}

void McMainWindow::onSimulate()
{
	if (m_simulateThread || m_analyzeThread) return;

	const auto files = DatabaseManager::instance().allFiles();
	if (files.isEmpty()) return;

	m_actAnalyze->setEnabled(false);
	m_actSimulate->setEnabled(false);

	m_whatIfDialog = new McWhatIfDialog(files.size(), this);
	m_whatIfDialog->setWindowModality(Qt::ApplicationModal);

	m_simulateThread = new QThread(this);
	m_simulateWorker = new SimulateWorker(files, m_profile);
	m_simulateWorker->moveToThread(m_simulateThread);

	connect(m_simulateThread, &QThread::started,         m_simulateWorker, &SimulateWorker::run);
	connect(m_simulateWorker, &SimulateWorker::finished, m_simulateThread, &QThread::quit);
	// Worker is deleted here, NOT from SimulateWorker::finished. Connecting deleteLater()
	// to finished() is a direct connection (same thread), so the object is freed on the
	// worker thread before the queued onSimulateFinished slot on the main thread runs and
	// reads decisions() — use-after-free. QThread::finished fires only after the thread
	// has fully exited, by which point onSimulateFinished has already been delivered.
	connect(m_simulateThread, &QThread::finished, this, [this] {
		if (m_simulateWorker) {
			m_simulateWorker->deleteLater();
			m_simulateWorker = nullptr;
		}
		m_simulateThread->deleteLater();
		m_simulateThread = nullptr;
	});

	connect(m_simulateWorker, &SimulateWorker::progress, m_whatIfDialog, &McWhatIfDialog::onProgress);
	connect(m_simulateWorker, &SimulateWorker::finished, this, &McMainWindow::onSimulateFinished);

	// If the user closes the dialog (Cancel / Escape / X), cancel the worker and
	// null the pointer immediately — onSimulateFinished may still fire from the
	// queued signal if the worker had already emitted finished() before the dialog
	// was closed, and a stale pointer would crash.
	connect(m_whatIfDialog, &QDialog::rejected, this, [this] {
		m_whatIfDialog = nullptr;
		if (m_simulateWorker) m_simulateWorker->cancel();
	});

	// "Analyze Library" button in results
	connect(m_whatIfDialog, &McWhatIfDialog::analyzeRequested, this, &McMainWindow::onAnalyzeLibrary);

	m_simulateThread->start();
	m_whatIfDialog->show();
}

void McMainWindow::onSimulateFinished(int /*analyzed*/, int /*filesAffected*/)
{
	// Copy decisions before anything can free the worker (QThread::finished fires later).
	const auto decisions = m_simulateWorker ? m_simulateWorker->decisions() : QList<FileDecision>{};
	if (m_whatIfDialog && !m_whatIfDialog->isHidden())
		m_whatIfDialog->onFinished(decisions);
	// m_simulateWorker is intentionally not nulled here — QThread::finished owns cleanup.
	m_whatIfDialog = nullptr;
	updateActionStates();
}

void McMainWindow::setScanningState(bool scanning)
{
	m_progressBar->setVisible(scanning);
	m_btnCancelScan->setVisible(scanning);
	if (scanning) {
		m_btnCancelScan->setEnabled(true);
		m_btnCancelScan->setText(tr("Cancel Scan"));
	} else {
		m_progressBar->setValue(0);
	}
	// Disable scan/analyze actions while scanning; restore proper conditional state when done
	m_actScanFolder->setEnabled(!scanning);
	m_actLibraryFolders->setEnabled(!scanning);
	if (scanning) {
		m_actScanLibrary->setEnabled(false);
		m_actQuickScan->setEnabled(false);
		m_actAnalyze->setEnabled(false);
		m_actQuickAnalyze->setEnabled(false);
		m_actSimulate->setEnabled(false);
	} else {
		updateActionStates();
	}
}

void McMainWindow::updateSavedLabel()
{
	const qint64 total = AppSettings::instance().reclaimedBytes();

	QString text;
	if (total > 0) {
		const double gb = total / 1073741824.0;
		text = gb >= 1.0
		    ? tr("Reclaimed: %1 GB").arg(gb, 0, 'f', 2)
		    : tr("Reclaimed: %1 MB").arg(total / 1048576.0, 0, 'f', 1);
	} else {
		text = tr("Reclaimed: None");
	}
	m_savedLabel->setText(text);
	m_savedLabel->setVisible(true);
}

void McMainWindow::updateRemuxStatusBar()
{
	const int n = m_runningJobNames.size();
	if (n == 0) {
		m_progressBar->setVisible(false);
		return;
	}

	m_progressBar->setVisible(true);

	int finishing = 0;
	int progressSum = 0;
	int progressCount = 0;
	for (auto it = m_runningJobNames.cbegin(); it != m_runningJobNames.cend(); ++it) {
		const int pct = m_runningJobProgress.value(it.key(), 0);
		if (pct >= 100)
			++finishing;
		else {
			progressSum += pct;
			++progressCount;
		}
	}

	if (n == 1) {
		const auto it = m_runningJobNames.cbegin();
		const qint64 jobId = it.key();
		const QString name = it.value();
		const int pct = m_runningJobProgress.value(jobId, 0);
		if (pct >= 100) {
			m_progressBar->setRange(0, 0);
			m_statusLabel->setText(tr("Finishing '%1'").arg(name));
#ifdef Q_OS_WIN
			if (m_taskbar)
				m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_INDETERMINATE);
#endif
		} else {
			m_progressBar->setRange(0, 100);
			m_progressBar->setValue(pct);
			m_statusLabel->setText(tr("Processing '%1'").arg(name));
#ifdef Q_OS_WIN
			setTaskbarProgress(pct);
#endif
		}
		return;
	}

	if (finishing == n) {
		m_progressBar->setRange(0, 0);
		m_statusLabel->setText(tr("Finishing %1 jobs").arg(n));
#ifdef Q_OS_WIN
		if (m_taskbar)
			m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_INDETERMINATE);
#endif
		return;
	}

	const int avgPct = progressCount > 0 ? progressSum / progressCount : 0;
	m_progressBar->setRange(0, 100);
	m_progressBar->setValue(avgPct);
#ifdef Q_OS_WIN
	setTaskbarProgress(avgPct);
#endif

	m_statusLabel->setText(tr("Processing %1 jobs").arg(n));
}

void McMainWindow::updateActionStates()
{
	const bool hasRoots = !AppSettings::instance().value("scan/roots").toStringList().isEmpty();
	const bool hasFiles = m_listModel->totalCount() > 0;
	m_actScanLibrary->setEnabled(hasRoots);
	m_actQuickScan->setEnabled(hasRoots);
	m_actAnalyze->setEnabled(hasFiles);
	m_actQuickAnalyze->setEnabled(hasFiles);
	m_actSimulate->setEnabled(hasFiles);
}

void McMainWindow::updateJobPanelVisibility(bool forceShow)
{
	const bool hasJobs = !DatabaseManager::instance().allJobsForPanel().isEmpty();

	if (forceShow && hasJobs) {
		m_jobPanelPinned = false;
		AppSettings::instance().setValue("mainWindow/queueHidden", false);
	}

	const bool shouldShow = hasJobs && !m_jobPanelPinned;

	if (m_actToggleQueue) {
		QSignalBlocker blocker(m_actToggleQueue);
		m_actToggleQueue->setChecked(shouldShow);
		m_actToggleQueue->setEnabled(hasJobs);
	}
	if (m_menuQueueBtn) {
		auto* toggle = static_cast<McQueueToggle*>(m_menuQueueBtn);
		toggle->setChecked(shouldShow);
		toggle->setEnabled(hasJobs);
	}

	if (shouldShow) {
		const int total = m_splitter->height();
		if (total <= 0) return;   // window not yet laid out — retry on next visibility sync

		const bool wasHidden = !m_jobPanel->isVisible();
		m_jobPanel->setVisible(true);
		if (wasHidden) {
			if (m_splitterRestored) {
				// restoreState already captured the correct sizes — don't overwrite.
				m_splitterRestored = false;
			} else {
				const int bottom = m_savedJobPanelHeight > 0
				                 ? m_savedJobPanelHeight
				                 : qMax(160, total / 4);
				m_splitter->setSizes({ total - bottom, bottom });
			}
		}
	} else if (!shouldShow && m_jobPanel->isVisible()) {
		const QList<int> sz = m_splitter->sizes();
		if (sz.size() > 1 && sz[1] > 0)
			m_savedJobPanelHeight = sz[1];
		m_jobPanel->setVisible(false);
	}
}

qint64 McMainWindow::currentReclaimedMb() const
{
	return AppSettings::instance().reclaimedBytes() / (1024 * 1024);
}

void McMainWindow::submitHighscoreIfDue()
{
	qint64 mb = currentReclaimedMb();
	if (mb < 1)
		return;

	QString name = AppSettings::instance().value("highscore/playerName").toString();
	if (name.isEmpty()) {
		if (m_highscoreDeclinedThisSession)
			return;   // don't nag every debounce firing this run
		bool ok = false;
		name = QInputDialog::getText(this, tr("Join the Leaderboard"),
		    tr("You've reclaimed %1 MB so far!\n"
		       "Enter a name to appear on MediaCurator's public leaderboard:").arg(mb),
		    QLineEdit::Normal, QString(), &ok).trimmed();
		if (!ok || name.isEmpty()) {
			m_highscoreDeclinedThisSession = true;
			return;
		}
		name.truncate(20);
		AppSettings::instance().setValue("highscore/playerName", name);
	}

	// Clamp against this player's last dreamlo-posted score (cached from the
	// most recent fetch) — generous enough that a legitimate one-time bulk
	// cleanup still climbs the board, but bounds how far a tampered local
	// counter can jump the shared leaderboard in a single submission. No
	// previous entry (first-ever submission) means nothing to clamp against.
	// Tune upward as real usage data comes in from larger libraries.
	constexpr qint64 kMaxHighscoreJumpMb = 500LL * 1024;   // 500 GB/submission
	for (const HighscoreEntry& e : m_lastHighscoreEntries) {
		if (e.name.compare(name, Qt::CaseInsensitive) == 0) {
			if (mb > e.score + kMaxHighscoreJumpMb) {
				qWarning() << "Highscore: clamping implausible jump from" << e.score
				           << "to" << mb << "MB for" << name;
				mb = e.score + kMaxHighscoreJumpMb;
			}
			break;
		}
	}

	HighscoreClient::instance().submitScore(name, mb);
}

void McMainWindow::onHighscoreLeaderboardReady(QList<HighscoreEntry> entries)
{
	m_lastHighscoreEntries = entries;   // already sorted by HighscoreClient

	// Recovery floor: if our local counter reads 0 but we've clearly submitted
	// under this name before (a name is only ever saved once a real score has
	// been submitted), the local integrity check most likely reset it as a
	// false positive — restore from the last value dreamlo has on record
	// rather than leaving a real user's history erased.
	const QString name = AppSettings::instance().value("highscore/playerName").toString();
	if (!name.isEmpty() && AppSettings::instance().reclaimedBytes() == 0) {
		for (const HighscoreEntry& e : entries) {
			if (e.name.compare(name, Qt::CaseInsensitive) == 0 && e.score > 0) {
				AppSettings::instance().restoreReclaimedBytes(e.score * 1024LL * 1024LL);
				break;
			}
		}
	}

	if (m_highscoreBand)
		m_highscoreBand->setEntries(entries.mid(0, 5));
	if (m_highscoreDialog)
		m_highscoreDialog->setEntries(entries);
	updateHighscoreVisibility();
}

void McMainWindow::updateHighscoreVisibility()
{
	if (!HighscoreClient::instance().isEnabled())
		return;   // UI was never created

	const bool hasData    = !m_lastHighscoreEntries.isEmpty();
	const bool shouldShow = hasData && !m_highscoreBandPinned;

	if (m_actToggleHighscore) {
		QSignalBlocker blocker(m_actToggleHighscore);
		m_actToggleHighscore->setChecked(shouldShow);
		m_actToggleHighscore->setEnabled(hasData);
	}
	if (m_menuHighscoreBtn) {
		auto* toggle = static_cast<McQueueToggle*>(m_menuHighscoreBtn);
		toggle->setChecked(shouldShow);
		toggle->setEnabled(hasData);
	}
	if (m_highscoreBand)
		m_highscoreBand->setVisible(shouldShow);
}

void McMainWindow::launchInVlc(const QString& rawPath)
{
	const QUrl url = QUrl::fromLocalFile(rawPath);
	const QString vlc = ExternalTools::instance().vlcPath();
	if (!vlc.isEmpty()) {
		QProcess::startDetached(vlc, { url.toString() });
		return;
	}
	QDesktopServices::openUrl(url);
}

void McMainWindow::onSettings()
{
	McSettingsDialog dlg(m_profile, this);

	// Applies to both the library cards and the job queue cards (separate
	// delegate instance) — used both for the live drag preview below and to
	// restore/commit the value once the dialog closes.
	auto applyFanartOpacity = [this](double opacity) {
		if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
			d->setFanartOpacity(opacity);
		m_jobPanel->setFanartOpacity(opacity);
	};
	connect(&dlg, &McSettingsDialog::fanartOpacityChanged, this, applyFanartOpacity);

	dlg.exec();

	// Not part of UserProfile — re-read regardless of Accepted/Rejected since
	// AppSettings has no change signal to drive this automatically. On Accept
	// this matches what was just live-previewed; on Cancel it reverts the
	// preview back to whatever was saved before the dialog opened.
	applyFanartOpacity(AppSettings::instance().value("library/fanartOpacity", 5).toInt() / 100.0);
}

void McMainWindow::onAbout()
{
	auto* dlg = new QDialog(this);
	dlg->setWindowTitle(tr("About MediaCurator"));
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setFixedWidth(480);

	auto* mainLayout = new QVBoxLayout(dlg);
	mainLayout->setSpacing(0);
	mainLayout->setContentsMargins(0, 0, 0, 0);

	// ── Cinematic banner ──────────────────────────────────────────────────────
	constexpr int BW = 480, BH = 170;
	constexpr int BSTRIP = 16;

	QPixmap banner(BW, BH);
	banner.fill(QColor(0x0d, 0x0e, 0x1a));
	{
		QPainter bp(&banner);
		bp.setRenderHint(QPainter::Antialiasing);
		bp.setRenderHint(QPainter::SmoothPixmapTransform);

		// 1. Base gradient
		{
			QLinearGradient bg(0, 0, 0, BH);
			bg.setColorAt(0.0, QColor(0x16, 0x18, 0x2e));
			bg.setColorAt(0.5, QColor(0x0f, 0x10, 0x20));
			bg.setColorAt(1.0, QColor(0x08, 0x09, 0x13));
			bp.fillRect(0, 0, BW, BH, bg);
		}

		// 2. Poster mosaic (same as splash — graceful no-op when cache is empty)
		{
			const QString posterDir = QStandardPaths::writableLocation(
				QStandardPaths::AppDataLocation) + "/posters";
			const QStringList files = QDir(posterDir).entryList({"*.jpg"}, QDir::Files);

			if (!files.isEmpty()) {
				constexpr int PW = 52, PH = 78, STEP = PW + 4;
				const int cols = BW / STEP + 2;
				const int needed = cols * 2;

				QList<QPixmap> pool;
				pool.reserve(needed);
				for (int i = 0; pool.size() < needed && i < needed * 2; ++i) {
					QPixmap pm(posterDir + "/" + files[i % files.size()]);
					if (!pm.isNull())
						pool.append(pm.scaled(PW, PH, Qt::IgnoreAspectRatio,
						                      Qt::SmoothTransformation));
				}

				if (!pool.isEmpty()) {
					const struct { int y; int xOff; } rows[2] = {
						{ -6,       0 },
						{ 88,  -STEP / 2 },
					};
					bp.setOpacity(0.15);
					int pi = 0;
					for (const auto& row : rows)
						for (int c = 0; c < cols; ++c, ++pi)
							bp.drawPixmap(row.xOff + c * STEP, row.y, pool[pi % pool.size()]);
					bp.setOpacity(1.0);
				}
			}
		}

		// 3. Dark overlay
		{
			QLinearGradient ov(0, 0, 0, BH);
			ov.setColorAt(0.0, QColor(0x0d, 0x0e, 0x1a, 205));
			ov.setColorAt(0.5, QColor(0x0d, 0x0e, 0x1a, 155));
			ov.setColorAt(1.0, QColor(0x0d, 0x0e, 0x1a, 205));
			bp.fillRect(0, 0, BW, BH, ov);
		}

		// 4. Radial vignette
		{
			QRadialGradient vig(BW / 2.0, BH / 2.0, BW * 0.65);
			vig.setColorAt(0.25, QColor(0, 0, 0,   0));
			vig.setColorAt(1.00, QColor(0, 0, 0, 200));
			bp.fillRect(0, 0, BW, BH, vig);
		}

		// 5. Film strips
		{
			const QColor stripBg(0x06, 0x06, 0x0f, 235);
			const QColor holeCol(0x1c, 0x1e, 0x34);   // visibly lighter than strip
			const QColor edgeCol(0x2a, 0x2a, 0x46, 160);
			constexpr int HOLE_W = 8, HOLE_H = 12, HOLE_R = 2, HOLE_STEP = 26;

			for (int side = 0; side < 2; ++side) {
				const int sx = (side == 0) ? 0 : BW - BSTRIP;
				bp.setPen(Qt::NoPen);
				bp.setBrush(stripBg);
				bp.drawRect(sx, 0, BSTRIP, BH);

				bp.setPen(QPen(edgeCol, 1));
				const int ex = (side == 0) ? BSTRIP : BW - BSTRIP - 1;
				bp.drawLine(ex, 0, ex, BH);

				bp.setPen(Qt::NoPen);
				bp.setBrush(holeCol);
				const int hx = sx + (BSTRIP - HOLE_W) / 2;
				for (int y = 1; y + HOLE_H <= BH; y += HOLE_STEP)
					bp.drawRoundedRect(hx, y, HOLE_W, HOLE_H, HOLE_R, HOLE_R);
			}
		}

		// 6. Film grain (stable seed)
		{
			QRandomGenerator rng(0x4d43'5241u);
			for (int i = 0; i < 2500; ++i)
				bp.setPen(QColor(255, 255, 255, rng.bounded(4, 12))),
				bp.drawPoint(rng.bounded(BW), rng.bounded(BH));
		}

		// 7. App icon with glow
		{
			constexpr int SZ = 56, IY = 12;
			const int IX = (BW - SZ) / 2;

			QRadialGradient glow(BW / 2.0, IY + SZ / 2.0, SZ * 0.9);
			glow.setColorAt(0.0, QColor(0x4a, 0x68, 0xd8, 55));
			glow.setColorAt(1.0, QColor(0, 0, 0, 0));
			bp.fillRect(IX - SZ / 2, IY - SZ / 2, SZ * 2, SZ * 2, glow);

			QSvgRenderer icon(QStringLiteral(":/icons/app_icon.svg"));
			icon.render(&bp, QRectF(IX, IY, SZ, SZ));
		}

		// 8. Title + accent line
		{
			bp.setFont(QFont("Segoe UI", 22, QFont::Light));
			bp.setPen(QColor(0xe8, 0xe8, 0xff));
			bp.drawText(QRect(BSTRIP, 78, BW - BSTRIP * 2, 36),
			            Qt::AlignHCenter | Qt::AlignVCenter, "MediaCurator");

			// Sample of the Job Queue card's live size bar: green (mkvmerge progress,
			// complete) fills the full width, blue (output bytes vs. original size)
			// sits on top and stops short by the saved fraction, exposing a green sliver.
			constexpr int LW = 190, LH = 3;
			const int lx = (BW - LW) / 2;
			constexpr int ly = 118;
			constexpr double kSavedFraction = 0.10;
			bp.fillRect(lx, ly, LW, LH, QColor(0x30, 0x30, 0x40));
			bp.fillRect(lx, ly, LW, LH, QColor(0x5a, 0xe8, 0x5a));
			bp.fillRect(lx, ly, qRound(LW * (1.0 - kSavedFraction)), LH, QColor(0x64, 0xb4, 0xf0));
		}

		// 9. Version + brand
		{
			bp.setFont(QFont("Segoe UI", 9));
			bp.setPen(QColor(0x72, 0x72, 0xa8));
			bp.drawText(QRect(BSTRIP, 124, BW - BSTRIP * 2, 22),
			            Qt::AlignHCenter | Qt::AlignVCenter,
			            QString("v%1  ·  Bleze Software")
			                .arg(QCoreApplication::applicationVersion()));
		}

		bp.end();
	}

	auto* bannerLabel = new QLabel(dlg);
	bannerLabel->setPixmap(banner);
	bannerLabel->setFixedSize(BW, BH);
	mainLayout->addWidget(bannerLabel);

	// ── Info section ──────────────────────────────────────────────────────────
	auto* infoLayout = new QVBoxLayout;
	infoLayout->setContentsMargins(20, 16, 20, 8);
	infoLayout->setSpacing(6);

	auto* descLabel = new QLabel(
		tr("Scan your video library with ffprobe, apply smart policy rules to identify "
		   "redundant audio and subtitle tracks, then use MKVToolNix to losslessly "
		   "strip them — no re-encoding, no quality loss."), dlg);
	descLabel->setWordWrap(true);
	infoLayout->addWidget(descLabel);

	infoLayout->addSpacing(6);

	auto makeRow = [&](const QString& key, const QString& val) {
		auto* lbl = new QLabel(
			QString("<span style='color:#8888aa;'>%1</span>  %2").arg(key, val), dlg);
		lbl->setTextFormat(Qt::RichText);
		infoLayout->addWidget(lbl);
	};
	makeRow(tr("Author:"),        tr("Jacob Pedersen — Bleze Software"));
	makeRow(tr("License:"),       tr("Apache 2.0 — open source, free to use and modify"));
	makeRow(tr("Built with:"),    QString("Qt %1  ·  SQLite  ·  nlohmann/json").arg(qVersion()));
	makeRow(tr("Bundled tools:"), tr("ffprobe (LGPL)  ·  mkvmerge, mkvpropedit (GPL, MKVToolNix)"));

	mainLayout->addLayout(infoLayout);

	// ── Close button ──────────────────────────────────────────────────────────
	auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	auto* btnLayout = new QHBoxLayout;
	btnLayout->setContentsMargins(16, 4, 16, 14);
	btnLayout->addStretch();
	btnLayout->addWidget(btnBox);
	mainLayout->addLayout(btnLayout);

	btnBox->button(QDialogButtonBox::Close)->setFocus();
	dlg->exec();
}

void McMainWindow::onDonate()
{
	auto* dlg = new QDialog(this);
	dlg->setWindowTitle(tr("Support MediaCurator"));
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setFixedWidth(460);

	auto* layout = new QVBoxLayout(dlg);
	layout->setSpacing(10);
	layout->setContentsMargins(16, 16, 16, 16);

	auto* intro = new QLabel(
		tr("If MediaCurator saves you money on storage or just makes managing "
		   "your media library less painful, consider saying thanks or supporting "
		   "continued development."), dlg);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	layout->addSpacing(8);

	auto* columns = new QHBoxLayout;
	columns->setSpacing(16);

	// Build the badge first so we can measure its exact height for PayPal alignment
	auto* preferredBadge = new QLabel(tr("★  Preferred"), dlg);
	preferredBadge->setStyleSheet(QStringLiteral(
		"color: white; background-color: #F7931A; border-radius: 4px;"
		" padding: 3px 12px; font-weight: bold; border: none;"));
	preferredBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	preferredBadge->ensurePolished();
	// PayPal title must start at: badge height + 1px panel border + 8px panel top margin
	const int paypalTopOffset = preferredBadge->sizeHint().height() + 1 + 8;

	// ---- PayPal column ----
	auto* paypalCol = new QVBoxLayout;
	paypalCol->setSpacing(0);
	paypalCol->addSpacing(paypalTopOffset);

	auto* paypalLabel = new QLabel(tr("PayPal"), dlg);
	paypalLabel->setStyleSheet("font-weight: bold;");
	paypalLabel->setAlignment(Qt::AlignHCenter);
	paypalCol->addWidget(paypalLabel);
	paypalCol->addSpacing(6);

	auto* paypalQrLabel = new QLabel(dlg);
	{
		QPixmap qr(":/icons/paypal_qr.png");
		if (!qr.isNull())
			paypalQrLabel->setPixmap(qr.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	paypalQrLabel->setAlignment(Qt::AlignHCenter);
	paypalCol->addWidget(paypalQrLabel);
	paypalCol->addSpacing(6);

	const QString paypalUrl = QStringLiteral("https://paypal.me/blezedk");
	auto* paypalRow = new QHBoxLayout;
	auto* paypalEdit = new QLineEdit(QStringLiteral("paypal.me/blezedk"), dlg);
	paypalEdit->setReadOnly(true);
	paypalEdit->setFrame(false);
	paypalEdit->setStyleSheet("background: transparent;");
	paypalRow->addWidget(paypalEdit, 1);
	auto* paypalCopyBtn = new QPushButton(svgIcon(":/icons/copy.svg"), QString{}, dlg);
	paypalCopyBtn->setFixedSize(32, 26);
	paypalCopyBtn->setToolTip(tr("Copy to clipboard"));
	connect(paypalCopyBtn, &QPushButton::clicked, [paypalUrl]() {
		QApplication::clipboard()->setText(paypalUrl);
	});
	paypalRow->addWidget(paypalCopyBtn);
	auto* paypalOpenBtn = new QPushButton(svgIcon(":/icons/link.svg"), QString{}, dlg);
	paypalOpenBtn->setFixedSize(32, 26);
	paypalOpenBtn->setToolTip(tr("Open in browser"));
	connect(paypalOpenBtn, &QPushButton::clicked, [paypalUrl]() {
		QDesktopServices::openUrl(QUrl(paypalUrl));
	});
	paypalRow->addWidget(paypalOpenBtn);
	paypalCol->addLayout(paypalRow);
	paypalCol->addStretch();

	columns->addLayout(paypalCol, 1);

	// ---- Lightning column (preferred) ----
	// Badge sits directly above the panel with 0 spacing → appears to rest on the border
	auto* lightningOuter = new QVBoxLayout;
	lightningOuter->setSpacing(0);

	auto* badgeRow = new QHBoxLayout;
	badgeRow->addStretch();
	badgeRow->addWidget(preferredBadge);
	badgeRow->addStretch();
	lightningOuter->addLayout(badgeRow);

	auto* lightningPanel = new QWidget(dlg);
	lightningPanel->setObjectName("lightningPanel");
	lightningPanel->setStyleSheet(
		"#lightningPanel { border: 1px solid #F7931A; border-radius: 8px; }");

	auto* lightningCol = new QVBoxLayout(lightningPanel);
	lightningCol->setSpacing(6);
	lightningCol->setContentsMargins(8, 8, 8, 8);

	auto* lightningLabel = new QLabel(tr("⚡ Lightning"), lightningPanel);
	lightningLabel->setStyleSheet("font-weight: bold; border: none;");
	lightningLabel->setAlignment(Qt::AlignHCenter);
	lightningCol->addWidget(lightningLabel);

	auto* qrLabel = new QLabel(lightningPanel);
	{
		QPixmap qr(":/icons/lightning_qr.png");
		qrLabel->setPixmap(qr.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	qrLabel->setAlignment(Qt::AlignHCenter);
	lightningCol->addWidget(qrLabel);

	auto* lightningRow = new QHBoxLayout;
	auto* lightningEdit = new QLineEdit(QStringLiteral("bleze@cake.cash"), lightningPanel);
	lightningEdit->setReadOnly(true);
	lightningEdit->setFrame(false);
	lightningEdit->setStyleSheet("background: transparent; border: none;");
	lightningRow->addWidget(lightningEdit, 1);
	auto* lightningCopyBtn = new QPushButton(svgIcon(":/icons/copy.svg"), QString{}, lightningPanel);
	lightningCopyBtn->setFixedSize(32, 26);
	lightningCopyBtn->setToolTip(tr("Copy to clipboard"));
	connect(lightningCopyBtn, &QPushButton::clicked, [lightningEdit]() {
		QApplication::clipboard()->setText(lightningEdit->text());
	});
	lightningRow->addWidget(lightningCopyBtn);
	lightningCol->addLayout(lightningRow);
	lightningCol->addStretch();

	lightningOuter->addWidget(lightningPanel, 1);
	columns->addLayout(lightningOuter, 1);

	layout->addLayout(columns);
	layout->addStretch();

	auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
	connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
	layout->addWidget(btnBox);

	btnBox->button(QDialogButtonBox::Close)->setFocus();
	dlg->exec();
}

void McMainWindow::onCheckForUpdates()
{
	m_actCheckUpdates->setEnabled(false);
	UpdateChecker::instance().check(/*silent=*/false);
}

void McMainWindow::onUpdateAvailable(QString version, QString htmlUrl, QString releaseNotes,
                                      QString installerUrl, bool /*silent*/)
{
	m_actCheckUpdates->setEnabled(true);

	QMessageBox box(this);
	box.setIcon(QMessageBox::Information);
	box.setWindowTitle(tr("Update Available"));
	box.setText(tr("MediaCurator %1 is available (you have %2).")
	                .arg(version, QCoreApplication::applicationVersion()));
	if (!releaseNotes.isEmpty())
		box.setDetailedText(releaseNotes);
	QAbstractButton* updateBtn = nullptr;
	if (!installerUrl.isEmpty())
		updateBtn = box.addButton(tr("Update Now"), QMessageBox::AcceptRole);
	auto* viewBtn = box.addButton(tr("View Release"), QMessageBox::ActionRole);
	auto* skipBtn = box.addButton(tr("Skip This Version"), QMessageBox::DestructiveRole);
	box.addButton(tr("Remind Me Later"), QMessageBox::RejectRole);
	box.exec();

	if (updateBtn && box.clickedButton() == updateBtn) {
		m_updateProgressDlg = new QProgressDialog(
		    tr("Downloading MediaCurator %1…").arg(version),
		    tr("Cancel"), 0, 100, this);
		m_updateProgressDlg->setWindowModality(Qt::WindowModal);
		m_updateProgressDlg->setMinimumDuration(0);
		m_updateProgressDlg->setAutoClose(false);
		m_updateProgressDlg->setAutoReset(false);
		connect(m_updateProgressDlg, &QProgressDialog::canceled, this, [] {
			UpdateChecker::instance().cancelDownload();
		});
		m_updateProgressDlg->show();
		UpdateChecker::instance().downloadAndInstall(installerUrl);
	} else if (box.clickedButton() == viewBtn) {
		QDesktopServices::openUrl(QUrl(htmlUrl));
	} else if (box.clickedButton() == skipBtn) {
		UpdateChecker::instance().skipVersion(version);
	}
}

void McMainWindow::onUpdateDownloadProgress(qint64 received, qint64 total)
{
	if (!m_updateProgressDlg) return;
	if (total > 0) {
		m_updateProgressDlg->setMaximum(static_cast<int>(total / 1024));
		m_updateProgressDlg->setValue(static_cast<int>(received / 1024));
	}
}

void McMainWindow::onUpdateDownloadFailed(QString error)
{
	if (m_updateProgressDlg) {
		m_updateProgressDlg->close();
		m_updateProgressDlg->deleteLater();
		m_updateProgressDlg = nullptr;
	}
	QMessageBox::warning(this, tr("Update Failed"), error);
}

void McMainWindow::onUpdateInstallerLaunched()
{
	if (m_updateProgressDlg) {
		m_updateProgressDlg->close();
		m_updateProgressDlg->deleteLater();
		m_updateProgressDlg = nullptr;
	}
	// The elevated installer is starting; close now so it isn't blocked by files
	// (the .exe itself, DLLs) this process still has open. close() runs the
	// normal closeEvent cleanup path (stopping SubtitleManager/PosterManager/
	// JobQueue) and quits the app since this is the last window.
	close();
}

void McMainWindow::onUpdateUpToDate(bool silent)
{
	m_actCheckUpdates->setEnabled(true);
	if (silent) return;
	QMessageBox::information(this, tr("Check for Updates"),
		tr("You're running the latest version (%1).").arg(QCoreApplication::applicationVersion()));
}

void McMainWindow::onUpdateCheckFailed(QString error, bool silent)
{
	m_actCheckUpdates->setEnabled(true);
	if (silent) return;
	QMessageBox::warning(this, tr("Check for Updates"),
		tr("Could not check for updates:\n%1").arg(error));
}

void McMainWindow::onShowPreview(qint64 fileId)
{
	auto& db = DatabaseManager::instance();

	const auto fileOpt = db.fileById(fileId);
	if (!fileOpt) return;

	const auto streams = db.streamsForFile(fileId);

	FileRecord f = *fileOpt;
	if (f.originalLanguage.isEmpty()) {
		const QString lang = OriginalLanguageDetector::detect(f, streams);
		if (!lang.isEmpty()) {
			db.updateFileOriginalLanguage(f.id, lang);
			f.originalLanguage = lang;
		}
	}

	RuleEngine engine(m_profile);
	const FileDecision decision = engine.evaluateFile(f, streams);

	QString flagChangesJson;
	if (const auto job = db.activeJobForFile(fileId))
		flagChangesJson = job->flagChangesJson;

	McPreviewDialog dlg(decision, flagChangesJson, this);
	dlg.exec();
}

// ── Windows taskbar progress ──────────────────────────────────────────────────

#ifdef Q_OS_WIN
void McMainWindow::setTaskbarProgress(int value, int total)
{
	if (!m_taskbar) {
		if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
		                             IID_ITaskbarList3,
		                             reinterpret_cast<void**>(&m_taskbar)))) {
			m_taskbar = nullptr;
			return;
		}
		m_taskbar->HrInit();
	}
	const HWND hwnd = reinterpret_cast<HWND>(winId());
	m_taskbar->SetProgressState(hwnd, TBPF_NORMAL);
	m_taskbar->SetProgressValue(hwnd, static_cast<ULONGLONG>(value),
	                                   static_cast<ULONGLONG>(total));
}

void McMainWindow::clearTaskbarProgress()
{
	if (!m_taskbar) return;
	m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_NOPROGRESS);
}
#endif

} // namespace Mc
