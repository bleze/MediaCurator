#include "donate/McDonateDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace Mc {

namespace {
QPushButton* makeIconButton(const QIcon& icon, const QString& fallbackText, const QString& tip, QWidget* parent)
{
	auto* btn = new QPushButton(icon, icon.isNull() ? fallbackText : QString(), parent);
	if (icon.isNull())
		btn->setFixedHeight(26);
	else
		btn->setFixedSize(32, 26);
	btn->setToolTip(tip);
	return btn;
}
} // namespace

McDonateDialog::McDonateDialog(const QString& appName, const QString& intro, QWidget* parent,
                               const QIcon& copyIcon, const QIcon& openIcon)
	: QDialog(parent)
{
	setWindowTitle(tr("Support %1").arg(appName));
	setFixedWidth(460);

	auto* layout = new QVBoxLayout(this);
	layout->setSpacing(10);
	layout->setContentsMargins(16, 16, 16, 16);

	auto* introLabel = new QLabel(intro, this);
	introLabel->setWordWrap(true);
	layout->addWidget(introLabel);

	layout->addSpacing(8);

	auto* columns = new QHBoxLayout;
	columns->setSpacing(16);

	// Build the badge first so we can measure its exact height for PayPal alignment
	auto* preferredBadge = new QLabel(tr("★  Preferred"), this);
	preferredBadge->setStyleSheet(QStringLiteral(
		"color: white; background-color: #F7931A; border-radius: 4px;"
		" padding: 3px 12px; font-weight: bold; border: none;"));
	preferredBadge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	preferredBadge->ensurePolished();
	// PayPal title must start at: badge height + 1px panel border + 8px panel top margin
	const int paypalTopOffset = preferredBadge->sizeHint().height() + 1 + 8;

	// ---- PayPal column ----
	auto* paypalCol = new QVBoxLayout;
	paypalCol->setSpacing(0);
	paypalCol->addSpacing(paypalTopOffset);

	auto* paypalLabel = new QLabel(tr("PayPal"), this);
	paypalLabel->setStyleSheet("font-weight: bold;");
	paypalLabel->setAlignment(Qt::AlignHCenter);
	paypalCol->addWidget(paypalLabel);
	paypalCol->addSpacing(6);

	auto* paypalQrLabel = new QLabel(this);
	{
		QPixmap qr(":/donate/paypal_qr.png");
		if (!qr.isNull())
			paypalQrLabel->setPixmap(qr.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	paypalQrLabel->setAlignment(Qt::AlignHCenter);
	paypalCol->addWidget(paypalQrLabel);
	paypalCol->addSpacing(6);

	const QString paypalUrl = QStringLiteral("https://paypal.me/blezedk");
	auto* paypalRow = new QHBoxLayout;
	auto* paypalEdit = new QLineEdit(QStringLiteral("paypal.me/blezedk"), this);
	paypalEdit->setReadOnly(true);
	paypalEdit->setFrame(false);
	paypalEdit->setStyleSheet("background: transparent;");
	paypalRow->addWidget(paypalEdit, 1);
	auto* paypalCopyBtn = makeIconButton(copyIcon, tr("Copy"), tr("Copy to clipboard"), this);
	connect(paypalCopyBtn, &QPushButton::clicked, this, [paypalUrl]() {
		QApplication::clipboard()->setText(paypalUrl);
	});
	paypalRow->addWidget(paypalCopyBtn);
	auto* paypalOpenBtn = makeIconButton(openIcon, tr("Open"), tr("Open in browser"), this);
	connect(paypalOpenBtn, &QPushButton::clicked, this, [paypalUrl]() {
		QDesktopServices::openUrl(QUrl(paypalUrl));
	});
	paypalRow->addWidget(paypalOpenBtn);
	paypalCol->addLayout(paypalRow);
	paypalCol->addStretch();

	columns->addLayout(paypalCol, 1);

	// ---- Lightning column (preferred) ----
	// Badge sits directly above the panel with 0 spacing → appears to rest on the border
	auto* lightningOuter = new QVBoxLayout;
	lightningOuter->setSpacing(0);

	auto* badgeRow = new QHBoxLayout;
	badgeRow->addStretch();
	badgeRow->addWidget(preferredBadge);
	badgeRow->addStretch();
	lightningOuter->addLayout(badgeRow);

	auto* lightningPanel = new QWidget(this);
	lightningPanel->setObjectName("lightningPanel");
	lightningPanel->setStyleSheet(
		"#lightningPanel { border: 1px solid #F7931A; border-radius: 8px; }");

	auto* lightningCol = new QVBoxLayout(lightningPanel);
	lightningCol->setSpacing(6);
	lightningCol->setContentsMargins(8, 8, 8, 8);

	auto* lightningLabel = new QLabel(tr("⚡ Lightning"), lightningPanel);
	lightningLabel->setStyleSheet("font-weight: bold; border: none;");
	lightningLabel->setAlignment(Qt::AlignHCenter);
	lightningCol->addWidget(lightningLabel);

	auto* qrLabel = new QLabel(lightningPanel);
	{
		QPixmap qr(":/donate/lightning_qr.png");
		if (!qr.isNull())
			qrLabel->setPixmap(qr.scaled(180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
	qrLabel->setAlignment(Qt::AlignHCenter);
	lightningCol->addWidget(qrLabel);

	auto* lightningRow = new QHBoxLayout;
	auto* lightningEdit = new QLineEdit(QStringLiteral("bleze@cake.cash"), lightningPanel);
	lightningEdit->setReadOnly(true);
	lightningEdit->setFrame(false);
	lightningEdit->setStyleSheet("background: transparent; border: none;");
	lightningRow->addWidget(lightningEdit, 1);
	auto* lightningCopyBtn = makeIconButton(copyIcon, tr("Copy"), tr("Copy to clipboard"), lightningPanel);
	connect(lightningCopyBtn, &QPushButton::clicked, this, [lightningEdit]() {
		QApplication::clipboard()->setText(lightningEdit->text());
	});
	lightningRow->addWidget(lightningCopyBtn);
	lightningCol->addLayout(lightningRow);
	lightningCol->addStretch();

	lightningOuter->addWidget(lightningPanel, 1);
	columns->addLayout(lightningOuter, 1);

	layout->addLayout(columns);
	layout->addStretch();

	auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(btnBox);

	btnBox->button(QDialogButtonBox::Close)->setFocus();
}

} // namespace Mc
