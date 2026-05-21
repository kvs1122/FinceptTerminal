#pragma once

// ─── Universal market-quote Producer (Alpaca + yfinance) ─────────────────
//
// Owns the entire `market:quote:*` topic family. Smart per-symbol routing:
//
//   • US stocks + ETFs + China ADRs (AAPL, MSFT, SPY, TLT, GLD, BABA):
//       Polygon /v3/snapshot when POLYGON_API_KEY is set (sub-second-
//       precise prices, 1s polling, unlimited calls on Stocks Dev plan).
//       Falls back to Alpaca /v2/stocks/snapshots when no Polygon key is
//       present. We deliberately do NOT use Polygon's WebSocket — the
//       bot's predictive layer reserves that connection (see
//       grok-claude/predictive/polygon_ws.py).
//
//   • Crypto (BTC-USD, ETH-USD, …):
//       /v1beta3/crypto/us/snapshots (Alpaca crypto endpoint)
//
//   • Everything else — routed through yfinance via PythonRunner (one
//     subprocess per refresh tick, calling scripts/yfinance_data.py
//     batch_quotes):
//
//       Yahoo-style indices  ^GSPC, ^DJI, ^IXIC, ^N225, ^FTSE, ^GDAXI,
//                            ^NSEI, ^BSESN, ^HSI, 000001.SS, ^TNX, …
//                            (returns TRUE index value, not ETF proxy)
//       Commodity futures    GC=F, CL=F, NG=F, HG=F, …
//       Forex pairs          EURUSD=X, GBPUSD=X, USDJPY=X, USDINR=X, …
//                            (no inverse-pair math — yf returns the rate
//                             quoted in the symbol)
//       International stocks .NS / .BO / .HK / .L / .AX / .TO / .SS …
//       Dual-class shares    BRK-B, PBR-A (Yahoo's dash format Alpaca
//                            doesn't accept on /v2/stocks/snapshots)
//
// PythonRunner pays the ~2 s yfinance import on every spawn, but at the
// dashboard's 10 s policy that's an acceptable trade for resilience: it
// handles async interpreter detection and request queuing automatically,
// and surfaces script errors via the result callback so failures show up
// in the diag log instead of silently giving up like PythonWorker did.
//
// Hijacks `market:quote:*` so EVERY widget that uses QuoteTableWidget
// (Indices, ETFs, Commodities, Forex, Bonds, Crypto, Regional-US,
// Regional-China, India, plus the dashboard's GLOBAL INDICES widget)
// lights up with no per-widget code change.

#include "datahub/Producer.h"

#include <QObject>
#include <QString>
#include <QStringList>

namespace fincept::services::bot {

class BotIndicesService : public QObject, public fincept::datahub::Producer {
    Q_OBJECT
  public:
    static BotIndicesService& instance();

    QStringList topic_patterns() const override;
    void refresh(const QStringList& topics) override;
    int max_requests_per_sec() const override { return 10; }

    /// Map any source symbol → the Alpaca-tradeable symbol that fetches its
    /// price. Returns empty if no proxy exists or the symbol isn't supported.
    static QString resolve_alpaca_symbol(const QString& source_sym);

  private:
    BotIndicesService();
    ~BotIndicesService() override = default;

    void fetch_and_publish(const QStringList& topics);
};

} // namespace fincept::services::bot
