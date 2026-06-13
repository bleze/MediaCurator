#include "ui/McWhatIfDialog.h"
#include "ui/McJobStatsBar.h"

#include <algorithm>

#include <QColor>
#include <QDialogButtonBox>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

namespace {

// ── Stats ─────────────────────────────────────────────────────────────────────

struct SimStats {
	int    totalFiles    = 0;
	int    filesAffected = 0;
	int    audioRemoved  = 0;
	int    subRemoved    = 0;
	int    videoRemoved  = 0;
	qint64 savedBytes    = 0;

	int reasonLanguage   = 0;
	int reasonRedundancy = 0;
	int reasonSdh        = 0;
	int reasonCommentary = 0;
	int reasonMjpeg      = 0;
	int reasonOther      = 0;

	QMap<QString, int> audioByLang;
	QMap<QString, int> subByLang;
};

static SimStats computeStats(const QList<Mc::FileDecision>& decisions)
{
	SimStats s;
	s.totalFiles = decisions.size();

	for (const Mc::FileDecision& fd : decisions) {
		if (fd.removalCount() == 0) continue;
		++s.filesAffected;

		for (const Mc::TrackDecision& td : fd.tracks) {
			if (td.decision != Mc::Decision::Remove) continue;
			const Mc::StreamRecord& sr = td.stream;

			if (sr.codecType == QLatin1String("audio")) {
				++s.audioRemoved;
				if (!sr.language.isEmpty() && sr.language != QLatin1String("und"))
					++s.audioByLang[sr.language];
			} else if (sr.codecType == QLatin1String("subtitle")) {
				++s.subRemoved;
				if (!sr.language.isEmpty() && sr.language != QLatin1String("und"))
					++s.subByLang[sr.language];
			} else if (sr.codecType == QLatin1String("video")) {
				++s.videoRemoved;
			}

			const QString& r = td.reason;
			if (r.contains(QLatin1String("MJPEG"), Qt::CaseInsensitive)
					|| r.contains(QLatin1String("cover-art"), Qt::CaseInsensitive))
				++s.reasonMjpeg;
			else if (r.contains(QLatin1String("Redundant"), Qt::CaseInsensitive))
				++s.reasonRedundancy;
			else if (r.contains(QLatin1String("SDH"), Qt::CaseInsensitive)
					|| r.contains(QLatin1String("superseded"), Qt::CaseInsensitive))
				++s.reasonSdh;
			else if (r.contains(QLatin1String("Commentary"), Qt::CaseInsensitive))
				++s.reasonCommentary;
			else if (r.contains(QLatin1String("not understood"), Qt::CaseInsensitive)
					|| r.contains(QLatin1String("not original"), Qt::CaseInsensitive)
					|| r.contains(QLatin1String("not in understood"), Qt::CaseInsensitive))
				++s.reasonLanguage;
			else
				++s.reasonOther;
		}
		s.savedBytes += fd.estimatedSavingBytes();
	}
	return s;
}

// ── UI helpers ────────────────────────────────────────────────────────────────

static QString langDisplayName(const QString& code)
{
	static const QHash<QString, QString> names = {
		{"mul","Multiple"}, {"ara","Arabic"}, {"zho","Chinese"}, {"hrv","Croatian"},
		{"ces","Czech"},    {"dan","Danish"},  {"nld","Dutch"},   {"eng","English"},
		{"fin","Finnish"},  {"fra","French"},  {"deu","German"},  {"ell","Greek"},
		{"heb","Hebrew"},   {"hun","Hungarian"},{"ind","Indonesian"},{"ita","Italian"},
		{"jpn","Japanese"}, {"kor","Korean"},  {"nor","Norwegian"},{"pol","Polish"},
		{"por","Portuguese"},{"ron","Romanian"},{"rus","Russian"}, {"srp","Serbian"},
		{"slk","Slovak"},   {"spa","Spanish"}, {"swe","Swedish"}, {"tha","Thai"},
		{"tur","Turkish"},  {"ukr","Ukrainian"},{"vie","Vietnamese"},
	};
	const QString n = names.value(code);
	return n.isEmpty() ? code : QStringLiteral("%1 (%2)").arg(n, code);
}

class MiniBar : public QWidget
{
public:
	MiniBar(int value, int max, const QColor& color, QWidget* parent = nullptr)
		: QWidget(parent), m_value(value), m_max(max), m_color(color)
	{
		setFixedHeight(9);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(128, 128, 128, 45));
		p.drawRoundedRect(rect(), 3, 3);
		if (m_max > 0 && m_value > 0) {
			QRect fill = rect();
			fill.setWidth(qMax(10, int(rect().width() * double(m_value) / m_max)));
			p.setBrush(m_color);
			p.drawRoundedRect(fill, 3, 3);
		}
	}
private:
	int    m_value;
	int    m_max;
	QColor m_color;
};

