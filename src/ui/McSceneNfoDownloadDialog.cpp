#include "ui/McSceneNfoDownloadDialog.h"
#include "engine/SrrdbClient.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMetaObject>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

namespace Mc {

McSceneNfoDownloadDialog::McSceneNfoDownloadDialog(const QString& videoPath, const QString& imdbId,
                                                   const QString& movieTitle, QWidget* parent)
	: QDialog(parent)
	, m_videoPath(videoPath)
	, m_imdbId(imdbId)
{
	setWindowTitle(movieTitle.isEmpty()
	    ? tr("Download Scene NFO")
	    : tr("Download Scene NFO — %1").arg(movieTitle));
	setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(12, 12, 12, 12);

	m_statusLabel = new QLabel(tr("Searching srrDB for a matching release…"), this);
	m_statusLabel->setWordWrap(true);
	root->addWidget(m_statusLabel);

	m_candidateList = new QListWidget(this);
	m_candidateList->setVisible(false);
	root->addWidget(m_candidateList);
	connect(m_candidateList, &QListWidget::itemSelectionChanged, this, [this] {
		m_pickBtn->setEnabled(!m_candidateList->selectedItems().isEmpty());
	});
	connect(m_candidateList, &QListWidget::itemActivated, this, [this](QListWidgetItem*) {
		onPickClicked();
	});

	auto* btnRow = new QHBoxLayout;
	btnRow->addStretch();
	m_pickBtn = new QPushButton(tr("Use Selected"), this);
	m_pickBtn->setEnabled(false);
	m_pickBtn->setVisible(false);
	m_closeBtn = new QPushButton(tr("Cancel"), this);
	btnRow->addWidget(m_pickBtn);
	btnRow->addWidget(m_closeBtn);
	root->addLayout(btnRow);

	connect(m_pickBtn,  &QPushButton::clicked, this, &McSceneNfoDownloadDialog::onPickClicked);
	connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::reject);

	setFixedWidth(420);
	adjustSize();

	startSearch();
}

McSceneNfoDownloadDialog::~McSceneNfoDownloadDialog()
{
	if (m_thread && m_thread->isRunning()) {
		if (m_worker)
			QMetaObject::invokeMethod(m_worker, "cancel", Qt::QueuedConnection);
		m_thread->wait(5000);
	}
}

void McSceneNfoDownloadDialog::startSearch()
{
	m_thread = new QThread(this);
	m_worker = new SceneNfoDownloadWorker(m_videoPath, m_imdbId);
	m_worker->moveToThread(m_thread);

	connect(m_thread, &QThread::started,
	        m_worker, &SceneNfoDownloadWorker::search, Qt::QueuedConnection);
	connect(m_worker, &SceneNfoDownloadWorker::candidatesFound,
	        this, &McSceneNfoDownloadDialog::onCandidatesFound, Qt::QueuedConnection);
	connect(m_worker, &SceneNfoDownloadWorker::releaseSelected,
	        this, &McSceneNfoDownloadDialog::onReleaseSelected, Qt::QueuedConnection);
	connect(m_worker, &SceneNfoDownloadWorker::done,
	        this, &McSceneNfoDownloadDialog::onDone, Qt::QueuedConnection);
	connect(m_worker, &SceneNfoDownloadWorker::done,
	        m_worker, &QObject::deleteLater);
	connect(m_worker, &SceneNfoDownloadWorker::done,
	        m_thread, &QThread::quit);
	connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
	m_thread->start();
}

void McSceneNfoDownloadDialog::onCandidatesFound(QStringList labels, QStringList names)
{
	if (labels.isEmpty()) {
		m_statusLabel->setText(tr("No matching scene release found on srrDB."));
		m_closeBtn->setText(tr("Close"));
		return;
	}
	m_candidateNames = names;
	m_statusLabel->setText(tr("Multiple possible releases found — pick the correct one:"));
	m_candidateList->clear();
	m_candidateList->addItems(labels);
	m_candidateList->setVisible(true);
	m_pickBtn->setVisible(true);
	adjustSize();
}

void McSceneNfoDownloadDialog::onReleaseSelected(const QString& releaseName)
{
	m_candidateList->setVisible(false);
	m_pickBtn->setVisible(false);
	m_statusLabel->setText(tr("Downloading NFO for \"%1\"…").arg(releaseName));
	adjustSize();
}

void McSceneNfoDownloadDialog::onPickClicked()
{
	const int row = m_candidateList->currentRow();
	if (row < 0 || row >= m_candidateNames.size()) return;
	m_candidateList->setEnabled(false);
	m_pickBtn->setEnabled(false);
	if (m_worker)
		QMetaObject::invokeMethod(m_worker, "chooseRelease", Qt::QueuedConnection,
		                          Q_ARG(QString, m_candidateNames.at(row)));
}

void McSceneNfoDownloadDialog::onDone(bool success, const QString& releaseName,
                                       const QByteArray& rawContent, const QString& errorMessage)
{
	m_finished = true;
	if (success) {
		m_statusLabel->setText(tr("Downloaded NFO for \"%1\".").arg(releaseName));
		emit downloadSucceeded(rawContent);
	} else {
		m_statusLabel->setText(errorMessage.isEmpty() ? tr("Download failed.") : errorMessage);
	}
	m_closeBtn->setEnabled(true);
	m_closeBtn->setText(tr("Close"));
	if (m_closeRequested)
		QDialog::reject();
}

void McSceneNfoDownloadDialog::reject()
{
	if (!m_finished) {
		m_closeRequested = true;
		m_candidateList->setEnabled(false);
		m_pickBtn->setEnabled(false);
		if (m_worker)
			QMetaObject::invokeMethod(m_worker, "cancel", Qt::QueuedConnection);
		m_closeBtn->setEnabled(false);
		m_closeBtn->setText(tr("Cancelling…"));
		return; // dialog actually closes from onDone() once the worker stops
	}
	QDialog::reject();
}

} // namespace Mc
