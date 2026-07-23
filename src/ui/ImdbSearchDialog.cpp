#include "ui/ImdbSearchDialog.h"
#include "core/AppSettings.h"
#include "core/DatabaseManager.h"
#include "ui/McLanguageFlags.h"
#include "ui/McWindowGeometry.h"

#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QScreen>
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
	static constexpr int kPosterW = 58;   // poster column at left edge (~2:3 at 87px height)

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

	static void drawBackdrop(QPainter* p, const QStyleOptionViewItem& option,
	                         const QModelIndex& index)
	{
		const QPixmap raw = index.data(Qt::UserRole + 9).value<QPixmap>();
		if (raw.isNull()) return;
		const QString cacheKey = QStringLiteral("dlg_bd:%1:%2")
		    .arg(index.data(Qt::UserRole + 8).toString()).arg(option.rect.width());
		QPixmap scaled;
		if (!QPixmapCache::find(cacheKey, &scaled)) {
			scaled = raw.scaled(option.rect.size(), Qt::KeepAspectRatioByExpanding,
			                    Qt::SmoothTransformation);
			QPixmapCache::insert(cacheKey, scaled);
		}
		const int ox = (scaled.width()  - option.rect.width())  / 2;
		const int oy = (scaled.height() - option.rect.height()) / 2;
		p->save();
		p->setClipRect(option.rect);
		p->setOpacity(0.05);
		p->drawPixmap(option.rect.left() - ox, option.rect.top() - oy, scaled);
		p->restore();
	}

	QSize sizeHint(const QStyleOptionViewItem& option,
	               const QModelIndex& index) const override
	{
		QSize sh = QStyledItemDelegate::sizeHint(option, index);
		sh.setHeight(qMax(sh.height(), kPosterW * 3 / 2 + 4));  // poster 2:3 + padding
		return sh;
	}

	void paint(QPainter* p, const QStyleOptionViewItem& option,
	           const QModelIndex& index) const override
	{
		const bool sel    = option.state & QStyle::State_Selected;
		const bool active = option.state & QStyle::State_Active;
		const QPalette::ColorGroup grp = active ? QPalette::Active : QPalette::Inactive;
		const QString id = index.data(Qt::UserRole + 4).toString();

		// ── Background (full row) ──
		const QColor bg = sel
			? option.palette.color(grp, QPalette::Highlight)
			: (index.row() % 2 == 0
			   ? option.palette.color(grp, QPalette::Base)
			   : option.palette.color(grp, QPalette::AlternateBase));
		p->fillRect(option.rect, bg);

		// ── Fanart backdrop ──
		drawBackdrop(p, option, index);

		// ── Poster (left edge, full height) ──
		{
			const QRect posterRect(option.rect.left(), option.rect.top(),
			                       kPosterW, option.rect.height());
			const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
			if (!icon.isNull()) {
				const QPixmap pm = icon.pixmap(
				    icon.actualSize({kPosterW * 4, option.rect.height() * 4}));
				if (!pm.isNull()) {
					const QPixmap scaled = pm.scaled(posterRect.size(),
					    Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
					p->save();
					p->setClipRect(posterRect);
					const int ox = (scaled.width()  - posterRect.width())  / 2;
					const int oy = (scaled.height() - posterRect.height()) / 2;
					p->drawPixmap(posterRect.left() - ox, posterRect.top() - oy, scaled);
					p->restore();
				} else {
					p->fillRect(posterRect, option.palette.color(grp, QPalette::Mid).darker(110));
				}
			} else {
				p->fillRect(posterRect, option.palette.color(grp, QPalette::Mid).darker(110));
			}
		}

		// ── Title text ──
		const int rightReserved = id.isEmpty() ? 0 : kRatingW + kBtnW + kGap * 2;
		const QRect textRect = option.rect.adjusted(kPosterW + 8, 0, -rightReserved, 0);
		const QColor textColor = sel
			? option.palette.color(grp, QPalette::HighlightedText)
			: option.palette.color(grp, QPalette::Text);
		// Optional type tag (Movie / TV / Doc) from UserRole + 12.
		const QString typeTag = index.data(Qt::UserRole + 12).toString();
		QString titleText = index.data(Qt::DisplayRole).toString();
		if (!typeTag.isEmpty())
			titleText = QStringLiteral("%1  ·  %2").arg(titleText, typeTag);
		p->save();
		p->setPen(textColor);
		p->setFont(option.font);
		p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, titleText);
		p->restore();

		if (id.isEmpty()) return;

		// ── IMDb-style rating badge ───────────────────────────────────────────
		const double voteAvg   = index.data(Qt::UserRole + 6).toDouble();
		const int    voteCount = index.data(Qt::UserRole + 7).toInt();
		if (voteAvg > 0.0) {
			const QRect ratingRect(textRect.right() + kGap, option.rect.top(),
			                       kRatingW - kGap, option.rect.height());

			QFont starFont = option.font;
			starFont.setPointSizeF(option.font.pointSizeF() * 1.5);
			starFont.setBold(true);
			const int starH    = QFontMetrics(starFont).height();
			const int starW    = QFontMetrics(starFont).horizontalAdvance(QStringLiteral("\xe2\x98\x85"));
			const int midY     = option.rect.top() + option.rect.height() / 2;
			const int starTop  = midY - starH / 2 - (voteCount > 0 ? 4 : 0);
			p->save();
			p->setFont(starFont);
			p->setPen(QColor(0xF5, 0xC5, 0x18));
			p->drawText(QRect(ratingRect.left(), starTop, starW + 2, starH),
			            Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("\xe2\x98\x85"));
			p->restore();

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
			const QColor dimColor = sel ? textColor.darker(140)
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

// QComboBox subclass that always opens its popup above the widget.
class UpComboBox : public QComboBox {
public:
	using QComboBox::QComboBox;
	void showPopup() override {
		QComboBox::showPopup();
		QWidget* popup = view() ? view()->parentWidget() : nullptr;
		if (popup && popup->isVisible())
			popup->move(mapToGlobal(QPoint(0, -popup->height())));
	}
};

// QListWidget with no viewport top margin (setViewportMargins is protected).
class NoMarginListWidget : public QListWidget {
public:
	explicit NoMarginListWidget(QWidget* parent = nullptr) : QListWidget(parent) {
		setViewportMargins(0, 0, 0, 0);
	}
protected:
	void showEvent(QShowEvent* e) override {
		QListWidget::showEvent(e);
		setViewportMargins(0, 0, 0, 0);
	}
	void changeEvent(QEvent* e) override {
		QListWidget::changeEvent(e);
		if (e->type() == QEvent::StyleChange || e->type() == QEvent::FontChange)
			setViewportMargins(0, 0, 0, 0);
	}
};

} // anonymous namespace

namespace Mc {

ImdbSearchDialog::ImdbSearchDialog(const QString& videoPath,
									const QString& suggestedTitle,
									const QString& existingImdbId,
									const QString& tmdbApiKey,
									QWidget* parent,
									const QString& existingPosterPath,
									const QString& existingFanartPath)
	: QDialog(parent, Qt::Dialog)
	, m_videoPath(videoPath)
	, m_tmdbApiKey(tmdbApiKey)
	, m_existingImdbId(existingImdbId)
	, m_existingPosterPath(existingPosterPath)
	, m_existingFanartPath(existingFanartPath)
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
		if (!geo.isEmpty()) {
			restoreGeometry(geo);
			ensureGeometryFitsScreen(this);
		} else
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

	// Results list
	m_resultsList = new QListWidget(this);
	m_resultsList->setSelectionMode(QAbstractItemView::SingleSelection);
	m_resultsList->setAlternatingRowColors(true);
	m_resultsList->setIconSize({54, 80});
	m_resultsList->setSpacing(0);
	m_resultsList->setFrameShape(QFrame::NoFrame);
	m_resultsList->setStyleSheet("QListView { outline: 0; } QListView::item { outline: none; }");
	m_resultsList->setItemDelegate(new ImdbResultDelegate(m_resultsList));
	m_resultsList->viewport()->setMouseTracking(true);
	m_resultsList->viewport()->installEventFilter(this);
	m_resultsList->installEventFilter(this);

	// Poster gallery panel (right side of splitter)
	auto* galleryPanel  = new QWidget(this);
	auto* galleryLayout = new QVBoxLayout(galleryPanel);
	galleryLayout->setContentsMargins(0, 0, 0, 0);
	galleryLayout->setSpacing(6);

	m_langFilter = new UpComboBox(galleryPanel);
	m_langFilter->addItem(tr("All"), QString{});
	m_langFilter->setFixedWidth(140);
	m_langFilter->setIconSize({20, 14});

	auto* filterRow = new QHBoxLayout;
	filterRow->addWidget(new QLabel(tr("Language:"), galleryPanel));
	filterRow->addWidget(m_langFilter);
	filterRow->addStretch();

	m_gallerySplitter = new QSplitter(Qt::Horizontal, galleryPanel);

	auto* posterSide  = new QWidget(galleryPanel);
	auto* posterSideL = new QVBoxLayout(posterSide);
	posterSideL->setContentsMargins(0, 0, 0, 0);
	posterSideL->setSpacing(0);

	m_posterGallery = new NoMarginListWidget(posterSide);
	m_posterGallery->setViewMode(QListView::IconMode);
	m_posterGallery->setIconSize({90, 135});
	m_posterGallery->setGridSize({100, 158});
	m_posterGallery->setResizeMode(QListView::Adjust);
	m_posterGallery->setMovement(QListView::Static);
	m_posterGallery->setUniformItemSizes(true);
	m_posterGallery->setWordWrap(false);
	m_posterGallery->setSpacing(0);
	m_posterGallery->setFrameShape(QFrame::NoFrame);
	posterSideL->addWidget(m_posterGallery, 1);

	auto* fanartSide  = new QWidget(galleryPanel);
	auto* fanartSideL = new QVBoxLayout(fanartSide);
	fanartSideL->setContentsMargins(0, 0, 0, 0);
	fanartSideL->setSpacing(0);

	m_fanartGallery = new NoMarginListWidget(fanartSide);
	m_fanartGallery->setViewMode(QListView::IconMode);
	m_fanartGallery->setIconSize({150, 84});
	m_fanartGallery->setGridSize({158, 88});
	m_fanartGallery->setResizeMode(QListView::Adjust);
	m_fanartGallery->setMovement(QListView::Static);
	m_fanartGallery->setUniformItemSizes(true);
	m_fanartGallery->setWordWrap(false);
	m_fanartGallery->setSpacing(0);
	m_fanartGallery->setFrameShape(QFrame::NoFrame);
	fanartSideL->addWidget(m_fanartGallery, 1);


	m_gallerySplitter->addWidget(posterSide);
	m_gallerySplitter->addWidget(fanartSide);
	m_gallerySplitter->setStretchFactor(0, 1);
	m_gallerySplitter->setStretchFactor(1, 1);
	m_gallerySplitter->setContentsMargins(0, 0, 0, 0);
	{
		QSettings s(Mc::AppSettings::geometryFilePath(), QSettings::IniFormat);
		const QByteArray gst = s.value("imdbSearchDialog/gallerySplitter").toByteArray();
		if (!gst.isEmpty())
			m_gallerySplitter->restoreState(gst);
		else
			m_gallerySplitter->setSizes({3 * 100 + 10, 2 * 158 + 10});
	}
	galleryLayout->addWidget(m_gallerySplitter, 1);
	galleryLayout->addLayout(filterRow);

	// Left side: results list + IMDb ID row beneath it
	auto* leftSide = new QWidget(this);
	auto* leftLayout = new QVBoxLayout(leftSide);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->setSpacing(6);
	leftLayout->addWidget(m_resultsList, 1);
	{
		auto* idRow = new QHBoxLayout;
		idRow->setContentsMargins(0, 0, 0, 0);
		idRow->addWidget(new QLabel(tr("IMDb ID:"), leftSide));
		m_imdbIdEdit = new QLineEdit(leftSide);
		m_imdbIdEdit->setPlaceholderText("tt0000000");
		m_imdbIdEdit->setFixedWidth(140);
		if (!existingImdbId.isEmpty())
			m_imdbIdEdit->setText(existingImdbId);
		idRow->addWidget(m_imdbIdEdit);
		idRow->addStretch();
		leftLayout->addLayout(idRow);
	}

	auto* splitter = new QSplitter(Qt::Horizontal, this);
	splitter->addWidget(leftSide);
	splitter->addWidget(galleryPanel);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 2);
	splitter->setContentsMargins(0, 0, 0, 0);

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
		m_userSelectedPoster = true;
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
	connect(m_fanartGallery, &QListWidget::currentItemChanged,
	        this, [this](QListWidgetItem* item) {
		m_selectedFanartPath = item ? item->data(Qt::UserRole).toString() : QString{};
		if (!item) return;  // gallery clearing — leave the search-result backdrop intact
		m_userSelectedFanart = true;
		const int row = m_resultsList->currentRow();
		if (row < 0 || row >= m_resultsList->count()) return;
		// Update UserRole+8 (scale-cache key) so drawBackdrop doesn't serve the old scaled image.
		// UserRole+9 carries the raw pixmap; both must change together.
		QPixmap px;
		const QByteArray bytes = m_fanartThumbData.value(m_selectedFanartPath);
		if (!bytes.isEmpty()) px.loadFromData(bytes);
		m_resultsList->item(row)->setData(Qt::UserRole + 8, m_selectedFanartPath);
		m_resultsList->item(row)->setData(Qt::UserRole + 9, QVariant::fromValue(px));
		m_resultsList->viewport()->update();
	});
	connect(m_langFilter, &QComboBox::currentIndexChanged, this, [this]() {
		m_galleryFilter = m_langFilter->currentData().toString();
		populateGallery();
	});

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

	// Auto-search when the dialog opens. Prefer a direct lookup by the known IMDb
	// ID over a folder-name title search — the latter can resolve to the wrong
	// movie (sequels, remakes, shared titles) even when the correct ID is on file.
	static const QRegularExpression validExistingId(R"(^tt\d{7,8}$)");
	if (!tmdbApiKey.isEmpty() && validExistingId.match(existingImdbId).hasMatch())
		QMetaObject::invokeMethod(this, &ImdbSearchDialog::searchByExistingImdbId, Qt::QueuedConnection);
	else if (!tmdbApiKey.isEmpty() && !suggestedTitle.isEmpty())
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
	for (auto it = m_fanartThumbReplies.begin(); it != m_fanartThumbReplies.end(); ++it)
		it.key()->disconnect(this);
	m_fanartThumbReplies.clear();
	for (QNetworkReply* r : std::as_const(m_thumbReplyByRow)) r->disconnect(this);
	m_thumbReplyByRow.clear();
	for (QNetworkReply* r : std::as_const(m_backdropReplyByRow)) r->disconnect(this);
	m_backdropReplyByRow.clear();
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
	if (m_gallerySplitter)
		s.setValue("imdbSearchDialog/gallerySplitter", m_gallerySplitter->saveState());
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
QString    ImdbSearchDialog::selectedMediaType()  const { return m_selectedMediaType; }
QString    ImdbSearchDialog::selectedFanartPath() const
{
	// Only return a path if the user explicitly picked a backdrop; otherwise the
	// caller should leave the existing fanart intact (worker checks before overwriting).
	return m_userSelectedFanart ? m_selectedFanartPath : QString{};
}
QByteArray ImdbSearchDialog::selectedImageData()  const
{
	if (!m_galleryImageData.isEmpty()) return m_galleryImageData;
	if (!m_resultsList) return {};
	// If user explicitly picked from gallery, use the TMDB thumbnail
	if (m_userSelectedPoster) return m_thumbDataByRow.value(m_resultsList->currentRow());
	// No explicit gallery selection AND the resolved IMDb id is unchanged from what was
	// already on file: prefer the existing library poster (preserves quality and avoids
	// overwriting a higher-resolution image with the w92 TMDB thumbnail). If the user picked
	// a different title, the id differs and we must fall through to download its own poster.
	if (!m_existingPosterPath.isEmpty() && !m_existingImdbId.isEmpty()
	    && selectedImdbId() == m_existingImdbId) {
		QFile f(m_existingPosterPath);
		if (f.open(QIODevice::ReadOnly)) {
			const QByteArray bytes = f.readAll();
			if (!bytes.isEmpty()) return bytes;
		}
	}
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

int ImdbSearchDialog::selectedTmdbId() const
{
	if (!m_resultsList) return 0;
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole).toInt() : 0;
}

QString ImdbSearchDialog::selectedOriginalTitle() const
{
	if (!m_resultsList) return {};
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 10).toString() : QString{};
}

