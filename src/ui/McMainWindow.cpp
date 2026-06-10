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
#include "ui/McPreviewDialog.h"
#include "ui/McSettingsDialog.h"
#include "engine/ActionEngine.h"
#include "engine/AnalyzeWorker.h"
#include "engine/SimulateWorker.h"
#include "ui/McWhatIfDialog.h"
#include "engine/JobQueue.h"
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
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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

	// Library is a static/informational view — badge toggle is handled in the job queue.
	// pressed still fires so handlePress can route play-button clicks.
	connect(m_listView, &QAbstractItemView::pressed,
	        this, [this, fileDelegate](const QModelIndex& idx) {
		if (!idx.isValid()) return;
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
		ImdbSearchDialog dlg(file.path, suggested, existing, m_profile->tmdbApiKey(), this);
		if (existing.isEmpty()) dlg.setAutoSelectSingle(true);
		if (dlg.exec() == QDialog::Accepted) {
			const QString id = dlg.selectedImdbId();
			if (!id.isEmpty()) {
				NfoParser::writeMovieNfo(file.path, id, dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(file.id, dlg.selectedPosterPath(), dlg.selectedImageData(), id,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount());
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
			}
			m_statusLabel->setText(created
			    ? tr("Proposed job for %1").arg(file.filename)
			    : tr("No removals found for %1").arg(file.filename));
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
		    ? tr("Edit &IMDb Links (%1 files)…").arg(imdbFiles.size())
		    : tr("Edit &IMDb Link…");
		auto* imdbAction = menu.addAction(svgIcon(":/icons/link.svg"), imdbLabel);
		connect(imdbAction, &QAction::triggered, this, [this, imdbFiles, firstSelRow] {
			const int total = imdbFiles.size();
			for (int i = 0; i < total; ++i) {
				const FileRecord& f = imdbFiles[i];
				const QString suggested = smartSuggestedTitle(f);
				QString existing;
				if (const auto pr = DatabaseManager::instance().posterForFile(f.id))
					existing = pr->imdbId;
				if (existing.isEmpty()) existing = NfoParser::readImdbId(f.path);
				ImdbSearchDialog dlg(f.path, suggested, existing, m_profile->tmdbApiKey(), this);
				if (existing.isEmpty()) dlg.setAutoSelectSingle(true);
				if (total > 1) dlg.setBatchMode(i + 1, total);
				const int result = dlg.exec();
				if (result == ImdbSearchDialog::CancelBatch) break;
				if (result != QDialog::Accepted) continue;
				const QString imdbId = dlg.selectedImdbId();
				if (imdbId.isEmpty()) continue;
				NfoParser::writeMovieNfo(f.path, imdbId, dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(f.id, dlg.selectedPosterPath(), dlg.selectedImageData(), imdbId,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount());
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
		if (const auto pr = DatabaseManager::instance().posterForFile(fileId))
			existingId = pr->imdbId;
		if (existingId.isEmpty())
			existingId = NfoParser::readImdbId(fileOpt->path);
		ImdbSearchDialog dlg(fileOpt->path,
		                     smartSuggestedTitle(*fileOpt),
		                     existingId,
		                     m_profile->tmdbApiKey(), this);
		if (existingId.isEmpty()) dlg.setAutoSelectSingle(true);
		if (dlg.exec() == QDialog::Accepted) {
			const QString id = dlg.selectedImdbId();
			if (!id.isEmpty()) {
				NfoParser::writeMovieNfo(fileOpt->path, id,
				                        dlg.selectedTitle(), dlg.selectedYear());
				PosterManager::instance().refresh(fileId, dlg.selectedPosterPath(), dlg.selectedImageData(), id,
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount());
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
			if (const auto pr = DatabaseManager::instance().posterForFile(fileId))
				existingId = pr->imdbId;
			if (existingId.isEmpty())
				existingId = NfoParser::readImdbId(fileOpt->path);

			ImdbSearchDialog dlg(fileOpt->path,
			                     smartSuggestedTitle(*fileOpt),
			                     existingId,
			                     m_profile->tmdbApiKey(), this);
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
				                                 dlg.selectedVoteAverage(), dlg.selectedVoteCount());
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

	// Help menu
	QMenu* helpMenu = menuBar()->addMenu(tr("&Help"));
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

void McMainWindow::closeEvent(QCloseEvent* event)
{
	if (m_jobQueue->hasActiveJob()) {
		const auto answer = QMessageBox::question(
		    this, tr("Jobs Running"),
		    tr("A job is currently running. Closing now will interrupt it and may leave a temporary file on disk.\n\nClose anyway?"),
		    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes) {
			event->ignore();
			return;
		}
		m_jobQueue->cancel();
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
	createScanWorker(m_pendingRoots.takeFirst());
}

void McMainWindow::onRemoveFolder()
{
	McManageFoldersDialog dlg(this);

	// Route Add Folder through the existing scan infrastructure — the dialog
	// is just a view and emits a signal rather than owning its own worker.
	connect(&dlg, &McManageFoldersDialog::folderAdded, this, [this](const QString& path) {
		if (m_scanThread && m_scanThread->isRunning())
			m_pendingRoots << path;
		else
			createScanWorker(path);
	});

	dlg.exec();

	if (dlg.anyRemoved() || dlg.anyAdded())
		onRefreshView();
}

void McMainWindow::createScanWorker(const QString& folderPath)
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

void McMainWindow::onScanFinished(int /*scanned*/, int /*added*/, int /*updated*/, int /*failed*/, int /*skipped*/, int /*removed*/)
{
	m_scanThread = nullptr;
	m_scanWorker = nullptr;

	if (!m_pendingRoots.isEmpty()) {
		m_statusLabel->setText(tr("Folder scan done — %1 more to go…").arg(m_pendingRoots.size()));
		createScanWorker(m_pendingRoots.takeFirst());
		return;
	}

	setScanningState(false);

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

	// ── First page: library + queue (synchronous, splash still visible) ───────
	// Keep this block as lean as possible — only the two paged queries.
	{
		const auto firstFiles = db.allFilesPaged(0, kFirstPageSize);
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

	updateActionStates();

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
	auto* thread = new QThread(this);
	// Use the actual number of files loaded synchronously as the start offset —
	// not kFirstPageSize — so LibraryLoader's total count is correct when the
	// DB has fewer files than one full page (e.g. empty DB → offset 0, total 0).
	auto* loader = new LibraryLoader(qMin(kFirstPageSize, dbTotal));
	loader->moveToThread(thread);

	connect(thread, &QThread::started,        loader, &LibraryLoader::run);
	connect(loader, &LibraryLoader::finished, thread, &QThread::quit);
	connect(loader, &LibraryLoader::finished, loader, &QObject::deleteLater);
	connect(thread, &QThread::finished,       thread, &QObject::deleteLater);

	connect(loader, &LibraryLoader::metaReady, this,
	        [this](const QHash<qint64, QString>& posters,
	               const QHash<qint64, QString>& imdbIds,
	               const QSet<qint64>& filesWithJobs,
	               const QHash<qint64, double>& ratings) {
		m_listModel->initMeta(posters, imdbIds, filesWithJobs, ratings);
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

	thread->start();
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

	if (db.hasActiveJobForFile(f.id)) return false;

	JobRecord job;
	job.fileId          = f.id;
	job.status          = "proposed";
	job.jobType         = "remux";
	job.commandArgsJson = QJsonDocument(arr).toJson(QJsonDocument::Compact);
	job.summary         = summary;
	job.descriptionText = descriptionText;
	(void)db.insertJob(job);
	return true;
}

void McMainWindow::onAnalyzeLibrary()
{
	if (m_analyzeThread) return;   // already running

	auto& db = DatabaseManager::instance();
	db.clearJobsByStatus("proposed");

	const auto files = db.allFiles();
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
	if (scanning) {
		m_actScanLibrary->setEnabled(false);
		m_actAnalyze->setEnabled(false);
		m_actSimulate->setEnabled(false);
	} else {
		updateActionStates();
	}
}

void McMainWindow::updateSavedLabel()
{
	qint64 total = 0;
	const auto jobs = DatabaseManager::instance().allJobsForPanel();
	for (const auto& r : jobs)
		if (r.status == "done") total += r.savedBytes;

	if (total > 0) {
		const double gb = total / 1073741824.0;
		const QString text = gb >= 1.0
		    ? tr("Reclaimed: %1 GB").arg(gb, 0, 'f', 2)
		    : tr("Reclaimed: %1 MB").arg(total / 1048576.0, 0, 'f', 1);
		m_savedLabel->setText(text);
		m_savedLabel->setVisible(true);
	} else {
		m_savedLabel->setVisible(false);
	}
}

void McMainWindow::updateActionStates()
{
	const bool hasRoots = !AppSettings::instance().value("scan/roots").toStringList().isEmpty();
	const bool hasFiles = m_listModel->totalCount() > 0;
	const bool hasJobs  = !DatabaseManager::instance().allJobsForPanel().isEmpty();
	m_actScanLibrary->setEnabled(hasRoots);
	m_actAnalyze->setEnabled(hasFiles);
	m_actSimulate->setEnabled(hasFiles);
	if (m_actToggleQueue) m_actToggleQueue->setEnabled(hasJobs);
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
	QMessageBox::about(this, tr("About MediaCurator"),
		tr("<h3>MediaCurator %1</h3>"
		   "<p>Scan your video library with ffprobe, apply smart policy rules "
		   "to identify redundant audio and subtitle tracks, then use MKVToolNix "
		   "to losslessly strip them &mdash; no re-encoding, no quality loss.</p>"
		   "<p><b>Author:</b> Jacob Pedersen &mdash; Bleze Software<br>"
		   "<b>License:</b> Apache 2.0 &mdash; open source, free to use and modify.</p>"
		   "<p><small>"
		   "<b>Built with:</b> Qt %2 &middot; SQLite &middot; nlohmann/json<br>"
		   "<b>Bundled tools:</b> ffprobe (LGPL) &middot; MKVToolNix (GPL)"
		   "</small></p>")
		.arg(QCoreApplication::applicationVersion(), QString(qVersion()))
	);
}

void McMainWindow::onDonate()
{
	auto* dlg = new QDialog(this);
	dlg->setWindowTitle(tr("Support MediaCurator"));
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setFixedWidth(420);

	auto* layout = new QVBoxLayout(dlg);
	layout->setSpacing(10);
	layout->setContentsMargins(16, 16, 16, 16);

	auto* intro = new QLabel(
		tr("If MediaCurator saves you money on storage or just makes managing "
		   "your media library less painful, consider saying thanks or supporting "
		   "continued development."), dlg);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	layout->addSpacing(6);

	auto* ghBtn = new QPushButton(svgIcon(":/icons/link.svg"), tr("GitHub Sponsors"), dlg);
	connect(ghBtn, &QPushButton::clicked, []() {
		QDesktopServices::openUrl(QUrl("https://github.com/sponsors/your-username"));
	});
	layout->addWidget(ghBtn);

	auto* bmcBtn = new QPushButton(svgIcon(":/icons/link.svg"), tr("Buy Me a Coffee"), dlg);
	connect(bmcBtn, &QPushButton::clicked, []() {
		QDesktopServices::openUrl(QUrl("https://buymeacoffee.com/your-username"));
	});
	layout->addWidget(bmcBtn);

	layout->addSpacing(6);

	auto* lightningLabel = new QLabel(tr("⚡ Lightning"), dlg);
	lightningLabel->setStyleSheet("font-weight: bold;");
	layout->addWidget(lightningLabel);

	// QR code — centered, scaled to 180 px
	auto* qrLabel = new QLabel(dlg);
	{
		QPixmap qr(":/icons/lightning_qr.png");
		qrLabel->setPixmap(qr.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	qrLabel->setAlignment(Qt::AlignHCenter);
	layout->addWidget(qrLabel);

	auto* lightningRow = new QHBoxLayout;
	auto* lightningEdit = new QLineEdit(QStringLiteral("bleze@cake.cash"), dlg);
	lightningEdit->setReadOnly(true);
	lightningEdit->setFrame(false);
	lightningEdit->setStyleSheet("background: transparent;");
	lightningRow->addWidget(lightningEdit, 1);
	auto* copyBtn = new QPushButton(tr("Copy"), dlg);
	copyBtn->setFixedWidth(64);
	connect(copyBtn, &QPushButton::clicked, [lightningEdit, copyBtn]() {
		QApplication::clipboard()->setText(lightningEdit->text());
		copyBtn->setText(tr("Copied!"));
		QTimer::singleShot(1500, copyBtn, [copyBtn]() { copyBtn->setText(tr("Copy")); });
	});
	lightningRow->addWidget(copyBtn);
	layout->addLayout(lightningRow);

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

	McPreviewDialog dlg(decision, this);
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
