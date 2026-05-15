#include "screens/dashboard/widgets/BotPnLWidget.h"

#include "datahub/DataHub.h"
#include "services/bot/BotConfig.h"
#include "ui/theme/Theme.h"

#include <QJsonObject>
#include <QLabel>
#include <QVBoxLayout>

namespace fincept::screens::widgets {

namespace {
QString fmt_money(double v, bool signed_) {
    const QString abs_str = QString::number(qAbs(v), 'f', 2);
    if (signed_)
        return (v >= 0 ? "+$" : "−$") + abs_str;
    return "$" + abs_str;
}
} // namespace

BotPnLWidget::BotPnLWidget(const QJsonObject& /*cfg*/, QWidget* parent)
    : BaseWidget("BOT TODAY P&L", parent) {
    auto* vl = content_layout();
    vl->setContentsMargins(12, 8, 12, 8);
    vl->setSpacing(6);
    vl->setAlignment(Qt::AlignCenter);

    hero_pnl_ = new QLabel("—", this);
    hero_pnl_->setAlignment(Qt::AlignCenter);
    hero_pnl_->setObjectName("botPnlHero");
    vl->addWidget(hero_pnl_);

    day_pct_ = new QLabel("—", this);
    day_pct_->setAlignment(Qt::AlignCenter);
    day_pct_->setObjectName("botPnlPct");
    vl->addWidget(day_pct_);

    equity_ = new QLabel("Equity —", this);
    equity_->setAlignment(Qt::AlignCenter);
    equity_->setObjectName("botPnlEquity");
    vl->addWidget(equity_);

    halt_ = new QLabel("", this);
    halt_->setAlignment(Qt::AlignCenter);
    halt_->setObjectName("botPnlHalt");
    halt_->setVisible(false);
    vl->addWidget(halt_);

    apply_styles();
    set_loading(true);
}

void BotPnLWidget::showEvent(QShowEvent* e) {
    BaseWidget::showEvent(e);
    if (!hub_active_)
        hub_resubscribe();
}

void BotPnLWidget::hideEvent(QHideEvent* e) {
    BaseWidget::hideEvent(e);
    if (hub_active_)
        hub_unsubscribe_all();
}

void BotPnLWidget::hub_resubscribe() {
    auto& hub = datahub::DataHub::instance();
    hub.unsubscribe(this);

    const QString acct_topic = QString::fromUtf8(services::bot::topics::kAccount);
    hub.subscribe(this, acct_topic, [this](const QVariant& v) {
        if (v.canConvert<services::bot::BotAccount>())
            render(v.value<services::bot::BotAccount>());
    });
    auto cached_a = hub.peek(acct_topic);
    if (cached_a.isValid() && cached_a.canConvert<services::bot::BotAccount>())
        render(cached_a.value<services::bot::BotAccount>());

    // Also subscribe to risk state so the halt-banner shows live without
    // waiting for an account refresh tick.
    const QString risk_topic = QString::fromUtf8(services::bot::topics::kRisk);
    hub.subscribe(this, risk_topic, [this](const QVariant& v) {
        if (v.canConvert<services::bot::BotRiskState>())
            render_risk(v.value<services::bot::BotRiskState>());
    });
    auto cached_r = hub.peek(risk_topic);
    if (cached_r.isValid() && cached_r.canConvert<services::bot::BotRiskState>())
        render_risk(cached_r.value<services::bot::BotRiskState>());

    hub_active_ = true;
}

void BotPnLWidget::hub_unsubscribe_all() {
    datahub::DataHub::instance().unsubscribe(this);
    hub_active_ = false;
}

void BotPnLWidget::render(const services::bot::BotAccount& a) {
    set_loading(false);
    hero_pnl_->setText(fmt_money(a.day_pnl, /*signed=*/true));
    const QColor c = a.day_pnl > 0 ? QColor(ui::colors::POSITIVE())
                   : a.day_pnl < 0 ? QColor(ui::colors::NEGATIVE())
                                    : QColor(ui::colors::TEXT_TERTIARY());
    QPalette pp = hero_pnl_->palette();
    pp.setColor(QPalette::WindowText, c);
    hero_pnl_->setPalette(pp);

    day_pct_->setText(QString("%1%2%")
                          .arg(a.day_pct >= 0 ? "+" : "")
                          .arg(a.day_pct, 0, 'f', 2));
    QPalette pct_pal = day_pct_->palette();
    pct_pal.setColor(QPalette::WindowText, c);
    day_pct_->setPalette(pct_pal);

    equity_->setText("Equity " + fmt_money(a.equity, /*signed=*/false));
}

void BotPnLWidget::render_risk(const services::bot::BotRiskState& r) {
    if (r.trading_halted || !r.halted_by.isEmpty()) {
        halt_->setText("⛔ HALTED · " + (r.halted_by.isEmpty()
                                       ? r.drawdown_tier.toUpper()
                                       : r.halted_by));
        halt_->setVisible(true);
        QPalette p = halt_->palette();
        p.setColor(QPalette::WindowText, QColor(ui::colors::NEGATIVE()));
        halt_->setPalette(p);
    } else if (r.drawdown_tier == "red" || r.drawdown_tier == "black") {
        halt_->setText("⚠ DRAWDOWN " + r.drawdown_tier.toUpper());
        halt_->setVisible(true);
        QPalette p = halt_->palette();
        p.setColor(QPalette::WindowText, QColor(ui::colors::NEGATIVE()));
        halt_->setPalette(p);
    } else if (r.drawdown_tier == "yellow") {
        halt_->setText("⚠ DRAWDOWN YELLOW");
        halt_->setVisible(true);
    } else {
        halt_->setVisible(false);
    }
}

void BotPnLWidget::on_theme_changed() {
    apply_styles();
}

void BotPnLWidget::apply_styles() {
    hero_pnl_->setStyleSheet("font-size:32px; font-weight:800;");
    day_pct_->setStyleSheet("font-size:13px; font-weight:600;");
    equity_->setStyleSheet(QString("font-size:11px; color:%1;").arg(ui::colors::TEXT_TERTIARY()));
    halt_->setStyleSheet("font-size:11px; font-weight:700; letter-spacing:1px;");
}

QWidget* create_bot_pnl_widget(const QJsonObject& cfg) {
    return new BotPnLWidget(cfg);
}

} // namespace fincept::screens::widgets
