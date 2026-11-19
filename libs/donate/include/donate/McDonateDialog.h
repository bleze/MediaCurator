#pragma once

#include <QDialog>
#include <QIcon>

namespace Mc {

// "Support <app>" dialog: PayPal and (preferred) Lightning columns, each with a
// QR code and a copyable address. copyIcon/openIcon are the caller's (so they
// match its icon theme); a null icon gives that button a text label instead.
class McDonateDialog : public QDialog
{
	Q_OBJECT
public:
	McDonateDialog(const QString& appName, const QString& intro, QWidget* parent = nullptr,
	               const QIcon& copyIcon = {}, const QIcon& openIcon = {});
};

} // namespace Mc
