#pragma once
#include "core/DatabaseManager.h"
#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

namespace Mc {

struct FileEntry {
	FileRecord          file;
	QList<StreamRecord> streams;
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
	};

	// Must stay in sync with McFilterPanel::QuickFilter
	enum QuickFilter : quint32 {
		QF_None   = 0,
		QF_4K     = 1 << 0,
		QF_DV     = 1 << 1,
		QF_HDR    = 1 << 2,
		QF_Atmos  = 1 << 3,
		QF_TrueHD = 1 << 4,
		QF_DtsHD  = 1 << 5,
		QF_DtsX   = 1 << 6,
	};

	enum SortOrder {
		SortByName    = 0,
		SortByNewest  = 1,
		SortByOldest  = 2,
		SortByLargest = 3,
	};

	explicit McFileListModel(QObject* parent = nullptr);

	int      rowCount(const QModelIndex& parent = {}) const override;
	QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

	void reload();
	void reloadFile(qint64 fileId);
	void applyFileUpdate(const Mc::FileRecord& file, const QList<Mc::StreamRecord>& streams);
	void removeEntry(qint64 fileId);
	void refreshJobFilter();        // re-query proposed jobs and reapply filter
	int  fileCount() const { return m_entries.size(); }
	int  totalCount() const { return m_allEntries.size(); }

	/** Returns the set of stream indices the user has force-marked for removal. */
	QSet<int> forcedRemovalsFor(qint64 fileId) const { return m_forcedRemovals.value(fileId); }

public slots:
	void setFilterText(const QString& text);
	void setFilterHasRemovals(bool on);
	void setFilterMissingImdb(bool on);
	void setQuickFilters(quint32 flags);
	void setSortOrder(int order);
	void onPosterReady(qint64 fileId, const QString& imagePath);
	void onImdbIdSaved(qint64 fileId, const QString& imdbId);
	void toggleForcedRemoval(qint64 fileId, int streamIndex);

private:
	bool entryPassesFilter(const FileEntry& e) const;
	void applyFilter();
	void applyEntry(const FileEntry& entry);

	QList<FileEntry>          m_allEntries;      // full unfiltered set
	QList<FileEntry>          m_entries;         // visible (filtered) set
	QSet<qint64>              m_filesWithJobs;
	QHash<qint64, QString>    m_posterPaths;     // fileId → cached image path
	QHash<qint64, int>        m_posterVersions;  // fileId → version counter (increments on update)
	QHash<qint64, QString>    m_imdbIds;         // fileId → IMDb ID
	QHash<qint64, QSet<int>>  m_forcedRemovals;  // fileId → stream indices user wants removed
	QString                   m_filterText;
	bool                      m_filterHasRemovals  = false;
	bool                      m_filterMissingImdb  = false;
	quint32                   m_quickFilters       = QF_None;
	int                       m_sortOrder          = SortByName;
};

} // namespace Mc
