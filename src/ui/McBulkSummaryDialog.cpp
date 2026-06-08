#include "ui/McBulkSummaryDialog.h"
#include "ui/McJobListModel.h"
#include "ui/McJobStatsBar.h"
#include "core/DatabaseManager.h"

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

static bool isLosslessAudio(const Mc::StreamRecord& s)
{
	if (s.codecType != QLatin1String("audio")) return false;
	const QString cn = s.codecName.toLower();
	if (cn == QLatin1String("flac")   || cn == QLatin1String("alac")
	 || cn == QLatin1String("truehd") || cn == QLatin1String("mlp")
	 || cn == QLatin1String("tta")    || cn == QLatin1String("wavpack")
	 || cn.startsWith(QLatin1String("pcm_")))
		return true;
	if (cn == QLatin1String("dts")) {
		const QString cp = s.codecProfile.toUpper();
		return cp.contains(QLatin1String("MA")) || cp.contains(QLatin1String("HRA"));
	}
	return false;
}

static int pcmBitDepth(const QString& cn)
{
	const int u = cn.indexOf('_');
	if (u < 0 || u + 2 >= cn.size()) return 0;
	int i = u + 1;
	if (!cn[i].isLetter()) return 0;
	++i;
	const int start = i;
	while (i < cn.size() && cn[i].isDigit()) ++i;
	return (i > start) ? cn.mid(start, i - start).toInt() : 0;
}

static qint64 losslessBytesPerSecPerChannel(const Mc::StreamRecord& s)
{
	const QString cn = s.codecName.toLower();
	if (cn.startsWith(QLatin1String("pcm_"))) {
		if (s.sampleRate <= 0) return -1;
		const int depth = pcmBitDepth(cn);
		return (depth > 0) ? static_cast<qint64>(s.sampleRate) * depth / 8 : -1;
	}
	if (cn == QLatin1String("truehd") || cn == QLatin1String("mlp")) return 87'500;
	if (cn == QLatin1String("flac"))    return 56'250;
	if (cn == QLatin1String("alac"))    return 50'000;
	if (cn == QLatin1String("tta") || cn == QLatin1String("wavpack")) return 56'250;
	if (cn == QLatin1String("dts")) {
		const QString cp = s.codecProfile.toUpper();
		if (cp.contains(QLatin1String("MA")))  return 87'500;
		if (cp.contains(QLatin1String("HRA"))) return 62'500;
	}
	return -1;
}

static qint64 estimateStreamBytes(const Mc::StreamRecord& s,
                                   const QList<Mc::StreamRecord>&,
                                   qint64,
                                   double fileDurationSec)
{
	if (fileDurationSec <= 0) return -1;
	if (!isLosslessAudio(s)) {
		if (s.bitRate > 0)
			return static_cast<qint64>(s.bitRate / 8.0 * fileDurationSec);
		if (s.codecType == QLatin1String("subtitle"))
			return 256LL * 1024;
		return 0;
	}
	const qint64 bpsPerChannel = losslessBytesPerSecPerChannel(s);
	if (bpsPerChannel < 0) return -1;
	return static_cast<qint64>(bpsPerChannel * qMax(s.channels, 1) * fileDurationSec);
}

} // anonymous namespace

namespace Mc {

// ── Mini proportional bar ─────────────────────────────────────────────────────

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

// ── Collapsible section ───────────────────────────────────────────────────────

class CollapsibleSection : public QWidget
{
public:
	CollapsibleSection(const QString& title, bool startExpanded, QWidget* parent = nullptr)
		: QWidget(parent), m_title(title)
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

		setExpanded(startExpanded);

		connect(m_toggle, &QPushButton::clicked, this, [this]() {
			setExpanded(!m_expanded);
		});
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

// ── Stats struct ──────────────────────────────────────────────────────────────

struct BulkStats {
	int    filesAffected        = 0;
	int    audioRemoved         = 0;
	int    subtitleRemoved      = 0;
	int    videoRemoved         = 0;
	qint64 estimatedSavedBytes  = 0;

	int reasonLanguage   = 0;
	int reasonRedundancy = 0;
	int reasonSdh        = 0;
	int reasonCommentary = 0;
	int reasonMjpeg      = 0;
	int reasonOther      = 0;

