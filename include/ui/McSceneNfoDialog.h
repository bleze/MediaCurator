#pragma once
#include <QDialog>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QString>

class QNetworkAccessManager;
class QTextBrowser;

namespace Mc {

// Read-only viewer for a scene-release NFO's cached text (see
// DatabaseManager::sceneNfoText) — shared by the library and Job Queue cards.
// Shows the text immediately, then downloads any images the NFO links to
// (screenshots, group logos) and re-renders with the ones that arrive shown
// inline; an image that fails to load simply stays an "[image]" link.
class McSceneNfoDialog : public QDialog
{
	Q_OBJECT
public:
	explicit McSceneNfoDialog(const QString& videoPath, const QString& nfoText, QWidget* parent = nullptr);

private:
	void render();
	void fitToContent(bool growOnly);
	void fetchImages();

	QString                m_text;
	QTextBrowser*          m_view = nullptr;
	QNetworkAccessManager* m_net  = nullptr;
	QHash<QString, QImage> m_images;      // image URL → decoded (and downscaled) image
	QSet<QString>          m_loadedUrls;  // keys of m_images, as bbcodeToHtml wants them
	QSize                  m_autoSize;    // last size we set; differs once the user resizes
	int                    m_maxImageSide = 1000;
};

} // namespace Mc
