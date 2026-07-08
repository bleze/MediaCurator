#include "ui/McFileListModel.h"
#include "ui/McCardDelegate.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QPixmap>
#include <QPixmapCache>
#include <algorithm>

namespace Mc {

McFileListModel::McFileListModel(QObject* parent)
	: QAbstractListModel(parent)
{
	m_fanartBatchTimer.setSingleShot(true);
	m_fanartBatchTimer.setInterval(150);
	connect(&m_fanartBatchTimer, &QTimer::timeout, this, [this]() {
		for (auto it = m_pendingFanartIds.cbegin(); it != m_pendingFanartIds.cend(); ++it) {
			const qint64 fileId = it.key();
			for (int row = 0; row < m_entries.size(); ++row) {
				if (m_entries.at(row).file.id != fileId) continue;
				const QModelIndex idx = index(row);
				emit dataChanged(idx, idx, {FanartRole});
				break;
			}
		}
		m_pendingFanartIds.clear();
	});
}

// ── Filter helpers ────────────────────────────────────────────────────────────

bool McFileListModel::entryPassesFilter(const FileEntry& e) const
{
	// ── Text search: filename, parent folder, stream metadata, container ──────
	if (!m_filterText.isEmpty()) {
		bool found = e.file.filename.contains(m_filterText, Qt::CaseInsensitive);
		if (!found) {
			const QString folder = QFileInfo(e.file.path).dir().dirName();
			found = folder.contains(m_filterText, Qt::CaseInsensitive);
		}
		if (!found) {
			for (const StreamRecord& s : e.streams) {
				if (s.codecName.contains(m_filterText, Qt::CaseInsensitive)    ||
				    s.codecProfile.contains(m_filterText, Qt::CaseInsensitive)  ||
				    s.hdrFormat.contains(m_filterText, Qt::CaseInsensitive)     ||
				    s.title.contains(m_filterText, Qt::CaseInsensitive)) {
					found = true;
					break;
				}
			}
		}
		if (!found) found = e.file.container.contains(m_filterText, Qt::CaseInsensitive);
		if (!found) return false;
	}

	// ── Ignored filter ────────────────────────────────────────────────────────
	if (m_filterIgnoredOnly) {
		if (!e.file.ignored) return false;
	} else {
		if (e.file.ignored) return false;
	}

	// ── Status filters ────────────────────────────────────────────────────────
	if (m_filterHasRemovals && !m_filesWithJobs.contains(e.file.id))
		return false;
	if (m_filterMissingImdb && m_posterPaths.contains(e.file.id))
		return false;

	// ── Quick-filter pills ────────────────────────────────────────────────────
	if (m_quickFilters != QF_None) {
		// 4K: use width, not height — rips are often vertically cropped
		if (m_quickFilters & QF_4K) {
			bool has4K = false;
			for (const StreamRecord& s : e.streams)
				if (s.codecType == QLatin1String("video") && (s.width >= 3840 || s.height >= 2160))
					{ has4K = true; break; }
			if (!has4K) return false;
		}

		// Dolby Vision: hdrFormat stores "DolbyVision", or codec profile is dvhe/dvav
		if (m_quickFilters & QF_DV) {
			bool hasDV = false;
			for (const StreamRecord& s : e.streams) {
				if (s.codecType != QLatin1String("video")) continue;
				if (s.hdrFormat == QLatin1String("DolbyVision") ||
				    s.codecProfile.startsWith(QLatin1String("dvhe"), Qt::CaseInsensitive) ||
				    s.codecProfile.startsWith(QLatin1String("dvav"), Qt::CaseInsensitive))
					{ hasDV = true; break; }
			}
			if (!hasDV) return false;
		}

		// HDR (non-DV): any non-empty hdrFormat that isn't Dolby Vision
		if (m_quickFilters & QF_HDR) {
			bool hasHDR = false;
			for (const StreamRecord& s : e.streams) {
				if (s.codecType != QLatin1String("video")) continue;
				if (!s.hdrFormat.isEmpty() && s.hdrFormat != QLatin1String("DolbyVision"))
					{ hasHDR = true; break; }
			}
			if (!hasHDR) return false;
		}

		// Audio group: OR within — a file passes if it has ANY of the active audio flags
		const quint32 audioMask = QF_Atmos | QF_TrueHD | QF_DtsHD | QF_DtsX;
		if (m_quickFilters & audioMask) {
			bool hasMatch = false;
			for (const StreamRecord& s : e.streams) {
				if (s.codecType != QLatin1String("audio")) continue;
				const QString n = s.codecName.toLower();
				const QString p = s.codecProfile.toLower();
				const QString t = s.title.toLower();

				// Atmos: TrueHD+Atmos or EAC3+Atmos
				if (!hasMatch && (m_quickFilters & QF_Atmos) &&
				    (n == QLatin1String("truehd") || n == QLatin1String("eac3")) &&
				    (p.contains(QLatin1String("atmos")) || t.contains(QLatin1String("atmos"))))
					hasMatch = true;

				// TrueHD: any TrueHD track (includes Atmos TrueHD)
				if (!hasMatch && (m_quickFilters & QF_TrueHD) && n == QLatin1String("truehd"))
					hasMatch = true;

				// DTS-HD MA: DTS with MA profile, including DTS:X (which is DTS-HD + object layer)
				if (!hasMatch && (m_quickFilters & QF_DtsHD) && n == QLatin1String("dts") &&
				    (p.contains(QLatin1String("ma")) || t.contains(QLatin1String("dts-hd")) ||
				     p.contains(QLatin1String("dts:x")) || t.contains(QLatin1String("dts:x"))))
					hasMatch = true;

				// DTS:X: object-based DTS audio specifically
				if (!hasMatch && (m_quickFilters & QF_DtsX) && n == QLatin1String("dts") &&
				    (p.contains(QLatin1String("dts:x")) || p.contains(QLatin1String("dts-x")) ||
				     t.contains(QLatin1String("dts:x"))))
					hasMatch = true;

				if (hasMatch) break;
			}
			if (!hasMatch) return false;
		}
	}

	// ── Rating filter ─────────────────────────────────────────────────────────
	const bool ratingFilterActive = (m_ratingMin > 0.0 || m_ratingMax < 10.0);
	if (ratingFilterActive) {
		const double r = m_ratings.value(e.file.id, -1.0);
		if (r < 0.0) return false;          // no rating — exclude when filter active
		if (r < m_ratingMin || r > m_ratingMax) return false;
	}

	return true;
}

bool McFileListModel::entryLessThan(const FileEntry& a, const FileEntry& b) const
{
	switch (m_sortOrder) {
	case SortByNewest:     return a.file.createdMs   > b.file.createdMs;
	case SortByOldest:     return a.file.createdMs   < b.file.createdMs;
	case SortByLargest:    return a.file.sizeBytes   > b.file.sizeBytes;
	case SortByRatingHigh: return m_ratings.value(a.file.id, 0.0) > m_ratings.value(b.file.id, 0.0);
	case SortByRatingLow:  return m_ratings.value(a.file.id, 0.0) < m_ratings.value(b.file.id, 0.0);
	default:               // SortByName
		return a.file.filename.compare(b.file.filename, Qt::CaseInsensitive) < 0;
	}
}

void McFileListModel::applyFilter()
{
	beginResetModel();
	m_entries.clear();
	for (const auto& e : m_allEntries)
		if (entryPassesFilter(e)) m_entries.append(e);

	std::sort(m_entries.begin(), m_entries.end(),
	          [this](const FileEntry& a, const FileEntry& b) { return entryLessThan(a, b); });
	endResetModel();
}

// ── Public API ────────────────────────────────────────────────────────────────

void McFileListModel::reload()
{
	QApplication::setOverrideCursor(Qt::WaitCursor);

	auto& db = DatabaseManager::instance();
	m_filesWithJobs = db.proposedJobFileIds();

	const QList<FileRecord>                  files   = db.allFiles();
	const QHash<qint64, QList<StreamRecord>> streams = db.allStreamsGrouped();
	m_allEntries.clear();
	m_allEntries.reserve(files.size());
	for (const FileRecord& f : files)
		m_allEntries.append({ f, streams.value(f.id) });

	m_posterPaths    = db.allDonePosterPaths();
	m_fanartPaths    = db.allDoneFanartPaths();
	m_imdbIds        = db.allKnownImdbIds();
	m_ratings        = db.allRatings();
	m_forcedRemovals = db.allStreamForcedRemovals();

	recomputeFolderCounts();
	applyFilter();

	QApplication::restoreOverrideCursor();
}

void McFileListModel::recomputeFolderCounts()
{
	QHash<QString, int> dirCounts;
	for (const FileEntry& e : m_allEntries)
		dirCounts[QFileInfo(e.file.path).absolutePath()]++;

	m_folderCounts.clear();
	m_folderCounts.reserve(m_allEntries.size());
	for (const FileEntry& e : m_allEntries)
		m_folderCounts[e.file.id] = dirCounts.value(QFileInfo(e.file.path).absolutePath(), 1);
}

void McFileListModel::initMeta(const QHash<qint64, QString>& posterPaths,
                               const QHash<qint64, QString>& imdbIds,
                               const QSet<qint64>& filesWithJobs,
                               const QHash<qint64, double>& ratings,
                               const QHash<qint64, QString>& fanartPaths)
{
	m_posterPaths    = posterPaths;
	m_imdbIds        = imdbIds;
	m_filesWithJobs  = filesWithJobs;
	m_forcedRemovals = DatabaseManager::instance().allStreamForcedRemovals();
	if (!ratings.isEmpty())     m_ratings     = ratings;
	if (!fanartPaths.isEmpty()) m_fanartPaths = fanartPaths;
	if (!m_entries.isEmpty()) {
		const QList<int> roles = {
			FanartRole, PosterRole, PosterVersionRole,
			DisplayTitleRole, DisplayYearRole, RatingRole, ImdbRole
		};
		emit dataChanged(index(0), index(m_entries.size() - 1), roles);
	}
	if (!m_forcedRemovals.isEmpty() && !m_entries.isEmpty())
		emit dataChanged(index(0), index(m_entries.size() - 1), {OverridesRole});
}

void McFileListModel::refreshJobFilter()
{
	m_filesWithJobs = DatabaseManager::instance().proposedJobFileIds();
	applyFilter();
}

void McFileListModel::applyEntry(const FileEntry& entry)
{
	const qint64 fileId = entry.file.id;

	bool foundInAll = false;
	for (int i = 0; i < m_allEntries.size(); ++i) {
		if (m_allEntries[i].file.id == fileId) {
			m_allEntries[i] = entry;
			foundInAll = true;
			break;
		}
	}
	if (!foundInAll) m_allEntries.append(entry);

	const bool passes = entryPassesFilter(entry);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].file.id == fileId) {
			if (passes) {
				m_entries[i] = entry;
				const QModelIndex idx = index(i);
				emit dataChanged(idx, idx, { FileRole, StreamsRole });
			} else {
				beginRemoveRows({}, i, i);
				m_entries.removeAt(i);
				endRemoveRows();
			}
			return;
		}
	}
	if (passes) {
		auto it = std::lower_bound(m_entries.begin(), m_entries.end(), entry,
		                           [this](const FileEntry& a, const FileEntry& b) { return entryLessThan(a, b); });
		const int insertPos = static_cast<int>(std::distance(m_entries.begin(), it));
		beginInsertRows({}, insertPos, insertPos);
		m_entries.insert(insertPos, entry);
		endInsertRows();
	}
}