	QMap<QString, int> audioByLang;
	QMap<QString, int> subtitleByLang;
};


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

static BulkStats computeStats()
{
	auto& db = DatabaseManager::instance();
	const auto jobs = db.allJobs();

	BulkStats s;

	for (const JobRecord& job : jobs) {
		if (job.status != QStringLiteral("proposed")) continue;
		++s.filesAffected;

		// Determine removed streams from kept-stream delta
		const auto allStreams  = db.streamsForFile(job.fileId);
		const auto keptStreams = McJobListModel::computeKeptStreams(allStreams, job.commandArgsJson);

		const auto fileOpt = db.fileById(job.fileId);
		const double durationSec = fileOpt ? fileOpt->durationSec : 0.0;

		QSet<int> keptIdx;
		for (const auto& sr : keptStreams) keptIdx.insert(sr.streamIndex);

		for (const StreamRecord& sr : allStreams) {
			if (keptIdx.contains(sr.streamIndex)) continue;
			if (sr.codecType == QStringLiteral("audio")) {
				++s.audioRemoved;
				if (!sr.language.isEmpty() && sr.language != QStringLiteral("und"))
					++s.audioByLang[sr.language];
			} else if (sr.codecType == QStringLiteral("subtitle")) {
				++s.subtitleRemoved;
				if (!sr.language.isEmpty() && sr.language != QStringLiteral("und"))
					++s.subtitleByLang[sr.language];
			} else if (sr.codecType == QStringLiteral("video")) {
				++s.videoRemoved;
			}

			const qint64 est = estimateStreamBytes(
			    sr, allStreams,
			    fileOpt ? fileOpt->sizeBytes : 0LL, durationSec);
			if (est > 0) s.estimatedSavedBytes += est;
		}

		// Bucket removal reasons from description text
		for (const QString& line : job.descriptionText.split('\n', Qt::SkipEmptyParts)) {
			// Each line: "  [TYPE] codec (lang) ... — reason"
			const int dash = line.indexOf(QStringLiteral(" — "));
			const QString reason = (dash >= 0) ? line.mid(dash + 3) : QString();

			if (reason.contains(QStringLiteral("MJPEG"), Qt::CaseInsensitive)
					|| reason.contains(QStringLiteral("cover-art"), Qt::CaseInsensitive)) {
				++s.reasonMjpeg;
			} else if (reason.contains(QStringLiteral("Redundant"), Qt::CaseInsensitive)) {
				++s.reasonRedundancy;
			} else if (reason.contains(QStringLiteral("SDH"), Qt::CaseInsensitive)
					|| reason.contains(QStringLiteral("superseded by"), Qt::CaseInsensitive)) {
				++s.reasonSdh;
			} else if (reason.contains(QStringLiteral("Commentary"), Qt::CaseInsensitive)) {
				++s.reasonCommentary;
			} else if (reason.contains(QStringLiteral("not understood"), Qt::CaseInsensitive)
					|| reason.contains(QStringLiteral("not in understood"), Qt::CaseInsensitive)
					|| reason.contains(QStringLiteral("not original"), Qt::CaseInsensitive)) {
				++s.reasonLanguage;
			} else {
				++s.reasonOther;
			}
		}
	}

	return s;
}

// ── Layout helpers ─────────────────────────────────────────────────────────────

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
	auto* row    = new QWidget(parent);
	auto* hbox   = new QHBoxLayout(row);
	hbox->setContentsMargins(0, 2, 0, 2);
	hbox->setSpacing(8);

	auto* nameLbl = new QLabel(label, row);
	nameLbl->setFixedWidth(200);
	nameLbl->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

	auto* bar = new MiniBar(value, max, color, row);

	auto* countLbl = new QLabel(QString::number(value), row);
	countLbl->setFixedWidth(40);
	countLbl->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	QFont bf = countLbl->font();
	bf.setBold(true);
	countLbl->setFont(bf);

	hbox->addWidget(nameLbl);
	hbox->addWidget(bar, 1);
	hbox->addWidget(countLbl);
	layout->addWidget(row);
}

// ── Dialog ────────────────────────────────────────────────────────────────────

McBulkSummaryDialog::McBulkSummaryDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Analysis Summary"));
	setMinimumSize(640, 440);
	resize(780, 600);
	setAttribute(Qt::WA_DeleteOnClose);

	const BulkStats s = computeStats();
	const int totalTracks = s.audioRemoved + s.subtitleRemoved + s.videoRemoved;

	auto* root   = new QVBoxLayout(this);
	auto* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto* body   = new QWidget(scroll);
	auto* layout = new QVBoxLayout(body);
	layout->setSpacing(8);
	layout->setContentsMargins(16, 12, 16, 12);
	scroll->setWidget(body);
	root->addWidget(scroll, 1);

