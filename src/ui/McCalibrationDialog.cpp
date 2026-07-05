#include "ui/McCalibrationDialog.h"
#include "core/DatabaseManager.h"
#include "engine/TrackDecision.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace Mc {

// currentFallbackBps()/fallbackConstName() used to re-derive the fallback bucket from
// (codecName, codecType) alone, without codecProfile — which meant DTS and DTS-HD MA/HRA
// (same ffprobe codec_name "dts", very different fallback constants) were indistinguishable
// here. calibrationReport() now returns entries already keyed by Mc::fallbackBpsKey(), the
// same stable bucket identifier updateCalibrationFromJob() stores by, so both the value and
// constexpr-name lookups below just resolve that key directly via the shared TrackDecision.h
// functions — one source of truth instead of two hand-maintained copies of the same mapping.

// Format a bps integer with C++ digit separators: 3500000 -> "3'500'000"
static QString fmtBps(qint64 bps)
{
	QString s = QString::number(bps);
	for (int i = s.length() - 3; i > 0; i -= 3)
		s.insert(i, QLatin1Char('\''));
	return s;
}

McCalibrationDialog::McCalibrationDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Estimation Calibration Data"));
	setAttribute(Qt::WA_DeleteOnClose);
	resize(840, 500);

	auto* vlay = new QVBoxLayout(this);

	auto* info = new QLabel(
		tr("<b>How to read this:</b> Each row shows the average accuracy ratio for a codec "
		   "across completed jobs. A ratio of 1.0 = perfect estimate; &lt; 1.0 = over-estimated. "
		   "Fallback rows are worth tuning — click <b>Copy Suggested Code</b> to get ready-to-paste "
		   "<code>constexpr</code> lines for the <code>FallbackBps</code> namespace in "
		   "<code>include/engine/TrackDecision.h</code>. "
		   "If you recently changed a fallback value, click <b>Clear Calibration Data</b> first "
		   "so old measurements don’t contaminate the averages."),
		this);
	info->setWordWrap(true);
	info->setTextFormat(Qt::RichText);
	vlay->addWidget(info);

	const auto entries = DatabaseManager::instance().calibrationReport();

	auto* table = new QTableWidget(entries.size(), 7, this);
	table->setHorizontalHeaderLabels({
		tr("Codec"), tr("Type"), tr("Bitrate Source"), tr("Samples"),
		tr("Avg Ratio"), tr("Std Dev"), tr("Suggested Fallback")
	});
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
	table->verticalHeader()->setVisible(false);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setAlternatingRowColors(true);

	const int    MIN_SAMPLES = 5;
	const double MIN_DRIFT   = 0.05;

	for (int row = 0; row < entries.size(); ++row) {
		const CalibrationEntry& e = entries[row];
		const qint64 fallback     = static_cast<qint64>(fallbackBpsForKey(e.codecName));
		const bool confident      = e.sampleCount >= MIN_SAMPLES
		                         && e.stdDevRatio / qMax(e.avgRatio, 0.01) < 0.4;
		const bool worthChanging  = qAbs(e.avgRatio - 1.0) >= MIN_DRIFT;
		const bool extreme        = e.avgRatio < 0.2 || e.avgRatio > 5.0;

		const auto cell = [&](int col, const QString& text, bool dim = false,
		                      bool warn = false) {
			auto* item = new QTableWidgetItem(text);
			item->setTextAlignment(Qt::AlignCenter);
			if (warn)
				item->setForeground(QColor(0xC0, 0x60, 0x00));
			else if (dim)
				item->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
			table->setItem(row, col, item);
		};

		cell(0, e.codecName);
		cell(1, e.codecType);
		cell(2, e.usedFallback ? tr("fallback") : tr("declared"));
		cell(3, QString::number(e.sampleCount));
		cell(4, QString::number(e.avgRatio, 'f', 3), false, extreme);
		cell(5, e.sampleCount > 1 ? QString::number(e.stdDevRatio, 'f', 3) : tr("—"));

		if (e.usedFallback && fallback > 0) {
			const qint64 suggested = qRound64(fallback * e.avgRatio / 1000.0) * 1000;
			const bool actionable  = confident && worthChanging;
			QString label;
			if (extreme) {
				label = QStringLiteral("%1 kbps  (!!! suspicious ratio)").arg(suggested / 1000);
			} else if (actionable) {
				label = QStringLiteral("%1 kbps  (was %2)").arg(suggested / 1000).arg(fallback / 1000);
			} else if (e.sampleCount < MIN_SAMPLES) {
				label = QStringLiteral("%1 kbps  (low samples)").arg(suggested / 1000);
			} else {
				label = tr("no change needed");
			}
			cell(6, label, !actionable, extreme);
		} else {
			cell(6, tr("—"), true);
		}
	}

	vlay->addWidget(table);

	// ── Buttons ─────────────────────────────────────────────────────────────
	auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, this);

	auto* clearBtn = new QPushButton(tr("Clear Calibration Data"), this);
	btnBox->addButton(clearBtn, QDialogButtonBox::ResetRole);

	auto* csvBtn = new QPushButton(tr("Copy as CSV"), this);
	btnBox->addButton(csvBtn, QDialogButtonBox::ActionRole);

	auto* codeBtn = new QPushButton(tr("Copy Suggested Code"), this);
	btnBox->addButton(codeBtn, QDialogButtonBox::ActionRole);

	connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::accept);

	connect(clearBtn, &QPushButton::clicked, this, [this] {
		const auto reply = QMessageBox::warning(
			this,
			tr("Clear Calibration Data"),
			tr("This will delete all accumulated calibration samples.\n\n"
			   "Do this after changing a fallback constant so that old measurements\n"
			   "don’t skew future suggestions.\n\n"
			   "Continue?"),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (reply == QMessageBox::Yes) {
			DatabaseManager::instance().clearCalibration();
			accept(); // Close and re-open to show empty state
		}
	});

	connect(csvBtn, &QPushButton::clicked, this, [entries] {
		QStringList lines;
		lines << QStringLiteral("codec_name,codec_type,bitrate_source,sample_count,avg_ratio,std_dev_ratio,suggested_fallback_bps");
		for (const CalibrationEntry& e : entries) {
			const qint64 fallback  = static_cast<qint64>(fallbackBpsForKey(e.codecName));
			const qint64 suggested = (e.usedFallback && fallback > 0)
			    ? qRound64(fallback * e.avgRatio / 1000.0) * 1000 : 0;
			lines << QStringLiteral("%1,%2,%3,%4,%5,%6,%7")
			         .arg(e.codecName, e.codecType,
			              e.usedFallback ? "fallback" : "declared")
			         .arg(e.sampleCount)
			         .arg(e.avgRatio, 0, 'f', 4)
			         .arg(e.stdDevRatio, 0, 'f', 4)
			         .arg(suggested);
		}
		QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
	});

	connect(codeBtn, &QPushButton::clicked, this, [entries] {
		const int    MIN_S = 5;
		const double MIN_D = 0.05;

		QStringList out;
		out << QStringLiteral("// Calibration report -- update FallbackBps in include/engine/TrackDecision.h");
		out << QStringLiteral("// Codec                         | Type     |  n  |  ratio  | cur kbps -> sug kbps");

		// summary comment lines
		for (const CalibrationEntry& e : entries) {
			if (!e.usedFallback) continue;
			const qint64 fallback = static_cast<qint64>(fallbackBpsForKey(e.codecName));
			if (fallback <= 0) continue;
			const qint64 suggested = qRound64(fallback * e.avgRatio / 1000.0) * 1000;
			const bool confident   = e.sampleCount >= MIN_S
			                      && e.stdDevRatio / qMax(e.avgRatio, 0.01) < 0.4;
			const bool drift       = qAbs(e.avgRatio - 1.0) >= MIN_D;
			const bool extreme     = e.avgRatio < 0.2 || e.avgRatio > 5.0;
			QString flag;
			if (extreme)
				flag = QStringLiteral("  // !! suspicious ratio -- stale data? clear and re-collect");
			else if (!confident || !drift)
				flag = QStringLiteral("  // LOW CONFIDENCE");
			out << QStringLiteral("// %1 | %2 | %3 | %4 | %5 -> %6%7")
			           .arg(e.codecName, -30)
			           .arg(e.codecType, -8)
			           .arg(e.sampleCount, 3)
			           .arg(e.avgRatio, 7, 'f', 3)
			           .arg(fallback / 1000, 6)
			           .arg(suggested / 1000, 6)
			           .arg(flag);
		}

		// actual constexpr lines — one per unique constant, taking the most-sampled entry
		// when multiple codecs map to the same constant (e.g. pgs + dvd_subtitle -> kPgsSubtitle)
		QMap<QString, QPair<qint64, int>> best; // constName -> (suggestedBps, sampleCount)
		for (const CalibrationEntry& e : entries) {
			if (!e.usedFallback) continue;
			const qint64 fallback = static_cast<qint64>(fallbackBpsForKey(e.codecName));
			if (fallback <= 0) continue;
			const bool confident  = e.sampleCount >= MIN_S
			                     && e.stdDevRatio / qMax(e.avgRatio, 0.01) < 0.4;
			const bool drift      = qAbs(e.avgRatio - 1.0) >= MIN_D;
			if (!confident || !drift) continue;
			const QString cname   = fallbackBpsConstName(e.codecName);
			if (cname.isEmpty()) continue;
			const qint64 suggested = qRound64(fallback * e.avgRatio / 1000.0) * 1000;
			if (!best.contains(cname) || e.sampleCount > best[cname].second)
				best[cname] = {suggested, e.sampleCount};
		}

		if (best.isEmpty()) {
			out << QStringLiteral("//");
			out << QStringLiteral("// No confident suggestions yet -- run more jobs to build up sample counts.");
			out << QStringLiteral("// (Or clear stale data above and re-run if ratios look suspicious.)");
		} else {
			out << QStringLiteral("//");
			out << QStringLiteral("// Paste these into TrackDecision.h, replacing matching lines in namespace FallbackBps:");
			for (auto it = best.constBegin(); it != best.constEnd(); ++it) {
				const QString constName  = it.key();
				const qint64  sugBps     = it.value().first;
				out << QStringLiteral("constexpr double %1 = %2.0;")
				           .arg(constName, -14)
				           .arg(fmtBps(sugBps));
			}
		}

		QApplication::clipboard()->setText(out.join(QLatin1Char('\n')));
	});

	vlay->addWidget(btnBox);

	if (entries.isEmpty()) {
		table->setRowCount(1);
		auto* placeholder = new QTableWidgetItem(tr("No calibration data yet -- complete some jobs first."));
		placeholder->setTextAlignment(Qt::AlignCenter);
		placeholder->setForeground(QApplication::palette().color(QPalette::PlaceholderText));
		table->setSpan(0, 0, 1, 7);
		table->setItem(0, 0, placeholder);
	}
}

} // namespace Mc
