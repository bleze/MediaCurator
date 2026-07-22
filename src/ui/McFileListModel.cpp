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

void McFileListModel::computeDerived(FileEntry& e)
{
	QFileInfo fi(e.file.path);
	e.dirName = fi.dir().dirName();
	e.parentPath = fi.absolutePath();
	e.storageGroup = StorageGroupSettings::groupForFilePath(e.file.path);

	QString s;
	s.reserve(128 + e.streams.size() * 32);
	s += e.file.filename;
	s += QChar(' ');
	s += e.dirName;
	s += QChar(' ');
	s += e.file.container;
	s += QChar(' ');
	s += e.file.originalLanguage;
	s += QChar(' ');
	for (const StreamRecord& st : e.streams) {
		s += st.codecName;
		s += QChar(' ');
		s += st.codecProfile;
		s += QChar(' ');
		s += st.hdrFormat;
		if (st.maxCll > 0) { s += QString::number(st.maxCll); s += QChar(' '); }
		if (st.maxFall > 0) { s += QString::number(st.maxFall); s += QChar(' '); }
		s += st.masteringDisplay;
		s += QChar(' ');
		s += st.title;
		s += QChar(' ');
		s += st.language;
		s += QChar(' ');
	}
	e.searchText = s.toLower();

	// Precompute quick-filter flags by scanning streams once + populate grouped lists
	e.has4K = e.hasDV = e.hasHDR = e.hasAtmos = e.hasTrueHD = e.hasDtsHD = e.hasDtsX = false;
	e.videoStreams.clear();
	e.audioStreams.clear();
	e.subtitleStreams.clear();
	for (const StreamRecord& st : e.streams) {
		if (st.codecType == QLatin1String("video")) {
			e.videoStreams.append(st);
			if (st.width >= 3840 || st.height >= 2160)
				e.has4K = true;
			if (st.hdrFormat == QLatin1String("DolbyVision") ||
			    st.codecProfile.startsWith(QLatin1String("dvhe"), Qt::CaseInsensitive) ||
			    st.codecProfile.startsWith(QLatin1String("dvav"), Qt::CaseInsensitive))
				e.hasDV = true;
			if (!st.hdrFormat.isEmpty() && st.hdrFormat != QLatin1String("DolbyVision"))
				e.hasHDR = true;
		} else if (st.codecType == QLatin1String("audio")) {
			e.audioStreams.append(st);
			const QString n = st.codecName.toLower();
			const QString p = st.codecProfile.toLower();
			const QString t = st.title.toLower();
			if ((n == QLatin1String("truehd") || n == QLatin1String("eac3")) &&
			    (p.contains(QLatin1String("atmos")) || t.contains(QLatin1String("atmos"))))
				e.hasAtmos = true;
			if (n == QLatin1String("truehd"))
				e.hasTrueHD = true;
			if (n == QLatin1String("dts") &&
			    (p.contains(QLatin1String("ma")) || t.contains(QLatin1String("dts-hd")) ||
			     p.contains(QLatin1String("dts:x")) || t.contains(QLatin1String("dts:x"))))
				e.hasDtsHD = true;
			if (n == QLatin1String("dts") &&
			    (p.contains(QLatin1String("dts:x")) || p.contains(QLatin1String("dts-x")) ||
			     t.contains(QLatin1String("dts:x"))))
				e.hasDtsX = true;
		} else if (st.codecType == QLatin1String("subtitle")) {
			e.subtitleStreams.append(st);
		}
	}
}

// ── Filter helpers ────────────────────────────────────────────────────────────

