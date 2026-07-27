#include "ui/McScanCompleteDialog.h"
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"
#include "engine/PosterManager.h"
#include "scanner/NfoParser.h"
#include "ui/McCardDelegate.h"

#include <QAbstractItemView>
#include <QColor>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace Mc {

namespace {

// Git-status-inspired colors: readable on both the light and dark ends of the
// default Windows widget palette without needing app-wide theme awareness.
const QColor kAddedColor(0x1a, 0x8f, 0x3c);
const QColor kRemovedColor(0xcc, 0x2b, 0x2b);
const QColor kPendingChipColor(0x90, 0x90, 0x90);

// Column layout, shared between construction and the live TMDB update handler.
constexpr int kColSign    = 0;
constexpr int kColMedia   = 1;
constexpr int kColTitle   = 2;
constexpr int kColQuality = 3;
constexpr int kColSize    = 4;
constexpr int kColFolder  = 5;
constexpr int kColGroup   = 6; // only present when showGroupColumn

struct ChangeRow
{
	ScanChangeEntry entry;
	QString         title; // NfoParser::titleFromFilename(entry.path's filename), precomputed once
	bool            added; // true = newly discovered, false = pruned from the DB
};

QString formatBytes(qint64 bytes)
{
	const double gb = bytes / 1073741824.0;
	if (gb >= 1.0) return QStringLiteral("%1 GB").arg(gb, 0, 'f', 2);
	return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', 1);
}

// Rounded-pill chip, same visual language as the storage-group chip below.
QPixmap pillPixmap(const QString& label, const QColor& color, const QFont& baseFont, qreal dpr)
{
	if (label.isEmpty() || !color.isValid())
		return {};

	QFont chipFont = baseFont;
	chipFont.setPointSizeF(baseFont.pointSizeF() * 0.85);
	chipFont.setBold(true);
	const QFontMetrics fm(chipFont);
	const int padX = 6;
	const int h    = McCardDelegate::kBadgeH;
	const int w    = fm.horizontalAdvance(label) + padX * 2;

	QPixmap pix(QSize(w, h) * dpr);
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);

	QPainter p(&pix);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	p.drawRoundedRect(QRectF(0, 0, w, h), 3, 3);
	p.setPen(Qt::white);
	p.setFont(chipFont);
	p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, label);
	return pix;
}

// "…" pill shown for a just-added file until PosterManager's background TMDB
// lookup resolves its real category (or gives up and leaves it unknown).
QPixmap pendingChipPixmap(const QFont& baseFont, qreal dpr)
{
	return pillPixmap(QStringLiteral("\xE2\x80\xA6"), kPendingChipColor, baseFont, dpr);
}

// PosterManager enqueues each file for TMDB lookup as soon as ScanWorker scans
// it — not after the whole session finishes — so a file scanned early in a
// multi-root session can already be classified by the time the last root
// finishes and this dialog opens. Re-check the DB instead of trusting the
// "unknown" snapshot ScanWorker captured mid-scan, so already-resolved rows
// don't sit on the pending pill forever waiting for a signal that already fired.
QString currentMediaType(const ScanChangeEntry& e, bool added)
{
	if (!added)
		return e.mediaType; // gone from the DB now — last known value is all we have
	const auto file = DatabaseManager::instance().fileById(e.fileId);
	return file ? file->mediaType : e.mediaType;
}

// Null pixmap (no chip shown) if the type is still unknown — matches the card
// view, which also skips the chip entirely for MediaTypes::Unknown.
QPixmap mediaTypeChipPixmap(const QString& mediaType, const QFont& baseFont, qreal dpr)
{
	const QString mt = MediaTypes::normalize(mediaType);
	QString label;
	if (mt == QLatin1String(MediaTypes::Movie))            label = QStringLiteral("Movie");
	else if (mt == QLatin1String(MediaTypes::Tv))          label = QStringLiteral("TV");
	else if (mt == QLatin1String(MediaTypes::Documentary)) label = QStringLiteral("Doc");
	else if (mt == QLatin1String(MediaTypes::Misc))        label = QStringLiteral("Misc");
	return pillPixmap(label, MediaTypes::badgeColor(mt), baseFont, dpr);
}

