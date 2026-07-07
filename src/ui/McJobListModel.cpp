#include "ui/McJobListModel.h"
#include "ui/McFilterPanel.h"
#include "ui/McCardDelegate.h"
#include "engine/ActionEngine.h"
#include "engine/TrackDecision.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QPixmapCache>
#include <QStringList>
#include <algorithm>

namespace Mc {

// Sort m_allEntries by the live savings estimate (same formula as the badge).
// j.saved_bytes can be stale when jobs were created with an older formula;
// this ensures "Largest savings first" matches what the user sees in the card.
static void sortEntriesByDisplaySavings(QList<JobCardEntry>& entries, QList<Qt::CheckState>& checks)
{
	const int n = entries.size();
	if (n < 2) return;

	QList<qint64> saved(n);
	for (int i = 0; i < n; ++i) {
		const JobCardEntry& e = entries[i];
		QSet<int> keptIdx;
		for (const auto& s : e.keptStreams) keptIdx.insert(s.streamIndex);
		QSet<int> removed;
		for (const auto& s : e.allStreams)
			if (!keptIdx.contains(s.streamIndex)) removed.insert(s.streamIndex);
		saved[i] = estimateSavingBytes(e.allStreams, removed, e.job.sizeBytes, e.job.durationSec);
	}

	QList<int> perm(n);
	for (int i = 0; i < n; ++i) perm[i] = i;
	std::stable_sort(perm.begin(), perm.end(), [&](int a, int b) { return saved[a] > saved[b]; });

	QList<JobCardEntry>    sortedE;
	QList<Qt::CheckState>  sortedC;
	sortedE.reserve(n);
	sortedC.reserve(n);
	for (int i : perm) {
		sortedE.append(std::move(entries[i]));
		sortedC.append(checks[i]);
	}
	entries = std::move(sortedE);
	checks  = std::move(sortedC);
}

// Bakes pending flag_changes_json overrides (default/forced/original/language) onto the
// freshly-loaded stream records, so badges reflect a pending change immediately after a
// reload — not just while the change is still held in the in-memory entry that made it
// (e.g. setStreamFlag/setStreamLanguage), which doesn't survive reload() / reloadPaged().
static void applyPendingFlagChanges(QList<StreamRecord>& streams, const QString& flagChangesJson)
{
	if (flagChangesJson.isEmpty()) return;
	const QJsonArray changes = QJsonDocument::fromJson(flagChangesJson.toUtf8()).array();
	for (const QJsonValue& v : changes) {
		const QJsonObject o = v.toObject();
		const int     streamIndex = o[QLatin1String("streamIndex")].toInt();
		const QString flag        = o[QLatin1String("flag")].toString();
		for (StreamRecord& s : streams) {
			if (s.streamIndex != streamIndex) continue;
			if      (flag == QLatin1String("default"))  s.isDefault  = o[QLatin1String("value")].toBool();
			else if (flag == QLatin1String("forced"))   s.isForced   = o[QLatin1String("value")].toBool();
			else if (flag == QLatin1String("original")) s.isOriginal = o[QLatin1String("value")].toBool();
			else if (flag == QLatin1String("language")) s.language   = o[QLatin1String("value")].toString();
			break;
		}
	}
}

McJobListModel::McJobListModel(QObject* parent)
	: QAbstractListModel(parent)
{
	m_fanartBatchTimer.setSingleShot(true);
	m_fanartBatchTimer.setInterval(150);
	connect(&m_fanartBatchTimer, &QTimer::timeout, this, [this]() {
		for (auto it = m_pendingFanartIds.cbegin(); it != m_pendingFanartIds.cend(); ++it) {
			const qint64 fileId = it.key();
			for (int i = 0; i < m_entries.size(); ++i) {
				if (m_entries[i].job.fileId == fileId) {
					const QModelIndex idx = index(i);
					emit dataChanged(idx, idx, {FanartRole});
				}
			}
		}
		m_pendingFanartIds.clear();
	});
}

// ── Public API ────────────────────────────────────────────────────────────────

void McJobListModel::reload()
{
	m_allEntries.clear();
	m_allCheckStates.clear();
	m_posterPaths = DatabaseManager::instance().allDonePosterPaths();
	m_fanartPaths = DatabaseManager::instance().allDoneFanartPaths();

	auto& db = DatabaseManager::instance();
	const auto displayJobs = db.allJobsForPanel(m_sortMode);

	const auto allJobRecords = db.allJobs();
	QHash<qint64, QString> argsMap;
	QHash<qint64, QString> origStreamsMap;
	QHash<qint64, QString> flagChangesMap;
	QSet<qint64> liveJobIds;
	for (const JobRecord& j : allJobRecords) {
		argsMap.insert(j.id, j.commandArgsJson);
		origStreamsMap.insert(j.id, j.originalStreamsJson);
		flagChangesMap.insert(j.id, j.flagChangesJson);
		liveJobIds.insert(j.id);
	}

	// Prune stale progress entries (deleted jobs) but keep the running job's value
	// so it survives the reload and the card doesn't flash back to 0 % mid-run.
	for (auto it = m_progress.begin(); it != m_progress.end(); ) {
		it = liveJobIds.contains(it.key()) ? ++it : m_progress.erase(it);
	}
	for (auto it = m_outputSize.begin(); it != m_outputSize.end(); ) {
		it = liveJobIds.contains(it.key()) ? ++it : m_outputSize.erase(it);
	}

	// Batch-load streams for all jobs that need live DB data (non-done, or done without snapshot)
	QList<qint64> fileIdsForBatch;
	for (const JobDisplayRecord& djr : displayJobs) {
		const QString& origJson = origStreamsMap.value(djr.jobId);
		if (!(djr.status == QLatin1String("done") && !origJson.isEmpty()))
			fileIdsForBatch << djr.fileId;
	}
	const auto streamsMap = db.streamsForFiles(fileIdsForBatch);

	for (const JobDisplayRecord& djr : displayJobs) {
		JobCardEntry e;
		e.job = djr;
		const QString& origJson = origStreamsMap.value(djr.jobId);
		if (djr.status == QLatin1String("done") && !origJson.isEmpty()) {
			// Use the pre-remux stream snapshot so removed tracks (now gone from DB)
			// still show with strikethrough and indices match the original command args.
			e.allStreams = streamsFromJson(origJson);
		} else {
			e.allStreams = streamsMap.value(djr.fileId);
		}
		e.flagChangesJson = flagChangesMap.value(djr.jobId);
		applyPendingFlagChanges(e.allStreams, e.flagChangesJson);
		e.keptStreams = computeKeptStreams(e.allStreams, argsMap.value(djr.jobId));
		m_allEntries.append(e);
		m_allCheckStates.append(djr.status == QLatin1String("proposed") ? Qt::Checked : Qt::Unchecked);
	}

	if (m_sortMode == JobSortMode::LargestSavingsFirst)
		sortEntriesByDisplaySavings(m_allEntries, m_allCheckStates);
	applyFilter();
}

void McJobListModel::reloadPaged(int limit)
{
	// "Running" filter must also show queued jobs — delegate to full reload so both
	// statuses enter m_allEntries (the paged DB query would otherwise drop 'queued').
	if (m_filterStatus == QLatin1String("running")) {
		reload();
		return;
	}

	m_allEntries.clear();
	m_allCheckStates.clear();

	auto& db = DatabaseManager::instance();
	m_posterPaths = db.allDonePosterPaths();
	m_fanartPaths = db.allDoneFanartPaths();
	const auto displayJobs = db.allJobsForPanelPaged(limit, m_filterStatus, m_sortMode);

	// Batch-load live streams for jobs that don't have an original-streams snapshot
	QList<qint64> fileIds;
	fileIds.reserve(displayJobs.size());
	for (const JobDisplayRecord& djr : displayJobs)
		if (!(djr.status == QLatin1String("done") && !djr.originalStreamsJson.isEmpty()))
			fileIds << djr.fileId;
	const auto streamsMap = db.streamsForFiles(fileIds);

	for (const JobDisplayRecord& djr : displayJobs) {
		JobCardEntry e;
		e.job = djr;
		if (djr.status == QLatin1String("done") && !djr.originalStreamsJson.isEmpty())
			e.allStreams = streamsFromJson(djr.originalStreamsJson);
		else
			e.allStreams = streamsMap.value(djr.fileId);
		e.flagChangesJson = djr.flagChangesJson;
		applyPendingFlagChanges(e.allStreams, e.flagChangesJson);
		e.keptStreams = computeKeptStreams(e.allStreams, djr.commandArgsJson);
		m_allEntries.append(e);
		m_allCheckStates.append(djr.status == QLatin1String("proposed") ? Qt::Checked : Qt::Unchecked);
	}

	if (m_sortMode == JobSortMode::LargestSavingsFirst)
		sortEntriesByDisplaySavings(m_allEntries, m_allCheckStates);
	applyFilter();
}

void McJobListModel::setSortMode(JobSortMode sortMode)
{
	m_sortMode = sortMode;
}

void McJobListModel::updateJob(qint64 jobId, const QString& status, qint64 savedBytes)
{
	// Update master list
	for (auto& e : m_allEntries) {
		if (e.job.jobId != jobId) continue;
		e.job.status = status;
		if (savedBytes >= 0) e.job.savedBytes = savedBytes;
		break;
	}
	// Update filtered view — or reapply if status filter makes entry appear/disappear
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId != jobId) continue;
		m_entries[i].job.status = status;
		if (savedBytes >= 0) m_entries[i].job.savedBytes = savedBytes;
		if (!statusMatchesFilter(status)) {
			applyFilter();   // entry no longer passes status filter
		} else {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { StatusRole, SavedRole });
		}
		return;
	}
	// Not in filtered view — check if the status change makes it visible now
	if (statusMatchesFilter(status))
		applyFilter();
}

