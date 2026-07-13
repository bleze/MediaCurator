#pragma once

#include <QColor>
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

	// Fixed per-group accent color (group 1..MaxGroup), shared by the card
	// badge (McCardDelegate) and the group picker (McManageFoldersDialog).
	// Keyed by group number, not by how many groups are currently offered —
	// a group's color never changes if uiMaxGroup() shrinks or grows.
	[[nodiscard]] static QColor colorForGroup(int group);

	// True when the configured library roots resolve to more than one
	// distinct storage group — the signal used to decide whether the
	// per-card group badge should render at all.
	[[nodiscard]] static bool multipleGroupsInUse();
};

} // namespace Mc