class CollapsibleSection : public QWidget
{
public:
	CollapsibleSection(const QString& title, bool startExpanded, QWidget* parent = nullptr)
		: QWidget(parent)
	{
		auto* vbox = new QVBoxLayout(this);
		vbox->setContentsMargins(0, 0, 0, 0);
		vbox->setSpacing(0);

		m_toggle = new QPushButton(this);
		m_toggle->setFlat(true);
		m_toggle->setCursor(Qt::PointingHandCursor);
		m_toggle->setStyleSheet(
		    "QPushButton { text-align: left; padding: 4px 2px; font-weight: bold; border: none; }"
		    "QPushButton:hover { color: palette(highlight); }");

		m_container = new QWidget(this);
		m_rows = new QVBoxLayout(m_container);
		m_rows->setContentsMargins(0, 4, 0, 6);
		m_rows->setSpacing(0);

		vbox->addWidget(m_toggle);
		vbox->addWidget(m_container);

		m_toggle->setText(title);
		setExpanded(startExpanded);
		connect(m_toggle, &QPushButton::clicked, this, [this]() { setExpanded(!m_expanded); });
	}

	QVBoxLayout* rowLayout() { return m_rows; }

private:
	void setExpanded(bool expanded)
	{
		m_expanded = expanded;
		m_container->setVisible(expanded);
		m_toggle->setText((expanded ? QStringLiteral("▼  ") : QStringLiteral("▶  ")) + m_title);
	}

	QPushButton* m_toggle    = nullptr;
	QWidget*     m_container = nullptr;
	QVBoxLayout* m_rows      = nullptr;
	QString      m_title;
	bool         m_expanded  = true;
};

static QFrame* separator(QWidget* parent)
{
	auto* f = new QFrame(parent);
	f->setFrameShape(QFrame::HLine);
	f->setFrameShadow(QFrame::Sunken);
	return f;
}

static void addBarRow(QVBoxLayout* layout, const QString& label, int value, int max,
                      const QColor& color, QWidget* parent)
{
	auto* row  = new QWidget(parent);
	auto* hbox = new QHBoxLayout(row);
	hbox->setContentsMargins(0, 2, 0, 2);
	hbox->setSpacing(8);

	auto* nameLbl = new QLabel(label, row);
	nameLbl->setFixedWidth(200);
	nameLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

	auto* bar = new MiniBar(value, max, color, row);

	auto* cntLbl = new QLabel(QString::number(value), row);
	cntLbl->setFixedWidth(40);
	cntLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	QFont bf = cntLbl->font(); bf.setBold(true); cntLbl->setFont(bf);

	hbox->addWidget(nameLbl);
	hbox->addWidget(bar, 1);
	hbox->addWidget(cntLbl);
	layout->addWidget(row);
}

} // anonymous namespace

