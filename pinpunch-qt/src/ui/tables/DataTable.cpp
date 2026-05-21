#include "ui/tables/DataTable.h"

#include "ui/theme/Theme.h"
#include "ui/theme/ThemeManager.h"

#include <QHeaderView>

namespace fincept::ui {

DataTable::DataTable(QWidget* parent) : QTableWidget(parent) {
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setShowGrid(false);
    verticalHeader()->setVisible(false);
    horizontalHeader()->setStretchLastSection(true);
    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Helper: build the table stylesheet from current theme tokens. We
    // rebuild this on every theme_changed so the table reads correctly in
    // both Obsidian (dark) and Parchment (light) — previously the QSS was
    // built once at construction and the colours stayed baked in even
    // after a theme swap, leaving GLOBAL INDICES / etc. still black on a
    // light dashboard. The header palette is also rebuilt so column
    // labels and the row-selected highlight match the active theme.
    auto rebuild_qss = [this]() {
        // Match the visual density of BotWatchlistWidget / BotPositionsWidget
        // (~10.5 px cells, ~9 px small-caps headers) so the dashboard reads
        // as one coherent surface instead of two different table styles.
        // Critical: do NOT put `color:` inside QTableWidget::item — that
        // CSS rule trumps QStandardItem::setForeground(), which would make
        // the green/red CHG / CHG% cells disappear (rendered as plain
        // text colour). Default text colour comes from the outer
        // QTableWidget rule; per-cell tints from set_cell_color() win.
        setStyleSheet(QString("QTableWidget { background:%1; alternate-background-color:%2;"
                              " gridline-color:%3; border:none; color:%4; font-size:10.5px; }"
                              "QTableWidget::item { padding:2px 6px; }"
                              "QTableWidget::item:selected { background:%5; }"
                              "QHeaderView { background:%6; }"
                              "QHeaderView::section { background:%6; color:%7;"
                              " border:none; border-bottom:1px solid %3; padding:2px 4px;"
                              " font-size:9px; font-weight:bold; letter-spacing:1px; }")
                          .arg(colors::PANEL())          // 1 — table body
                          .arg(colors::ROW_ALT())        // 2 — alt rows
                          .arg(colors::BORDER())         // 3 — gridline / header underline
                          .arg(colors::TEXT_PRIMARY())   // 4 — default cell text
                          .arg(colors::BG_HOVER())       // 5 — selected row
                          .arg(colors::BG_RAISED())      // 6 — header background
                          .arg(colors::TEXT_SECONDARY()));  // 7 — header label
    };
    rebuild_qss();

    // Re-apply on theme switch.
    connect(&ThemeManager::instance(), &ThemeManager::theme_changed, this,
            [this, rebuild_qss](const ThemeTokens&) {
                rebuild_qss();
                // Clear any forced-default foregrounds on data cells so the
                // new theme's text colour (from the QSS rule above) wins.
                // Cells that were explicitly tinted by set_cell_color()
                // (e.g. green/red P&L) hold an "explicit" sentinel in
                // Qt::UserRole+1; we leave those alone and trust the next
                // data refresh to re-tint with the new theme's accent.
                for (int r = 0; r < rowCount(); ++r) {
                    for (int c = 0; c < columnCount(); ++c) {
                        auto* it = item(r, c);
                        if (!it) continue;
                        if (it->data(Qt::UserRole + 1).toBool())
                            continue;  // explicit override — leave it
                        it->setData(Qt::ForegroundRole, QVariant{});
                    }
                }
                viewport()->update();
            });
}

void DataTable::set_headers(const QStringList& headers) {
    setColumnCount(headers.size());
    setHorizontalHeaderLabels(headers);
}

void DataTable::set_data(const QVector<QStringList>& rows) {
    setRowCount(0);
    for (const auto& row : rows) {
        add_row(row);
    }
}

void DataTable::add_row(const QStringList& row) {
    int r = rowCount();
    insertRow(r);
    for (int c = 0; c < row.size() && c < columnCount(); ++c) {
        auto* item = new QTableWidgetItem(row[c]);
        // No explicit foreground — let the QSS `color:` rule above provide
        // the theme-correct text colour. Forcing setForeground(WHITE) here
        // froze cells to whatever WHITE() resolved to at table-construction
        // time, which made them invisible on the Parchment (light) theme.
        setItem(r, c, item);
    }
    setRowHeight(r, 26);
}

void DataTable::clear_data() {
    setRowCount(0);
}

void DataTable::set_column_widths(const QVector<int>& widths) {
    for (int i = 0; i < widths.size() && i < columnCount(); ++i) {
        setColumnWidth(i, widths[i]);
    }
}

void DataTable::set_cell_color(int row, int col, const QString& color) {
    auto* it = item(row, col);
    if (it) {
        it->setForeground(QColor(color));
        // Mark as explicitly tinted so the theme_changed handler doesn't
        // strip the colour. The next data refresh will re-tint via the
        // theme-aware change_color() helper, so this only matters for
        // cells that aren't refreshed quickly.
        it->setData(Qt::UserRole + 1, true);
    }
}

} // namespace fincept::ui
