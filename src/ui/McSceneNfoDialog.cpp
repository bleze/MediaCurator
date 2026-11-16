#include "ui/McSceneNfoDialog.h"
#include "scanner/NfoParser.h"

#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScreen>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace Mc {

namespace {
constexpr int    kMaxImages     = 24;
constexpr qint64 kMaxImageBytes = 10 * 1024 * 1024;
constexpr int    kTimeoutMs     = 15000;
} // namespace

McSceneNfoDialog::McSceneNfoDialog(const QString& videoPath, const QString& nfoText, QWidget* parent)
	: QDialog(parent), m_text(nfoText)
{
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Scene NFO — %1").arg(QFileInfo(videoPath).completeBaseName()));

	auto* layout = new QVBoxLayout(this);
	m_view = new QTextBrowser(this);
	m_view->setOpenExternalLinks(true);
	m_view->setLineWrapMode(QTextEdit::NoWrap);
	QFont mono("Courier New", 9);
	mono.setStyleHint(QFont::Monospace);
	m_view->setFont(mono);
	layout->addWidget(m_view);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	const QScreen* scr = parent ? parent->screen() : screen();
	if (scr)
		m_maxImageSide = qBound(200, scr->availableGeometry().width() - 200, 1000);

	render();
	fitToContent(/*growOnly=*/false);
	fetchImages();
}

void McSceneNfoDialog::render()
{
	const int hPos = m_view->horizontalScrollBar()->value();
	const int vPos = m_view->verticalScrollBar()->value();

	// Registered before setHtml so the document resolves each <img src> from
	// memory — QTextBrowser itself never loads http(s) resources.
	for (auto it = m_images.cbegin(); it != m_images.cend(); ++it)
		m_view->document()->addResource(QTextDocument::ImageResource, QUrl(it.key()), it.value());

	// <pre> preserves whitespace/alignment for the art; bbcodeToHtml already
	// HTML-escaped everything else, so this is safe to feed straight in.
	m_view->setHtml(QStringLiteral("<pre>%1</pre>").arg(NfoParser::bbcodeToHtml(m_text, &m_loadedUrls)));

	m_view->horizontalScrollBar()->setValue(hPos);
	m_view->verticalScrollBar()->setValue(vPos);
}

void McSceneNfoDialog::fitToContent(bool growOnly)
{
	// Size to the actual content (so a short NFO gets a small, centered window
	// instead of always filling the screen) — capped at the screen size minus a
	// 50px margin on each edge, past which the QTextBrowser's own scrollbars
	// take over rather than growing the window off-screen.
	const QWidget* anchor = parentWidget() ? parentWidget()->window() : nullptr;
	const QScreen* scr    = anchor ? anchor->screen() : screen();
	if (!scr) return;
	const QSize maxSize = scr->availableGeometry().size() - QSize(100, 100);

	QTextDocument* doc = m_view->document();
	const qreal oldTextWidth = doc->textWidth();
	doc->setTextWidth(-1);   // unconstrained — measure the content's natural width
	const int idealW = qRound(doc->idealWidth());
	doc->setTextWidth(idealW);   // now measure the height that width needs
	const int idealH = qRound(doc->size().height());
	if (growOnly)
		doc->setTextWidth(oldTextWidth);

	// Padding for the QTextBrowser's own frame/margins plus the button row and
	// layout spacing below it — enough that content just barely fitting
	// doesn't still trigger a scrollbar.
	QSize target(qBound(400, idealW + 48, maxSize.width()),
	             qBound(200, idealH + 90, maxSize.height()));

	QPoint center;
	if (growOnly) {
		// Images arrived after the window was shown: only ever grow, and leave
		// the window alone entirely once the user has sized it themselves.
		if (size() != m_autoSize) return;
		target = target.expandedTo(size());
		if (target == size()) return;
		center = frameGeometry().center();
	} else {
		center = anchor ? anchor->frameGeometry().center() : scr->availableGeometry().center();
	}

	resize(target);
	m_autoSize = target;
	QPoint topLeft = center - QPoint(target.width() / 2, target.height() / 2);
	const QRect avail = scr->availableGeometry();
	topLeft.setX(qBound(avail.left(), topLeft.x(), qMax(avail.left(), avail.right() - target.width())));
	topLeft.setY(qBound(avail.top(), topLeft.y(), qMax(avail.top(), avail.bottom() - target.height())));
	move(topLeft);
}

void McSceneNfoDialog::fetchImages()
{
	const QStringList urls = NfoParser::sceneNfoImageUrls(m_text).mid(0, kMaxImages);
	if (urls.isEmpty()) return;

	// Parented to the dialog: closing it tears down the manager and aborts
	// whatever is still in flight.
	m_net = new QNetworkAccessManager(this);
	for (const QString& urlStr : urls) {
		const QUrl url(urlStr);
		if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")))
			continue;

		QNetworkRequest req(url);
		req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
		req.setTransferTimeout(kTimeoutMs);
		QNetworkReply* reply = m_net->get(req);

		connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
			if (received > kMaxImageBytes || total > kMaxImageBytes)
				reply->abort();
		});
		connect(reply, &QNetworkReply::finished, this, [this, reply, urlStr]() {
			reply->deleteLater();
			if (reply->error() != QNetworkReply::NoError) return;

			// Dead image hosts commonly answer with an HTML page instead of
			// the image — that just fails to decode and stays a link.
			QImage img;
			if (!img.loadFromData(reply->readAll()) || img.isNull()) return;
			if (img.width() > m_maxImageSide || img.height() > m_maxImageSide)
				img = img.scaled(m_maxImageSide, m_maxImageSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);

			m_images.insert(urlStr, img);
			m_loadedUrls.insert(urlStr);
			render();
			fitToContent(/*growOnly=*/true);
		});
	}
}

} // namespace Mc
