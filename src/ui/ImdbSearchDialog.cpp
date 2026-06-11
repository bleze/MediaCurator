#include "ui/ImdbSearchDialog.h"
#include "core/AppSettings.h"
#include "ui/McLanguageFlags.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QAbstractItemView>
#include <QDesktopServices>
#include <QHelpEvent>
#include <QPainter>
#include <QPixmap>
#include <QPixmapCache>
#include <QPushButton>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

// ── Per-result IMDb shortcut button ──────────────────────────────────────────
namespace {

class ImdbResultDelegate : public QStyledItemDelegate
{
public:
	static constexpr int kBtnW    = 24;
	static constexpr int kGap     = 8;
	static constexpr int kRatingW = 76;   // reserved width for the IMDb rating badge

	QPoint hoverPos{-1, -1};  // updated by the dialog's eventFilter

	explicit ImdbResultDelegate(QObject* parent = nullptr)
		: QStyledItemDelegate(parent) {}

	// "15000" → "15K",  "1500000" → "1.5M"
	static QString formatVotes(int n)
	{
		if (n <= 0)       return {};
		if (n >= 1000000) return QStringLiteral("%1M").arg(n / 1000000.0, 0, 'f', n % 1000000 >= 100000 ? 1 : 0);
		if (n >= 1000)    return QStringLiteral("%1K").arg(n / 1000);
		return QString::number(n);
	}

