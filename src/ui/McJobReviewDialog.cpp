#include "ui/McJobReviewDialog.h"
#include "ui/McJobCardDelegate.h"
#include "ui/McJobListModel.h"
#include "ui/McWindowGeometry.h"
#include "core/DatabaseManager.h"
#include "engine/TrackDecision.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListView>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "core/AppSettings.h"

namespace Mc {

// ── Helpers ───────────────────────────────────────────────────────────────────

static QStandardItem* makeCardItem(
    const QString&             filename,
    const QString&             filePath,
    qint64                     sizeBytes,
    double                     durationSec,
    const QString&             posterPath,
    const QString&             fanartPath,
    const QString&             imdbId,
    double                     rating,
    const QString&             originalLanguage,
    const QString&             status,
    const QList<StreamRecord>& allStreams,
    const QList<StreamRecord>& keptStreams,
    qint64                     savedBytes)
{
	auto* item = new QStandardItem;
	item->setData(filename,                                  McJobListModel::FilenameRole);
	item->setData(filePath,                                  McJobListModel::FilePathRole);
	item->setData(sizeBytes,                                 McJobListModel::FileSizeRole);
	item->setData(durationSec,                               McJobListModel::DurationRole);
	item->setData(posterPath,                                McJobListModel::PosterRole);
	item->setData(fanartPath,                                McJobListModel::FanartRole);
	item->setData(imdbId,                                    McJobListModel::ImdbIdRole);
	item->setData(rating,                                    McJobListModel::RatingRole);
	item->setData(originalLanguage,                          McJobListModel::OriginalLanguageRole);
	item->setData(status,                                    McJobListModel::StatusRole);
	item->setData(savedBytes,                                McJobListModel::SavedRole);
	item->setData(0,                                         McJobListModel::ProgressRole);
	item->setData(QVariant::fromValue(allStreams),            McJobListModel::AllStreamsRole);
	item->setData(QVariant::fromValue(keptStreams),           McJobListModel::KeptStreamsRole);
	item->setCheckState(Qt::Unchecked);
	return item;
}

// ── McJobReviewDialog ─────────────────────────────────────────────────────────

McJobReviewDialog::McJobReviewDialog(
    qint64                     jobId,
    const QString&             filename,
    const QString&             warningText,
    const QString&             commandArgsJson,
    const QList<StreamRecord>& sourceStreams,
    const QList<StreamRecord>& outputStreams,
    QWidget*                   parent)
    : QDialog(parent)
    , m_jobId(jobId)
{
	setWindowTitle(tr("Track Mismatch — %1").arg(filename));
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(740, 430);

	{
	    QSettings s(AppSettings::geometryFilePath(), QSettings::IniFormat);
	    const QByteArray geo = s.value("jobReviewDialog/geometry").toByteArray();
	    if (!geo.isEmpty()) {
	        restoreGeometry(geo);
	        ensureGeometryFitsScreen(this);
	    } else
	        resize(960, 490);
	}

	// ── Fetch file metadata for the card headers ──────────────────────────────
	auto& db = DatabaseManager::instance();
	const auto jobOpt  = db.jobById(jobId);
	const auto fileOpt = jobOpt ? db.fileById(jobOpt->fileId) : std::nullopt;

	const qint64  sizeBytes       = fileOpt ? fileOpt->sizeBytes  : 0;
	const double  durationSec     = fileOpt ? fileOpt->durationSec : 0.0;
	const QString filePath        = fileOpt ? fileOpt->path        : QString{};
	const QString originalLang    = fileOpt ? fileOpt->originalLanguage : QString{};

	QString posterPath, fanartPath, imdbId;
	double  rating = 0.0;
	if (fileOpt) {
		const auto pr = db.posterForFile(fileOpt->id);
		if (pr) {
			posterPath = pr->imagePath;
			fanartPath = pr->fanartPath;
			imdbId     = pr->imdbId;
			rating     = pr->voteAverage;
		}
	}

	// ── Build the two card items ──────────────────────────────────────────────
	// Source card: all source streams; planned-removal ones are struck through.
	const QList<StreamRecord> keptSource =
	    McJobListModel::computeKeptStreams(sourceStreams, commandArgsJson);

	// Estimated saving for the source card: proportional bitrate of removed tracks.
	QSet<int> keptIdx;
	for (const auto& s : keptSource) keptIdx.insert(s.streamIndex);
	QSet<int> removedIdx;
	for (const auto& s : sourceStreams)
	    if (!keptIdx.contains(s.streamIndex)) removedIdx.insert(s.streamIndex);
	const qint64 estimatedSaving =
	    estimateSavingBytes(sourceStreams, removedIdx, sizeBytes, durationSec);

	// Actual saving for the remux card: source size minus the .tmp file on disk.
	qint64 tmpSize = 0;
	{
	    const QJsonArray argsArr =
	        QJsonDocument::fromJson(commandArgsJson.toUtf8()).array();
	    if (argsArr.size() >= 2 && argsArr[0].toString() == QLatin1String("-o")) {
	        const QFileInfo fi(argsArr[1].toString());
	        tmpSize = fi.exists() ? fi.size() : 0;
	    }
	}
	const qint64 actualSaving = qMax(0LL, sizeBytes - tmpSize);

	// Output card: all output streams; nothing struck through.
	auto* model = new QStandardItemModel(this);
	model->appendRow(makeCardItem(filename, filePath, sizeBytes, durationSec,
	                              posterPath, fanartPath, imdbId, rating,
	                              originalLang, QStringLiteral("source"),
	                              sourceStreams, keptSource, estimatedSaving));
	model->appendRow(makeCardItem(filename, filePath, tmpSize > 0 ? tmpSize : sizeBytes,
	                              durationSec,
	                              posterPath, fanartPath, imdbId, rating,
	                              originalLang, QStringLiteral("output"),
	                              outputStreams, outputStreams, actualSaving));

	// ── Layout ────────────────────────────────────────────────────────────────
	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(12, 12, 12, 12);

	// Warning label
	auto* warnLabel = new QLabel(this);
	warnLabel->setWordWrap(true);
	warnLabel->setText(
	    tr("<b>⚠ Track layout mismatch</b><br>"
	       "%1<br><br>"
	       "The file may have been modified since this job was created — "
	       "tracks could have shifted or been removed externally. "
	       "Compare the two cards: the top shows the <b>current source file</b> "
	       "(struck-through tracks were planned for removal), the bottom shows "
	       "the <b>proposed output</b>. Accept only if the result looks correct.")
	    .arg(warningText.toHtmlEscaped()));
	QPalette pal = warnLabel->palette();
	pal.setColor(QPalette::WindowText, QColor(0xb8, 0x60, 0x00));
	warnLabel->setPalette(pal);
	root->addWidget(warnLabel);

	// Card list — two items rendered with the real job card delegate
	auto* listView = new QListView(this);
	auto* delegate = new McJobCardDelegate(listView);
	listView->setModel(model);
	listView->setItemDelegate(delegate);
	listView->setSelectionMode(QAbstractItemView::NoSelection);
	listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
	listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	listView->setFocusPolicy(Qt::NoFocus);
	listView->setUniformItemSizes(false);
	root->addWidget(listView, 1);

	// Buttons
	auto* btnBox       = new QDialogButtonBox(this);
	auto* acceptBtn    = btnBox->addButton(tr("Accept Output"),
	                                       QDialogButtonBox::AcceptRole);
	auto* reanalyzeBtn = btnBox->addButton(tr("Rescan && Re-analyze"),
	                                       QDialogButtonBox::ActionRole);
	btnBox->addButton(tr("Discard"), QDialogButtonBox::RejectRole);

	acceptBtn->setToolTip(
	    tr("Commit the output — the tracks above look correct."));
	reanalyzeBtn->setToolTip(
	    tr("Discard the output, rescan the source file, and re-analyze from scratch."));

	connect(acceptBtn,    &QPushButton::clicked, this, [this]() { done(QDialog::Accepted); });
	connect(reanalyzeBtn, &QPushButton::clicked, this, [this]() { done(2); });
	connect(btnBox, &QDialogButtonBox::rejected, this, [this]() { done(QDialog::Rejected); });

	root->addWidget(btnBox);
}

void McJobReviewDialog::done(int result)
{
	QSettings s(AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("jobReviewDialog/geometry", saveGeometry());
	QDialog::done(result);
}

} // namespace Mc
