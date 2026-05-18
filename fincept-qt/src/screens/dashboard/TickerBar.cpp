#include "screens/dashboard/TickerBar.h"

#include "services/markets/MarketDataService.h"
#include "storage/repositories/SettingsRepository.h"
#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QResizeEvent>
#include <QtConcurrent>

#include <cmath>

namespace fincept::screens {

static constexpr int    kItemSpacing      = 40;
static constexpr int    kSegmentGap       = 8;
static constexpr int    kEditBarHeight    = 24;
static const char*      kSettingsKey      = "ticker_bar_symbols";
static const char*      kSettingsCategory = "dashboard";

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

TickerBar::TickerBar(QWidget* parent) : QWidget(parent) {
    setFixedHeight(kEditBarHeight);
    setContextMenuPolicy(Qt::DefaultContextMenu);

    auto apply_bg = [this]() {
        setStyleSheet(QString("background-color: %1;").arg(ui::colors::BG_BASE()));
    };
    apply_bg();
    connect(&ui::ThemeManager::instance(), &ui::ThemeManager::theme_changed, this,
            [this, apply_bg](const ui::ThemeTokens&) {
                apply_bg();
                update();
            });

    // Time-based scroll with stutter-spreading catch-up.
    //
    // The main thread blocks for ~100ms every second when the realtime bot
    // refresh tick (account / positions / risk / watchlist) all callback
    // at once. During that block no paint events fire. Naive elapsed-time
    // advance would catch up the missed ~2px in a single frame after the
    // unblock, which the eye reads as a "stop + jump". Instead we advance
    // by the EXPECTED frame budget (16ms × 20 px/s ≈ 0.32px) and bank any
    // excess into pending_catchup_, then drain ~25% of that bank each
    // subsequent frame. Result: the marquee keeps moving continuously and
    // accelerates briefly to swallow the lag, instead of freezing.
    elapsed_.start();
    last_advance_ms_ = elapsed_.elapsed();
    connect(&scroll_timer_, &QTimer::timeout, this, [this]() {
        const qint64 now = elapsed_.elapsed();
        const qint64 dt  = now - last_advance_ms_;
        last_advance_ms_ = now;
        constexpr double kPxPerMs = 20.0 / 1000.0;        // 20 px/sec
        constexpr qint64 kFrameBudgetMs = 16;             // ~60 Hz target
        constexpr qint64 kMaxStutterMs  = 500;            // cap to avoid teleport on sleep/resume
        const qint64 dt_clamped = std::min<qint64>(dt, kMaxStutterMs);

        // Advance by at most one frame's worth this tick — bank the rest.
        const qint64 frame_dt = std::min<qint64>(dt_clamped, kFrameBudgetMs);
        offset_         += frame_dt * kPxPerMs;
        pending_catchup_ += (dt_clamped - frame_dt) * kPxPerMs;

        // Drain ~25% of the catch-up bank per frame so a 100ms stutter
        // gets absorbed over ~5 frames (~80ms perceived) instead of in
        // one visible jump.
        if (pending_catchup_ > 0) {
            const double drain = std::max(0.05, pending_catchup_ * 0.25);
            const double take  = std::min(drain, pending_catchup_);
            offset_         += take;
            pending_catchup_ -= take;
        }

        if (total_width_ > 0 && offset_ >= total_width_)
            offset_ = std::fmod(offset_, total_width_);
        update();
    });
    scroll_timer_.setInterval(16); // ~60 Hz — actual advance is time-based,
                                   // tighter interval just smooths sampling

    // ── Inline edit overlay (hidden by default) ───────────────────────────────
    edit_bar_ = new QWidget(this);
    edit_bar_->setFixedHeight(kEditBarHeight);
    edit_bar_->hide();

    auto* hl = new QHBoxLayout(edit_bar_);
    hl->setContentsMargins(6, 2, 6, 2);
    hl->setSpacing(4);

    auto* lbl = new QLabel("SYMBOLS:", edit_bar_);
    lbl->setStyleSheet(QString("color:%1; font-size:9px; font-weight:bold; background:transparent;")
                           .arg(ui::colors::TEXT_TERTIARY()));
    hl->addWidget(lbl);

    edit_input_ = new QLineEdit(edit_bar_);
    edit_input_->setPlaceholderText("AAPL, MSFT, ^GSPC, BTC-USD ...");
    edit_input_->setStyleSheet(
        QString("QLineEdit { background:%1; color:%2; border:1px solid %3;"
                " font-size:10px; padding:1px 6px; font-family:Consolas; }"
                "QLineEdit:focus { border-color:%4; }")
            .arg(ui::colors::BG_BASE(), ui::colors::TEXT_PRIMARY(),
                 ui::colors::BORDER_DIM(), ui::colors::AMBER()));
    connect(edit_input_, &QLineEdit::returnPressed, this, &TickerBar::commit_edit);
    hl->addWidget(edit_input_, 1);

    edit_ok_ = new QPushButton("OK", edit_bar_);
    edit_ok_->setFixedWidth(32);
    edit_ok_->setStyleSheet(
        QString("QPushButton { background:%1; color:%2; border:none;"
                " font-size:9px; font-weight:bold; padding:2px; }"
                "QPushButton:hover { background:%3; }")
            .arg(ui::colors::AMBER(), ui::colors::BG_BASE(), ui::colors::AMBER()));
    connect(edit_ok_, &QPushButton::clicked, this, &TickerBar::commit_edit);
    hl->addWidget(edit_ok_);

    edit_cancel_ = new QPushButton("✕", edit_bar_);
    edit_cancel_->setFixedWidth(24);
    edit_cancel_->setStyleSheet(
        QString("QPushButton { background:transparent; color:%1; border:none;"
                " font-size:11px; padding:2px; }"
                "QPushButton:hover { color:%2; }")
            .arg(ui::colors::TEXT_TERTIARY(), ui::colors::RED()));
    connect(edit_cancel_, &QPushButton::clicked, this, &TickerBar::hide_edit_bar);
    hl->addWidget(edit_cancel_);

    // Load persisted symbols (falls back to defaults if nothing saved yet).
    load_symbols();
}

// ─────────────────────────────────────────────────────────────────────────────
// Symbol persistence
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::load_symbols() {
    // Cold-start seed — US large-caps + high-volume movers. We DROP the
    // international index symbols (^N225 / ^FTSE / etc.) that used to be
    // in this seed: the runner is meant for US-stock context and the
    // foreign indices made the tape feel stale (closed exchanges, last
    // tick from hours ago, no actionable info while you're trading US
    // markets). DashboardScreen replaces this list with Polygon's live
    // top-movers (gainers + losers) every 5 minutes via set_symbols, so
    // this seed is only what you see for the first few seconds.
    const QStringList defaults = {
        // S&P large-caps that always have something happening intraday
        "SPY", "QQQ", "AAPL", "MSFT", "NVDA", "TSLA", "GOOGL", "AMZN",
        "META", "AMD", "JPM", "V", "MA", "UNH", "XOM", "WMT",
        // Top-of-mind names from MarketDataService::mover_symbols
        "SMCI", "PLTR", "MSTR", "INTC", "BA", "COIN", "DIS", "NFLX",
    };

    // Attempt to load from settings on a background thread; fall back to defaults
    // immediately so the ticker isn't blank while the DB read is in flight.
    symbols_ = defaults;

    QPointer<TickerBar> self = this;
    (void)QtConcurrent::run([self, defaults]() {
        auto result = fincept::SettingsRepository::instance().get(kSettingsKey);
        QStringList loaded;
        if (result.is_ok() && !result.value().isEmpty()) {
            for (const QString& s : result.value().split(',')) {
                const QString t = s.trimmed().toUpper();
                if (!t.isEmpty())
                    loaded << t;
            }
        }
        QMetaObject::invokeMethod(self, [self, loaded, defaults]() {
            if (!self) return;
            self->symbols_ = loaded.isEmpty() ? defaults : loaded;
            emit self->symbols_changed(self->symbols_);
        }, Qt::QueuedConnection);
    });
}

void TickerBar::save_symbols() {
    const QString value = symbols_.join(",");
    (void)QtConcurrent::run([value]() {
        fincept::SettingsRepository::instance().set(kSettingsKey, value, kSettingsCategory);
    });
}

void TickerBar::set_symbols(const QStringList& symbols) {
    // De-dupe + uppercase; drop empties. Mirrors commit_edit's hygiene
    // so a Polygon-supplied list can't break the marquee invariants.
    QStringList clean;
    for (const QString& s : symbols) {
        const QString u = s.trimmed().toUpper();
        if (!u.isEmpty() && !clean.contains(u)) clean << u;
    }
    if (clean.isEmpty() || clean == symbols_) return;
    symbols_ = clean;
    // Programmatic update — do NOT persist (user's hand-curated list in
    // SettingsRepository must survive across these rotations).
    emit symbols_changed(symbols_);
}

// ─────────────────────────────────────────────────────────────────────────────
// set_data — called by DashboardScreen after fetch_quotes returns
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::set_data(const QVector<Entry>& entries) {
    // Detect whether the actual symbol set or order changed. Price/change
    // refreshes flow through here every ~1–2 s; resetting offset_ on every
    // call made the marquee snap back to x=0 every refresh and look like it
    // was stuck on the same symbols. We only want to restart the scroll
    // when the LAYOUT actually changes (user edited the ticker symbol list
    // or the universe was rebuilt).
    bool layout_changed = (entries.size() != entries_.size());
    if (!layout_changed) {
        for (int i = 0; i < entries.size(); ++i) {
            if (entries.at(i).symbol != entries_.at(i).symbol) {
                layout_changed = true;
                break;
            }
        }
    }

    entries_ = entries;
    if (layout_changed)
        offset_ = 0;

    // Cache total width — paintEvent runs every 50ms, no per-frame allocation.
    QFont font(ui::fonts::DATA_FAMILY(), ui::fonts::font_px(-2));
    QFontMetrics fm(font);
    total_width_ = 0;
    for (const auto& e : entries_) {
        const int symbol_w = fm.horizontalAdvance(e.symbol);
        const int price_w  = fm.horizontalAdvance(QString::number(e.price, 'f', 2));
        const QString change_str =
            QString("%1%2%").arg(e.change >= 0 ? "+" : "").arg(e.change, 0, 'f', 2);
        const int change_w = fm.horizontalAdvance(change_str);
        total_width_ += symbol_w + kSegmentGap + price_w + kSegmentGap + change_w + kItemSpacing;
    }

    // Defensive wrap: if individual price-string widths shrank between
    // refreshes (e.g. a 3-digit price became 2-digit) total_width_ may now
    // be smaller than the current offset. Wrap so the marquee doesn't paint
    // into the void waiting for offset to "catch up".
    if (total_width_ > 0 && offset_ >= total_width_)
        offset_ = std::fmod(offset_, total_width_);

    if (total_width_ > 0 && isVisible() && !edit_bar_->isVisible()) {
        last_advance_ms_ = elapsed_.elapsed();   // reset anchor before (re)start
        pending_catchup_ = 0;                    // drop stale stutter bank
        scroll_timer_.start();
    }

    update();
}

// ─────────────────────────────────────────────────────────────────────────────
// Show / Hide
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (total_width_ > 0) {
        last_advance_ms_ = elapsed_.elapsed();
        pending_catchup_ = 0;
        scroll_timer_.start();
    }
}