void McJobListModel::removeJobIds(const QList<qint64>& ids)
{
	if (ids.isEmpty()) return;
	const QSet<qint64> idSet(ids.begin(), ids.end());
	for (int i = m_allEntries.size() - 1; i >= 0; --i) {
		if (idSet.contains(m_allEntries[i].job.jobId)) {
			m_allEntries.removeAt(i);
			m_allCheckStates.removeAt(i);
		}
	}
	applyFilter();
}

void McJobListModel::setFilterText(const QString& text)
{
	if (m_filterText == text) return;
	m_filterText = text;
	applyFilter();
}

void McJobListModel::setFilterStatus(const QString& status)
{
	if (m_filterStatus == status) return;
	m_filterStatus = status;
	applyFilter();
}

void McJobListModel::setQuickFilters(quint32 flags)
{
	if (m_quickFilters == flags) return;
	m_quickFilters = flags;
	applyFilter();
}

void McJobListModel::setRatingFilter(double minRating, double maxRating)
{
	if (qFuzzyCompare(m_ratingMin, minRating) && qFuzzyCompare(m_ratingMax, maxRating)) return;
	m_ratingMin = minRating;
	m_ratingMax = maxRating;
	applyFilter();
}

void McJobListModel::setRatingForFile(qint64 fileId, double rating)
{
	for (auto& e : m_allEntries)
		if (e.job.fileId == fileId) { e.job.voteAverage = rating; break; }

	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId != fileId) continue;
		m_entries[i].job.voteAverage = rating;
		emit dataChanged(index(i), index(i), { RatingRole });
		break;
	}
}

