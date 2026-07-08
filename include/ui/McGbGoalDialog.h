#pragma once

#include <QDialog>

class QLabel;
class QSlider;

namespace Mc {

// Slider dialog for picking a "free up N GB" target for JobQueue::startWithGoal()
// / setGoalBytes(). Also reused (with minGB clamped to the amount already freed)
// to raise the target while a goal run is in progress.
class McGbGoalDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McGbGoalDialog(int initialGB, int minGB, QWidget* parent = nullptr);

	int    goalGB() const;
	qint64 goalBytes() const;

private:
	QSlider* m_slider    = nullptr;
	QLabel*  m_valueLabel = nullptr;
};

} // namespace Mc