void McFileListModel::reloadFile(qint64 fileId)
{
	auto& db = DatabaseManager::instance();
	const auto fileOpt = db.fileById(fileId);
	if (!fileOpt) return;
	applyEntry({ *fileOpt, db.streamsForFile(fileId) });
}

void McFileListModel::applyFileUpdate(const FileRecord& file, const QList<StreamRecord>& streams)
{
	applyEntry({ file, streams });
}

void McFileListModel::removeEntry(qint64 fileId)
{
	for (int i = 0; i < m_allEntries.size(); ++i) {
		if (m_allEntries[i].file.id == fileId) {
			m_allEntries.removeAt(i);
			break;
		}
	}
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].file.id == fileId) {
			beginRemoveRows({}, i, i);
			m_entries.removeAt(i);
			endRemoveRows();
			return;
		}
	}
}

void McFileListModel::setFilterText(const QString& text)
{
	if (m_filterText == text) return;
	m_filterText = text;
	applyFilter();
}

void McFileListModel::setFilterHasRemovals(bool on)
{
	if (m_filterHasRemovals == on) return;
	m_filterHasRemovals = on;
	applyFilter();
}

void McFileListModel::onPosterReady(qint64 fileId, const QString& imagePath)
{
	m_posterPaths[fileId] = imagePath;
	m_posterVersions[fileId]++;
	if (m_filterMissingImdb) {
		applyFilter();
		return;
	}
	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id != fileId) continue;
		const QModelIndex idx = index(row);
		// Include title/year/rating roles: onTmdbDataReady may have updated them
		// in the same event-loop batch, so we repaint everything at once.
		emit dataChanged(idx, idx, {PosterRole, PosterVersionRole,
		                            DisplayTitleRole, DisplayYearRole, RatingRole});
		break;
	}
}

