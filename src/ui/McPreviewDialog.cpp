#include "ui/McPreviewDialog.h"
#include "ui/McJobStatsBar.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

static bool isLosslessAudio(const Mc::StreamRecord& s)
{
	if (s.codecType != QLatin1String("audio")) return false;
	const QString cn = s.codecName.toLower();
	if (cn == QLatin1String("flac")   || cn == QLatin1String("alac")
	 || cn == QLatin1String("truehd") || cn == QLatin1String("mlp")
	 || cn == QLatin1String("tta")    || cn == QLatin1String("wavpack")
	 || cn.startsWith(QLatin1String("pcm_")))
		return true;
	if (cn == QLatin1String("dts")) {
		const QString cp = s.codecProfile.toUpper();
		return cp.contains(QLatin1String("MA")) || cp.contains(QLatin1String("HRA"));
	}
	return false;
}

static int pcmBitDepth(const QString& cn)
{
	const int u = cn.indexOf('_');
	if (u < 0 || u + 2 >= cn.size()) return 0;
	int i = u + 1;
	if (!cn[i].isLetter()) return 0;
	++i;
	const int start = i;
	while (i < cn.size() && cn[i].isDigit()) ++i;
	return (i > start) ? cn.mid(start, i - start).toInt() : 0;
}

static qint64 losslessBytesPerSecPerChannel(const Mc::StreamRecord& s)
{
	const QString cn = s.codecName.toLower();
	if (cn.startsWith(QLatin1String("pcm_"))) {
		if (s.sampleRate <= 0) return -1;
		const int depth = pcmBitDepth(cn);
		return (depth > 0) ? static_cast<qint64>(s.sampleRate) * depth / 8 : -1;
	}
	if (cn == QLatin1String("truehd") || cn == QLatin1String("mlp")) return 87'500;
	if (cn == QLatin1String("flac"))    return 56'250;
	if (cn == QLatin1String("alac"))    return 50'000;
	if (cn == QLatin1String("tta") || cn == QLatin1String("wavpack")) return 56'250;
	if (cn == QLatin1String("dts")) {
		const QString cp = s.codecProfile.toUpper();
		if (cp.contains(QLatin1String("MA")))  return 87'500;
		if (cp.contains(QLatin1String("HRA"))) return 62'500;
	}
	return -1;
}

static qint64 estimateStreamBytes(const Mc::StreamRecord& s,
                                   const QList<Mc::StreamRecord>&,
                                   qint64,
                                   double fileDurationSec)
{
	if (fileDurationSec <= 0) return -1;
	if (!isLosslessAudio(s)) {
		if (s.bitRate > 0)
			return static_cast<qint64>(s.bitRate / 8.0 * fileDurationSec);
		if (s.codecType == QLatin1String("subtitle"))
			return 256LL * 1024;
		return 0;
	}
	const qint64 bpsPerChannel = losslessBytesPerSecPerChannel(s);
	if (bpsPerChannel < 0) return -1;
	return static_cast<qint64>(bpsPerChannel * qMax(s.channels, 1) * fileDurationSec);
}

} // anonymous namespace