	void paint(QPainter* p, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override
	{
		const QString id = index.data(Qt::UserRole + 4).toString();
		if (id.isEmpty()) {
			QStyleOptionViewItem opt = option;
			opt.state &= ~QStyle::State_HasFocus;
			QStyledItemDelegate::paint(p, opt, index);
			return;
		}

		// Narrow text area to leave room for rating badge + IMDb button.
		QStyleOptionViewItem opt = option;
		opt.state &= ~QStyle::State_HasFocus;
		opt.rect.setRight(option.rect.right() - kRatingW - kBtnW - kGap * 2);
		QStyledItemDelegate::paint(p, opt, index);

		// Fill the right two columns with the same background so they blend.
		const bool sel    = option.state & QStyle::State_Selected;
		const bool active = option.state & QStyle::State_Active;
		const QPalette::ColorGroup grp = active ? QPalette::Active : QPalette::Inactive;
		const QColor bg = sel
			? option.palette.color(grp, QPalette::Highlight)
			: (index.row() % 2 == 0
			   ? option.palette.color(grp, QPalette::Base)
			   : option.palette.color(grp, QPalette::AlternateBase));
		p->fillRect(QRect(opt.rect.right() + 1, option.rect.top(),
		                  option.rect.right() - opt.rect.right(), option.rect.height()), bg);

		// ── IMDb-style rating badge ───────────────────────────────────────────
		const double voteAvg   = index.data(Qt::UserRole + 6).toDouble();
		const int    voteCount = index.data(Qt::UserRole + 7).toInt();
		if (voteAvg > 0.0) {
			const QRect ratingRect(opt.rect.right() + kGap, option.rect.top(),
			                       kRatingW - kGap, option.rect.height());

			// Star — centered vertically, left-aligned in rating rect
			QFont starFont = option.font;
			starFont.setPointSizeF(option.font.pointSizeF() * 1.5);
			starFont.setBold(true);
			const int starH    = QFontMetrics(starFont).height();
			const int starW    = QFontMetrics(starFont).horizontalAdvance(QStringLiteral("\xe2\x98\x85"));
			const int midY     = option.rect.top() + option.rect.height() / 2;
			const int starTop  = midY - starH / 2 - (voteCount > 0 ? 4 : 0);
			p->save();
			p->setFont(starFont);
			p->setPen(QColor(0xF5, 0xC5, 0x18));  // IMDb yellow
			p->drawText(QRect(ratingRect.left(), starTop, starW + 2, starH),
			            Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("\xe2\x98\x85"));
			p->restore();

			// "X.X/10" — next to the star
			const int numX = ratingRect.left() + starW + 4;
			QFont numFont = option.font;
			numFont.setBold(true);
			numFont.setPointSizeF(option.font.pointSizeF() * 1.1);
			const QString scoreText = QStringLiteral("%1").arg(voteAvg, 0, 'f', 1);
			QFont tenFont = option.font;
			tenFont.setPointSizeF(option.font.pointSizeF() * 0.78);
			const int scoreW = QFontMetrics(numFont).horizontalAdvance(scoreText);
			const int tenW   = QFontMetrics(tenFont).horizontalAdvance(QStringLiteral("/10"));

			p->save();
			const QColor textColor = sel ? option.palette.color(grp, QPalette::HighlightedText)
			                             : option.palette.color(grp, QPalette::Text);
			const QColor dimColor  = sel ? textColor.darker(140)
			                             : option.palette.color(grp, QPalette::PlaceholderText);
			p->setFont(numFont);
			p->setPen(textColor);
			p->drawText(QRect(numX, starTop, scoreW, QFontMetrics(numFont).height()),
			            Qt::AlignLeft | Qt::AlignVCenter, scoreText);
			p->setFont(tenFont);
			p->setPen(dimColor);
			p->drawText(QRect(numX + scoreW, starTop + QFontMetrics(numFont).height() - QFontMetrics(tenFont).height(),
			                  tenW, QFontMetrics(tenFont).height()),
			            Qt::AlignLeft | Qt::AlignBottom, QStringLiteral("/10"));
			p->restore();

			// Vote count below
			if (voteCount > 0) {
				const QString votes = formatVotes(voteCount);
				QFont cntFont = option.font;
				cntFont.setPointSizeF(option.font.pointSizeF() * 0.78);
				const int cntY = starTop + QFontMetrics(numFont).height() + 1;
				p->save();
				p->setFont(cntFont);
				p->setPen(sel ? textColor.darker(150) : option.palette.color(grp, QPalette::PlaceholderText));
				p->drawText(QRect(ratingRect.left() + starW + 4, cntY, kRatingW, QFontMetrics(cntFont).height()),
				            Qt::AlignLeft, votes);
				p->restore();
			}
		}

		// ── IMDb logo button ─────────────────────────────────────────────────
		const QRect r = btnRect(option.rect);
		QPixmap logo;
		if (!QPixmapCache::find("dlg_imdb_logo", &logo)) {
			logo = QPixmap(":/icons/imdb.png").scaled(
			    kBtnW, kBtnW, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			QPixmapCache::insert("dlg_imdb_logo", logo);
		}
		p->save();
		p->setOpacity(r.contains(hoverPos) ? 1.0 : 0.75);
		p->drawPixmap(r.left() + (r.width()  - logo.width())  / 2,
		              r.top()  + (r.height() - logo.height()) / 2,
		              logo);
		p->restore();
	}

	static QRect btnRect(const QRect& itemRect)
	{
		return QRect(itemRect.right() - kBtnW - kGap / 2,
		             itemRect.top()   + (itemRect.height() - kBtnW) / 2,
		             kBtnW, kBtnW);
	}
};

} // anonymous namespace

namespace Mc {

ImdbSearchDialog::ImdbSearchDialog(const QString& videoPath,
									const QString& suggestedTitle,
									const QString& existingImdbId,
									const QString& tmdbApiKey,
									QWidget* parent)
	: QDialog(parent, Qt::Dialog)
	, m_videoPath(videoPath)
	, m_tmdbApiKey(tmdbApiKey)
	, m_nam([] { static QNetworkAccessManager* s = new QNetworkAccessManager; return s; }())
{
	const QString fname = QFileInfo(videoPath).fileName();
	setWindowTitle(fname.isEmpty() ? tr("Movie Metadata")
	                               : tr("Movie Metadata — %1").arg(fname));
	setSizeGripEnabled(true);
	setMinimumWidth(700);

	{
		QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
		const QByteArray geo = s.value("imdbSearchDialog/geometry").toByteArray();
		if (!geo.isEmpty())
			restoreGeometry(geo);
		else
			resize(1000, 620);
		m_galleryFilter = s.value("imdbSearchDialog/galleryFilter").toString();
	}

	auto* mainLayout = new QVBoxLayout(this);
	mainLayout->setSpacing(10);
	mainLayout->setContentsMargins(12, 12, 12, 12);

	// Search row
	auto* searchRow = new QHBoxLayout;
	m_searchEdit = new QLineEdit(suggestedTitle, this);
	m_searchEdit->setPlaceholderText(tr("Movie title…"));
	m_btnSearch = new QPushButton(tr("Search"), this);
	m_btnSearch->setAutoDefault(false);
	searchRow->addWidget(m_searchEdit, 1);
	searchRow->addWidget(m_btnSearch);
	mainLayout->addLayout(searchRow);

	m_statusLabel = new QLabel(this);
	m_statusLabel->setVisible(false);
	mainLayout->addWidget(m_statusLabel);

	// Results list
	m_resultsList = new QListWidget(this);
	m_resultsList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_resultsList->setAlternatingRowColors(true);
	m_resultsList->setIconSize({54, 80});
	m_resultsList->setSpacing(2);
	m_resultsList->setStyleSheet("QListView { outline: 0; } QListView::item { outline: none; }");
	m_resultsList->setItemDelegate(new ImdbResultDelegate(m_resultsList));
	m_resultsList->viewport()->setMouseTracking(true);
	m_resultsList->viewport()->installEventFilter(this);
	m_resultsList->installEventFilter(this);

	// Poster gallery panel (right side of splitter)
	auto* galleryPanel  = new QWidget(this);
	auto* galleryLayout = new QVBoxLayout(galleryPanel);
	galleryLayout->setContentsMargins(0, 0, 0, 0);
	galleryLayout->setSpacing(4);

	m_langFilter = new QComboBox(galleryPanel);
	m_langFilter->addItem(tr("All"), QString{});
	m_langFilter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	m_galleryStatus = new QLabel(tr("Select a result to browse posters."), galleryPanel);
	m_galleryStatus->setStyleSheet("color: gray;");
	m_galleryStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

	auto* filterRow = new QHBoxLayout;
	filterRow->addWidget(new QLabel(tr("Language:"), galleryPanel));
	filterRow->addWidget(m_langFilter, 1);
	filterRow->addWidget(m_galleryStatus);
	galleryLayout->addLayout(filterRow);

	m_posterGallery = new QListWidget(galleryPanel);
	m_posterGallery->setViewMode(QListView::IconMode);
	m_posterGallery->setIconSize({90, 135});
	m_posterGallery->setGridSize({100, 158});
	m_posterGallery->setResizeMode(QListView::Adjust);
	m_posterGallery->setMovement(QListView::Static);
	m_posterGallery->setUniformItemSizes(true);
	m_posterGallery->setWordWrap(false);
	galleryLayout->addWidget(m_posterGallery, 1);

	auto* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(m_resultsList);
	splitter->addWidget(galleryPanel);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);