void McFileListModel::onFanartReady(qint64 fileId, const QString& fanartPath,
                                    const QImage& image)
{
	m_fanartPaths[fileId] = fanartPath;
	if (!image.isNull())
		McCardDelegate::prefetchFanart(fanartPath, QPixmap::fromImage(image));
	m_pendingFanartIds[fileId] = fanartPath;
	if (!m_fanartBatchTimer.isActive())
		m_fanartBatchTimer.start();
}

void McFileListModel::onTmdbDataReady(qint64 fileId, const QString& title, int year, double rating)
{
	m_ratings[fileId] = rating;
	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id != fileId) continue;
		auto& entry = m_entries[row];
		if (!title.isEmpty()) {
			entry.file.displayTitle = title;
			entry.file.displayYear  = year;
		}
		const QModelIndex idx = index(row);
		emit dataChanged(idx, idx, {DisplayTitleRole, DisplayYearRole, RatingRole});
		break;
	}
}

void McFileListModel::onImdbIdSaved(qint64 fileId, const QString& imdbId)
{
	m_imdbIds[fileId] = imdbId;
	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			emit dataChanged(idx, idx, { ImdbRole });
			break;
		}
	}
}

void McFileListModel::setFilterMissingImdb(bool on)
{
	if (m_filterMissingImdb == on) return;
	m_filterMissingImdb = on;
	applyFilter();
}

