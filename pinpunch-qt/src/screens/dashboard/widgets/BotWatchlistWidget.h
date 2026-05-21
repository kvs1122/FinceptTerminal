#pragma once
// BotWatchlistWidget — bot's intraday watchlist with live Alpaca prices.
// Subscribes to `bot:watchlist` (QVector<bot::BotWatchlistItem>) which
// BotService rebuilds every ~1s. Adds/drops are visible immediately
// because the symbol set comes from grok_status.json:watchlist (live).
//
// Display: SYM | SCORE | SIDE | PRICE | CHG% | CATALYST (tooltip)

#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/bot/BotTypes.h"

#include <QTableWidget>
class QLabel;
class QStackedWidget;

namespace fincept::screens::widgets {

class BotWatchlistWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit BotWatchlistWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private:
    void apply_styles();
    void hub_resubscribe();
    void hub_unsubscribe_all();
    void render(const QVector<services::bot::BotWatchlistItem>& rows);

    QStackedWidget* stack_ = nullptr;
    QTableWidget*   table_ = nullptr;
    QLabel*         empty_ = nullptr;
    bool hub_active_ = false;
};

QWidget* create_bot_watchlist_widget(const QJsonObject& cfg = {});

} // namespace fincept::screens::widgets
