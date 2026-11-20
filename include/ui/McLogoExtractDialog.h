#pragma once
#include "engine/LogoExtractor.h"

#include <QDialog>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QUrl>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QRadioButton;
class QSlider;
class QSplitter;
class QSpinBox;
class QTimer;
class QToolButton;

namespace Mc {

namespace LogoExtractUi {
class SourceView;
class ResultView;
} // namespace LogoExtractUi

// Interactive clear-logo cutter: drag a box around the title on a poster or
// backdrop, pick the logo's colours, touch up with a brush, then use the
// transparent PNG as the movie's clear logo and/or save it for a TMDB upload.
class McLogoExtractDialog : public QDialog {
	Q_OBJECT
public:
	// movieLabel names the window and the exported file, e.g. "La Bamba (1987)".
	McLogoExtractDialog(const QString& movieLabel, QNetworkAccessManager* nam, QWidget* parent = nullptr);
	~McLogoExtractDialog() override;

	// Downloads and shows full-resolution artwork (e.g. TMDB /t/p/original/...).
	void loadUrl(const QUrl& url);
	void loadFile(const QString& path);

	// Trimmed full-resolution logo; valid once the dialog was accepted.
	[[nodiscard]] QImage resultImage() const { return m_result; }

protected:
	void done(int result) override;

private:
	struct Snapshot {
		QList<QRgb> logo;
		QList<QRgb> bg;
		QRect       maskRect;   // brush stroke: the part of m_overrides it changed...
		QImage      maskPatch;  // ...and what was there before (null for colour edits)
	};

	void setSourceImage(const QImage& img);
	void setSelection(const QRect& sel);
	void scheduleRecompute();
	void recompute();
	// Size label + Save/Use enabled state, from the current result.
	void updateStatus();
	// m_overrides' slice under the box, at preview size.
	[[nodiscard]] QImage previewOverridesFromMask() const;
	[[nodiscard]] LogoExtractParams currentParams() const;
	[[nodiscard]] QImage finalImage() const;
	void pushUndo();
	void undo();
	void rebuildChips();
	void updateHint();
	void onPick(QPoint previewPos, bool shift);
	void onStroke(QPoint fromPreview, QPoint toPreview, int radiusPreview);
	void saveToFile();
	void useAsLogo();

	QString                 m_movieLabel;
	QNetworkAccessManager*  m_nam = nullptr;
	QPointer<QNetworkReply> m_reply;

	QImage        m_fullSource;
	QRect         m_selection;      // in m_fullSource coordinates
	QImage        m_fullCrop;
	QImage        m_overrides;      // Grayscale8, m_fullSource size (see LogoOverride) — survives box changes
	double        m_previewScale = 1.0;
	QImage        m_previewCrop;
	LogoExtractor m_previewExtractor;
	QImage        m_previewBase;     // colour-model result without overrides (see recompute)
	QImage        m_previewOverrides; // m_overrides at preview size, painted alongside it
	QList<QRgb>   m_logoColours;
	QList<QRgb>   m_bgColours;
	QList<Snapshot> m_undo;
	QImage        m_result;

	LogoExtractUi::SourceView* m_sourceView = nullptr;
	LogoExtractUi::ResultView* m_resultView = nullptr;
	QAbstractButton* m_modeKeep   = nullptr;
	QAbstractButton* m_modeRemove = nullptr;
	QHBoxLayout*  m_logoChips     = nullptr;
	QHBoxLayout*  m_bgChips       = nullptr;
	QLabel*       m_logoChipsLabel = nullptr;
	QCheckBox*    m_autoBg        = nullptr;
	QSlider*      m_tolerance     = nullptr;
	QSlider*      m_softness      = nullptr;
	QSpinBox*     m_edgeShrink    = nullptr;
	QSpinBox*     m_speckArea     = nullptr;
	QButtonGroup* m_tools         = nullptr;
	QSlider*      m_brushSize     = nullptr;
	QAbstractButton* m_showOriginal = nullptr;
	QButtonGroup* m_previewBgGroup = nullptr;
	QToolButton*  m_btnUndo       = nullptr;
	QLabel*       m_hint          = nullptr;
	QLabel*       m_sizeLabel     = nullptr;
	QPushButton*  m_btnSave       = nullptr;
	QPushButton*  m_btnUse        = nullptr;
	QTimer*       m_recomputeTimer = nullptr;
	QTimer*       m_selectionTimer = nullptr;
	QRect         m_pendingSelection;
	QImage        m_strokeBackup;   // m_overrides as the current stroke began
	QRect         m_strokeDirty;    // artwork pixels the current stroke touched
	bool          m_nearlyEmpty = false;  // the cut-out left (almost) nothing — see updateHint()
	QSplitter*    m_splitter      = nullptr;
};

} // namespace Mc