void McFileListModel::setFilterIgnoredOnly(bool on)
{
	if (m_filterIgnoredOnly == on) return;
	m_filterIgnoredOnly = on;
	applyFilter();
}

void McFileListModel::setStatusFilter(int statusIndex)
{
	const bool hasRemovals  = (statusIndex == 1);
	const bool missingImdb  = (statusIndex == 2);
	const bool ignoredOnly  = (statusIndex == 3);
	if (m_filterHasRemovals == hasRemovals && m_filterMissingImdb == missingImdb
	    && m_filterIgnoredOnly == ignoredOnly)
		return;
	m_filterHasRemovals = hasRemovals;
	m_filterMissingImdb = missingImdb;
	m_filterIgnoredOnly = ignoredOnly;
	applyFilter();
}

void McFileListModel::setIgnoredBatch(const QList<qint64>& fileIds, bool ignored)
{
	const QSet<qint64> idSet(fileIds.begin(), fileIds.end());
	for (auto& e : m_allEntries)
		if (idSet.contains(e.file.id)) e.file.ignored = ignored;
	applyFilter();
}

void McFileListModel::setQuickFilters(quint32 flags)
{
	if (m_quickFilters == flags) return;
	m_quickFilters = flags;
	applyFilter();
}

void McFileListModel::setSortOrder(int order)
{
	if (m_sortOrder == order) return;
	m_sortOrder = order;
	applyFilter();
}

void McFileListModel::setRatingFilter(double minRating, double maxRating)
{
	if (qFuzzyCompare(m_ratingMin, minRating) && qFuzzyCompare(m_ratingMax, maxRating)) return;
	m_ratingMin = minRating;
	m_ratingMax = maxRating;
	applyFilter();
}

void McFileListModel::setRatingForFile(qint64 fileId, double rating)
{
	if (rating > 0.0) m_ratings[fileId] = rating;
	else              m_ratings.remove(fileId);

	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			emit dataChanged(idx, idx, { RatingRole });
			break;
		}
	}
	// If the rating filter is active this file may now enter or leave the visible set
	if (m_ratingMin > 0.0 || m_ratingMax < 10.0) applyFilter();
}

// ── QAbstractListModel ────────────────────────────────────────────────────────

int McFileListModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return m_entries.size();
}

QVariant McFileListModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.row() >= m_entries.size())
		return {};

	const FileEntry& e = m_entries.at(index.row());

	switch (role) {
	case Qt::DisplayRole: return e.file.filename;
	case FileRole:        return QVariant::fromValue(e.file);
	case StreamsRole:     return QVariant::fromValue(e.streams);
	case PosterRole:        return m_posterPaths.value(e.file.id);
	case FanartRole:        return m_fanartPaths.value(e.file.id);
	case PosterVersionRole: return m_posterVersions.value(e.file.id, 0);
	case ImdbRole:          return m_imdbIds.value(e.file.id);
	case RatingRole:        return m_ratings.value(e.file.id, 0.0);
	case DisplayTitleRole:  return e.file.displayTitle;
	case DisplayYearRole:   return e.file.displayYear;
	case ContainerTitleRole:return e.file.containerTitle;
	case FolderCountRole:
		return m_folderCounts.value(e.file.id, 1);
	case OverridesRole:   return QVariant::fromValue(m_forcedRemovals.value(e.file.id));
	default:              return {};
	}
}

void McFileListModel::toggleForcedRemoval(qint64 fileId, int streamIndex)
{
	auto& set = m_forcedRemovals[fileId];
	const bool nowForced = !set.contains(streamIndex);
	if (nowForced)
		set.insert(streamIndex);
	else
		set.remove(streamIndex);
	DatabaseManager::instance().setStreamForcedRemoval(fileId, streamIndex, nowForced);

	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			emit dataChanged(idx, idx, { OverridesRole });
			break;
		}
	}
}

} // namespace Mc