	{
		QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
		const QByteArray st = s.value("imdbSearchDialog/splitter").toByteArray();
		if (!st.isEmpty()) splitter->restoreState(st);
	}
	m_splitter = splitter;

	mainLayout->addWidget(splitter, 1);

	connect(m_posterGallery, &QListWidget::currentItemChanged,
	        this, [this](QListWidgetItem* item) {
		if (!item) return;
		m_selectedPosterPath = item->data(Qt::UserRole).toString();
		m_galleryImageData   = m_galleryThumbData.value(m_selectedPosterPath);
		if (!m_galleryImageData.isEmpty()) {
			QPixmap px;
			if (px.loadFromData(m_galleryImageData)) {
				const int row = m_resultsList->currentRow();
				if (row >= 0 && row < m_resultsList->count())
					m_resultsList->item(row)->setIcon(
						QIcon(px.scaled(54, 80, Qt::KeepAspectRatio,
						               Qt::SmoothTransformation)));
			}
		}
	});
	connect(m_langFilter, &QComboBox::currentIndexChanged, this, [this]() {
		m_galleryFilter = m_langFilter->currentData().toString();
		populateGallery();
	});

	// IMDb ID row
	auto* idRow = new QHBoxLayout;
	idRow->addWidget(new QLabel(tr("IMDb ID:"), this));
	m_imdbIdEdit = new QLineEdit(this);
	m_imdbIdEdit->setPlaceholderText("tt0000000");
	m_imdbIdEdit->setFixedWidth(140);
	if (!existingImdbId.isEmpty())
		m_imdbIdEdit->setText(existingImdbId);
	idRow->addWidget(m_imdbIdEdit);
	idRow->addStretch();
	mainLayout->addLayout(idRow);

	// Dialog buttons
	m_btnBox = new QDialogButtonBox(this);
	m_btnSave = new QPushButton(tr("Save"), this);
	static const QRegularExpression validId(R"(^tt\d{7,8}$)");
	m_btnSave->setEnabled(validId.match(existingImdbId).hasMatch());
	m_btnBox->addButton(m_btnSave, QDialogButtonBox::AcceptRole);
	m_btnBox->addButton(QDialogButtonBox::Cancel);
	mainLayout->addWidget(m_btnBox);

	if (tmdbApiKey.isEmpty()) {
		m_searchEdit->setEnabled(false);
		m_btnSearch->setEnabled(false);
		setStatusText(tr("Configure a TMDB API key in Settings to enable search."), true);
	}

