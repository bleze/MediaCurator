#include "ui/McGbGoalDialog.h"

#include <QDialogButtonBox>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QVBoxLayout>

namespace Mc {

McGbGoalDialog::McGbGoalDialog(int initialGB, int minGB, QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Free Up Space"));

	const int maxGB = qMax(150, minGB);
	const int startGB = qBound(minGB, initialGB, maxGB);

	auto* layout = new QVBoxLayout(this);

	auto* introLabel = new QLabel(tr("Run the largest-savings jobs first until this much space is freed:"), this);
	introLabel->setWordWrap(true);
	layout->addWidget(introLabel);

	m_valueLabel = new QLabel(this);
	QFont f = m_valueLabel->font();
	f.setPointSizeF(f.pointSizeF() * 1.6);
	f.setBold(true);
	m_valueLabel->setFont(f);
	m_valueLabel->setAlignment(Qt::AlignHCenter);
	layout->addWidget(m_valueLabel);

	m_slider = new QSlider(Qt::Horizontal, this);
	m_slider->setRange(minGB, maxGB);
	m_slider->setValue(startGB);
	m_slider->setTickPosition(QSlider::TicksBelow);
	m_slider->setTickInterval(qMax(1, (maxGB - minGB) / 10));
	layout->addWidget(m_slider);

	auto* rangeLayout = new QHBoxLayout();
	rangeLayout->addWidget(new QLabel(tr("%1 GB").arg(minGB), this));
	rangeLayout->addStretch(1);
	rangeLayout->addWidget(new QLabel(tr("%1 GB").arg(maxGB), this));
	layout->addLayout(rangeLayout);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	connect(m_slider, &QSlider::valueChanged, this, [this](int v) {
		m_valueLabel->setText(tr("%1 GB").arg(v));
	});
	m_valueLabel->setText(tr("%1 GB").arg(startGB));

	setMinimumWidth(360);
}

int McGbGoalDialog::goalGB() const
{
	return m_slider->value();
}

qint64 McGbGoalDialog::goalBytes() const
{
	return static_cast<qint64>(m_slider->value()) * 1024LL * 1024 * 1024;
}

} // namespace Mc
