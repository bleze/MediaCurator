#pragma once
#include <QDialog>
#include <QHash>
#include <QString>

#include "scanner/ScanWorker.h"

class QTableWidget;

namespace Mc {

// McScanCompleteDialog — replaces the old QMessageBox::setDetailedText scan
// summary, which had no size policy and was unusable for anything past a
// handful of files. Lists every newly discovered and removed file, git-status
// style (green "+" / red "-"), with title, resolution/HDR badge, size and
// storage group (when more than one group is in use) in a properly sized,
// resizable table. Media-type chips for newly added files start as a pending
// "…" pill and fill in live as PosterManager's background TMDB lookups
// complete while the dialog is still open.
class McScanCompleteDialog : public QDialog
{
	Q_OBJECT
public:
	// newFiles/removedFiles: files added to / pruned from the DB this scan session.
	// updatedCount: session total — no per-file list is tracked for in-place updates yet
	// (see ScanWorker::finished), so it's summarized only.
	McScanCompleteDialog(const ScanChangeList& newFiles, const ScanChangeList& removedFiles,
	                     int updatedCount, QWidget* parent = nullptr);

private:
	void onTmdbDataReady(qint64 fileId, const QString& mediaType);

	QTableWidget* m_table = nullptr;
	// row index for each added file's fileId, so a late-arriving TMDB result
	// can find its row without a linear rescan of the table.
	QHash<qint64, int> m_rowForAddedFileId;
};

} // namespace Mc