	connect(m_btnSearch,  &QPushButton::clicked, this, [this]() {
		m_userHasSearched = true;
		onSearch();
	});
	connect(m_searchEdit, &QLineEdit::returnPressed, this, [this]() {
		m_userHasSearched = true;
		onSearch();
	});
	connect(m_resultsList, &QListWidget::currentRowChanged,
	        this, &ImdbSearchDialog::onResultSelectionChanged);
	connect(m_resultsList, &QListWidget::doubleClicked,
	        this, [this]() {
		static const QRegularExpression re(R"(^tt\d{7,8}$)");
		if (re.match(m_imdbIdEdit->text().trimmed()).hasMatch())
			accept();
		else
			m_acceptAfterFetch = true;  // accept as soon as the ID arrives
	});
	connect(m_imdbIdEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
		static const QRegularExpression re(R"(^tt\d{7,8}$)");
		m_btnSave->setEnabled(re.match(t.trimmed()).hasMatch());
	});
	connect(m_btnSave, &QPushButton::clicked,      this, &QDialog::accept);
	connect(m_btnBox,  &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Auto-search when the dialog opens (if key and title are both available)
	if (!tmdbApiKey.isEmpty() && !suggestedTitle.isEmpty())
		QMetaObject::invokeMethod(this, &ImdbSearchDialog::onSearch, Qt::QueuedConnection);
}

ImdbSearchDialog::~ImdbSearchDialog()
{
	// Disconnect this from all in-flight replies so finished() callbacks can't
	// re-enter destroyed members. Do NOT abort() — aborting an in-flight HTTP/1.1
	// request closes the underlying TLS connection synchronously on the main thread
	// (~2-5 s per connection on Windows/Schannel), blocking the paint queue.
	// The self-cleanup connections (reply → reply->deleteLater) handle memory;
	// downloads complete quietly in the background on the shared static NAM.
	if (m_searchReply) m_searchReply->disconnect(this);
	if (m_extIdsReply) m_extIdsReply->disconnect(this);
	if (m_imagesReply) m_imagesReply->disconnect(this);
	for (auto it = m_galleryThumbReplies.begin(); it != m_galleryThumbReplies.end(); ++it)
		it.key()->disconnect(this);
	m_galleryThumbReplies.clear();
	for (QNetworkReply* r : std::as_const(m_thumbReplyByRow)) r->disconnect(this);
	m_thumbReplyByRow.clear();
	for (QNetworkReply* r : std::as_const(m_prefetchByRow)) r->disconnect(this);
	m_prefetchByRow.clear();
	// m_nam is a shared static — never destroyed.
}

void ImdbSearchDialog::done(int result)
{
	QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
	s.setValue("imdbSearchDialog/geometry", saveGeometry());
	if (m_splitter)
		s.setValue("imdbSearchDialog/splitter", m_splitter->saveState());
	s.setValue("imdbSearchDialog/galleryFilter", m_galleryFilter);
	QDialog::done(result);
}

void ImdbSearchDialog::closeEvent(QCloseEvent* event)
{
	QDialog::closeEvent(event);
}


void ImdbSearchDialog::setBatchMode(int current, int total)
{
	m_batchMode = true;
	const QString fname = QFileInfo(m_videoPath).fileName();
	if (fname.isEmpty())
		setWindowTitle(tr("Movie Metadata — %1 of %2").arg(current).arg(total));
	else
		setWindowTitle(tr("Movie Metadata — %1 — %2 of %3").arg(fname).arg(current).arg(total));
	if (!m_btnBox) return;

	// Relabel the standard "Cancel" button to "Skip" so it's clear what it does
	// in a multi-file flow (skips this file, continues with the next one).
	if (auto* cancelBtn = m_btnBox->button(QDialogButtonBox::Cancel))
		cancelBtn->setText(tr("Skip"));

	// "Cancel all" — aborts the entire batch loop.
	auto* cancelAllBtn = m_btnBox->addButton(tr("Cancel all"), QDialogButtonBox::DestructiveRole);
	connect(cancelAllBtn, &QPushButton::clicked, this, [this]() { done(CancelBatch); });
}

QString    ImdbSearchDialog::selectedImdbId()     const { return m_imdbIdEdit->text().trimmed(); }
QString    ImdbSearchDialog::selectedTitle()      const { return m_selectedTitle; }
int        ImdbSearchDialog::selectedYear()       const { return m_selectedYear; }
QString    ImdbSearchDialog::selectedPosterPath() const { return m_selectedPosterPath; }
QByteArray ImdbSearchDialog::selectedImageData()  const
{
	if (!m_galleryImageData.isEmpty()) return m_galleryImageData;
	if (!m_resultsList) return {};
	return m_thumbDataByRow.value(m_resultsList->currentRow());
}
QString ImdbSearchDialog::selectedOriginalLanguage() const
{
	if (!m_resultsList) return {};
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 5).toString() : QString{};
}

double ImdbSearchDialog::selectedVoteAverage() const
{
	if (!m_resultsList) return 0.0;
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 6).toDouble() : 0.0;
}

int ImdbSearchDialog::selectedVoteCount() const
{
	if (!m_resultsList) return 0;
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 7).toInt() : 0;
}

void ImdbSearchDialog::setStatusText(const QString& text, bool isError)
{
	m_statusLabel->setText(text);
	m_statusLabel->setStyleSheet(isError ? "color: #c0392b;" : "color: gray;");
	m_statusLabel->setVisible(!text.isEmpty());
}