void TickerBar::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    scroll_timer_.stop();
    hide_edit_bar();
}

void TickerBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (edit_bar_)
        edit_bar_->setGeometry(0, 0, width(), kEditBarHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
// Context menu — right-click to edit symbols
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    menu.setStyleSheet(
        QString("QMenu { background:%1; color:%2; border:1px solid %3; font-size:11px; }"
                "QMenu::item:selected { background:%4; color:%5; }")
            .arg(ui::colors::BG_RAISED(), ui::colors::TEXT_PRIMARY(),
                 ui::colors::BORDER_DIM(), ui::colors::AMBER(), ui::colors::BG_BASE()));

    auto* edit_action = menu.addAction("Edit Symbols...");
    connect(edit_action, &QAction::triggered, this, &TickerBar::show_edit_bar);

    menu.exec(event->globalPos());
}

// ─────────────────────────────────────────────────────────────────────────────
// Inline edit bar
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::show_edit_bar() {
    scroll_timer_.stop();
    edit_input_->setText(symbols_.join(", "));
    edit_bar_->setGeometry(0, 0, width(), kEditBarHeight);
    edit_bar_->raise();
    edit_bar_->show();
    edit_input_->setFocus();
    edit_input_->selectAll();
}

void TickerBar::hide_edit_bar() {
    edit_bar_->hide();
    if (total_width_ > 0 && isVisible()) {
        last_advance_ms_ = elapsed_.elapsed();
        pending_catchup_ = 0;
        scroll_timer_.start();
    }
}

