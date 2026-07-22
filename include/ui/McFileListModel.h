#pragma once
#include "core/DatabaseManager.h"
#include "core/StorageGroupSettings.h"
#include <QAbstractListModel>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>

namespace Mc {

struct FileEntry {
	FileRecord          file;
	QList<StreamRecord> streams;

	// Precomputed for fast filtering and display (avoids repeated QFileInfo + stream scans on applyFilter)
	QString dirName;      // leaf folder name for text search + display
	QString parentPath;   // absolute parent dir for folder counts / grouping
	QString searchText;   // lowercased composite of filename + dir + container + stream fields for fast .contains()

	// Precomputed quick filter flags (set once at entry creation / update)
	bool has4K    = false;
	bool hasDV    = false;
	bool hasHDR   = false;
	bool hasAtmos = false;
	bool hasTrueHD = false;
	bool hasDtsHD = false;
	bool hasDtsX  = false;

	// Pre-grouped streams (populated once) to avoid repeated type-grouping in delegate sizeHint/paint
	QList<StreamRecord> videoStreams;
	QList<StreamRecord> audioStreams;
	QList<StreamRecord> subtitleStreams;

	int storageGroup = StorageGroupSettings::DefaultGroup;  // 1-4, from StorageGroupSettings::groupForFilePath(file.path)
};

class McFileListModel : public QAbstractListModel
{
	Q_OBJECT
public:
	enum Roles {
		FileRole          = Qt::UserRole + 1,
		StreamsRole       = Qt::UserRole + 2,
		PosterRole        = Qt::UserRole + 3,   // QString — absolute path to cached poster image
		OverridesRole     = Qt::UserRole + 4,   // QSet<int> — stream indices forced to Remove by user
		PosterVersionRole = Qt::UserRole + 5,   // int — increments each time a poster changes
		ImdbRole          = Qt::UserRole + 6,   // QString — IMDb ID (e.g. "tt1234567") or empty
		RatingRole        = Qt::UserRole + 7,   // double — TMDB vote_average (0.0 if unknown)
		DisplayTitleRole  = Qt::UserRole + 8,   // QString — TMDB/user-assigned title override
		ContainerTitleRole= Qt::UserRole + 9,   // QString — title from ffprobe format tags
		FolderCountRole   = Qt::UserRole + 10,  // int — number of files sharing same parent folder
		DisplayYearRole   = Qt::UserRole + 11,  // int — release year from TMDB (0 = unknown)
		FanartRole        = Qt::UserRole + 12,  // QString — absolute path to w780 backdrop image
		VideoStreamsRole   = Qt::UserRole + 13,
		AudioStreamsRole   = Qt::UserRole + 14,
		SubtitleStreamsRole= Qt::UserRole + 15,
		FileIdRole         = Qt::UserRole + 16,  // qint64 for fast id lookup (avoids full FileRecord copy in sizeHint)
		StorageGroupRole   = Qt::UserRole + 17,  // int 1-4 — see StorageGroupSettings
		TmdbRole           = Qt::UserRole + 18,  // int — TMDB movie/tv numeric id, 0 if unknown
	};

	// Must stay in sync with McFilterPanel::QuickFilter
	enum QuickFilter : quint32 {
		QF_None         = 0,
		QF_4K           = 1 << 0,
		QF_DV           = 1 << 1,
		QF_HDR          = 1 << 2,
		QF_Atmos        = 1 << 3,
		QF_TrueHD       = 1 << 4,
		QF_DtsHD        = 1 << 5,
		QF_DtsX         = 1 << 6,
		QF_Movie        = 1 << 7,
		QF_Tv           = 1 << 8,
		QF_Documentary  = 1 << 9,
		QF_Misc         = 1 << 10,
	};

	enum SortOrder {
		SortByName       = 0,
		SortByNewest     = 1,
		SortByOldest     = 2,
		SortByLargest    = 3,
		SortByRatingHigh = 4,
		SortByRatingLow  = 5,
		SortByLastScanned= 6,
	};

	explicit McFileListModel(QObject* parent = nullptr);

	int      rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

	void reload();
	void reloadFile(qint64 fileId);
	void recomputeFolderCounts();
	void initMeta(const QHash<qint64, QString>& posterPaths,
	              const QHash<qint64, QString>& imdbIds,
	              const QSet<qint64>& filesWithJobs,
	              const QHash<qint64, double>& ratings = {},
	              const QHash<qint64, QString>& fanartPaths = {},
	              const QHash<qint64, int>& tmdbIds = {});
	void applyFileUpdate(const Mc::FileRecord& file, const QList<Mc::StreamRecord>& streams);
	void removeEntry(qint64 fileId);
	void refreshJobFilter();        // re-query proposed jobs and reapply filter
	int  fileCount() const { return m_entries.size(); }
	int  totalCount() const { return m_allEntries.size(); }
	int  sortOrder() const { return m_sortOrder; }