QHash<QString, int> McJobListModel::countsByStatus() const
{
	QHash<QString, int> counts;
	for (const auto& e : m_allEntries)
		++counts[e.job.status];
	return counts;
}

void McJobListModel::onPosterReady(qint64 fileId, const QString& imagePath)
{
	m_posterPaths.insert(fileId, imagePath);
	m_posterVersions[fileId]++;
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId == fileId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx);
		}
	}
}

void McJobListModel::onFanartReady(qint64 fileId, const QString& fanartPath,
                                   const QImage& image)
{
	m_fanartPaths.insert(fileId, fanartPath);
	if (!image.isNull())
		McCardDelegate::prefetchFanart(fanartPath, QPixmap::fromImage(image));
	m_pendingFanartIds[fileId] = fanartPath;
	if (!m_fanartBatchTimer.isActive())
		m_fanartBatchTimer.start();
}

void McJobListModel::updateImdbId(qint64 fileId, const QString& imdbId)
{
	for (int i = 0; i < m_allEntries.size(); ++i) {
		if (m_allEntries[i].job.fileId == fileId) {
			m_allEntries[i].job.imdbId = imdbId;
			break;
		}
	}
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId == fileId) {
			m_entries[i].job.imdbId = imdbId;
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { ImdbIdRole });
			return;
		}
	}
}

void McJobListModel::updateProgress(qint64 jobId, int percent)
{
	m_progress.insert(jobId, percent);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId == jobId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { ProgressRole });
			return;
		}
	}
}

void McJobListModel::updateOutputSize(qint64 jobId, qint64 bytes)
{
	m_outputSize.insert(jobId, bytes);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId == jobId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { OutputSizeRole });
			return;
		}
	}
}

void McJobListModel::updatePhase(qint64 jobId, const QString& label)
{
	m_phaseLabel.insert(jobId, label);
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.jobId == jobId) {
			const QModelIndex idx = index(i);
			emit dataChanged(idx, idx, { PhaseLabelRole });
			return;
		}
	}
}

bool McJobListModel::statusMatchesFilter(const QString& status) const
{
	if (m_filterStatus.isEmpty()) return true;
	// "Running" filter intentionally includes queued jobs so the user can see
	// both what is processing now and what is lined up next.
	if (m_filterStatus == QLatin1String("running"))
		return status == QLatin1String("running") || status == QLatin1String("queued");
	return status == m_filterStatus;
}

