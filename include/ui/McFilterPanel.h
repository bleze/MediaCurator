#pragma once
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QTimer;

namespace Mc {

class McFilterPanel : public QWidget {
	Q_OBJECT
public:
	explicit McFilterPanel(QWidget* parent = nullptr);

	QComboBox* statusCombo() const { return m_statusCombo; }
	QComboBox* sortCombo()   const { return m_sortCombo;   }

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
		SortByName       = 0,
		SortByNewest     = 1,
		SortByOldest     = 2,
		SortByLargest    = 3,
		SortByRatingHigh = 4,
		SortByRatingLow  = 5,
	};

signals:
	void filterTextChanged(const QString& text);
	void filterStatusChanged(int statusIndex);   // 0=all, 1=proposed, 2=missing-poster
	void quickFiltersChanged(quint32 flags);
	void sortOrderChanged(int order);
	void ratingFilterChanged(double minRating, double maxRating);

private:
	void onPillToggled(quint32 flag, bool on);

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
};

} // namespace Mc
