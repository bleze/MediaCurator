#pragma once
#include <QImage>
#include <QList>
#include <QRect>
#include <QRgb>

namespace Mc {

enum class LogoExtractMode {
	// Keep pixels that are a blend of a picked logo colour and the background
	// (alpha = how far along that blend they are). Best for logos over busy art.
	KeepLogoColours,
	// Drop pixels close to a background colour. Best for logos on flat backgrounds.
	RemoveBackground,
};

struct LogoExtractParams {
	LogoExtractMode mode = LogoExtractMode::KeepLogoColours;
	QList<QRgb>     logoColours;
	QList<QRgb>     backgroundColours;     // user-picked, in addition to the auto ones
	bool            autoBackground = true; // sample background colours from the crop's edge
	int             tolerance      = 60;   // RGB distance treated as a full match
	int             softness       = 40;   // RGB distance over which a match fades out
	int             edgeShrink     = 0;    // px of alpha erosion (kills halos)
	int             minSpeckArea   = 0;    // opaque blobs with less mass than this are dropped; 0 = off
};

// Brush override values, one byte per crop pixel (Format_Grayscale8).
namespace LogoOverride {
constexpr uchar None  = 0;
constexpr uchar Keep  = 128;
constexpr uchar Erase = 255;
} // namespace LogoOverride

// Cuts a clear logo out of a crop of a poster/backdrop. Pure image maths, no UI.
class LogoExtractor {
public:
	// Takes the selected area of the source artwork. Recomputes the auto
	// background colours from its edge.
	void setSource(const QImage& crop);
	[[nodiscard]] const QImage& source() const { return m_src; }
	[[nodiscard]] const QList<QRgb>& autoBackgroundColours() const { return m_autoBg; }
	// The auto colours process() actually uses: in Keep logo colours mode,
	// ones the tolerance would also accept as a logo colour are left out.
	[[nodiscard]] QList<QRgb> usableAutoBackground(const LogoExtractParams& params) const;

	// Straight-alpha ARGB32 result, same size as the source crop. overrides is
	// either null or a Grayscale8 image of the same size (see LogoOverride).
	// Without colours to work from, returns the crop unchanged (fully opaque).
	[[nodiscard]] QImage process(const LogoExtractParams& params, const QImage& overrides) const;
	// The two halves of process(), so a brush stroke only redoes the overrides:
	// the colour-model result without any overrides...
	[[nodiscard]] QImage processBase(const LogoExtractParams& params) const;
	// ...and result = base with the overrides applied, inside rect only.
	void applyOverrides(QImage& result, const QImage& base, const QImage& overrides, const QRect& rect) const;

	// Smallest rect containing every pixel with alpha above threshold; null if none.
	[[nodiscard]] static QRect opaqueBounds(const QImage& img, int alphaThreshold = 8);
	// Crops img to its opaqueBounds plus padding px of transparent margin.
	[[nodiscard]] static QImage trimmed(const QImage& img, int padding);
	// Average colour of the (2r+1)^2 block around pos, clamped to the image.
	[[nodiscard]] static QRgb sampleColour(const QImage& img, QPoint pos, int r = 1);

private:
	QImage      m_src;  // Format_RGB32
	QList<QRgb> m_autoBg;
};

} // namespace Mc
