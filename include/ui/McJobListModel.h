#pragma once
#include "core/DatabaseManager.h"
#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>

namespace Mc {

struct JobCardEntry {
	JobDisplayRecord    job;
	QList<StreamRecord> allStreams;   // every stream for the file
	QList<StreamRecord> keptStreams;  // streams that survive the job
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
		FileSizeRole   = Qt::UserRole + 11,  // qint64 original file size in bytes
		FilePathRole   = Qt::UserRole + 12,  // QString absolute file path
		ImdbIdRole     = Qt::UserRole + 13,  // QString IMDb tt-id, empty if not linked
		DurationRole   = Qt::UserRole + 14,  // double duration in seconds
		RatingRole            = Qt::UserRole + 15,  // double TMDB vote_average, 0 if unknown
		OriginalLanguageRole  = Qt::UserRole + 16,  // QString ISO 639-2 original audio language
	};

	explicit McJobListModel(QObject* parent = nullptr);

	int      rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
	bool     setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	void reload();
	void reloadPaged(int limit);

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

	// Toggle a stream's inclusion in a Proposed job's kept-tracks list.
	// No-op if the job is not Proposed. Persists the new command args to DB.
	void toggleStream(const QModelIndex& index, int streamIndex);

	static QList<StreamRecord> computeKeptStreams(
	    const QList<StreamRecord>& all,
	    const QString& commandArgsJson);

public slots:
	void setFilterText(const QString& text);
	void setSortMode(JobSortMode sortMode);
	void setFilterStatus(const QString& status);   // empty string = show all
	void setQuickFilters(quint32 flags);           // McFilterPanel::QF_* bitmask
	void setRatingFilter(double minRating, double maxRating);
	void setRatingForFile(qint64 fileId, double rating);
	void updateProgress(qint64 jobId, int percent);
	void onPosterReady(qint64 fileId, const QString& imagePath);
	void updateImdbId(qint64 fileId, const QString& imdbId);

private:
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
	QHash<qint64, QString> m_posterPaths;    // fileId → local image path
	QString                m_filterText;
	QString                m_filterStatus;
	quint32                m_quickFilters = 0;
	double                 m_ratingMin    = 0.0;
	double                 m_ratingMax    = 10.0;
	JobSortMode            m_sortMode     = JobSortMode::SmallestFirst;
};

} // namespace Mc