bool McFileListModel::entryPassesFilter(const FileEntry& e) const
{
	// ── Text search: filename, parent folder, stream metadata, container ──────
	if (!m_filterText.isEmpty()) {
		if (!e.searchText.contains(m_filterText, Qt::CaseInsensitive))
			return false;
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
		if ((m_quickFilters & QF_4K) && !e.has4K) return false;
		if ((m_quickFilters & QF_DV) && !e.hasDV) return false;
		if ((m_quickFilters & QF_HDR) && !e.hasHDR) return false;

		const quint32 audioMask = QF_Atmos | QF_TrueHD | QF_DtsHD | QF_DtsX;
		if (m_quickFilters & audioMask) {
			bool hasAudioMatch = false;
			if ((m_quickFilters & QF_Atmos) && e.hasAtmos) hasAudioMatch = true;
			if ((m_quickFilters & QF_TrueHD) && e.hasTrueHD) hasAudioMatch = true;
			if ((m_quickFilters & QF_DtsHD) && e.hasDtsHD) hasAudioMatch = true;
			if ((m_quickFilters & QF_DtsX) && e.hasDtsX) hasAudioMatch = true;
			if (!hasAudioMatch) return false;
		}

		// Media category pills — OR within the group (Movies | TV | Docs | Misc).
		const quint32 mediaMask = QF_Movie | QF_Tv | QF_Documentary | QF_Misc;
		if (m_quickFilters & mediaMask) {
			const QString t = MediaTypes::normalize(e.file.mediaType);
			bool hasMediaMatch = false;
			if ((m_quickFilters & QF_Movie)
			    && t == QLatin1String(MediaTypes::Movie)) hasMediaMatch = true;
			if ((m_quickFilters & QF_Tv)
			    && t == QLatin1String(MediaTypes::Tv)) hasMediaMatch = true;
			if ((m_quickFilters & QF_Documentary)
			    && t == QLatin1String(MediaTypes::Documentary)) hasMediaMatch = true;
			if ((m_quickFilters & QF_Misc)
			    && (t == QLatin1String(MediaTypes::Misc)
			        || t == QLatin1String(MediaTypes::Unknown))) hasMediaMatch = true;
			if (!hasMediaMatch) return false;
		}
	}

	// ── Storage group filter ──────────────────────────────────────────────────
	if (m_storageGroupMask != 0 && !(m_storageGroupMask & (1u << e.storageGroup)))
		return false;

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
	case SortByLastScanned:return a.file.scanTime    > b.file.scanTime;
	default:               // SortByName
		return a.file.filename.compare(b.file.filename, Qt::CaseInsensitive) < 0;
	}
}

void McFileListModel::sortAllEntries()
{
	std::sort(m_allEntries.begin(), m_allEntries.end(),
	          [this](const FileEntry& a, const FileEntry& b) { return entryLessThan(a, b); });
}

void McFileListModel::applyFilter(bool forceFullReset)
{
	// Build the filtered view as an order-preserving subsequence of m_allEntries.
	// m_allEntries must already be in the active sort order (see sortAllEntries /
	// setSortOrder / reload) so we never re-sort here — re-sorting would break the
	// subsequence invariant the incremental diff below relies on.
	QList<FileEntry> newEntries;
	newEntries.reserve(m_allEntries.size());
	for (const auto& e : m_allEntries)
		if (entryPassesFilter(e)) newEntries.append(e);

	if (forceFullReset || m_entries.isEmpty()) {
		beginResetModel();
		m_entries = std::move(newEntries);
		endResetModel();
		return;
	}

	// Diff against the previous filtered list instead of a full model reset.
	// Matched rows keep their indices (scroll position + size cache stay warm);
	// only pure insertions/removals are emitted. Same idea as McJobListModel.
	QSet<qint64> newIds;
	newIds.reserve(newEntries.size());
	for (const auto& e : newEntries) newIds.insert(e.file.id);
	QSet<qint64> oldIds;
	oldIds.reserve(m_entries.size());
	for (const auto& e : m_entries) oldIds.insert(e.file.id);

	int i = 0, j = 0;
	while (i < m_entries.size() || j < newEntries.size()) {
		if (i < m_entries.size() && j < newEntries.size()
		    && m_entries[i].file.id == newEntries[j].file.id) {
			// Same file still visible — keep row; no dataChanged (filter-only).
			++i;
			++j;
		} else if (i < m_entries.size() && !newIds.contains(m_entries[i].file.id)) {
			// Batch consecutive removals for large filter flips (e.g. Movies on
			// a mostly-movie library removes thousands of unknowns at once).
			int end = i;
			while (end + 1 < m_entries.size()
			       && !newIds.contains(m_entries[end + 1].file.id))
				++end;
			beginRemoveRows({}, i, end);
			m_entries.erase(m_entries.begin() + i, m_entries.begin() + end + 1);
			endRemoveRows();
		} else if (j < newEntries.size() && !oldIds.contains(newEntries[j].file.id)) {
			int endJ = j;
			while (endJ + 1 < newEntries.size()
			       && !oldIds.contains(newEntries[endJ + 1].file.id))
				++endJ;
			const int count = endJ - j + 1;
			beginInsertRows({}, i, i + count - 1);
			for (int k = 0; k < count; ++k)
				m_entries.insert(i + k, newEntries[j + k]);
			endInsertRows();
			i += count;
			j = endJ + 1;
		} else {
			// Subsequence invariant broken (sort keys changed without a full
			// re-sort of m_allEntries). Fall back to a clean rebuild.
			beginResetModel();
			m_entries = std::move(newEntries);
			endResetModel();
			return;
		}
	}
}

// ── Public API ────────────────────────────────────────────────────────────────

bool McFileListModel::hasClassifiedMediaTypes() const
{
	for (const FileEntry& e : m_allEntries) {
		const QString t = MediaTypes::normalize(e.file.mediaType);
		if (t != QLatin1String(MediaTypes::Unknown))
			return true;
	}
	return false;
}

