#include "engine/LogoExtractor.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Mc {

namespace {

struct Vec3 {
	float r = 0, g = 0, b = 0;
};

inline Vec3 toVec(QRgb c)
{
	return {float(qRed(c)), float(qGreen(c)), float(qBlue(c))};
}
inline Vec3  operator-(Vec3 a, Vec3 b) { return {a.r - b.r, a.g - b.g, a.b - b.b}; }
inline Vec3  operator+(Vec3 a, Vec3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
inline Vec3  operator*(Vec3 a, float s) { return {a.r * s, a.g * s, a.b * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }
inline float len(Vec3 a) { return std::sqrt(dot(a, a)); }

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// 0 at or below tol, 1 at tol + soft, linear in between.
inline float ramp(float d, float tol, float soft)
{
	return clamp01((d - tol) / std::max(soft, 1.f));
}

// Dominant colours of the crop's 2 px edge ring: a tiny k-means (k <= 3,
// farthest-point seeded); clusters under 5% of the ring or within 20 of a
// bigger one are dropped.
QList<QRgb> edgeColours(const QImage& img)
{
	const int w = img.width(), h = img.height();
	if (w < 4 || h < 4) return {};
	std::vector<Vec3> ring;
	ring.reserve(size_t(4 * (w + h)));
	for (int y = 0; y < h; ++y) {
		const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
		if (y < 2 || y >= h - 2) {
			for (int x = 0; x < w; ++x) ring.push_back(toVec(line[x]));
		} else {
			ring.push_back(toVec(line[0]));
			ring.push_back(toVec(line[1]));
			ring.push_back(toVec(line[w - 2]));
			ring.push_back(toVec(line[w - 1]));
		}
	}

	std::vector<Vec3> cent;
	Vec3 mean;
	for (const Vec3& v : ring) mean = mean + v;
	cent.push_back(mean * (1.f / float(ring.size())));
	while (cent.size() < 3) {
		float bestD = -1;
		Vec3  best;
		for (const Vec3& v : ring) {
			float d = 1e9f;
			for (const Vec3& c : cent) d = std::min(d, len(v - c));
			if (d > bestD) { bestD = d; best = v; }
		}
		if (bestD < 20.f) break;  // edge is essentially one colour
		cent.push_back(best);
	}

	std::vector<int> count(cent.size());
	for (int iter = 0; iter < 8; ++iter) {
		std::vector<Vec3> sum(cent.size());
		std::fill(count.begin(), count.end(), 0);
		for (const Vec3& v : ring) {
			size_t k = 0;
			float  dk = 1e9f;
			for (size_t i = 0; i < cent.size(); ++i) {
				const float d = len(v - cent[i]);
				if (d < dk) { dk = d; k = i; }
			}
			sum[k] = sum[k] + v;
			++count[k];
		}
		for (size_t i = 0; i < cent.size(); ++i)
			if (count[i] > 0) cent[i] = sum[i] * (1.f / float(count[i]));
	}

	std::vector<size_t> order(cent.size());
	for (size_t i = 0; i < order.size(); ++i) order[i] = i;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return count[a] > count[b]; });
	QList<QRgb> out;
	std::vector<Vec3> kept;
	for (size_t i : order) {
		if (count[i] < int(ring.size() / 20)) continue;
		bool dup = false;
		for (const Vec3& k : kept) dup = dup || len(k - cent[i]) < 20.f;
		if (dup) continue;
		kept.push_back(cent[i]);
		out << qRgb(qRound(cent[i].r), qRound(cent[i].g), qRound(cent[i].b));
	}
	return out;
}

// Zeroes alpha of every 8-connected blob (alpha > 12) whose total alpha mass
// is below minArea px.
// Raw pointers throughout: these loops touch every pixel several times, and
// checked std::vector indexing makes them crawl in debug builds.
void removeSpecks(std::vector<float>& alphaVec, int w, int h, int minArea)
{
	constexpr float kOn = 12.f / 255.f;
	float* const       alpha = alphaVec.data();
	const int          n     = w * h;
	std::vector<uchar> seenVec(size_t(n), 0);
	uchar* const       seen  = seenVec.data();
	std::vector<int>   stack;
	std::vector<int>   members;
	for (int start = 0; start < n; ++start) {
		if (seen[start] || alpha[start] <= kOn) continue;
		members.clear();
		stack.push_back(start);
		seen[start] = 1;
		float mass = 0;
		while (!stack.empty()) {
			const int p = stack.back();
			stack.pop_back();
			members.push_back(p);
			mass += alpha[p];
			const int px = p % w, py = p / w;
			const int y0 = std::max(0, py - 1), y1 = std::min(h - 1, py + 1);
			const int x0 = std::max(0, px - 1), x1 = std::min(w - 1, px + 1);
			for (int ny = y0; ny <= y1; ++ny)
				for (int nx = x0; nx <= x1; ++nx) {
					const int q = ny * w + nx;
					if (!seen[q] && alpha[q] > kOn) {
						seen[q] = 1;
						stack.push_back(q);
					}
				}
		}
		if (mass < float(minArea)) {
			const int* m = members.data();
			for (size_t i = 0, c = members.size(); i < c; ++i) alpha[m[i]] = 0.f;
		}
	}
}

// r passes of a 3x3 minimum filter (separable).
void erode(std::vector<float>& alphaVec, int w, int h, int r)
{
	std::vector<float> tmpVec(alphaVec.size());
	float* const alpha = alphaVec.data();
	float* const tmp   = tmpVec.data();
	for (int pass = 0; pass < r; ++pass) {
		for (int y = 0; y < h; ++y) {
			const float* a = alpha + y * w;
			float*       t = tmp + y * w;
			for (int x = 0; x < w; ++x) {
				float m = a[x];
				if (x > 0) m = std::min(m, a[x - 1]);
				if (x < w - 1) m = std::min(m, a[x + 1]);
				t[x] = m;
			}
		}
		for (int y = 0; y < h; ++y) {
			const float* t  = tmp + y * w;
			const float* up = y > 0 ? t - w : t;
			const float* dn = y < h - 1 ? t + w : t;
			float*       a  = alpha + y * w;
			for (int x = 0; x < w; ++x)
				a[x] = std::min(t[x], std::min(up[x], dn[x]));
		}
	}
}

} // namespace

