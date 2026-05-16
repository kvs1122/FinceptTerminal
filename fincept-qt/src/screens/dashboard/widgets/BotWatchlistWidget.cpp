#include "screens/dashboard/widgets/BotWatchlistWidget.h"

#include "datahub/DataHub.h"
#include "services/bot/BotConfig.h"
#include "ui/theme/Theme.h"

#include <QDesktopServices>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QStackedWidget>
#include <QUrl>

namespace fincept::screens::widgets {

BotWatchlistWidget::BotWatchlistWidget(const QJsonObject& /*cfg*/, QWidget* parent)
    : BaseWidget("BOT WATCHLIST", parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(8, 6, 8, 6);
    vl->setSpacing(4);

    stack_ = new QStackedWidget(this);
    vl->addWidget(stack_, 1);

    table_ = new QTableWidget(0, 6);
    table_->setHorizontalHeaderLabels({"SYM", "AI", "SIDE", "PRICE", "CHG%", "CATALYST"});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    // Click on the SYM cell (column 0) opens the asset on Alpaca's trade
    // page (https://app.alpaca.markets/trade/<SYM>?asset_class=stocks)
    // so the operator can see the chart and any working/filled orders
    // for that ticker in one view. Same UX as BotPositionsWidget.
    QObject::connect(table_, &QTableWidget::cellClicked, this,
        [tbl = table_](int row, int col) {
            if (col != 0) return;
            auto* it = tbl->item(row, 0);
            if (!it) return;
            const QString sym = it->text().trimmed();
            if (sym.isEmpty()) return;
            QDesktopServices::openUrl(QUrl(
                QStringLiteral("https://app.alpaca.markets/trade/") + sym +
                QStringLiteral("?asset_class=stocks")));
        });
    stack_->addWidget(table_);

    empty_ = new QLabel("● connected to bot · no watchlist symbols\n\n"
                        "When the bot adds tickers to its intraday watchlist,\n"
                        "they will appear here automatically.");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    stack_->addWidget(empty_);

    apply_styles();
    set_loading(true);
}

void BotWatchlistWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_) hub_resubscribe();
}

void BotWatchlistWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_) hub_unsubscribe_all();
}

void BotWatchlistWidget::hub_resubscribe() {
    auto& hub = datahub::DataHub::instance();
    hub.unsubscribe(this);
    const QString topic = QString::fromUtf8(services::bot::topics::kWatchlist);
    hub.subscribe(this, topic, [this](const QVariant& v) {
        if (v.canConvert<QVector<services::bot::BotWatchlistItem>>())
            render(v.value<QVector<services::bot::BotWatchlistItem>>());
    });
    auto cached = hub.peek(topic);
    if (cached.isValid() && cached.canConvert<QVector<services::bot::BotWatchlistItem>>())
        render(cached.value<QVector<services::bot::BotWatchlistItem>>());
    hub_active_ = true;
}

void BotWatchlistWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void BotWatchlistWidget::render(const QVector<services::bot::BotWatchlistItem>& rows) {
    set_loading(false);
    // Surface the live count in the title so the operator can sanity-check
    // against the bot's Telegram pings ("Watchlist refreshed: N tickers").
    // The bot's state.watchlist trims through the day as positions
    // open/close, gap-fades complete, scores decay, etc., so the count here
    // will be smaller than the premarket peak Telegram reported.
    set_title(rows.isEmpty()
        ? QStringLiteral("BOT WATCHLIST")
        : QString("BOT WATCHLIST  ·  %1 SYMBOLS").arg(rows.size()));
    if (rows.isEmpty()) {
        stack_->setCurrentWidget(empty_);
        table_->setRowCount(0);
        return;
    }
    stack_->setCurrentWidget(table_);
    table_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& w = rows[i];
        auto* sym  = new QTableWidgetItem(w.symbol);
        // Cyan + underline + tooltip → SYM cell looks like (and is) a link
        // to the Alpaca asset page. Click handler is wired once in the ctor.
        sym->setForeground(QColor(ui::colors::CYAN()));
        QFont link_font = sym->font();
        link_font.setUnderline(true);
        sym->setFont(link_font);
        sym->setToolTip(QString("Open %1 trade page on Alpaca").arg(w.symbol));
        auto* ai   = new QTableWidgetItem(w.score > 0 ? QString::number(w.score, 'f', 0) : "—");
        auto* side = new QTableWidgetItem(w.direction.toUpper());
        auto* px   = new QTableWidgetItem(w.price > 0
            ? QString("$%1").arg(w.price, 0, 'f', 2)
            : "—");
        auto* chg  = new QTableWidgetItem(w.price > 0 && w.prev_close > 0
            ? QString("%1%2%").arg(w.change_pct >= 0 ? "+" : "")
                               .arg(w.change_pct, 0, 'f', 2)
            : "—");
        auto* cat  = new QTableWidgetItem(w.catalyst.isEmpty() ? "—" : w.catalyst);

        // Color side: green for long, pink for short
        side->setForeground(w.direction.toLower() == "short"
            ? QColor(ui::colors::NEGATIVE())
            : QColor(ui::colors::POSITIVE()));
        // Color CHG% by sign
        const QColor cc = w.change_pct > 0 ? QColor(ui::colors::POSITIVE())
                        : w.change_pct < 0 ? QColor(ui::colors::NEGATIVE())
                                            : QColor(ui::colors::TEXT_TERTIARY());
        chg->setForeground(cc);
        // Score: gold for >=8, normal otherwise
        if (w.score >= 8) ai->setForeground(QColor("#ffcc44"));
        // Tooltip on the catalyst column for full text
        if (!w.catalyst.isEmpty()) cat->setToolTip(w.catalyst);

        table_->setItem(i, 0, sym);
        table_->setItem(i, 1, ai);
        table_->setItem(i, 2, side);
        table_->setItem(i, 3, px);
        table_->setItem(i, 4, chg);
        table_->setItem(i, 5, cat);
    }
}

void BotWatchlistWidget::on_theme_changed() {
    apply_styles();
}

void BotWatchlistWidget::apply_styles() {
    table_->setStyleSheet(QString(
        "QTableWidget{background:transparent;color:%1;gridline-color:%2;font-size:10.5px;border:none;}"
        "QHeaderView::section{background:%3;color:%4;border:none;border-bottom:1px solid %2;"
        "padding:2px 4px;font-size:9px;font-weight:bold;letter-spacing:1px;}"
        "QTableWidget::item{padding:2px 6px;}")
        .arg(ui::colors::TEXT_PRIMARY(), ui::colors::BORDER_DIM(), ui::colors::BG_RAISED(),
             ui::colors::TEXT_TERTIARY()));
    if (empty_) {
        empty_->setStyleSheet(QString("color:%1; font-size:11px; line-height:1.4;")
                                  .arg(ui::colors::TEXT_TERTIARY()));
    }
}

QWidget* create_bot_watchlist_widget(const QJsonObject& cfg) {
    return new BotWatchlistWidget(cfg);
}

} // namespace fincept::screens::widgets
