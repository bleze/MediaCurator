#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

namespace Mc {

/**
 * Storage-group assignments stored in AppSettings under storage/groups.
 *
 * Group folders that share the same underlying drive or NAS volume together.
 * Different groups may scan and remux in parallel; folders within one group
 * are processed one at a time so the same hardware is not saturated.
 */
class StorageGroupSettings
{
public:
	static constexpr int DefaultGroup = 1;
	static constexpr int MinGroup     = 1;
	static constexpr int MaxGroup     = 4;

	[[nodiscard]] static QString normalizedRoot(const QString& root);

	[[nodiscard]] static int groupForRoot(const QString& root);
	static void setGroupForRoot(const QString& root, int group);
	static void removeRoot(const QString& root);

	[[nodiscard]] static QHash<int, QStringList> partitionRootsByGroup(const QStringList& roots);

	// Resolve the storage group for a media file from its library root assignment.
	[[nodiscard]] static int groupForFilePath(const QString& filePath);
	[[nodiscard]] static int groupForFileId(qint64 fileId);

	// How many storage groups the UI offers (2–MaxGroup).
	[[nodiscard]] static int uiMaxGroup();
	static void setUiMaxGroup(int maxGroup);
};

} // namespace Mc