QString ImdbSearchDialog::selectedReleaseDate() const
{
	if (!m_resultsList) return {};
	const QListWidgetItem* item = m_resultsList->currentItem();
	return item ? item->data(Qt::UserRole + 11).toString() : QString{};
}


// abort() emits finished() synchronously, and every per-row handler starts by
// removing itself from the hash it lives in — aborting while iterating those
// members directly is iterator-invalidation UB. Detach the handlers and empty
// the members first, then abort local copies; the reply → deleteLater
// self-cleanup connections survive the disconnect(this).
void ImdbSearchDialog::abortResultRequests()
{
	if (m_searchReply) { m_searchReply->abort(); m_searchReply = nullptr; }
	if (m_extIdsReply) { m_extIdsReply->abort(); m_extIdsReply = nullptr; }

	const auto thumbs    = m_thumbReplyByRow;    m_thumbReplyByRow.clear();
	const auto backdrops = m_backdropReplyByRow; m_backdropReplyByRow.clear();
	const auto prefetch  = m_prefetchByRow;      m_prefetchByRow.clear();
	for (QNetworkReply* r : thumbs)    { r->disconnect(this); r->abort(); }
	for (QNetworkReply* r : backdrops) { r->disconnect(this); r->abort(); }
	for (QNetworkReply* r : prefetch)  { r->disconnect(this); r->abort(); }

	m_thumbDataByRow.clear();
	m_imdbIdByRow.clear();
}