void LogoExtractor::setSource(const QImage& crop)
{
	m_src    = crop.convertToFormat(QImage::Format_RGB32);
	m_autoBg = edgeColours(m_src);
}

QImage LogoExtractor::process(const LogoExtractParams& params, const QImage& overrides) const
{
	const QImage base = processBase(params);
	if (base.isNull()) return {};
	QImage out = base;
	applyOverrides(out, base, overrides, out.rect());
	return out;
}

void LogoExtractor::applyOverrides(QImage& result, const QImage& base, const QImage& overrides, const QRect& rect) const
{
	if (result.isNull() || overrides.isNull() || overrides.size() != result.size()
	    || overrides.format() != QImage::Format_Grayscale8 || base.size() != result.size()
	    || m_src.size() != result.size())
		return;
	const QRect r = rect.intersected(result.rect());
	for (int y = r.top(); y <= r.bottom(); ++y) {
		auto*        dst = reinterpret_cast<QRgb*>(result.scanLine(y));
		const auto*  bas = reinterpret_cast<const QRgb*>(base.constScanLine(y));
		const auto*  src = reinterpret_cast<const QRgb*>(m_src.constScanLine(y));
		const uchar* ov  = overrides.constScanLine(y);
		for (int x = r.left(); x <= r.right(); ++x) {
			if (ov[x] == LogoOverride::Erase)
				dst[x] = qRgba(0, 0, 0, 0);
			else if (ov[x] == LogoOverride::Keep)
				dst[x] = src[x] | 0xff000000u;
			else
				dst[x] = bas[x];
		}
	}
}

QList<QRgb> LogoExtractor::usableAutoBackground(const LogoExtractParams& params) const
{
	if (!params.autoBackground) return {};
	if (params.mode != LogoExtractMode::KeepLogoColours || params.logoColours.isEmpty()) return m_autoBg;
	// A box edge running through something logo-coloured (a white shirt next
	// to cream lettering) would otherwise make the logo colour "background"
	// and erase the logo. Anything the tolerance would accept as logo goes.
	const float limit = float(params.tolerance + params.softness);
	QList<QRgb> out;
	for (QRgb b : m_autoBg) {
		bool logoLike = false;
		for (QRgb l : params.logoColours)
			logoLike = logoLike || len(toVec(b) - toVec(l)) < limit;
		if (!logoLike) out << b;
	}
	return out;
}

