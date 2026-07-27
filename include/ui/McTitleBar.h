#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

namespace Mc {

// Custom-drawn titlebar row replacing the native Windows caption (see
// McMainWindow::setupTitleBar()/nativeEvent WM_NCCALCSIZE+WM_NCHITTEST
// handling) so a minimize-to-tray button can sit next to the normal minimize
// button — Qt cannot inject a widget into the OS-drawn native titlebar.
// Windows-only; built on every platform but only ever instantiated on Win32.
class McTitleBar : public QWidget
{
	Q_OBJECT
public:
	explicit McTitleBar(QWidget* parent = nullptr);

	void setTitleBarWindowIcon(const QIcon& icon);
	void setTitleText(const QString& title);
	void setMaximized(bool maximized);

	// False over the four buttons, true everywhere else in the bar — used by
	// McMainWindow's WM_NCHITTEST handler to decide HTCAPTION vs HTCLIENT.
	[[nodiscard]] bool isDragRegion(const QPoint& localPos) const;

	static constexpr int kPreferredHeight = 32;

signals:
	void minimizeToTrayRequested();
	void minimizeRequested();
	void maximizeRestoreRequested();
	void closeRequested();

private:
	QLabel*      m_iconLabel     = nullptr;
	QLabel*      m_titleLabel    = nullptr;
	QToolButton* m_btnTray       = nullptr;
	QToolButton* m_btnMinimize   = nullptr;
	QToolButton* m_btnMaxRestore = nullptr;
	QToolButton* m_btnClose      = nullptr;
	bool         m_maximized     = false;
};

} // namespace Mc