void TickerBar::commit_edit() {
    QStringList updated;
    for (const QString& s : edit_input_->text().split(',')) {
        const QString t = s.trimmed().toUpper();
        if (!t.isEmpty())
            updated << t;
    }

    hide_edit_bar();

    if (updated.isEmpty() || updated == symbols_)
        return;

    symbols_ = updated;
    save_symbols();
    emit symbols_changed(symbols_);
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void TickerBar::paintEvent(QPaintEvent*) {
    if (entries_.isEmpty() || total_width_ <= 0)
        return;

    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);

    QFont font(ui::fonts::DATA_FAMILY(), ui::fonts::font_px(-2));
    p.setFont(font);
    QFontMetrics fm(font);

    const int text_y  = (height() + fm.ascent() - fm.descent()) / 2;
    double    x       = -offset_;
    const int passes  = (width() / total_width_) + 2;

    for (int pass = 0; pass < passes; ++pass) {
        for (const auto& e : entries_) {
            // Symbol — primary text
            p.setPen(QColor(ui::colors::WHITE()));
            p.drawText(QPointF(x, text_y), e.symbol);
            x += fm.horizontalAdvance(e.symbol) + kSegmentGap;

            // Price — muted
            const QString price_str = QString::number(e.price, 'f', 2);
            p.setPen(QColor(ui::colors::GRAY()));
            p.drawText(QPointF(x, text_y), price_str);
            x += fm.horizontalAdvance(price_str) + kSegmentGap;

            // Change — green / red
            const QString change_str =
                QString("%1%2%").arg(e.change >= 0 ? "+" : "").arg(e.change, 0, 'f', 2);
            p.setPen(QColor(ui::change_color(e.change)));
            p.drawText(QPointF(x, text_y), change_str);
            x += fm.horizontalAdvance(change_str) + kItemSpacing;
        }
    }
}

} // namespace fincept::screens