QImage LogoExtractor::processBase(const LogoExtractParams& params) const
{
	if (m_src.isNull()) return {};
	const int w = m_src.width(), h = m_src.height();

	std::vector<Vec3> logo, bg;
	for (QRgb c : params.logoColours) logo.push_back(toVec(c));
	for (QRgb c : usableAutoBackground(params)) bg.push_back(toVec(c));
	for (QRgb c : params.backgroundColours) bg.push_back(toVec(c));

	// Gradients between two background colours (lit to shadowed wall, sky to
	// horizon) are background too, not a faint blend with the logo. Only
	// fairly close pairs: a line from black to white would swallow any grey.
	struct Segment { Vec3 a; Vec3 v; float vv; };
	std::vector<Segment> bgLines;
	for (size_t i = 0; i < bg.size(); ++i)
		for (size_t j = i + 1; j < bg.size(); ++j) {
			const Vec3 v = bg[j] - bg[i];
			const float vv = dot(v, v);
			if (vv > 1.f && vv <= 220.f * 220.f) bgLines.push_back({bg[i], v, vv});
		}
	const Segment* bgLineP = bgLines.data();
	const size_t   nBgLine = bgLines.size();

	const bool keepMode = params.mode == LogoExtractMode::KeepLogoColours;
	const bool haveModel = keepMode ? !logo.empty() : !bg.empty();

	const float tol  = float(params.tolerance);
	const float soft = float(params.softness);

	std::vector<float> alpha(size_t(w) * size_t(h), 1.f);
	std::vector<Vec3>  colour(alpha.size());
	float* const alphaP  = alpha.data();
	Vec3* const  colourP = colour.data();
	const Vec3*  logoP   = logo.data();
	const Vec3*  bgP     = bg.data();
	const size_t nLogo   = logo.size();
	const size_t nBg     = bg.size();

	for (int y = 0; y < h; ++y) {
		const auto* line = reinterpret_cast<const QRgb*>(m_src.constScanLine(y));
		for (int x = 0; x < w; ++x) {
			const size_t i = size_t(y * w + x);
			const Vec3   c = toVec(line[x]);
			colourP[i] = c;
			if (!haveModel) continue;

			float a = 1.f;
			Vec3  under;           // background colour the pixel is blended over
			bool  haveUnder = false;
			if (keepMode && !bg.empty()) {
				// Closest point on any background→logo colour line: t is the
				// blend amount (alpha), r how badly the pixel fits that blend.
				float bestR = 1e9f, bestT = 0.f;
				Vec3  bestV;
				for (size_t li = 0; li < nLogo; ++li)
					for (size_t bi = 0; bi < nBg; ++bi) {
						const Vec3& l  = logoP[li];
						const Vec3& b  = bgP[bi];
						const Vec3  v  = l - b;
						const float vv = std::max(dot(v, v), 1e-3f);
						const float t  = clamp01(dot(c - b, v) / vv);
						const float r  = len(c - (b + v * t));
						if (r < bestR) { bestR = r; bestT = t; bestV = v; }
					}
				for (size_t si = 0; si < nBgLine; ++si) {
					const Segment& sgm = bgLineP[si];
					const float t = clamp01(dot(c - sgm.a, sgm.v) / sgm.vv);
					const float r = len(c - (sgm.a + sgm.v * t));
					if (r < bestR) { bestR = r; bestT = 0.f; bestV = {}; }  // background blend
				}
				a = clamp01((bestT - 0.1f) / 0.8f) * (1.f - ramp(bestR, tol, soft));
				// Un-blend: move the pixel the rest of the way along its
				// background→logo line. Keeps its own texture (the off-line
				// residual) without amplifying it, so edges carry no fringe.
				if (bestT < 1.f) {
					const Vec3 f = c + bestV * (1.f - bestT);
					colourP[i] = {std::clamp(f.r, 0.f, 255.f), std::clamp(f.g, 0.f, 255.f),
					             std::clamp(f.b, 0.f, 255.f)};
				}
			} else if (keepMode) {
				float d = 1e9f;
				for (size_t li = 0; li < nLogo; ++li) d = std::min(d, len(c - logoP[li]));
				a = 1.f - ramp(d, tol, soft);
			} else {
				float d = 1e9f;
				for (size_t bi = 0; bi < nBg; ++bi) {
					const float db = len(c - bgP[bi]);
					if (db < d) { d = db; under = bgP[bi]; }
				}
				for (size_t si = 0; si < nBgLine; ++si) {
					const Segment& sgm = bgLineP[si];
					const Vec3  p  = sgm.a + sgm.v * clamp01(dot(c - sgm.a, sgm.v) / sgm.vv);
					const float db = len(c - p);
					if (db < d) { d = db; under = p; }
				}
				haveUnder = true;
				a = ramp(d, tol, soft);
			}
			alphaP[i] = a;
			// Un-blend the background out of semi-transparent edge pixels so
			// they don't carry a dark/light fringe onto other backgrounds.
			if (haveUnder && a > 0.f && a < 1.f) {
				const Vec3 f = (c - under * (1.f - a)) * (1.f / std::max(a, 0.1f));
				colourP[i] = {std::clamp(f.r, 0.f, 255.f), std::clamp(f.g, 0.f, 255.f),
				             std::clamp(f.b, 0.f, 255.f)};
			}
		}
	}

	if (haveModel) {
		if (params.minSpeckArea > 0) removeSpecks(alpha, w, h, params.minSpeckArea);
		if (params.edgeShrink > 0) erode(alpha, w, h, params.edgeShrink);
	}

	QImage out(w, h, QImage::Format_ARGB32);
	const Vec3*  col = colour.data();
	const float* alp = alpha.data();
	for (int y = 0; y < h; ++y) {
		auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
		for (int x = 0; x < w; ++x) {
			const int   i = y * w + x;
			const Vec3& c = col[i];
			dst[x] = qRgba(qRound(c.r), qRound(c.g), qRound(c.b), qRound(alp[i] * 255.f));
		}
	}
	return out;
}