void ImdbSearchDialog::onSearch()
{
	if (m_tmdbApiKey.isEmpty()) return;

	const QString query = m_searchEdit->text().trimmed();
	if (query.isEmpty()) return;

	if (m_searchReply) { m_searchReply->abort(); m_searchReply = nullptr; }
	if (m_extIdsReply) { m_extIdsReply->abort(); m_extIdsReply = nullptr; }
	for (QNetworkReply* r : m_thumbReplyByRow) r->abort();
	m_thumbReplyByRow.clear();
	m_thumbDataByRow.clear();
	for (QNetworkReply* r : m_prefetchByRow) r->abort();
	m_prefetchByRow.clear();
	m_imdbIdByRow.clear();

	m_resultsList->clear();
	setStatusText(tr("Searching…"));
	m_btnSearch->setEnabled(false);

	// Split title and year — TMDB's year param improves precision
	static const QRegularExpression yearRe(R"(\b(19|20)\d{2}\b)");
	const auto yearMatch = yearRe.match(query);

	QUrlQuery params;
	if (yearMatch.hasMatch()) {
		// Strip any trailing punctuation left by "(YYYY)" style inputs
		static const QRegularExpression trailingPunct(R"([\s\W]+$)");
		QString titlePart = query.left(yearMatch.capturedStart()).trimmed();
		titlePart.remove(trailingPunct);
		params.addQueryItem("query", titlePart);
		params.addQueryItem("year",  yearMatch.captured(0));
	} else {
		params.addQueryItem("query", query);
	}
	params.addQueryItem("api_key",  m_tmdbApiKey);
	params.addQueryItem("language", "en-US");
	params.addQueryItem("page",     "1");

	QUrl url("https://api.themoviedb.org/3/search/movie");
	url.setQuery(params);

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");

	m_searchReply = m_nam->get(req);
	connect(m_searchReply, &QNetworkReply::finished, m_searchReply, &QObject::deleteLater);
	connect(m_searchReply, &QNetworkReply::finished, this, [this, yearMatch]() {
		auto* reply = qobject_cast<QNetworkReply*>(sender());
		if (!reply || reply != m_searchReply) return;
		m_searchReply = nullptr;
		m_btnSearch->setEnabled(true);

		if (reply->error() != QNetworkReply::NoError) {
			if (reply->error() != QNetworkReply::OperationCanceledError)
				setStatusText(tr("Network error: %1").arg(reply->errorString()), true);
			if (m_autoSelectSingle) { setWindowOpacity(1.0); raise(); activateWindow(); }
			reply->deleteLater();
			return;
		}

		const QJsonObject root    = QJsonDocument::fromJson(reply->readAll()).object();
		const QJsonArray  results = root["results"].toArray();
		reply->deleteLater();

		if (results.isEmpty()) {
			setStatusText(tr("No results found."));
			return;
		}

		for (const auto& v : results) {
			const QJsonObject obj         = v.toObject();
			const QString     title       = obj["title"].toString();
			const int         year        = obj["release_date"].toString().left(4).toInt();
			const int         tmdbId      = obj["id"].toInt();
			const QString     posterPath  = obj["poster_path"].toString();
			const QString     origLang    = obj["original_language"].toString();
			const double      voteAvg     = obj["vote_average"].toDouble();
			const int         voteCount   = obj["vote_count"].toInt();

			const QString display = year > 0
				? QStringLiteral("%1  (%2)").arg(title).arg(year)
				: title;

			auto* item = new QListWidgetItem(display, m_resultsList);
			item->setData(Qt::UserRole,     tmdbId);
			item->setData(Qt::UserRole + 1, title);
			item->setData(Qt::UserRole + 2, year);
			item->setData(Qt::UserRole + 3, posterPath);
			item->setData(Qt::UserRole + 5, origLang);
			item->setData(Qt::UserRole + 6, voteAvg);
			item->setData(Qt::UserRole + 7, voteCount);

			// Fetch thumbnail asynchronously
			if (!posterPath.isEmpty()) {
				const QUrl thumbUrl(
					QStringLiteral("https://image.tmdb.org/t/p/w92%1").arg(posterPath));
				QNetworkRequest thumbReq(thumbUrl);
				thumbReq.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
				QNetworkReply* r = m_nam->get(thumbReq);
				const int row = m_resultsList->count() - 1;
				m_thumbReplyByRow[row] = r;
				connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
				connect(r, &QNetworkReply::finished, this, [this, r, row]() {
					m_thumbReplyByRow.remove(row);
					if (r->error() == QNetworkReply::NoError) {
						const QByteArray bytes = r->readAll();
						QPixmap px;
						if (px.loadFromData(bytes) && row < m_resultsList->count()) {
							m_thumbDataByRow[row] = bytes;
							m_resultsList->item(row)->setIcon(
								QIcon(px.scaled(54, 80, Qt::KeepAspectRatio,
								                Qt::SmoothTransformation)));
						}
					}
					r->deleteLater();
				});
			}
		}

		setStatusText(tr("%1 result(s)").arg(results.size()));

		// Prefetch IMDb IDs for all results in parallel
		for (int row = 0; row < m_resultsList->count(); ++row) {
			const int tmdbId = m_resultsList->item(row)->data(Qt::UserRole).toInt();
			QUrlQuery pq;
			pq.addQueryItem("api_key", m_tmdbApiKey);
			QUrl pUrl(QStringLiteral("https://api.themoviedb.org/3/movie/%1/external_ids").arg(tmdbId));
			pUrl.setQuery(pq);
			QNetworkRequest pReq(pUrl);
			pReq.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
			QNetworkReply* pr = m_nam->get(pReq);
			m_prefetchByRow[row] = pr;
			connect(pr, &QNetworkReply::finished, pr, &QObject::deleteLater);
			connect(pr, &QNetworkReply::finished, this, [this, pr, row]() {
				m_prefetchByRow.remove(row);
				const QString id = pr->error() == QNetworkReply::NoError
				    ? QJsonDocument::fromJson(pr->readAll()).object()["imdb_id"].toString()
				    : QString{};

				if (!id.isEmpty()) {
					m_imdbIdByRow[row] = id;
					if (row < m_resultsList->count())
						m_resultsList->item(row)->setData(Qt::UserRole + 4, id);
				}

				if (m_resultsList->currentRow() == row) {
					m_imdbIdEdit->setEnabled(true);
					if (!id.isEmpty()) {
						m_imdbIdEdit->setText(id);
						if (m_acceptAfterFetch) { m_acceptAfterFetch = false; accept(); }
					} else {
						// No IMDb ID available — clear the waiting state so Save isn't stuck disabled.
						m_acceptAfterFetch = false;
						m_imdbIdEdit->clear();
						m_btnSave->setEnabled(false);
					}
				} else if (m_acceptAfterFetch && id.isEmpty()) {
					// This row was the auto-select target but is no longer current — give up waiting.
					m_acceptAfterFetch = false;
				}
			});
		}

		if (m_resultsList->count() > 0) {
			// Pick the result whose year is closest to the searched year.
			// Falls back to row 0 (TMDB popularity order) if no year was searched.
			int bestRow = 0;
			if (yearMatch.hasMatch()) {
				const int yearSearched = yearMatch.captured(0).toInt();
				int bestDiff = INT_MAX;
				for (int i = 0; i < m_resultsList->count(); ++i) {
					const int rowYear = m_resultsList->item(i)->data(Qt::UserRole + 2).toInt();
					const int diff    = qAbs(rowYear - yearSearched);
					if (diff < bestDiff) { bestDiff = diff; bestRow = i; }
				}
			}
			m_resultsList->setCurrentRow(bestRow);

			if (m_autoSelectSingle && m_resultsList->count() == 1) {
				// Exactly one result — auto-accept once the IMDb ID arrives.
				const QString cachedId = m_imdbIdByRow.value(bestRow);
				if (!cachedId.isEmpty()) {
					m_imdbIdEdit->setText(cachedId);
					accept();
				} else {
					// IMDb ID still in-flight — accept as soon as the prefetch lands.
					m_acceptAfterFetch = true;
				}
			}
		}
	});
}

