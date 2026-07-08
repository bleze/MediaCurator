#pragma once

#include <QWidget>

class QMouseEvent;
class QTimer;

namespace Mc {

// Aggregate progress bar shown at the top of the job queue while a GB-goal run
// (JobQueue::startWithGoal) is active. Reuses the same 3px "size bar" look drawn
// on job cards (McCardDelegate) — including its light/dark green pulse animation,
// which signals "actively running" the same way there — for live aggregated
// savings (confirmed savings from finished jobs plus the in-flight job's partial
// estimate). The numeric readout lives in McJobPanel's footer label, not here.
class McGoalProgressBar : public QWidget
{
	Q_OBJECT
public:
	explicit McGoalProgressBar(QWidget* parent = nullptr);

	void setGoal(qint64 bytes);
	void setCompleted(qint64 bytes);
	// Estimated partial contribution of the currently running job (0 when idle) —
	// call this from JobQueue::progressChanged so the bar moves continuously while
	// a file is being muxed, not just when a job finishes.
	void setLive(qint64 bytes);

	qint64 goalBytes() const { return m_goalBytes; }
	// Confirmed savings from finished jobs only (excludes the in-flight job's live
	// estimate) — used to isolate how much more the current job alone will add.
	qint64 completedBytes() const { return m_completedBytes; }
	// Confirmed savings from finished jobs plus the in-flight job's live estimate —
	// the same value the bar's fill fraction is computed from.
	qint64 currentBytes() const { return m_completedBytes + m_liveBytes; }

signals:
	// Emitted on click — McJobPanel uses this to open the goal-edit dialog.
	void clicked();

protected:
	void paintEvent(QPaintEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;

private:
	QTimer* m_pulseTimer     = nullptr;
	qint64  m_goalBytes      = 0;
	qint64  m_completedBytes = 0;
	qint64  m_liveBytes      = 0;
};

} // namespace Mc
