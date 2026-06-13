#include "ui/McLegendDialog.h"
#include "ui/McCardDelegate.h"
#include "ui/McLanguageFlags.h"

#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QVBoxLayout>

namespace Mc {

namespace {

// Renders a badge pill into a label using the exact card drawing code.
QLabel* badgeLabel(QWidget* parent, const QString& text, const QString& codecType,
                   const QString& flagLang = {}, bool removed = false)
{
	auto* lbl = new QLabel(parent);
	lbl->setPixmap(McCardDelegate::badgePixmap(text, codecType, parent->font(),
	                                           parent->devicePixelRatioF(),
	                                           flagLang, removed));
	return lbl;
}

QLabel* glyphLabel(QWidget* parent, const QString& glyph)
{
	auto* lbl = new QLabel(glyph, parent);
	QFont f = parent->font();
	f.setPointSizeF(f.pointSizeF() * 1.15);
	lbl->setFont(f);
	return lbl;
}

// Hyperlink to a Grokipedia article for deeper reading on a format.
QString grok(const QString& article, const QString& label)
{
	return QStringLiteral("<a href=\"https://grokipedia.com/page/%1\" "
	                      "style=\"text-decoration:none;\">%2</a>")
	       .arg(article, label);
}

// One legend entry: sample badge(s) in column 0, description in column 1.
void addRow(QGridLayout* grid, const QList<QWidget*>& samples, const QString& text)
{
	const int row = grid->rowCount();
	auto* holder = new QWidget;
	auto* h = new QHBoxLayout(holder);
	h->setContentsMargins(0, 0, 0, 0);
	h->setSpacing(4);
	for (QWidget* s : samples)
		h->addWidget(s);
	h->addStretch();
	grid->addWidget(holder, row, 0, Qt::AlignLeft | Qt::AlignVCenter);
	auto* lbl = new QLabel(text);
	lbl->setTextFormat(Qt::RichText);
	lbl->setOpenExternalLinks(true);
	grid->addWidget(lbl, row, 1);
}

} // anonymous namespace

McLegendDialog::McLegendDialog(QWidget* parent)
	: QDialog(parent)
{
	setWindowTitle(tr("Legend"));

	auto* root = new QVBoxLayout(this);
	root->setContentsMargins(12, 12, 12, 12);
	root->setSpacing(10);

	auto* intro = new QLabel(tr("Tracks are shown as color-coded badges — the same pills you see "
	                            "on the library cards, the job queue and the preview dialog. "
	                            "Linked names open on grokipedia.com."), this);
	intro->setWordWrap(true);
	root->addWidget(intro);

	auto makeGroup = [this](const QString& title, QGridLayout*& gridOut) {
		auto* box  = new QGroupBox(title, this);
		auto* grid = new QGridLayout(box);
		grid->setContentsMargins(10, 6, 10, 8);
		grid->setHorizontalSpacing(14);
		grid->setVerticalSpacing(6);
		grid->setColumnStretch(1, 1);
		gridOut = grid;
		return box;
	};

	QGridLayout* g = nullptr;

	// ── Video ─────────────────────────────────────────────────────────────────
	auto* videoBox = makeGroup(tr("Video"), g);
	addRow(g, { badgeLabel(this, QStringLiteral("H.264"), QStringLiteral("video")) },
	       tr("%1 — the standard HD codec")
	           .arg(grok(QStringLiteral("Advanced_Video_Coding"), tr("AVC"))));
	addRow(g, { badgeLabel(this, QStringLiteral("H.265"), QStringLiteral("video")) },
	       tr("%1 — 4K/UHD; roughly half the size of H.264")
	           .arg(grok(QStringLiteral("High_Efficiency_Video_Coding"), tr("HEVC"))));
	addRow(g, { badgeLabel(this, QStringLiteral("AV1"), QStringLiteral("video")),
	            badgeLabel(this, QStringLiteral("VP9"), QStringLiteral("video")) },
	       tr("Royalty-free codecs (%1, %2), common in web rips")
	           .arg(grok(QStringLiteral("AV1"), QStringLiteral("AV1")),
	                grok(QStringLiteral("VP9"), QStringLiteral("VP9"))));
	addRow(g, { badgeLabel(this, QStringLiteral("MPEG-2"), QStringLiteral("video")),
	            badgeLabel(this, QStringLiteral("MPEG-4"), QStringLiteral("video")) },
	       tr("Legacy codecs (%1, %2) — DVD and older rips")
	           .arg(grok(QStringLiteral("MPEG-2"), QStringLiteral("MPEG-2")),
	                grok(QStringLiteral("MPEG-4"), QStringLiteral("MPEG-4"))));
	addRow(g, { badgeLabel(this, QStringLiteral("H.265  3840\xC3\x97" "2160  HDR10"),
	                       QStringLiteral("video")) },
	       tr("Resolution and %1 format follow the codec")
	           .arg(grok(QStringLiteral("High-dynamic-range_television"), tr("HDR"))));

	// ── Audio ─────────────────────────────────────────────────────────────────
	auto* audioBox = makeGroup(tr("Audio"), g);
	addRow(g, { badgeLabel(this, QStringLiteral("Atmos"), QStringLiteral("audio")) },
	       tr("%1 — object-based, on a TrueHD core, lossless")
	           .arg(grok(QStringLiteral("Dolby_Atmos"), tr("Dolby Atmos"))));
	addRow(g, { badgeLabel(this, QStringLiteral("TrueHD"), QStringLiteral("audio")) },
	       tr("%1 — lossless")
	           .arg(grok(QStringLiteral("Dolby_TrueHD"), tr("Dolby TrueHD"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DTS:X"), QStringLiteral("audio")) },
	       tr("%1 — object-based, on a DTS-HD MA core")
	           .arg(grok(QStringLiteral("DTS_(sound_system)"), QStringLiteral("DTS:X"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DTS-HD MA"), QStringLiteral("audio")) },
	       tr("%1 — lossless")
	           .arg(grok(QStringLiteral("DTS-HD_Master_Audio"), tr("DTS-HD Master Audio"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DTS-HD HRA"), QStringLiteral("audio")) },
	       tr("DTS-HD High Resolution Audio — high-bitrate lossy"));
	addRow(g, { badgeLabel(this, QStringLiteral("DTS"), QStringLiteral("audio")) },
	       tr("%1 — lossy core")
	           .arg(grok(QStringLiteral("DTS_(sound_system)"), tr("DTS Digital Surround"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DD+"), QStringLiteral("audio")) },
	       tr("%1 — lossy, streaming services")
	           .arg(grok(QStringLiteral("Dolby_Digital_Plus"), tr("Dolby Digital Plus (E-AC-3)"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DD"), QStringLiteral("audio")) },
	       tr("%1 — lossy, DVD and broadcast")
	           .arg(grok(QStringLiteral("Dolby_Digital"), tr("Dolby Digital (AC-3)"))));
	addRow(g, { badgeLabel(this, QStringLiteral("FLAC"), QStringLiteral("audio")),
	            badgeLabel(this, QStringLiteral("PCM"), QStringLiteral("audio")) },
	       tr("Lossless audio (%1, %2)")
	           .arg(grok(QStringLiteral("FLAC"), QStringLiteral("FLAC")),
	                grok(QStringLiteral("Pulse-code_modulation"), QStringLiteral("PCM"))));
	addRow(g, { badgeLabel(this, QStringLiteral("AAC"), QStringLiteral("audio")),
	            badgeLabel(this, QStringLiteral("MP3"), QStringLiteral("audio")),
	            badgeLabel(this, QStringLiteral("Opus"), QStringLiteral("audio")) },
	       tr("Common lossy codecs (%1, %2, %3)")
	           .arg(grok(QStringLiteral("Advanced_Audio_Coding"), QStringLiteral("AAC")),
	                grok(QStringLiteral("MP3"), QStringLiteral("MP3")),
	                grok(QStringLiteral("Opus_(audio_format)"), QStringLiteral("Opus"))));
	addRow(g, { badgeLabel(this, QStringLiteral("DD+  5.1  \xE2\x98\x85"),
	                       QStringLiteral("audio"), QStringLiteral("eng")) },
	       tr("Channels follow the codec — 7.1, 5.1, 2.0 (stereo)"));

	// ── Subtitles ─────────────────────────────────────────────────────────────
	auto* subBox = makeGroup(tr("Subtitles"), g);
	addRow(g, { badgeLabel(this, QStringLiteral("SRT"), QStringLiteral("subtitle")) },
	       tr("%1 — plain text")
	           .arg(grok(QStringLiteral("SubRip"), tr("SubRip"))));
	addRow(g, { badgeLabel(this, QStringLiteral("ASS"), QStringLiteral("subtitle")) },
	       tr("(Advanced) SubStation Alpha — styled text"));
	addRow(g, { badgeLabel(this, QStringLiteral("PGS"), QStringLiteral("subtitle")) },
	       tr("%1 bitmap subtitles")
	           .arg(grok(QStringLiteral("Blu-ray"), tr("Blu-ray"))));
	addRow(g, { badgeLabel(this, QStringLiteral("VobSub"), QStringLiteral("subtitle")) },
	       tr("%1 bitmap subtitles")
	           .arg(grok(QStringLiteral("DVD-Video"), tr("DVD"))));
	addRow(g, { badgeLabel(this, QStringLiteral("VTT"), QStringLiteral("subtitle")) },
	       tr("%1 — web text format")
	           .arg(grok(QStringLiteral("WebVTT"), QStringLiteral("WebVTT"))));

	// ── Icons ─────────────────────────────────────────────────────────────────
	auto* iconsBox = makeGroup(tr("Icons"), g);
	addRow(g, { glyphLabel(this, QStringLiteral("\xE2\x98\x85")) },   // ★
	       tr("Default track"));
	addRow(g, { glyphLabel(this, QStringLiteral("\xE2\x97\x8E")) },   // ◎
	       tr("Original audio language of the movie"));
	addRow(g, { glyphLabel(this, QStringLiteral("\xE2\x97\x8F")) },   // ●
	       tr("Forced subtitle (signs / foreign dialogue only)"));
	addRow(g, { glyphLabel(this, QStringLiteral("\xE2\x9C\x8E")) },   // ✎
	       tr("Commentary track (detected from track title)"));

	// ── Badges ────────────────────────────────────────────────────────────────
	auto* badgesBox = makeGroup(tr("Badges"), g);
	{
		auto* flagLbl = new QLabel(this);
		flagLbl->setPixmap(McLanguageFlags::flag(QStringLiteral("dan"),
		                                         McCardDelegate::kFlagH, devicePixelRatioF()));
		addRow(g, { flagLbl }, tr("Track language — hover a badge for the full name"));
	}
	addRow(g, { badgeLabel(this, QStringLiteral("DD  5.1"), QStringLiteral("audio"),
	                       QStringLiteral("ger"), true) },
	       tr("Struck-through — track will be removed"));
	addRow(g, { badgeLabel(this, tr("Video"),    QStringLiteral("video")),
	            badgeLabel(this, tr("Audio"),    QStringLiteral("audio")),
	            badgeLabel(this, tr("Subtitle"), QStringLiteral("subtitle")) },
	       tr("Pill color shows the track type"));

	// ── Assembly: Audio + Icons left, Video + Subtitles + Badges right ───────
	auto* columns  = new QHBoxLayout;
	columns->setSpacing(10);
	auto* leftCol  = new QVBoxLayout;
	auto* rightCol = new QVBoxLayout;
	leftCol->setSpacing(10);
	rightCol->setSpacing(10);
	leftCol->addWidget(audioBox);
	leftCol->addWidget(iconsBox);
	leftCol->addStretch();
	rightCol->addWidget(videoBox);
	rightCol->addWidget(subBox);
	rightCol->addWidget(badgesBox);
	rightCol->addStretch();
	columns->addLayout(leftCol, 1);
	columns->addLayout(rightCol, 1);
	root->addLayout(columns, 1);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	root->addWidget(buttons);
}

} // namespace Mc
