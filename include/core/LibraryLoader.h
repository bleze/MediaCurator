#pragma once
#include "core/DatabaseManager.h"
#include <QHash>
#include <QObject>
#include <QSet>

namespace Mc {

class LibraryLoader : public QObject
{
	Q_OBJECT
public:
	explicit LibraryLoader(int startOffset, QObject* parent = nullptr);

public slots:
	void run();

signals:
	void metaReady(QHash<qint64, QString> posterPaths,
	               QHash<qint64, QString> imdbIds,
	               QSet<qint64> filesWithJobs,
	               QHash<qint64, double> ratings);
	void fileReady(Mc::FileRecord file, QList<Mc::StreamRecord> streams);
	void finished(int totalFileCount);

private:
	int m_startOffset;
};

} // namespace Mc