	// ── Stat cells ────────────────────────────────────────────────────────────
	if (s.filesAffected == 0) {
		layout->addWidget(new QLabel(tr("No proposed jobs found — run Analyze Library first."), body));
		layout->addStretch();
	} else {
		QList<McJobStatsBar::StatItem> statItems;
		statItems << McJobStatsBar::StatItem{QString::number(s.filesAffected), tr("files\naffected")};
		statItems << McJobStatsBar::StatItem{QString::number(totalTracks),     tr("tracks\nremoved")};
		if (s.audioRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(s.audioRemoved),    tr("audio\nremoved")};
		if (s.subtitleRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(s.subtitleRemoved), tr("subtitle\nremoved")};
		if (s.videoRemoved > 0)
			statItems << McJobStatsBar::StatItem{QString::number(s.videoRemoved),    tr("MJPEG\nremoved")};
		layout->addWidget(new McJobStatsBar(statItems, s.estimatedSavedBytes, body));
		layout->addWidget(separator(body));

		// ── Removal reasons ───────────────────────────────────────────────────
		const int maxReason = std::max({s.reasonLanguage, s.reasonRedundancy,
		                               s.reasonSdh, s.reasonCommentary,
		                               s.reasonMjpeg, s.reasonOther});
		if (maxReason > 0) {
			auto* sec = new CollapsibleSection(tr("Removal reasons"), true, body);
			layout->addWidget(sec);

			const QColor colLang ("#4a8ccc");
			const QColor colRed  ("#6aaa60");
			const QColor colSdh  ("#c08040");
			const QColor colCom  ("#9060b0");
			const QColor colMjpeg("#888888");
			const QColor colOther("#666666");

			if (s.reasonLanguage   > 0) addBarRow(sec->rowLayout(), tr("Language policy"),        s.reasonLanguage,   maxReason, colLang,  sec);
			if (s.reasonRedundancy > 0) addBarRow(sec->rowLayout(), tr("Codec redundancy"),       s.reasonRedundancy, maxReason, colRed,   sec);
			if (s.reasonSdh        > 0) addBarRow(sec->rowLayout(), tr("SDH (no regular track)"), s.reasonSdh,        maxReason, colSdh,   sec);
			if (s.reasonCommentary > 0) addBarRow(sec->rowLayout(), tr("Commentary"),              s.reasonCommentary, maxReason, colCom,   sec);
			if (s.reasonMjpeg      > 0) addBarRow(sec->rowLayout(), tr("MJPEG cover art"),         s.reasonMjpeg,      maxReason, colMjpeg, sec);
			if (s.reasonOther      > 0) addBarRow(sec->rowLayout(), tr("Other"),                   s.reasonOther,      maxReason, colOther, sec);
			layout->addWidget(separator(body));
		}

		// ── Audio by language ─────────────────────────────────────────────────
		if (!s.audioByLang.isEmpty()) {
			const int maxAL = *std::max_element(s.audioByLang.cbegin(), s.audioByLang.cend());
			QList<QPair<int,QString>> sorted;
			for (auto it = s.audioByLang.cbegin(); it != s.audioByLang.cend(); ++it)
				sorted.append({ it.value(), it.key() });
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first > b.first; });

			const bool bigList = sorted.size() > 5;
			auto* sec = new CollapsibleSection(
			    tr("Audio removed by language  (%1)").arg(sorted.size()),
			    !bigList, body);
			layout->addWidget(sec);
			for (const auto& [count, lang] : sorted)
				addBarRow(sec->rowLayout(), langDisplayName(lang), count, maxAL, QColor("#4a8ccc"), sec);
			layout->addWidget(separator(body));
		}

		// ── Subtitle by language ──────────────────────────────────────────────
		if (!s.subtitleByLang.isEmpty()) {
			const int maxSL = *std::max_element(s.subtitleByLang.cbegin(), s.subtitleByLang.cend());
			QList<QPair<int,QString>> sorted;
			for (auto it = s.subtitleByLang.cbegin(); it != s.subtitleByLang.cend(); ++it)
				sorted.append({ it.value(), it.key() });
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b){ return a.first > b.first; });

			const bool bigList = sorted.size() > 5;
			auto* sec = new CollapsibleSection(
			    tr("Subtitles removed by language  (%1)").arg(sorted.size()),
			    !bigList, body);
			layout->addWidget(sec);
			for (const auto& [count, lang] : sorted)
				addBarRow(sec->rowLayout(), langDisplayName(lang), count, maxSL, QColor("#c08040"), sec);
		}
	}

	layout->addStretch();

	// ── Buttons ───────────────────────────────────────────────────────────────
	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

} // namespace Mc
