#include "ui/McSettingsDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McLanguageFlags.h"
#include "ui/McWindowGeometry.h"
#include "core/AppSettings.h"
#include "core/DownloadIntegrationSettings.h"
#include "core/StorageGroupSettings.h"
#include "core/UserProfile.h"
#include "engine/DownloadClientRegistry.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QUrl>
#include <QUrlQuery>
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
	// 760x701 is sized for the tallest tab's content at 100% scaling. At high DPI
	// scaling (e.g. 300% on a 4K display, whose logical resolution shrinks to
	// ~1280x720) that can exceed what's actually on screen — clamp it so this
	// minimum can never itself push the dialog past the screen's available area.
	setMinimumSize(clampSizeToScreen(this, QSize(760, 670)));

	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	if (const QByteArray geo = s.value("settingsDialog/geometry").toByteArray(); !geo.isEmpty())
		restoreGeometry(geo);
	ensureGeometryFitsScreen(this);

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
		"Some MKV files contain an MJPEG or PNG video stream used as embedded cover art or a thumbnail.\n"
		"These streams are not playable content and add unnecessary size.\n"
		"Enable to mark them for removal during Analyze."));
	m_chkRemoveMjpeg->setChecked(profile->removeMjpegCoverArt());
	videoLayout->addWidget(m_chkRemoveMjpeg);

	videoPageLo->addWidget(videoGroup);

	// Dolby Vision deep scan (FEL/MEL)
	auto* dvGroup  = new QGroupBox(tr("Dolby Vision Deep Scan"), videoPage);
	auto* dvLayout = new QVBoxLayout(dvGroup);

	m_chkDeepDolbyVisionScan = new QCheckBox(
		tr("Enable \"Deep Scan for FEL/MEL\""), dvGroup);
	m_chkDeepDolbyVisionScan->setChecked(profile->deepDolbyVisionScanEnabled());
	m_chkDeepDolbyVisionScan->setToolTip(tr(
		"Adds a right-click action on dual-layer Dolby Vision Profile 7 video tracks that\n"
		"extracts the elementary video stream and analyzes it with dovi_tool to determine\n"
		"whether it carries a Full (FEL) or Minimal (MEL) Enhancement Layer — a distinction\n"
		"most players and devices care about, but that plain metadata scanning can't see.\n\n"
		"Always explicit and per-file, never automatic during a scan, since it means reading\n"
		"and analyzing the whole video track rather than just its metadata. Off by default."));
	dvLayout->addWidget(m_chkDeepDolbyVisionScan);
	videoPageLo->addWidget(dvGroup);

	videoPageLo->addStretch();

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 1 — Languages
	// ═══════════════════════════════════════════════════════════════════════════
	auto* langPage   = new QWidget;
	auto* langPageLo = new QVBoxLayout(langPage);
	langPageLo->setSpacing(8);
	langPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(langPage, tr("Languages"));
	tabs->setTabColor(1, QColor(0x2a, 0x9a, 0x6a));

	auto* langGroup  = new QGroupBox(tr("Understood Languages"), langPage);
	auto* langLayout = new QVBoxLayout(langGroup);
	langLayout->setContentsMargins(4, 4, 4, 4);
	langLayout->setSpacing(4);

	auto* langHint = new QLabel(
		tr("Drag or use buttons to reorder. Topmost is used as the preferred TMDB "
		   "display-title language for card titles and .nfo files. Only applies when "
		   "a file is scanned or (re-)matched — reordering doesn't retitle already-matched files."),
		langGroup);
	langHint->setWordWrap(true);
	langLayout->addWidget(langHint);

	m_langList = new QListWidget(langGroup);
	m_langList->setDragDropMode(QAbstractItemView::InternalMove);
	m_langList->setDefaultDropAction(Qt::MoveAction);
	m_langList->setSelectionMode(QAbstractItemView::SingleSelection);
	// Now that this has a dedicated tab (unlike its old cramped spot sharing
	// "Other" with four other groups), it can claim the full available height
	// the same way Audio/Subtitles' format-order lists do.
	m_langList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
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
	langPageLo->addWidget(langGroup, 1);

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 2 — Audio
	// ═══════════════════════════════════════════════════════════════════════════
	auto* audioPage   = new QWidget;
	auto* audioPageLo = new QHBoxLayout(audioPage);
	audioPageLo->setSpacing(8);
	audioPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(audioPage, tr("Audio"));
	tabs->setTabColor(2, QColor(0x10, 0x6a, 0xc0));

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
		"When a file has a surround (5.1+) audio track, any stereo track in the same language\n"
		"is assumed to be a commentary or secondary mix — even without a title or flag indicating it.\n\n"
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
	// Tab 3 — Subtitles
	// ═══════════════════════════════════════════════════════════════════════════
	auto* subPage   = new QWidget;
	auto* subPageLo = new QHBoxLayout(subPage);
	subPageLo->setSpacing(8);
	subPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(subPage, tr("Subtitles"));
	tabs->setTabColor(3, QColor(0x1a, 0x86, 0x4a));

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
		"When a file is remuxed for another reason, also absorb any external .srt/.ass/.vtt\n"
		"sidecar subtitles into the output.\n\n"
		"Disable to leave sidecar files untouched on disk."));
	m_chkDetectSubLanguage->setChecked(profile->detectSidecarSubtitleLanguage());
	m_chkDetectSubLanguage->setToolTip(tr(
		"When a sidecar .srt/.ass/.ssa/.vtt file's name carries no language code, sample its\n"
		"dialogue text to detect the language and rename the file to include it.\n\n"
		"Only renames on a high-confidence read. Off by default since this renames files on disk."));

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
		"Reads the first and last 64 KB of each file to compute OpenSubtitles' own file hash,\n"
		"which — when it matches — guarantees the exact right subtitle.\n\n"
		"Only matches an unmodified original release rip, so it's useless for files you've\n"
		"remuxed/edited yourself, and reading every file up front slows down batch downloads.\n"
		"Off by default."));
	osLayout->addWidget(m_chkComputeMovieHash);
	osLayout->addSpacing(16);   // edition-tags list used to occupy this space; now moved to its own tab

	auto* osHint = new QLabel(
		tr("Without credentials, up to 100 anonymous downloads per day are available. "
		   "Add your account for a larger personal quota.\n"
		   "Get a free account at <a href=\"https://www.opensubtitles.com\">opensubtitles.com</a>."),
		osGroup);
	osHint->setTextFormat(Qt::RichText);
	osHint->setOpenExternalLinks(true);
	osHint->setWordWrap(true);
	osLayout->addWidget(osHint);
	// Pin everything above to the top of the box and let the (now-empty, since the
	// edition-tags list moved to its own tab) leftover space collect below the hint
	// instead of getting redistributed around existing widgets by the layout engine.
	osLayout->addStretch(1);
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
	// Tab 4 — Editions
	// ═══════════════════════════════════════════════════════════════════════════
	auto* editionsPage   = new QWidget;
	auto* editionsPageLo = new QVBoxLayout(editionsPage);
	editionsPageLo->setSpacing(8);
	editionsPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(editionsPage, tr("Editions"));
	tabs->setTabColor(4, McCardDelegate::badgeColor(QStringLiteral("edition")));

	auto* editionsHint = new QLabel(
		tr("This list is used two ways: it scores OpenSubtitles candidates by whether their "
		   "release name claims the same cut as your filename, and it detects each file's "
		   "edition/cut for the library's \"Group by Movie\" view — the badge shown on each "
		   "file, and how duplicate versions of the same movie are found.\n\n"
		   "An entry may list several spellings of the same cut separated by '|' — e.g. "
		   "\"Director's Cut|Directors Cut|DC\" — matching any of them is treated as that one "
		   "edition, labeled with whichever spelling is listed first. Not exhaustive — release "
		   "naming isn't standardized — add any tag you run into.\n\n"
		   "Double-click an entry to edit it in place — reorder the '|' list to change which "
		   "spelling is used as the badge."),
		editionsPage);
	editionsHint->setWordWrap(true);
	editionsPageLo->addWidget(editionsHint);

	m_editionTokenList = new QListWidget(editionsPage);
	m_editionTokenList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_editionTokenList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	for (const QString& tok : profile->editionTokens()) {
		auto* item = new QListWidgetItem(tok, m_editionTokenList);
		item->setFlags(item->flags() | Qt::ItemIsEditable);
	}
	editionsPageLo->addWidget(m_editionTokenList, 1);

	auto* editionRow = new QHBoxLayout;
	m_editEditionToken = new QLineEdit(editionsPage);
	m_editEditionToken->setPlaceholderText(tr("e.g. \"Open Matte\" or \"Redux|Special Edition\""));
	auto* editionAddBtn    = new QPushButton(tr("Add"),    editionsPage);
	auto* editionRemoveBtn = new QPushButton(tr("Remove"), editionsPage);
	auto* editionResetBtn  = new QPushButton(tr("Reset to Defaults"), editionsPage);
	editionRow->addWidget(m_editEditionToken, 1);
	editionRow->addWidget(editionAddBtn);
	editionRow->addWidget(editionRemoveBtn);
	editionRow->addWidget(editionResetBtn);
	editionsPageLo->addLayout(editionRow);
	connect(editionAddBtn,    &QPushButton::clicked, this, &McSettingsDialog::onAddEditionToken);
	connect(editionRemoveBtn, &QPushButton::clicked, this, &McSettingsDialog::onRemoveEditionToken);
	connect(editionResetBtn,  &QPushButton::clicked, this, &McSettingsDialog::onResetEditionTokens);
	connect(m_editEditionToken, &QLineEdit::returnPressed, this, &McSettingsDialog::onAddEditionToken);
	connect(m_editionTokenList, &QListWidget::itemChanged, this, &McSettingsDialog::onEditionTokenEdited);

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 5 — Performance
	// ═══════════════════════════════════════════════════════════════════════════
	auto* perfPage   = new QWidget;
	auto* perfPageLo = new QVBoxLayout(perfPage);
	perfPageLo->setSpacing(8);
	perfPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(perfPage, tr("Performance"));
	tabs->setTabColor(5, QColor(0xc0, 0x20, 0x20));

	auto* perfGroup  = new QGroupBox(tr("Performance"), perfPage);
	auto* perfLayout = new QVBoxLayout(perfGroup);

	auto* scanGroupsRow   = new QHBoxLayout;
	auto* scanGroupsLabel = new QLabel(tr("Storage groups shown:"), perfGroup);
	m_spinScanGroups      = new QSpinBox(perfGroup);
	m_spinScanGroups->setRange(2, StorageGroupSettings::MaxGroup);
	m_spinScanGroups->setValue(StorageGroupSettings::uiMaxGroup());
	m_spinScanGroups->setToolTip(tr(
		"How many storage groups appear in Manage Folders. Group folders on the same\n"
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
		"Number of parallel background threads for TMDB poster and fanart downloads.\n"
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
		"When the source file lives on a network share, reading and writing to it at the\n"
		"same time can slow both down.\n\n"
		"Enabling this writes the muxed output to a local folder first, then copies the\n"
		"finished file back over the network as a separate step.\n\n"
		"Requires enough free space in the local folder to hold the output file — falls\n"
		"back to muxing in place when there isn't enough room."));
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
	// Tab 6 — Interface
	// ═══════════════════════════════════════════════════════════════════════════
	auto* ifacePage   = new QWidget;
	auto* ifacePageLo = new QVBoxLayout(ifacePage);
	ifacePageLo->setSpacing(8);
	ifacePageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(ifacePage, tr("Interface"));
	tabs->setTabColor(6, QColor(0x7a, 0x4a, 0xb0));

	// Job Queue
	auto* jobGroup  = new QGroupBox(tr("Job Queue"), ifacePage);
	auto* jobLayout = new QVBoxLayout(jobGroup);
	m_chkAutoTrack  = new QCheckBox(tr("Automatically scroll to the running job"), jobGroup);
	m_chkAutoTrack->setChecked(AppSettings::instance().value("jobPanel/followRunning", true).toBool());
	jobLayout->addWidget(m_chkAutoTrack);

	auto* jobHint = new QLabel(
	    tr("When a job starts, the Job Queue panel scrolls it into view — switching to the "
	       "Running filter first if a different filter is active, so the job doesn't stay "
	       "hidden behind it."), jobGroup);
	jobHint->setWordWrap(true);
	jobLayout->addWidget(jobHint);

	ifacePageLo->addWidget(jobGroup);