void ImdbSearchDialog::keyPressEvent(QKeyEvent* event)
{
	// 1–9: jump to that result row (works regardless of where focus is)
	const int key = event->key();
	if (key >= Qt::Key_1 && key <= Qt::Key_9) {
		const int row = key - Qt::Key_1;
		if (row < m_resultsList->count()) {
			m_resultsList->setCurrentRow(row);
			m_resultsList->setFocus();
		}
		return;
	}
	QDialog::keyPressEvent(event);
}

bool ImdbSearchDialog::eventFilter(QObject* obj, QEvent* ev)
{
	// List viewport — IMDb button click, hover cursor, and tooltip
	if (obj == m_resultsList->viewport()) {
		const auto hitImdb = [&](const QPoint& pos) -> QString {
			const QModelIndex idx = m_resultsList->indexAt(pos);
			if (!idx.isValid()) return {};
			const QString id = idx.data(Qt::UserRole + 4).toString();
			if (id.isEmpty()) return {};
			return ImdbResultDelegate::btnRect(m_resultsList->visualRect(idx)).contains(pos) ? id : QString();
		};

		if (ev->type() == QEvent::MouseButtonPress) {
			const QString id = hitImdb(static_cast<const QMouseEvent*>(ev)->position().toPoint());
			if (!id.isEmpty()) {
				QDesktopServices::openUrl(
				    QUrl(QStringLiteral("https://www.imdb.com/title/%1/").arg(id)));
				return true;
			}
		}
		if (ev->type() == QEvent::MouseMove) {
			const QPoint pos = static_cast<const QMouseEvent*>(ev)->position().toPoint();
			auto* del = static_cast<ImdbResultDelegate*>(m_resultsList->itemDelegate());
			del->hoverPos = pos;
			m_resultsList->viewport()->update();
			m_resultsList->viewport()->setCursor(
			    hitImdb(pos).isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
		}
		if (ev->type() == QEvent::Leave) {
			auto* del = static_cast<ImdbResultDelegate*>(m_resultsList->itemDelegate());
			del->hoverPos = {-1, -1};
			m_resultsList->viewport()->update();
			m_resultsList->viewport()->setCursor(Qt::ArrowCursor);
		}
		if (ev->type() == QEvent::ToolTip) {
			const auto* he = static_cast<const QHelpEvent*>(ev);
			const QString id = hitImdb(he->pos());
			if (!id.isEmpty()) {
				QToolTip::showText(he->globalPos(),
				    tr("Open IMDb page — %1").arg(id));
				return true;
			}
		}
	}

	// Enter/Return on the results list → accept (same as double-click)
	if (obj == m_resultsList && ev->type() == QEvent::KeyPress) {
		const auto* ke = static_cast<const QKeyEvent*>(ev);
		if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
		        && m_resultsList->currentItem()) {
			static const QRegularExpression re(R"(^tt\d{7,8}$)");
			if (re.match(m_imdbIdEdit->text().trimmed()).hasMatch())
				accept();
			else
				m_acceptAfterFetch = true;
			return true;
		}
	}
	return QDialog::eventFilter(obj, ev);
}

