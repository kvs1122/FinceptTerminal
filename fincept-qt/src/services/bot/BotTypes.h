#pragma once

// ─── Pinpunch ↔ grok-claude-bot integration: shared data types ─────────────
//
// These structs are the contract between BotService (the Producer that pulls
// data from the bot's disk files + Alpaca REST) and the widgets that
// subscribe to bot:* DataHub topics.
//
// Each type registered with Q_DECLARE_METATYPE so DataHub's typed
// subscribe<T>() can unwrap QVariant cleanly.
//
// Architecture rule: Pinpunch NEVER touches the Alpaca news WebSocket nor
// the Alpaca trade-stream WebSocket (the bot owns those exclusively, one
// WS per API key). Pinpunch only reads:
//   • disk files written by the bot (grok_status.json, trades.csv,
//     logs/alpaca_news_raw/YYYY-MM-DD.jsonl)
//   • Alpaca REST endpoints (account, positions, orders) — separate
//     channel, no contention with the bot's WS subscriptions

#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace fincept::services::bot {

// ─── Account snapshot (bot:account, refresh ~5s) ──────────────────────────
struct BotAccount {
    double  equity = 0.0;          // Alpaca account.equity
    double  last_equity = 0.0;     // prior session close
    double  day_pnl = 0.0;         // equity - last_equity (live MTM)
    double  day_pct = 0.0;         // day_pnl / last_equity * 100
    double  cash = 0.0;
    double  buying_power = 0.0;
    bool    trading_blocked = false;
    bool    is_connected = false;  // false = REST call failed; widgets show error state
    qint64  ts = 0;                // unix seconds of snapshot
};

// ─── Single open position (bot:positions, refresh ~2s) ────────────────────
struct BotPosition {
    QString symbol;
    QString side;                  // "long" | "short"
    double  qty = 0.0;
    double  avg_entry_price = 0.0;
    double  current_price = 0.0;
    double  market_value = 0.0;
    double  cost_basis = 0.0;
    double  unrealized_pl = 0.0;   // broker-truth signed dollars
    double  unrealized_plpc = 0.0; // percent (already × 100 server-side)
};

// ─── A single completed (or open) trade segment today ─────────────────────
// Zero-crossing semantics: a trade is a contiguous flat→flat (or flat→
// still-open) position segment. Multiple buy fills building a position
// AND multiple partial-exit sells all belong to the SAME segment.
struct BotTrade {
    QString symbol;
    QString side;                  // "long" | "short"
    double  qty = 0.0;
    double  entry = 0.0;           // qty-weighted avg entry price
    double  exit  = 0.0;           // qty-weighted avg exit price (0 if open)
    double  pnl   = 0.0;           // realized P&L (0 if still open)
    bool    is_open = false;       // true = still held, false = closed today
    QString ts;                    // ISO-format timestamp string
};

// ─── Today's trades aggregate (bot:trades_today, refresh ~5s) ─────────────
struct BotTradesToday {
    int     trades_total = 0;       // closed + open
    int     trades_closed = 0;
    int     trades_open = 0;
    int     wins = 0;
    int     losses = 0;
    double  realized_pnl = 0.0;    // closed only
    double  win_rate_pct = 0.0;    // wins / trades_closed × 100, 0 if denom=0
    QStringList open_symbols;
    QVector<BotTrade> trades;      // chronological list (newest last)
};

// ─── Watchlist item (bot:watchlist, refresh ~1s) ──────────────────────────
// Each row joins the bot's intraday watchlist metadata (from
// grok_status.json:watchlist — score, direction, catalyst) with the
// LIVE price snapshot from Alpaca (price, prev_close, change %).
//
// The bot's `state.watchlist` field updates every ~1s with intraday
// adds/drops, so this dict reflects current "what bot is paying
// attention to" + "what the broker says it's worth right now".
struct BotWatchlistItem {
    QString symbol;
    // From bot's state.watchlist (intraday-tracked)
    double  score = 0.0;           // bot's AI score (0-10)
    QString direction;             // "long" | "short"
    QString catalyst;              // bot's stated reason for watching
    QString strategy;              // bot's selected strategy tag
    bool    subscribed = false;    // true = bot has live tick stream open
    // From Alpaca /v2/stocks/snapshots (live broker truth)
    double  price = 0.0;
    double  prev_close = 0.0;
    double  change_pct = 0.0;      // (price - prev_close) / prev_close × 100
    qint64  ts = 0;
};

