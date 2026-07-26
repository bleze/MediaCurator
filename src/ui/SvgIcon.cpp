#include "ui/SvgIcon.h"

#include <QApplication>
#include <QFile>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QRegularExpression>
#include <QSvgRenderer>

namespace Mc {
namespace {

QString recolorSvg(const QString& svg, const QColor& color)
{
	// Find the opening <svg ...> tag and inject/replace the fill attribute.
	// Material Symbols paths carry no fill of their own so adding fill to the
	// root element cascades to all children.
	const int svgStart = svg.indexOf(QLatin1String("<svg"));
	const int tagEnd   = svg.indexOf(QLatin1Char('>'), svgStart);
	if (svgStart < 0 || tagEnd < 0) return svg;

	QString tag = svg.mid(svgStart, tagEnd - svgStart + 1);

	static const QRegularExpression fillAttr(R"(\s+fill="[^"]*")");
	tag.remove(fillAttr);
	tag.insert(4, QStringLiteral(R"( fill="%1")").arg(color.name()));

	return svg.left(svgStart) + tag + svg.mid(tagEnd + 1);
}

class SvgIconEngine final : public QIconEngine
{
	QString m_normal;
	QString m_disabled;

public:
	SvgIconEngine(QString normal, QString disabled)
		: m_normal(std::move(normal))
		, m_disabled(std::move(disabled))
	{}

	void paint(QPainter* p, const QRect& r,
	           QIcon::Mode mode, QIcon::State /*state*/) override
	{
		const QString& data = (mode == QIcon::Disabled) ? m_disabled : m_normal;
		QSvgRenderer(data.toUtf8()).render(p, QRectF(r));
	}

	QPixmap pixmap(const QSize& sz, QIcon::Mode mode, QIcon::State state) override
	{
		QPixmap px(sz);
		px.fill(Qt::transparent);
		QPainter p(&px);
		paint(&p, QRect(QPoint{}, sz), mode, state);
		return px;
	}

	QIconEngine* clone() const override
	{
		return new SvgIconEngine(m_normal, m_disabled);
	}

	QString key() const override { return QStringLiteral("Mc::SvgIconEngine"); }

	QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override
	{
		return {{16,16},{24,24},{32,32},{48,48}};
	}

	bool isNull() override { return m_normal.isEmpty(); }
};

} // namespace

QIcon svgIcon(const QString& resourcePath)
{
	QFile f(resourcePath);
	if (!f.open(QIODevice::ReadOnly)) return {};
	const QString svg = QString::fromUtf8(f.readAll());

	const QPalette pal = QApplication::palette();
	const QString normal   = recolorSvg(svg, pal.color(QPalette::Active,   QPalette::ButtonText));
	const QString disabled = recolorSvg(svg, pal.color(QPalette::Disabled, QPalette::ButtonText));

	return QIcon(new SvgIconEngine(normal, disabled));
}

QIcon svgIcon(const QString& resourcePath, const QColor& color)
{
	QFile f(resourcePath);
	if (!f.open(QIODevice::ReadOnly)) return {};
	const QString svg = QString::fromUtf8(f.readAll());

	const QString normal   = recolorSvg(svg, color);
	const QString disabled = recolorSvg(svg, color.lighter(150));

	return QIcon(new SvgIconEngine(normal, disabled));
}

} // namespace Mc
