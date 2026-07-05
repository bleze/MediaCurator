#include "ui/McMainWindow.h"
#include "core/AppSettings.h"
#include "ui/ImdbSearchDialog.h"
#include "ui/McFilterPanel.h"
#include "ui/McManageFoldersDialog.h"
#include "ui/SvgIcon.h"
#include "ui/McFileCardDelegate.h"
#include "ui/McFileListModel.h"
#include "ui/McJobPanel.h"
#include "ui/McLegendDialog.h"
#include "ui/McOnboardingDialog.h"
#include "ui/McPreviewDialog.h"
#include "ui/McJobReviewDialog.h"
#include "ui/McSettingsDialog.h"
#include "engine/ActionEngine.h"
#include "engine/AnalyzeWorker.h"
#include "engine/SimulateWorker.h"
#include "ui/McWhatIfDialog.h"
#include "engine/JobQueue.h"
#include "engine/OpenSubtitlesClient.h"
#include "ui/McSubtitleDownloadDialog.h"
#include "ui/McCalibrationDialog.h"
#include "engine/PosterManager.h"
#include "engine/RuleEngine.h"
#include "engine/TrackDecision.h"
#include "scanner/NfoParser.h"
#include "scanner/OriginalLanguageDetector.h"
#include "scanner/ScanWorker.h"
#include "core/DatabaseManager.h"
#include "core/ExternalTools.h"
#include "core/LibraryLoader.h"
#include "core/UserProfile.h"

#ifdef Q_OS_WIN
#include <shobjidl.h>
#endif

#include <QAbstractItemView>
#include <QComboBox>
#include <QShowEvent>
#include <QTimer>
#include <functional>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QLineEdit>
#include <QCursor>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPixmap>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QSplitter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QStatusBar>
#include <QColor>
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

		// Full-row hover fill — always drawn first so the pill sits on top of it
		if (m_hovered)
			p.fillRect(r, m_hoverColor);

		// Checked pill: inset 1px top/bottom, 2px left/right — brighter when hovered
		if (m_checked) {
			p.setPen(Qt::NoPen);
			p.setBrush(m_hovered ? m_checkedHoverColor : m_checkedColor);
			p.drawRoundedRect(QRectF(r).adjusted(2.0, 1.0, -2.0, -1.0), 3.0, 3.0);
		}

		// Icon (16×16, left-aligned with 8px left padding)
		constexpr int kIconW   = 16;
		constexpr int kLeftPad = 8;
		constexpr int kGap     = 5;
		const QRect iconRect(kLeftPad, (r.height() - kIconW) / 2, kIconW, kIconW);
		m_icon.paint(&p, iconRect);

		// Text
		p.setPen(palette().color(QPalette::Text));
		p.setFont(font());
		const QRect textRect = r.adjusted(kLeftPad + kIconW + kGap, 0, -kLeftPad, 0);
		p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_text);
	}

	void enterEvent(QEnterEvent*) override { m_hovered = true;  update(); }
	void leaveEvent(QEvent*)      override { m_hovered = false; update(); }

	void mousePressEvent(QMouseEvent* e) override
	{
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

} // anonymous namespace