void ImdbSearchDialog::onResultSelectionChanged()
{
	const int row = m_resultsList->currentRow();
	const QListWidgetItem* item = m_resultsList->currentItem();
	if (!item) return;

	m_selectedTitle      = item->data(Qt::UserRole + 1).toString();
	m_selectedYear       = item->data(Qt::UserRole + 2).toInt();
	m_selectedPosterPath = item->data(Qt::UserRole + 3).toString();
	m_galleryImageData.clear();

	fetchPosterImages(item->data(Qt::UserRole).toInt(),
	                  item->data(Qt::UserRole + 5).toString());

	if (m_extIdsReply) { m_extIdsReply->abort(); m_extIdsReply = nullptr; }

	// Use prefetched ID if available
	if (m_imdbIdByRow.contains(row)) {
		m_imdbIdEdit->setText(m_imdbIdByRow[row]);
		m_imdbIdEdit->setEnabled(true);
		return;
	}

	// Prefetch in-flight — show spinner text and wait for the prefetch callback
	if (m_prefetchByRow.contains(row)) {
		m_imdbIdEdit->setText(tr("Fetching…"));
		m_imdbIdEdit->setEnabled(false);
		m_btnSave->setEnabled(false);
		return;
	}

	// Fallback: start a fresh request (prefetch failed or wasn't started)
	const int tmdbId = item->data(Qt::UserRole).toInt();
	m_imdbIdEdit->setText(tr("Fetching…"));
	m_imdbIdEdit->setEnabled(false);
	m_btnSave->setEnabled(false);

	QUrlQuery params;
	params.addQueryItem("api_key", m_tmdbApiKey);
	QUrl url(QStringLiteral("https://api.themoviedb.org/3/movie/%1/external_ids").arg(tmdbId));
	url.setQuery(params);
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");

	m_extIdsReply = m_nam->get(req);
	connect(m_extIdsReply, &QNetworkReply::finished, m_extIdsReply, &QObject::deleteLater);
	connect(m_extIdsReply, &QNetworkReply::finished, this, [this, row]() {
		auto* reply = qobject_cast<QNetworkReply*>(sender());
		if (!reply || reply != m_extIdsReply) return;
		m_extIdsReply = nullptr;
		m_imdbIdEdit->setEnabled(true);

		if (reply->error() != QNetworkReply::NoError) {
			if (reply->error() != QNetworkReply::OperationCanceledError)
				m_imdbIdEdit->clear();
			return;
		}

		const QString imdbId = QJsonDocument::fromJson(reply->readAll()).object()["imdb_id"].toString();
		m_imdbIdEdit->setText(imdbId);
		if (!imdbId.isEmpty()) m_imdbIdByRow[row] = imdbId;
		if (m_acceptAfterFetch && !imdbId.isEmpty()) { m_acceptAfterFetch = false; accept(); }
		m_acceptAfterFetch = false;
	});
}

void ImdbSearchDialog::fetchPosterImages(int tmdbId, const QString& origLang)
{
	if (m_imagesReply) { m_imagesReply->disconnect(this); m_imagesReply = nullptr; }
	for (auto it = m_galleryThumbReplies.begin(); it != m_galleryThumbReplies.end(); ++it)
		it.key()->disconnect(this);
	m_galleryThumbReplies.clear();
	m_galleryThumbData.clear();
	m_allPosters.clear();
	m_posterGallery->clear();
	m_langFilter->blockSignals(true);
	m_langFilter->clear();
	m_langFilter->addItem(tr("All"), QString{});
	m_langFilter->blockSignals(false);
	m_galleryFilter.clear();
	m_galleryStatus->setText(tr("Loading posters\xe2\x80\xa6"));

	QStringList langs = {QStringLiteral("en"), QStringLiteral("null")};
	if (!origLang.isEmpty() && origLang != QLatin1String("en"))
		langs.append(origLang);
	for (const QString& iso2 : std::as_const(m_understoodLanguages)) {
		const QString iso1 = McLanguageFlags::toIso1(iso2);
		if (!iso1.isEmpty() && !langs.contains(iso1))
			langs.append(iso1);
	}

	QUrlQuery params;
	params.addQueryItem(QStringLiteral("api_key"), m_tmdbApiKey);
	params.addQueryItem(QStringLiteral("include_image_language"), langs.join(u','));
	QUrl url(QStringLiteral("https://api.themoviedb.org/3/movie/%1/images").arg(tmdbId));
	url.setQuery(params);
	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");

	m_imagesReply = m_nam->get(req);
	connect(m_imagesReply, &QNetworkReply::finished, m_imagesReply, &QObject::deleteLater);
	connect(m_imagesReply, &QNetworkReply::finished, this, [this]() {
		auto* reply = qobject_cast<QNetworkReply*>(sender());
		if (!reply || reply != m_imagesReply) return;
		m_imagesReply = nullptr;

		if (reply->error() != QNetworkReply::NoError) {
			if (reply->error() != QNetworkReply::OperationCanceledError)
				m_galleryStatus->setText(tr("Failed to load posters."));
			return;
		}

		const QJsonArray posters =
			QJsonDocument::fromJson(reply->readAll()).object()[QStringLiteral("posters")].toArray();

		m_allPosters.clear();
		m_allPosters.reserve(posters.size());
		for (const auto& v : posters)
			m_allPosters.append(v.toObject());

		// Rebuild language filter from what the response actually contains
		QSet<QString> langs;
		for (const auto& p : std::as_const(m_allPosters))
			langs.insert(p[QStringLiteral("iso_639_1")].isNull()
			             ? QStringLiteral("null")
			             : p[QStringLiteral("iso_639_1")].toString());

		m_langFilter->blockSignals(true);
		m_langFilter->clear();
		m_langFilter->addItem(tr("All"), QString{});
		if (langs.contains(QStringLiteral("null")))
			m_langFilter->addItem(tr("Textless"), QStringLiteral("null"));
		QStringList sorted(langs.begin(), langs.end());
		sorted.sort();
		for (const QString& l : std::as_const(sorted)) {
			if (l != QLatin1String("null") && !l.isEmpty())
				m_langFilter->addItem(l.toUpper(), l);
		}
		m_langFilter->blockSignals(false);
		// Restore previous filter if the new result also has it; otherwise "All"
		const QString wanted = m_galleryFilter;
		m_galleryFilter.clear();
		for (int i = 0; i < m_langFilter->count(); ++i) {
			if (m_langFilter->itemData(i).toString() == wanted) {
				m_langFilter->setCurrentIndex(i);
				m_galleryFilter = wanted;
				break;
			}
		}

		populateGallery();
	});
}

