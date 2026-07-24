#include "core/DriveActivityMonitor.h"
#include "core/AppSettings.h"
#include "core/StorageGroupSettings.h"

#include <QDateTime>
#include <QMutexLocker>

namespace Mc {

DriveActivityMonitor& DriveActivityMonitor::instance()
{
	static DriveActivityMonitor s;
	return s;
}

void DriveActivityMonitor::touch(int group)
{
	auto& self = instance();
	bool emitSignal = false;
	{
		QMutexLocker lock(&self.m_mutex);
		self.m_current = { group, QDateTime::currentMSecsSinceEpoch() };
		emitSignal = !self.m_everTouched || self.m_lastAccepted.hasExpired(kMinIntervalMs);
		if (emitSignal) {
			self.m_everTouched = true;
			self.m_lastAccepted.start();
		}
	}
	if (emitSignal)
		emit self.activity(group);
}

void DriveActivityMonitor::touchPath(const QString& filePath)
{
	touch(StorageGroupSettings::groupForFilePath(filePath));
}

DriveActivityMonitor::LastActivity DriveActivityMonitor::current()
{
	auto& self = instance();
	QMutexLocker lock(&self.m_mutex);
	return self.m_current;
}

void DriveActivityMonitor::persist()
{
	const LastActivity la = current();
	if (la.group <= 0)
		return;
	auto& settings = AppSettings::instance();
	settings.setValue(QStringLiteral("driveActivity/lastGroup"), la.group);
	settings.setValue(QStringLiteral("driveActivity/lastTouchMs"), la.epochMs);
}

DriveActivityMonitor::LastActivity DriveActivityMonitor::loadPersisted()
{
	auto& settings = AppSettings::instance();
	LastActivity la;
	la.group   = settings.value(QStringLiteral("driveActivity/lastGroup"), 0).toInt();
	la.epochMs = settings.value(QStringLiteral("driveActivity/lastTouchMs"), 0).toLongLong();
	return la;
}

} // namespace Mc
