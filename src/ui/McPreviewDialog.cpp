#include "ui/McPreviewDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McJobStatsBar.h"
#include "ui/McLanguageFlags.h"
#include "core/AppSettings.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

static qint64 estimateStreamSizeBytes(const Mc::StreamRecord& s,
                                       const QList<Mc::StreamRecord>& allStreams,
                                       qint64 fileSizeBytes,
                                       double durationSec)
{
	double totalBr = 0.0;
	for (const Mc::StreamRecord& sr : allStreams) {
		totalBr += sr.bitRate > 0 ? static_cast<double>(sr.bitRate)
		         : sr.codecType == QLatin1String("subtitle") ? 50'000.0 : 0.0;
	}
	if (durationSec > 0)
		totalBr = qMax(totalBr, static_cast<double>(fileSizeBytes) * 8.0 / durationSec);
	if (totalBr <= 0.0) return 0;
	const double streamBr = s.bitRate > 0 ? static_cast<double>(s.bitRate)
	                      : s.codecType == QLatin1String("subtitle") ? 50'000.0 : 0.0;
	return static_cast<qint64>(streamBr / totalBr * static_cast<double>(fileSizeBytes));
}

static QString buildTrackTooltip(const Mc::StreamRecord& s, bool isOrig)
{
	QStringList lines;
	QString head = s.codecName.isEmpty() ? QStringLiteral("—") : s.codecName;
	if (!s.codecProfile.isEmpty())
		head += QStringLiteral(" (%1)").arg(s.codecProfile);
	lines << head;
	if (!s.title.isEmpty())
		lines << QStringLiteral("\"%1\"").arg(s.title);
	const QString lang = s.language.isEmpty() ? QStringLiteral("und") : s.language;
	lines << QObject::tr("Language: %1 (%2)")
	         .arg(Mc::McLanguageFlags::displayName(lang), lang);
	if (isOrig)              lines << QObject::tr("\xE2\x97\x8E Original language");   // ◎
	if (s.isDefault)         lines << QObject::tr("\xE2\x98\x85 Default track");       // ★
	if (s.isForced)          lines << QObject::tr("\xE2\x97\x8F Forced");              // ●
	if (s.isHearingImpaired) lines << QObject::tr("SDH (hearing impaired)");
	const QString tt = s.trackType.toLower();
	if (tt == QLatin1String("commentary")) lines << QObject::tr("Commentary");
	if (tt == QLatin1String("signs"))      lines << QObject::tr("Signs");
	return lines.join(QLatin1Char('\n'));
}

} // anonymous namespace

namespace Mc {

// ── Track badge delegate ──────────────────────────────────────────────────────
// Paints the Track column as a card-style badge pill (colour-coded by stream
// type, struck-through when removed) so the preview matches the card views.

class McTrackBadgeDelegate : public QStyledItemDelegate
{
	Q_OBJECT
public:
	using QStyledItemDelegate::QStyledItemDelegate;

	static constexpr int kCellPadH = 4; // gap between cell edge and badge pill
	static constexpr int kCellPadV = 4; // vertical padding around the pill

	static QFont badgeFont(const QFont& base)
	{
		QFont f = base;
		f.setPointSizeF(base.pointSizeF() * 0.82); // same scale as McCardDelegate
		return f;
	}

