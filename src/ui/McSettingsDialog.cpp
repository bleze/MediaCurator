#include "ui/McSettingsDialog.h"
#include "core/UserProfile.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

namespace Mc {

// ISO 639-2 codes and display names for the language picker
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

McSettingsDialog::McSettingsDialog(UserProfile* profile, QWidget* parent)
	: QDialog(parent), m_profile(profile)
{
	setWindowTitle(tr("Settings"));
	setMinimumSize(800, 540);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(10);
	root->setContentsMargins(10, 10, 10, 10);

	// ── Two-column body ───────────────────────────────────────────────────────
	auto* cols  = new QHBoxLayout;
	cols->setSpacing(10);
	auto* left  = new QVBoxLayout;
	auto* right = new QVBoxLayout;
	left->setSpacing(10);
	right->setSpacing(10);
	cols->addLayout(left,  1);
	cols->addLayout(right, 1);
	root->addLayout(cols, 1);

	// ── LEFT: Understood Languages ────────────────────────────────────────────
	auto* langGroup  = new QGroupBox(tr("Understood Languages"), this);
	auto* langLayout = new QVBoxLayout(langGroup);

	m_langList = new QListWidget(langGroup);
	m_langList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_langList->setMaximumHeight(110);
	for (const QString& code : profile->understoodLanguages()) {
		auto* item = new QListWidgetItem(displayName(code), m_langList);
		item->setData(Qt::UserRole, code);
	}
	langLayout->addWidget(m_langList);

	auto* addRow = new QHBoxLayout;
	m_langCombo  = new QComboBox(langGroup);
	for (const auto& [code, name] : kKnownLanguages)
		m_langCombo->addItem(QStringLiteral("%1 — %2").arg(code, name), code);
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

	left->addWidget(langGroup);

	// ── LEFT: Audio ───────────────────────────────────────────────────────────
	auto* audioGroup  = new QGroupBox(tr("Audio"), this);
	auto* audioLayout = new QVBoxLayout(audioGroup);

	m_chkKeepOriginalAudio = new QCheckBox(tr("Always keep audio in the file's original language"), audioGroup);
	m_chkKeepCommentary    = new QCheckBox(tr("Keep commentary tracks if in an understood language"), audioGroup);
	m_chkStereoCommentary  = new QCheckBox(tr("Treat stereo as commentary when a surround track exists"), audioGroup);
	m_chkStereoCommentary->setToolTip(tr(
		"When a file has a surround (5.1+) audio track, any stereo track in the same language "
		"is assumed to be a commentary or secondary mix — even without a title or flag indicating it. "
		"The commentary keep/remove policy above still applies."));

	m_chkKeepOriginalAudio->setChecked(profile->alwaysKeepOriginalAudio());
	m_chkKeepCommentary->setChecked(profile->keepCommentaryIfUnderstood());
	m_chkStereoCommentary->setChecked(profile->stereoAsCommentaryHeuristic());

	audioLayout->addWidget(m_chkKeepOriginalAudio);
	audioLayout->addWidget(m_chkKeepCommentary);
	audioLayout->addWidget(m_chkStereoCommentary);
	left->addWidget(audioGroup);

	// ── LEFT: Audio Format Priority ───────────────────────────────────────────
	auto* fmtGroup  = new QGroupBox(tr("Audio Format Priority"), this);
	auto* fmtLayout = new QVBoxLayout(fmtGroup);

	auto* fmtHint = new QLabel(
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
	for (const QString& id : order) {
		auto* item = new QListWidgetItem(UserProfile::audioFormatDisplayName(id), m_audioFormatList);
		item->setData(Qt::UserRole, id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(disabled.contains(id) ? Qt::Unchecked : Qt::Checked);
	}

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
	left->addWidget(fmtGroup, 1);

	// ── RIGHT: Subtitles ──────────────────────────────────────────────────────
	auto* subGroup  = new QGroupBox(tr("Subtitles"), this);
	auto* subLayout = new QVBoxLayout(subGroup);

	m_chkKeepForced      = new QCheckBox(tr("Always keep forced subtitle tracks"), subGroup);
	m_chkKeepOriginalSub = new QCheckBox(tr("Keep original-language subtitle even if not understood"), subGroup);
	m_chkKeepForced->setChecked(profile->keepForcedSubtitlesAlways());
	m_chkKeepOriginalSub->setChecked(profile->keepOriginalLanguageSubtitle());

	auto* sdhRow   = new QHBoxLayout;
	auto* sdhLabel = new QLabel(tr("SDH / hearing-impaired subtitles:"), subGroup);
	m_cmbSdhMode   = new QComboBox(subGroup);
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

	subLayout->addWidget(m_chkKeepForced);
	subLayout->addLayout(sdhRow);
	subLayout->addWidget(m_chkKeepOriginalSub);
	right->addWidget(subGroup);

	// ── RIGHT: Subtitle Format Priority ───────────────────────────────────────
	auto* subFmtGroup  = new QGroupBox(tr("Subtitle Format Priority"), this);
	auto* subFmtLayout = new QVBoxLayout(subFmtGroup);

	auto* subFmtHint = new QLabel(
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
	for (const QString& id : subOrder) {
		auto* item = new QListWidgetItem(UserProfile::subtitleFormatDisplayName(id), m_subFormatList);
		item->setData(Qt::UserRole, id);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(subDisabled.contains(id) ? Qt::Unchecked : Qt::Checked);
	}

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
	right->addWidget(subFmtGroup, 1);

	// ── RIGHT: Analysis ───────────────────────────────────────────────────────
	auto* analysisGroup  = new QGroupBox(tr("Analysis"), this);
	auto* analysisLayout = new QVBoxLayout(analysisGroup);

	m_chkRemoveMjpeg = new QCheckBox(tr("Remove embedded MJPEG cover-art video streams"), analysisGroup);
	m_chkRemoveMjpeg->setToolTip(tr(
		"Some MKV files contain an MJPEG video stream used as embedded cover art or a thumbnail. "
		"These streams are not playable content and add unnecessary size. "
		"Enable to mark them for removal during Analyze."));
	m_chkRemoveMjpeg->setChecked(profile->removeMjpegCoverArt());
	analysisLayout->addWidget(m_chkRemoveMjpeg);

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
	right->addWidget(analysisGroup);

	// ── RIGHT: Enrichment ─────────────────────────────────────────────────────
	auto* enrichGroup  = new QGroupBox(tr("Enrichment"), this);
	auto* enrichLayout = new QVBoxLayout(enrichGroup);

	auto* tmdbRow   = new QHBoxLayout;
	auto* tmdbLabel = new QLabel(tr("TMDB API Key:"), enrichGroup);
	m_editTmdbKey   = new QLineEdit(enrichGroup);
	m_editTmdbKey->setPlaceholderText(tr("Leave empty to skip poster lookup"));
	m_editTmdbKey->setText(profile->tmdbApiKey());
	tmdbRow->addWidget(tmdbLabel);
	tmdbRow->addWidget(m_editTmdbKey, 1);
	enrichLayout->addLayout(tmdbRow);

	auto* tmdbHint = new QLabel(
		tr("Get a free key at <a href=\"https://www.themoviedb.org/settings/api\">themoviedb.org</a>."),
		enrichGroup);
	tmdbHint->setOpenExternalLinks(true);
	enrichLayout->addWidget(tmdbHint);
	right->addWidget(enrichGroup);
	right->addStretch();

	// ── Buttons ───────────────────────────────────────────────────────────────
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Save"));
	connect(buttons, &QDialogButtonBox::accepted, this, &McSettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

void McSettingsDialog::onAddLanguage()
{
	const int idx  = m_langCombo->currentIndex();
	const QString code = (idx >= 0)
		? m_langCombo->itemData(idx).toString()
		: m_langCombo->currentText().trimmed().toLower().left(3);

	if (code.isEmpty()) return;

	// Check for duplicates
	for (int i = 0; i < m_langList->count(); ++i)
		if (m_langList->item(i)->data(Qt::UserRole).toString() == code) return;

	auto* item = new QListWidgetItem(displayName(code), m_langList);
	item->setData(Qt::UserRole, code);
}

void McSettingsDialog::onRemoveLanguage()
{
	const auto selected = m_langList->selectedItems();
	if (selected.isEmpty()) return;
	delete selected.first();
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
	m_profile->setWriteJobLog(m_chkWriteLog->isChecked());
	m_profile->setTmdbApiKey(m_editTmdbKey->text().trimmed());
	m_profile->save();

	QDialog::accept();
}

} // namespace Mc
