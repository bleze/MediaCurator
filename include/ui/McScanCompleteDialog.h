#pragma once
#include <QDialog>
#include <QString>
#include <QStringList>

class QTableWidget;

namespace Mc {

// McScanCompleteDialog — replaces the old QMessageBox::setDetailedText scan
// summary, which had no size policy and was unusable for anything past a
// handful of files. Lists every newly discovered file with its storage group
// (when more than one group is in use) in a properly sized, resizable table.
class McScanCompleteDialog : public QDialog
{
	Q_OBJECT
public:
	// newFiles: full paths of files discovered this scan session.
	// updatedCount/removedCount: session totals (no per-file lists are tracked
	// for these yet — see ScanWorker::finished — so they're summarized only).
	McScanCompleteDialog(const QStringList& newFiles, int updatedCount, int removedCount,
	                     QWidget* parent = nullptr);

private:
	QTableWidget* m_table = nullptr;
};

} // namespace Mc