#ifdef Q_OS_WIN
	// File Manager
	auto* fileMgrGroup  = new QGroupBox(tr("File Manager"), ifacePage);
	auto* fileMgrLayout = new QVBoxLayout(fileMgrGroup);
	m_chkAlwaysUseExplorer = new QCheckBox(
	    tr("Always use File Explorer for \"Open Containing Folder\" (selects the file)"), fileMgrGroup);
	m_chkAlwaysUseExplorer->setToolTip(tr(
	    "Off (default): opens your system default file manager, but it just opens the\n"
	    "folder without selecting the file.\n"
	    "On: always launches File Explorer specifically, with the file pre-selected —\n"
	    "useful if your default file manager isn't File Explorer but you still want the\n"
	    "file highlighted."));
	m_chkAlwaysUseExplorer->setChecked(
	    AppSettings::instance().value("settings/alwaysUseExplorerForReveal", false).toBool());
	fileMgrLayout->addWidget(m_chkAlwaysUseExplorer);

	ifacePageLo->addWidget(fileMgrGroup);
#endif

	// Stats
	auto* statsGroup  = new QGroupBox(tr("Stats"), ifacePage);
	auto* statsLayout = new QVBoxLayout(statsGroup);
	m_chkAggregateManualDeletes = new QCheckBox(
	    tr("Include manually deleted files (e.g. duplicates) in Reclaimed / Money Saved"), statsGroup);
	m_chkAggregateManualDeletes->setToolTip(tr(
	    "Files deleted via \"Delete File from Disk\" aren't posted to the leaderboard — unlike\n"
	    "mkvmerge track removal, a manual delete is easy to fake (copy a file, then \"reclaim\" it).\n\n"
	    "But you can still choose to see them reflected in your own local totals here."));
	m_chkAggregateManualDeletes->setChecked(
	    AppSettings::instance().value("settings/includeManualDeletesInTotals", true).toBool());
	statsLayout->addWidget(m_chkAggregateManualDeletes);

	auto* statsHint = new QLabel(
	    tr("This only affects your local Reclaimed / Money Saved totals in the status bar. "
	       "The leaderboard is unaffected either way — it only ever counts space saved by "
	       "removing tracks via mkvmerge."), statsGroup);
	statsHint->setWordWrap(true);
	statsLayout->addWidget(statsHint);

	ifacePageLo->addWidget(statsGroup);

	// Cards
	auto* cardsGroup  = new QGroupBox(tr("Cards"), ifacePage);
	auto* cardsLayout = new QVBoxLayout(cardsGroup);

	auto* fanartRow   = new QHBoxLayout;
	auto* fanartLabel = new QLabel(tr("Fanart background opacity:"), cardsGroup);
	m_sliderFanartOpacity = new QSlider(Qt::Horizontal, cardsGroup);
	m_sliderFanartOpacity->setRange(0, 100);
	const int initialFanartPct = AppSettings::instance().value("library/fanartOpacity", 5).toInt();
	m_sliderFanartOpacity->setValue(initialFanartPct);
	m_sliderFanartOpacity->setToolTip(tr(
		"How visible the movie's fanart backdrop is behind each card.\n"
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
	ifacePageLo->addWidget(cardsGroup);

	ifacePageLo->addStretch();

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 7 — Downloads (NZBGet, SABnzbd; future providers get their own group box here)
	// ═══════════════════════════════════════════════════════════════════════════
	auto* dlPage   = new QWidget;
	auto* dlPageLo = new QVBoxLayout(dlPage);
	dlPageLo->setSpacing(8);
	dlPageLo->setContentsMargins(8, 8, 8, 8);
	tabs->addTab(dlPage, tr("Downloads"));
	tabs->setTabColor(7, QColor(0x55, 0x65, 0x55));

	m_grpNzbEnabled = new QGroupBox(tr("NZBGet"), dlPage);
	m_grpNzbEnabled->setCheckable(true);
	auto* nzbLayout = new QVBoxLayout(m_grpNzbEnabled);

	const NzbGetConfig nzbConfig = DownloadIntegrationSettings::nzbgetConfig();
	m_grpNzbEnabled->setChecked(nzbConfig.enabled);

	auto* nzbHostRow   = new QHBoxLayout;
	auto* nzbHostLabel = new QLabel(tr("Host:"), m_grpNzbEnabled);
	m_editNzbHost      = new QLineEdit(m_grpNzbEnabled);
	m_editNzbHost->setPlaceholderText(tr("e.g. 192.168.1.10"));
	m_editNzbHost->setText(nzbConfig.host);
	auto* nzbPortLabel = new QLabel(tr("Port:"), m_grpNzbEnabled);
	m_spinNzbPort      = new QSpinBox(m_grpNzbEnabled);
	m_spinNzbPort->setRange(1, 65535);
	m_spinNzbPort->setValue(nzbConfig.port);
	nzbHostRow->addWidget(nzbHostLabel);
	nzbHostRow->addWidget(m_editNzbHost, 1);
	nzbHostRow->addWidget(nzbPortLabel);
	nzbHostRow->addWidget(m_spinNzbPort);
	nzbLayout->addLayout(nzbHostRow);

	auto* nzbUserRow   = new QHBoxLayout;
	auto* nzbUserLabel = new QLabel(tr("Username:"), m_grpNzbEnabled);
	m_editNzbUsername  = new QLineEdit(m_grpNzbEnabled);
	m_editNzbUsername->setText(nzbConfig.username);
	nzbUserRow->addWidget(nzbUserLabel);
	nzbUserRow->addWidget(m_editNzbUsername, 1);
	nzbLayout->addLayout(nzbUserRow);

	auto* nzbPassRow   = new QHBoxLayout;
	auto* nzbPassLabel = new QLabel(tr("Password:"), m_grpNzbEnabled);
	m_editNzbPassword  = new QLineEdit(m_grpNzbEnabled);
	m_editNzbPassword->setEchoMode(QLineEdit::Password);
	m_editNzbPassword->setText(nzbConfig.password);
	nzbPassRow->addWidget(nzbPassLabel);
	nzbPassRow->addWidget(m_editNzbPassword, 1);
	nzbLayout->addLayout(nzbPassRow);

	auto* nzbTestRow = new QHBoxLayout;
	m_btnNzbTestConnection = new QPushButton(tr("Test Connection"), m_grpNzbEnabled);
	connect(m_btnNzbTestConnection, &QPushButton::clicked, this, &McSettingsDialog::onTestNzbConnection);
	m_lblNzbTestResult = new QLabel(m_grpNzbEnabled);
	nzbTestRow->addWidget(m_btnNzbTestConnection);
	nzbTestRow->addWidget(m_lblNzbTestResult, 1);
	nzbLayout->addLayout(nzbTestRow);

	dlPageLo->addWidget(m_grpNzbEnabled);

	m_grpSabEnabled = new QGroupBox(tr("SABnzbd"), dlPage);
	m_grpSabEnabled->setCheckable(true);
	auto* sabLayout = new QVBoxLayout(m_grpSabEnabled);

	const SabnzbdConfig sabConfig = DownloadIntegrationSettings::sabnzbdConfig();
	m_grpSabEnabled->setChecked(sabConfig.enabled);

	auto* sabHostRow   = new QHBoxLayout;
	auto* sabHostLabel = new QLabel(tr("Host:"), m_grpSabEnabled);
	m_editSabHost      = new QLineEdit(m_grpSabEnabled);
	m_editSabHost->setPlaceholderText(tr("e.g. 192.168.1.10"));
	m_editSabHost->setText(sabConfig.host);
	auto* sabPortLabel = new QLabel(tr("Port:"), m_grpSabEnabled);
	m_spinSabPort      = new QSpinBox(m_grpSabEnabled);
	m_spinSabPort->setRange(1, 65535);
	m_spinSabPort->setValue(sabConfig.port);
	sabHostRow->addWidget(sabHostLabel);
	sabHostRow->addWidget(m_editSabHost, 1);
	sabHostRow->addWidget(sabPortLabel);
	sabHostRow->addWidget(m_spinSabPort);
	sabLayout->addLayout(sabHostRow);

	auto* sabKeyRow   = new QHBoxLayout;
	auto* sabKeyLabel = new QLabel(tr("API Key:"), m_grpSabEnabled);
	m_editSabApiKey   = new QLineEdit(m_grpSabEnabled);
	m_editSabApiKey->setEchoMode(QLineEdit::Password);
	m_editSabApiKey->setText(sabConfig.apiKey);
	sabKeyRow->addWidget(sabKeyLabel);
	sabKeyRow->addWidget(m_editSabApiKey, 1);
	sabLayout->addLayout(sabKeyRow);

	auto* sabKeyHint = new QLabel(
		tr("Found in SABnzbd under Config → General → API Key."), m_grpSabEnabled);
	sabKeyHint->setWordWrap(true);
	sabLayout->addWidget(sabKeyHint);

	auto* sabTestRow = new QHBoxLayout;
	m_btnSabTestConnection = new QPushButton(tr("Test Connection"), m_grpSabEnabled);
	connect(m_btnSabTestConnection, &QPushButton::clicked, this, &McSettingsDialog::onTestSabConnection);
	m_lblSabTestResult = new QLabel(m_grpSabEnabled);
	sabTestRow->addWidget(m_btnSabTestConnection);
	sabTestRow->addWidget(m_lblSabTestResult, 1);
	sabLayout->addLayout(sabTestRow);

	dlPageLo->addWidget(m_grpSabEnabled);

	// Shared behavior — applies no matter which provider above reports the
	// completion, so it isn't duplicated inside each provider's group box.
	auto* dlBehaviorGroup  = new QGroupBox(tr("On Download Complete"), dlPage);
	auto* dlBehaviorLayout = new QVBoxLayout(dlBehaviorGroup);

	m_chkAutoQuickScan = new QCheckBox(
		tr("Automatically Quick Scan when a download completes"), dlBehaviorGroup);
	m_chkAutoQuickScan->setChecked(DownloadIntegrationSettings::autoQuickScanOnComplete());
	dlBehaviorLayout->addWidget(m_chkAutoQuickScan);

	m_chkAutoQuickAnalyze = new QCheckBox(
		tr("Automatically Quick Analyze after scanning"), dlBehaviorGroup);
	m_chkAutoQuickAnalyze->setChecked(DownloadIntegrationSettings::autoQuickAnalyzeOnComplete());
	dlBehaviorLayout->addWidget(m_chkAutoQuickAnalyze);

	auto* dlBehaviorHint = new QLabel(
		tr("Applies to any enabled provider above. Each only reports a download as complete "
		   "once its own post-processing (unpack, par-repair, any sort/rename scripts) has "
		   "finished, so the file is already in place by the time a Quick Scan is triggered."),
		dlBehaviorGroup);
	dlBehaviorHint->setWordWrap(true);
	dlBehaviorLayout->addWidget(dlBehaviorHint);

	dlPageLo->addWidget(dlBehaviorGroup);
	dlPageLo->addStretch();

	// ═══════════════════════════════════════════════════════════════════════════
	// Tab 8 — Other (always last — catch-all for settings that don't fit elsewhere)
	// ═══════════════════════════════════════════════════════════════════════════
	auto* genPage   = new QWidget;
	auto* genPageLo = new QVBoxLayout(genPage);
	genPageLo->setSpacing(8);
	genPageLo->setContentsMargins(8, 8, 8, 8);

	// Scrollable: this tab stacks several groups — without this, every new setting
	// added here permanently raises the whole dialog's forced minimum height
	// instead of just needing a scroll within the tab.
	auto* genScroll = new QScrollArea;
	genScroll->setWidget(genPage);
	genScroll->setWidgetResizable(true);
	genScroll->setFrameShape(QFrame::NoFrame);
	tabs->addTab(genScroll, tr("Other"));
	tabs->setTabColor(8, QColor(0x55, 0x55, 0x65));

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
		"Writes a Kodi-style .nfo file next to each matched video, containing its IMDb and\n"
		"TMDB ids, title, original title, year, and TMDB rating. Title is picked from your\n"
		"Understood Languages list, above — the first one TMDB actually has a translation\n"
		"for; original title is always included too, so Kodi's own \"Show original titles\n"
		"for movies\" setting can prefer it on this machine without affecting other viewers\n"
		"of a shared library.\n\n"
		"If an .nfo already exists, only these specific tags are added or updated —\n"
		"everything else in the file is left untouched, and a scene-release NFO's free-form\n"
		"text only ever has its id corrected in place, never replaced.\n\n"
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

	// Scene NFO (srrDB)
	auto* sceneNfoGroup  = new QGroupBox(tr("Scene NFO"), genPage);
	auto* sceneNfoLayout = new QVBoxLayout(sceneNfoGroup);

	m_chkDownloadSceneNfo = new QCheckBox(
		tr("Enable \"Download Scene NFO…\" (via srrDB)"), sceneNfoGroup);
	m_chkDownloadSceneNfo->setChecked(profile->downloadSceneNfoEnabled());
	m_chkDownloadSceneNfo->setToolTip(tr(
		"Adds a right-click action per file that looks up the original scene-release\n"
		".nfo on srrDB.com (a community-run scene-release archive, not an official API)\n"
		"and saves it — useful when a local scene NFO is wrong/stale or corrupted beyond\n"
		"recovery.\n\n"
		"The result is always recorded in the database so the NFO viewer picks it up.\n"
		"Whether it's also written to disk as a real .nfo file depends on \"Write .nfo\n"
		"files\" above: when that's on, MediaCurator is already writing its own Kodi-style\n"
		"XML to that same filename, so a downloaded scene NFO stays database-only to avoid\n"
		"the two fighting over one file; when it's off, the download is written to disk too.\n"
		"Off by default."));
	sceneNfoLayout->addWidget(m_chkDownloadSceneNfo);

	m_chkAutoDownloadSceneNfo = new QCheckBox(
		tr("Automatically download missing scene NFOs after scanning"), sceneNfoGroup);
	m_chkAutoDownloadSceneNfo->setChecked(profile->autoDownloadSceneNfo());
	m_chkAutoDownloadSceneNfo->setEnabled(m_chkDownloadSceneNfo->isChecked());
	connect(m_chkDownloadSceneNfo, &QCheckBox::toggled, m_chkAutoDownloadSceneNfo, &QCheckBox::setEnabled);
	m_chkAutoDownloadSceneNfo->setToolTip(tr(
		"Looks up and saves the scene-release .nfo in the background as files are\n"
		"scanned, the same way subtitles/posters are fetched automatically — only for\n"
		"files with no local scene NFO already, and only when a single confident srrDB\n"
		"match is found (an ambiguous result is left for the action above to resolve\n"
		"by hand). Requires \"Enable Download Scene NFO\" above. Off by default."));
	sceneNfoLayout->addWidget(m_chkAutoDownloadSceneNfo);
	genPageLo->addWidget(sceneNfoGroup);

	// Retry cooldown — shared by TMDB poster/NFO lookups and OpenSubtitles re-search,
	// so it gets its own group rather than living under either one specifically.
	// Explicitly not job-queue retry (a failed remux job) — that's a different
	// concept entirely and lives in the Job Queue group below.
	auto* retryGroup  = new QGroupBox(tr("Metadata Retry Cooldown"), genPage);
	auto* retryLayout = new QVBoxLayout(retryGroup);

	auto* retryHint = new QLabel(
		tr("Applies to background poster, fanart, and subtitle lookups that came up empty "
		   "— not to a failed job in the Job Queue, which is retried separately."),
		retryGroup);
	retryHint->setWordWrap(true);
	retryLayout->addWidget(retryHint);

	auto* retryRow   = new QHBoxLayout;
	auto* retryLabel = new QLabel(tr("Wait before retrying (days):"), retryGroup);
	m_spinRetryCooldown = new QSpinBox(retryGroup);
	m_spinRetryCooldown->setRange(0, 365);
	m_spinRetryCooldown->setSpecialValueText(tr("Off (always retry)"));
	m_spinRetryCooldown->setValue(profile->subtitleRetryCooldownDays());
	m_spinRetryCooldown->setToolTip(tr(
		"How long to wait before re-checking a file that came up empty on a previous attempt\n"
		"— a poster/rating TMDB had no match for, or a subtitle OpenSubtitles had nothing for.\n\n"
		"Without this, every scan and every launch repeats the same lookup (including a local\n"
		"folder scan for poster/NFO files) for files that were already checked and found to\n"
		"have nothing. Shared by both TMDB and OpenSubtitles lookups. 0 disables the cooldown."));
	retryRow->addWidget(retryLabel);
	retryRow->addWidget(m_spinRetryCooldown);
	retryRow->addStretch();
	retryLayout->addLayout(retryRow);
	genPageLo->addWidget(retryGroup);

	// Analysis
	auto* analysisGroup  = new QGroupBox(tr("Analysis"), genPage);
	auto* analysisLayout = new QVBoxLayout(analysisGroup);

	m_chkSkipSubOnly = new QCheckBox(tr("Skip jobs where only subtitle tracks would be removed"), analysisGroup);
	m_chkSkipSubOnly->setToolTip(tr(
		"Subtitle tracks are tiny — removing them saves almost no space but still requires\n"
		"a full remux. Enable this to ignore those files during Analyze."));
	m_chkSkipSubOnly->setChecked(profile->skipSubtitleOnlyJobs());
	analysisLayout->addWidget(m_chkSkipSubOnly);

	m_chkWriteLog = new QCheckBox(tr("Write .mc-log file alongside each processed file"), analysisGroup);
	m_chkWriteLog->setToolTip(tr(
		"After each successful remux, writes a plain-text report next to the output file.\n"
		"Contains the filename, date, space reclaimed, removed tracks, and the exact mkvmerge command."));
	m_chkWriteLog->setChecked(profile->writeJobLog());
	analysisLayout->addWidget(m_chkWriteLog);
	genPageLo->addWidget(analysisGroup);

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

	auto* item = new QListWidgetItem(tok, m_editionTokenList);
	item->setFlags(item->flags() | Qt::ItemIsEditable);
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
	for (const QString& tok : UserProfile::defaultEditionTokens()) {
		auto* item = new QListWidgetItem(tok, m_editionTokenList);
		item->setFlags(item->flags() | Qt::ItemIsEditable);
	}
}

void McSettingsDialog::onEditionTokenEdited(QListWidgetItem* item)
{
	const QString tok = item->text().trimmed();
	if (tok.isEmpty()) {
		delete item;
		return;
	}

	for (int i = 0; i < m_editionTokenList->count(); ++i) {
		QListWidgetItem* other = m_editionTokenList->item(i);
		if (other != item && other->text().compare(tok, Qt::CaseInsensitive) == 0) {
			delete item;
			return;
		}
	}

	if (item->text() != tok) {
		const QSignalBlocker blocker(m_editionTokenList);
		item->setText(tok);
	}
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
	m_profile->setDownloadSceneNfoEnabled(m_chkDownloadSceneNfo->isChecked());
	m_profile->setAutoDownloadSceneNfo(m_chkAutoDownloadSceneNfo->isChecked());
	m_profile->setDeepDolbyVisionScanEnabled(m_chkDeepDolbyVisionScan->isChecked());
	m_profile->setOpenSubtitlesApiKey(m_editOsApiKey->text().trimmed());
	m_profile->setOpenSubtitlesUsername(m_editOsUsername->text().trimmed());
	m_profile->setOpenSubtitlesPassword(m_editOsPassword->text());
	m_profile->setAutoDownloadSubtitles(m_chkAutoDownloadSubs->isChecked());
	m_profile->setComputeSubtitleMovieHash(m_chkComputeMovieHash->isChecked());
	m_profile->setSubtitleRetryCooldownDays(m_spinRetryCooldown->value());

	QStringList editionTokens;
	for (int i = 0; i < m_editionTokenList->count(); ++i)
		editionTokens << m_editionTokenList->item(i)->text();
	m_profile->setEditionTokens(editionTokens);

	m_profile->save();

	AppSettings::instance().setValue("jobPanel/followRunning", m_chkAutoTrack->isChecked());
	AppSettings::instance().setValue("settings/includeManualDeletesInTotals",
	                                  m_chkAggregateManualDeletes->isChecked());
#ifdef Q_OS_WIN
	AppSettings::instance().setValue("settings/alwaysUseExplorerForReveal",
	                                  m_chkAlwaysUseExplorer->isChecked());
#endif
	StorageGroupSettings::setUiMaxGroup(m_spinScanGroups->value());
	PosterManager::instance().setParallelWorkers(m_spinPosterWorkers->value());
	AppSettings::instance().setValue("library/fanartOpacity", m_sliderFanartOpacity->value());

	NzbGetConfig nzbConfig;
	nzbConfig.enabled  = m_grpNzbEnabled->isChecked();
	nzbConfig.host     = m_editNzbHost->text().trimmed();
	nzbConfig.port     = m_spinNzbPort->value();
	nzbConfig.username = m_editNzbUsername->text().trimmed();
	nzbConfig.password = m_editNzbPassword->text();
	DownloadIntegrationSettings::setNzbgetConfig(nzbConfig);

	SabnzbdConfig sabConfig;
	sabConfig.enabled = m_grpSabEnabled->isChecked();
	sabConfig.host    = m_editSabHost->text().trimmed();
	sabConfig.port    = m_spinSabPort->value();
	sabConfig.apiKey  = m_editSabApiKey->text().trimmed();
	DownloadIntegrationSettings::setSabnzbdConfig(sabConfig);

	DownloadIntegrationSettings::setAutoQuickScanOnComplete(m_chkAutoQuickScan->isChecked());
	DownloadIntegrationSettings::setAutoQuickAnalyzeOnComplete(m_chkAutoQuickAnalyze->isChecked());

	DownloadClientRegistry::instance().reconfigureAll();

	QDialog::accept();
}

void McSettingsDialog::onTestNzbConnection()
{
	const QString host = m_editNzbHost->text().trimmed();
	if (host.isEmpty()) {
		m_lblNzbTestResult->setText(tr("Enter a host first."));
		return;
	}

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(host);
	url.setPort(m_spinNzbPort->value());
	url.setPath(QStringLiteral("/jsonrpc"));

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	const QString username = m_editNzbUsername->text().trimmed();
	if (!username.isEmpty()) {
		const QByteArray creds = (username + QLatin1Char(':') + m_editNzbPassword->text()).toUtf8();
		req.setRawHeader("Authorization", "Basic " + creds.toBase64());
	}

	m_lblNzbTestResult->setText(tr("Testing…"));
	m_btnNzbTestConnection->setEnabled(false);

	// Tests the credentials currently typed in the dialog, not whatever is
	// already saved — a standalone QNetworkAccessManager here (rather than
	// going through NzbGetClient, which always reads persisted settings) is
	// what lets this work before Save is clicked.
	auto* nam = new QNetworkAccessManager(this);
	QNetworkReply* reply = nam->post(req, QByteArray("{\"method\":\"status\",\"params\":[],\"id\":1}"));
	connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
		reply->deleteLater();
		nam->deleteLater();
		m_btnNzbTestConnection->setEnabled(true);

		if (reply->error() != QNetworkReply::NoError) {
			m_lblNzbTestResult->setText(tr("Failed: %1").arg(reply->errorString()));
			return;
		}
		const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
		m_lblNzbTestResult->setText(obj.contains(QStringLiteral("result"))
		    ? tr("Connected successfully.")
		    : tr("Unexpected response from server."));
	});
}

