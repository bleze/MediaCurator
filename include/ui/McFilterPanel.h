#pragma once
#include <QList>
#include <QSet>
#include <QString>
#include <QWidget>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;

namespace Mc {

class McStorageGroupChipToggle;
class McMultiCheckDropdown;

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

	// Repopulates the edition checklist from every distinct edition currently
	// in the library (DatabaseManager::distinctEditions()). Call after a scan,
	// analyze, or the edition backfill worker finishes — any of those can
	// discover editions that didn't exist in the list before. Previously
	// checked editions stay checked if they're still present.
	void refreshEditions();

	// Hide Movies/TV/Docs/Misc pills when the library has no classified entries
	// (everything is still "unknown"). Active category filters are cleared on hide.
	void setMediaCategoryFiltersVisible(bool visible);

	// Updates the redundant-versions chip's count label. The chip itself is only
	// shown while GroupedByEdition is the active sort mode.
	void setRedundantGroupCount(int count);

	enum QuickFilter : quint32 {
		QF_None         = 0,
		QF_4K           = 1 << 0,
		// 1<<1 and 1<<2 formerly QF_DV/QF_HDR — replaced by the HDR/DV checklist
		// dropdown (see hdrDvFilterChanged), same treatment as Edition below.
		// 1<<3 .. 1<<6 formerly QF_Atmos/QF_TrueHD/QF_DtsHD/QF_DtsX — replaced by
		// the Audio checklist dropdown (see audioFormatFilterChanged).
		// Media categories (OR within group when any selected)
		QF_Movie        = 1 << 7,
		QF_Tv           = 1 << 8,
		QF_Documentary  = 1 << 9,
		QF_Misc         = 1 << 10,  // misc + unknown/unmatched
		QF_3D           = 1 << 11,  // FileRecord::edition == "3D" (see EditionDetector)
	};

	enum SortOrder {
		SortByName       = 0,
		SortByNewest     = 1,
		SortByOldest     = 2,
		SortByLargest    = 3,
		SortByRatingHigh = 4,
		SortByRatingLow  = 5,
		SortByLastScanned= 6,
		GroupedByEdition = 7,   // "Group by Movie" — mega cards, one per movie
	};

signals:
	void filterTextChanged(const QString& text);
	void filterStatusChanged(int statusIndex);   // 0=all, 1=proposed, 2=missing-poster
	void quickFiltersChanged(quint32 flags);
	void sortOrderChanged(int order);
	void ratingFilterChanged(double minRating, double maxRating);
	void storageGroupFilterChanged(quint32 groupMask);   // bit (1<<group); 0 = show all
	void redundantOnlyFilterChanged(bool on);   // GroupedByEdition only
	void editionFilterChanged(const QSet<QString>& editions);   // empty = show all
	void hdrDvFilterChanged(const QSet<QString>& labels);   // empty = show all; see DolbyVisionInfo::filterLabels()
	void audioFormatFilterChanged(const QSet<QString>& labels);   // empty = show all; see AudioFormatInfo::filterLabels()

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

	// "N duplicate versions" toggle — visible only while GroupedByEdition is active
	// AND there's at least one actual duplicate to filter for.
	QToolButton*  m_redundantChip = nullptr;
	int           m_redundantGroupCount = 0;
	bool          m_groupModeActive = false;

	McMultiCheckDropdown* m_editionDropdown = nullptr;
	McMultiCheckDropdown* m_hdrDvDropdown = nullptr;
	McMultiCheckDropdown* m_audioFormatDropdown = nullptr;

	void updateRedundantChipVisibility();
};

} // namespace Mc