namespace Mc {

McMainWindow::McMainWindow(QWidget* parent)
	: QMainWindow(parent)
{
	setWindowTitle("MediaCurator");
	setMinimumSize(900, 600);

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	const bool firstLaunch = !s.contains("mainWindow/geometry");
	if (!firstLaunch) {
		restoreGeometry(s.value("mainWindow/geometry").toByteArray());
		restoreState(s.value("mainWindow/state").toByteArray());
	}
	// Splitter state is restored after setupUi() creates m_splitter

	m_profile  = new UserProfile(this);
	m_profile->load();

	m_jobQueue = new JobQueue(this);
	m_jobQueue->setWriteJobLog(m_profile->writeJobLog());

	connect(m_profile, &UserProfile::profileChanged, this, [this]() {
		m_jobQueue->setWriteJobLog(m_profile->writeJobLog());
		PosterManager::instance().setTmdbApiKey(m_profile->tmdbApiKey());
	});

	m_savedJobPanelHeight = AppSettings::instance().value("mainWindow/jobPanelHeight", 0).toInt();
	m_jobPanelPinned     = AppSettings::instance().value("mainWindow/queueHidden",    false).toBool();

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
		for (const auto& j : DatabaseManager::instance().allJobsForPanel()) {
			if (j.jobId == jobId) { m_currentJobFilename = j.filename; break; }
		}
		m_queuedAtStart = DatabaseManager::instance().queuedJobCount();
		m_progressBar->setRange(0, 100);
		m_progressBar->setValue(0);
		m_progressBar->setVisible(true);
		const QString suffix = m_queuedAtStart > 0
		    ? tr(" (%1 queued)").arg(m_queuedAtStart) : QString();
		m_statusLabel->setText(tr("Processing '%1'…%2").arg(m_currentJobFilename, suffix));
	});

	connect(m_jobQueue, &JobQueue::progressChanged, this, [this](qint64, int pct) {
		if (pct >= 100) {
			// mkvmerge has finished encoding but is still flushing/closing the
			// output file before the process exits. Switch to an indeterminate
			// animation so the UI doesn't look frozen at 100%.
			m_progressBar->setRange(0, 0);
			const QString finSuffix = m_queuedAtStart > 0
			    ? tr(" (%1 queued)").arg(m_queuedAtStart) : QString();
			m_statusLabel->setText(tr("Finishing '%1'…%2").arg(m_currentJobFilename, finSuffix));
#ifdef Q_OS_WIN
			if (m_taskbar)
				m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_INDETERMINATE);
#endif
		} else {
			m_progressBar->setRange(0, 100);
			m_progressBar->setValue(pct);
#ifdef Q_OS_WIN
			setTaskbarProgress(pct);
#endif
		}
	});

	connect(m_jobQueue, &JobQueue::jobFinished, this, [this](qint64, bool success, qint64) {
		updateSavedLabel();
		m_progressBar->setRange(0, 100);
		m_progressBar->setVisible(false);
		m_progressBar->setValue(0);
		if (!success) {
			m_statusLabel->setText(tr("Job failed for '%1'").arg(m_currentJobFilename));
#ifdef Q_OS_WIN
			if (m_taskbar) {
				m_taskbar->SetProgressState(reinterpret_cast<HWND>(winId()), TBPF_ERROR);
			}
#endif
		} else if (m_jobQueue->isPaused()) {
			// More jobs are waiting but the queue is paused; allFinished won't
			// fire until the user resumes and the last job completes.
			m_statusLabel->setText(tr("Paused — click Start to resume"));
#ifdef Q_OS_WIN
			clearTaskbarProgress();
#endif
		}
		// Non-paused success: runNext() fires synchronously from onJobFinished,
		// so either jobStarted (next job) or allFinished (queue empty) will
		// update the status immediately after this handler returns.
	});

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
		m_currentJobFilename.clear();
		updateSavedLabel();
		const int total    = m_listModel->totalCount();
		const int proposed = DatabaseManager::instance().totalJobCount();
		QString msg = tr("All jobs finished — %1 files in library").arg(total);
		if (proposed > 0)
			msg += tr(", %1 proposed").arg(proposed);
		m_statusLabel->setText(msg);
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
	connect(&pm, &PosterManager::imdbIdSaved,
	        m_listModel, &McFileListModel::onImdbIdSaved);
	connect(&pm, &PosterManager::tmdbDataReady,
	        m_listModel, &McFileListModel::onTmdbDataReady);

	startLibraryLoader();
}