namespace Mc {

static QFrame* previewSeparator(QWidget* parent)
{
	auto* f = new QFrame(parent);
	f->setFrameShape(QFrame::HLine);
	f->setFrameShadow(QFrame::Sunken);
	return f;
}

// ── Default (stub) constructor — keeps Phase2Stubs usage valid ────────────────

McPreviewDialog::McPreviewDialog(QWidget* parent)
	: QDialog(parent)
{}

// ── Full constructor ──────────────────────────────────────────────────────────

McPreviewDialog::McPreviewDialog(const FileDecision& decision, QWidget* parent)
	: QDialog(parent)
{
	setupUi(decision);
}

void McPreviewDialog::done(int result)
{
	QSettings s;
	s.setValue("previewDialog/geometry", saveGeometry());
	if (m_splitter)
		s.setValue("previewDialog/splitter", m_splitter->saveState());
	QDialog::done(result);
}

void McPreviewDialog::setupUi(const FileDecision& decision)
{
	setWindowTitle(tr("Preview — %1").arg(decision.file.filename));
	setMinimumSize(860, 500);

	QSettings s;
	if (const QByteArray geo = s.value("previewDialog/geometry").toByteArray(); !geo.isEmpty())
		restoreGeometry(geo);
	else
		resize(1060, 600);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(8, 8, 8, 8);
	root->setSpacing(8);

	// ── Stats bar (mirrors BulkSummaryDialog style) ───────────────────────────
	int audioRemoved    = 0;
	int subtitleRemoved = 0;
	int videoRemoved    = 0;
	int reasonLanguage  = 0, reasonRedundant = 0, reasonSdh = 0;
	int reasonCommentary = 0, reasonOther = 0;

	for (const TrackDecision& td : decision.tracks) {
		if (td.decision != Decision::Remove) continue;
		if      (td.stream.codecType == "audio")    ++audioRemoved;
		else if (td.stream.codecType == "subtitle") ++subtitleRemoved;
		else if (td.stream.codecType == "video")    ++videoRemoved;

		if (td.reason.contains("SDH", Qt::CaseInsensitive)
		        || td.reason.contains("superseded", Qt::CaseInsensitive))
			++reasonSdh;
		else if (td.reason.contains("Redundant", Qt::CaseInsensitive))
			++reasonRedundant;
		else if (td.reason.contains("Commentary", Qt::CaseInsensitive))
			++reasonCommentary;
		else if (td.reason.contains("not understood", Qt::CaseInsensitive)
		         || td.reason.contains("not in understood", Qt::CaseInsensitive)
		         || td.reason.contains("not original", Qt::CaseInsensitive))
			++reasonLanguage;
		else
			++reasonOther;
	}

	QList<StreamRecord> allStreams;
	for (const auto& td : decision.tracks) allStreams << td.stream;

	qint64 estimatedSaving = 0;
	for (const auto& td : decision.tracks) {
		if (td.decision != Decision::Remove) continue;
		const qint64 est = estimateStreamBytes(
		    td.stream, allStreams,
		    decision.file.sizeBytes, decision.file.durationSec);
		if (est > 0) estimatedSaving += est;
	}

	// ── Stat cells row (shared widget, same style as McBulkSummaryDialog) ────────
	{
		QList<McJobStatsBar::StatItem> statItems;
		if (audioRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(audioRemoved),    tr("audio\nremoved")};
		if (subtitleRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(subtitleRemoved), tr("subtitle\nremoved")};
		if (videoRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(videoRemoved),    tr("MJPEG\nremoved")};
		if (!statItems.isEmpty() || estimatedSaving > 0)
			root->addWidget(new McJobStatsBar(statItems, estimatedSaving, this));
	}

	// ── Compact reasons line ──────────────────────────────────────────────────────
	{
		QStringList parts;
		if (reasonLanguage   > 0) parts << tr("Language ×%1").arg(reasonLanguage);
		if (reasonRedundant  > 0) parts << tr("Redundant ×%1").arg(reasonRedundant);
		if (reasonSdh        > 0) parts << tr("SDH ×%1").arg(reasonSdh);
		if (reasonCommentary > 0) parts << tr("Commentary ×%1").arg(reasonCommentary);
		if (reasonOther      > 0) parts << tr("Other ×%1").arg(reasonOther);
		if (!parts.isEmpty()) {
			auto* reasonLbl = new QLabel(parts.join(QStringLiteral("  ·  ")), this);
			QPalette pal = reasonLbl->palette();
			pal.setColor(QPalette::WindowText, pal.color(QPalette::WindowText).darker(150));
			reasonLbl->setPalette(pal);
			QFont rf = reasonLbl->font();
			rf.setPointSizeF(rf.pointSizeF() * 0.92);
			reasonLbl->setFont(rf);
			reasonLbl->setContentsMargins(4, 0, 0, 0);
			root->addWidget(reasonLbl);
		}
	}

	root->addWidget(previewSeparator(this));

	// ── Two-panel layout ──────────────────────────────────────────────────────
	m_splitter = new QSplitter(Qt::Horizontal, this);
	auto* panels = m_splitter;

	// Before
	auto* beforeGroup = new QGroupBox(tr("Before  (%1 tracks)").arg(decision.tracks.size()), this);
	auto* beforeLayout = new QVBoxLayout(beforeGroup);
	beforeLayout->setContentsMargins(4, 4, 4, 4);

	auto* beforeTable = new QTableWidget(0, 6, beforeGroup);
	beforeTable->setHorizontalHeaderLabels({
		tr("Type"), tr("Codec"), tr("Language"), tr("Bitrate"), tr("Est. Size"), tr("Decision / Reason")
	});
	beforeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
	beforeTable->horizontalHeader()->resizeSection(0, 72);
	beforeTable->horizontalHeader()->resizeSection(1, 100);
	beforeTable->horizontalHeader()->resizeSection(2, 68);
	beforeTable->horizontalHeader()->resizeSection(3, 72);
	beforeTable->horizontalHeader()->resizeSection(4, 80);
	beforeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	beforeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	beforeTable->setAlternatingRowColors(true);
	beforeTable->setShowGrid(false);
	beforeTable->verticalHeader()->setVisible(false);
	beforeTable->verticalHeader()->setDefaultSectionSize(22);
	populateBeforeTable(beforeTable, decision);
	beforeLayout->addWidget(beforeTable);
	panels->addWidget(beforeGroup);
	panels->setStretchFactor(0, 1);

	// After
	const int keptCount = decision.tracks.size() - decision.removalCount();
	auto* afterGroup = new QGroupBox(tr("After  (%1 tracks)").arg(keptCount), this);
	auto* afterLayout = new QVBoxLayout(afterGroup);
	afterLayout->setContentsMargins(4, 4, 4, 4);

	auto* afterTable = new QTableWidget(0, 5, afterGroup);
	afterTable->setHorizontalHeaderLabels({
		tr("Type"), tr("Codec"), tr("Language"), tr("Bitrate"), tr("Est. Size")
	});
	afterTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
	afterTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
	afterTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	afterTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
	afterTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
	afterTable->horizontalHeader()->resizeSection(0, 72);
	afterTable->horizontalHeader()->resizeSection(1, 100);
	afterTable->horizontalHeader()->resizeSection(2, 68);
	afterTable->horizontalHeader()->resizeSection(3, 72);
	afterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	afterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	afterTable->setAlternatingRowColors(true);
	afterTable->setShowGrid(false);
	afterTable->verticalHeader()->setVisible(false);
	afterTable->verticalHeader()->setDefaultSectionSize(22);
	populateAfterTable(afterTable, decision);
	afterLayout->addWidget(afterTable);
	panels->addWidget(afterGroup);
	panels->setStretchFactor(1, 1);

	if (const QByteArray sp = s.value("previewDialog/splitter").toByteArray(); !sp.isEmpty())
		panels->restoreState(sp);

	root->addWidget(panels, 1);

	// ── Button box ────────────────────────────────────────────────────────────
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Debug readout — copies a plain-text track dump to clipboard for pasting
	// when investigating rule-engine decisions (e.g. why a language is kept).
	auto* copyBtn = buttons->addButton(tr("Copy debug info"), QDialogButtonBox::ActionRole);
	connect(copyBtn, &QPushButton::clicked, this, [decision, copyBtn]() mutable {
		QString text;
		text += QStringLiteral("File: %1\n").arg(decision.file.filename);
		text += QStringLiteral("Path: %1\n").arg(decision.file.path);
		text += QStringLiteral("Original language: %1\n")
		        .arg(decision.file.originalLanguage.isEmpty() ? QStringLiteral("—") : decision.file.originalLanguage);
		text += QStringLiteral("Duration: %1s\n\nTracks:\n").arg(decision.file.durationSec, 0, 'f', 1);

		for (const TrackDecision& td : decision.tracks) {
			const StreamRecord& s = td.stream;

			QStringList flags;
			if (s.isDefault)         flags << QStringLiteral("default");
			if (s.isForced)          flags << QStringLiteral("forced");
			if (s.isHearingImpaired) flags << QStringLiteral("hi");

			QString info;
			if (s.codecType == QStringLiteral("audio"))
				info = QStringLiteral("%1 %2ch").arg(s.codecName).arg(s.channels);
			else if (s.codecType == QStringLiteral("video"))
				info = QStringLiteral("%1 %2×%3").arg(s.codecName).arg(s.width).arg(s.height);
			else
				info = s.codecName;

			const QString lang = s.language.isEmpty() ? QStringLiteral("und") : s.language;
			if (!flags.isEmpty())
				info += QStringLiteral("  %1  [%2]").arg(lang, flags.join(','));
			else
				info += QStringLiteral("  %1").arg(lang);

			if (!s.trackType.isEmpty() && s.trackType != QStringLiteral("main"))
				info += QStringLiteral("  (%1)").arg(s.trackType);
			if (!s.title.isEmpty())
				info += QStringLiteral("  \"%1\"").arg(s.title);

			const QString dec = (td.decision == Decision::Keep   ? QStringLiteral("Keep")
			                   : td.decision == Decision::Remove  ? QStringLiteral("Remove")
			                                                      : QStringLiteral("Unsure"));
			const QString type3 = s.codecType.toUpper().left(3);
			text += QStringLiteral("[%1] %2  %3  —  %4").arg(s.streamIndex).arg(type3, info, dec);
			if (!td.reason.isEmpty()) text += QStringLiteral("  (%1)").arg(td.reason);
			text += '\n';
		}

		QApplication::clipboard()->setText(text);
		copyBtn->setText(tr("Copied!"));
		QTimer::singleShot(1500, copyBtn, [copyBtn]() {
			copyBtn->setText(tr("Copy debug info"));
		});
	});

	root->addWidget(buttons);
}

// ── Table population ──────────────────────────────────────────────────────────

void McPreviewDialog::populateBeforeTable(QTableWidget* table, const FileDecision& decision)
{
	QList<StreamRecord> allStreams;
	for (const auto& td : decision.tracks) allStreams << td.stream;

	table->setRowCount(0);
	for (const TrackDecision& td : decision.tracks) {
		const int row = table->rowCount();
		table->insertRow(row);

		auto* typeItem    = new QTableWidgetItem(td.stream.codecType);
		auto* codecItem   = new QTableWidgetItem(td.stream.codecName.isEmpty()
		                                         ? tr("—") : td.stream.codecName);
		auto* langItem    = new QTableWidgetItem(td.stream.language.isEmpty()
		                                         ? tr("und") : td.stream.language);
		auto* bitrateItem = new QTableWidgetItem(formatBitrate(td.stream.bitRate));
		auto* sizeItem    = new QTableWidgetItem(formatStreamSize(
		    estimateStreamBytes(td.stream, allStreams,
		                        decision.file.sizeBytes, decision.file.durationSec)));

		const QString decText = decisionText(td.decision)
		    + (td.reason.isEmpty() ? QString() : tr(" — ") + td.reason);
		auto* decItem = new QTableWidgetItem(decText);
		decItem->setForeground(decisionColor(td.decision));

		bitrateItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		// Strike-through the whole row for removed tracks
		if (td.decision == Decision::Remove) {
			for (auto* item : { typeItem, codecItem, langItem, bitrateItem, sizeItem }) {
				QFont f = item->font();
				f.setStrikeOut(true);
				item->setFont(f);
				item->setForeground(QColor(0x99, 0x99, 0x99));
			}
		}

		table->setItem(row, 0, typeItem);
		table->setItem(row, 1, codecItem);
		table->setItem(row, 2, langItem);
		table->setItem(row, 3, bitrateItem);
		table->setItem(row, 4, sizeItem);
		table->setItem(row, 5, decItem);
	}
}

void McPreviewDialog::populateAfterTable(QTableWidget* table, const FileDecision& decision)
{
	QList<StreamRecord> allStreams;
	for (const auto& td : decision.tracks) allStreams << td.stream;

	table->setRowCount(0);
	for (const TrackDecision& td : decision.tracks) {
		if (td.decision == Decision::Remove) continue;

		const int row = table->rowCount();
		table->insertRow(row);

		auto* typeItem    = new QTableWidgetItem(td.stream.codecType);
		auto* codecItem   = new QTableWidgetItem(td.stream.codecName.isEmpty()
		                                         ? tr("—") : td.stream.codecName);
		auto* langItem    = new QTableWidgetItem(td.stream.language.isEmpty()
		                                         ? tr("und") : td.stream.language);
		auto* bitrateItem = new QTableWidgetItem(formatBitrate(td.stream.bitRate));
		auto* sizeItem    = new QTableWidgetItem(formatStreamSize(
		    estimateStreamBytes(td.stream, allStreams,
		                        decision.file.sizeBytes, decision.file.durationSec)));
		bitrateItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		table->setItem(row, 0, typeItem);
		table->setItem(row, 1, codecItem);
		table->setItem(row, 2, langItem);
		table->setItem(row, 3, bitrateItem);
		table->setItem(row, 4, sizeItem);
	}
}

// ── Helpers ───────────────────────────────────────────────────────────────────

QString McPreviewDialog::formatBitrate(qint64 bps)
{
	if (bps <= 0)              return {};
	if (bps >= 1'000'000)      return QStringLiteral("%1 Mbps").arg(bps / 1'000'000.0, 0, 'f', 1);
	if (bps >= 1'000)          return QStringLiteral("%1 kbps").arg(bps / 1'000.0,     0, 'f', 0);
	return QStringLiteral("%1 bps").arg(bps);
}

QString McPreviewDialog::formatStreamSize(qint64 bytes)
{
	if (bytes <= 0)                  return QStringLiteral("—");
	if (bytes >= 1'073'741'824LL)    return QStringLiteral("~%1 GB").arg(bytes / 1'073'741'824.0, 0, 'f', 2);
	if (bytes >= 1'048'576LL)        return QStringLiteral("~%1 MB").arg(bytes / 1'048'576.0,     0, 'f', 1);
	if (bytes >= 1'024LL)            return QStringLiteral("~%1 KB").arg(bytes / 1'024.0,         0, 'f', 0);
	return QStringLiteral("~%1 B").arg(bytes);
}

QString McPreviewDialog::decisionText(Decision d)
{
	switch (d) {
	case Decision::Keep:   return tr("Keep");
	case Decision::Remove: return tr("Remove");
	case Decision::Unsure: return tr("Unsure");
	}
	return {};
}

QColor McPreviewDialog::decisionColor(Decision d)
{
	switch (d) {
	case Decision::Keep:   return { 0x1a, 0x86, 0x4a };   // green
	case Decision::Remove: return { 0xcc, 0x22, 0x22 };   // red
	case Decision::Unsure: return { 0xc0, 0x80, 0x00 };   // amber
	}
	return {};
}

} // namespace Mc
