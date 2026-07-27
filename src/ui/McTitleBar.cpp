#include "ui/McTitleBar.h"
#include "ui/SvgIcon.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace Mc {
namespace {

// Small QToolButton that swaps to a separately-supplied hover icon on
// mouseover — used so the close button can show a fixed white glyph over its
// red hover fill regardless of the app's light/dark palette, something
// Mc::svgIcon()'s palette-tracked recoloring can't give on its own.
class CaptionButton final : public QToolButton
{
public:
	explicit CaptionButton(QWidget* parent = nullptr) : QToolButton(parent) {}

	void setIcons(const QIcon& normalIcon, const QIcon& hoverIcon)
	{
		m_normalIcon = normalIcon;
		m_hoverIcon  = hoverIcon;
		setIcon(m_normalIcon);
	}

protected:
	void enterEvent(QEnterEvent* event) override
	{
		setIcon(m_hoverIcon);
		QToolButton::enterEvent(event);
	}

	void leaveEvent(QEvent* event) override
	{
		setIcon(m_normalIcon);
		QToolButton::leaveEvent(event);
	}

private:
	QIcon m_normalIcon;
	QIcon m_hoverIcon;
};

CaptionButton* makeCaptionButton(QWidget* parent, const QString& iconPath,
                                 const QString& tooltip, bool isCloseButton)
{
	auto* btn = new CaptionButton(parent);
	btn->setObjectName(QStringLiteral("titleBarCaptionButton"));
	btn->setProperty("close", isCloseButton);
	btn->setToolTip(tooltip);
	btn->setFixedSize(46, McTitleBar::kPreferredHeight);
	btn->setIconSize({16, 16});
	btn->setCursor(Qt::ArrowCursor);
	btn->setFocusPolicy(Qt::NoFocus);

	const QIcon normalIcon = svgIcon(iconPath);
	const QIcon hoverIcon  = isCloseButton ? svgIcon(iconPath, QColor(Qt::white)) : normalIcon;
	btn->setIcons(normalIcon, hoverIcon);
	return btn;
}

} // namespace

McTitleBar::McTitleBar(QWidget* parent)
	: QWidget(parent)
{
	setFixedHeight(kPreferredHeight);

	setStyleSheet(
	    "QToolButton#titleBarCaptionButton {"
	    "  border: none; background: transparent; padding: 0;"
	    "}"
	    "QToolButton#titleBarCaptionButton:hover {"
	    "  background: rgba(128,128,128,60);"
	    "}"
	    "QToolButton#titleBarCaptionButton:pressed {"
	    "  background: rgba(128,128,128,100);"
	    "}"
	    "QToolButton#titleBarCaptionButton[close=\"true\"]:hover {"
	    "  background: #E81123;"
	    "}"
	    "QToolButton#titleBarCaptionButton[close=\"true\"]:pressed {"
	    "  background: #C42B1C;"
	    "}");

	m_iconLabel = new QLabel(this);
	m_iconLabel->setFixedSize(16, 16);
	m_iconLabel->setScaledContents(true);

	m_titleLabel = new QLabel(this);

	m_btnTray       = makeCaptionButton(this, ":/icons/titlebar_tray.svg", tr("Minimize to Tray"), false);
	m_btnMinimize   = makeCaptionButton(this, ":/icons/titlebar_minimize.svg", tr("Minimize"), false);
	m_btnMaxRestore = makeCaptionButton(this, ":/icons/titlebar_maximize.svg", tr("Maximize"), false);
	m_btnClose      = makeCaptionButton(this, ":/icons/titlebar_close.svg", tr("Close"), true);

	connect(m_btnTray,       &QToolButton::clicked, this, &McTitleBar::minimizeToTrayRequested);
	connect(m_btnMinimize,   &QToolButton::clicked, this, &McTitleBar::minimizeRequested);
	connect(m_btnMaxRestore, &QToolButton::clicked, this, &McTitleBar::maximizeRestoreRequested);
	connect(m_btnClose,      &QToolButton::clicked, this, &McTitleBar::closeRequested);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 0, 0, 0);
	layout->setSpacing(6);
	layout->addWidget(m_iconLabel);
	layout->addWidget(m_titleLabel);
	layout->addStretch(1);
	layout->addWidget(m_btnTray);
	layout->addWidget(m_btnMinimize);
	layout->addWidget(m_btnMaxRestore);
	layout->addWidget(m_btnClose);
}

void McTitleBar::setTitleBarWindowIcon(const QIcon& icon)
{
	m_iconLabel->setPixmap(icon.pixmap(16, 16));
}

void McTitleBar::setTitleText(const QString& title)
{
	m_titleLabel->setText(title);
}

void McTitleBar::setMaximized(bool maximized)
{
	if (m_maximized == maximized)
		return;
	m_maximized = maximized;

	const QIcon icon = svgIcon(maximized ? ":/icons/titlebar_restore.svg" : ":/icons/titlebar_maximize.svg");
	static_cast<CaptionButton*>(m_btnMaxRestore)->setIcons(icon, icon);
	m_btnMaxRestore->setToolTip(maximized ? tr("Restore Down") : tr("Maximize"));
}

bool McTitleBar::isDragRegion(const QPoint& localPos) const
{
	QWidget* w = childAt(localPos);
	if (!w) return true;
	if (w == m_btnTray || w == m_btnMinimize || w == m_btnMaxRestore || w == m_btnClose)
		return false;
	return true;
}

} // namespace Mc
