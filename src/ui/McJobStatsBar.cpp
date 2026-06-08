#include "ui/McJobStatsBar.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

namespace Mc {

static QString formatSizeBar(qint64 bytes)
{
	if (bytes <= 0) return QStringLiteral("0 B");
	if (bytes >= qint64(1024) * 1024 * 1024)
		return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
	if (bytes >= 1024 * 1024)
		return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
	return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
}

static QWidget* makeStatCell(const QString& number, const QString& label,
                              bool highlight, QWidget* parent)
{
	auto* w      = new QWidget(parent);
	auto* layout = new QVBoxLayout(w);
	layout->setContentsMargins(12, 8, 12, 8);
	layout->setSpacing(2);

	auto* numLbl = new QLabel(number, w);
	QFont f = numLbl->font();
	f.setPointSizeF(f.pointSizeF() * (highlight ? 2.8 : 2.2));
	f.setBold(true);
	numLbl->setFont(f);
	numLbl->setAlignment(Qt::AlignHCenter);
	if (highlight) {
		QPalette np = numLbl->palette();
		np.setColor(QPalette::WindowText, np.color(QPalette::Highlight));
		numLbl->setPalette(np);
	}

	auto* txtLbl = new QLabel(label, w);
	txtLbl->setAlignment(Qt::AlignHCenter);
	QFont sf = txtLbl->font();
	sf.setPointSizeF(sf.pointSizeF() * 0.88);
	txtLbl->setFont(sf);
	QPalette pal = txtLbl->palette();
	pal.setColor(QPalette::WindowText, pal.color(QPalette::WindowText).darker(130));
	txtLbl->setPalette(pal);

	layout->addWidget(numLbl);
	layout->addWidget(txtLbl);
	return w;
}

McJobStatsBar::McJobStatsBar(const QList<StatItem>& items, qint64 estimatedBytes, QWidget* parent)
	: QWidget(parent)
{
	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	for (const auto& item : items)
		layout->addWidget(makeStatCell(item.number, item.label, false, this));

	layout->addStretch(1);

	if (estimatedBytes > 0) {
		auto* eqLbl = new QLabel(QStringLiteral("="), this);
		QFont eqFont = eqLbl->font();
		eqFont.setPointSizeF(eqFont.pointSizeF() * 2.2);
		eqFont.setBold(true);
		eqLbl->setFont(eqFont);
		eqLbl->setAlignment(Qt::AlignVCenter | Qt::AlignHCenter);
		eqLbl->setContentsMargins(8, 0, 8, 0);
		QPalette eqPal = eqLbl->palette();
		eqPal.setColor(QPalette::WindowText, eqPal.color(QPalette::PlaceholderText));
		eqLbl->setPalette(eqPal);
		layout->addWidget(eqLbl);

		layout->addWidget(makeStatCell(formatSizeBar(estimatedBytes), tr("est.\nreclaimed"), true, this));
	}
}

} // namespace Mc