void ImdbSearchDialog::onSearch()
{
	if (m_tmdbApiKey.isEmpty()) return;

	const QString query = m_searchEdit->text().trimmed();
	if (query.isEmpty()) return;

	abortResultRequests();

	m_resultsList->clear();
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

	// Multi-search covers movies + TV (+ person, which we filter out).
	QUrl url("https://api.themoviedb.org/3/search/multi");
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
				if (m_autoSelectSingle) { setWindowOpacity(1.0); raise(); activateWindow(); }
			reply->deleteLater();
			return;
		}

		const QJsonObject root    = QJsonDocument::fromJson(reply->readAll()).object();
		const QJsonArray  raw     = root["results"].toArray();
		reply->deleteLater();

		// Keep only movie/tv and tag each object with media_type for populate.
		QJsonArray results;
		for (const QJsonValue& v : raw) {
			QJsonObject obj = v.toObject();
			const QString mt = obj[QStringLiteral("media_type")].toString();
			if (mt != QLatin1String("movie") && mt != QLatin1String("tv"))
				continue;
			obj[QStringLiteral("_mc_media_type")] = mt;
			results.append(obj);
		}

		if (results.isEmpty())
			return;

		populateSearchResults(results, yearMatch);
	});
}

void ImdbSearchDialog::searchByExistingImdbId()
{
	if (m_tmdbApiKey.isEmpty() || m_existingImdbId.isEmpty()) return;

	abortResultRequests();

	m_resultsList->clear();
	m_btnSearch->setEnabled(false);

	QUrlQuery params;
	params.addQueryItem("api_key",         m_tmdbApiKey);
	params.addQueryItem("external_source", "imdb_id");

	QUrl url(QStringLiteral("https://api.themoviedb.org/3/find/%1").arg(m_existingImdbId));
	url.setQuery(params);

	QNetworkRequest req(url);
	req.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");

	m_searchReply = m_nam->get(req);
	connect(m_searchReply, &QNetworkReply::finished, m_searchReply, &QObject::deleteLater);
	connect(m_searchReply, &QNetworkReply::finished, this, [this]() {
		auto* reply = qobject_cast<QNetworkReply*>(sender());
		if (!reply || reply != m_searchReply) return;
		m_searchReply = nullptr;
		m_btnSearch->setEnabled(true);

		QJsonArray results;
		if (reply->error() == QNetworkReply::NoError) {
			const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
			for (const QJsonValue& v : root[QStringLiteral("movie_results")].toArray()) {
				QJsonObject obj = v.toObject();
				obj[QStringLiteral("_mc_media_type")] = QStringLiteral("movie");
				results.append(obj);
			}
			for (const QJsonValue& v : root[QStringLiteral("tv_results")].toArray()) {
				QJsonObject obj = v.toObject();
				obj[QStringLiteral("_mc_media_type")] = QStringLiteral("tv");
				results.append(obj);
			}
		}
		reply->deleteLater();

		if (!results.isEmpty()) {
			populateSearchResults(results, QRegularExpressionMatch{});
			return;
		}

		// The known ID didn't resolve on TMDB (removed/merged) or the request
		// failed — fall back to a title search so the dialog isn't left empty.
		if (!m_searchEdit->text().trimmed().isEmpty())
			onSearch();
	});
}

