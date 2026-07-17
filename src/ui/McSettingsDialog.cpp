#include "ui/McSettingsDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McLanguageFlags.h"
#include "core/AppSettings.h"
#include "core/StorageGroupSettings.h"
#include "core/UserProfile.h"
#include "engine/PosterManager.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtMath>

namespace Mc {

// ── Badge-styled tab bar ──────────────────────────────────────────────────────
// Draws each tab as a colored rectangle with 3px corner radius — matching the
// badge style used on cards. Selected tab is full color + bold;
// unselected is darker + dimmed text. Tabs touch the pane at the bottom to
// read as tabs rather than floating pills.
class McBadgeTabBar : public QTabBar
{
public:
	static constexpr int kHPad    = 12; // horizontal text padding inside tab
	static constexpr int kTabH    = 30; // total height of the tab bar row
	static constexpr int kTopGap  = 3;  // gap above the colored rectangle
	static constexpr int kSideGap = 2;  // half-gap between adjacent tabs

	explicit McBadgeTabBar(QWidget* parent = nullptr) : QTabBar(parent)
	{
		setExpanding(false);
		setDrawBase(false);
	}

	void setTabColor(int idx, const QColor& c) { m_colors[idx] = c; update(); }

protected:
	QSize tabSizeHint(int idx) const override
	{
		const QFontMetrics fm(font());
		const int w = fm.horizontalAdvance(tabText(idx)) + kHPad * 2;
		return { qMax(w, 56), kTabH };
	}

	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		for (int i = 0; i < count(); ++i) {
			const bool   sel  = (i == currentIndex());
			const QColor base = m_colors.value(i, QColor(0x55, 0x55, 0x65));
			const QColor bg   = sel ? base : base.darker(150);
			const QColor fg   = sel ? QColor(Qt::white) : QColor(185, 185, 185);

			// Badge rect: top gap + side gaps, no bottom gap so it sits on the pane.
			const QRect badge = tabRect(i).adjusted(kSideGap, kTopGap, -kSideGap, 0);
			p.setBrush(bg);
			p.setPen(Qt::NoPen);
			p.drawRoundedRect(badge, 3, 3); // 3px radius — matches card badges

			p.setPen(fg);
			if (sel) {
				QFont f = font();
				f.setBold(true);
				p.setFont(f);
			}
			p.drawText(badge, Qt::AlignCenter, tabText(i));
			if (sel)
				p.setFont(font());
		}
	}

private:
	QMap<int, QColor> m_colors;
};

// QTabWidget::setTabBar is protected — use a subclass to install the bar.
class McBadgeTabWidget : public QTabWidget
{
public:
	explicit McBadgeTabWidget(QWidget* parent = nullptr)
		: QTabWidget(parent), m_bar(new McBadgeTabBar(this))
	{
		setTabBar(m_bar);
	}

	void setTabColor(int idx, const QColor& c) { m_bar->setTabColor(idx, c); }

private:
	McBadgeTabBar* m_bar;
};

// ── ISO 639-2 language list ───────────────────────────────────────────────────
static const QList<QPair<QString,QString>> kKnownLanguages = {
	{"mul", "Multiple languages"},
	{"ara", "Arabic"},
	{"zho", "Chinese"},
	{"hrv", "Croatian"},
	{"ces", "Czech"},
	{"dan", "Danish"},
	{"nld", "Dutch"},
	{"eng", "English"},
	{"fin", "Finnish"},
	{"fra", "French"},
	{"deu", "German"},
	{"ell", "Greek"},
	{"heb", "Hebrew"},
	{"hun", "Hungarian"},
	{"ind", "Indonesian"},
	{"ita", "Italian"},
	{"jpn", "Japanese"},
	{"kor", "Korean"},
	{"nor", "Norwegian"},
	{"pol", "Polish"},
	{"por", "Portuguese"},
	{"ron", "Romanian"},
	{"rus", "Russian"},
	{"srp", "Serbian"},
	{"slk", "Slovak"},
	{"spa", "Spanish"},
	{"swe", "Swedish"},
	{"tha", "Thai"},
	{"tur", "Turkish"},
	{"ukr", "Ukrainian"},
	{"vie", "Vietnamese"},
};

static QString displayName(const QString& code)
{
	for (const auto& [c, n] : kKnownLanguages)
		if (c == code) return QStringLiteral("%1 — %2").arg(c, n);
	return code;
}

static QIcon langFlagIcon(const QString& code, qreal dpr)
{
	QPixmap pm = McLanguageFlags::flag(code, McCardDelegate::kFlagH, dpr);
	if (pm.isNull()) {
		pm = QPixmap(qCeil(McCardDelegate::kFlagW * dpr), qCeil(McCardDelegate::kFlagH * dpr));
		pm.setDevicePixelRatio(dpr);
		pm.fill(Qt::transparent);
	}
	return QIcon(pm);
}

static QString formatBadgeText(const QString& id)
{
	static const QHash<QString, QString> texts = {
		{"atmos",     "Atmos"},
		{"truehd",    "TrueHD"},
		{"dtsx",      "DTS:X"},
		{"dtshdma",   "DTS-HD MA"},
		{"dtshd_hra", "DTS-HD HRA"},
		{"flac",      "FLAC"},
		{"eac3",      "DD+"},
		{"dts",       "DTS"},
		{"ac3",       "DD"},
		{"aac",       "AAC"},
		{"mp3",       "MP3"},
		{"pgs",       "PGS"},
		{"vobsub",    "VobSub"},
		{"ass",       "ASS"},
		{"srt",       "SRT"},
		{"vtt",       "VTT"},
	};
	return texts.value(id, id.toUpper());
}