// ─── News item (bot:news, refresh ~5s tail of jsonl) ──────────────────────
struct BotNewsItem {
    QString id;
    QString headline;
    QString body;                  // first ~500 chars
    QStringList tickers;
    QString source;                // "benzinga" | "alpaca_news"
    qint64  published_ms = 0;      // unix ms — Benzinga publication time
    qint64  received_ts = 0;       // unix seconds — when bot saw it
    // Optional polarity from news_scorer (if the bot scored this headline)
    bool    has_polarity = false;
    double  polarity = 0.0;        // -1.0 .. +1.0
    double  magnitude = 0.0;       // 0.0 .. 1.0
    QString reasoning;             // short string from LLM
};

// ─── Per-gate block tracking (bot:risk gate_blocks field) ─────────────────
struct BotGateBlock {
    QString gate;                  // "regime" | "tech_long" | "tech_short" |
                                   // "vwap_strict" | "pred_contra" | "scanned_today"
    int     window = 0;            // skips in last 60s window
    int     distinct = 0;          // distinct tickers blocked today
    qint64  ts = 0;                // last update
};

// ─── Risk state aggregate (bot:risk, refresh ~2s) ────────────────────────
struct BotRiskState {
    QString drawdown_tier;         // "green" | "yellow" | "red" | "black"
    bool    trading_halted = false;
    QString halted_by;             // "" if not halted
    int     soft_blocked_count = 0;
    QStringList soft_blocked_top;  // top N rejected tickers today
    QVector<BotGateBlock> gate_blocks;
    qint64  ts = 0;
};

// ─── Market state aggregate (bot:market, refresh ~5s) ────────────────────
struct BotMarketState {
    QString regime;                // "bullish" | "bearish" | "neutral" | "unknown"
    QString vol_env;               // "high_vol" | "normal" | "low_vol"
    double  spy_price = 0.0;
    double  spy_pct = 0.0;
    QString why_text;              // Grok narrative
    qint64  why_ts = 0;            // when narrative was last refreshed
    qint64  ts = 0;
};

} // namespace fincept::services::bot

// ── QVariant integration ──────────────────────────────────────────────────
// Required by DataHub's typed subscribe<T>() — Q_DECLARE_METATYPE makes the
// type usable inside QVariant. The matching qRegisterMetaType<T>() calls
// fire from BotService::register_metatypes() at startup.

Q_DECLARE_METATYPE(fincept::services::bot::BotAccount)
Q_DECLARE_METATYPE(fincept::services::bot::BotPosition)
Q_DECLARE_METATYPE(QVector<fincept::services::bot::BotPosition>)
Q_DECLARE_METATYPE(fincept::services::bot::BotTrade)
Q_DECLARE_METATYPE(fincept::services::bot::BotTradesToday)
Q_DECLARE_METATYPE(fincept::services::bot::BotWatchlistItem)
Q_DECLARE_METATYPE(QVector<fincept::services::bot::BotWatchlistItem>)
Q_DECLARE_METATYPE(fincept::services::bot::BotNewsItem)
Q_DECLARE_METATYPE(QVector<fincept::services::bot::BotNewsItem>)
Q_DECLARE_METATYPE(fincept::services::bot::BotGateBlock)
Q_DECLARE_METATYPE(QVector<fincept::services::bot::BotGateBlock>)
Q_DECLARE_METATYPE(fincept::services::bot::BotRiskState)
Q_DECLARE_METATYPE(fincept::services::bot::BotMarketState)
