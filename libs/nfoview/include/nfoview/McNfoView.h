#pragma once
#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>
#include <QSize>
#include <QString>
#include <QWidget>

class QAction;
class QNetworkAccessManager;
class QTextBrowser;

namespace Mc {

// Read-only NFO renderer: monospace, no wrapping, BBCode converted to markup.
// Shows the text immediately, then downloads any images the NFO links to
// (screenshots, group logos) and re-renders with the ones that arrive shown
// inline; an image that fails to load simply stays an "[image]" link.
class McNfoView : public QWidget
{
	Q_OBJECT
public:
	enum class Fit {
		Initial,   // size to the content and (re)position the window
		GrowOnly,  // content got bigger after showing: only ever grow, around the current center
	};

	// NFO art is drawn as ink on paper: a filled block is meant to be dark. So
	// the view never follows the application theme — on a dark one that would
	// show every NFO as a negative.
	enum class Colors {
		Paper,   // black on white, as the art was authored
		Night,   // light grey on black — inverted on purpose
	};

	explicit McNfoView(QWidget* parent = nullptr);
	~McNfoView() override;

	// Zoom, font, colors and shading as actions — the ones the view's own
	// right-click menu shows, for a host that also wants them in a menu bar.
	// What they change is saved per application (QSettings, "nfoViewer/...")
	// and restored by the constructor, so every view in an app looks the same.
	QList<QAction*> viewActions() const { return m_viewActions; }

	void setColors(Colors colors);
	Colors colors() const { return m_colors; }

	// Monospace family for the text. Block elements are painted geometrically
	// whatever the font, so this mostly changes the letters and the cell shape.
	void setFontFamily(const QString& family);
	QString fontFamily() const;

	// The shade characters (light/medium/dark) are dot patterns in the DOS font
	// NFO art is drawn in, and that texture is part of the look — the default.
	// Smooth replaces them with flat 25/50/75% tones.
	void setSmoothShades(bool smooth);
	bool smoothShades() const { return m_smoothShades; }

	// Already-decoded text (see NfoText::decodeBytes). Replaces whatever is shown
	// and restarts image loading.
	void setText(const QString& nfoText);
	QString text() const { return m_text; }

	// Sent with image requests. Set before setText().
	void setUserAgent(const QByteArray& userAgent) { m_userAgent = userAgent; }

	// Whether linked images are downloaded and shown inline. Takes effect immediately.
	void setInlineImagesEnabled(bool enabled);
	bool inlineImagesEnabled() const { return m_inlineImages; }

	// Downloaded images larger than this on either side are scaled down.
	void setMaxImageSide(int pixels) { m_maxImageSide = pixels; }

	// Natural (unwrapped) size of the rendered document.
	QSize contentSize() const;

	// Sizes the top-level window holding this view to the content, capped at the
	// screen minus a margin, past which the view's own scrollbars take over.
	// chrome is everything in the window that isn't document: frame, margins,
	// button row, menu bar. Does nothing in GrowOnly mode once the user has
	// resized the window themselves.
	void fitWindow(Fit mode, const QSize& chrome);

	QTextBrowser* browser() const { return m_browser; }

protected:
	bool eventFilter(QObject* watched, QEvent* event) override;

signals:
	// An inline image arrived and was rendered, so contentSize() may have grown.
	void contentGrew();

	// The font family or size changed through a view action, so contentSize() changed.
	void cellSizeChanged();

private:
	void render();
	void applyCellFormats();
	void fetchImages();
	void buildViewActions();
	void zoomTo(int pointSize);
	void showContextMenu(const QPoint& viewportPos);

	QString                m_text;
	QTextBrowser*          m_browser = nullptr;
	QNetworkAccessManager* m_net     = nullptr;
	QHash<QString, QImage> m_images;      // image URL → decoded (and downscaled) image
	QSet<QString>          m_loadedUrls;  // keys of m_images, as bbcodeToHtml wants them
	QByteArray             m_userAgent    = "nfoview/1.0";
	QSize                  m_autoSize;    // last window size fitWindow set; differs once the user resizes
	int                    m_maxImageSide = 1000;
	int                    m_generation   = 0;   // bumped per setText, so late replies for old text are dropped
	bool                   m_inlineImages = true;
	bool                   m_smoothShades = false;
	Colors                 m_colors       = Colors::Paper;
	QList<QAction*>        m_viewActions;
};

} // namespace Mc
