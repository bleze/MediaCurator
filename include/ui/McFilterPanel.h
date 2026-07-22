#pragma once
#include <QList>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QTimer;

namespace Mc {

class McStorageGroupChipToggle;

class McFilterPanel : public QWidget {
	Q_OBJECT
public:
	explicit McFilterPanel(QWidget* parent = nullptr);

	QComboBox* statusCombo() const { return m_statusCombo; }
	QComboBox* sortCombo()   const { return m_sortCombo;   }

	// Rebuilds the storage-group chip row from the current folder→group assignment.
	// Call after folders are reassigned (e.g. Manage Folders dialog closes) so the
	// chip set stays live without requiring an app restart. The row renders no chips
	// (and takes no space) when a single storage group is in use.
	void refreshStorageGroups();

	// Hide Movies/TV/Docs/Misc pills when the library has no classified entries
	// (everything is still "unknown"). Active category filters are cleared on hide.
	void setMediaCategoryFiltersVisible(bool visible);

	enum QuickFilter : quint32 {
		QF_None         = 0,
		QF_4K           = 1 << 0,
		QF_DV           = 1 << 1,
		QF_HDR          = 1 << 2,
		QF_Atmos        = 1 << 3,
		QF_TrueHD       = 1 << 4,
		QF_DtsHD        = 1 << 5,
		QF_DtsX         = 1 << 6,
		// Media categories (OR within group when any selected)
		QF_Movie        = 1 << 7,
		QF_Tv           = 1 << 8,
		QF_Documentary  = 1 << 9,
		QF_Misc         = 1 << 10,  // misc + unknown/unmatched
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

signals:
	void filterTextChanged(const QString& text);
	void filterStatusChanged(int statusIndex);   // 0=all, 1=proposed, 2=missing-poster
	void quickFiltersChanged(quint32 flags);
	void sortOrderChanged(int order);
	void ratingFilterChanged(double minRating, double maxRating);
	void storageGroupFilterChanged(quint32 groupMask);   // bit (1<<group); 0 = show all

private:
	void onPillToggled(quint32 flag, bool on);
	void emitStorageGroupFilter();

	void updateRatingLabel();
	void emitRatingFilter();

	quint32    m_activeFilters  = QF_None;
	QTimer*    m_searchTimer    = nullptr;
	QTimer*    m_ratingTimer    = nullptr;
	QLineEdit* m_search         = nullptr;
	QComboBox* m_statusCombo    = nullptr;
	QComboBox* m_sortCombo      = nullptr;
	QWidget*   m_ratingSlider   = nullptr;  // RangeSlider (forward-declared as QWidget)
	QLabel*    m_ratingLabel    = nullptr;

	// Movies/TV/Docs/Misc pills + their leading separator — hidden until the
	// library has at least one non-unknown media_type.
	QWidget*      m_mediaCategoryContainer = nullptr;

	// Storage-group chip row — lives in its own container so refreshStorageGroups()
	// can rebuild it independently of the rest of the filter bar's layout.
	QWidget*      m_storageGroupContainer = nullptr;
	QHBoxLayout*  m_storageGroupLayout    = nullptr;
	QList<McStorageGroupChipToggle*> m_storageGroupChips;
};

} // namespace Mc
