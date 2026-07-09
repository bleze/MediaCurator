#pragma once
#include "core/DatabaseManager.h"
#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>
#include <QTimer>

namespace Mc {

struct JobCardEntry {
	JobDisplayRecord    job;
	QList<StreamRecord> allStreams;   // every stream for the file
	QList<StreamRecord> keptStreams;  // streams that survive the job
	QString             flagChangesJson; // cached flag_changes_json; updated by setStreamFlag
};

class McJobListModel : public QAbstractListModel
{
	Q_OBJECT
public:
	enum Roles {
		JobIdRole      = Qt::UserRole + 1,
		FileIdRole     = Qt::UserRole + 2,
		StatusRole     = Qt::UserRole + 3,
		SavedRole      = Qt::UserRole + 4,
		AllStreamsRole  = Qt::UserRole + 5,
		KeptStreamsRole = Qt::UserRole + 6,
		FilenameRole   = Qt::UserRole + 7,
		SummaryRole    = Qt::UserRole + 8,
		ProgressRole   = Qt::UserRole + 9,   // int 0-100, only meaningful when status=="running"
		PosterRole     = Qt::UserRole + 10,  // QString local image path, empty if none
		PosterVersionRole = Qt::UserRole + 21,  // int — increments each time the poster at PosterRole's path changes
		FileSizeRole   = Qt::UserRole + 11,  // qint64 original file size in bytes
		FilePathRole   = Qt::UserRole + 12,  // QString absolute file path
		ImdbIdRole     = Qt::UserRole + 13,  // QString IMDb tt-id, empty if not linked
		DurationRole   = Qt::UserRole + 14,  // double duration in seconds
		RatingRole            = Qt::UserRole + 15,  // double TMDB vote_average, 0 if unknown
		OriginalLanguageRole  = Qt::UserRole + 16,  // QString ISO 639-2 original audio language
		FanartRole            = Qt::UserRole + 17,  // QString absolute path to w780 backdrop image
		FlagChangesRole       = Qt::UserRole + 18,  // QString flag_changes_json for this job
		JobTypeRole           = Qt::UserRole + 19,  // QString "remux" | "tag_edit"
		OutputSizeRole        = Qt::UserRole + 20,  // qint64 live .tmp output size, only meaningful when status=="running"
		PhaseLabelRole        = Qt::UserRole + 22,  // QString sub-phase label (e.g. "Copying to NAS"), empty = default "Running"
		ContainerTitleRole    = Qt::UserRole + 23,  // QString title from ffprobe format tags; may be absent or junk
		DisplayTitleRole      = Qt::UserRole + 24,  // QString TMDB/user-assigned override; preferred over all others
		DisplayYearRole       = Qt::UserRole + 25,  // int release year from TMDB (0 = unknown)
	};

	explicit McJobListModel(QObject* parent = nullptr);

	int      rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	bool     setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	void reload();
	void reloadPaged(int limit);
	void removeJobIds(const QList<qint64>& ids);

	/** Update status + savedBytes for a single job without full reload. */
	void updateJob(qint64 jobId, const QString& status, qint64 savedBytes = -1);

	int jobCount()  const { return m_entries.size(); }
	int totalCount() const { return m_allEntries.size(); }

	/** Count of jobs per status string across the full (unfiltered) list. */
	QHash<QString, int> countsByStatus() const;

	/** IDs of jobs whose check state is Qt::Checked. */
	QList<qint64> checkedJobIds() const;
	/** IDs of all jobs with a given status. */
	QList<qint64> jobIdsByStatus(const QString& status) const;

	// Toggle a stream's inclusion in a Proposed or Queued job's kept-tracks list.
	// No-op if the job is Running, Done, Failed, or Cancelled. Persists the new command args to DB.
	void toggleStream(const QModelIndex& index, int streamIndex);

	// Set or clear a disposition flag on a specific stream of a Proposed or Queued job.
	// Updates the in-memory stream record and persists flag_changes_json to DB.
	void setStreamFlag(const QModelIndex& index, int streamIndex,
	                   const QString& flag, bool value);

	// Sets the language of an embedded (non-external) stream on a Proposed or Queued job.
	// Updates the in-memory stream record and persists flag_changes_json to DB — applied
	// by mkvpropedit/mkvmerge when the job actually runs.
	void setStreamLanguage(const QModelIndex& index, int streamIndex, const QString& langCode);

	// Updates the in-memory language/path of an external sidecar stream after it was
	// renamed on disk and the corresponding DB row already updated directly (see
	// DatabaseManager::updateStreamExternalInfo). No flag_changes_json entry is written —
	// the change is already fully applied, not pending. Looked up by fileId since the
	// caller (e.g. the Library view) may not hold a QModelIndex into this model.
	void updateExternalStreamInfo(qint64 fileId, int streamIndex,
	                               const QString& language, const QString& externalPath);

	static QList<StreamRecord> computeKeptStreams(
	    const QList<StreamRecord>& all,
	    const QString& commandArgsJson);

	QString filterStatus() const { return m_filterStatus; }

public slots:
	void setFilterText(const QString& text);
	void setSortMode(JobSortMode sortMode);
	void setFilterStatus(const QString& status);   // empty string = show all
	void setQuickFilters(quint32 flags);           // McFilterPanel::QF_* bitmask
	void setRatingFilter(double minRating, double maxRating);
	void setRatingForFile(qint64 fileId, double rating);
	void updateProgress(qint64 jobId, int percent);
	void updateOutputSize(qint64 jobId, qint64 bytes);
	void updatePhase(qint64 jobId, const QString& label);
	void onPosterReady(qint64 fileId, const QString& imagePath);
	void onFanartReady(qint64 fileId, const QString& fanartPath, const QImage& image);
	void updateImdbId(qint64 fileId, const QString& imdbId);

private:
	[[nodiscard]] bool ensureJobInMasterList(qint64 jobId);
	[[nodiscard]] JobCardEntry buildCardEntry(const JobDisplayRecord& djr,
	                                          const QHash<qint64, QList<StreamRecord>>& streamsMap) const;
	void applyFilter();
	bool statusMatchesFilter(const QString& status) const;
	static QList<StreamRecord> streamsFromJson(const QString& json);
	static QString rebuildCommandArgs(const QString& existingJson,
	                                   const QList<StreamRecord>& all,
	                                   const QList<StreamRecord>& kept);

	QList<JobCardEntry>    m_allEntries;
	QList<Qt::CheckState>  m_allCheckStates;
	QList<JobCardEntry>    m_entries;
	QList<Qt::CheckState>  m_checkStates;
	QHash<qint64, int>     m_progress;       // jobId → 0-100
	QHash<qint64, qint64>  m_outputSize;     // jobId → live .tmp output size in bytes
	QHash<qint64, QString> m_phaseLabel;     // jobId → sub-phase label, empty = default "Running"
	QHash<qint64, QString> m_posterPaths;    // fileId → local poster image path
	QHash<qint64, int>     m_posterVersions; // fileId → version counter (increments on update)
	QHash<qint64, QString> m_fanartPaths;    // fileId → local fanart (backdrop) path
	QHash<qint64, QString> m_pendingFanartIds; // fileId → path, flushed by m_fanartBatchTimer
	QTimer                 m_fanartBatchTimer;
	QString                m_filterText;
	QString                m_filterStatus;
	quint32                m_quickFilters = 0;
	double                 m_ratingMin    = 0.0;
	double                 m_ratingMax    = 10.0;
	JobSortMode            m_sortMode     = JobSortMode::SmallestFirst;
};

} // namespace Mc