namespace Mc {

McWhatIfDialog::McWhatIfDialog(int totalFiles, QWidget* parent)
	: QDialog(parent)
	, m_totalFiles(totalFiles)
{
	setWindowTitle(tr("Policy Simulation"));
	setMinimumWidth(660);
	setAttribute(Qt::WA_DeleteOnClose);

	auto* root = new QVBoxLayout(this);
	root->setSpacing(0);
	root->setContentsMargins(0, 0, 0, 0);

	// ── Progress page ─────────────────────────────────────────────────────────
	m_progressPage = new QWidget(this);
	auto* pl = new QVBoxLayout(m_progressPage);
	pl->setContentsMargins(24, 20, 24, 16);
	pl->setSpacing(8);

	auto* titleLbl = new QLabel(
	    tr("Simulating policy across %1 file(s)…").arg(totalFiles), m_progressPage);
	QFont f = titleLbl->font();
	f.setPointSize(f.pointSize() + 1);
	titleLbl->setFont(f);

	m_progressBar = new QProgressBar(m_progressPage);
	m_progressBar->setRange(0, qMax(1, totalFiles));
	m_progressBar->setValue(0);

	m_fileLabel = new QLabel(m_progressPage);
	m_fileLabel->setEnabled(false);
	m_fileLabel->setTextInteractionFlags(Qt::NoTextInteraction);

	pl->addWidget(titleLbl);
	pl->addWidget(m_progressBar);
	pl->addWidget(m_fileLabel);
	pl->addStretch();
	root->addWidget(m_progressPage);

	// ── Results page (populated later) ────────────────────────────────────────
	m_resultsPage = new QWidget(this);
	m_resultsPage->setVisible(false);
	root->addWidget(m_resultsPage, 1);

	// ── Button row ────────────────────────────────────────────────────────────
	auto* btnArea = new QWidget(this);
	auto* btnRow  = new QHBoxLayout(btnArea);
	btnRow->setContentsMargins(12, 8, 12, 10);
	btnRow->setSpacing(8);

	m_cancelBtn  = new QPushButton(tr("Cancel"), btnArea);
	m_closeBtn   = new QPushButton(tr("Close"),  btnArea);
	m_analyzeBtn = new QPushButton(tr("Analyze Library →"), btnArea);
	m_closeBtn->setVisible(false);
	m_analyzeBtn->setVisible(false);

	// Make "Analyze Library →" the default / highlighted button
	m_analyzeBtn->setDefault(true);
	QPalette ap = m_analyzeBtn->palette();
	ap.setColor(QPalette::Button, ap.color(QPalette::Highlight));
	ap.setColor(QPalette::ButtonText, ap.color(QPalette::HighlightedText));
	m_analyzeBtn->setPalette(ap);

	btnRow->addStretch();
	btnRow->addWidget(m_cancelBtn);
	btnRow->addWidget(m_closeBtn);
	btnRow->addWidget(m_analyzeBtn);
	root->addWidget(btnArea);

	connect(m_cancelBtn,  &QPushButton::clicked, this, &QDialog::reject);
	connect(m_closeBtn,   &QPushButton::clicked, this, &QDialog::reject);
	connect(m_analyzeBtn, &QPushButton::clicked, this, [this]() {
		emit analyzeRequested();
		accept();
	});

	resize(660, 180);
	centerOnParent();
}

void McWhatIfDialog::centerOnParent()
{
	if (QWidget* p = parentWidget()) {
		const QRect pg = p->geometry();
		move(pg.center().x() - width() / 2, pg.center().y() - height() / 2);
	}
}

void McWhatIfDialog::onProgress(int current, int total, const QString& filename)
{
	m_progressBar->setMaximum(qMax(1, total));
	m_progressBar->setValue(current);
	m_fileLabel->setText(filename);
}

void McWhatIfDialog::onFinished(const QList<FileDecision>& decisions)
{
	m_progressPage->setVisible(false);
	m_cancelBtn->setVisible(false);
	m_closeBtn->setVisible(true);
	m_analyzeBtn->setVisible(decisions.size() > 0);

	buildResults(decisions);
	m_resultsPage->setVisible(true);
	resize(820, 560);
	centerOnParent();
}

void McWhatIfDialog::buildResults(const QList<FileDecision>& decisions)
{
	const SimStats s = computeStats(decisions);
	const int totalTracks = s.audioRemoved + s.subRemoved + s.videoRemoved;

	auto* root   = new QVBoxLayout(m_resultsPage);
	auto* scroll = new QScrollArea(m_resultsPage);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* body   = new QWidget(scroll);
	auto* layout = new QVBoxLayout(body);
	layout->setSpacing(8);
	layout->setContentsMargins(16, 12, 16, 12);
	scroll->setWidget(body);
	root->addWidget(scroll);
	root->setContentsMargins(0, 0, 0, 0);

	if (s.filesAffected == 0) {
		auto* lbl = new QLabel(
		    tr("No tracks would be removed under the current policy settings.\n"
		       "Try adjusting your understood languages or codec preferences in Settings."), body);
		lbl->setAlignment(Qt::AlignCenter);
		lbl->setWordWrap(true);
		layout->addStretch();
		layout->addWidget(lbl);
		layout->addStretch();
		m_analyzeBtn->setVisible(false);
		return;
	}

	// ── Header stats ──────────────────────────────────────────────────────────
	QList<McJobStatsBar::StatItem> items;
	items << McJobStatsBar::StatItem{
	    QStringLiteral("%1 / %2").arg(s.filesAffected).arg(s.totalFiles),
	    tr("files\naffected")};
	items << McJobStatsBar::StatItem{QString::number(totalTracks), tr("tracks\nremoved")};
	if (s.audioRemoved  > 0) items << McJobStatsBar::StatItem{QString::number(s.audioRemoved),  tr("audio\nremoved")};
	if (s.subRemoved    > 0) items << McJobStatsBar::StatItem{QString::number(s.subRemoved),    tr("subtitle\nremoved")};
	if (s.videoRemoved  > 0) items << McJobStatsBar::StatItem{QString::number(s.videoRemoved),  tr("cover art\nremoved")};
	layout->addWidget(new McJobStatsBar(items, s.savedBytes, body));
	layout->addWidget(separator(body));

	// ── Removal reasons ───────────────────────────────────────────────────────
	const int maxReason = std::max({s.reasonLanguage, s.reasonRedundancy,
	                                s.reasonSdh, s.reasonCommentary,
	                                s.reasonMjpeg, s.reasonOther});
	if (maxReason > 0) {
		auto* sec = new CollapsibleSection(tr("Removal reasons"), true, body);
		const QColor colLang ("#4a8ccc");
		const QColor colRed  ("#6aaa60");
		const QColor colSdh  ("#c08040");
		const QColor colCom  ("#9060b0");
		const QColor colMjpeg("#888888");
		const QColor colOther("#666666");
		if (s.reasonLanguage   > 0) addBarRow(sec->rowLayout(), tr("Language policy"),        s.reasonLanguage,   maxReason, colLang,  sec);
		if (s.reasonRedundancy > 0) addBarRow(sec->rowLayout(), tr("Codec redundancy"),       s.reasonRedundancy, maxReason, colRed,   sec);
		if (s.reasonSdh        > 0) addBarRow(sec->rowLayout(), tr("SDH subtitle"),           s.reasonSdh,        maxReason, colSdh,   sec);
		if (s.reasonCommentary > 0) addBarRow(sec->rowLayout(), tr("Commentary"),             s.reasonCommentary, maxReason, colCom,   sec);
		if (s.reasonMjpeg      > 0) addBarRow(sec->rowLayout(), tr("Cover art"),               s.reasonMjpeg,      maxReason, colMjpeg, sec);
		if (s.reasonOther      > 0) addBarRow(sec->rowLayout(), tr("Other"),                  s.reasonOther,      maxReason, colOther, sec);
		layout->addWidget(sec);
		layout->addWidget(separator(body));
	}

	// ── Audio by language ─────────────────────────────────────────────────────
	if (!s.audioByLang.isEmpty()) {
		const int maxAL = *std::max_element(s.audioByLang.cbegin(), s.audioByLang.cend());
		QList<QPair<int,QString>> sorted;
		for (auto it = s.audioByLang.cbegin(); it != s.audioByLang.cend(); ++it)
			sorted.append({it.value(), it.key()});
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first > b.first; });

		auto* sec = new CollapsibleSection(
		    tr("Audio removed by language  (%1)").arg(sorted.size()), sorted.size() <= 5, body);
		for (const auto& [cnt, lang] : sorted)
			addBarRow(sec->rowLayout(), langDisplayName(lang), cnt, maxAL, QColor("#4a8ccc"), sec);
		layout->addWidget(sec);
		layout->addWidget(separator(body));
	}

	// ── Subtitles by language ─────────────────────────────────────────────────
	if (!s.subByLang.isEmpty()) {
		const int maxSL = *std::max_element(s.subByLang.cbegin(), s.subByLang.cend());
		QList<QPair<int,QString>> sorted;
		for (auto it = s.subByLang.cbegin(); it != s.subByLang.cend(); ++it)
			sorted.append({it.value(), it.key()});
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first > b.first; });

		auto* sec = new CollapsibleSection(
		    tr("Subtitles removed by language  (%1)").arg(sorted.size()), sorted.size() <= 5, body);
		for (const auto& [cnt, lang] : sorted)
			addBarRow(sec->rowLayout(), langDisplayName(lang), cnt, maxSL, QColor("#c08040"), sec);
		layout->addWidget(sec);
	}

	layout->addStretch();
}

} // namespace Mc
