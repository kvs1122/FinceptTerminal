#pragma once
#include "screens/dashboard/DashboardStatusBar.h"
#include "screens/dashboard/DashboardToolBar.h"
#include "screens/dashboard/MarketPulsePanel.h"
#include "screens/dashboard/TickerBar.h"
#include "screens/dashboard/canvas/DashboardCanvas.h"
#include "services/markets/MarketDataService.h"

namespace fincept::ui {
class NotifToast;
} // namespace fincept::ui

#include <QHash>
#include <QHideEvent>
#include <QScrollArea>
#include <QShowEvent>
#include <QSplitter>
#include <QTimer>
#include <QWidget>

namespace fincept::screens {

/// Main dashboard screen — toolbar, ticker, draggable widget grid, market pulse, status bar.
///
/// The ticker driver subscribes to `market:quote:<sym>` on the DataHub for
/// every user-configured ticker symbol. A cached per-symbol QuoteData is
/// pushed to the TickerBar on each delivery via `rebuild_ticker_from_cache()`.
/// `symbols_changed` wipes old subscriptions and re-subscribes to the new
/// set. Cadence is owned by the hub scheduler, not a local refresh timer.
class DashboardScreen : public QWidget {
    Q_OBJECT
  public:
    explicit DashboardScreen(QWidget* parent = nullptr);

  protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;

  private:
    void refresh_theme();
    void build_default_layout();
    void save_layout();
    void restore_layout();
    void refresh_ticker();
    void on_refresh_clicked();

    void hub_resubscribe_ticker();
    void hub_unsubscribe_ticker();
    void rebuild_ticker_from_cache();

    DashboardToolBar* toolbar_ = nullptr;
    TickerBar* ticker_bar_ = nullptr;
    QScrollArea* scroll_area_ = nullptr;
    DashboardCanvas* canvas_ = nullptr;
    QSplitter* content_split_ = nullptr;
    MarketPulsePanel* market_pulse_ = nullptr;
    DashboardStatusBar* status_bar_ = nullptr;
    fincept::ui::NotifToast* notif_toast_ = nullptr;
    QTimer* save_timer_ = nullptr;
    bool pulse_visible_ = true;
    bool layout_restored_ = false;
    bool split_sized_ = false;

    QHash<QString, services::QuoteData> ticker_cache_;
    QStringList ticker_subscribed_;
    bool hub_active_ = false;

    // Top-movers rotation — every kTopMoversRefreshMs the ticker bar's
    // symbol list is replaced with Polygon's "today's biggest US stock
    // movers" (gainers + losers, ordered by abs(change_pct) desc). Falls
    // back silently to a static seed list if Polygon isn't configured.
    QTimer* top_movers_timer_ = nullptr;
    QVector<QPair<QString,double>> gainers_buf_;   // accumulator across the
    QVector<QPair<QString,double>> losers_buf_;    // 2 parallel Polygon calls
    int     top_movers_responses_pending_ = 0;
    void rotate_top_movers();
    void apply_top_movers_to_ticker();
    static constexpr int kTopMoversRefreshMs = 5 * 60 * 1000;   // 5 min
    static constexpr int kTopMoversCount     = 30;              // 15 gainers + 15 losers
};

} // namespace fincept::screens
