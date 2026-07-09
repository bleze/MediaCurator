#include "core/StorageGroupSettings.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"

#include <QDir>
#include <QJsonObject>

namespace Mc {

QString StorageGroupSettings::normalizedRoot(const QString& root)
{
	QString path = QDir::fromNativeSeparators(root.trimmed());
	while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
		path.chop(1);
#ifdef Q_OS_WIN
	if (path.size() == 2 && path.at(1) == QLatin1Char(':'))
		path += QLatin1Char('/');
#endif
	return path;
}

static QJsonObject groupsObject()
{
	auto& settings = AppSettings::instance();

	// Read directly from JSON — avoids QVariant round-trips for nested objects.
	const QJsonObject app = settings.rawRoot().value(QStringLiteral("app")).toObject();
	QJsonObject obj;
	if (app.contains(QStringLiteral("storage/groups")))
		obj = app.value(QStringLiteral("storage/groups")).toObject();
	else if (app.contains(QStringLiteral("scan/threadGroups")))
		obj = app.value(QStringLiteral("scan/threadGroups")).toObject();

	if (!obj.isEmpty() && !app.contains(QStringLiteral("storage/groups")))
		settings.setValue(QStringLiteral("storage/groups"), obj);

	return obj;
}

static void saveGroupsObject(const QJsonObject& obj)
{
	AppSettings::instance().setValue(QStringLiteral("storage/groups"), obj);
}

int StorageGroupSettings::groupForRoot(const QString& root)
{
	const QString key = normalizedRoot(root);
	const QJsonObject groups = groupsObject();
	if (!groups.contains(key))
		return DefaultGroup;

	const int group = groups.value(key).toInt(DefaultGroup);
	return qBound(MinGroup, group, MaxGroup);
}

void StorageGroupSettings::setGroupForRoot(const QString& root, int group)
{
	const QString key = normalizedRoot(root);
	QJsonObject groups = groupsObject();
	groups.insert(key, qBound(MinGroup, group, MaxGroup));
	saveGroupsObject(groups);
}

void StorageGroupSettings::removeRoot(const QString& root)
{
	const QString key = normalizedRoot(root);
	QJsonObject groups = groupsObject();
	if (!groups.contains(key))
		return;
	groups.remove(key);
	saveGroupsObject(groups);
}

QHash<int, QStringList> StorageGroupSettings::partitionRootsByGroup(const QStringList& roots)
{
	QHash<int, QStringList> out;
	for (const QString& root : roots) {
		const QString norm = normalizedRoot(root);
		if (norm.isEmpty())
			continue;
		out[groupForRoot(norm)] << norm;
	}
	return out;
}

int StorageGroupSettings::groupForFilePath(const QString& filePath)
{
	const QString norm = normalizedRoot(filePath);
	if (norm.isEmpty())
		return DefaultGroup;

	const QStringList roots = AppSettings::instance().value(QStringLiteral("scan/roots")).toStringList();
	QString bestRoot;
	for (const QString& root : roots) {
		const QString r = normalizedRoot(root);
		if (r.isEmpty())
			continue;
		const bool matches = norm.startsWith(r)
		    && (norm.size() == r.size() || norm.at(r.size()) == QLatin1Char('/'));
		if (matches && r.size() > bestRoot.size())
			bestRoot = r;
	}

	if (bestRoot.isEmpty())
		return DefaultGroup;
	return groupForRoot(bestRoot);
}

int StorageGroupSettings::groupForFileId(qint64 fileId)
{
	const auto fileOpt = DatabaseManager::instance().fileById(fileId);
	if (!fileOpt)
		return DefaultGroup;
	return groupForFilePath(fileOpt->path);
}

int StorageGroupSettings::uiMaxGroup()
{
	auto& settings = AppSettings::instance();
	int v = settings.value(QStringLiteral("storage/uiMaxGroups")).toInt();
	if (v <= 0)
		v = settings.value(QStringLiteral("scan/uiMaxGroups"), MaxGroup).toInt();
	return qBound(2, v, MaxGroup);
}

void StorageGroupSettings::setUiMaxGroup(int maxGroup)
{
	AppSettings::instance().setValue(QStringLiteral("storage/uiMaxGroups"),
	                                qBound(2, maxGroup, MaxGroup));
}

} // namespace Mc