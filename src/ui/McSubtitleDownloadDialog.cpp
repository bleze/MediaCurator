#include "ui/McSubtitleDownloadDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McLanguageFlags.h"
#include "core/DatabaseManager.h"
#include "engine/OpenSubtitlesClient.h"
#include "engine/SubtitleManager.h"

#include <QClipboard>
#include <QCursor>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

namespace Mc {

// static
QString McSubtitleDownloadDialog::languageDisplayName(const QString& iso6392)
{
	return McLanguageFlags::displayName(iso6392);
}

McSubtitleDownloadDialog::McSubtitleDownloadDialog(const QString& apiKey,
                                                   const QString& username,
                                                   const QString& password,
                                                   const QString& imdbId,
                                                   const QStringList& iso6392Languages,
                                                   const QString& videoPath,
                                                   double durationSec,
                                                   const QStringList& editionTokens,
                                                   bool computeMovieHash,
                                                   const QList<StreamRecord>& existingStreams,
                                                   const QString& movieTitle,
                                                   QWidget* parent)
	: QDialog(parent)
	, m_apiKey(apiKey), m_username(username), m_password(password)
	, m_imdbId(imdbId), m_iso6392Languages(iso6392Languages), m_videoPath(videoPath)
	, m_durationSec(durationSec), m_editionTokens(editionTokens), m_computeMovieHash(computeMovieHash)
	, m_existingStreams(existingStreams)
{
	setWindowTitle(movieTitle.isEmpty()
	    ? tr("Download Subtitles")
	    : tr("Download Subtitles — %1").arg(movieTitle));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(12, 12, 12, 12);

	// ── Status line ───────────────────────────────────────────────────────────
	// Otherwise a slow phase (the reference-subtitle lookup, or a slow search/download)
	// looks identical to a frozen dialog — nothing here changes except this label.
	m_statusLabel = new QLabel(this);
	m_statusLabel->setWordWrap(true);
	// Left visible but blank (rather than hidden) so its line height is already
	// accounted for by setFixedSize() below — the dialog can't grow later to make
	// room for it once shown.
	m_statusLabel->setText(QStringLiteral(" "));
	root->addWidget(m_statusLabel);

	// ── Language table ────────────────────────────────────────────────────────
	m_table = new QTableWidget(iso6392Languages.size(), 3, this);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setFocusPolicy(Qt::NoFocus);
	m_table->setShowGrid(false);
	m_table->setAlternatingRowColors(true);
	m_table->horizontalHeader()->setVisible(false);
	m_table->verticalHeader()->setVisible(false);
	m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
	m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);

	const qreal dpr    = devicePixelRatioF();
	const int   rowH   = qCeil(McCardDelegate::kFlagH + 10);
	const int   flagW  = qCeil(McCardDelegate::kFlagW + 12);
	m_table->setColumnWidth(0, flagW);
	m_table->setColumnWidth(2, 160);

	for (int i = 0; i < iso6392Languages.size(); ++i) {
		const QString& lang6392 = iso6392Languages.at(i);
		const QString   lang6391 = iso6392to6391(lang6392);

		m_table->setRowHeight(i, rowH);

		// Flag
		auto* flagItem = new QTableWidgetItem;
		const QPixmap flag = McLanguageFlags::flag(lang6392, McCardDelegate::kFlagH, dpr);
		if (!flag.isNull()) flagItem->setIcon(QIcon(flag));
		flagItem->setTextAlignment(Qt::AlignCenter);
		m_table->setItem(i, 0, flagItem);

		// Language name
		m_table->setItem(i, 1, new QTableWidgetItem(languageDisplayName(lang6392)));

		// Status — pending
		auto* statusItem = new QTableWidgetItem(QStringLiteral("–"));
		statusItem->setTextAlignment(Qt::AlignCenter);
		statusItem->setForeground(palette().brush(QPalette::Disabled, QPalette::Text));
		m_table->setItem(i, 2, statusItem);

		if (!lang6391.isEmpty())
			m_rowByLang6391[lang6391] = i;
	}

	// Fit table height to content — no scrollbar needed
	int tableH = 4;
	for (int i = 0; i < m_table->rowCount(); ++i)
		tableH += m_table->rowHeight(i);
	m_table->setFixedHeight(tableH);

	root->addWidget(m_table);

	// ── Buttons ───────────────────────────────────────────────────────────────
	auto* btnRow = new QHBoxLayout;
	btnRow->addStretch();

	m_downloadBtn = new QPushButton(tr("Download"), this);
	m_downloadBtn->setDefault(true);
	m_closeBtn = new QPushButton(tr("Cancel"), this);

	btnRow->addWidget(m_downloadBtn);
	btnRow->addWidget(m_closeBtn);
	root->addLayout(btnRow);

	connect(m_downloadBtn, &QPushButton::clicked, this, &McSubtitleDownloadDialog::onDownload);
	connect(m_closeBtn,    &QPushButton::clicked, this, &QDialog::reject);
	connect(m_table, &QTableWidget::cellClicked, this, &McSubtitleDownloadDialog::onStatusCellClicked);

	setFixedWidth(400);
	adjustSize();
	setFixedSize(size());
}

