#include "ui/McSubtitleDownloadDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McLanguageFlags.h"
#include "engine/OpenSubtitlesClient.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QtMath>

namespace Mc {

static const QList<QPair<QString,QString>> kSubDlgLangNames = {
	{"ara","Arabic"},  {"zho","Chinese"},  {"hrv","Croatian"}, {"ces","Czech"},
	{"dan","Danish"},  {"nld","Dutch"},    {"eng","English"},  {"fin","Finnish"},
	{"fra","French"},  {"deu","German"},   {"ell","Greek"},    {"heb","Hebrew"},
	{"hun","Hungarian"},{"ind","Indonesian"},{"ita","Italian"},{"jpn","Japanese"},
	{"kor","Korean"},  {"nor","Norwegian"},{"pol","Polish"},   {"por","Portuguese"},
	{"ron","Romanian"},{"rus","Russian"},  {"srp","Serbian"},  {"slk","Slovak"},
	{"spa","Spanish"}, {"swe","Swedish"},  {"tha","Thai"},     {"tur","Turkish"},
	{"ukr","Ukrainian"},{"vie","Vietnamese"},
};

// static
QString McSubtitleDownloadDialog::languageDisplayName(const QString& iso6392)
{
	for (const auto& [code, name] : kSubDlgLangNames)
		if (code == iso6392) return name;
	return iso6392;
}

McSubtitleDownloadDialog::McSubtitleDownloadDialog(const QString& apiKey,
                                                   const QString& username,
                                                   const QString& password,
                                                   const QString& imdbId,
                                                   const QStringList& iso6392Languages,
                                                   const QString& videoPath,
                                                   QWidget* parent)
	: QDialog(parent)
	, m_apiKey(apiKey), m_username(username), m_password(password)
	, m_imdbId(imdbId), m_iso6392Languages(iso6392Languages), m_videoPath(videoPath)
{
	setWindowTitle(tr("Download Subtitles"));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(12, 12, 12, 12);

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

	setFixedWidth(400);
	adjustSize();
	setFixedSize(size());
}

void McSubtitleDownloadDialog::onDownload()
{
	m_downloadBtn->setEnabled(false);
	m_downloadBtn->hide();
	m_closeBtn->setEnabled(false);
	m_closeBtn->setText(tr("Close"));

	QStringList iso6391Languages;
	for (const QString& lang : m_iso6392Languages) {
		const QString l = iso6392to6391(lang);
		if (!l.isEmpty()) iso6391Languages << l;
	}

	m_thread = new QThread(this);
	m_worker = new SubtitleDownloadWorker(
		m_apiKey, m_username, m_password,
		m_imdbId, iso6391Languages, m_videoPath);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,
	        m_worker, &SubtitleDownloadWorker::run, Qt::QueuedConnection);
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

void McSubtitleDownloadDialog::onLanguageStarted(const QString& lang6391)
{
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
		item->setToolTip(QString{});
	} else {
		const bool notFound = message.contains(QLatin1String("Not found"), Qt::CaseInsensitive);
		item->setText(notFound ? tr("Not found") : tr("✗ Failed"));
		item->setForeground(notFound ? QColor(180, 100, 0) : QColor(200, 0, 0));
		if (!message.isEmpty() && !notFound)
			item->setToolTip(message);
	}
}

void McSubtitleDownloadDialog::onAllDone(int downloaded, int /*failed*/, const QString& /*statusMsg*/)
{
	m_downloaded = downloaded;
	m_closeBtn->setEnabled(true);
	emit downloadComplete(downloaded);
}

} // namespace Mc