QRect LogoExtractor::opaqueBounds(const QImage& img, int alphaThreshold)
{
	if (img.isNull()) return {};
	const QImage a = img.format() == QImage::Format_ARGB32 ? img : img.convertToFormat(QImage::Format_ARGB32);
	int minX = a.width(), minY = a.height(), maxX = -1, maxY = -1;
	for (int y = 0; y < a.height(); ++y) {
		const auto* line = reinterpret_cast<const QRgb*>(a.constScanLine(y));
		for (int x = 0; x < a.width(); ++x) {
			if (qAlpha(line[x]) > alphaThreshold) {
				minX = std::min(minX, x);
				maxX = std::max(maxX, x);
				minY = std::min(minY, y);
				maxY = std::max(maxY, y);
			}
		}
	}
	if (maxX < 0) return {};
	return QRect(QPoint(minX, minY), QPoint(maxX, maxY));
}

QImage LogoExtractor::trimmed(const QImage& img, int padding)
{
	const QRect b = opaqueBounds(img);
	if (b.isNull()) return {};
	QImage out(b.width() + 2 * padding, b.height() + 2 * padding, QImage::Format_ARGB32);
	out.fill(Qt::transparent);
	const QImage src = img.convertToFormat(QImage::Format_ARGB32);
	for (int y = 0; y < b.height(); ++y) {
		const auto* s = reinterpret_cast<const QRgb*>(src.constScanLine(b.top() + y)) + b.left();
		auto*       d = reinterpret_cast<QRgb*>(out.scanLine(padding + y)) + padding;
		std::copy(s, s + b.width(), d);
	}
	return out;
}

QRgb LogoExtractor::sampleColour(const QImage& img, QPoint pos, int r)
{
	if (img.isNull()) return 0;
	long long sr = 0, sg = 0, sb = 0, n = 0;
	for (int y = pos.y() - r; y <= pos.y() + r; ++y) {
		if (y < 0 || y >= img.height()) continue;
		for (int x = pos.x() - r; x <= pos.x() + r; ++x) {
			if (x < 0 || x >= img.width()) continue;
			const QRgb c = img.pixel(x, y);
			sr += qRed(c);
			sg += qGreen(c);
			sb += qBlue(c);
			++n;
		}
	}
	if (n == 0) return 0;
	return qRgb(int(sr / n), int(sg / n), int(sb / n));
}

} // namespace Mc