// Renders the same storage-group chip the card badges and McManageFoldersDialog's
// group picker use, so "which drive did this come from" reads identically everywhere.
QPixmap groupChipPixmap(int group, const QFont& baseFont, qreal dpr)
{
	const int w = McCardDelegate::groupChipWidth(group, baseFont);
	const int h = McCardDelegate::kBadgeH;
	QPixmap pix(QSize(w, h) * dpr);
	pix.setDevicePixelRatio(dpr);
	pix.fill(Qt::transparent);
	QPainter p(&pix);
	McCardDelegate::drawGroupChip(&p, 0, 0, h, group, baseFont, dpr);
	return pix;
}

} // namespace

McScanCompleteDialog::McScanCompleteDialog(const ScanChangeList& newFiles,
                                           const ScanChangeList& removedFiles, int updatedCount,
                                           QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Scan Complete"));
	setAttribute(Qt::WA_DeleteOnClose);
	setMinimumSize(680, 420);
	resize(880, 560);

	auto* root = new QVBoxLayout(this);

	qint64 addedBytes = 0, removedBytes = 0;
	for (const auto& e : newFiles)     addedBytes   += e.sizeBytes;
	for (const auto& e : removedFiles) removedBytes += e.sizeBytes;

	QStringList summaryParts;
	summaryParts << QStringLiteral("<span style='color:%1'>%2 (+%3)</span>")
	                    .arg(kAddedColor.name(), tr("%n new file(s)", "", newFiles.size()),
	                         formatBytes(addedBytes));
	if (updatedCount > 0) summaryParts << tr("%n updated", "", updatedCount);
	if (!removedFiles.isEmpty())
		summaryParts << QStringLiteral("<span style='color:%1'>%2 (\xE2\x88\x92%3)</span>")
		                    .arg(kRemovedColor.name(), tr("%n removed", "", removedFiles.size()),
		                         formatBytes(removedBytes));
	auto* summary = new QLabel(summaryParts.join(QStringLiteral(",  ")), this);
	summary->setTextFormat(Qt::RichText);
	root->addWidget(summary);

	const bool showGroupColumn = StorageGroupSettings::multipleGroupsInUse();

	m_table = new QTableWidget(this);
	m_table->setColumnCount(showGroupColumn ? 7 : 6);
	QStringList headers;
	headers.resize(showGroupColumn ? 7 : 6);
	headers[kColSign]    = QString();
	headers[kColMedia]   = tr("Category");
	headers[kColTitle]   = tr("Title");
	headers[kColQuality] = tr("Video");
	headers[kColSize]    = tr("Size");
	headers[kColFolder]  = tr("Folder");
	if (showGroupColumn) headers[kColGroup] = tr("Storage Group");
	m_table->setHorizontalHeaderLabels(headers);

	auto* hHeader = m_table->horizontalHeader();
	hHeader->setStretchLastSection(false);
	hHeader->setSectionResizeMode(kColSign, QHeaderView::Fixed);
	hHeader->resizeSection(kColSign, 20);
	hHeader->setSectionResizeMode(kColMedia, QHeaderView::ResizeToContents);
	hHeader->setSectionResizeMode(kColTitle, QHeaderView::ResizeToContents);
	hHeader->setSectionResizeMode(kColQuality, QHeaderView::ResizeToContents);
	hHeader->setSectionResizeMode(kColSize, QHeaderView::ResizeToContents);
	hHeader->setSectionResizeMode(kColFolder, QHeaderView::Stretch);
	if (showGroupColumn)
		hHeader->setSectionResizeMode(kColGroup, QHeaderView::ResizeToContents);
	m_table->verticalHeader()->setVisible(false);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::NoSelection);
	m_table->setSortingEnabled(false);
	root->addWidget(m_table, 1);

	// Single git-status-style list: added and removed files interleaved, sorted
	// by title so an add/remove pair for the same movie (e.g. a re-encode
	// replacing the old file) tends to land next to each other.
	auto titleFor = [](const ScanChangeEntry& e) {
		return NfoParser::titleFromFilename(QFileInfo(e.path).fileName());
	};

	QList<ChangeRow> rows;
	rows.reserve(newFiles.size() + removedFiles.size());
	for (const auto& e : newFiles)     rows.append({ e, titleFor(e), true });
	for (const auto& e : removedFiles) rows.append({ e, titleFor(e), false });
	std::sort(rows.begin(), rows.end(), [](const ChangeRow& a, const ChangeRow& b) {
		return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
	});

	const qreal dpr = devicePixelRatioF();

	m_table->setRowCount(rows.size());
	for (int row = 0; row < rows.size(); ++row) {
		const ChangeRow&       change = rows[row];
		const ScanChangeEntry& e      = change.entry;
		const QFileInfo        fi(e.path);
		const QColor&          color = change.added ? kAddedColor : kRemovedColor;

		auto* signItem = new QTableWidgetItem(change.added ? QStringLiteral("+")
		                                                    : QStringLiteral("\xE2\x88\x92"));
		signItem->setTextAlignment(Qt::AlignCenter);
		QFont signFont = signItem->font();
		signFont.setBold(true);
		signItem->setFont(signFont);
		signItem->setForeground(color);
		signItem->setToolTip(change.added ? tr("Added") : tr("Removed"));
		m_table->setItem(row, kColSign, signItem);

		auto* titleItem = new QTableWidgetItem(change.title);
		titleItem->setForeground(color);
		titleItem->setToolTip(fi.fileName());
		m_table->setItem(row, kColTitle, titleItem);

		auto* sizeItem = new QTableWidgetItem(formatBytes(e.sizeBytes));
		sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
		m_table->setItem(row, kColSize, sizeItem);

		auto* folderItem = new QTableWidgetItem(QDir::toNativeSeparators(fi.path()));
		folderItem->setToolTip(QDir::toNativeSeparators(e.path));
		m_table->setItem(row, kColFolder, folderItem);

		// Media-type chip. An added file may already be classified by now (see
		// currentMediaType() above) — only a still-unresolved added file gets the
		// pending "…" pill and a live-update subscription; everything else (already
		// classified, or a removed file with whatever category it last had) gets
		// its real chip immediately.
		const QString mediaType = currentMediaType(e, change.added);
		const bool    pending   = change.added
		    && MediaTypes::normalize(mediaType) == QLatin1String(MediaTypes::Unknown);
		const QPixmap mediaPix = pending
		    ? pendingChipPixmap(m_table->font(), dpr)
		    : mediaTypeChipPixmap(mediaType, m_table->font(), dpr);
		if (!mediaPix.isNull()) {
			auto* chip = new QLabel(m_table);
			chip->setPixmap(mediaPix);
			chip->setAlignment(Qt::AlignCenter);
			m_table->setCellWidget(row, kColMedia, chip);
		}
		if (pending)
			m_rowForAddedFileId.insert(e.fileId, row);

		if (!e.videoStream.codecType.isEmpty()) {
			const QString qualityText = McCardDelegate::buildBadgeText(e.videoStream);
			const QPixmap qualityPix  = McCardDelegate::badgePixmap(
			    qualityText, e.videoStream.codecType, m_table->font(), dpr);
			if (!qualityPix.isNull()) {
				auto* chip = new QLabel(m_table);
				chip->setPixmap(qualityPix);
				chip->setAlignment(Qt::AlignCenter);
				m_table->setCellWidget(row, kColQuality, chip);
			}
		}

		if (showGroupColumn) {
			const int group = StorageGroupSettings::groupForFilePath(e.path);
			auto* chip = new QLabel(m_table);
			chip->setPixmap(groupChipPixmap(group, m_table->font(), dpr));
			chip->setAlignment(Qt::AlignCenter);
			chip->setToolTip(tr("Group %1").arg(group));
			m_table->setCellWidget(row, kColGroup, chip);
		}
	}

	// Live-update media chips as background TMDB lookups resolve — connecting
	// with `this` as context auto-disconnects when the dialog closes.
	if (!m_rowForAddedFileId.isEmpty()) {
		connect(&PosterManager::instance(), &PosterManager::tmdbDataReady, this,
		        [this](qint64 fileId, const QString&, int, double, const QString& mediaType) {
			onTmdbDataReady(fileId, mediaType);
		});
	}

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void McScanCompleteDialog::onTmdbDataReady(qint64 fileId, const QString& mediaType)
{
	const auto it = m_rowForAddedFileId.constFind(fileId);
	if (it == m_rowForAddedFileId.constEnd())
		return;

	auto* chip = qobject_cast<QLabel*>(m_table->cellWidget(it.value(), kColMedia));
	if (!chip)
		return;

	const QPixmap pix = mediaTypeChipPixmap(mediaType, m_table->font(), devicePixelRatioF());
	// Still unknown (no TMDB match found) — clear the pending "…" pill rather
	// than leave it looking like classification is still in flight.
	chip->setPixmap(pix);
}

} // namespace Mc
