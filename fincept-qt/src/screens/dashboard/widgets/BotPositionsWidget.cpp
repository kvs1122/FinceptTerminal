#include "screens/dashboard/widgets/BotPositionsWidget.h"

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

BotPositionsWidget::BotPositionsWidget(const QJsonObject& /*cfg*/, QWidget* parent)
    : BaseWidget("BOT POSITIONS", parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(8, 6, 8, 6);
    vl->setSpacing(4);

    stack_ = new QStackedWidget(this);
    vl->addWidget(stack_, 1);

    // Page 0: the table
    table_ = new QTableWidget(0, 6);
    table_->setHorizontalHeaderLabels({"SYM", "SIDE", "QTY", "ENTRY", "LIVE", "P&L"});
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setShowGrid(false);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    // Click on the SYM cell (column 0) opens the asset on Alpaca's trade
    // page (https://app.alpaca.markets/trade/<SYM>?asset_class=stocks) so
    // the operator sees the chart, the order book, AND their own working
    // / filled orders for that ticker — more useful than a generic chart
    // site since it ties back to actual broker activity. Visual cues
    // (cyan + underline + tooltip) are applied per-cell in render().
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

    // Page 1: empty-state message — shown when bot has no open positions.
    // We deliberately make this LOUD ("● connected") so operator knows the
    // widget is wired correctly and just has nothing to show, rather than
    // wondering whether the widget is broken.
    empty_ = new QLabel("● connected to bot · no open positions\n\n"
                        "The bot has 0 positions held right now.\n"
                        "When it enters a trade, the row will appear here automatically.");
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    stack_->addWidget(empty_);

    apply_styles();
    set_loading(true);
}

void BotPositionsWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_resubscribe();
}

void BotPositionsWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void BotPositionsWidget::hub_resubscribe() {
    auto& hub = datahub::DataHub::instance();
    hub.unsubscribe(this);
    const QString topic = QString::fromUtf8(services::bot::topics::kPositions);
    hub.subscribe(this, topic, [this](const QVariant& v) {
        if (v.canConvert<QVector<services::bot::BotPosition>>())
            render(v.value<QVector<services::bot::BotPosition>>());
    });
    // Pull cached value immediately (subscribe also delivers it but only if fresh).
    auto cached = hub.peek(topic);
    if (cached.isValid() && cached.canConvert<QVector<services::bot::BotPosition>>())
        render(cached.value<QVector<services::bot::BotPosition>>());
    hub_active_ = true;
}

void BotPositionsWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void BotPositionsWidget::render(const QVector<services::bot::BotPosition>& rows) {
    set_loading(false);
    // Toggle between empty-state message and the table.
    if (rows.isEmpty()) {
        stack_->setCurrentWidget(empty_);
        table_->setRowCount(0);
        return;
    }
    stack_->setCurrentWidget(table_);
    table_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const auto& p = rows[i];
        auto* sym  = new QTableWidgetItem(p.symbol);
        // Style the SYM cell as a clickable link so the user knows it goes
        // somewhere. Click handler is wired once in the constructor on
        // QTableWidget::cellClicked → opens Alpaca's web dashboard.
        sym->setForeground(QColor(ui::colors::CYAN()));
        QFont link_font = sym->font();
        link_font.setUnderline(true);
        sym->setFont(link_font);
        sym->setToolTip(QString("Open %1 trade page on Alpaca").arg(p.symbol));
        auto* side = new QTableWidgetItem(p.side.toUpper());
        auto* qty  = new QTableWidgetItem(QString::number(p.qty, 'f', 0));
        auto* ent  = new QTableWidgetItem(QString::number(p.avg_entry_price, 'f', 2));
        auto* lv   = new QTableWidgetItem(QString::number(p.current_price, 'f', 2));

        const QString pnl_txt = QString("%1$%2 (%3%4%)")
            .arg(p.unrealized_pl >= 0 ? "+" : "−")
            .arg(qAbs(p.unrealized_pl), 0, 'f', 2)
            .arg(p.unrealized_plpc >= 0 ? "+" : "")
            .arg(p.unrealized_plpc, 0, 'f', 2);
        auto* pnl  = new QTableWidgetItem(pnl_txt);
        const QColor c = p.unrealized_pl > 0 ? QColor(ui::colors::POSITIVE())
                       : p.unrealized_pl < 0 ? QColor(ui::colors::NEGATIVE())
                                              : QColor(ui::colors::TEXT_TERTIARY());
        pnl->setForeground(c);
        // Side badge color (long=green, short=pink)
        side->setForeground(p.side.toLower() == "short"
            ? QColor(ui::colors::NEGATIVE())
            : QColor(ui::colors::POSITIVE()));

        table_->setItem(i, 0, sym);
        table_->setItem(i, 1, side);
        table_->setItem(i, 2, qty);
        table_->setItem(i, 3, ent);
        table_->setItem(i, 4, lv);
        table_->setItem(i, 5, pnl);
    }
}

void BotPositionsWidget::on_theme_changed() {
    apply_styles();
}

void BotPositionsWidget::apply_styles() {
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

QWidget* create_bot_positions_widget(const QJsonObject& cfg) {
    return new BotPositionsWidget(cfg);
}

} // namespace fincept::screens::widgets
