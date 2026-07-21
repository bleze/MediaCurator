#pragma once
#include <QDialog>

class QStackedWidget;
class QLabel;
class QPushButton;

namespace Mc {

/**
 * McOnboardingDialog — first-run welcome tour explaining the scan → review →
 * policy → remux workflow. Shown automatically once (McMainWindow tracks the
 * "onboarding/seen" flag) and re-openable anytime via View → Getting Started…
 */
class McOnboardingDialog : public QDialog
{
	Q_OBJECT
public:
	// Bump whenever pages are added or meaningfully changed. McMainWindow shows
	// the dialog again on next startup for anyone whose stored
	// "onboarding/seenVersion" is below this, even if they've seen an earlier tour.
	static constexpr int CurrentVersion = 2;

	explicit McOnboardingDialog(QWidget* parent = nullptr);

private:
	void goToPage(int index);

	QStackedWidget* m_stack     = nullptr;
	QLabel*         m_stepLabel = nullptr;
	QPushButton*    m_btnBack   = nullptr;
	QPushButton*    m_btnNext   = nullptr;
	int             m_pageCount = 0;
};

} // namespace Mc
