#pragma once
// BotPnLWidget — bot's day P&L + equity from grok-claude.
// Subscribes to `bot:account` (BotAccount) which the BotService Producer
// fills every ~5s from grok_status.json + Alpaca REST.
//
// Display: hero P&L + equity sub-line + day-pct + halt indicator

#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/bot/BotTypes.h"

class QLabel;

namespace fincept::screens::widgets {

class BotPnLWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit BotPnLWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private:
    void apply_styles();
    void hub_resubscribe();
    void hub_unsubscribe_all();
    void render(const services::bot::BotAccount& a);
    void render_risk(const services::bot::BotRiskState& r);

    QLabel* hero_pnl_  = nullptr;
    QLabel* equity_    = nullptr;
    QLabel* day_pct_   = nullptr;
    QLabel* halt_      = nullptr;
    bool    hub_active_ = false;
};

QWidget* create_bot_pnl_widget(const QJsonObject& cfg = {});

} // namespace fincept::screens::widgets