void McJobListModel::applyFilter()
{
	using QF = McFilterPanel;

	beginResetModel();
	m_entries.clear();
	m_checkStates.clear();

	for (int i = 0; i < m_allEntries.size(); ++i) {
		const JobCardEntry& e = m_allEntries[i];

		// ── Text search ──────────────────────────────────────────────────────────
		if (!m_filterText.isEmpty()) {
			const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
			bool hit = e.job.filename.contains(m_filterText, cs)
			        || e.job.summary.contains(m_filterText, cs);
			if (!hit) {
				const QString folder = QFileInfo(e.job.filePath).dir().dirName();
				hit = folder.contains(m_filterText, cs);
			}
			if (!hit) {
				for (const StreamRecord& s : e.allStreams) {
					if (s.language.contains(m_filterText, cs)
					 || s.title.contains(m_filterText, cs)
					 || s.codecName.contains(m_filterText, cs)
					 || s.codecProfile.contains(m_filterText, cs)
					 || s.hdrFormat.contains(m_filterText, cs)) {
						hit = true;
						break;
					}
				}
			}
			if (!hit) continue;
		}

		// ── Status filter ────────────────────────────────────────────────────────
		if (!statusMatchesFilter(e.job.status))
			continue;

		// ── Quick filters ────────────────────────────────────────────────────────
		if (m_quickFilters != 0) {
			if (m_quickFilters & QF::QF_4K) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams)
					if (s.codecType == QLatin1String("video") && (s.width >= 3840 || s.height >= 2160))
						{ ok = true; break; }
				if (!ok) continue;
			}
			if (m_quickFilters & QF::QF_DV) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("video")) continue;
					if (s.hdrFormat == QLatin1String("DolbyVision")
					 || s.codecProfile.startsWith(QLatin1String("dvhe"), Qt::CaseInsensitive)
					 || s.codecProfile.startsWith(QLatin1String("dvav"), Qt::CaseInsensitive))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
			if (m_quickFilters & QF::QF_HDR) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("video")) continue;
					if (!s.hdrFormat.isEmpty() && s.hdrFormat != QLatin1String("DolbyVision"))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
			if (m_quickFilters & (QF::QF_Atmos | QF::QF_TrueHD | QF::QF_DtsHD | QF::QF_DtsX)) {
				bool ok = false;
				for (const StreamRecord& s : e.allStreams) {
					if (s.codecType != QLatin1String("audio")) continue;
					const QString cn = s.codecName.toLower();
					const QString cp = s.codecProfile.toLower();
					const QString ct = s.title.toLower();
					if ((m_quickFilters & QF::QF_Atmos) &&
					    ((cn == QLatin1String("truehd") || cn == QLatin1String("eac3")) &&
					     (cp.contains(QLatin1String("atmos")) || ct.contains(QLatin1String("atmos")))))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_TrueHD) && cn == QLatin1String("truehd"))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_DtsHD) && cn == QLatin1String("dts") &&
					    (cp.contains(QLatin1String("ma")) || ct.contains(QLatin1String("dts-hd")) || ct.contains(QLatin1String("dts:x"))))
						{ ok = true; break; }
					if ((m_quickFilters & QF::QF_DtsX) && cn == QLatin1String("dts") &&
					    (cp.contains(QLatin1String("dts:x")) || cp.contains(QLatin1String("dts-x")) ||
					     ct.contains(QLatin1String("dts:x")) || ct.contains(QLatin1String("dts-x"))))
						{ ok = true; break; }
				}
				if (!ok) continue;
			}
		}

		// ── Rating filter ────────────────────────────────────────────────────────
		if (m_ratingMin > 0.0 || m_ratingMax < 10.0) {
			const double r = e.job.voteAverage;
			if (r <= 0.0 || r < m_ratingMin || r > m_ratingMax) continue;
		}

		m_entries.append(e);
		m_checkStates.append(m_allCheckStates[i]);
	}
	endResetModel();
}

QList<qint64> McJobListModel::checkedJobIds() const
{
	QList<qint64> ids;
	for (int i = 0; i < m_entries.size(); ++i)
		if (m_checkStates[i] == Qt::Checked)
			ids << m_entries[i].job.jobId;
	return ids;
}

QList<qint64> McJobListModel::jobIdsByStatus(const QString& status) const
{
	QList<qint64> ids;
	for (const auto& e : m_entries)
		if (e.job.status == status)
			ids << e.job.jobId;
	return ids;
}

// ── QAbstractListModel interface ──────────────────────────────────────────────

int McJobListModel::rowCount(const QModelIndex& parent) const
{
	if (parent.isValid()) return 0;
	return m_entries.size();
}

QVariant McJobListModel::data(const QModelIndex& index, int role) const
{
	if (!index.isValid() || index.row() >= m_entries.size())
		return {};

	const JobCardEntry& e = m_entries.at(index.row());

	switch (role) {
	case Qt::DisplayRole:   return e.job.filename;
	case Qt::CheckStateRole: return m_checkStates.at(index.row());
	case JobIdRole:         return e.job.jobId;
	case FileIdRole:        return e.job.fileId;
	case StatusRole:        return e.job.status;
	case SavedRole:         return e.job.savedBytes;
	case AllStreamsRole:     return QVariant::fromValue(e.allStreams);
	case KeptStreamsRole:    return QVariant::fromValue(e.keptStreams);
	case FilenameRole:      return e.job.filename;
	case SummaryRole:       return e.job.summary;
	case ProgressRole:      return m_progress.value(e.job.jobId, 0);
	case OutputSizeRole:    return m_outputSize.value(e.job.jobId, 0);
	case PhaseLabelRole:    return m_phaseLabel.value(e.job.jobId);
	case PosterRole:        return m_posterPaths.value(e.job.fileId);
	case PosterVersionRole: return m_posterVersions.value(e.job.fileId, 0);
	case FanartRole:        return m_fanartPaths.value(e.job.fileId);
	case FileSizeRole:      return e.job.sizeBytes;
	case FilePathRole:      return e.job.filePath;
	case ImdbIdRole:        return e.job.imdbId;
	case DurationRole:      return e.job.durationSec;
	case RatingRole:           return e.job.voteAverage;
	case OriginalLanguageRole: return e.job.originalLanguage;
	case FlagChangesRole: return e.flagChangesJson;
	case JobTypeRole:     return e.job.jobType;
	default:                   return {};
	}
}

