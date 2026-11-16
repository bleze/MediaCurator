#include "ui/McSceneNfoDialog.h"
#include "nfoview/McNfoView.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QScreen>
#include <QVBoxLayout>

namespace Mc {

namespace {
// Room for the view's frame/margins plus the button row and layout spacing
// below it — enough that content just barely fitting doesn't still trigger a
// scrollbar.
constexpr QSize kChrome(48, 90);
} // namespace

McSceneNfoDialog::McSceneNfoDialog(const QString& videoPath, const QString& nfoText, QWidget* parent)
	: QDialog(parent)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Scene NFO — %1").arg(QFileInfo(videoPath).completeBaseName()));

	auto* layout = new QVBoxLayout(this);
	m_view = new McNfoView(this);
	m_view->setUserAgent("MediaCurator/1.0");
	layout->addWidget(m_view);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	const QScreen* scr = parent ? parent->screen() : screen();
	if (scr)
		m_view->setMaxImageSide(qBound(200, scr->availableGeometry().width() - 200, 1000));

	m_view->setText(nfoText);
	m_view->fitWindow(McNfoView::Fit::Initial, kChrome);
	connect(m_view, &McNfoView::contentGrew, this,
	        [this]() { m_view->fitWindow(McNfoView::Fit::GrowOnly, kChrome); });
	connect(m_view, &McNfoView::cellSizeChanged, this,
	        [this]() { m_view->fitWindow(McNfoView::Fit::Initial, kChrome); });
}

} // namespace Mc