void ImdbSearchDialog::populateSearchResults(const QJsonArray& results, const QRegularExpressionMatch& yearMatch)
{
	static constexpr int kGenreDocumentary = 99;

	auto hasGenre = [](const QJsonObject& obj, int genreId) {
		for (const QJsonValue& v : obj[QStringLiteral("genre_ids")].toArray()) {
			if (v.toInt() == genreId) return true;
		}
		return false;
	};

	for (const auto& v : results) {
		const QJsonObject obj = v.toObject();
		const QString kind = obj.contains(QStringLiteral("_mc_media_type"))
		    ? obj[QStringLiteral("_mc_media_type")].toString()
		    : (obj.contains(QStringLiteral("name")) && !obj.contains(QStringLiteral("title"))
		           ? QStringLiteral("tv") : QStringLiteral("movie"));
		const bool isTv = (kind == QLatin1String("tv"));

		const QString title = isTv
		    ? obj[QStringLiteral("name")].toString()
		    : obj[QStringLiteral("title")].toString();
		const QString originalTitle = isTv
		    ? obj[QStringLiteral("original_name")].toString()
		    : obj[QStringLiteral("original_title")].toString();
		const QString releaseDate = isTv
		    ? obj[QStringLiteral("first_air_date")].toString()
		    : obj[QStringLiteral("release_date")].toString();
		const int         year         = releaseDate.left(4).toInt();
		const int         tmdbId       = obj[QStringLiteral("id")].toInt();
		const QString     posterPath   = obj[QStringLiteral("poster_path")].toString();
		const QString     backdropPath = obj[QStringLiteral("backdrop_path")].toString();
		const QString     origLang     = obj[QStringLiteral("original_language")].toString();
		const double      voteAvg      = obj[QStringLiteral("vote_average")].toDouble();
		const int         voteCount    = obj[QStringLiteral("vote_count")].toInt();

		const QString mediaType = Mc::MediaTypes::classify(kind, hasGenre(obj, kGenreDocumentary));
		const QString typeTag =
		    mediaType == QLatin1String(Mc::MediaTypes::Documentary) ? QStringLiteral("Doc")
		  : mediaType == QLatin1String(Mc::MediaTypes::Tv)          ? QStringLiteral("TV")
		  : mediaType == QLatin1String(Mc::MediaTypes::Movie)       ? QStringLiteral("Movie")
		                                                            : QString{};

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
		item->setData(Qt::UserRole + 8, backdropPath);
		item->setData(Qt::UserRole + 10, originalTitle);
		item->setData(Qt::UserRole + 11, releaseDate);
		item->setData(Qt::UserRole + 12, typeTag);
		item->setData(Qt::UserRole + 13, mediaType);
		item->setData(Qt::UserRole + 14, kind);  // "movie" or "tv" for API endpoints

		// Fetch poster thumbnail asynchronously
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
						// Don't overwrite the pre-loaded existing poster icon in the
						// single-result case — the gallery selection lambda handles updates.
						if (m_resultsList->count() > 1 || m_existingPosterPath.isEmpty()) {
							m_resultsList->item(row)->setIcon(
								QIcon(px.scaled(54, 80, Qt::KeepAspectRatio,
								                Qt::SmoothTransformation)));
						}
					}
				}
				r->deleteLater();
			});
		}

		// Fetch backdrop thumbnail asynchronously (w300 is enough for a 10% opacity tint)
		if (!backdropPath.isEmpty()) {
			const QUrl bdUrl(
				QStringLiteral("https://image.tmdb.org/t/p/w300%1").arg(backdropPath));
			QNetworkRequest bdReq(bdUrl);
			bdReq.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
			QNetworkReply* r = m_nam->get(bdReq);
			const int row = m_resultsList->count() - 1;
			m_backdropReplyByRow[row] = r;
			connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
			connect(r, &QNetworkReply::finished, this, [this, r, row]() {
				m_backdropReplyByRow.remove(row);
				if (r->error() == QNetworkReply::NoError) {
					QPixmap px;
					if (px.loadFromData(r->readAll()) && row < m_resultsList->count()) {
						// Don't overwrite the pre-loaded existing fanart backdrop in the
						// single-result case — the gallery selection lambda handles updates.
						if (m_resultsList->count() > 1 || m_existingFanartPath.isEmpty()) {
							m_resultsList->item(row)->setData(Qt::UserRole + 9,
							                                  QVariant::fromValue(px));
						}
					}
				}
				r->deleteLater();
			});
		}
	}


	// Prefetch IMDb IDs for all results in parallel
	for (int row = 0; row < m_resultsList->count(); ++row) {
		const int tmdbId = m_resultsList->item(row)->data(Qt::UserRole).toInt();
		const QString kind = m_resultsList->item(row)->data(Qt::UserRole + 14).toString();
		const QString apiKind = (kind == QLatin1String("tv")) ? QStringLiteral("tv")
		                                                      : QStringLiteral("movie");
		QUrlQuery pq;
		pq.addQueryItem("api_key", m_tmdbApiKey);
		QUrl pUrl(QStringLiteral("https://api.themoviedb.org/3/%1/%2/external_ids")
		              .arg(apiKind).arg(tmdbId));
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

		// When there is exactly one result, pre-populate the mini card with the
		// current library poster/backdrop so the user sees what they already have
		// before any TMDB thumbnail arrives (Issue 7).
		if (m_resultsList->count() == 1
		    && (!m_existingPosterPath.isEmpty() || !m_existingFanartPath.isEmpty())) {
			auto* firstItem = m_resultsList->item(0);
			if (firstItem) {
				if (!m_existingPosterPath.isEmpty()) {
					QPixmap px(m_existingPosterPath);
					if (!px.isNull())
						firstItem->setIcon(QIcon(px.scaled(54, 80, Qt::KeepAspectRatio,
						                                   Qt::SmoothTransformation)));
				}
				if (!m_existingFanartPath.isEmpty()) {
					QPixmap px(m_existingFanartPath);
					if (!px.isNull()) {
						if (px.height() > 300) {
							const int y = (px.height() - 300) / 2;
							px = px.copy(0, y, px.width(), 300);
						}
						firstItem->setData(Qt::UserRole + 8, m_existingFanartPath);
						firstItem->setData(Qt::UserRole + 9, QVariant::fromValue(px));
					}
				}
			}
		}

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
	// A pending accept-on-fetch was armed for the previously selected row —
	// moving to another row must disarm it, or that row's prefetch landing
	// would silently accept() a movie the user never confirmed. (The batch
	// auto-select flow arms only after setCurrentRow(), so it is unaffected.)
	m_acceptAfterFetch = false;

	const int row = m_resultsList->currentRow();
	const QListWidgetItem* item = m_resultsList->currentItem();
	if (!item) return;

	m_userSelectedPoster = false;
	m_userSelectedFanart = false;
	m_selectedTitle      = item->data(Qt::UserRole + 1).toString();
	m_selectedYear       = item->data(Qt::UserRole + 2).toInt();
	m_selectedPosterPath = item->data(Qt::UserRole + 3).toString();
	m_selectedMediaType  = item->data(Qt::UserRole + 13).toString();
	m_galleryImageData.clear();

	const QString kind = item->data(Qt::UserRole + 14).toString();
	const QString apiKind = (kind == QLatin1String("tv")) ? QStringLiteral("tv")
	                                                      : QStringLiteral("movie");
	fetchPosterImages(item->data(Qt::UserRole).toInt(),
	                  item->data(Qt::UserRole + 5).toString(),
	                  apiKind);

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
	QUrl url(QStringLiteral("https://api.themoviedb.org/3/%1/%2/external_ids")
	             .arg(apiKind).arg(tmdbId));
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

void ImdbSearchDialog::fetchPosterImages(int tmdbId, const QString& origLang, const QString& kind)
{
	if (m_imagesReply) { m_imagesReply->disconnect(this); m_imagesReply = nullptr; }
	for (auto it = m_galleryThumbReplies.begin(); it != m_galleryThumbReplies.end(); ++it)
		it.key()->disconnect(this);
	m_galleryThumbReplies.clear();
	m_galleryThumbData.clear();
	m_allPosters.clear();
	m_posterGallery->clear();
	for (auto it = m_fanartThumbReplies.begin(); it != m_fanartThumbReplies.end(); ++it)
		it.key()->disconnect(this);
	m_fanartThumbReplies.clear();
	m_fanartThumbData.clear();
	m_allBackdrops.clear();
	m_fanartGallery->clear();
	m_selectedFanartPath.clear();
	m_langFilter->blockSignals(true);
	m_langFilter->clear();
	m_langFilter->addItem(tr("All"), QString{});
	m_langFilter->blockSignals(false);

	QStringList langs = {QStringLiteral("en"), QStringLiteral("null")};
	if (!origLang.isEmpty() && origLang != QLatin1String("en"))
		langs.append(origLang);
	for (const QString& iso2 : std::as_const(m_understoodLanguages)) {
		const QString iso1 = McLanguageFlags::toIso1(iso2);
		if (!iso1.isEmpty() && !langs.contains(iso1))
			langs.append(iso1);
	}

	const QString apiKind = (kind == QLatin1String("tv")) ? QStringLiteral("tv")
	                                                      : QStringLiteral("movie");
	QUrlQuery params;
	params.addQueryItem(QStringLiteral("api_key"), m_tmdbApiKey);
	params.addQueryItem(QStringLiteral("include_image_language"), langs.join(u','));
	QUrl url(QStringLiteral("https://api.themoviedb.org/3/%1/%2/images")
	             .arg(apiKind).arg(tmdbId));
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
				return;
		}

		const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
		const QJsonArray posters   = root[QStringLiteral("posters")].toArray();
		const QJsonArray backdrops = root[QStringLiteral("backdrops")].toArray();

		m_allPosters.clear();
		m_allPosters.reserve(posters.size());
		for (const auto& v : posters)
			m_allPosters.append(v.toObject());

		m_allBackdrops.clear();
		m_allBackdrops.reserve(backdrops.size());
		for (const auto& v : backdrops)
			m_allBackdrops.append(v.toObject());

		// Rebuild language filter from what the response actually contains
		QSet<QString> langs;
		for (const auto& p : std::as_const(m_allPosters))
			langs.insert(p[QStringLiteral("iso_639_1")].isNull()
			             ? QStringLiteral("null")
			             : p[QStringLiteral("iso_639_1")].toString());

		m_langFilter->blockSignals(true);
		m_langFilter->clear();
		const qreal    dpr    = m_langFilter->devicePixelRatioF();
		const QSize    iconSz = m_langFilter->iconSize();
		QPixmap placeholder(QSize(qRound(iconSz.width() * dpr), qRound(iconSz.height() * dpr)));
		placeholder.fill(Qt::transparent);
		placeholder.setDevicePixelRatio(dpr);
		const QIcon noIcon(placeholder);
		m_langFilter->addItem(noIcon, tr("All"), QString{});
		if (langs.contains(QStringLiteral("null")))
			m_langFilter->addItem(noIcon, tr("Textless"), QStringLiteral("null"));
		QStringList sorted(langs.begin(), langs.end());
		sorted.sort();
		for (const QString& l : std::as_const(sorted)) {
			if (l != QLatin1String("null") && !l.isEmpty()) {
				const QPixmap flagPx = McLanguageFlags::flag(l, iconSz.height(), dpr);
				m_langFilter->addItem(flagPx.isNull() ? noIcon : QIcon(flagPx), l.toUpper(), l);
			}
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
		populateFanartGallery();
	});
}

