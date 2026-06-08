#pragma once
#include "engine/TrackDecision.h"
#include <QDialog>

class QSplitter;
class QTableWidget;
class QLabel;

namespace Mc {

/**
 * McPreviewDialog — shows the before/after track layout for a single file.
 *
 * "Before" table: every track with a Decision column (Keep/Remove/Unsure)
 *                 colour-coded, plus the rule-engine reason string.
 * "After"  table: only tracks that are marked Keep.
 *
 * Geometry and splitter position are persisted in QSettings.
 */
class McPreviewDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McPreviewDialog(QWidget* parent = nullptr);
	explicit McPreviewDialog(const FileDecision& decision, QWidget* parent = nullptr);

protected:
	void done(int result) override;

private:
	void setupUi(const FileDecision& decision);
	static void populateBeforeTable(QTableWidget* table, const FileDecision& decision);
	static void populateAfterTable (QTableWidget* table, const FileDecision& decision);
	static QString formatBitrate(qint64 bps);
	static QString formatStreamSize(qint64 bytes);
	static QString decisionText(Decision d);
	static QColor  decisionColor(Decision d);

	QSplitter* m_splitter = nullptr;
};

} // namespace Mc
