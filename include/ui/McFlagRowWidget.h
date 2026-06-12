#pragma once
#include <QWidget>
#include <QPainter>
#include <QEnterEvent>
#include <QMouseEvent>
#include <functional>

// Context-menu row for a single flag toggle.
// Visual style matches McQueueToggle: hover fill + checked highlight pill + badge mini-pill.
// Not Q_OBJECT — uses std::function callback.
class McFlagRowWidget final : public QWidget
{
public:
	std::function<void(bool)> onToggled;

	McFlagRowWidget(const QString& badgeChar, const QColor& badgeColor,
	                const QString& label, bool checked,
	                QWidget* parent = nullptr)
	    : QWidget(parent)
	    , m_badgeChar(badgeChar), m_badgeColor(badgeColor)
	    , m_label(label), m_checked(checked)
	{
		setAttribute(Qt::WA_Hover);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	}

	void setChecked(bool on) { if (m_checked != on) { m_checked = on; update(); } }
	bool isChecked() const { return m_checked; }

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRect  r = rect();
		const QColor h = palette().color(QPalette::Highlight);

		// Full-row hover fill (same alpha as McQueueToggle)
		if (m_hovered)
			p.fillRect(r, h.lighter(175));

		// Checked highlight pill — inset 2px left/right, 1px top/bottom
		if (m_checked) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(h.red(), h.green(), h.blue(), m_hovered ? 180 : 140));
			p.drawRoundedRect(QRectF(r).adjusted(2.0, 1.0, -2.0, -1.0), 3.0, 3.0);
		}

		// Badge char — fixed-width column so all labels align; no pill background
		constexpr int kLeft  = 8;
		constexpr int kIconW = 20;
		constexpr int kGap   = 4;
		QFont bf = font();
		bf.setPointSizeF(bf.pointSizeF() * 0.88);
		p.setFont(bf);
		p.setPen(m_badgeColor);
		// ★ sits slightly low optically — nudge it up 1px
		const int yOff = (m_badgeChar == QStringLiteral("\xe2\x98\x85")) ? -1 : 0;
		p.drawText(QRect(kLeft, yOff, kIconW, r.height()), Qt::AlignCenter, m_badgeChar);

		// Label text
		p.setPen(palette().color(QPalette::Text));
		p.setFont(font());
		p.drawText(r.adjusted(kLeft + kIconW + kGap, 0, -kLeft, 0),
		           Qt::AlignLeft | Qt::AlignVCenter, m_label);
	}

	void enterEvent(QEnterEvent*) override { m_hovered = true;  update(); }
	void leaveEvent(QEvent*)      override { m_hovered = false; update(); }

	void mousePressEvent(QMouseEvent* e) override
	{
		if (e->button() == Qt::LeftButton) {
			m_checked = !m_checked;
			update();
			if (onToggled) onToggled(m_checked);
		}
	}

	QSize sizeHint() const override
	{
		return { 8 + 20 + 4 + fontMetrics().horizontalAdvance(m_label) + 8,
		         fontMetrics().height() + 8 };
	}

private:
	QString m_badgeChar;
	QColor  m_badgeColor;
	QString m_label;
	bool    m_checked = false;
	bool    m_hovered = false;
};