bool McJobListModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	if (!index.isValid() || index.row() >= m_entries.size())
		return false;

	if (role == Qt::CheckStateRole) {
		m_checkStates[index.row()] = static_cast<Qt::CheckState>(value.toInt());
		emit dataChanged(index, index, { Qt::CheckStateRole });
		return true;
	}
	return false;
}

Qt::ItemFlags McJobListModel::flags(const QModelIndex& index) const
{
	if (!index.isValid()) return Qt::NoItemFlags;
	Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	// Proposed and queued (not yet started) jobs are user-checkable
	const auto& st = m_entries.at(index.row()).job.status;
	if (st == "proposed" || st == "queued")
		f |= Qt::ItemIsUserCheckable;
	return f;
}

// ── Stream toggle ─────────────────────────────────────────────────────────────

void McJobListModel::toggleStream(const QModelIndex& index, int streamIndex)
{
	if (!index.isValid() || index.row() >= m_entries.size()) return;

	JobCardEntry& filt = m_entries[index.row()];
	if (filt.job.status != "proposed" && filt.job.status != "queued" && filt.job.status != "failed") return;

	const qint64 jobId = filt.job.jobId;

	// Toggle: remove from keptStreams if present, otherwise add it back
	bool wasKept = false;
	for (int i = 0; i < filt.keptStreams.size(); ++i) {
		if (filt.keptStreams[i].streamIndex == streamIndex) {
			filt.keptStreams.removeAt(i);
			wasKept = true;
			break;
		}
	}
	if (!wasKept) {
		for (const auto& s : filt.allStreams) {
			if (s.streamIndex == streamIndex) {
				filt.keptStreams.append(s);
				break;
			}
		}
	}

	// Commentary cascade: when the user removes the only kept commentary audio track
	// and there is exactly one kept commentary subtitle, also remove that subtitle.
	// Counts are taken from keptStreams (not allStreams) so non-understood tracks
	// already removed by the rule engine do not inflate the count or trigger a false
	// cascade. The just-removed audio is no longer in keptStreams at this point, so
	// we add 1 manually to correctly read "there was exactly 1 commentary audio kept".
	if (wasKept) {
		const StreamRecord* src = nullptr;
		for (const auto& s : filt.allStreams)
			if (s.streamIndex == streamIndex) { src = &s; break; }

		const auto isCommentaryTrack = [](const StreamRecord& s) {
			return s.trackType == QLatin1String("commentary") || s.isCommentary;
		};

		if (src && src->codecType == QLatin1String("audio") && isCommentaryTrack(*src)) {
			int commentaryAudioKept = 1; // +1 for the audio just removed above
			int commentarySubKept = 0, commentarySubIdx = -1;
			for (const auto& k : filt.keptStreams) {
				if (!isCommentaryTrack(k)) continue;
				if (k.codecType == QLatin1String("audio"))    ++commentaryAudioKept;
				if (k.codecType == QLatin1String("subtitle")) { ++commentarySubKept; commentarySubIdx = k.streamIndex; }
			}
			if (commentaryAudioKept == 1 && commentarySubKept == 1) {
				for (int i = 0; i < filt.keptStreams.size(); ++i) {
					if (filt.keptStreams[i].streamIndex == commentarySubIdx) {
						filt.keptStreams.removeAt(i);
						break;
					}
				}
			}
		}
	}

	// Sync the master list so filter re-apply preserves the change
	for (auto& ae : m_allEntries) {
		if (ae.job.jobId == jobId) {
			ae.keptStreams = filt.keptStreams;
			break;
		}
	}

	// Compute removed indices from current kept state (needed below for savings + downgrade check)
	QSet<int> removedIndices;
	for (const auto& s : filt.allStreams) {
		bool kept = false;
		for (const auto& k : filt.keptStreams)
			if (k.streamIndex == s.streamIndex) { kept = true; break; }
		if (!kept) removedIndices.insert(s.streamIndex);
	}

	// Regenerate commandArgsJson and persist
	const auto allJobs = DatabaseManager::instance().allJobs();
	QString existingArgs;
	for (const auto& j : allJobs)
		if (j.id == jobId) { existingArgs = j.commandArgsJson; break; }

	QString newArgs;
	const QJsonArray existingArr = QJsonDocument::fromJson(existingArgs.toUtf8()).array();
	if (existingArr.size() < 3 && !removedIndices.isEmpty()) {
		// tag_edit job (commandArgsJson="[]") being upgraded: build a fresh mkvmerge command.
		const QFileInfo fi(filt.job.filePath);
		const QString   ext   = fi.suffix().toLower();
		const bool      isMkv = (ext == QLatin1String("mkv") || ext == QLatin1String("mka") || ext == QLatin1String("mks"));
		const QString   outputPath = isMkv
			? filt.job.filePath + QLatin1String(".tmp")
			: fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QLatin1String(".mkv.tmp");

		bool hasAudio = false, hasSub = false;
		for (const auto& s : filt.allStreams) {
			if (s.isExternal) continue;
			if (s.codecType == QLatin1String("audio"))    hasAudio = true;
			if (s.codecType == QLatin1String("subtitle")) hasSub   = true;
		}
		QStringList keepAudio, keepSub;
		for (const auto& s : filt.keptStreams) {
			if (s.isExternal) continue;  // sidecar — not a container track
			if (s.codecType == QLatin1String("audio"))    keepAudio << QString::number(s.streamIndex);
			if (s.codecType == QLatin1String("subtitle")) keepSub   << QString::number(s.streamIndex);
		}
		QStringList argList = { "-o", outputPath };
		if (hasAudio) {
			if (!keepAudio.isEmpty()) argList << "--audio-tracks" << keepAudio.join(',');
			else                      argList << "--no-audio";
		}
		if (hasSub) {
			if (!keepSub.isEmpty())   argList << "--subtitle-tracks" << keepSub.join(',');
			else                      argList << "--no-subtitles";
		}
		argList << filt.job.filePath;
		QJsonArray arr;
		for (const QString& s : argList) arr << s;
		newArgs = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));

		DatabaseManager::instance().updateJobType(jobId, QStringLiteral("remux"));
		filt.job.jobType = QStringLiteral("remux");
		for (auto& ae : m_allEntries)
			if (ae.job.jobId == jobId) { ae.job.jobType = QStringLiteral("remux"); break; }
	} else {
		newArgs = rebuildCommandArgs(existingArgs, filt.allStreams, filt.keptStreams);
	}
	DatabaseManager::instance().updateJobCommandArgs(jobId, newArgs);

	// Recalculate and persist estimated savings based on current removals
	const qint64 newSavedBytes = Mc::estimateSavingBytes(
		filt.allStreams, removedIndices, filt.job.sizeBytes, filt.job.durationSec);
	filt.job.savedBytes = newSavedBytes;
	for (auto& ae : m_allEntries) {
		if (ae.job.jobId == jobId) { ae.job.savedBytes = newSavedBytes; break; }
	}
	DatabaseManager::instance().updateJobSavedBytes(jobId, newSavedBytes);

	// Also refresh the frozen per-track breakdown so the log dialog and calibration
	// reflect the tracks actually being removed, not the original proposal.
	// (JobDisplayRecord doesn't carry these fields — only the log dialog reads them,
	// via a fresh DatabaseManager::jobById() lookup — so no local mirroring needed.)
	const QString newEstimatesJson = Mc::buildStreamEstimatesJson(
		filt.allStreams, removedIndices, filt.job.sizeBytes, filt.job.durationSec);
	DatabaseManager::instance().updateJobEstimate(jobId, newSavedBytes, newEstimatesJson);

	// Rebuild sidecar deletions list: paths of external streams now in the remove set
	{
		QJsonArray sidecarPaths;
		for (const auto& s : filt.allStreams) {
			if (s.isExternal && removedIndices.contains(s.streamIndex) && !s.externalPath.isEmpty())
				sidecarPaths << s.externalPath;
		}
		const QString sidecarJson = sidecarPaths.isEmpty()
			? QString{}
			: QString::fromUtf8(QJsonDocument(sidecarPaths).toJson(QJsonDocument::Compact));
		DatabaseManager::instance().updateJobSidecarDeletions(jobId, sidecarJson);
	}

	// If no CONTAINER tracks remain removed, downgrade to tag_edit.
	// External-only removals don't need mkvmerge — sidecar_deletions_json handles them.
	bool containerTracksRemoved = false;
	for (const auto& s : filt.allStreams) {
		if (!s.isExternal && removedIndices.contains(s.streamIndex)) {
			containerTracksRemoved = true;
			break;
		}
	}
	if (!containerTracksRemoved && filt.job.jobType == QLatin1String("remux")) {
		DatabaseManager::instance().updateJobType(jobId, QStringLiteral("tag_edit"));
		DatabaseManager::instance().updateJobCommandArgs(jobId, QStringLiteral("[]"));
		filt.job.jobType = QStringLiteral("tag_edit");
		for (auto& ae : m_allEntries) {
			if (ae.job.jobId == jobId) { ae.job.jobType = QStringLiteral("tag_edit"); break; }
		}
	}

	// Regenerate and persist the human-readable summary from the current removal set.
	{
		int audioRemoved = 0, subRemoved = 0;
		for (const auto& s : filt.allStreams) {
			if (!removedIndices.contains(s.streamIndex)) continue;
			if (s.codecType == QLatin1String("audio"))    audioRemoved++;
			if (s.codecType == QLatin1String("subtitle")) subRemoved++;
		}
		QStringList parts;
		if (audioRemoved > 0) parts << QStringLiteral("%1 audio").arg(audioRemoved);
		if (subRemoved   > 0) parts << QStringLiteral("%1 subtitle").arg(subRemoved);
		const QString newSummary = parts.isEmpty()
			? QStringLiteral("Edit track flags")
			: QStringLiteral("Remove ") + parts.join(QStringLiteral(", "));
		filt.job.summary = newSummary;
		for (auto& ae : m_allEntries)
			if (ae.job.jobId == jobId) { ae.job.summary = newSummary; break; }
		DatabaseManager::instance().updateJobSummary(jobId, newSummary);
	}

	emit dataChanged(index, index, { KeptStreamsRole, SavedRole, SummaryRole });
}

