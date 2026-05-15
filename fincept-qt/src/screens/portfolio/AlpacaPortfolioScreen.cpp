#include "screens/portfolio/AlpacaPortfolioScreen.h"

#include "screens/dashboard/widgets/BotPnLWidget.h"
#include "screens/dashboard/widgets/BotPositionsWidget.h"
#include "ui/theme/Theme.h"

#include <QLabel>
#include <QVBoxLayout>

namespace fincept::screens {

AlpacaPortfolioScreen::AlpacaPortfolioScreen(QWidget* parent) : QWidget(parent) {
    setStyleSheet(QString("background:%1;").arg(ui::colors::BG_BASE()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    // Header strip — small label so the operator immediately knows this is
    // the live broker view (vs the bot's internal P&L tracking).
    auto* header = new QLabel(this);
    header->setText("ALPACA PAPER ACCOUNT  ·  LIVE");
    header->setStyleSheet(QString(
        "color:%1; font-family:Consolas; font-size:10px; font-weight:bold;"
        " letter-spacing:1.2px; padding:2px 4px;")
        .arg(ui::colors::AMBER()));
    root->addWidget(header);

    // Account hero — equity / day P&L / halt indicator. Already wired to
    // bot:account + bot:risk topics that BotService publishes from the
    // Alpaca REST /v2/account endpoint every ~1s (realtime policy).
    auto* pnl = widgets::create_bot_pnl_widget();
    pnl->setMinimumHeight(140);
    pnl->setMaximumHeight(180);
    root->addWidget(pnl, 0);

    // Open positions table — sym / side / qty / entry / live / unreal P&L.
    // Subscribes to bot:positions which BotService publishes from
    // /v2/positions every realtime tick. Empty-state copy already says
    // "connected to bot · no open positions" so the user knows the wire
    // is live even when flat.
    auto* positions = widgets::create_bot_positions_widget();
    root->addWidget(positions, 1);
}

} // namespace fincept::screens