McMainWindow::~McMainWindow()
{
#ifdef Q_OS_WIN
	if (m_taskbar) { m_taskbar->Release(); m_taskbar = nullptr; }
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
		m_listModel->toggleForcedRemoval(fileId, streamIndex);
	});

	connect(m_listView, &QAbstractItemView::pressed,
	        this, [this, fileDelegate](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		if (!(QApplication::mouseButtons() & Qt::LeftButton)) return;
		const QPoint pos = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		fileDelegate->handlePress(pos, m_listView->visualRect(idx), m_listView->font(), idx);
	});
	m_listView->setItemDelegate(fileDelegate);

	// When stream layout changes, the delegate's size cache may hold stale heights.
	// Only clear it for roles that actually affect card height (streams / overrides).
	// Title, year, rating, and poster path never change the badge row count or height.
	connect(m_listModel, &QAbstractItemModel::dataChanged, this,
	        [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
		const bool affectsHeight = roles.isEmpty()
		    || roles.contains(McFileListModel::StreamsRole)
		    || roles.contains(McFileListModel::OverridesRole);
		if (affectsHeight) {
			if (auto* d = qobject_cast<McFileCardDelegate*>(m_listView->itemDelegate()))
				d->clearSizeCache();
		}
	});

	// Double-click on the poster column → open IMDb search dialog
	connect(m_listView, &QAbstractItemView::doubleClicked, this,
	        [this](const QModelIndex& idx) {
		if (!idx.isValid()) return;
		const QPoint pos    = m_listView->viewport()->mapFromGlobal(QCursor::pos());
		const QRect  iRect  = m_listView->visualRect(idx);
		if (pos.x() > iRect.left() + McFileCardDelegate::kPosterW) return;
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
	m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_listView->setAlternatingRowColors(true);
	m_listView->setSpacing(0);
	m_listView->setResizeMode(QListView::Adjust);
	m_listView->setContextMenuPolicy(Qt::CustomContextMenu);

	connect(m_listView, &QListView::customContextMenuRequested,
	        this, [this](const QPoint& pos) {
		const QModelIndex idx = m_listView->indexAt(pos);
		if (!idx.isValid()) return;
		const FileRecord file = idx.data(McFileListModel::FileRole).value<FileRecord>();

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

		auto* refreshPosterAction = menu.addAction(svgIcon(":/icons/refresh.svg"),
		                                            tr("Refresh &Poster"));
		connect(refreshPosterAction, &QAction::triggered, this, [file] {
			PosterManager::instance().refresh(file.id);
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

			// Find which understood languages have no subtitle coverage
			const auto streams = DatabaseManager::instance().streamsForFile(file.id);
			QSet<QString> coveredIso6391;
			for (const auto& s : streams) {
				if (s.codecType != QLatin1String("subtitle")) continue;
				if (s.language.isEmpty() || s.language == QLatin1String("und")) continue;
				const QString c = Mc::iso6392to6391(s.language);
				if (!c.isEmpty()) coveredIso6391.insert(c);
				else if (s.language.length() == 2) coveredIso6391.insert(s.language.toLower());
			}

			QStringList missingIso6392;
			for (const QString& lang6392 : m_profile->understoodLanguages()) {
				if (lang6392 == QLatin1String("mul")) continue;
				const QString lang6391 = Mc::iso6392to6391(lang6392);
				if (!lang6391.isEmpty() && !coveredIso6391.contains(lang6391))
					missingIso6392 << lang6392;
			}

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
				imdbId, missingIso6392, filePath, movieTitle, this);
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
						filePath, containerStreams.size());
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
				++removed;
			}
			m_jobPanel->refresh();
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
		m_listModel->setFilterHasRemovals(status == 1);
		m_listModel->setFilterMissingImdb(status == 2);
		m_listModel->setFilterIgnoredOnly(status == 3);
	});
	connect(m_filterPanel, &McFilterPanel::quickFiltersChanged,
	        m_listModel, &McFileListModel::setQuickFilters);
	connect(m_filterPanel, &McFilterPanel::sortOrderChanged,
	        m_listModel, &McFileListModel::setSortOrder);
	connect(m_filterPanel, &McFilterPanel::ratingFilterChanged,
	        m_listModel, &McFileListModel::setRatingFilter);

	auto* topWidget = new QWidget(this);
	auto* topLayout = new QVBoxLayout(topWidget);
	topLayout->setContentsMargins(0, 0, 0, 0);
	topLayout->setSpacing(0);
	topLayout->addWidget(m_filterPanel);
	topLayout->addWidget(m_listView, 1);

	splitter->addWidget(topWidget);

	// ── Job panel (bottom) ────────────────────────────────────────────────────
	m_jobPanel = new McJobPanel(this);
	m_jobPanel->setJobQueue(m_jobQueue);
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

		const auto streams = DatabaseManager::instance().streamsForFile(file.id);
		QSet<QString> coveredIso6391;
		for (const auto& s : streams) {
			if (s.codecType != QLatin1String("subtitle")) continue;
			if (s.language.isEmpty() || s.language == QLatin1String("und")) continue;
			const QString c = Mc::iso6392to6391(s.language);
			if (!c.isEmpty()) coveredIso6391.insert(c);
			else if (s.language.length() == 2) coveredIso6391.insert(s.language.toLower());
		}

		QStringList missingIso6392;
		for (const QString& lang6392 : m_profile->understoodLanguages()) {
			if (lang6392 == QLatin1String("mul")) continue;
			const QString lang6391 = Mc::iso6392to6391(lang6392);
			if (!lang6391.isEmpty() && !coveredIso6391.contains(lang6391))
				missingIso6392 << lang6392;
		}

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
			imdbId, missingIso6392, filePath, movieTitle, this);
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
					filePath, containerStreams.size());
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

	m_actRemoveFolder = new QAction(tr("Manage Library Folders…"), this);
	m_actRemoveFolder->setToolTip(tr("View and remove folders from the library database"));
	m_actRemoveFolder->setIcon(svgIcon(":/icons/delete.svg"));
	connect(m_actRemoveFolder, &QAction::triggered, this, &McMainWindow::onRemoveFolder);

	m_actAnalyze = new QAction(tr("Analyze Library"), this);
	m_actAnalyze->setShortcut(QKeySequence("Ctrl+Shift+A"));
	m_actAnalyze->setToolTip(tr("Run rule engine across all files and propose jobs (Ctrl+Shift+A)"));
	m_actAnalyze->setIcon(svgIcon(":/icons/manage_search.svg"));
	connect(m_actAnalyze, &QAction::triggered, this, &McMainWindow::onAnalyzeLibrary);

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
	tb->addAction(m_actScanLibrary);
	tb->addAction(m_actQuickScan);
	tb->addAction(m_actAnalyze);
	tb->addAction(m_actSimulate);
	tb->addAction(m_actToggleQueue);
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
	fileMenu->addAction(m_actScanLibrary);
	fileMenu->addAction(m_actQuickScan);
	fileMenu->addAction(m_actRemoveFolder);
	fileMenu->addSeparator();
	auto* quitAction = new QAction(svgIcon(":/icons/logout.svg"), tr("&Quit"), this);
	quitAction->setShortcut(QKeySequence::Quit);
	connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
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

	// Tools menu
	QMenu* toolsMenu = menuBar()->addMenu(tr("&Tools"));
	toolsMenu->addAction(m_actAnalyze);
	toolsMenu->addAction(m_actSimulate);
	toolsMenu->addSeparator();
	toolsMenu->addAction(m_actSettings);
	toolsMenu->addSeparator();
	auto* calibAct = new QAction(tr("Estimation Calibration Data…"), this);
	connect(calibAct, &QAction::triggered, this, [this] {
		auto* dlg = new Mc::McCalibrationDialog(this);
		dlg->exec();
	});
	toolsMenu->addAction(calibAct);

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
		m_pendingRoots.clear();  // don't start the next folder after this one finishes
		if (m_scanWorker) m_scanWorker->cancel();
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
	if (m_jobQueue->hasActiveJob()) {
		QMessageBox msg(this);
		msg.setWindowTitle(tr("Job Running"));
		msg.setText(tr("A job is currently processing.\n\n"
		               "Closing immediately will interrupt it and may leave a temporary file on disk."));
		auto* pauseBtn  = msg.addButton(tr("Pause && Wait"),  QMessageBox::AcceptRole);
		auto* closeBtn  = msg.addButton(tr("Close Anyway"),   QMessageBox::DestructiveRole);
		/*auto* cancelBtn =*/ msg.addButton(tr("Cancel"),     QMessageBox::RejectRole);
		msg.setDefaultButton(pauseBtn);
		msg.exec();

		if (msg.clickedButton() == pauseBtn) {
			// Pause the queue so no new job starts, then close automatically
			// once the current job finishes cleanly.
			m_jobQueue->pause();
			connect(m_jobQueue, &JobQueue::jobFinished, this, [this]() {
				close();
			}, Qt::SingleShotConnection);
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

	// Stop the scan worker before destroying the window.
	// cancel() sets an atomic flag; the worker checks it between files.
	// We wait up to 8 s for the current FFprobe call to finish naturally;
	// only then terminate to avoid leaking a child process.
	if (m_scanThread && m_scanThread->isRunning()) {
		if (m_scanWorker) m_scanWorker->cancel();
		if (!m_scanThread->wait(8000)) {
			m_scanThread->terminate();
			m_scanThread->wait(1000);
		}
	}

	PosterManager::instance().stop();

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
	AppSettings::instance().setValue("library/sortOrder",         m_filterPanel->sortCombo()->currentData().toInt());
	event->accept();
}

void McMainWindow::showEvent(QShowEvent* event)
{
	QMainWindow::showEvent(event);
	if (!m_firstShowDone) {
		m_firstShowDone = true;
		// First show: always reveal the queue if jobs exist — the "pinned hidden"
		// setting is a within-session preference, not a permanent preference.
		updateJobPanelVisibility(/*forceShow=*/true);

		if (!AppSettings::instance().value("onboarding/seen", false).toBool()) {
			QTimer::singleShot(0, this, [this] {
				McOnboardingDialog dlg(this);
				dlg.exec();
				AppSettings::instance().setValue("onboarding/seen", true);
			});
		}
	} else {
		// Un-minimise / screen restore: just sync state, don't override user's choice.
		updateJobPanelVisibility();
	}
}

// ── Slots ─────────────────────────────────────────────────────────────────────

void McMainWindow::onScanFolder()
{
	if (m_scanThread && m_scanThread->isRunning()) {
		QMessageBox::information(this, tr("Scan in progress"),
			tr("A scan is already running. Please wait for it to finish."));
		return;
	}

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();
	const QString hint = roots.isEmpty() ? QString() : roots.last();

	const QString raw = QFileDialog::getExistingDirectory(
		this, tr("Add Media Folder"), hint,
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
	);
	if (raw.isEmpty()) return;
	const QString folder = QDir::fromNativeSeparators(raw);

	if (!roots.contains(folder))
		roots << folder;
	AppSettings::instance().setValue("scan/roots", roots);

	m_newFilesFound.clear();
	createScanWorker(folder);
}

void McMainWindow::onScanLibrary()
{
	if (m_scanThread && m_scanThread->isRunning()) {
		QMessageBox::information(this, tr("Scan in progress"),
			tr("A scan is already running. Please wait for it to finish."));
		return;
	}

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	if (roots.isEmpty()) {
		// No folders configured yet — redirect to Add Folder
		onScanFolder();
		return;
	}

	m_pendingRoots = roots;
	m_quickScanPending = false;
	m_newFilesFound.clear();
	createScanWorker(m_pendingRoots.takeFirst());
}

void McMainWindow::onQuickScan()
{
	if (m_scanThread && m_scanThread->isRunning()) {
		QMessageBox::information(this, tr("Scan in progress"),
			tr("A scan is already running. Please wait for it to finish."));
		return;
	}

	QStringList roots = AppSettings::instance().value("scan/roots").toStringList();

	if (roots.isEmpty()) {
		onScanFolder();
		return;
	}

	m_pendingRoots = roots;
	m_quickScanPending = true;
	m_newFilesFound.clear();
	createScanWorker(m_pendingRoots.takeFirst(), /*quickScan=*/true);
}

void McMainWindow::onRemoveFolder()
{
	McManageFoldersDialog dlg(this);

	// Route Add Folder through the existing scan infrastructure — the dialog
	// is just a view and emits a signal rather than owning its own worker.
	connect(&dlg, &McManageFoldersDialog::folderAdded, this, [this](const QString& path) {
		if (m_scanThread && m_scanThread->isRunning()) {
			m_pendingRoots << path;
		} else {
			m_newFilesFound.clear();
			createScanWorker(path);
		}
	});

	dlg.exec();

	if (dlg.anyRemoved() || dlg.anyAdded())
		onRefreshView();
}

void McMainWindow::createScanWorker(const QString& folderPath, bool quickScan)
{
	const QString exeDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
	const QString ffprobePath = exeDir + "/tools/windows/ffprobe.exe";
#elif defined(Q_OS_MACOS)
	const QString ffprobePath = exeDir + "/tools/macos/ffprobe";
#else
	const QString ffprobePath = exeDir + "/tools/linux/ffprobe";
#endif

	m_scanThread = new QThread(this);
	m_scanWorker = new ScanWorker(ffprobePath);
	m_scanWorker->setRootPath(folderPath);
	m_scanWorker->setQuickScan(quickScan);
	m_scanWorker->moveToThread(m_scanThread);

	connect(m_scanThread, &QThread::started,   m_scanWorker, &ScanWorker::run);
	connect(m_scanWorker, &ScanWorker::finished, m_scanThread, &QThread::quit);
	connect(m_scanWorker, &ScanWorker::finished, m_scanWorker, &QObject::deleteLater);
	connect(m_scanThread, &QThread::finished,  m_scanThread,  &QObject::deleteLater);

	connect(m_scanWorker, &ScanWorker::progress,       this,        &McMainWindow::onScanProgress);
	connect(m_scanWorker, &ScanWorker::finished,       this,        &McMainWindow::onScanFinished);
	connect(m_scanWorker, &ScanWorker::fileProcessed,  m_listModel, &McFileListModel::applyFileUpdate);
	connect(m_scanWorker, &ScanWorker::imdbIdFound,    m_listModel, &McFileListModel::onImdbIdSaved);
	connect(m_scanWorker, &ScanWorker::fileRemoved,    m_listModel, &McFileListModel::removeEntry);

	setScanningState(true);
	m_scanThread->start();
}

void McMainWindow::onScanProgress(int current, int total, const QString& currentFile)
{
	m_progressBar->setMaximum(total);   // 0 → indeterminate animated bar
	m_progressBar->setValue(current);
	if (total > 0)
		m_statusLabel->setText(tr("Scanning %1/%2: %3").arg(current).arg(total).arg(currentFile));
	else
		m_statusLabel->setText(tr("Scanning (%1 found): %2").arg(current).arg(currentFile));
}

void McMainWindow::onScanFinished(int /*scanned*/, int /*added*/, int /*updated*/, int /*failed*/, int /*skipped*/, int /*removed*/, QStringList newFiles)
{
	m_scanThread = nullptr;
	m_scanWorker = nullptr;
	m_newFilesFound << newFiles;

	if (!m_pendingRoots.isEmpty()) {
		m_statusLabel->setText(tr("Folder scan done — %1 more to go…").arg(m_pendingRoots.size()));
		createScanWorker(m_pendingRoots.takeFirst(), m_quickScanPending);
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

	// Model was updated incrementally via fileProcessed/fileRemoved signals — no full reload needed.
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
	static constexpr int kFirstPageSize = 25;

	auto& db = DatabaseManager::instance();

	// Jobs left in 'running' state from a previous crash/force-close: mark failed
	// so the model shows them correctly and the user can retry them.
	db.recoverRunningJobs();

	// ── First page: library + queue (synchronous, splash still visible) ───────
	// Keep this block as lean as possible — only the two paged queries.
	const auto firstFiles = db.allFilesPaged(0, kFirstPageSize);
	{
		if (!firstFiles.isEmpty()) {
			QList<qint64> ids;
			ids.reserve(firstFiles.size());
			for (const auto& f : firstFiles) ids << f.id;
			const auto streams = db.streamsForFiles(ids);
			for (const auto& f : firstFiles)
				m_listModel->applyFileUpdate(f, streams.value(f.id));
		}
		m_jobPanel->refreshPaged(kFirstPageSize);   // uses batch streams — fast
	}

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
	m_loader     = new LibraryLoader(qMin(kFirstPageSize, dbTotal));
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
	});

	connect(loader, &LibraryLoader::fileReady,
	        m_listModel, &McFileListModel::applyFileUpdate);

	// Update progress bar on each file arriving from the background thread.
	// This connection is registered after the model's so m_listModel->totalCount()
	// already reflects the newly added entry when this lambda runs.
	if (dbTotal > kFirstPageSize) {
		connect(loader, &LibraryLoader::fileReady, this,
		        [this](const Mc::FileRecord&, const QList<Mc::StreamRecord>&) {
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

	m_loadThread->start();
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

	if (decision.removalCount() == 0) return false;

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
	const QString summary = tr("Remove ") + parts.join(", ");

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

	JobRecord job;
	job.fileId          = f.id;
	job.status          = "proposed";
	job.jobType         = "remux";
	job.commandArgsJson = QJsonDocument(arr).toJson(QJsonDocument::Compact);
	job.summary         = summary;
	job.descriptionText = descriptionText;
	job.flagChangesJson = inheritedFlagChanges;
	(void)db.insertJob(job);
	return true;
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

	// "Analyze Library →" button in results
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
	m_actRemoveFolder->setEnabled(!scanning);
	if (scanning) {
		m_actScanLibrary->setEnabled(false);
		m_actQuickScan->setEnabled(false);
		m_actAnalyze->setEnabled(false);
		m_actSimulate->setEnabled(false);
	} else {
		updateActionStates();
	}
}

void McMainWindow::updateSavedLabel()
{
	const qint64 total = AppSettings::instance()
	    .value(QStringLiteral("stats/totalReclaimedBytes"), 0LL).toLongLong();

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

void McMainWindow::updateActionStates()
{
	const bool hasRoots = !AppSettings::instance().value("scan/roots").toStringList().isEmpty();
	const bool hasFiles = m_listModel->totalCount() > 0;
	m_actScanLibrary->setEnabled(hasRoots);
	m_actQuickScan->setEnabled(hasRoots);
	m_actAnalyze->setEnabled(hasFiles);
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
	}
	if (m_menuQueueBtn)
		static_cast<McQueueToggle*>(m_menuQueueBtn)->setChecked(shouldShow);

	if (shouldShow) {
		const int total = m_splitter->height();
		if (total <= 0) return;   // window not yet laid out — showEvent will call us again

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
	dlg.exec();
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

			constexpr int LW = 48;
			const int lx = (BW - LW) / 2;
			bp.setPen(QPen(QColor(0x55, 0x78, 0xe8, 185), 1.5));
			bp.drawLine(lx, 119, lx + LW, 119);
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
	makeRow(tr("Bundled tools:"), tr("ffprobe (LGPL)  ·  MKVToolNix (GPL)"));

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