void McFileListModel::notifyMediaCategoriesAvailability()
{
	const bool has = hasClassifiedMediaTypes();
	if (has == m_hadClassifiedMedia) return;
	m_hadClassifiedMedia = has;
	emit mediaCategoriesAvailabilityChanged(has);
}

void McFileListModel::reload()
{
	QApplication::setOverrideCursor(Qt::WaitCursor);

	auto& db = DatabaseManager::instance();
	m_filesWithJobs = db.proposedJobFileIds();

	const QList<FileRecord>                  files   = db.allFiles();
	const QHash<qint64, QList<StreamRecord>> streams = db.allStreamsGrouped();
	m_allEntries.clear();
	m_allEntries.reserve(files.size());
	for (const FileRecord& f : files) {
		FileEntry e{ f, streams.value(f.id) };
		computeDerived(e);
		m_allEntries.append(e);
	}

	db.loadPosterMeta(m_posterPaths, m_imdbIds, m_ratings, m_fanartPaths, m_tmdbIds);
	m_forcedRemovals = db.allStreamForcedRemovals();

	recomputeFolderCounts();
	sortAllEntries();
	applyFilter(/*forceFullReset=*/true);
	notifyMediaCategoriesAvailability();

	QApplication::restoreOverrideCursor();
}

void McFileListModel::recomputeFolderCounts()
{
	QHash<QString, int> dirCounts;
	for (const FileEntry& e : m_allEntries)
		dirCounts[e.parentPath]++;

	m_folderCounts.clear();
	m_folderCounts.reserve(m_allEntries.size());
	for (const FileEntry& e : m_allEntries)
		m_folderCounts[e.file.id] = dirCounts.value(e.parentPath, 1);
}

