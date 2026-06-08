#include "ui/McFileListModel.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace Mc {

McFileListModel::McFileListModel(QObject* parent)
	: QAbstractListModel(parent)
{}

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

	return true;
}

void McFileListModel::applyFilter()
{
	beginResetModel();
	m_entries.clear();
	for (const auto& e : m_allEntries)
		if (entryPassesFilter(e)) m_entries.append(e);

	std::sort(m_entries.begin(), m_entries.end(),
	          [this](const FileEntry& a, const FileEntry& b) {
		switch (m_sortOrder) {
		case SortByNewest:  return a.file.createdMs > b.file.createdMs;
		case SortByOldest:  return a.file.createdMs < b.file.createdMs;
		case SortByLargest: return a.file.sizeBytes > b.file.sizeBytes;
		default:            // SortByName
			return a.file.filename.compare(b.file.filename, Qt::CaseInsensitive) < 0;
		}
	});
	endResetModel();
}

// ── Public API ────────────────────────────────────────────────────────────────

void McFileListModel::reload()
{
	// Refresh proposed-job file IDs for the "with removals" filter
	m_filesWithJobs.clear();
	for (const auto& j : DatabaseManager::instance().allJobsForPanel())
		if (j.status == "proposed") m_filesWithJobs.insert(j.fileId);

	auto& db = DatabaseManager::instance();
	m_allEntries.clear();
	const QList<FileRecord>                    files   = db.allFiles();
	const QHash<qint64, QList<StreamRecord>>   streams = db.allStreamsGrouped();
	m_allEntries.reserve(files.size());
	for (const FileRecord& f : files)
		m_allEntries.append({ f, streams.value(f.id) });

	// Pre-load poster paths and IMDb IDs already stored in the DB.
	m_posterPaths = db.allDonePosterPaths();
	m_imdbIds     = db.allKnownImdbIds();

	applyFilter();
}

void McFileListModel::refreshJobFilter()
{
	m_filesWithJobs.clear();
	for (const auto& j : DatabaseManager::instance().allJobsForPanel())
		if (j.status == "proposed") m_filesWithJobs.insert(j.fileId);
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
				emit dataChanged(idx, idx);
			} else {
				beginRemoveRows({}, i, i);
				m_entries.removeAt(i);
				endRemoveRows();
			}
			return;
		}
	}
	if (passes) {
		beginInsertRows({}, m_entries.size(), m_entries.size());
		m_entries.append(entry);
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
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			// Specify roles so Qt issues a targeted viewport update rather than a
			// full doItemsLayout() (which empty roles triggers via SizeHintRole).
			emit dataChanged(idx, idx, {PosterRole, PosterVersionRole});
			break;
		}
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
	case PosterVersionRole: return m_posterVersions.value(e.file.id, 0);
	case ImdbRole:          return m_imdbIds.value(e.file.id);
	case OverridesRole:   return QVariant::fromValue(m_forcedRemovals.value(e.file.id));
	default:              return {};
	}
}

void McFileListModel::toggleForcedRemoval(qint64 fileId, int streamIndex)
{
	auto& set = m_forcedRemovals[fileId];
	if (set.contains(streamIndex))
		set.remove(streamIndex);
	else
		set.insert(streamIndex);

	for (int row = 0; row < m_entries.size(); ++row) {
		if (m_entries.at(row).file.id == fileId) {
			const QModelIndex idx = index(row);
			emit dataChanged(idx, idx, { OverridesRole });
			break;
		}
	}
}

} // namespace Mc