void ImdbSearchDialog::populateFanartGallery()
{
	m_fanartGallery->clear();

	for (const auto& b : std::as_const(m_allBackdrops)) {
		const QString path    = b[QStringLiteral("file_path")].toString();
		const double  voteAvg = b[QStringLiteral("vote_average")].toDouble();

		auto* item = new QListWidgetItem(m_fanartGallery);
		item->setData(Qt::UserRole, path);
		item->setSizeHint({158, 88});
		item->setToolTip(QStringLiteral("\xe2\x98\x85 %1").arg(voteAvg, 0, 'f', 1));

		if (m_fanartThumbData.contains(path)) {
			QPixmap px;
			if (px.loadFromData(m_fanartThumbData[path]))
				item->setIcon(QIcon(px.scaled(150, 84, Qt::KeepAspectRatio,
				                              Qt::SmoothTransformation)));
		} else {
			const QUrl thumbUrl(QStringLiteral("https://image.tmdb.org/t/p/w300%1").arg(path));
			QNetworkRequest thumbReq(thumbUrl);
			thumbReq.setHeader(QNetworkRequest::UserAgentHeader, "MediaCurator/1.0");
			QNetworkReply* r = m_nam->get(thumbReq);
			m_fanartThumbReplies[r] = path;
			connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
			connect(r, &QNetworkReply::finished, this, [this, r, path]() {
				m_fanartThumbReplies.remove(r);
				if (r->error() != QNetworkReply::NoError) return;
				const QByteArray bytes = r->readAll();
				m_fanartThumbData[path] = bytes;
				QPixmap px;
				if (!px.loadFromData(bytes)) return;
				const QIcon icon(px.scaled(150, 84, Qt::KeepAspectRatio,
				                           Qt::SmoothTransformation));
				for (int i = 0; i < m_fanartGallery->count(); ++i) {
					if (m_fanartGallery->item(i)->data(Qt::UserRole).toString() == path) {
						m_fanartGallery->item(i)->setIcon(icon);
						break;
					}
				}
				// If this thumb belongs to the currently selected backdrop, push it to the mini card now.
				if (path == m_selectedFanartPath) {
					const int row = m_resultsList->currentRow();
					if (row >= 0 && row < m_resultsList->count()) {
						m_resultsList->item(row)->setData(Qt::UserRole + 8, path);
						m_resultsList->item(row)->setData(Qt::UserRole + 9, QVariant::fromValue(px));
						m_resultsList->viewport()->update();
					}
				}
			});
		}
	}

}