void McFileListModel::initMeta(const QHash<qint64, QString>& posterPaths,
                               const QHash<qint64, QString>& imdbIds,
                               const QSet<qint64>& filesWithJobs,
                               const QHash<qint64, double>& ratings,
                               const QHash<qint64, QString>& fanartPaths,
                               const QHash<qint64, int>& tmdbIds)
{
	m_posterPaths    = posterPaths;
	m_imdbIds        = imdbIds;
	m_filesWithJobs  = filesWithJobs;
	if (m_forcedRemovals.isEmpty()) {
		// Only query on first population to avoid repeated small queries during background meta delivery.
		m_forcedRemovals = DatabaseManager::instance().allStreamForcedRemovals();
	}
	if (!ratings.isEmpty())     m_ratings     = ratings;
	if (!fanartPaths.isEmpty()) m_fanartPaths = fanartPaths;
	if (!tmdbIds.isEmpty())     m_tmdbIds     = tmdbIds;
	if (!m_entries.isEmpty()) {
		const QList<int> roles = {
			FanartRole, PosterRole, PosterVersionRole,
			DisplayTitleRole, DisplayYearRole, RatingRole, ImdbRole, TmdbRole
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

	FileEntry e = entry;
	computeDerived(e);  // always recompute (cheap); ensures searchText + quick flags are fresh after rescan/update

	bool foundInAll = false;
	for (int i = 0; i < m_allEntries.size(); ++i) {
		if (m_allEntries[i].file.id == fileId) {
			m_allEntries[i] = e;
			foundInAll = true;
			break;
		}
	}
	if (!foundInAll) m_allEntries.append(e);

	// Newly loaded/updated entries may introduce the first classified type.
	notifyMediaCategoriesAvailability();

	const bool passes = entryPassesFilter(e);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].file.id == fileId) {
			if (passes) {
				m_entries[i] = e;
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
		auto it = std::lower_bound(m_entries.begin(), m_entries.end(), e,
		                           [this](const FileEntry& a, const FileEntry& b) { return entryLessThan(a, b); });
		const int insertPos = static_cast<int>(std::distance(m_entries.begin(), it));
		beginInsertRows({}, insertPos, insertPos);
		m_entries.insert(insertPos, e);
		endInsertRows();
		const QModelIndex idx = index(insertPos);
		if (m_posterPaths.contains(fileId) || m_fanartPaths.contains(fileId)) {
			emit dataChanged(idx, idx, {PosterRole, PosterVersionRole, FanartRole});
		}
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

void McFileListModel::onTmdbDataReady(qint64 fileId, const QString& title, int year, double rating,
                                      const QString& mediaType)
{
	m_ratings[fileId] = rating;
	const QString normalizedType = mediaType.isEmpty()
	    ? QString{} : MediaTypes::normalize(mediaType);

	// Keep the unfiltered set in sync so refilters don't lose the update.
	for (auto& e : m_allEntries) {
		if (e.file.id != fileId) continue;
		if (!title.isEmpty()) {
			e.file.displayTitle = title;
			e.file.displayYear  = year;
		}
		if (!normalizedType.isEmpty()
		    && normalizedType != QLatin1String(MediaTypes::Unknown))
			e.file.mediaType = normalizedType;
		break;
	}

	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id != fileId) continue;
		auto& entry = m_entries[row];
		if (!title.isEmpty()) {
			entry.file.displayTitle = title;
			entry.file.displayYear  = year;
		}
		if (!normalizedType.isEmpty()
		    && normalizedType != QLatin1String(MediaTypes::Unknown))
			entry.file.mediaType = normalizedType;
		const QModelIndex idx = index(row);
		emit dataChanged(idx, idx, {DisplayTitleRole, DisplayYearRole, RatingRole, FileRole});
		break;
	}

	// Category filter may now include/exclude this file.
	const quint32 mediaMask = QF_Movie | QF_Tv | QF_Documentary | QF_Misc;
	if ((m_quickFilters & mediaMask) && !normalizedType.isEmpty())
		applyFilter();

	if (!normalizedType.isEmpty()
	    && normalizedType != QLatin1String(MediaTypes::Unknown))
		notifyMediaCategoriesAvailability();
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

void McFileListModel::onTmdbIdSaved(qint64 fileId, int tmdbId)
{
	m_tmdbIds[fileId] = tmdbId;
	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			emit dataChanged(idx, idx, { TmdbRole });
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

void McFileListModel::setStorageGroupFilter(quint32 groupMask)
{
	if (m_storageGroupMask == groupMask) return;
	m_storageGroupMask = groupMask;
	applyFilter();
}

void McFileListModel::setSortOrder(int order)
{
	if (m_sortOrder == order) return;
	m_sortOrder = order;
	// Sort the master list first so applyFilter can treat the visible set as a
	// subsequence (incremental). Full reset because every row reorders.
	sortAllEntries();
	applyFilter(/*forceFullReset=*/true);
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

void McFileListModel::setDisplayTitleForFile(qint64 fileId, const QString& title, int year)
{
	if (title.isEmpty()) return;

	for (auto& e : m_allEntries) {
		if (e.file.id != fileId) continue;
		e.file.displayTitle = title;
		e.file.displayYear  = year;
		break;
	}

	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id != fileId) continue;
		m_entries[row].file.displayTitle = title;
		m_entries[row].file.displayYear  = year;
		const QModelIndex idx = index(row);
		emit dataChanged(idx, idx, { DisplayTitleRole, DisplayYearRole });
		break;
	}
}

void McFileListModel::setMediaTypeForFile(qint64 fileId, const QString& mediaType)
{
	const QString t = MediaTypes::normalize(mediaType);
	for (auto& e : m_allEntries) {
		if (e.file.id != fileId) continue;
		e.file.mediaType = t;
		break;
	}
	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id != fileId) continue;
		m_entries[row].file.mediaType = t;
		const QModelIndex idx = index(row);
		emit dataChanged(idx, idx, { FileRole });
		break;
	}
	const quint32 mediaMask = QF_Movie | QF_Tv | QF_Documentary | QF_Misc;
	if (m_quickFilters & mediaMask)
		applyFilter();
	notifyMediaCategoriesAvailability();
}

void McFileListModel::setMediaTypeBatch(const QList<qint64>& fileIds, const QString& mediaType)
{
	const QString t = MediaTypes::normalize(mediaType);
	const QSet<qint64> idSet(fileIds.begin(), fileIds.end());
	for (auto& e : m_allEntries)
		if (idSet.contains(e.file.id)) e.file.mediaType = t;
	// Visible set may change under a category filter — rebuild.
	applyFilter();
	notifyMediaCategoriesAvailability();
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
	case TmdbRole:          return m_tmdbIds.value(e.file.id, 0);
	case RatingRole:        return m_ratings.value(e.file.id, 0.0);
	case DisplayTitleRole:  return e.file.displayTitle;
	case DisplayYearRole:   return e.file.displayYear;
	case ContainerTitleRole:return e.file.containerTitle;
	case FolderCountRole:
		return m_folderCounts.value(e.file.id, 1);
	case OverridesRole:   return QVariant::fromValue(m_forcedRemovals.value(e.file.id));
	case VideoStreamsRole:   return QVariant::fromValue(e.videoStreams);
	case AudioStreamsRole:   return QVariant::fromValue(e.audioStreams);
	case SubtitleStreamsRole:return QVariant::fromValue(e.subtitleStreams);
	case FileIdRole:         return e.file.id;
	case StorageGroupRole:   return e.storageGroup;
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