static QString formatDescription(const QString& id)
{
	static const QHash<QString, QString> texts = {
		{"atmos",     "Dolby Atmos — object-based, lossless"},
		{"truehd",    "Dolby TrueHD — lossless"},
		{"dtsx",      "DTS:X — object-based"},
		{"dtshdma",   "DTS-HD Master Audio — lossless"},
		{"dtshd_hra", "DTS-HD High Resolution"},
		{"flac",      "FLAC / PCM — lossless"},
		{"eac3",      "Dolby Digital Plus (E-AC-3)"},
		{"dts",       "DTS Digital Surround"},
		{"ac3",       "Dolby Digital (AC-3)"},
		{"aac",       "AAC"},
		{"mp3",       "MP3"},
		{"pgs",       "Blu-ray image subtitles"},
		{"vobsub",    "DVD image subtitles"},
		{"ass",       "Styled text (ASS / SSA)"},
		{"srt",       "Plain text (SubRip)"},
		{"vtt",       "Web text (WebVTT)"},
	};
	return texts.value(id, id);
}

// ── Constructor ───────────────────────────────────────────────────────────────
McSettingsDialog::McSettingsDialog(UserProfile* profile, QWidget* parent)
	: QDialog(parent), m_profile(profile)
{
	setWindowTitle(tr("Settings"));
	setMinimumSize(760, 701);

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	if (const QByteArray geo = s.value("settingsDialog/geometry").toByteArray(); !geo.isEmpty())
		restoreGeometry(geo);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(10, 10, 10, 10);

	// ── Tab widget ────────────────────────────────────────────────────────────
	auto* tabs = new McBadgeTabWidget(this);
	tabs->setStyleSheet(
		"QTabWidget::pane { border: 1px solid palette(mid); padding: 4px; }");
	root->addWidget(tabs, 1);

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 0 — Video
	// ═══════════════════════════════════════════════════════════════════════════
	auto* videoPage   = new QWidget;
	auto* videoPageLo = new QVBoxLayout(videoPage);
	videoPageLo->setSpacing(8);
	videoPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(videoPage, tr("Video"));
	tabs->setTabColor(0, QColor(0xa0, 0x50, 0x00));

	auto* videoGroup  = new QGroupBox(tr("Video Streams"), videoPage);
	auto* videoLayout = new QVBoxLayout(videoGroup);

	m_chkRemoveMjpeg = new QCheckBox(tr("Remove embedded cover-art streams (MJPEG, PNG)"), videoGroup);
	m_chkRemoveMjpeg->setToolTip(tr(
		"Some MKV files contain an MJPEG or PNG video stream used as embedded cover art or a thumbnail. "
		"These streams are not playable content and add unnecessary size. "
		"Enable to mark them for removal during Analyze."));
	m_chkRemoveMjpeg->setChecked(profile->removeMjpegCoverArt());
	videoLayout->addWidget(m_chkRemoveMjpeg);

	videoPageLo->addWidget(videoGroup);
	videoPageLo->addStretch();

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 1 — Audio
	// ═══════════════════════════════════════════════════════════════════════════
	auto* audioPage   = new QWidget;
	auto* audioPageLo = new QHBoxLayout(audioPage);
	audioPageLo->setSpacing(8);
	audioPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(audioPage, tr("Audio"));
	tabs->setTabColor(1, QColor(0x10, 0x6a, 0xc0));

	auto* audioLeft  = new QVBoxLayout;
	auto* audioRight = new QVBoxLayout;
	audioLeft->setSpacing(8);
	audioRight->setSpacing(8);
	audioPageLo->addLayout(audioLeft,  1);
	audioPageLo->addLayout(audioRight, 1);

	// Left: Audio behaviour options
	auto* audioOptGroup  = new QGroupBox(tr("Audio"), audioPage);
	auto* audioOptLayout = new QVBoxLayout(audioOptGroup);
	m_chkKeepOriginalAudio = new QCheckBox(tr("Always keep audio in the file's original language"), audioOptGroup);
	m_chkKeepCommentary    = new QCheckBox(tr("Keep commentary tracks if in an understood language"), audioOptGroup);
	m_chkStereoCommentary  = new QCheckBox(tr("Treat stereo as commentary when a surround track exists"), audioOptGroup);
	m_chkStereoCommentary->setToolTip(tr(
		"When a file has a surround (5.1+) audio track, any stereo track in the same language "
		"is assumed to be a commentary or secondary mix — even without a title or flag indicating it. "
		"The commentary keep/remove policy above still applies."));
	m_chkKeepOriginalAudio->setChecked(profile->alwaysKeepOriginalAudio());
	m_chkKeepCommentary->setChecked(profile->keepCommentaryIfUnderstood());
	m_chkStereoCommentary->setChecked(profile->stereoAsCommentaryHeuristic());
	audioOptLayout->addWidget(m_chkKeepOriginalAudio);
	audioOptLayout->addWidget(m_chkKeepCommentary);
	audioOptLayout->addWidget(m_chkStereoCommentary);
	audioLeft->addWidget(audioOptGroup);
	audioLeft->addStretch();

	// Right: Audio Format Priority
	auto* fmtGroup  = new QGroupBox(tr("Format Priority"), audioPage);
	auto* fmtLayout = new QVBoxLayout(fmtGroup);
	auto* fmtHint   = new QLabel(
		tr("Drag or use buttons to reorder. Topmost = most preferred when multiple "
		   "tracks exist in the same language. Uncheck formats your player cannot "
		   "decode — those are removed when a checked alternative is available."),
		fmtGroup);
	fmtHint->setWordWrap(true);
	fmtLayout->addWidget(fmtHint);

	m_audioFormatList = new QListWidget(fmtGroup);
	m_audioFormatList->setDragDropMode(QAbstractItemView::InternalMove);
	m_audioFormatList->setDefaultDropAction(Qt::MoveAction);
	m_audioFormatList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_audioFormatList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	const QStringList& order    = profile->audioFormatOrder();
	const QStringList& disabled = profile->disabledAudioFormats();
	int audioBadgeW = 0;
	for (const QString& id : order) {
		const QPixmap pm = McCardDelegate::badgePixmap(
		    formatBadgeText(id), QStringLiteral("audio"), font(), devicePixelRatioF());
		audioBadgeW = qMax(audioBadgeW, qCeil(pm.deviceIndependentSize().width()));
		auto* item = new QListWidgetItem(QIcon(pm), formatDescription(id), m_audioFormatList);
		item->setData(Qt::UserRole, id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(disabled.contains(id) ? Qt::Unchecked : Qt::Checked);
	}
	m_audioFormatList->setIconSize(QSize(audioBadgeW, McCardDelegate::kBadgeH));

	auto* fmtBtnRow = new QHBoxLayout;
	m_btnFormatUp   = new QPushButton(tr("▲  Up"),   fmtGroup);
	m_btnFormatDown = new QPushButton(tr("▼  Down"), fmtGroup);
	fmtBtnRow->addWidget(m_btnFormatUp);
	fmtBtnRow->addWidget(m_btnFormatDown);
	fmtBtnRow->addStretch();
	connect(m_btnFormatUp,   &QPushButton::clicked, this, &McSettingsDialog::onAudioFormatUp);
	connect(m_btnFormatDown, &QPushButton::clicked, this, &McSettingsDialog::onAudioFormatDown);
	fmtLayout->addWidget(m_audioFormatList);
	fmtLayout->addLayout(fmtBtnRow);
	fmtGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	audioRight->addWidget(fmtGroup, 1);

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 2 — Subtitles
	// ═══════════════════════════════════════════════════════════════════════════
	auto* subPage   = new QWidget;
	auto* subPageLo = new QHBoxLayout(subPage);
	subPageLo->setSpacing(8);
	subPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(subPage, tr("Subtitles"));
	tabs->setTabColor(2, QColor(0x1a, 0x86, 0x4a));

	auto* subLeft  = new QVBoxLayout;
	auto* subRight = new QVBoxLayout;
	subLeft->setSpacing(8);
	subRight->setSpacing(8);
	subPageLo->addLayout(subLeft,  1);
	subPageLo->addLayout(subRight, 1);

	// Left top: Subtitle behaviour options
	auto* subOptGroup  = new QGroupBox(tr("Subtitles"), subPage);
	auto* subOptLayout = new QVBoxLayout(subOptGroup);
	m_chkKeepForced      = new QCheckBox(tr("Always keep forced subtitle tracks"), subOptGroup);
	m_chkKeepOriginalSub = new QCheckBox(tr("Keep original-language subtitle even if not understood"), subOptGroup);
	m_chkMergeSidecarSubs = new QCheckBox(tr("Merge sidecar subtitles into the container when remuxing"), subOptGroup);
	m_chkDetectSubLanguage = new QCheckBox(
		tr("Detect language of unlabeled sidecar subtitles and rename the file"), subOptGroup);
	m_chkKeepForced->setChecked(profile->keepForcedSubtitlesAlways());
	m_chkKeepOriginalSub->setChecked(profile->keepOriginalLanguageSubtitle());
	m_chkMergeSidecarSubs->setChecked(profile->mergeSidecarSubtitles());
	m_chkMergeSidecarSubs->setToolTip(tr(
		"When a file is remuxed for another reason, also absorb any external .srt/.ass/.vtt "
		"sidecar subtitles into the output. Disable to leave sidecar files untouched on disk."));
	m_chkDetectSubLanguage->setChecked(profile->detectSidecarSubtitleLanguage());
	m_chkDetectSubLanguage->setToolTip(tr(
		"When a sidecar .srt/.ass/.ssa/.vtt file's name carries no language code, sample its "
		"dialogue text to detect the language and rename the file to include it. Only renames "
		"on a high-confidence read. Off by default since this renames files on disk."));

	auto* sdhRow   = new QHBoxLayout;
	auto* sdhLabel = new QLabel(tr("SDH / hearing-impaired:"), subOptGroup);
	m_cmbSdhMode   = new QComboBox(subOptGroup);
	m_cmbSdhMode->addItem(tr("Always keep"),                          0);
	m_cmbSdhMode->addItem(tr("Always remove"),                        1);
	m_cmbSdhMode->addItem(tr("Remove SDH if a regular track exists"), 2);
	m_cmbSdhMode->addItem(tr("Prefer SDH, remove regular track"),     3);
	m_cmbSdhMode->setCurrentIndex(static_cast<int>(profile->sdhSubtitleMode()));
	m_cmbSdhMode->setToolTip(tr(
		"Controls how SDH (Subtitles for the Deaf and Hard of hearing) tracks are handled.\n"
		"\"Remove SDH if a regular track exists\" keeps SDH only as a fallback — recommended default.\n"
		"\"Prefer SDH\" is useful if you want sound-effects-as-text included."));
	sdhRow->addWidget(sdhLabel);
	sdhRow->addWidget(m_cmbSdhMode, 1);

	subOptLayout->addWidget(m_chkKeepForced);
	subOptLayout->addLayout(sdhRow);
	subOptLayout->addWidget(m_chkKeepOriginalSub);
	subOptLayout->addWidget(m_chkMergeSidecarSubs);
	subOptLayout->addWidget(m_chkDetectSubLanguage);
	subLeft->addWidget(subOptGroup);

	// Right: OpenSubtitles
	auto* osGroup  = new QGroupBox(tr("OpenSubtitles"), subPage);
	auto* osLayout = new QVBoxLayout(osGroup);

	auto* osApiRow   = new QHBoxLayout;
	auto* osApiLabel = new QLabel(tr("API Key:"), osGroup);
	m_editOsApiKey   = new QLineEdit(osGroup);
	m_editOsApiKey->setEchoMode(QLineEdit::Password);
	m_editOsApiKey->setPlaceholderText(tr("OpenSubtitles.com API key"));
	m_editOsApiKey->setText(profile->openSubtitlesApiKey());
	osApiRow->addWidget(osApiLabel);
	osApiRow->addWidget(m_editOsApiKey, 1);
	osLayout->addLayout(osApiRow);

	auto* osUserRow   = new QHBoxLayout;
	auto* osUserLabel = new QLabel(tr("Username:"), osGroup);
	m_editOsUsername  = new QLineEdit(osGroup);
	m_editOsUsername->setPlaceholderText(tr("Optional — leave empty for anonymous downloads"));
	m_editOsUsername->setText(profile->openSubtitlesUsername());
	osUserRow->addWidget(osUserLabel);
	osUserRow->addWidget(m_editOsUsername, 1);
	osLayout->addLayout(osUserRow);

	auto* osPassRow   = new QHBoxLayout;
	auto* osPassLabel = new QLabel(tr("Password:"), osGroup);
	m_editOsPassword  = new QLineEdit(osGroup);
	m_editOsPassword->setEchoMode(QLineEdit::Password);
	m_editOsPassword->setPlaceholderText(tr("Optional"));
	m_editOsPassword->setText(profile->openSubtitlesPassword());
	osPassRow->addWidget(osPassLabel);
	osPassRow->addWidget(m_editOsPassword, 1);
	osLayout->addLayout(osPassRow);

	m_chkAutoDownloadSubs = new QCheckBox(
		tr("Automatically download missing subtitles after scanning"), osGroup);
	m_chkAutoDownloadSubs->setChecked(profile->autoDownloadSubtitles());
	m_chkAutoDownloadSubs->setToolTip(
		tr("Downloads subtitles for your understood languages in the background as\n"
		   "files are scanned, the same way posters/fanart are fetched automatically.\n"
		   "Leave off if you'd rather manage your daily OpenSubtitles quota manually."));
	osLayout->addWidget(m_chkAutoDownloadSubs);

	m_chkComputeMovieHash = new QCheckBox(
		tr("Also send an exact-file hash (moviehash) when searching"), osGroup);
	m_chkComputeMovieHash->setChecked(profile->computeSubtitleMovieHash());
	m_chkComputeMovieHash->setToolTip(tr(
		"Reads the first and last 64 KB of each file to compute OpenSubtitles' own file hash, "
		"which — when it matches — guarantees the exact right subtitle. Only matches an\n"
		"unmodified original release rip, so it's useless for files you've remuxed/edited "
		"yourself, and reading every file up front slows down batch downloads. Off by default."));
	osLayout->addWidget(m_chkComputeMovieHash);

	auto* subRetryRow   = new QHBoxLayout;
	auto* subRetryLabel = new QLabel(tr("Retry cooldown (days):"), osGroup);
	m_spinSubtitleRetryCooldown = new QSpinBox(osGroup);
	m_spinSubtitleRetryCooldown->setRange(0, 365);
	m_spinSubtitleRetryCooldown->setSpecialValueText(tr("Off (always retry)"));
	m_spinSubtitleRetryCooldown->setValue(profile->subtitleRetryCooldownDays());
	m_spinSubtitleRetryCooldown->setToolTip(tr(
		"How long to wait before re-searching a file that's still missing a subtitle after "
		"a previous attempt found nothing. Without this, every scan re-searches the same\n"
		"files OpenSubtitles has no match for. 0 disables the cooldown."));
	subRetryRow->addWidget(subRetryLabel);
	subRetryRow->addWidget(m_spinSubtitleRetryCooldown);
	subRetryRow->addStretch();
	osLayout->addLayout(subRetryRow);

	auto* editionLabel = new QLabel(tr("Edition/cut tags:"), osGroup);
	editionLabel->setToolTip(tr(
		"Words that mark a distinct cut of a film (\"EXTENDED\", \"DIRECTORS CUT\", ...). Used to "
		"penalize a subtitle whose release name claims a different edition than your filename.\n"
		"Not exhaustive — release naming isn't standardized — add any tag you run into."));
	osLayout->addWidget(editionLabel);

	m_editionTokenList = new QListWidget(osGroup);
	m_editionTokenList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_editionTokenList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	for (const QString& tok : profile->editionTokens())
		new QListWidgetItem(tok, m_editionTokenList);
	osLayout->addWidget(m_editionTokenList, 1);

	auto* editionRow = new QHBoxLayout;
	m_editEditionToken = new QLineEdit(osGroup);
	m_editEditionToken->setPlaceholderText(tr("e.g. \"open matte\""));
	auto* editionAddBtn    = new QPushButton(tr("Add"),    osGroup);
	auto* editionRemoveBtn = new QPushButton(tr("Remove"), osGroup);
	auto* editionResetBtn  = new QPushButton(tr("Reset to Defaults"), osGroup);
	editionRow->addWidget(m_editEditionToken, 1);
	editionRow->addWidget(editionAddBtn);
	editionRow->addWidget(editionRemoveBtn);
	editionRow->addWidget(editionResetBtn);
	osLayout->addLayout(editionRow);
	connect(editionAddBtn,    &QPushButton::clicked, this, &McSettingsDialog::onAddEditionToken);
	connect(editionRemoveBtn, &QPushButton::clicked, this, &McSettingsDialog::onRemoveEditionToken);
	connect(editionResetBtn,  &QPushButton::clicked, this, &McSettingsDialog::onResetEditionTokens);
	connect(m_editEditionToken, &QLineEdit::returnPressed, this, &McSettingsDialog::onAddEditionToken);

	auto* osHint = new QLabel(
		tr("Without credentials, up to 100 anonymous downloads per day are available. "
		   "Add your account for a larger personal quota.\n"
		   "Get a free account at <a href=\"https://www.opensubtitles.com\">opensubtitles.com</a>."),
		osGroup);
	osHint->setTextFormat(Qt::RichText);
	osHint->setOpenExternalLinks(true);
	osHint->setWordWrap(true);
	osLayout->addWidget(osHint);
	osGroup->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
	subRight->addWidget(osGroup, 1);

	// Left bottom: Subtitle Format Priority
	auto* subFmtGroup  = new QGroupBox(tr("Format Priority"), subPage);
	auto* subFmtLayout = new QVBoxLayout(subFmtGroup);
	auto* subFmtHint   = new QLabel(
		tr("When multiple formats exist for the same language and type (regular / SDH / forced), "
		   "only the topmost enabled format is kept. Uncheck formats you never want."),
		subFmtGroup);
	subFmtHint->setWordWrap(true);
	subFmtLayout->addWidget(subFmtHint);

	m_subFormatList = new QListWidget(subFmtGroup);
	m_subFormatList->setDragDropMode(QAbstractItemView::InternalMove);
	m_subFormatList->setDefaultDropAction(Qt::MoveAction);
	m_subFormatList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_subFormatList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

	const QStringList& subOrder    = profile->subtitleFormatOrder();
	const QStringList& subDisabled = profile->disabledSubtitleFormats();
	int subBadgeW = 0;
	for (const QString& id : subOrder) {
		const QPixmap pm = McCardDelegate::badgePixmap(
		    formatBadgeText(id), QStringLiteral("subtitle"), font(), devicePixelRatioF());
		subBadgeW = qMax(subBadgeW, qCeil(pm.deviceIndependentSize().width()));
		auto* item = new QListWidgetItem(QIcon(pm), formatDescription(id), m_subFormatList);
		item->setData(Qt::UserRole, id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(subDisabled.contains(id) ? Qt::Unchecked : Qt::Checked);
	}
	m_subFormatList->setIconSize(QSize(subBadgeW, McCardDelegate::kBadgeH));

	auto* subBtnRow  = new QHBoxLayout;
	m_btnSubFmtUp   = new QPushButton(tr("▲  Up"),   subFmtGroup);
	m_btnSubFmtDown = new QPushButton(tr("▼  Down"), subFmtGroup);
	subBtnRow->addWidget(m_btnSubFmtUp);
	subBtnRow->addWidget(m_btnSubFmtDown);
	subBtnRow->addStretch();
	connect(m_btnSubFmtUp,   &QPushButton::clicked, this, &McSettingsDialog::onSubFmtUp);
	connect(m_btnSubFmtDown, &QPushButton::clicked, this, &McSettingsDialog::onSubFmtDown);
	subFmtLayout->addWidget(m_subFormatList);
	subFmtLayout->addLayout(subBtnRow);
	subFmtGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	subLeft->addWidget(subFmtGroup, 1);

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 3 — Performance
	// ═══════════════════════════════════════════════════════════════════════════
	auto* perfPage   = new QWidget;
	auto* perfPageLo = new QVBoxLayout(perfPage);
	perfPageLo->setSpacing(8);
	perfPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(perfPage, tr("Performance"));
	tabs->setTabColor(3, QColor(0xc0, 0x20, 0x20));

	auto* perfGroup  = new QGroupBox(tr("Performance"), perfPage);
	auto* perfLayout = new QVBoxLayout(perfGroup);

	auto* scanGroupsRow   = new QHBoxLayout;
	auto* scanGroupsLabel = new QLabel(tr("Storage groups shown:"), perfGroup);
	m_spinScanGroups      = new QSpinBox(perfGroup);
	m_spinScanGroups->setRange(2, StorageGroupSettings::MaxGroup);
	m_spinScanGroups->setValue(StorageGroupSettings::uiMaxGroup());
	m_spinScanGroups->setToolTip(tr(
		"How many storage groups appear in Manage Folders. Group folders on the same "
		"drive or NAS together; different groups can scan and remux in parallel."));
	scanGroupsRow->addWidget(scanGroupsLabel);
	scanGroupsRow->addWidget(m_spinScanGroups);
	scanGroupsRow->addStretch();
	perfLayout->addLayout(scanGroupsRow);

	auto* posterWorkersRow   = new QHBoxLayout;
	auto* posterWorkersLabel = new QLabel(tr("TMDB poster workers:"), perfGroup);
	m_spinPosterWorkers      = new QSpinBox(perfGroup);
	m_spinPosterWorkers->setRange(1, 12);
	m_spinPosterWorkers->setValue(
	    AppSettings::instance().value(QStringLiteral("poster/parallelWorkers"), 4).toInt());
	m_spinPosterWorkers->setToolTip(tr(
		"Number of parallel background threads for TMDB poster and fanart downloads. "
		"Higher values speed up large libraries but use more network bandwidth."));
	posterWorkersRow->addWidget(posterWorkersLabel);
	posterWorkersRow->addWidget(m_spinPosterWorkers);
	posterWorkersRow->addStretch();
	perfLayout->addLayout(posterWorkersRow);

	perfPageLo->addWidget(perfGroup);

	auto* stagingGroup  = new QGroupBox(tr("Local Staging"), perfPage);
	auto* stagingLayout = new QVBoxLayout(stagingGroup);

	m_chkUseLocalStaging = new QCheckBox(tr("Mux to a local folder, then copy the result back"), stagingGroup);
	m_chkUseLocalStaging->setToolTip(tr(
		"When the source file lives on a network share, reading and writing to it at the "
		"same time can slow both down. Enabling this writes the muxed output to a local "
		"folder first, then copies the finished file back over the network as a separate "
		"step. Requires enough free space in the local folder to hold the output file — "
		"falls back to muxing in place when there isn't enough room."));
	m_chkUseLocalStaging->setChecked(profile->useLocalStaging());
	stagingLayout->addWidget(m_chkUseLocalStaging);

	auto* stagingDirRow = new QHBoxLayout();
	m_editStagingDir = new QLineEdit(profile->localStagingDir(), stagingGroup);
	m_editStagingDir->setPlaceholderText(tr("Local staging folder…"));
	m_btnBrowseStagingDir = new QPushButton(tr("Browse…"), stagingGroup);
	stagingDirRow->addWidget(m_editStagingDir);
	stagingDirRow->addWidget(m_btnBrowseStagingDir);
	stagingLayout->addLayout(stagingDirRow);

	m_editStagingDir->setEnabled(m_chkUseLocalStaging->isChecked());
	m_btnBrowseStagingDir->setEnabled(m_chkUseLocalStaging->isChecked());
	connect(m_chkUseLocalStaging, &QCheckBox::toggled, m_editStagingDir, &QLineEdit::setEnabled);
	connect(m_chkUseLocalStaging, &QCheckBox::toggled, m_btnBrowseStagingDir, &QPushButton::setEnabled);
	connect(m_btnBrowseStagingDir, &QPushButton::clicked, this, &McSettingsDialog::onBrowseStagingDir);

	perfPageLo->addWidget(stagingGroup);
	perfPageLo->addStretch();

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 4 — Other
	// ═══════════════════════════════════════════════════════════════════════════
	auto* genPage   = new QWidget;
	auto* genPageLo = new QVBoxLayout(genPage);
	genPageLo->setSpacing(8);
	genPageLo->setContentsMargins(8, 8, 8, 8);

	// Scrollable: this tab has grown to five stacked groups and counting — without
	// this, every new setting added here permanently raises the whole dialog's
	// forced minimum height instead of just needing a scroll within the tab.
	auto* genScroll = new QScrollArea;
	genScroll->setWidget(genPage);
	genScroll->setWidgetResizable(true);
	genScroll->setFrameShape(QFrame::NoFrame);
	tabs->addTab(genScroll, tr("Other"));
	tabs->setTabColor(4, QColor(0x55, 0x55, 0x65));

	// Understood Languages
	auto* langGroup  = new QGroupBox(tr("Understood Languages"), genPage);
	auto* langLayout = new QVBoxLayout(langGroup);
	langLayout->setContentsMargins(4, 4, 4, 4);
	langLayout->setSpacing(4);

	auto* langHint = new QLabel(
		tr("Drag or use buttons to reorder. Topmost is used as the preferred TMDB "
		   "display-title language when writing .nfo files."),
		langGroup);
	langHint->setWordWrap(true);
	langLayout->addWidget(langHint);

	m_langList = new QListWidget(langGroup);
	m_langList->setDragDropMode(QAbstractItemView::InternalMove);
	m_langList->setDefaultDropAction(Qt::MoveAction);
	m_langList->setSelectionMode(QAbstractItemView::SingleSelection);
	// Fixed-ish, not Expanding: "Other" packs five groups into one tab (unlike
	// Audio/Subtitles, which each devote most of their tab to one such list), so
	// letting this one claim leftover vertical space blew up the whole tab's —
	// and therefore the dialog's — minimum height.
	m_langList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	m_langList->setMaximumHeight(120);
	for (const QString& code : profile->understoodLanguages()) {
		auto* item = new QListWidgetItem(langFlagIcon(code, devicePixelRatioF()),
		                                 displayName(code), m_langList);
		item->setData(Qt::UserRole, code);
	}
	langLayout->addWidget(m_langList, 1);

	auto* langBtnRow = new QHBoxLayout;
	m_btnLangUp      = new QPushButton(tr("▲  Up"),   langGroup);
	m_btnLangDown    = new QPushButton(tr("▼  Down"), langGroup);
	langBtnRow->addWidget(m_btnLangUp);
	langBtnRow->addWidget(m_btnLangDown);
	langBtnRow->addStretch();
	connect(m_btnLangUp,   &QPushButton::clicked, this, &McSettingsDialog::onLanguageUp);
	connect(m_btnLangDown, &QPushButton::clicked, this, &McSettingsDialog::onLanguageDown);
	langLayout->addLayout(langBtnRow);

	auto* addRow = new QHBoxLayout;
	m_langCombo  = new QComboBox(langGroup);
	for (const auto& [code, name] : kKnownLanguages)
		m_langCombo->addItem(langFlagIcon(code, devicePixelRatioF()),
		                     QStringLiteral("%1 — %2").arg(code, name), code);
	m_langCombo->setEditable(true);
	m_langCombo->setInsertPolicy(QComboBox::NoInsert);
	m_langCombo->lineEdit()->setPlaceholderText(tr("ISO 639-2 code (e.g. eng, dan)"));
	auto* addBtn    = new QPushButton(tr("Add"),    langGroup);
	auto* removeBtn = new QPushButton(tr("Remove"), langGroup);
	addRow->addWidget(m_langCombo, 1);
	addRow->addWidget(addBtn);
	addRow->addWidget(removeBtn);
	langLayout->addLayout(addRow);
	connect(addBtn,    &QPushButton::clicked, this, &McSettingsDialog::onAddLanguage);
	connect(removeBtn, &QPushButton::clicked, this, &McSettingsDialog::onRemoveLanguage);
	genPageLo->addWidget(langGroup);

	// The Movie Database (TMDB)
	auto* enrichGroup  = new QGroupBox(tr("The Movie Database (TMDB)"), genPage);
	auto* enrichLayout = new QVBoxLayout(enrichGroup);

	auto* tmdbRow   = new QHBoxLayout;
	auto* tmdbLabel = new QLabel(tr("API Key:"), enrichGroup);
	m_editTmdbKey   = new QLineEdit(enrichGroup);
	m_editTmdbKey->setEchoMode(QLineEdit::Password);
	m_editTmdbKey->setPlaceholderText(tr("Leave empty to skip poster lookup"));
	m_editTmdbKey->setText(profile->tmdbApiKey());
	tmdbRow->addWidget(tmdbLabel);
	tmdbRow->addWidget(m_editTmdbKey, 1);
	enrichLayout->addLayout(tmdbRow);

	m_chkWriteNfo = new QCheckBox(tr("Write .nfo files"), enrichGroup);
	m_chkWriteNfo->setToolTip(tr(
		"Writes a Kodi-style .nfo file next to each matched video, containing its IMDb and "
		"TMDB ids, title, original title, year, and TMDB rating. Title is picked from your "
		"Understood Languages list, above — the first one TMDB actually has a translation "
		"for; original title is always included too, so Kodi's own \"Show original titles "
		"for movies\" setting can prefer it on this machine without affecting other viewers "
		"of a shared library. If an .nfo already exists, only these specific tags are added "
		"or updated — everything else in the file is left untouched, and a scene-release "
		"NFO's free-form text only ever has its id corrected in place, never replaced. "
		"Off by default."));
	m_chkWriteNfo->setChecked(profile->writeNfoFiles());
	enrichLayout->addWidget(m_chkWriteNfo);

	auto* tmdbHint = new QLabel(
		tr("Get a free key at <a href=\"https://www.themoviedb.org/settings/api\">themoviedb.org</a>."),
		enrichGroup);
	tmdbHint->setTextFormat(Qt::RichText);
	tmdbHint->setOpenExternalLinks(true);
	enrichLayout->addWidget(tmdbHint);
	genPageLo->addWidget(enrichGroup);

	// Analysis
	auto* analysisGroup  = new QGroupBox(tr("Analysis"), genPage);
	auto* analysisLayout = new QVBoxLayout(analysisGroup);

	m_chkSkipSubOnly = new QCheckBox(tr("Skip jobs where only subtitle tracks would be removed"), analysisGroup);
	m_chkSkipSubOnly->setToolTip(tr(
		"Subtitle tracks are tiny — removing them saves almost no space but still requires "
		"a full remux. Enable this to ignore those files during Analyze."));
	m_chkSkipSubOnly->setChecked(profile->skipSubtitleOnlyJobs());
	analysisLayout->addWidget(m_chkSkipSubOnly);

	m_chkWriteLog = new QCheckBox(tr("Write .mc-log file alongside each processed file"), analysisGroup);
	m_chkWriteLog->setToolTip(tr(
		"After each successful remux, writes a plain-text report next to the output file. "
		"Contains the filename, date, space reclaimed, removed tracks, and the exact mkvmerge command."));
	m_chkWriteLog->setChecked(profile->writeJobLog());
	analysisLayout->addWidget(m_chkWriteLog);
	genPageLo->addWidget(analysisGroup);

	// Job Queue
	auto* jobGroup  = new QGroupBox(tr("Job Queue"), genPage);
	auto* jobLayout = new QVBoxLayout(jobGroup);
	m_chkAutoTrack  = new QCheckBox(tr("Track running job"), jobGroup);
	m_chkAutoTrack->setToolTip(tr(
		"When a job starts: scroll it into view, switching the filter to Running "
		"first if another filter is active"));
	m_chkAutoTrack->setChecked(AppSettings::instance().value("jobPanel/followRunning", true).toBool());
	jobLayout->addWidget(m_chkAutoTrack);
	genPageLo->addWidget(jobGroup);

	// Cards
	auto* cardsGroup  = new QGroupBox(tr("Cards"), genPage);
	auto* cardsLayout = new QVBoxLayout(cardsGroup);

	auto* fanartRow   = new QHBoxLayout;
	auto* fanartLabel = new QLabel(tr("Fanart background opacity:"), cardsGroup);
	m_sliderFanartOpacity = new QSlider(Qt::Horizontal, cardsGroup);
	m_sliderFanartOpacity->setRange(0, 100);
	const int initialFanartPct = AppSettings::instance().value("library/fanartOpacity", 5).toInt();
	m_sliderFanartOpacity->setValue(initialFanartPct);
	m_sliderFanartOpacity->setToolTip(tr(
		"How visible the movie's fanart backdrop is behind each card. "
		"0% hides it entirely; higher values make it more prominent."));
	m_lblFanartOpacity = new QLabel(tr("%1%").arg(initialFanartPct), cardsGroup);
	m_lblFanartOpacity->setMinimumWidth(36);
	connect(m_sliderFanartOpacity, &QSlider::valueChanged, this, [this](int v) {
		m_lblFanartOpacity->setText(tr("%1%").arg(v));
		emit fanartOpacityChanged(v / 100.0);
	});
	fanartRow->addWidget(fanartLabel);
	fanartRow->addWidget(m_sliderFanartOpacity, 1);
	fanartRow->addWidget(m_lblFanartOpacity);
	cardsLayout->addLayout(fanartRow);
	genPageLo->addWidget(cardsGroup);

	genPageLo->addStretch();

	// ── Buttons ───────────────────────────────────────────────────────────────
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Save"));
	connect(buttons, &QDialogButtonBox::accepted, this, &McSettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void McSettingsDialog::done(int result)
{
	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("settingsDialog/geometry", saveGeometry());
	QDialog::done(result);
}

void McSettingsDialog::onBrowseStagingDir()
{
	const QString raw = QFileDialog::getExistingDirectory(
		this, tr("Choose Local Staging Folder"), m_editStagingDir->text(),
		QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
	if (raw.isEmpty()) return;
	m_editStagingDir->setText(QDir::fromNativeSeparators(raw));
}

void McSettingsDialog::onAddLanguage()
{
	const int idx  = m_langCombo->currentIndex();
	const QString code = (idx >= 0)
		? m_langCombo->itemData(idx).toString()
		: m_langCombo->currentText().trimmed().toLower().left(3);

	if (code.isEmpty()) return;

	for (int i = 0; i < m_langList->count(); ++i)
		if (m_langList->item(i)->data(Qt::UserRole).toString() == code) return;

	auto* item = new QListWidgetItem(langFlagIcon(code, devicePixelRatioF()),
	                                 displayName(code), m_langList);
	item->setData(Qt::UserRole, code);
}

void McSettingsDialog::onRemoveLanguage()
{
	const auto selected = m_langList->selectedItems();
	if (selected.isEmpty()) return;
	delete selected.first();
}

void McSettingsDialog::onLanguageUp()
{
	const int row = m_langList->currentRow();
	if (row <= 0) return;
	auto* item = m_langList->takeItem(row);
	m_langList->insertItem(row - 1, item);
	m_langList->setCurrentRow(row - 1);
}

void McSettingsDialog::onLanguageDown()
{
	const int row = m_langList->currentRow();
	if (row < 0 || row >= m_langList->count() - 1) return;
	auto* item = m_langList->takeItem(row);
	m_langList->insertItem(row + 1, item);
	m_langList->setCurrentRow(row + 1);
}

void McSettingsDialog::onAddEditionToken()
{
	const QString tok = m_editEditionToken->text().trimmed();
	if (tok.isEmpty()) return;

	for (int i = 0; i < m_editionTokenList->count(); ++i)
		if (m_editionTokenList->item(i)->text().compare(tok, Qt::CaseInsensitive) == 0) {
			m_editEditionToken->clear();
			return;
		}

	new QListWidgetItem(tok, m_editionTokenList);
	m_editEditionToken->clear();
}

void McSettingsDialog::onRemoveEditionToken()
{
	const auto selected = m_editionTokenList->selectedItems();
	for (auto* item : selected)
		delete item;
}

void McSettingsDialog::onResetEditionTokens()
{
	m_editionTokenList->clear();
	for (const QString& tok : UserProfile::defaultEditionTokens())
		new QListWidgetItem(tok, m_editionTokenList);
}

void McSettingsDialog::onAudioFormatUp()
{
	const int row = m_audioFormatList->currentRow();
	if (row <= 0) return;
	auto* item = m_audioFormatList->takeItem(row);
	m_audioFormatList->insertItem(row - 1, item);
	m_audioFormatList->setCurrentRow(row - 1);
}

void McSettingsDialog::onAudioFormatDown()
{
	const int row = m_audioFormatList->currentRow();
	if (row < 0 || row >= m_audioFormatList->count() - 1) return;
	auto* item = m_audioFormatList->takeItem(row);
	m_audioFormatList->insertItem(row + 1, item);
	m_audioFormatList->setCurrentRow(row + 1);
}

void McSettingsDialog::onSubFmtUp()
{
	const int row = m_subFormatList->currentRow();
	if (row <= 0) return;
	auto* item = m_subFormatList->takeItem(row);
	m_subFormatList->insertItem(row - 1, item);
	m_subFormatList->setCurrentRow(row - 1);
}

void McSettingsDialog::onSubFmtDown()
{
	const int row = m_subFormatList->currentRow();
	if (row < 0 || row >= m_subFormatList->count() - 1) return;
	auto* item = m_subFormatList->takeItem(row);
	m_subFormatList->insertItem(row + 1, item);
	m_subFormatList->setCurrentRow(row + 1);
}

void McSettingsDialog::accept()
{
	QStringList langs;
	for (int i = 0; i < m_langList->count(); ++i)
		langs << m_langList->item(i)->data(Qt::UserRole).toString();

	if (langs.isEmpty()) {
		QMessageBox::warning(this, tr("Settings"),
			tr("At least one understood language is required."));
		return;
	}

	QStringList fmtOrder, fmtDisabled;
	for (int i = 0; i < m_audioFormatList->count(); ++i) {
		const auto* item = m_audioFormatList->item(i);
		const QString id = item->data(Qt::UserRole).toString();
		fmtOrder << id;
		if (item->checkState() == Qt::Unchecked)
			fmtDisabled << id;
	}

	m_profile->setUnderstoodLanguages(langs);
	m_profile->setAlwaysKeepOriginalAudio(m_chkKeepOriginalAudio->isChecked());
	m_profile->setKeepCommentaryIfUnderstood(m_chkKeepCommentary->isChecked());
	m_profile->setStereoAsCommentaryHeuristic(m_chkStereoCommentary->isChecked());
	m_profile->setAudioFormatOrder(fmtOrder);
	m_profile->setDisabledAudioFormats(fmtDisabled);

	QStringList subOrder, subDisabled;
	for (int i = 0; i < m_subFormatList->count(); ++i) {
		const auto* item = m_subFormatList->item(i);
		const QString id = item->data(Qt::UserRole).toString();
		subOrder << id;
		if (item->checkState() == Qt::Unchecked)
			subDisabled << id;
	}
	m_profile->setSubtitleFormatOrder(subOrder);
	m_profile->setDisabledSubtitleFormats(subDisabled);
	m_profile->setRemoveMjpegCoverArt(m_chkRemoveMjpeg->isChecked());
	m_profile->setSkipSubtitleOnlyJobs(m_chkSkipSubOnly->isChecked());
	m_profile->setKeepForcedSubtitlesAlways(m_chkKeepForced->isChecked());
	m_profile->setSdhSubtitleMode(static_cast<UserProfile::SdhSubtitleMode>(m_cmbSdhMode->currentIndex()));
	m_profile->setKeepOriginalLanguageSubtitle(m_chkKeepOriginalSub->isChecked());
	m_profile->setMergeSidecarSubtitles(m_chkMergeSidecarSubs->isChecked());
	m_profile->setDetectSidecarSubtitleLanguage(m_chkDetectSubLanguage->isChecked());
	m_profile->setWriteJobLog(m_chkWriteLog->isChecked());
	m_profile->setUseLocalStaging(m_chkUseLocalStaging->isChecked());
	m_profile->setLocalStagingDir(QDir::fromNativeSeparators(m_editStagingDir->text().trimmed()));
	m_profile->setTmdbApiKey(m_editTmdbKey->text().trimmed());
	m_profile->setWriteNfoFiles(m_chkWriteNfo->isChecked());
	m_profile->setOpenSubtitlesApiKey(m_editOsApiKey->text().trimmed());
	m_profile->setOpenSubtitlesUsername(m_editOsUsername->text().trimmed());
	m_profile->setOpenSubtitlesPassword(m_editOsPassword->text());
	m_profile->setAutoDownloadSubtitles(m_chkAutoDownloadSubs->isChecked());
	m_profile->setComputeSubtitleMovieHash(m_chkComputeMovieHash->isChecked());
	m_profile->setSubtitleRetryCooldownDays(m_spinSubtitleRetryCooldown->value());

	QStringList editionTokens;
	for (int i = 0; i < m_editionTokenList->count(); ++i)
		editionTokens << m_editionTokenList->item(i)->text();
	m_profile->setEditionTokens(editionTokens);

	m_profile->save();

	AppSettings::instance().setValue("jobPanel/followRunning", m_chkAutoTrack->isChecked());
	StorageGroupSettings::setUiMaxGroup(m_spinScanGroups->value());
	PosterManager::instance().setParallelWorkers(m_spinPosterWorkers->value());
	AppSettings::instance().setValue("library/fanartOpacity", m_sliderFanartOpacity->value());

	QDialog::accept();
}

} // namespace Mc