void McSubtitleDownloadDialog::onDownload()
{
	m_downloadBtn->setEnabled(false);
	m_downloadBtn->hide();
	m_downloading = true;
	m_statusLabel->setText(tr("Logging in to OpenSubtitles…"));
	// m_closeBtn stays enabled, reading "Cancel", so the user can abort mid-download.

	QStringList iso6391Languages;
	for (const QString& lang : m_iso6392Languages) {
		const QString l = iso6392to6391(lang);
		if (!l.isEmpty()) iso6391Languages << l;
	}

	m_thread = new QThread(this);
	m_worker = new SubtitleDownloadWorker(
		m_apiKey, m_username, m_password,
		m_imdbId, iso6391Languages, m_videoPath,
		m_durationSec, m_editionTokens, m_computeMovieHash,
		m_existingStreams);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,
	        m_worker, &SubtitleDownloadWorker::run, Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::referenceLookupStarted,
	        this, &McSubtitleDownloadDialog::onReferenceLookupStarted, Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::referenceLookupDone,
	        this, &McSubtitleDownloadDialog::onReferenceLookupDone, Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::languageStarted,
	        this, &McSubtitleDownloadDialog::onLanguageStarted, Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::languageDone,
	        this, &McSubtitleDownloadDialog::onLanguageDone,    Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::done,
	        this, &McSubtitleDownloadDialog::onAllDone,         Qt::QueuedConnection);
	connect(m_worker, &SubtitleDownloadWorker::done,
	        m_worker, &QObject::deleteLater);
	connect(m_worker, &SubtitleDownloadWorker::done,
	        m_thread, &QThread::quit);
	connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
	m_thread->start();
}

void McSubtitleDownloadDialog::onReferenceLookupStarted()
{
	m_statusLabel->setText(tr("Checking existing subtitles for a sync reference…"));
}

void McSubtitleDownloadDialog::onReferenceLookupDone(bool found, const QString& label)
{
	m_statusLabel->setText(found
		? tr("Using %1 as a sync reference").arg(label)
		: tr("No existing subtitle found — matching by runtime only"));
}

void McSubtitleDownloadDialog::onLanguageStarted(const QString& lang6391)
{
	m_statusLabel->setText(tr("Searching %1…").arg(languageDisplayName(iso6391to6392(lang6391))));

	const int row = m_rowByLang6391.value(lang6391, -1);
	if (row < 0) return;
	auto* item = m_table->item(row, 2);
	if (!item) return;
	item->setText(tr("Downloading…"));
	item->setForeground(QColor(0, 100, 200));
	item->setToolTip(QString{});
}

void McSubtitleDownloadDialog::onLanguageDone(const QString& lang6391,
                                               bool success, const QString& message)
{
	const int row = m_rowByLang6391.value(lang6391, -1);
	if (row < 0) return;
	auto* item = m_table->item(row, 2);
	if (!item) return;

	if (success) {
		item->setText(tr("✓ Downloaded"));
		item->setForeground(QColor(0, 150, 0));
		item->setToolTip(message); // which candidate matched, and why — see SubtitleDownloadWorker::run()
	} else {
		const bool notFound = message.contains(QLatin1String("Not found"), Qt::CaseInsensitive);
		item->setText(notFound ? tr("Not found") : tr("✗ Failed"));
		item->setForeground(notFound ? QColor(180, 100, 0) : QColor(200, 0, 0));
		if (!message.isEmpty() && !notFound)
			item->setToolTip(message);
	}
}

void McSubtitleDownloadDialog::onStatusCellClicked(int row, int column)
{
	if (column != 2) return;
	auto* item = m_table->item(row, column);
	if (!item) return;

	const QString text = item->toolTip().isEmpty() ? item->text() : item->toolTip();
	if (text.isEmpty()) return;

	QGuiApplication::clipboard()->setText(text);
	QToolTip::showText(QCursor::pos(), tr("Copied to clipboard"), m_table);
}

void McSubtitleDownloadDialog::onAllDone(int downloaded, int /*failed*/, const QString& statusMsg,
                                          int /*remaining*/, bool quotaExceeded, int retryAfterSecs)
{
	m_downloaded  = downloaded;
	m_downloading = false;
	m_closeBtn->setEnabled(true);
	m_closeBtn->setText(tr("Close"));
	m_statusLabel->setText(statusMsg);
	emit downloadComplete(downloaded);

	// A 429 seen here means the account's daily quota is exhausted regardless of
	// which dialog observed it — pause the background auto-download queue too,
	// using whatever Retry-After wait OpenSubtitles reported (if any).
	if (quotaExceeded)
		SubtitleManager::instance().reportQuotaExceeded(retryAfterSecs);

	if (m_closeRequested)
		QDialog::reject();
}

void McSubtitleDownloadDialog::reject()
{
	if (m_downloading) {
		m_closeRequested = true;
		if (m_worker)
			QMetaObject::invokeMethod(m_worker, "cancel", Qt::QueuedConnection);
		m_closeBtn->setEnabled(false);
		m_closeBtn->setText(tr("Cancelling…"));
		return; // dialog actually closes from onAllDone() once the worker stops
	}
	QDialog::reject();
}

} // namespace Mc