// ── Stream flag toggle ────────────────────────────────────────────────────────

void McJobListModel::setStreamFlag(const QModelIndex& index, int streamIndex,
                                    const QString& flag, bool value)
{
	if (!index.isValid() || index.row() >= m_entries.size()) return;

	JobCardEntry& filt = m_entries[index.row()];
	if (filt.job.status != "proposed" && filt.job.status != "queued") return;

	const qint64 jobId = filt.job.jobId;

	// Update the in-memory stream record so badges reflect the desired state immediately
	auto applyFlag = [&](StreamRecord& s) {
		if (s.streamIndex != streamIndex) return;
		if (flag == QLatin1String("default"))      s.isDefault  = value;
		else if (flag == QLatin1String("forced"))   s.isForced   = value;
		else if (flag == QLatin1String("original")) s.isOriginal = value;
	};
	for (auto& s : filt.allStreams)   applyFlag(s);
	for (auto& s : filt.keptStreams)  applyFlag(s);
	for (auto& ae : m_allEntries) {
		if (ae.job.jobId != jobId) continue;
		for (auto& s : ae.allStreams)  applyFlag(s);
		for (auto& s : ae.keptStreams) applyFlag(s);
		break;
	}

	// Load current flag_changes_json, add/update the entry for this stream+flag
	const auto jobOpt = DatabaseManager::instance().jobById(jobId);
	QJsonArray changes = jobOpt
		? QJsonDocument::fromJson(jobOpt->flagChangesJson.toUtf8()).array()
		: QJsonArray{};

	// Replace existing entry for this (streamIndex, flag) pair or append a new one
	bool found = false;
	for (int i = 0; i < changes.size(); ++i) {
		QJsonObject o = changes[i].toObject();
		if (o["streamIndex"].toInt() == streamIndex && o["flag"].toString() == flag) {
			o["value"] = value;
			changes[i] = o;
			found = true;
			break;
		}
	}
	if (!found) {
		QJsonObject o;
		o["streamIndex"] = streamIndex;
		o["flag"]        = flag;
		o["value"]       = value;
		changes.append(o);
	}

	const QString newJson = QString::fromUtf8(
		QJsonDocument(changes).toJson(QJsonDocument::Compact));
	filt.flagChangesJson = newJson;
	for (auto& ae : m_allEntries)
		if (ae.job.jobId == jobId) { ae.flagChangesJson = newJson; break; }
	DatabaseManager::instance().updateJobFlagChanges(jobId, newJson);

	emit dataChanged(index, index, { AllStreamsRole, KeptStreamsRole, FlagChangesRole });
}

