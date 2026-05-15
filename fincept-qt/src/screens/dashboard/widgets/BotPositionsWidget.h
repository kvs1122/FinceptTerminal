#pragma once
// BotPositionsWidget — live Alpaca positions from the grok-claude bot.
// Subscribes to `bot:positions` (QVector<bot::BotPosition>) which the
// BotService Producer fills every ~2s from Alpaca REST + bot status JSON.
//
// Display: one row per open position with sym | side | qty | entry | live | P&L (color-coded)

#include "screens/dashboard/widgets/BaseWidget.h"
#include "services/bot/BotTypes.h"

#include <QTableWidget>
class QLabel;
class QStackedWidget;

namespace fincept::screens::widgets {

class BotPositionsWidget : public BaseWidget {
    Q_OBJECT
  public:
    explicit BotPositionsWidget(const QJsonObject& cfg = {}, QWidget* parent = nullptr);

  protected:
    void on_theme_changed() override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

  private:
    void apply_styles();
    void hub_resubscribe();
    void hub_unsubscribe_all();
    void render(const QVector<services::bot::BotPosition>& rows);

    QStackedWidget* stack_ = nullptr;
    QTableWidget* table_   = nullptr;
    QLabel*       empty_   = nullptr;
    bool hub_active_ = false;
};

QWidget* create_bot_positions_widget(const QJsonObject& cfg = {});

} // namespace fincept::screens::widgets
