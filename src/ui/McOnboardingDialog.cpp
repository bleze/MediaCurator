#include "ui/McOnboardingDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/SvgIcon.h"

#include <QApplication>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtMath>

namespace Mc {

namespace {

// Tinted functional icon (matches the "ButtonText/theme-aware" icons used elsewhere).
QLabel* iconLabel(QWidget* parent, const QString& resourcePath, int size)
{
	auto* lbl = new QLabel(parent);
	lbl->setPixmap(McCardDelegate::renderSvgIcon(resourcePath, parent->palette().color(QPalette::Highlight),
	                                             size, parent->devicePixelRatioF()));
	lbl->setAlignment(Qt::AlignHCenter);
	return lbl;
}

// The app icon has intentional brand colors — render it as-is, never tinted.
QLabel* appIconLabel(QWidget* parent, int size)
{
	auto* lbl = new QLabel(parent);
	lbl->setPixmap(qApp->windowIcon().pixmap(size, size));
	lbl->setAlignment(Qt::AlignHCenter);
	return lbl;
}

QLabel* sampleBadge(QWidget* parent, const QString& text, const QString& codecType,
                    const QString& flagLang = {})
{
	auto* lbl = new QLabel(parent);
	lbl->setPixmap(McCardDelegate::badgePixmap(text, codecType, parent->font(),
	                                           parent->devicePixelRatioF(), flagLang));
	return lbl;
}

// Static sample of the Job Queue card's live size bar (McCardDelegate), showing what a
// finished job with a given savings percentage looks like: green (mkvmerge progress,
// complete) fills the full width, blue (output bytes vs. original size) sits on top and
// stops short by the saved fraction, exposing a green sliver at the end.
QLabel* sizeBarSample(QWidget* parent, int width, int height, double savedFraction)
{
	const qreal dpr = parent->devicePixelRatioF();
	QPixmap pm(qCeil(width * dpr), qCeil(height * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	p.fillRect(QRect(0, 0, width, height), QColor(0x30, 0x30, 0x40));
	p.fillRect(QRect(0, 0, width, height), QColor(0x5a, 0xe8, 0x5a));
	const int keptW = qRound(width * (1.0 - savedFraction));
	p.fillRect(QRect(0, 0, keptW, height), QColor(0x64, 0xb4, 0xf0));
	p.end();

	auto* lbl = new QLabel(parent);
	lbl->setPixmap(pm);
	lbl->setFixedSize(width, height);
	return lbl;
}

// Renders the actual card group badge (McCardDelegate::drawGroupChip) standalone,
// so the onboarding preview matches the real library-card badge pixel for pixel.
QLabel* groupChip(QWidget* parent, int group)
{
	constexpr int h = 20;
	const QFont font = parent->font();
	const int w = McCardDelegate::groupChipWidth(group, font);

	const qreal dpr = parent->devicePixelRatioF();
	QPixmap pm(qCeil(w * dpr), qCeil(h * dpr));
	pm.setDevicePixelRatio(dpr);
	pm.fill(Qt::transparent);

	QPainter p(&pm);
	McCardDelegate::drawGroupChip(&p, 0, 0, h, group, font, dpr);
	p.end();

	auto* lbl = new QLabel(parent);
	lbl->setPixmap(pm);
	lbl->setFixedSize(w, h);
	return lbl;
}

// One onboarding page: icon, bold title, body text, and an optional row of extra widgets
// (e.g. sample badges) placed between the body and the bottom stretch.
QWidget* buildPage(QWidget* parent, QLabel* icon, const QString& title,
                   const QString& bodyHtml, const QList<QWidget*>& extras = {})
{
	auto* page = new QWidget(parent);
	auto* v = new QVBoxLayout(page);
	v->setContentsMargins(24, 20, 24, 8);
	v->setSpacing(12);

	v->addStretch(1);
	v->addWidget(icon);

	auto* titleLbl = new QLabel(title, page);
	QFont tf = titleLbl->font();
	tf.setPointSizeF(tf.pointSizeF() * 1.3);
	tf.setBold(true);
	titleLbl->setFont(tf);
	titleLbl->setAlignment(Qt::AlignHCenter);
	v->addWidget(titleLbl);

	auto* bodyLbl = new QLabel(bodyHtml, page);
	bodyLbl->setTextFormat(Qt::RichText);
	bodyLbl->setWordWrap(true);
	bodyLbl->setAlignment(Qt::AlignHCenter);
	v->addWidget(bodyLbl);

	if (!extras.isEmpty()) {
		auto* row = new QHBoxLayout;
		row->addStretch(1);
		for (QWidget* w : extras)
			row->addWidget(w);
		row->addStretch(1);
		v->addLayout(row);
	}

	v->addStretch(2);
	return page;
}

} // anonymous namespace

McOnboardingDialog::McOnboardingDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Welcome to MediaCurator"));
	setMinimumSize(460, 420);

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(0, 0, 0, 12);
	root->setSpacing(10);

	m_stack = new QStackedWidget(this);
	root->addWidget(m_stack, 1);

	m_stack->addWidget(buildPage(m_stack, appIconLabel(m_stack, 56),
		tr("Welcome to MediaCurator"),
		tr("MediaCurator scans your personal video library, reads every track in every file, "
		   "and finds the audio, subtitle, and codec tracks you don't need — then removes them "
		   "losslessly with mkvmerge, so picture and kept-audio quality never change."
		   "<br><br>Nothing is touched until you explicitly approve it — every scan and every "
		   "removal is preview-first.")));

	m_stack->addWidget(buildPage(m_stack, iconLabel(m_stack, QStringLiteral(":/icons/manage_search.svg"), 56),
		tr("Tell it what you care about"),
		tr("Open <b>Tools → Settings</b> to set the languages you understand (kept audio and "
		   "subtitle tracks), your preferred codec order, and how to handle commentary, SDH, "
		   "and forced subtitles. The movie's original-language audio track is always kept, "
		   "no matter what you choose."),
		{ sampleBadge(m_stack, QStringLiteral("H.265  3840" "\xC3\x97" "2160  DolbyVision"), QStringLiteral("video")),
		  sampleBadge(m_stack, QStringLiteral("Atmos  7.1"), QStringLiteral("audio"), QStringLiteral("eng")),
		  sampleBadge(m_stack, QStringLiteral("SRT"), QStringLiteral("subtitle"), QStringLiteral("eng")) }));

	m_stack->addWidget(buildPage(m_stack, iconLabel(m_stack, QStringLiteral(":/icons/playlist_add_check.svg"), 56),
		tr("Reclaim disk space"),
		tr("Use <b>File → Scan</b> to add a folder or refresh your library. Flagged tracks show "
		   "up struck-through on each card — see <b>View → Legend</b> for what every badge and "
		   "icon means. Run <b>Tools → Analyze</b> to preview the savings, then queue the job: "
		   "the Job Queue panel and status bar track how much space you've reclaimed."),
		{ sizeBarSample(m_stack, 220, 3, 0.10) }));

	m_stack->addWidget(buildPage(m_stack, iconLabel(m_stack, QStringLiteral(":/icons/refresh.svg"), 56),
		tr("Posters, fanart, and NFOs"),
		tr("Add a free TMDB API key in <b>Tools → Settings → Other</b> and MediaCurator will "
		   "match your movies automatically, download posters and fanart, and — if you turn it "
		   "on — write Kodi-style .nfo files with the correct title, original title, year, and "
		   "rating. Got the wrong match? Right-click a card to search and pick manually.")));

	m_stack->addWidget(buildPage(m_stack, iconLabel(m_stack, QStringLiteral(":/icons/translate.svg"), 56),
		tr("Missing subtitles? Downloaded"),
		tr("OpenSubtitles integration finds subtitle tracks in the languages you understand and "
		   "adds them automatically. Turn on <b>Settings → Subtitles → Automatically download "
		   "missing subtitles after scanning</b>, or right-click any file and choose <b>Download "
		   "Subtitles…</b> to fetch one on the spot. Works anonymously out of the box — add an "
		   "API key in the same tab for a higher daily quota.")));

	m_stack->addWidget(buildPage(m_stack, iconLabel(m_stack, QStringLiteral(":/icons/storage_group.svg"), 56),
		tr("Scan multiple drives in parallel"),
		tr("If your library spans more than one physical drive or NAS share, open <b>File → "
		   "Manage Library Folders…</b> and give each folder a storage group. Folders in "
		   "different groups scan and remux at the same time; folders sharing a group still run "
		   "one at a time so a single disk never gets saturated."),
		{ groupChip(m_stack, 1), groupChip(m_stack, 2), groupChip(m_stack, 3) }));

	auto* footer = new QHBoxLayout;
	footer->setContentsMargins(16, 0, 16, 0);

	m_stepLabel = new QLabel(this);
	footer->addWidget(m_stepLabel);
	footer->addStretch(1);

	auto* skipBtn = new QPushButton(tr("Skip"), this);
	connect(skipBtn, &QPushButton::clicked, this, &QDialog::accept);
	footer->addWidget(skipBtn);

	m_btnBack = new QPushButton(tr("< Back"), this);
	connect(m_btnBack, &QPushButton::clicked, this, [this] { goToPage(m_stack->currentIndex() - 1); });
	footer->addWidget(m_btnBack);

	m_btnNext = new QPushButton(this);
	m_btnNext->setDefault(true);
	connect(m_btnNext, &QPushButton::clicked, this, [this] {
		if (m_stack->currentIndex() + 1 >= m_pageCount)
			accept();
		else
			goToPage(m_stack->currentIndex() + 1);
	});
	footer->addWidget(m_btnNext);

	root->addLayout(footer);

	m_pageCount = m_stack->count();
	goToPage(0);
}

void McOnboardingDialog::goToPage(int index)
{
	m_stack->setCurrentIndex(index);
	m_stepLabel->setText(tr("Step %1 of %2").arg(index + 1).arg(m_pageCount));
	m_btnBack->setEnabled(index > 0);
	m_btnNext->setText(index + 1 >= m_pageCount ? tr("Get Started") : tr("Next >"));
}

} // namespace Mc