void McSettingsDialog::onTestSabConnection()
{
	const QString host = m_editSabHost->text().trimmed();
	if (host.isEmpty()) {
		m_lblSabTestResult->setText(tr("Enter a host first."));
		return;
	}

	QUrl url;
	url.setScheme(QStringLiteral("http"));
	url.setHost(host);
	url.setPort(m_spinSabPort->value());
	url.setPath(QStringLiteral("/api"));

	QUrlQuery query;
	query.addQueryItem(QStringLiteral("mode"), QStringLiteral("version"));
	query.addQueryItem(QStringLiteral("output"), QStringLiteral("json"));
	query.addQueryItem(QStringLiteral("apikey"), m_editSabApiKey->text().trimmed());
	url.setQuery(query);

	m_lblSabTestResult->setText(tr("Testing…"));
	m_btnSabTestConnection->setEnabled(false);

	// Tests the credentials currently typed in the dialog, not whatever is
	// already saved — a standalone QNetworkAccessManager here (rather than
	// going through SabnzbdClient, which always reads persisted settings) is
	// what lets this work before Save is clicked.
	auto* nam = new QNetworkAccessManager(this);
	QNetworkReply* reply = nam->get(QNetworkRequest(url));
	connect(reply, &QNetworkReply::finished, this, [this, reply, nam] {
		reply->deleteLater();
		nam->deleteLater();
		m_btnSabTestConnection->setEnabled(true);

		if (reply->error() != QNetworkReply::NoError) {
			m_lblSabTestResult->setText(tr("Failed: %1").arg(reply->errorString()));
			return;
		}
		const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
		m_lblSabTestResult->setText(obj.contains(QStringLiteral("version"))
		    ? tr("Connected successfully.")
		    : tr("Unexpected response from server."));
	});
}

} // namespace Mc