	void paint(QPainter* p, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override
	{
		QStyleOptionViewItem opt = option;
		initStyleOption(&opt, index);
		opt.text.clear();
		QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
		style->drawControl(QStyle::CE_ItemViewItem, &opt, p, opt.widget);

		const QString text = index.data(Qt::DisplayRole).toString();
		if (text.isEmpty())
			return;

		const QColor  color   = McCardDelegate::badgeColor(index.data(Qt::UserRole).toString());
		const bool    removed = index.data(Qt::UserRole + 1).toBool();
		const QString lang    = index.data(Qt::UserRole + 2).toString();
		const QVariantMap flagsVariant = index.data(Qt::UserRole + 3).toMap();
		QMap<QString, bool> streamFlags;
		for (auto it = flagsVariant.constBegin(); it != flagsVariant.constEnd(); ++it)
			streamFlags[it.key()] = it.value().toBool();
		const int     y       = opt.rect.top() + (opt.rect.height() - McCardDelegate::kBadgeH) / 2;
		McCardDelegate::drawBadge(p, opt.rect.left() + kCellPadH, y,
		                          McCardDelegate::kBadgeH, text, color,
		                          badgeFont(opt.font), removed,
		                          false, {}, false, lang, streamFlags);
	}

	QSize sizeHint(const QStyleOptionViewItem& option,
	               const QModelIndex& index) const override
	{
		const QFontMetrics fm(badgeFont(option.font));
		int w = fm.horizontalAdvance(index.data(Qt::DisplayRole).toString())
		      + 2 * McCardDelegate::kBadgePad + 2 * kCellPadH;
		const QString lang = index.data(Qt::UserRole + 2).toString();
		if (!McLanguageFlags::countryForLanguage(lang).isEmpty())
			w += McCardDelegate::kFlagW + McCardDelegate::kFlagGap;
		return { w, McCardDelegate::kBadgeH + 2 * kCellPadV };
	}
};

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

McPreviewDialog::McPreviewDialog(const FileDecision& decision, const QString& flagChangesJson, QWidget* parent)
	: QDialog(parent)
{
	setupUi(decision, flagChangesJson);
}

void McPreviewDialog::done(int result)
{
	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("previewDialog/geometry", saveGeometry());
	if (m_splitter)
		s.setValue("previewDialog/splitter", m_splitter->saveState());
	QDialog::done(result);
}

void McPreviewDialog::setupUi(const FileDecision& decision, const QString& flagChangesJson)
{
	setWindowTitle(tr("Preview — %1").arg(decision.file.filename));
	setMinimumSize(860, 500);

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	if (const QByteArray geo = s.value("previewDialog/geometry").toByteArray(); !geo.isEmpty())
		restoreGeometry(geo);
	else
		resize(1100, 620);

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

	const qint64 estimatedSaving = decision.estimatedSavingBytes();

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

	auto* beforeTable = new QTableWidget(0, 4, beforeGroup);
	beforeTable->setHorizontalHeaderLabels({
		tr("Track"), tr("Bitrate"), tr("Est. Size"), tr("Decision / Reason")
	});
	beforeTable->setItemDelegateForColumn(0, new McTrackBadgeDelegate(beforeTable));
	beforeTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	beforeTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
	beforeTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
	beforeTable->horizontalHeader()->resizeSection(1, 68);
	beforeTable->horizontalHeader()->resizeSection(2, 76);
	beforeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	beforeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	beforeTable->setAlternatingRowColors(true);
	beforeTable->setShowGrid(false);
	beforeTable->verticalHeader()->setVisible(false);
	beforeTable->verticalHeader()->setDefaultSectionSize(
	    McCardDelegate::kBadgeH + 2 * McTrackBadgeDelegate::kCellPadV);
	populateBeforeTable(beforeTable, decision, flagChangesJson);
	beforeLayout->addWidget(beforeTable);
	panels->addWidget(beforeGroup);
	panels->setStretchFactor(0, 1);

	// After
	const int keptCount = decision.tracks.size() - decision.removalCount();
	auto* afterGroup = new QGroupBox(tr("After  (%1 tracks)").arg(keptCount), this);
	auto* afterLayout = new QVBoxLayout(afterGroup);
	afterLayout->setContentsMargins(4, 4, 4, 4);

	auto* afterTable = new QTableWidget(0, 3, afterGroup);
	afterTable->setHorizontalHeaderLabels({
		tr("Track"), tr("Bitrate"), tr("Est. Size")
	});
	afterTable->setItemDelegateForColumn(0, new McTrackBadgeDelegate(afterTable));
	afterTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	afterTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
	afterTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
	afterTable->horizontalHeader()->resizeSection(1, 68);
	afterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
	afterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
	afterTable->setAlternatingRowColors(true);
	afterTable->setShowGrid(false);
	afterTable->verticalHeader()->setVisible(false);
	afterTable->verticalHeader()->setDefaultSectionSize(
	    McCardDelegate::kBadgeH + 2 * McTrackBadgeDelegate::kCellPadV);
	populateAfterTable(afterTable, decision, flagChangesJson);
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

static QHash<int, QMap<QString, bool>> parsePendingChanges(const QString& flagChangesJson)
{
	QHash<int, QMap<QString, bool>> out;
	if (flagChangesJson.isEmpty()) return out;
	const QJsonArray arr = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	for (const QJsonValue& v : arr) {
		const QJsonObject o = v.toObject();
		out[o[QLatin1String("streamIndex")].toInt()]
		   [o[QLatin1String("flag")].toString()] = o[QLatin1String("value")].toBool();
	}
	return out;
}

static StreamRecord applyPendingFlags(const StreamRecord& s, const QMap<QString, bool>& flags)
{
	StreamRecord d = s;
	for (auto it = flags.constBegin(); it != flags.constEnd(); ++it) {
		const bool v = it.value();
		if      (it.key() == QLatin1String("default"))    d.isDefault    = v;
		else if (it.key() == QLatin1String("forced"))     d.isForced     = v;
		else if (it.key() == QLatin1String("original"))   d.isOriginal   = v;
		else if (it.key() == QLatin1String("commentary")) d.isCommentary = v;
	}
	return d;
}

void McPreviewDialog::populateBeforeTable(QTableWidget* table, const FileDecision& decision,
                                           const QString& flagChangesJson)
{
	const auto pendingChanges = parsePendingChanges(flagChangesJson);
	QList<StreamRecord> allStreams;
	for (const auto& td : decision.tracks) allStreams << td.stream;

	table->setRowCount(0);
	for (const TrackDecision& td : decision.tracks) {
		const int row = table->rowCount();
		table->insertRow(row);

		const bool isOrig = td.stream.codecType == QLatin1String("audio")
		                 && !decision.file.originalLanguage.isEmpty()
		                 && td.stream.language.compare(decision.file.originalLanguage, Qt::CaseInsensitive) == 0;
		const bool removed = td.decision == Decision::Remove;

		const QMap<QString, bool> streamFlags = pendingChanges.value(td.stream.streamIndex);
		const StreamRecord displayS = applyPendingFlags(td.stream, streamFlags);
		QVariantMap flagsVariant;
		for (auto it = streamFlags.constBegin(); it != streamFlags.constEnd(); ++it)
			flagsVariant[it.key()] = it.value();

		auto* trackItem = new QTableWidgetItem(McCardDelegate::buildBadgeText(displayS, isOrig));
		trackItem->setData(Qt::UserRole, td.stream.codecType);
		trackItem->setData(Qt::UserRole + 1, removed);
		trackItem->setData(Qt::UserRole + 2,
		    td.stream.codecType == QLatin1String("video") ? QString() : td.stream.language);
		trackItem->setData(Qt::UserRole + 3, flagsVariant);
		trackItem->setToolTip(buildTrackTooltip(displayS, isOrig));

		auto* bitrateItem = new QTableWidgetItem(formatBitrate(td.stream.bitRate));
		auto* sizeItem    = new QTableWidgetItem(formatStreamSize(
		    estimateStreamSizeBytes(td.stream, allStreams,
		                        decision.file.sizeBytes, decision.file.durationSec)));

		const QString decText = decisionText(td.decision)
		    + (td.reason.isEmpty() ? QString() : tr(" — ") + td.reason);
		auto* decItem = new QTableWidgetItem(decText);
		decItem->setForeground(decisionColor(td.decision));

		bitrateItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		// Tint the original-language audio row so it's easy to spot which
		// track is protected by the keep-original-audio policy.
		if (isOrig) {
			const QColor tint(0x40, 0x90, 0xe0, 35);
			for (auto* item : { trackItem, bitrateItem, sizeItem })
				item->setBackground(tint);
		}

		// Removed tracks: the badge delegate draws its own strike-through;
		// grey out the remaining text cells to match.
		if (removed) {
			for (auto* item : { bitrateItem, sizeItem }) {
				QFont f = item->font();
				f.setStrikeOut(true);
				item->setFont(f);
				item->setForeground(QColor(0x99, 0x99, 0x99));
			}
		}

		table->setItem(row, 0, trackItem);
		table->setItem(row, 1, bitrateItem);
		table->setItem(row, 2, sizeItem);
		table->setItem(row, 3, decItem);
	}
}

void McPreviewDialog::populateAfterTable(QTableWidget* table, const FileDecision& decision,
                                          const QString& flagChangesJson)
{
	const auto pendingChanges = parsePendingChanges(flagChangesJson);
	QList<StreamRecord> allStreams;
	for (const auto& td : decision.tracks) allStreams << td.stream;

	table->setRowCount(0);
	for (const TrackDecision& td : decision.tracks) {
		if (td.decision == Decision::Remove) continue;

		const int row = table->rowCount();
		table->insertRow(row);

		const bool isOrig = td.stream.codecType == QLatin1String("audio")
		                 && !decision.file.originalLanguage.isEmpty()
		                 && td.stream.language.compare(decision.file.originalLanguage, Qt::CaseInsensitive) == 0;

		const QMap<QString, bool> streamFlags = pendingChanges.value(td.stream.streamIndex);
		const StreamRecord displayS = applyPendingFlags(td.stream, streamFlags);
		QVariantMap flagsVariant;
		for (auto it = streamFlags.constBegin(); it != streamFlags.constEnd(); ++it)
			flagsVariant[it.key()] = it.value();

		auto* trackItem = new QTableWidgetItem(McCardDelegate::buildBadgeText(displayS, isOrig));
		trackItem->setData(Qt::UserRole, td.stream.codecType);
		trackItem->setData(Qt::UserRole + 1, false);
		trackItem->setData(Qt::UserRole + 2,
		    td.stream.codecType == QLatin1String("video") ? QString() : td.stream.language);
		trackItem->setData(Qt::UserRole + 3, flagsVariant);
		trackItem->setToolTip(buildTrackTooltip(displayS, isOrig));

		auto* bitrateItem = new QTableWidgetItem(formatBitrate(td.stream.bitRate));
		auto* sizeItem    = new QTableWidgetItem(formatStreamSize(
		    estimateStreamSizeBytes(td.stream, allStreams,
		                        decision.file.sizeBytes, decision.file.durationSec)));
		bitrateItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		if (isOrig) {
			const QColor tint(0x40, 0x90, 0xe0, 35);
			for (auto* item : { trackItem, bitrateItem, sizeItem })
				item->setBackground(tint);
		}

		table->setItem(row, 0, trackItem);
		table->setItem(row, 1, bitrateItem);
		table->setItem(row, 2, sizeItem);
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

#include "McPreviewDialog.moc"