void McJobListModel::setStreamLanguage(const QModelIndex& index, int streamIndex,
                                        const QString& langCode)
{
	if (!index.isValid() || index.row() >= m_entries.size()) return;

	JobCardEntry& filt = m_entries[index.row()];
	if (filt.job.status != "proposed" && filt.job.status != "queued") return;

	const qint64 jobId = filt.job.jobId;

	// Update the in-memory stream record so the badge reflects the new language immediately.
	auto applyLang = [&](StreamRecord& s) {
		if (s.streamIndex == streamIndex) s.language = langCode;
	};
	for (auto& s : filt.allStreams)   applyLang(s);
	for (auto& s : filt.keptStreams)  applyLang(s);
	for (auto& ae : m_allEntries) {
		if (ae.job.jobId != jobId) continue;
		for (auto& s : ae.allStreams)  applyLang(s);
		for (auto& s : ae.keptStreams) applyLang(s);
		break;
	}

	// Load current flag_changes_json, add/update the language entry for this stream
	const auto jobOpt = DatabaseManager::instance().jobById(jobId);
	QJsonArray changes = jobOpt
		? QJsonDocument::fromJson(jobOpt->flagChangesJson.toUtf8()).array()
		: QJsonArray{};

	bool found = false;
	for (int i = 0; i < changes.size(); ++i) {
		QJsonObject o = changes[i].toObject();
		if (o["streamIndex"].toInt() == streamIndex
		        && o["flag"].toString() == QLatin1String("language")) {
			o["value"] = langCode;
			changes[i] = o;
			found = true;
			break;
		}
	}
	if (!found) {
		QJsonObject o;
		o["streamIndex"] = streamIndex;
		o["flag"]        = QStringLiteral("language");
		o["value"]       = langCode;
		changes.append(o);
	}

	const QString newJson = QString::fromUtf8(
		QJsonDocument(changes).toJson(QJsonDocument::Compact));
	filt.flagChangesJson = newJson;
	for (auto& ae : m_allEntries)
		if (ae.job.jobId == jobId) { ae.flagChangesJson = newJson; break; }
	DatabaseManager::instance().updateJobFlagChanges(jobId, newJson);

	emit dataChanged(index, index, { AllStreamsRole, KeptStreamsRole, FlagChangesRole });
}

