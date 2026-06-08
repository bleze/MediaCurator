#include "ui/ImdbSearchDialog.h"

#include <QDialogButtonBox>
#include <QFileInfo>
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
	static constexpr int kBtnW = 24;
	static constexpr int kGap  = 8;

	QPoint hoverPos{-1, -1};  // updated by the dialog's eventFilter

	explicit ImdbResultDelegate(QObject* parent = nullptr)
		: QStyledItemDelegate(parent) {}

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

		// Draw standard item narrowed to leave room for button.
		// Suppress the dotted focus rectangle — results look like cards, not grid cells.
		QStyleOptionViewItem opt = option;
		opt.state &= ~QStyle::State_HasFocus;
		opt.rect.setRight(option.rect.right() - kBtnW - kGap);
		QStyledItemDelegate::paint(p, opt, index);

		// Fill the gap + button column to match exactly what the standard delegate
		// painted for the text/poster area.  Qt uses QPalette::Inactive when the
		// view doesn't have keyboard focus (State_Active is absent), so we must
		// mirror that group selection — otherwise the two halves show different
		// shades and look like separate table columns.
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

		// IMDb logo — hover brightens it
		const QRect r = btnRect(option.rect);
		const bool hovered = r.contains(hoverPos);
		QPixmap logo;
		if (!QPixmapCache::find("dlg_imdb_logo", &logo)) {
			logo = QPixmap(":/icons/imdb.png").scaled(
			    kBtnW, kBtnW, Qt::KeepAspectRatio, Qt::SmoothTransformation);
			QPixmapCache::insert("dlg_imdb_logo", logo);
		}
		p->save();
		p->setOpacity(hovered ? 1.0 : 0.75);
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
	setWindowTitle(fname.isEmpty() ? tr("Edit IMDb Link")
	                               : tr("Edit IMDb Link — %1").arg(fname));
	setSizeGripEnabled(true);
	setMinimumWidth(480);

	{
		QSettings s;
		const QByteArray geo = s.value("imdbSearchDialog/geometry").toByteArray();
		if (!geo.isEmpty())
			restoreGeometry(geo);
		else
			resize(840, 600);
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
	mainLayout->addWidget(m_resultsList, 1);

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
	for (QNetworkReply* r : std::as_const(m_thumbReplyByRow)) r->disconnect(this);
	m_thumbReplyByRow.clear();
	for (QNetworkReply* r : std::as_const(m_prefetchByRow)) r->disconnect(this);
	m_prefetchByRow.clear();
	// m_nam is a shared static — never destroyed.
}

void ImdbSearchDialog::done(int result)
{
	QSettings().setValue("imdbSearchDialog/geometry", saveGeometry());
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
		setWindowTitle(tr("Edit IMDb Link — %1 of %2").arg(current).arg(total));
	else
		setWindowTitle(tr("Edit IMDb Link — %1 — %2 of %3").arg(fname).arg(current).arg(total));
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
	if (!m_resultsList) return {};
	return m_thumbDataByRow.value(m_resultsList->currentRow());
}
QString ImdbSearchDialog::selectedOriginalLanguage() const
{
	if (!m_resultsList) return {};
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 5).toString() : QString{};
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
			if (m_autoSelectSingle) { setWindowOpacity(1.0); raise(); activateWindow(); }
			return;
		}

		for (const auto& v : results) {
			const QJsonObject obj        = v.toObject();
			const QString     title      = obj["title"].toString();
			const int         year       = obj["release_date"].toString().left(4).toInt();
			const int         tmdbId     = obj["id"].toInt();
			const QString     posterPath = obj["poster_path"].toString();
			const QString     origLang   = obj["original_language"].toString();

			const QString display = year > 0
				? QStringLiteral("%1  (%2)").arg(title).arg(year)
				: title;

			auto* item = new QListWidgetItem(display, m_resultsList);
			item->setData(Qt::UserRole,     tmdbId);
			item->setData(Qt::UserRole + 1, title);
			item->setData(Qt::UserRole + 2, year);
			item->setData(Qt::UserRole + 5, origLang);  // ISO 639-1 original language from TMDB
			item->setData(Qt::UserRole + 3, posterPath);

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
				if (pr->error() != QNetworkReply::NoError) return;
				const QString id = QJsonDocument::fromJson(pr->readAll()).object()["imdb_id"].toString();
				if (id.isEmpty()) return;
				m_imdbIdByRow[row] = id;
				if (row < m_resultsList->count())
					m_resultsList->item(row)->setData(Qt::UserRole + 4, id);
				if (m_resultsList->currentRow() == row) {
					m_imdbIdEdit->setText(id);
					m_imdbIdEdit->setEnabled(true);
					if (m_acceptAfterFetch) { m_acceptAfterFetch = false; accept(); }
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

			if (m_autoSelectSingle) {
				if (m_resultsList->count() == 1) {
					// Exactly one result — silently accept without showing the dialog.
					m_acceptAfterFetch = true;
					if (!m_imdbIdByRow.value(bestRow).isEmpty()) {
						m_imdbIdEdit->setText(m_imdbIdByRow.value(bestRow));
						m_acceptAfterFetch = false;
						accept();
					}
				} else {
					// Multiple results, or user manually searched — show the dialog.
					setWindowOpacity(1.0);
					raise();
					activateWindow();
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

} // namespace Mc
