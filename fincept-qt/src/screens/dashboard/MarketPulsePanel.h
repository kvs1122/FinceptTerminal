#pragma once
#include "services/bot/BotTypes.h"
#include "services/markets/MarketDataService.h"

#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

class QFrame;

namespace fincept::screens::widgets {
class LoadingOverlay;
}

namespace fincept::screens {

/// Right-side market pulse panel — Fear/Greed, breadth, top movers, global
/// snapshot, market hours.
///
/// Subscribes to `market:quote:<sym>` on the DataHub for the union of
/// breadth + mover + snapshot symbol sets. Each delivery updates the
/// matching row cache and triggers the relevant `rebuild_*_from_cache()`
/// to re-render that section. The hub scheduler owns data-refresh cadence;
/// `hours_timer_` is retained only because it drives wall-clock status
/// labels.
class MarketPulsePanel : public QWidget {
    Q_OBJECT
  public:
    explicit MarketPulsePanel(QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  public slots:
    /// Public so the Dashboard REFRESH button can drive it directly. The
    /// internal timer also wires to this slot.
    void refresh_data();

  private slots:
    void refresh_market_hours();

  private:
    // ── Build helpers ──
    QWidget* build_header();
    QWidget* build_section_header(const QString& title, const QString& icon_char, const QString& color);
    QWidget* build_fear_greed_section();
    QWidget* build_breadth_section();
    QWidget* build_gainers_section();
    QWidget* build_losers_section();
    QWidget* build_global_snapshot_section();
    QWidget* build_market_hours_section();
    QWidget* build_market_context_section();
    QWidget* build_ticker_search_section();
    QWidget* build_mover_row(const QString& symbol, double change, const QString& volume);
    QWidget* build_stat_row(const QString& label, const QString& value, const QString& change, const QString& color);
    QWidget* build_breadth_bar(const QString& label, int advancing, int declining);

    /// Resolve a free-text query to a Polygon ticker, then chain the
    /// snapshot + reference + 52w-aggregate fetches and render results.
    void on_search_submit();
    void render_search_status(const QString& msg, bool error);
    void render_search_result();

    /// Re-apply all token-based styles so a theme switch updates every child widget.
    void refresh_theme();

    static QString market_status(const QString& region);

    // ── Header ──
    QWidget* header_bar_ = nullptr;
    QLabel* header_icon_ = nullptr;
    QLabel* header_title_ = nullptr;
    QLabel* header_live_dot_ = nullptr;

    // ── Scroll area ──
    QScrollArea* scroll_area_ = nullptr;

    // ── Section headers (5 total) ──
    struct SectionHeader {
        QWidget* container = nullptr;
        QLabel* icon = nullptr;
        QLabel* title = nullptr;
    };
    SectionHeader sh_breadth_;
    SectionHeader sh_gainers_;
    SectionHeader sh_losers_;
    SectionHeader sh_snapshot_;
    SectionHeader sh_hours_;
    SectionHeader sh_context_;       // WHY MARKETS ARE MOVING
    SectionHeader sh_lookup_;        // TICKER LOOKUP

    // ── Fear & Greed ──
    QLabel* fg_header_label_ = nullptr;
    QLabel* fg_gauge_icon_ = nullptr;
    QLabel* fg_score_val_ = nullptr;
    QLabel* fg_score_max_ = nullptr;
    QLabel* fg_sentiment_ = nullptr;
    QFrame* fg_gradient_bar_ = nullptr;

    // ── Market Breadth ──
    // per-exchange: {name_label, adv_label, slash_label, dec_label, green_bar, red_bar}
    struct BreadthRow {
        QLabel* name = nullptr;
        QLabel* adv = nullptr;
        QLabel* slash = nullptr;
        QLabel* dec = nullptr;
        QWidget* green = nullptr;
        QWidget* red = nullptr;
    };
    BreadthRow nyse_row_;
    BreadthRow nasdaq_row_;
    BreadthRow sp500_row_;

    // ── Top Movers ──
    QVBoxLayout* gainers_layout_ = nullptr;
    QVBoxLayout* losers_layout_ = nullptr;

    // ── Global Snapshot ──
    struct StatRow {
        QWidget* container = nullptr;
        QLabel* name_lbl = nullptr;
        QLabel* val = nullptr;
        QLabel* chg = nullptr;
    };
    StatRow vix_row_;
    StatRow us10y_row_;
    StatRow dxy_row_;
    StatRow gold_row_;
    StatRow oil_row_;
    StatRow btc_row_;

    // ── Market Hours ──
    struct HoursRow {
        QWidget* container = nullptr;
        QLabel* name_lbl = nullptr;
        QLabel* dot = nullptr;
        QLabel* status = nullptr;
        QString region;
    };
    QVector<HoursRow> hours_rows_;

    void hub_subscribe_all();
    void hub_unsubscribe_all();
    void rebuild_breadth_from_cache();
    void rebuild_movers_from_cache();
    void rebuild_snapshot_from_cache();

    QHash<QString, services::QuoteData> breadth_cache_;
    QHash<QString, services::QuoteData> movers_cache_;
    QHash<QString, services::QuoteData> snapshot_cache_;
    bool hub_active_ = false;
    QTimer* hours_timer_ = nullptr;

    // ── Ticker Search (Polygon + Finnhub) ──
    // Generation counter discards stale callbacks: each on_search_submit
    // bumps it; callbacks compare against current_search_gen_ and bail if
    // the user typed a new query while their previous responses were in
    // flight. Each search fans out 4 parallel HTTP calls (snapshot +
    // ticker details + 52w aggregates + finnhub earnings); fields render
    // as each completes.
    QLineEdit*   search_input_   = nullptr;
    QPushButton* search_btn_     = nullptr;
    QPushButton* search_refresh_ = nullptr;     // re-fetches the currently-displayed ticker
    QLabel*      search_status_  = nullptr;     // "Searching…", error msg, etc.
    QWidget*     search_result_box_ = nullptr; // shown when we have data
    QLabel*      sr_symbol_     = nullptr;     // "TSLA — Tesla, Inc."
    QLabel*      sr_price_      = nullptr;     // "$259.32 +1.42%"
    QLabel*      sr_day_range_  = nullptr;     // "DAY  H 261.10  /  L 257.40"
    QLabel*      sr_52w_range_  = nullptr;     // "52W  H 488.54  /  L 138.80"
    QLabel*      sr_mcap_       = nullptr;     // "MCAP  $834.5B"
    QLabel*      sr_eps_actual_ = nullptr;     // "EPS Actual  $1.21"
    QLabel*      sr_eps_est_    = nullptr;     // "EPS Estimate  $1.16"
    QLabel*      sr_eps_date_   = nullptr;     // "Period  2025-09-30"
    QLabel*      sr_eps_surp_   = nullptr;     // "Surprise  +4.31% (BEAT)"

    struct SearchState {
        QString ticker;
        QString name;
        double current_price = 0, change_pct = 0;
        double day_high = 0, day_low = 0;
        double w52_high = 0, w52_low = 0;
        double market_cap = 0;
        double eps_actual = 0, eps_estimate = 0, eps_surprise_pct = 0;
        QString eps_period;
        bool has_snapshot = false;
        bool has_details = false;
        bool has_w52 = false;
        bool has_eps = false;
    };
    SearchState  current_search_;
    qint64       current_search_gen_ = 0;

    void start_polygon_chain(qint64 gen);
    void start_fuzzy_search(qint64 gen, const QString& query);
    void fetch_finnhub_earnings(qint64 gen, const QString& ticker);
    void refresh_current_ticker();

    // ── Market Context (LLM-generated narrative) ──
    QLabel*      mctx_title_     = nullptr;
    QLabel*      mctx_text_      = nullptr;
    QLabel*      mctx_meta_      = nullptr;     // "Updated 14:00 · next 15:00 · Gemini"
    QPushButton* mctx_refresh_   = nullptr;
    void render_market_context(const services::bot::MarketContextItem& item);

    // Loading overlay shown over the scroll area while the union of the
    // breadth/movers/snapshot caches fills up. The denominator is the
    // unique-symbol union size (~70), updated every time a cache changes.
    int total_expected_symbols_ = 0;
    widgets::LoadingOverlay* loading_overlay_ = nullptr;
    void update_loading_progress();
};

} // namespace fincept::screens