void ImdbSearchDialog::populateGallery()
{
	// In "All" mode, show language labels — keep full-height grid cells.
	// In filtered mode, there's nothing useful to label, so shrink cells to recover space.
	const bool showLabels = m_galleryFilter.isEmpty();
	m_posterGallery->blockSignals(true);
	m_posterGallery->setGridSize(showLabels ? QSize(100, 158) : QSize(100, 140));
	m_posterGallery->clear();

	for (const auto& p : std::as_const(m_allPosters)) {
		const QString lang = p[QStringLiteral("iso_639_1")].isNull()
		                     ? QStringLiteral("null")
		                     : p[QStringLiteral("iso_639_1")].toString();
		if (!m_galleryFilter.isEmpty() && lang != m_galleryFilter)
			continue;

		const QString path      = p[QStringLiteral("file_path")].toString();
		const double  voteAvg   = p[QStringLiteral("vote_average")].toDouble();
		const QString langLabel = showLabels
			? (lang == QLatin1String("null") ? tr("Textless") : lang.toUpper())
			: QString{};

		auto* item = new QListWidgetItem(langLabel, m_posterGallery);
		item->setData(Qt::UserRole, path);
		item->setSizeHint(showLabels ? QSize(100, 158) : QSize(100, 140));
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
						if (m_userSelectedPoster && cur && cur->data(Qt::UserRole).toString() == path) {
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
	m_posterGallery->blockSignals(false);

}

} // namespace Mc
