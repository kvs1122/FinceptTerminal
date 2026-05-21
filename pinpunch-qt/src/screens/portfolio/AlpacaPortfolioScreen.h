#pragma once

// AlpacaPortfolioScreen — operator-focused portfolio view backed by the
// live Alpaca paper account, NOT Fincept's hosted portfolio workspace.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────┐
//   │  [BotPnLWidget — hero equity / day P&L / halt status]    │
//   ├──────────────────────────────────────────────────────────┤
//   │  [BotPositionsWidget — open positions table]             │
//   │     SYM  SIDE  QTY  ENTRY  LIVE  P&L                     │
//   │     ...                                                   │
//   └──────────────────────────────────────────────────────────┘
//
// Both child widgets subscribe to the bot:* DataHub topics that
// BotService already publishes (account/positions populated from Alpaca
// REST every realtime tick), so this screen is pure composition — no new
// data fetching logic needed.
//
// Replaces Fincept's PortfolioScreen which talked to api.fincept.in for
// the user-account-attached portfolio workspace. We intentionally do NOT
// reuse that screen because it forces a Fincept-hosted account flow.

#include <QWidget>

namespace fincept::screens {

class AlpacaPortfolioScreen : public QWidget {
    Q_OBJECT
  public:
    explicit AlpacaPortfolioScreen(QWidget* parent = nullptr);
};

} // namespace fincept::screens