void ImdbSearchDialog::populateGallery()
{
	m_posterGallery->clear();

	for (const auto& p : std::as_const(m_allPosters)) {
		const QString lang = p[QStringLiteral("iso_639_1")].isNull()
		                     ? QStringLiteral("null")
		                     : p[QStringLiteral("iso_639_1")].toString();
		if (!m_galleryFilter.isEmpty() && lang != m_galleryFilter)
			continue;

		const QString path      = p[QStringLiteral("file_path")].toString();
		const double  voteAvg   = p[QStringLiteral("vote_average")].toDouble();
		const QString langLabel = m_galleryFilter.isEmpty()
			? (lang == QLatin1String("null") ? tr("Textless") : lang.toUpper())
			: QString{};

		auto* item = new QListWidgetItem(langLabel, m_posterGallery);
		item->setData(Qt::UserRole, path);
		item->setSizeHint({100, 158});
		item->setToolTip(QStringLiteral("%1  \xe2\x98\x85 %2").arg(langLabel).arg(voteAvg, 0, 'f', 1));

		if (m_galleryThumbData.contains(path)) {
			QPixmap px;
			if (px.loadFromData(m_galleryThumbData[path]))
				item->setIcon(QIcon(px.scaled(90, 135, Qt::KeepAspectRatio,
				                              Qt::SmoothTransformation)));
		} else {
			const QUrl thumbUrl(QStringLiteral("https://image.tmdb.org/t/p/w154%1").arg(path));
			QNetworkRequest thumbReq(thumbUrl);
			thumbReq.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
			QNetworkReply* r = m_nam->get(thumbReq);
			m_galleryThumbReplies[r] = path;
			connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
			connect(r, &QNetworkReply::finished, this, [this, r, path]() {
				m_galleryThumbReplies.remove(r);
				if (r->error() != QNetworkReply::NoError) return;
				const QByteArray bytes = r->readAll();
				m_galleryThumbData[path] = bytes;
				QPixmap px;
				if (!px.loadFromData(bytes)) return;
				const QIcon icon(px.scaled(90, 135, Qt::KeepAspectRatio,
				                           Qt::SmoothTransformation));
				for (int i = 0; i < m_posterGallery->count(); ++i) {
					if (m_posterGallery->item(i)->data(Qt::UserRole).toString() == path) {
						m_posterGallery->item(i)->setIcon(icon);
						auto* cur = m_posterGallery->currentItem();
						if (cur && cur->data(Qt::UserRole).toString() == path) {
							m_galleryImageData = bytes;
							const int row = m_resultsList->currentRow();
							if (row >= 0 && row < m_resultsList->count())
								m_resultsList->item(row)->setIcon(
									QIcon(px.scaled(54, 80, Qt::KeepAspectRatio,
									               Qt::SmoothTransformation)));
						}
						break;
					}
				}
			});
		}
	}

	const int count = m_posterGallery->count();
	m_galleryStatus->setText(count > 0
		? tr("%1 poster(s)").arg(count)
		: (m_allPosters.isEmpty()
		   ? tr("No posters available.")
		   : tr("No posters match this filter.")));
}

} // namespace Mc