void McJobListModel::updateExternalStreamInfo(qint64 fileId, int streamIndex,
                                               const QString& language, const QString& externalPath)
{
	auto apply = [&](StreamRecord& s) {
		if (s.streamIndex != streamIndex) return;
		s.language     = language;
		s.externalPath = externalPath;
	};
	for (auto& ae : m_allEntries) {
		if (ae.job.fileId != fileId) continue;
		for (auto& s : ae.allStreams)  apply(s);
		for (auto& s : ae.keptStreams) apply(s);
	}
	for (int i = 0; i < m_entries.size(); ++i) {
		if (m_entries[i].job.fileId != fileId) continue;
		for (auto& s : m_entries[i].allStreams)  apply(s);
		for (auto& s : m_entries[i].keptStreams) apply(s);
		const QModelIndex idx = index(i);
		emit dataChanged(idx, idx, { AllStreamsRole, KeptStreamsRole });
	}
}

QString McJobListModel::rebuildCommandArgs(const QString& existingJson,
                                            const QList<StreamRecord>& all,
                                            const QList<StreamRecord>& kept)
{
	const QJsonArray arr = QJsonDocument::fromJson(existingJson.toUtf8()).array();
	if (arr.size() < 3) return existingJson;

	// Structure from ActionEngine::buildCommand:
	//   ["-o", outputPath, <track-selection flags...>, inputPath]
	const QString outputPath = arr[1].toString();
	const QString inputPath  = arr.last().toString();

	bool hasAudio = false, hasSub = false;
	for (const auto& s : all) {
		if (s.isExternal) continue;
		if (s.codecType == "audio")    hasAudio = true;
		if (s.codecType == "subtitle") hasSub   = true;
	}

	QStringList keepAudio, keepSub;
	for (const auto& s : kept) {
		if (s.isExternal) continue;  // sidecar — not a container track
		if (s.codecType == "audio")    keepAudio << QString::number(s.streamIndex);
		if (s.codecType == "subtitle") keepSub   << QString::number(s.streamIndex);
	}

	// Preserve video flags unchanged — toggleStream only applies to audio/subtitle.
	QStringList newArgs = { "-o", outputPath };
	for (int i = 0; i < arr.size(); ++i) {
		const QString a = arr[i].toString();
		if (a == QStringLiteral("--video-tracks") && i + 1 < arr.size()) {
			newArgs << a << arr[i + 1].toString();
			++i;
		} else if (a == QStringLiteral("--no-video")) {
			newArgs << a;
		}
	}
	if (hasAudio) {
		if (!keepAudio.isEmpty())
			newArgs << "--audio-tracks" << keepAudio.join(',');
		else
			newArgs << "--no-audio";
	}
	if (hasSub) {
		if (!keepSub.isEmpty())
			newArgs << "--subtitle-tracks" << keepSub.join(',');
		else
			newArgs << "--no-subtitles";
	}
	newArgs << inputPath;

	QJsonArray result;
	for (const QString& s : newArgs) result << s;
	return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

// ── Static helper ─────────────────────────────────────────────────────────────

QList<StreamRecord> McJobListModel::streamsFromJson(const QString& json)
{
	return ActionEngine::deserializeStreamSnapshot(json);
}

QList<StreamRecord> McJobListModel::computeKeptStreams(
	const QList<StreamRecord>& all,
	const QString& commandArgsJson)
{
	return ActionEngine::computeKeptStreams(all, commandArgsJson);
}

} // namespace Mc
