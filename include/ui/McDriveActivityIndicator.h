#pragma once
#include <QWidget>

class QPropertyAnimation;

namespace Mc {

// Status-bar "is a drive spinning right now" indicator. Lights up (in the
// touched storage group's accent color — see StorageGroupSettings::colorForGroup)
// on DriveActivityMonitor::activity() and eases back to a dim idle gray over
// that group's configured spin-down duration (StorageGroupSettings::
// spinDownMinutes), so the fade roughly tracks when the real NAS/disk is
// expected to actually power its drives down. Purely observational — this
// widget never touches disk itself.
class McDriveActivityIndicator final : public QWidget
{
	Q_OBJECT
	Q_PROPERTY(qreal level READ level WRITE setLevel)
public:
	explicit McDriveActivityIndicator(QWidget* parent = nullptr);

	qreal level() const { return m_level; }
	void  setLevel(qreal level);

	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent*) override;

private slots:
	void onActivity(int group);

private:
	static constexpr int kIconSize = 16;

	int                  m_activeGroup = 1;
	qreal                m_level       = 0.0;
	QPropertyAnimation*  m_fade        = nullptr;
};

} // namespace Mc