	// True when at least one library file has a non-unknown media_type — used to
	// show/hide the Movies/TV/Docs/Misc filter pills.
	[[nodiscard]] bool hasClassifiedMediaTypes() const;

	/** Returns the set of stream indices the user has force-marked for removal. */
	QSet<int> forcedRemovalsFor(qint64 fileId) const { return m_forcedRemovals.value(fileId); }

signals:
	// Fired when the library goes from "no classified types" ↔ "has at least one".
	// Filter bars use this to show/hide the Movies/TV/Docs/Misc pills.
	void mediaCategoriesAvailabilityChanged(bool hasClassified);

public slots:
	void setFilterText(const QString& text);
	void setFilterHasRemovals(bool on);
	void setFilterMissingImdb(bool on);
	void setFilterIgnoredOnly(bool on);   // true = show only ignored; false = hide ignored
	void setStatusFilter(int statusIndex); // 0=all, 1=proposed, 2=missing-poster, 3=ignored
	void setIgnoredBatch(const QList<qint64>& fileIds, bool ignored); // update flag in-place + refilter
	void setQuickFilters(quint32 flags);
	void setStorageGroupFilter(quint32 groupMask);   // bit (1<<group) per StorageGroupSettings group; 0 = show all
	void setSortOrder(int order);
	void setRatingFilter(double minRating, double maxRating);
	void setRatingForFile(qint64 fileId, double rating);
	void setDisplayTitleForFile(qint64 fileId, const QString& title, int year);
	void setMediaTypeForFile(qint64 fileId, const QString& mediaType);
	void setMediaTypeBatch(const QList<qint64>& fileIds, const QString& mediaType);
	void onPosterReady(qint64 fileId, const QString& imagePath);
	void onFanartReady(qint64 fileId, const QString& fanartPath, const QImage& image);
	void onImdbIdSaved(qint64 fileId, const QString& imdbId);
	void onTmdbIdSaved(qint64 fileId, int tmdbId);
	void onTmdbDataReady(qint64 fileId, const QString& title, int year, double rating,
	                     const QString& mediaType = {});
	void toggleForcedRemoval(qint64 fileId, int streamIndex);

private:
	bool entryPassesFilter(const FileEntry& e) const;
	bool entryLessThan(const FileEntry& a, const FileEntry& b) const;
	// forceFullReset: tear down and rebuild the view (sort-order changes). Filter-only
	// updates use an incremental insert/remove diff so scroll position and size
	// caches stay intact — same approach as McJobListModel.
	void applyFilter(bool forceFullReset = false);
	void sortAllEntries();
	void applyEntry(const FileEntry& entry);
	void computeDerived(FileEntry& e);

	QList<FileEntry>          m_allEntries;      // full unfiltered set
	QList<FileEntry>          m_entries;         // visible (filtered) set
	QSet<qint64>              m_filesWithJobs;
	QHash<qint64, QString>    m_posterPaths;     // fileId → cached poster image path
	QHash<qint64, QString>    m_fanartPaths;     // fileId → cached fanart (backdrop) path
	QHash<qint64, QString>    m_pendingFanartIds; // fileId → path, flushed by m_fanartBatchTimer
	QTimer                    m_fanartBatchTimer;
	QHash<qint64, int>        m_posterVersions;  // fileId → version counter (increments on update)
	QHash<qint64, QString>    m_imdbIds;         // fileId → IMDb ID
	QHash<qint64, int>        m_tmdbIds;         // fileId → TMDB numeric id
	QHash<qint64, double>     m_ratings;         // fileId → TMDB vote_average (absent = no rating)
	QHash<qint64, int>        m_folderCounts;    // fileId → count of files sharing the same parent folder
	QHash<qint64, QSet<int>>  m_forcedRemovals;  // fileId → stream indices user wants removed
	QString                   m_filterText;
	bool                      m_filterHasRemovals  = false;
	bool                      m_filterMissingImdb  = false;
	bool                      m_filterIgnoredOnly  = false;
	quint32                   m_quickFilters       = QF_None;
	quint32                   m_storageGroupMask   = 0;   // 0 = no storage-group filtering
	int                       m_sortOrder          = SortByName;
	double                    m_ratingMin          = 0.0;
	double                    m_ratingMax          = 10.0;
	bool                      m_hadClassifiedMedia = false;

	void notifyMediaCategoriesAvailability();
};

} // namespace Mc
