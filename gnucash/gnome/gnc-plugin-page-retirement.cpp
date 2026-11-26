/********************************************************************
 * gnc-plugin-page-retirement.cpp -- Standalone Owl-inspired        *
 * retirement planner page for GnuCash.                             *
 *                                                                  *
 * Inspired by mdlacasse/Owl (https://github.com/mdlacasse/Owl,     *
 * GPL-3.0). Original C++17/GTK3 implementation for GnuCash; see    *
 * ATTRIBUTIONS.md and LICENSE for upstream license accounting.     *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 51 Franklin Street, Fifth Floor    Fax:    +1-617-542-2652       *
 * Boston, MA  02110-1301,  USA       gnu@gnu.org                   *
 *******************************************************************/

#include <config.h>

#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <cairo.h>
#include <libsoup/soup.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Account.h"
#include "gnc-gobject-utils.h"
#include "gnc-icons.h"
#include "gnc-plugin-page-retirement.h"
#include "gnc-plugin-retirement.h"
#include "gnc-retirement-engine.hpp"
#include "gnc-session.h"
#include "gnc-state.h"
#include "gnc-ui-util.h"

using namespace gnc::retirement;

namespace {

struct MappedAccount
{
    std::string full_name;
    double balance = 0.0;
    TaxBucket bucket = TaxBucket::Taxable;
    bool included = true;
};

enum class ChartMode
{
    PortfolioBuckets = 0,
    IncomeAndWithdrawals = 1,
    TaxesRmdAndRoth = 2,
    MonteCarloFan = 3,
    TaxAndHealthcare = 4
};

enum
{
    COL_YEAR = 0,
    COL_AGE,
    COL_TOTAL,
    COL_TAXABLE,
    COL_TAX_DEF,
    COL_ROTH,
    COL_HSA,
    COL_FIXED_INC,
    COL_RMD,
    COL_ROTH_CONV,
    COL_TOTAL_TAX,
    COL_NET_SPEND,
    COL_FLOOR_GAP,
    COL_RMD_REINV,
    COL_BRACKET_IRMAA,
    NUM_SCHED_COLS
};

enum
{
    ACCT_COL_INCLUDE = 0,
    ACCT_COL_NAME,
    ACCT_COL_BAL,
    ACCT_COL_BUCKET_STR,
    ACCT_COL_BUCKET_ID,
    ACCT_COL_ORIG_IDX,
    NUM_ACCT_COLS
};

enum
{
    CMP_COL_METRIC = 0,
    CMP_COL_BASELINE,
    CMP_COL_OWL,
    CMP_COL_DIFF,
    NUM_CMP_COLS
};

std::string fmt_currency(double v)
{
    char buf[64];
    double av = std::abs(v);
    const char* sign = (v < -0.5) ? "-" : "";
    if (av >= 1e6)
        std::snprintf(buf, sizeof(buf), "%s$%.2fM", sign, av / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%s$%'.0f", sign, std::round(av));
    return std::string(buf);
}

std::string fmt_table_dollar(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "$%'.0f", std::round(v));
    return std::string(buf);
}

} // namespace

struct GncPluginPageRetirementPrivate
{
    GtkWidget* root_widget = nullptr;
    GtkWidget* left_panel = nullptr;

    // Top display options
    GtkToggleButton* btn_toggle_params = nullptr;
    GtkComboBoxText* combo_chart_mode = nullptr;
    GtkCheckButton* chk_real_dollars = nullptr;

    // Controls
    GtkSpinButton* spin_cur_age = nullptr;
    GtkSpinButton* spin_ret_age = nullptr;
    GtkSpinButton* spin_hor_age = nullptr;
    GtkComboBoxText* combo_filing = nullptr;
    GtkComboBoxText* combo_objective = nullptr;
    GtkComboBoxText* combo_strategy = nullptr;

    GtkSpinButton* spin_target_spend = nullptr;
    GtkSpinButton* spin_target_bequest = nullptr;
    GtkSpinButton* spin_ss_fra = nullptr;
    GtkSpinButton* spin_ss_age = nullptr;
    GtkCheckButton* chk_opt_ss = nullptr;
    GtkSpinButton* spin_pension = nullptr;
    GtkSpinButton* spin_pension_age = nullptr;
    GtkCheckButton* chk_pension_cola = nullptr;

    GtkSpinButton* spin_contrib_tax = nullptr;
    GtkSpinButton* spin_contrib_def = nullptr;
    GtkSpinButton* spin_contrib_roth = nullptr;
    GtkSpinButton* spin_contrib_hsa = nullptr;

    GtkSpinButton* spin_return_pct = nullptr;
    GtkSpinButton* spin_vol_pct = nullptr;
    GtkSpinButton* spin_inf_pct = nullptr;
    GtkSpinButton* spin_ret_taxable = nullptr;      // per-bucket return: taxable
    GtkSpinButton* spin_ret_tax_def = nullptr;      // per-bucket return: tax-deferred
    GtkSpinButton* spin_ret_tax_free = nullptr;     // per-bucket return: tax-free (Roth)
    GtkSpinButton* spin_ret_hsa = nullptr;          // per-bucket return: HSA
    GtkSpinButton* spin_basis_pct = nullptr;        // taxable cost-basis %
    GtkSpinButton* spin_rmd_age = nullptr;
    GtkComboBoxText* combo_state = nullptr;
    GtkCheckButton* chk_enable_aca = nullptr;
    GtkSpinButton* spin_aca_bench = nullptr;
    GtkCheckButton* chk_aca_cliff = nullptr;
    GtkCheckButton* chk_reinvest_rmd = nullptr;
    GtkCheckButton* chk_enable_guardrails = nullptr;
    GtkCheckButton* chk_enable_glide = nullptr;
    GtkSpinButton* spin_eq_pre = nullptr;
    GtkSpinButton* spin_eq_in = nullptr;
    GtkSpinButton* spin_eq_ret = nullptr;
    GtkSpinButton* spin_bd_ret = nullptr;
    GtkSpinButton* spin_eq_vol = nullptr;
    GtkSpinButton* spin_bd_vol = nullptr;
    GtkSpinButton* spin_gr_upper = nullptr;
    GtkSpinButton* spin_gr_lower = nullptr;
    GtkSpinButton* spin_gr_adj = nullptr;
    GtkSpinButton* spin_heir_tax = nullptr;
    GtkSpinButton* spin_tips_floor = nullptr;
    GtkSpinButton* spin_bridge_inc = nullptr;
    GtkSpinButton* spin_bridge_start = nullptr;
    GtkSpinButton* spin_bridge_end = nullptr;
    GtkSpinButton* spin_mc_trials = nullptr;

    // Spouse / Partner controls
    GtkCheckButton* chk_has_spouse = nullptr;
    GtkSpinButton* spin_sp_cur_age = nullptr;
    GtkSpinButton* spin_sp_ret_age = nullptr;
    GtkSpinButton* spin_sp_hor_age = nullptr;
    GtkSpinButton* spin_sp_ss_fra = nullptr;
    GtkSpinButton* spin_sp_ss_age = nullptr;
    GtkCheckButton* chk_sp_opt_ss = nullptr;
    GtkSpinButton* spin_sp_pension = nullptr;
    GtkSpinButton* spin_sp_pension_age = nullptr;
    GtkCheckButton* chk_sp_pension_cola = nullptr;
    GtkSpinButton* spin_sp_rmd_age = nullptr;
    GtkSpinButton* spin_sp_contrib_def = nullptr;
    GtkSpinButton* spin_sp_contrib_roth = nullptr;

    // Spending Phases & One-Time Events
    GtkSpinButton* spin_slow_go_age = nullptr;
    GtkSpinButton* spin_slow_go_pct = nullptr;
    GtkSpinButton* spin_no_go_age = nullptr;
    GtkSpinButton* spin_no_go_pct = nullptr;
    GtkSpinButton* spin_debt_pmt = nullptr;
    GtkSpinButton* spin_debt_age = nullptr;
    GtkSpinButton* spin_ltc_age = nullptr;
    GtkSpinButton* spin_ltc_yrs = nullptr;
    GtkSpinButton* spin_ltc_cost = nullptr;
    GtkListStore* event_store = nullptr;
    GtkTreeView* event_tv = nullptr;

    // Bucket summary label
    GtkLabel* lbl_bucket_summary = nullptr;

    // KPI labels
    GtkLabel* lbl_kpi_primary_title = nullptr;
    GtkLabel* lbl_kpi_primary_val = nullptr;
    GtkLabel* lbl_kpi_adv_val = nullptr;
    GtkLabel* lbl_kpi_tax_val = nullptr;
    GtkLabel* lbl_kpi_mc_val = nullptr;
    GtkLabel* lbl_kpi_rec_val = nullptr;

    // Visualizations & tables
    GtkDrawingArea* chart_area = nullptr;
    int chart_hover_idx = -1;
    double chart_hover_x = 0.0;
    double chart_hover_y = 0.0;
    GtkListStore* sched_store = nullptr;
    GtkListStore* cmp_store = nullptr;
    GtkListStore* acct_store = nullptr;
    GtkTreeModelFilter* acct_filter = nullptr;
    GtkSearchEntry* entry_acct_filter = nullptr;
    GtkLabel* lbl_acct_count = nullptr;
    GtkListStore* stress_store = nullptr;
    GtkListStore* sens_store = nullptr;
    GtkListStore* hist_store = nullptr;
    GtkLabel* lbl_hist_summary = nullptr;
    GtkListStore* ss_be_store = nullptr;
    GtkTreeViewColumn* sens_cols[6] = {};

    std::vector<MappedAccount> accounts;
    std::vector<CashflowEvent> events;
    std::string last_plan_path;
    PlanInput last_input;
    PlanResult last_result;
    bool has_snapshot = false;
    PlanInput snap_input;
    PlanResult snap_result;

    // Ask Owl AI Advisor
    GtkComboBoxText* combo_ai_provider = nullptr;
    GtkEntry* entry_ai_endpoint = nullptr;
    GtkEntry* entry_ai_model = nullptr;
    GtkEntry* entry_ai_key = nullptr;
    GtkEntry* entry_ai_question = nullptr;
    GtkTextView* tv_ai_response = nullptr;
    GtkLabel* lbl_ai_status = nullptr;
    GtkWidget* btn_ai_apply = nullptr;
    SoupSession* soup_session = nullptr;
    bool has_ai_suggestion = false;
    PlanInput ai_suggested_input;
};

G_DEFINE_TYPE_WITH_PRIVATE(GncPluginPageRetirement, gnc_plugin_page_retirement, GNC_TYPE_PLUGIN_PAGE)

#define GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(o) \
    ((GncPluginPageRetirementPrivate*)gnc_plugin_page_retirement_get_instance_private((GncPluginPageRetirement*)(o)))

static void gnc_plugin_page_retirement_finalize(GObject* object);
static GtkWidget* gnc_plugin_page_retirement_create_widget(GncPluginPage* plugin_page);
static void gnc_plugin_page_retirement_destroy_widget(GncPluginPage* plugin_page);
static void gnc_plugin_page_retirement_save_page(GncPluginPage* plugin_page, GKeyFile* key_file, const gchar* group_name);
static GncPluginPage* gnc_plugin_page_retirement_recreate_page(GtkWidget* window, GKeyFile* key_file, const gchar* group_name);

static void refresh_accounts_from_book(GncPluginPageRetirementPrivate* priv);
static void populate_schedule_and_comparison(GncPluginPageRetirementPrivate* priv);
static void run_planner(GncPluginPageRetirementPrivate* priv);

GncPluginPage*
gnc_plugin_page_retirement_new(void)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(g_object_new(GNC_TYPE_PLUGIN_PAGE_RETIREMENT, nullptr));
    g_object_set(G_OBJECT(page), "page-name", _("Retirement Planner (Owl)"), nullptr);
    return GNC_PLUGIN_PAGE(page);
}

static void
gnc_plugin_page_retirement_class_init(GncPluginPageRetirementClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GncPluginPageClass* gnc_plugin_class = GNC_PLUGIN_PAGE_CLASS(klass);

    object_class->finalize = gnc_plugin_page_retirement_finalize;

    gnc_plugin_class->tab_icon = GNC_ICON_BUDGET;
    gnc_plugin_class->plugin_name = GNC_PLUGIN_PAGE_RETIREMENT_NAME;
    gnc_plugin_class->create_widget = gnc_plugin_page_retirement_create_widget;
    gnc_plugin_class->destroy_widget = gnc_plugin_page_retirement_destroy_widget;
    gnc_plugin_class->save_page = gnc_plugin_page_retirement_save_page;
    gnc_plugin_class->recreate_page = gnc_plugin_page_retirement_recreate_page;
}

static void
gnc_plugin_page_retirement_init(GncPluginPageRetirement* page)
{
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);
    new (&priv->accounts) std::vector<MappedAccount>();
    new (&priv->events) std::vector<CashflowEvent>();
    new (&priv->last_plan_path) std::string();
    new (&priv->last_input) PlanInput();
    new (&priv->last_result) PlanResult();
    new (&priv->snap_input) PlanInput();
    new (&priv->snap_result) PlanResult();
    new (&priv->ai_suggested_input) PlanInput();

    gnc_plugin_page_add_book(GNC_PLUGIN_PAGE(page), gnc_get_current_book());
}

static void
gnc_plugin_page_retirement_finalize(GObject* object)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(object);
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);
    priv->accounts.~vector();
    priv->events.~vector();
    priv->last_plan_path.~basic_string();
    priv->last_input.~PlanInput();
    priv->last_result.~PlanResult();
    priv->snap_input.~PlanInput();
    priv->snap_result.~PlanResult();
    priv->ai_suggested_input.~PlanInput();
    G_OBJECT_CLASS(gnc_plugin_page_retirement_parent_class)->finalize(object);
}

static AccountBuckets
compute_buckets_from_accounts(const std::vector<MappedAccount>& accounts)
{
    AccountBuckets b;
    for (const auto& a : accounts)
    {
        if (!a.included || a.balance <= 0.0)
            continue;
        switch (a.bucket)
        {
            case TaxBucket::Taxable:
                b.taxable += a.balance;
                b.taxable_basis += a.balance * 0.70;
                break;
            case TaxBucket::TaxDeferred:
                b.tax_deferred += a.balance;
                break;
            case TaxBucket::TaxFree:
                b.tax_free += a.balance;
                break;
            case TaxBucket::HSA:
                b.hsa += a.balance;
                break;
            case TaxBucket::Excluded:
                break;
        }
    }
    return b;
}

static void
update_bucket_summary_label(GncPluginPageRetirementPrivate* priv)
{
    AccountBuckets b = compute_buckets_from_accounts(priv->accounts);
    std::string txt = "<b>Balances:</b>  "
        + std::string("Taxable ") + fmt_currency(b.taxable)
        + "  •  401k/IRA " + fmt_currency(b.tax_deferred)
        + "  •  Roth " + fmt_currency(b.tax_free)
        + "  •  HSA " + fmt_currency(b.hsa)
        + "  •  <b>Total " + fmt_currency(b.total()) + "</b>";
    gtk_label_set_markup(priv->lbl_bucket_summary, txt.c_str());

    if (priv->lbl_acct_count)
    {
        size_t inc = 0;
        for (const auto& a : priv->accounts)
            if (a.included && a.bucket != TaxBucket::Excluded)
                ++inc;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu / %zu included", inc, priv->accounts.size());
        gtk_label_set_text(priv->lbl_acct_count, buf);
    }
}

static void
collect_account_cb(Account* acct, gpointer user_data)
{
    auto* list = static_cast<std::vector<MappedAccount>*>(user_data);
    if (!acct)
        return;
    GNCAccountType t = xaccAccountGetType(acct);
    if (t != ACCT_TYPE_ASSET && t != ACCT_TYPE_BANK &&
        t != ACCT_TYPE_STOCK && t != ACCT_TYPE_MUTUAL)
        return;
    if (gnc_account_n_children(acct) > 0 && xaccAccountGetPlaceholder(acct))
        return;

    gnc_commodity* curr = gnc_default_currency();
    gnc_numeric bal = xaccAccountGetBalanceInCurrency(acct, curr, FALSE);
    double d_bal = gnc_numeric_to_double(bal);
    if (std::abs(d_bal) < 0.01)
        return;

    gchar* full = gnc_account_get_full_name(acct);
    const gchar* desc = xaccAccountGetDescription(acct);
    MappedAccount ma;
    ma.full_name = full ? full : "";
    ma.balance = d_bal;
    ma.bucket = classify_account_name(ma.full_name, desc ? desc : "");
    ma.included = (ma.bucket != TaxBucket::Excluded);
    g_free(full);

    list->push_back(std::move(ma));
}

static void
sync_acct_store_from_accounts(GncPluginPageRetirementPrivate* priv)
{
    gtk_list_store_clear(priv->acct_store);
    for (size_t i = 0; i < priv->accounts.size(); ++i)
    {
        const auto& ma = priv->accounts[i];
        GtkTreeIter iter;
        gtk_list_store_append(priv->acct_store, &iter);
        std::string bal_str = fmt_table_dollar(ma.balance);
        gtk_list_store_set(priv->acct_store, &iter,
                           ACCT_COL_INCLUDE, ma.included ? TRUE : FALSE,
                           ACCT_COL_NAME, ma.full_name.c_str(),
                           ACCT_COL_BAL, bal_str.c_str(),
                           ACCT_COL_BUCKET_STR, bucket_label(ma.bucket),
                           ACCT_COL_BUCKET_ID, static_cast<int>(ma.bucket),
                           ACCT_COL_ORIG_IDX, static_cast<int>(i),
                           -1);
    }
    update_bucket_summary_label(priv);
}

static void
refresh_accounts_from_book(GncPluginPageRetirementPrivate* priv)
{
    priv->accounts.clear();
    QofBook* book = gnc_get_current_book();
    if (book)
    {
        Account* root = gnc_book_get_root_account(book);
        if (root)
            gnc_account_foreach_descendant(root, collect_account_cb, &priv->accounts);
    }
    sync_acct_store_from_accounts(priv);
}

static void
populate_schedule_and_comparison(GncPluginPageRetirementPrivate* priv)
{
    const PlanResult& res = priv->last_result;
    const bool use_real = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars));
    const double inf_rate = priv->last_input.inflation;

    gtk_list_store_clear(priv->sched_store);
    for (size_t i = 0; i < res.schedule.size(); ++i)
    {
        const auto& yp = res.schedule[i];
        const double div = use_real ? std::pow(1.0 + inf_rate, static_cast<double>(i)) : 1.0;

        char brk_buf[32];
        if (yp.irmaa_tier > 0)
            std::snprintf(brk_buf, sizeof(brk_buf), "%d%% / T%d%s", yp.marginal_bracket_pct, yp.irmaa_tier,
                          yp.cliff_warning ? " ⚠" : "");
        else
            std::snprintf(brk_buf, sizeof(brk_buf), "%d%%%s", yp.marginal_bracket_pct,
                          yp.cliff_warning ? " ⚠" : "");

        GtkTreeIter iter;
        gtk_list_store_append(priv->sched_store, &iter);
        gtk_list_store_set(priv->sched_store, &iter,
                           COL_YEAR, yp.year,
                           COL_AGE, yp.age,
                           COL_TOTAL, fmt_table_dollar(yp.end_total_nominal / div).c_str(),
                           COL_TAXABLE, fmt_table_dollar(yp.end_taxable / div).c_str(),
                           COL_TAX_DEF, fmt_table_dollar(yp.end_tax_deferred / div).c_str(),
                           COL_ROTH, fmt_table_dollar(yp.end_tax_free / div).c_str(),
                           COL_HSA, fmt_table_dollar(yp.end_hsa / div).c_str(),
                           COL_FIXED_INC, fmt_table_dollar((yp.ss_income + yp.pension_income) / div).c_str(),
                           COL_RMD, fmt_table_dollar(yp.rmd_amount / div).c_str(),
                           COL_ROTH_CONV, fmt_table_dollar(yp.roth_conversion / div).c_str(),
                           COL_TOTAL_TAX, fmt_table_dollar(yp.total_tax / div).c_str(),
                           COL_NET_SPEND, fmt_table_dollar(yp.net_spending_nominal / div).c_str(),
                           COL_FLOOR_GAP, fmt_table_dollar(yp.floor_surplus_nominal / div).c_str(),
                           COL_RMD_REINV, fmt_table_dollar(yp.rmd_reinvested / div).c_str(),
                           COL_BRACKET_IRMAA, brk_buf,
                           -1);
    }

    // Populate Baseline vs Owl comparison table
    gtk_list_store_clear(priv->cmp_store);
    auto add_cmp_row = [&](const char* metric, const std::string& base,
                           const std::string& owl, const std::string& diff) {
        GtkTreeIter iter;
        gtk_list_store_append(priv->cmp_store, &iter);
        gtk_list_store_set(priv->cmp_store, &iter,
                           CMP_COL_METRIC, metric,
                           CMP_COL_BASELINE, base.c_str(),
                           CMP_COL_OWL, owl.c_str(),
                           CMP_COL_DIFF, diff.c_str(),
                           -1);
    };

    double sp_diff = res.sustainable_spending_real - res.baseline_spending_real;
    double bq_diff = res.final_bequest_real - res.baseline_bequest_real;
    double tx_diff = res.lifetime_taxes_real - res.baseline_taxes_real;

    add_cmp_row(_("Withdrawal Strategy"),
                strategy_label(WithdrawalStrategy::StandardTaxableFirst),
                strategy_label(res.chosen_strategy),
                res.chosen_strategy == WithdrawalStrategy::StandardTaxableFirst ? "Same" : "Optimized");
    add_cmp_row(_("Social Security Claim Age"),
                std::to_string(priv->last_input.ss_claim_age),
                std::to_string(res.chosen_ss_age),
                std::to_string(res.chosen_ss_age - priv->last_input.ss_claim_age) + " yrs");
    if (priv->last_input.has_spouse)
    {
        add_cmp_row(_("Spouse SS Claim Age"),
                    std::to_string(priv->last_input.spouse_ss_claim_age),
                    std::to_string(res.chosen_spouse_ss_age),
                    std::to_string(res.chosen_spouse_ss_age - priv->last_input.spouse_ss_claim_age) + " yrs");
    }
    add_cmp_row(_("Sustainable Spending / yr (Today $)"),
                fmt_currency(res.baseline_spending_real),
                fmt_currency(res.sustainable_spending_real),
                (sp_diff >= 0 ? "+" : "") + fmt_currency(sp_diff));
    add_cmp_row(_("After-Tax Bequest (Today $)"),
                fmt_currency(res.baseline_bequest_real),
                fmt_currency(res.final_bequest_real),
                (bq_diff >= 0 ? "+" : "") + fmt_currency(bq_diff));
    add_cmp_row(_("Lifetime Taxes + IRMAA (Today $)"),
                fmt_currency(res.baseline_taxes_real),
                fmt_currency(res.lifetime_taxes_real),
                (tx_diff >= 0 ? "+" : "") + fmt_currency(tx_diff));
    add_cmp_row(_("Monte Carlo P10 / P50 / P90 (Today $)"),
                "—",
                fmt_currency(res.monte_carlo.p50_end_real),
                fmt_currency(res.monte_carlo.p10_end_real) + " .. " + fmt_currency(res.monte_carlo.p90_end_real));

    if (priv->has_snapshot)
    {
        const PlanResult& pa = priv->snap_result;
        double d_sp = res.sustainable_spending_real - pa.sustainable_spending_real;
        double d_bq = res.final_bequest_real - pa.final_bequest_real;
        double d_tx = res.lifetime_taxes_real - pa.lifetime_taxes_real;
        add_cmp_row(_("--- Plan A (Snapshot) vs Plan B (Current) ---"), "Plan A", "Plan B", "Delta");
        add_cmp_row(_("Snapshot Strategy"),
                    strategy_label(pa.chosen_strategy),
                    strategy_label(res.chosen_strategy),
                    pa.chosen_strategy == res.chosen_strategy ? "Same" : "Changed");
        add_cmp_row(_("Snapshot Spending / yr"),
                    fmt_currency(pa.sustainable_spending_real),
                    fmt_currency(res.sustainable_spending_real),
                    (d_sp >= 0 ? "+" : "") + fmt_currency(d_sp));
        add_cmp_row(_("Snapshot Final Bequest"),
                    fmt_currency(pa.final_bequest_real),
                    fmt_currency(res.final_bequest_real),
                    (d_bq >= 0 ? "+" : "") + fmt_currency(d_bq));
        add_cmp_row(_("Snapshot Lifetime Taxes"),
                    fmt_currency(pa.lifetime_taxes_real),
                    fmt_currency(res.lifetime_taxes_real),
                    (d_tx >= 0 ? "+" : "") + fmt_currency(d_tx));
    }
}

static PlanInput
read_plan_input_from_ui(GncPluginPageRetirementPrivate* priv)
{
    PlanInput in;
    GDate* today = g_date_new();
    g_date_set_time_t(today, time(nullptr));
    in.start_year = g_date_get_year(today);
    g_date_free(today);

    in.current_age = gtk_spin_button_get_value_as_int(priv->spin_cur_age);
    in.retirement_age = std::max(in.current_age, gtk_spin_button_get_value_as_int(priv->spin_ret_age));
    in.horizon_age = std::max(in.retirement_age + 1, gtk_spin_button_get_value_as_int(priv->spin_hor_age));
    in.filing_status = (gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_filing)) == 0)
                       ? FilingStatus::Single : FilingStatus::MarriedFilingJointly;
    in.objective = (gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_objective)) == 0)
                   ? PlanObjective::MaximizeSpending : PlanObjective::MaximizeBequest;
    in.strategy = static_cast<WithdrawalStrategy>(
        std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_strategy)), 0, 5));

    in.initial_balances = compute_buckets_from_accounts(priv->accounts);
    in.target_annual_spending = gtk_spin_button_get_value(priv->spin_target_spend);
    in.target_bequest = gtk_spin_button_get_value(priv->spin_target_bequest);
    in.ss_fra_annual = gtk_spin_button_get_value(priv->spin_ss_fra);
    in.ss_claim_age = gtk_spin_button_get_value_as_int(priv->spin_ss_age);
    in.optimize_ss_age = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_opt_ss));
    in.pension_annual = gtk_spin_button_get_value(priv->spin_pension);
    in.pension_start_age = gtk_spin_button_get_value_as_int(priv->spin_pension_age);
    in.pension_cola = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_pension_cola));

    in.contrib_taxable = gtk_spin_button_get_value(priv->spin_contrib_tax);
    in.contrib_tax_deferred = gtk_spin_button_get_value(priv->spin_contrib_def);
    in.contrib_tax_free = gtk_spin_button_get_value(priv->spin_contrib_roth);
    in.contrib_hsa = gtk_spin_button_get_value(priv->spin_contrib_hsa);

    in.nominal_return = gtk_spin_button_get_value(priv->spin_return_pct) / 100.0;
    in.volatility = gtk_spin_button_get_value(priv->spin_vol_pct) / 100.0;
    in.inflation = gtk_spin_button_get_value(priv->spin_inf_pct) / 100.0;
    in.return_taxable     = gtk_spin_button_get_value(priv->spin_ret_taxable)  / 100.0;
    in.return_tax_deferred= gtk_spin_button_get_value(priv->spin_ret_tax_def)  / 100.0;
    in.return_tax_free    = gtk_spin_button_get_value(priv->spin_ret_tax_free) / 100.0;
    in.return_hsa         = gtk_spin_button_get_value(priv->spin_ret_hsa)      / 100.0;
    in.taxable_basis_pct  = gtk_spin_button_get_value(priv->spin_basis_pct)    / 100.0;
    in.initial_balances.taxable_basis = in.initial_balances.taxable * in.taxable_basis_pct;
    in.rmd_start_age = gtk_spin_button_get_value_as_int(priv->spin_rmd_age);
    in.state = static_cast<USState>(
        std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_state)), 0, 51));
    in.enable_aca = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_enable_aca));
    in.aca_benchmark_premium = gtk_spin_button_get_value(priv->spin_aca_bench);
    in.aca_enforce_400_cliff = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_aca_cliff));
    in.reinvest_rmd_surplus = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_reinvest_rmd));
    in.enable_guardrails = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_enable_guardrails));
    in.guardrail_upper_wr = gtk_spin_button_get_value(priv->spin_gr_upper) / 100.0;
    in.guardrail_lower_wr = gtk_spin_button_get_value(priv->spin_gr_lower) / 100.0;
    in.guardrail_adj_pct = gtk_spin_button_get_value(priv->spin_gr_adj) / 100.0;
    in.enable_glide_path = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_enable_glide));
    in.equity_pct_pre_ret = gtk_spin_button_get_value(priv->spin_eq_pre);
    in.equity_pct_in_ret = gtk_spin_button_get_value(priv->spin_eq_in);
    in.equity_return = gtk_spin_button_get_value(priv->spin_eq_ret) / 100.0;
    in.bond_return = gtk_spin_button_get_value(priv->spin_bd_ret) / 100.0;
    in.equity_vol = gtk_spin_button_get_value(priv->spin_eq_vol) / 100.0;
    in.bond_vol = gtk_spin_button_get_value(priv->spin_bd_vol) / 100.0;
    in.heir_tax_rate = gtk_spin_button_get_value(priv->spin_heir_tax) / 100.0;
    in.tips_floor_annual = gtk_spin_button_get_value(priv->spin_tips_floor);
    in.bridge_income_annual = gtk_spin_button_get_value(priv->spin_bridge_inc);
    in.bridge_start_age = gtk_spin_button_get_value_as_int(priv->spin_bridge_start);
    in.bridge_end_age = gtk_spin_button_get_value_as_int(priv->spin_bridge_end);
    in.monte_carlo_trials = gtk_spin_button_get_value_as_int(priv->spin_mc_trials);

    in.has_spouse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_has_spouse));
    in.spouse_current_age = gtk_spin_button_get_value_as_int(priv->spin_sp_cur_age);
    in.spouse_retirement_age = std::max(in.spouse_current_age, gtk_spin_button_get_value_as_int(priv->spin_sp_ret_age));
    in.spouse_horizon_age = std::max(in.spouse_retirement_age + 1, gtk_spin_button_get_value_as_int(priv->spin_sp_hor_age));
    in.spouse_ss_fra_annual = gtk_spin_button_get_value(priv->spin_sp_ss_fra);
    in.spouse_ss_claim_age = gtk_spin_button_get_value_as_int(priv->spin_sp_ss_age);
    in.spouse_optimize_ss_age = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_sp_opt_ss));
    in.spouse_pension_annual = gtk_spin_button_get_value(priv->spin_sp_pension);
    in.spouse_pension_start_age = gtk_spin_button_get_value_as_int(priv->spin_sp_pension_age);
    in.spouse_pension_cola = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_sp_pension_cola));
    in.spouse_rmd_start_age = gtk_spin_button_get_value_as_int(priv->spin_sp_rmd_age);
    in.spouse_contrib_tax_deferred = gtk_spin_button_get_value(priv->spin_sp_contrib_def);
    in.spouse_contrib_tax_free = gtk_spin_button_get_value(priv->spin_sp_contrib_roth);

    in.slow_go_age = gtk_spin_button_get_value_as_int(priv->spin_slow_go_age);
    in.slow_go_pct = gtk_spin_button_get_value(priv->spin_slow_go_pct);
    in.no_go_age = std::max(in.slow_go_age, gtk_spin_button_get_value_as_int(priv->spin_no_go_age));
    in.no_go_pct = gtk_spin_button_get_value(priv->spin_no_go_pct);
    in.debt_annual_payment = gtk_spin_button_get_value(priv->spin_debt_pmt);
    in.debt_payoff_age = gtk_spin_button_get_value_as_int(priv->spin_debt_age);
    in.ltc_start_age = gtk_spin_button_get_value_as_int(priv->spin_ltc_age);
    in.ltc_years = gtk_spin_button_get_value_as_int(priv->spin_ltc_yrs);
    in.ltc_annual_cost_real = gtk_spin_button_get_value(priv->spin_ltc_cost);
    in.events = priv->events;
    return in;
}

static void
sync_event_store(GncPluginPageRetirementPrivate* priv)
{
    if (!priv->event_store)
        return;
    gtk_list_store_clear(priv->event_store);
    for (const auto& ev : priv->events)
    {
        GtkTreeIter it;
        gtk_list_store_append(priv->event_store, &it);
        std::string amt_str = (ev.amount_real >= 0 ? "+" : "-") + fmt_table_dollar(std::abs(ev.amount_real));
        gtk_list_store_set(priv->event_store, &it,
                           0, ev.age,
                           1, ev.name.c_str(),
                           2, amt_str.c_str(),
                           3, bucket_label(ev.bucket),
                           -1);
    }
}

static void
update_spouse_sensitivity(GncPluginPageRetirementPrivate* priv)
{
    const gboolean en = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_has_spouse));
    GtkWidget* widgets[] = {
        GTK_WIDGET(priv->spin_sp_cur_age), GTK_WIDGET(priv->spin_sp_ret_age),
        GTK_WIDGET(priv->spin_sp_hor_age), GTK_WIDGET(priv->spin_sp_ss_fra),
        GTK_WIDGET(priv->spin_sp_ss_age), GTK_WIDGET(priv->chk_sp_opt_ss),
        GTK_WIDGET(priv->spin_sp_pension), GTK_WIDGET(priv->spin_sp_pension_age),
        GTK_WIDGET(priv->chk_sp_pension_cola),
        GTK_WIDGET(priv->spin_sp_rmd_age), GTK_WIDGET(priv->spin_sp_contrib_def),
        GTK_WIDGET(priv->spin_sp_contrib_roth)
    };
    for (GtkWidget* w : widgets)
        if (w) gtk_widget_set_sensitive(w, en);
}

static void
apply_plan_input_to_ui(GncPluginPageRetirementPrivate* priv, const PlanInput& in)
{
    gtk_spin_button_set_value(priv->spin_cur_age, in.current_age);
    gtk_spin_button_set_value(priv->spin_ret_age, in.retirement_age);
    gtk_spin_button_set_value(priv->spin_hor_age, in.horizon_age);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_filing),
                             in.filing_status == FilingStatus::Single ? 0 : 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_objective),
                             in.objective == PlanObjective::MaximizeSpending ? 0 : 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_strategy),
                             std::clamp(static_cast<int>(in.strategy), 0, 5));

    gtk_spin_button_set_value(priv->spin_target_spend, in.target_annual_spending);
    gtk_spin_button_set_value(priv->spin_target_bequest, in.target_bequest);
    gtk_spin_button_set_value(priv->spin_ss_fra, in.ss_fra_annual);
    gtk_spin_button_set_value(priv->spin_ss_age, in.ss_claim_age);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_opt_ss), in.optimize_ss_age);
    gtk_spin_button_set_value(priv->spin_pension, in.pension_annual);
    gtk_spin_button_set_value(priv->spin_pension_age, in.pension_start_age);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_pension_cola), in.pension_cola);

    gtk_spin_button_set_value(priv->spin_contrib_tax, in.contrib_taxable);
    gtk_spin_button_set_value(priv->spin_contrib_def, in.contrib_tax_deferred);
    gtk_spin_button_set_value(priv->spin_contrib_roth, in.contrib_tax_free);
    gtk_spin_button_set_value(priv->spin_contrib_hsa, in.contrib_hsa);

    gtk_spin_button_set_value(priv->spin_return_pct, in.nominal_return * 100.0);
    gtk_spin_button_set_value(priv->spin_vol_pct, in.volatility * 100.0);
    gtk_spin_button_set_value(priv->spin_inf_pct, in.inflation * 100.0);
    gtk_spin_button_set_value(priv->spin_ret_taxable,  in.return_taxable      * 100.0);
    gtk_spin_button_set_value(priv->spin_ret_tax_def,  in.return_tax_deferred * 100.0);
    gtk_spin_button_set_value(priv->spin_ret_tax_free, in.return_tax_free     * 100.0);
    gtk_spin_button_set_value(priv->spin_ret_hsa,      in.return_hsa          * 100.0);
    gtk_spin_button_set_value(priv->spin_basis_pct,    in.taxable_basis_pct   * 100.0);
    gtk_spin_button_set_value(priv->spin_rmd_age, in.rmd_start_age);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state),
                             std::clamp(static_cast<int>(in.state), 0, 51));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_enable_aca), in.enable_aca);
    gtk_spin_button_set_value(priv->spin_aca_bench, in.aca_benchmark_premium);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_aca_cliff), in.aca_enforce_400_cliff);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_reinvest_rmd), in.reinvest_rmd_surplus);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_enable_guardrails), in.enable_guardrails);
    gtk_spin_button_set_value(priv->spin_gr_upper, in.guardrail_upper_wr * 100.0);
    gtk_spin_button_set_value(priv->spin_gr_lower, in.guardrail_lower_wr * 100.0);
    gtk_spin_button_set_value(priv->spin_gr_adj, in.guardrail_adj_pct * 100.0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_enable_glide), in.enable_glide_path);
    gtk_spin_button_set_value(priv->spin_eq_pre, in.equity_pct_pre_ret);
    gtk_spin_button_set_value(priv->spin_eq_in, in.equity_pct_in_ret);
    gtk_spin_button_set_value(priv->spin_eq_ret, in.equity_return * 100.0);
    gtk_spin_button_set_value(priv->spin_bd_ret, in.bond_return * 100.0);
    gtk_spin_button_set_value(priv->spin_eq_vol, in.equity_vol * 100.0);
    gtk_spin_button_set_value(priv->spin_bd_vol, in.bond_vol * 100.0);
    gtk_spin_button_set_value(priv->spin_heir_tax, in.heir_tax_rate * 100.0);
    gtk_spin_button_set_value(priv->spin_tips_floor, in.tips_floor_annual);
    gtk_spin_button_set_value(priv->spin_bridge_inc, in.bridge_income_annual);
    gtk_spin_button_set_value(priv->spin_bridge_start, in.bridge_start_age);
    gtk_spin_button_set_value(priv->spin_bridge_end, in.bridge_end_age);
    gtk_spin_button_set_value(priv->spin_mc_trials, in.monte_carlo_trials);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_has_spouse), in.has_spouse);
    gtk_spin_button_set_value(priv->spin_sp_cur_age, in.spouse_current_age);
    gtk_spin_button_set_value(priv->spin_sp_ret_age, in.spouse_retirement_age);
    gtk_spin_button_set_value(priv->spin_sp_hor_age, in.spouse_horizon_age);
    gtk_spin_button_set_value(priv->spin_sp_ss_fra, in.spouse_ss_fra_annual);
    gtk_spin_button_set_value(priv->spin_sp_ss_age, in.spouse_ss_claim_age);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_sp_opt_ss), in.spouse_optimize_ss_age);
    gtk_spin_button_set_value(priv->spin_sp_pension, in.spouse_pension_annual);
    gtk_spin_button_set_value(priv->spin_sp_pension_age, in.spouse_pension_start_age);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_sp_pension_cola), in.spouse_pension_cola);
    gtk_spin_button_set_value(priv->spin_sp_rmd_age, in.spouse_rmd_start_age);
    gtk_spin_button_set_value(priv->spin_sp_contrib_def, in.spouse_contrib_tax_deferred);
    gtk_spin_button_set_value(priv->spin_sp_contrib_roth, in.spouse_contrib_tax_free);
    update_spouse_sensitivity(priv);

    gtk_spin_button_set_value(priv->spin_slow_go_age, in.slow_go_age);
    gtk_spin_button_set_value(priv->spin_slow_go_pct, in.slow_go_pct);
    gtk_spin_button_set_value(priv->spin_no_go_age, in.no_go_age);
    gtk_spin_button_set_value(priv->spin_no_go_pct, in.no_go_pct);
    gtk_spin_button_set_value(priv->spin_debt_pmt, in.debt_annual_payment);
    gtk_spin_button_set_value(priv->spin_debt_age, in.debt_payoff_age);
    gtk_spin_button_set_value(priv->spin_ltc_age, in.ltc_start_age);
    gtk_spin_button_set_value(priv->spin_ltc_yrs, in.ltc_years);
    gtk_spin_button_set_value(priv->spin_ltc_cost, in.ltc_annual_cost_real);
    priv->events = in.events;
    sync_event_store(priv);
}

static std::vector<std::pair<std::string, TaxBucket>>
collect_account_overrides(const GncPluginPageRetirementPrivate* priv)
{
    std::vector<std::pair<std::string, TaxBucket>> out;
    out.reserve(priv->accounts.size());
    for (const auto& a : priv->accounts)
        out.emplace_back(a.full_name, a.bucket);
    return out;
}

static std::vector<std::string>
collect_excluded_accounts(const GncPluginPageRetirementPrivate* priv)
{
    std::vector<std::string> out;
    for (const auto& a : priv->accounts)
        if (!a.included)
            out.push_back(a.full_name);
    return out;
}

static void
apply_account_overrides_to_ui(GncPluginPageRetirementPrivate* priv,
                              const std::vector<std::pair<std::string, TaxBucket>>& overrides,
                              const std::vector<std::string>& excluded = {})
{
    if (overrides.empty() && excluded.empty())
        return;
    for (auto& a : priv->accounts)
    {
        for (const auto& kv : overrides)
        {
            if (a.full_name == kv.first)
            {
                a.bucket = kv.second;
                a.included = (kv.second != TaxBucket::Excluded);
                break;
            }
        }
        for (const auto& ex : excluded)
        {
            if (a.full_name == ex)
            {
                a.included = false;
                break;
            }
        }
    }
    sync_acct_store_from_accounts(priv);
}

static constexpr const char* STATE_GROUP_PLAN = "RetirementPlanner";
static constexpr const char* STATE_GROUP_ACCTS = "RetirementPlannerAccounts";
static constexpr const char* STATE_GROUP_EXCLUDED = "RetirementPlannerExcluded";
static constexpr const char* STATE_GROUP_EVENTS = "RetirementPlannerEvents";

static void
save_state_to_book(GncPluginPageRetirementPrivate* priv, bool flush_to_disk = true)
{
    if (!priv || !priv->root_widget)
        return;
    GKeyFile* state = gnc_state_get_current();
    if (!state)
        return;

    PlanInput in = read_plan_input_from_ui(priv);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "current_age", in.current_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "retirement_age", in.retirement_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "horizon_age", in.horizon_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "filing_status", static_cast<int>(in.filing_status));
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "objective", static_cast<int>(in.objective));
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "strategy", static_cast<int>(in.strategy));
    g_key_file_set_double(state, STATE_GROUP_PLAN, "target_annual_spending", in.target_annual_spending);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "target_bequest", in.target_bequest);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "ss_fra_annual", in.ss_fra_annual);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "ss_claim_age", in.ss_claim_age);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "optimize_ss_age", in.optimize_ss_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "pension_annual", in.pension_annual);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "pension_start_age", in.pension_start_age);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "pension_cola", in.pension_cola);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_taxable", in.contrib_taxable);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_tax_deferred", in.contrib_tax_deferred);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_tax_free", in.contrib_tax_free);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_hsa", in.contrib_hsa);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "nominal_return", in.nominal_return);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "volatility", in.volatility);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "inflation", in.inflation);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "return_taxable",      in.return_taxable);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "return_tax_deferred", in.return_tax_deferred);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "return_tax_free",     in.return_tax_free);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "return_hsa",          in.return_hsa);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "taxable_basis_pct",   in.taxable_basis_pct);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "rmd_start_age", in.rmd_start_age);
    g_key_file_set_string(state, STATE_GROUP_PLAN, "state", state_code(in.state));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "enable_aca", in.enable_aca);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "aca_benchmark_premium", in.aca_benchmark_premium);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "aca_enforce_400_cliff", in.aca_enforce_400_cliff);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "reinvest_rmd_surplus", in.reinvest_rmd_surplus);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "enable_guardrails", in.enable_guardrails);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "guardrail_upper_wr", in.guardrail_upper_wr);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "guardrail_lower_wr", in.guardrail_lower_wr);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "guardrail_adj_pct", in.guardrail_adj_pct);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "enable_glide_path", in.enable_glide_path);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "equity_pct_pre_ret", in.equity_pct_pre_ret);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "equity_pct_in_ret", in.equity_pct_in_ret);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "equity_return", in.equity_return);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "bond_return", in.bond_return);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "equity_vol", in.equity_vol);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "bond_vol", in.bond_vol);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "heir_tax_rate", in.heir_tax_rate);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "tips_floor_annual", in.tips_floor_annual);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "bridge_income_annual", in.bridge_income_annual);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "bridge_start_age", in.bridge_start_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "bridge_end_age", in.bridge_end_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "debt_annual_payment", in.debt_annual_payment);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "debt_payoff_age", in.debt_payoff_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "ltc_start_age", in.ltc_start_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "ltc_years", in.ltc_years);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "ltc_annual_cost_real", in.ltc_annual_cost_real);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "monte_carlo_trials", in.monte_carlo_trials);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "has_spouse", in.has_spouse);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_current_age", in.spouse_current_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_retirement_age", in.spouse_retirement_age);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_horizon_age", in.spouse_horizon_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "spouse_ss_fra_annual", in.spouse_ss_fra_annual);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_ss_claim_age", in.spouse_ss_claim_age);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "spouse_optimize_ss_age", in.spouse_optimize_ss_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "spouse_pension_annual", in.spouse_pension_annual);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_pension_start_age", in.spouse_pension_start_age);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "spouse_pension_cola", in.spouse_pension_cola);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "spouse_rmd_start_age", in.spouse_rmd_start_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "spouse_contrib_tax_deferred", in.spouse_contrib_tax_deferred);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "spouse_contrib_tax_free", in.spouse_contrib_tax_free);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "slow_go_age", in.slow_go_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "slow_go_pct", in.slow_go_pct);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "no_go_age", in.no_go_age);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "no_go_pct", in.no_go_pct);

    g_key_file_set_integer(state, STATE_GROUP_PLAN, "chart_mode",
                           gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_chart_mode)));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "real_dollars",
                           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars)));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "show_params",
                           gtk_toggle_button_get_active(priv->btn_toggle_params));
    if (!priv->last_plan_path.empty())
        g_key_file_set_string(state, STATE_GROUP_PLAN, "last_owlplan_path", priv->last_plan_path.c_str());
    else
        g_key_file_remove_key(state, STATE_GROUP_PLAN, "last_owlplan_path", nullptr);

    if (priv->combo_ai_provider)
        g_key_file_set_integer(state, STATE_GROUP_PLAN, "ai_provider",
                               gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)));
    if (priv->entry_ai_endpoint)
        g_key_file_set_string(state, STATE_GROUP_PLAN, "ai_endpoint",
                              gtk_entry_get_text(priv->entry_ai_endpoint));
    if (priv->entry_ai_model)
        g_key_file_set_string(state, STATE_GROUP_PLAN, "ai_model",
                              gtk_entry_get_text(priv->entry_ai_model));

    g_key_file_remove_group(state, STATE_GROUP_ACCTS, nullptr);
    g_key_file_remove_group(state, STATE_GROUP_EXCLUDED, nullptr);
    g_key_file_remove_group(state, STATE_GROUP_EVENTS, nullptr);
    size_t excl_idx = 0;
    for (size_t i = 0; i < priv->accounts.size(); ++i)
    {
        const auto& a = priv->accounts[i];
        char akey[32];
        std::snprintf(akey, sizeof(akey), "acct_%zu", i);
        std::string aval = std::string(a.included ? "1" : "0") + "|" + bucket_label(a.bucket) + "|" + a.full_name;
        g_key_file_set_string(state, STATE_GROUP_ACCTS, akey, aval.c_str());

        if (!a.included)
        {
            char ekey[32];
            std::snprintf(ekey, sizeof(ekey), "excl_%zu", excl_idx++);
            g_key_file_set_string(state, STATE_GROUP_EXCLUDED, ekey, a.full_name.c_str());
        }
    }
    for (size_t i = 0; i < priv->events.size(); ++i)
    {
        const auto& ev = priv->events[i];
        char key[32];
        std::snprintf(key, sizeof(key), "event_%zu", i);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%d|%.2f|%s|%s",
                      ev.age, ev.amount_real, bucket_label(ev.bucket), ev.name.c_str());
        g_key_file_set_string(state, STATE_GROUP_EVENTS, key, buf);
    }

    if (flush_to_disk && gnc_current_session_exist())
    {
        if (QofSession* sess = gnc_get_current_session())
        {
            if (!g_key_file_has_group(state, "Top"))
            {
                if (QofBook* book = qof_session_get_book(sess))
                {
                    gchar guid_str[GUID_ENCODING_LENGTH + 1];
                    guid_to_string_buff(qof_entity_get_guid(QOF_INSTANCE(book)), guid_str);
                    g_key_file_set_string(state, "Top", "BookGuid", guid_str);
                }
            }
            gnc_state_save(sess);
        }
    }
}

static void
load_state_from_book(GncPluginPageRetirementPrivate* priv)
{
    GKeyFile* state = gnc_state_get_current();
    if (!state || !g_key_file_has_group(state, STATE_GROUP_PLAN))
        return;

    auto get_int = [&](const char* k, int def) {
        GError* err = nullptr;
        int v = g_key_file_get_integer(state, STATE_GROUP_PLAN, k, &err);
        if (err) { g_error_free(err); return def; }
        return v;
    };
    auto get_dbl = [&](const char* k, double def) {
        GError* err = nullptr;
        double v = g_key_file_get_double(state, STATE_GROUP_PLAN, k, &err);
        if (err) { g_error_free(err); return def; }
        return v;
    };
    auto get_bool = [&](const char* k, bool def) {
        GError* err = nullptr;
        gboolean v = g_key_file_get_boolean(state, STATE_GROUP_PLAN, k, &err);
        if (err) { g_error_free(err); return def; }
        return v != FALSE;
    };

    PlanInput in;
    in.current_age = std::clamp(get_int("current_age", in.current_age), 18, 95);
    in.retirement_age = std::clamp(get_int("retirement_age", in.retirement_age), 30, 95);
    in.horizon_age = std::clamp(get_int("horizon_age", in.horizon_age), 65, 110);
    in.filing_status = (get_int("filing_status", static_cast<int>(in.filing_status)) == 0)
                       ? FilingStatus::Single : FilingStatus::MarriedFilingJointly;
    in.objective = (get_int("objective", static_cast<int>(in.objective)) == 0)
                   ? PlanObjective::MaximizeSpending : PlanObjective::MaximizeBequest;
    in.strategy = static_cast<WithdrawalStrategy>(
        std::clamp(get_int("strategy", static_cast<int>(in.strategy)), 0, 5));
    in.target_annual_spending = std::max(0.0, get_dbl("target_annual_spending", in.target_annual_spending));
    in.target_bequest = std::max(0.0, get_dbl("target_bequest", in.target_bequest));
    in.ss_fra_annual = std::max(0.0, get_dbl("ss_fra_annual", in.ss_fra_annual));
    in.ss_claim_age = std::clamp(get_int("ss_claim_age", in.ss_claim_age), 62, 70);
    in.optimize_ss_age = get_bool("optimize_ss_age", in.optimize_ss_age);
    in.pension_annual = std::max(0.0, get_dbl("pension_annual", in.pension_annual));
    in.pension_start_age = std::clamp(get_int("pension_start_age", in.pension_start_age), 50, 85);
    in.pension_cola = get_bool("pension_cola", in.pension_cola);
    in.contrib_taxable = std::max(0.0, get_dbl("contrib_taxable", in.contrib_taxable));
    in.contrib_tax_deferred = std::max(0.0, get_dbl("contrib_tax_deferred", in.contrib_tax_deferred));
    in.contrib_tax_free = std::max(0.0, get_dbl("contrib_tax_free", in.contrib_tax_free));
    in.contrib_hsa = std::max(0.0, get_dbl("contrib_hsa", in.contrib_hsa));
    in.nominal_return = std::clamp(get_dbl("nominal_return", in.nominal_return), 0.0, 0.25);
    in.volatility = std::clamp(get_dbl("volatility", in.volatility), 0.0, 0.50);
    in.inflation = std::clamp(get_dbl("inflation", in.inflation), 0.0, 0.15);
    in.return_taxable      = std::clamp(get_dbl("return_taxable",      in.return_taxable),      0.0, 0.25);
    in.return_tax_deferred = std::clamp(get_dbl("return_tax_deferred", in.return_tax_deferred), 0.0, 0.25);
    in.return_tax_free     = std::clamp(get_dbl("return_tax_free",     in.return_tax_free),     0.0, 0.25);
    in.return_hsa          = std::clamp(get_dbl("return_hsa",          in.return_hsa),          0.0, 0.25);
    in.taxable_basis_pct   = std::clamp(get_dbl("taxable_basis_pct",   in.taxable_basis_pct),   0.0, 1.00);
    in.rmd_start_age = std::clamp(get_int("rmd_start_age", in.rmd_start_age), 72, 75);
    if (gchar* st = g_key_file_get_string(state, STATE_GROUP_PLAN, "state", nullptr))
    {
        in.state = state_from_code(st);
        g_free(st);
    }
    in.enable_aca = get_bool("enable_aca", in.enable_aca);
    in.aca_benchmark_premium = std::max(0.0, get_dbl("aca_benchmark_premium", in.aca_benchmark_premium));
    in.aca_enforce_400_cliff = get_bool("aca_enforce_400_cliff", in.aca_enforce_400_cliff);
    in.reinvest_rmd_surplus = get_bool("reinvest_rmd_surplus", in.reinvest_rmd_surplus);
    in.enable_guardrails = get_bool("enable_guardrails", in.enable_guardrails);
    in.guardrail_upper_wr = std::clamp(get_dbl("guardrail_upper_wr", in.guardrail_upper_wr), 0.02, 0.20);
    in.guardrail_lower_wr = std::clamp(get_dbl("guardrail_lower_wr", in.guardrail_lower_wr), 0.01, 0.15);
    in.guardrail_adj_pct = std::clamp(get_dbl("guardrail_adj_pct", in.guardrail_adj_pct), 0.01, 0.50);
    in.enable_glide_path = get_bool("enable_glide_path", in.enable_glide_path);
    in.equity_pct_pre_ret = std::clamp(get_dbl("equity_pct_pre_ret", in.equity_pct_pre_ret), 0.0, 100.0);
    in.equity_pct_in_ret = std::clamp(get_dbl("equity_pct_in_ret", in.equity_pct_in_ret), 0.0, 100.0);
    in.equity_return = std::clamp(get_dbl("equity_return", in.equity_return), 0.0, 0.25);
    in.bond_return = std::clamp(get_dbl("bond_return", in.bond_return), 0.0, 0.20);
    in.equity_vol = std::clamp(get_dbl("equity_vol", in.equity_vol), 0.0, 0.50);
    in.bond_vol = std::clamp(get_dbl("bond_vol", in.bond_vol), 0.0, 0.30);
    in.heir_tax_rate = std::clamp(get_dbl("heir_tax_rate", in.heir_tax_rate), 0.0, 0.60);
    in.tips_floor_annual = std::max(0.0, get_dbl("tips_floor_annual", in.tips_floor_annual));
    in.bridge_income_annual = std::max(0.0, get_dbl("bridge_income_annual", in.bridge_income_annual));
    in.bridge_start_age = std::clamp(get_int("bridge_start_age", in.bridge_start_age), 30, 95);
    in.bridge_end_age = std::clamp(get_int("bridge_end_age", in.bridge_end_age), 30, 100);
    in.debt_annual_payment = std::max(0.0, get_dbl("debt_annual_payment", in.debt_annual_payment));
    in.debt_payoff_age = std::clamp(get_int("debt_payoff_age", in.debt_payoff_age), 30, 110);
    in.ltc_start_age = std::clamp(get_int("ltc_start_age", in.ltc_start_age), 50, 110);
    in.ltc_years = std::clamp(get_int("ltc_years", in.ltc_years), 0, 20);
    in.ltc_annual_cost_real = std::max(0.0, get_dbl("ltc_annual_cost_real", in.ltc_annual_cost_real));
    in.monte_carlo_trials = std::clamp(get_int("monte_carlo_trials", in.monte_carlo_trials), 50, 1000);
    in.has_spouse = get_bool("has_spouse", in.has_spouse);
    in.spouse_current_age = std::clamp(get_int("spouse_current_age", in.spouse_current_age), 18, 95);
    in.spouse_retirement_age = std::clamp(get_int("spouse_retirement_age", in.spouse_retirement_age), 30, 95);
    in.spouse_horizon_age = std::clamp(get_int("spouse_horizon_age", in.spouse_horizon_age), 65, 110);
    in.spouse_ss_fra_annual = std::max(0.0, get_dbl("spouse_ss_fra_annual", in.spouse_ss_fra_annual));
    in.spouse_ss_claim_age = std::clamp(get_int("spouse_ss_claim_age", in.spouse_ss_claim_age), 62, 70);
    in.spouse_optimize_ss_age = get_bool("spouse_optimize_ss_age", in.spouse_optimize_ss_age);
    in.spouse_pension_annual = std::max(0.0, get_dbl("spouse_pension_annual", in.spouse_pension_annual));
    in.spouse_pension_start_age = std::clamp(get_int("spouse_pension_start_age", in.spouse_pension_start_age), 50, 85);
    in.spouse_pension_cola = get_bool("spouse_pension_cola", in.spouse_pension_cola);
    in.spouse_rmd_start_age = std::clamp(get_int("spouse_rmd_start_age", in.spouse_rmd_start_age), 72, 75);
    in.spouse_contrib_tax_deferred = std::max(0.0, get_dbl("spouse_contrib_tax_deferred", in.spouse_contrib_tax_deferred));
    in.spouse_contrib_tax_free = std::max(0.0, get_dbl("spouse_contrib_tax_free", in.spouse_contrib_tax_free));
    in.slow_go_age = std::clamp(get_int("slow_go_age", in.slow_go_age), 50, 110);
    in.slow_go_pct = std::clamp(get_dbl("slow_go_pct", in.slow_go_pct), 10.0, 200.0);
    in.no_go_age = std::clamp(get_int("no_go_age", in.no_go_age), 55, 110);
    in.no_go_pct = std::clamp(get_dbl("no_go_pct", in.no_go_pct), 10.0, 200.0);

    if (g_key_file_has_group(state, STATE_GROUP_EVENTS))
    {
        gsize nkeys = 0;
        gchar** keys = g_key_file_get_keys(state, STATE_GROUP_EVENTS, &nkeys, nullptr);
        for (gsize i = 0; keys && i < nkeys; ++i)
        {
            gchar* val = g_key_file_get_string(state, STATE_GROUP_EVENTS, keys[i], nullptr);
            if (val)
            {
                std::string s(val);
                size_t p1 = s.find('|');
                size_t p2 = (p1 != std::string::npos) ? s.find('|', p1 + 1) : std::string::npos;
                size_t p3 = (p2 != std::string::npos) ? s.find('|', p2 + 1) : std::string::npos;
                if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
                {
                    try
                    {
                        CashflowEvent ev;
                        ev.age = std::clamp(std::stoi(s.substr(0, p1)), 18, 110);
                        ev.amount_real = std::stod(s.substr(p1 + 1, p2 - p1 - 1));
                        ev.bucket = bucket_from_label(s.substr(p2 + 1, p3 - p2 - 1));
                        ev.name = s.substr(p3 + 1);
                        in.events.push_back(std::move(ev));
                    }
                    catch (...) {}
                }
                g_free(val);
            }
        }
        g_strfreev(keys);
    }

    apply_plan_input_to_ui(priv, in);

    // Automatically load the last saved/opened .owlplan file if it still exists
    if (gchar* last_path = g_key_file_get_string(state, STATE_GROUP_PLAN, "last_owlplan_path", nullptr))
    {
        if (last_path[0] != '\0' && g_file_test(last_path, G_FILE_TEST_IS_REGULAR))
        {
            gchar* contents = nullptr;
            gsize length = 0;
            if (g_file_get_contents(last_path, &contents, &length, nullptr))
            {
                PlanInput file_in = in;
                std::vector<std::pair<std::string, TaxBucket>> file_ov;
                std::vector<std::string> file_ex;
                if (deserialize_plan_ini(std::string(contents, length), file_in, file_ov, &file_ex))
                {
                    priv->last_plan_path = last_path;
                    apply_plan_input_to_ui(priv, file_in);
                    apply_account_overrides_to_ui(priv, file_ov, file_ex);
                }
                g_free(contents);
            }
        }
        g_free(last_path);
    }

    int chart_mode = std::clamp(get_int("chart_mode", 0), 0, 4);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_chart_mode), chart_mode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars),
                                 get_bool("real_dollars", true));
    bool show_params = get_bool("show_params", true);
    gtk_toggle_button_set_active(priv->btn_toggle_params, show_params);
    if (priv->left_panel)
        gtk_widget_set_visible(priv->left_panel, show_params);

    if (priv->combo_ai_provider)
        gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ai_provider),
                                 std::clamp(get_int("ai_provider", 0), 0, 2));
    if (priv->entry_ai_endpoint)
    {
        if (gchar* ep = g_key_file_get_string(state, STATE_GROUP_PLAN, "ai_endpoint", nullptr))
        {
            if (ep[0] != '\0')
                gtk_entry_set_text(priv->entry_ai_endpoint, ep);
            g_free(ep);
        }
    }
    if (priv->entry_ai_model)
    {
        if (gchar* md = g_key_file_get_string(state, STATE_GROUP_PLAN, "ai_model", nullptr))
        {
            if (md[0] != '\0')
                gtk_entry_set_text(priv->entry_ai_model, md);
            g_free(md);
        }
    }

    if (g_key_file_has_group(state, STATE_GROUP_ACCTS))
    {
        gsize nkeys = 0;
        gchar** keys = g_key_file_get_keys(state, STATE_GROUP_ACCTS, &nkeys, nullptr);
        std::vector<std::pair<std::string, TaxBucket>> overrides;
        std::vector<std::string> excluded;
        for (gsize i = 0; keys && i < nkeys; ++i)
        {
            gchar* val = g_key_file_get_string(state, STATE_GROUP_ACCTS, keys[i], nullptr);
            if (val)
            {
                std::string s(val);
                if (g_str_has_prefix(keys[i], "acct_"))
                {
                    size_t p1 = s.find('|');
                    size_t p2 = (p1 != std::string::npos) ? s.find('|', p1 + 1) : std::string::npos;
                    if (p1 != std::string::npos && p2 != std::string::npos)
                    {
                        bool inc = (s.substr(0, p1) != "0");
                        TaxBucket tb = bucket_from_label(s.substr(p1 + 1, p2 - p1 - 1));
                        std::string name = s.substr(p2 + 1);
                        overrides.emplace_back(name, tb);
                        if (!inc)
                            excluded.push_back(name);
                    }
                }
                else
                {
                    overrides.emplace_back(keys[i], bucket_from_label(s));
                }
                g_free(val);
            }
        }
        g_strfreev(keys);
        apply_account_overrides_to_ui(priv, overrides, excluded);
    }

    if (g_key_file_has_group(state, STATE_GROUP_EXCLUDED))
    {
        gsize nkeys = 0;
        gchar** keys = g_key_file_get_keys(state, STATE_GROUP_EXCLUDED, &nkeys, nullptr);
        std::vector<std::string> excluded;
        for (gsize i = 0; keys && i < nkeys; ++i)
        {
            if (g_str_has_prefix(keys[i], "excl_"))
            {
                gchar* val = g_key_file_get_string(state, STATE_GROUP_EXCLUDED, keys[i], nullptr);
                if (val)
                {
                    excluded.emplace_back(val);
                    g_free(val);
                }
            }
            else
            {
                GError* err = nullptr;
                gboolean ex = g_key_file_get_boolean(state, STATE_GROUP_EXCLUDED, keys[i], &err);
                if (err)
                    g_error_free(err);
                else if (ex)
                    excluded.emplace_back(keys[i]);
            }
        }
        g_strfreev(keys);
        if (!excluded.empty())
        {
            for (auto& a : priv->accounts)
            {
                for (const auto& ex : excluded)
                {
                    if (a.full_name == ex)
                    {
                        a.included = false;
                        break;
                    }
                }
            }
            sync_acct_store_from_accounts(priv);
        }
    }
}

static void
run_planner(GncPluginPageRetirementPrivate* priv)
{
    PlanInput in = read_plan_input_from_ui(priv);

    priv->last_input = in;
    priv->last_result = optimize_plan(in);
    const PlanResult& res = priv->last_result;

    if (in.objective == PlanObjective::MaximizeSpending)
    {
        gtk_label_set_text(priv->lbl_kpi_primary_title, _("Max Spending / yr"));
        gtk_label_set_text(priv->lbl_kpi_primary_val, fmt_currency(res.sustainable_spending_real).c_str());
    }
    else
    {
        gtk_label_set_text(priv->lbl_kpi_primary_title, _("Projected Bequest"));
        gtk_label_set_text(priv->lbl_kpi_primary_val, fmt_currency(res.final_bequest_real).c_str());
    }

    std::string adv_txt = (res.advantage_real >= 0 ? "+" : "") + fmt_currency(res.advantage_real) + " vs base";
    gtk_label_set_text(priv->lbl_kpi_adv_val, adv_txt.c_str());
    gtk_label_set_text(priv->lbl_kpi_tax_val, fmt_currency(res.lifetime_taxes_real).c_str());

    char mc_buf[128];
    std::snprintf(mc_buf, sizeof(mc_buf), "%.0f%% (P50: %s)",
                  res.monte_carlo.success_rate * 100.0,
                  fmt_currency(res.monte_carlo.p50_end_real).c_str());
    gtk_label_set_text(priv->lbl_kpi_mc_val, mc_buf);

    char rec_buf[200];
    if (in.has_spouse)
    {
        std::snprintf(rec_buf, sizeof(rec_buf), "Recommended: %s  |  SS Claim Ages: %d & %d (Spouse)",
                      strategy_label(res.chosen_strategy), res.chosen_ss_age, res.chosen_spouse_ss_age);
    }
    else
    {
        std::snprintf(rec_buf, sizeof(rec_buf), "Recommended: %s  |  SS Claim Age %d",
                      strategy_label(res.chosen_strategy), res.chosen_ss_age);
    }
    gtk_label_set_text(priv->lbl_kpi_rec_val, rec_buf);

    populate_schedule_and_comparison(priv);

    // Populate Stress Scenarios & 5x5 Sensitivity Matrix
    if (priv->stress_store && priv->sens_store)
    {
        const auto stress = run_sequence_stress_tests(
            in, res.chosen_strategy, res.chosen_ss_age,
            res.sustainable_spending_real, res.chosen_spouse_ss_age);
        gtk_list_store_clear(priv->stress_store);
        for (const auto& sr : stress)
        {
            GtkTreeIter it;
            gtk_list_store_append(priv->stress_store, &it);
            gtk_list_store_set(priv->stress_store, &it,
                               0, sr.name.c_str(),
                               1, sr.feasible ? _("Pass") : _("Shortfall"),
                               2, fmt_currency(sr.min_spending_real).c_str(),
                               3, fmt_currency(sr.final_bequest_real).c_str(),
                               4, sr.shortfall_years,
                               5, sr.feasible ? "#2e7d32" : "#c62828",
                               -1);
        }

        const auto mat = run_sensitivity_matrix(
            in, res.chosen_strategy, res.chosen_ss_age,
            res.sustainable_spending_real, res.chosen_spouse_ss_age);
        const char* pct_lbls[5] = { "85%", "92.5%", "100%", "107.5%", "115%" };
        for (size_t c = 0; c < 5 && c < mat.spending_levels.size(); ++c)
        {
            if (priv->sens_cols[c + 1])
            {
                std::string hdr = std::string(pct_lbls[c]) + " (" + fmt_currency(mat.spending_levels[c]) + ")";
                gtk_tree_view_column_set_title(priv->sens_cols[c + 1], hdr.c_str());
            }
        }
        gtk_list_store_clear(priv->sens_store);
        for (size_t r = 0; r < mat.return_rates.size() && r < mat.cells.size(); ++r)
        {
            char ret_buf[32];
            std::snprintf(ret_buf, sizeof(ret_buf), "%.1f%%", mat.return_rates[r] * 100.0);
            std::string txt[5];
            const char* bg[5] = {};
            for (size_t c = 0; c < 5 && c < mat.cells[r].size(); ++c)
            {
                const auto& cell = mat.cells[r][c];
                if (cell.feasible)
                {
                    txt[c] = fmt_currency(cell.final_bequest_real);
                    bg[c] = (r == 2 && c == 2) ? "#c8e6c9" : "#e8f5e9";
                }
                else
                {
                    txt[c] = "✗ " + std::to_string(cell.shortfall_years) + " yr";
                    bg[c] = "#ffebee";
                }
            }
            GtkTreeIter it;
            gtk_list_store_append(priv->sens_store, &it);
            gtk_list_store_set(priv->sens_store, &it,
                               0, ret_buf,
                               1, txt[0].c_str(), 2, txt[1].c_str(), 3, txt[2].c_str(),
                               4, txt[3].c_str(), 5, txt[4].c_str(),
                               6, bg[0], 7, bg[1], 8, bg[2], 9, bg[3], 10, bg[4],
                               -1);
        }
    }

    if (priv->hist_store)
    {
        auto hist = run_historical_backtests(
            in, res.chosen_strategy, res.chosen_ss_age,
            res.sustainable_spending_real, res.chosen_spouse_ss_age);
        if (priv->lbl_hist_summary)
        {
            char hbuf[128];
            std::snprintf(hbuf, sizeof(hbuf),
                          "Historical Backtests (1928-2024): %.1f%% success across %d rolling cycles",
                          hist.rolling_success_rate * 100.0, hist.total_cycles);
            gtk_label_set_text(priv->lbl_hist_summary, hbuf);
        }
        gtk_list_store_clear(priv->hist_store);
        for (const auto& sc : hist.scenarios)
        {
            GtkTreeIter it;
            gtk_list_store_append(priv->hist_store, &it);
            gtk_list_store_set(priv->hist_store, &it,
                               0, sc.name.c_str(),
                               1, std::to_string(sc.start_year).c_str(),
                               2, sc.feasible ? "✓ Pass" : "✗ Shortfall",
                               3, fmt_currency(sc.min_spending_real).c_str(),
                               4, fmt_currency(sc.final_bequest_real).c_str(),
                               5, std::to_string(sc.shortfall_years).c_str(),
                               6, sc.feasible ? "#e8f5e9" : "#ffebee",
                               -1);
        }
    }

    if (priv->ss_be_store)
    {
        auto be_rows = compute_ss_breakeven(in);
        gtk_list_store_clear(priv->ss_be_store);
        for (const auto& r : be_rows)
        {
            char age_buf[16];
            std::snprintf(age_buf, sizeof(age_buf), "%d%s",
                          r.claim_age, (r.claim_age == res.chosen_ss_age) ? " ★" : "");
            std::string be62 = r.breakeven_vs_62 > 0 ? ("Age " + std::to_string(r.breakeven_vs_62)) : "—";
            std::string be67 = r.breakeven_vs_67 > 0 ? ("Age " + std::to_string(r.breakeven_vs_67)) : "—";
            auto fmt_m = [&](size_t idx) -> std::string {
                return (idx < r.cumulative_at_ages.size()) ? fmt_currency(r.cumulative_at_ages[idx]) : "—";
            };
            GtkTreeIter it;
            gtk_list_store_append(priv->ss_be_store, &it);
            gtk_list_store_set(priv->ss_be_store, &it,
                               0, age_buf,
                               1, fmt_currency(r.annual_benefit_real).c_str(),
                               2, be62.c_str(),
                               3, be67.c_str(),
                               4, fmt_m(0).c_str(), 5, fmt_m(1).c_str(), 6, fmt_m(2).c_str(),
                               7, fmt_m(3).c_str(), 8, fmt_m(4).c_str(), 9, fmt_m(5).c_str(),
                               10, fmt_m(6).c_str(),
                               -1);
        }
    }

    if (priv->chart_area)
        gtk_widget_queue_draw(GTK_WIDGET(priv->chart_area));

    save_state_to_book(priv);
}

static gboolean
on_chart_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    const auto& sched = priv->last_result.schedule;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    const double w = alloc.width;
    const double h = alloc.height;

    GtkStyleContext* sc = gtk_widget_get_style_context(widget);
    GdkRGBA fg_color;
    gtk_style_context_get_color(sc, GTK_STATE_FLAG_NORMAL, &fg_color);

    if (sched.empty() || w < 100 || h < 80)
        return FALSE;

    const ChartMode mode = static_cast<ChartMode>(
        std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_chart_mode)), 0, 4));
    const bool use_real = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars));
    const double inf_rate = priv->last_input.inflation;

    const double pad_l = 72.0, pad_r = 20.0, pad_t = 34.0, pad_b = 36.0;
    const double plot_w = w - pad_l - pad_r;
    const double plot_h = h - pad_t - pad_b;
    if (plot_w <= 10 || plot_h <= 10)
        return FALSE;

    const auto& mc = priv->last_result.monte_carlo;
    auto bar_total_for = [&](size_t i) -> double {
        const auto& yp = sched[i];
        double div = use_real ? std::pow(1.0 + inf_rate, static_cast<double>(i)) : 1.0;
        switch (mode)
        {
            case ChartMode::PortfolioBuckets:
                return yp.end_total_nominal / div;
            case ChartMode::IncomeAndWithdrawals:
                return (yp.ss_income + yp.pension_income + yp.withdraw_taxable +
                        yp.withdraw_tax_deferred + yp.withdraw_tax_free + yp.withdraw_hsa) / div;
            case ChartMode::TaxesRmdAndRoth:
                return (yp.total_tax + yp.rmd_amount + yp.roth_conversion) / div;
            case ChartMode::MonteCarloFan:
            {
                double det = yp.end_total_nominal / div;
                double p90 = (i < mc.p90_real.size())
                    ? (use_real ? mc.p90_real[i] : mc.p90_real[i] * std::pow(1.0 + inf_rate, static_cast<double>(i)))
                    : 0.0;
                return std::max(det, p90);
            }
            case ChartMode::TaxAndHealthcare:
                return (yp.ordinary_tax + yp.ltcg_tax + yp.niit_tax +
                        yp.state_tax + yp.irmaa_surcharge + yp.aca_premium) / div;
        }
        return 0.0;
    };

    double max_val = 1.0;
    for (size_t i = 0; i < sched.size(); ++i)
        max_val = std::max(max_val, bar_total_for(i));

    // Horizontal gridlines
    cairo_set_line_width(cr, 1.0);
    for (int g = 0; g <= 4; ++g)
    {
        double frac = g / 4.0;
        double y = pad_t + plot_h * (1.0 - frac);
        cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.12);
        cairo_move_to(cr, pad_l, y);
        cairo_line_to(cr, pad_l + plot_w, y);
        cairo_stroke(cr);

        cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.75);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.0);
        std::string lbl = fmt_currency(max_val * frac);
        cairo_move_to(cr, 6.0, y + 4.0);
        cairo_show_text(cr, lbl.c_str());
    }

    const size_t n = sched.size();
    if (mode == ChartMode::MonteCarloFan)
    {
        auto x_at = [&](size_t i) -> double {
            return pad_l + (n > 1 ? (static_cast<double>(i) / static_cast<double>(n - 1)) * plot_w : plot_w * 0.5);
        };
        auto y_at = [&](double val) -> double {
            return pad_t + plot_h * (1.0 - std::clamp(val / max_val, 0.0, 1.0));
        };
        auto mc_val = [&](const std::vector<double>& vec, size_t i) -> double {
            if (i >= vec.size()) return 0.0;
            return use_real ? vec[i] : vec[i] * std::pow(1.0 + inf_rate, static_cast<double>(i));
        };

        if (mc.p90_real.size() >= n && mc.p10_real.size() >= n)
        {
            // P10-P90 outer band
            cairo_move_to(cr, x_at(0), y_at(mc_val(mc.p90_real, 0)));
            for (size_t i = 1; i < n; ++i)
                cairo_line_to(cr, x_at(i), y_at(mc_val(mc.p90_real, i)));
            for (size_t i = n; i-- > 0;)
                cairo_line_to(cr, x_at(i), y_at(mc_val(mc.p10_real, i)));
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.16);
            cairo_fill(cr);

            // P25-P75 inner band
            cairo_move_to(cr, x_at(0), y_at(mc_val(mc.p75_real, 0)));
            for (size_t i = 1; i < n; ++i)
                cairo_line_to(cr, x_at(i), y_at(mc_val(mc.p75_real, i)));
            for (size_t i = n; i-- > 0;)
                cairo_line_to(cr, x_at(i), y_at(mc_val(mc.p25_real, i)));
            cairo_close_path(cr);
            cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.28);
            cairo_fill(cr);

            // P50 median line
            cairo_set_source_rgba(cr, 0.15, 0.45, 0.85, 0.95);
            cairo_set_line_width(cr, 2.5);
            cairo_move_to(cr, x_at(0), y_at(mc_val(mc.p50_real, 0)));
            for (size_t i = 1; i < n; ++i)
                cairo_line_to(cr, x_at(i), y_at(mc_val(mc.p50_real, i)));
            cairo_stroke(cr);
        }

        // Deterministic baseline line (dashed)
        double dashes[] = { 6.0, 4.0 };
        cairo_set_dash(cr, dashes, 2, 0.0);
        cairo_set_source_rgba(cr, 0.18, 0.76, 0.49, 0.95);
        cairo_set_line_width(cr, 2.0);
        for (size_t i = 0; i < n; ++i)
        {
            double div = use_real ? std::pow(1.0 + inf_rate, static_cast<double>(i)) : 1.0;
            double y = y_at(sched[i].end_total_nominal / div);
            if (i == 0) cairo_move_to(cr, x_at(i), y);
            else        cairo_line_to(cr, x_at(i), y);
        }
        cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0.0);

        for (size_t i = 0; i < n; ++i)
        {
            if (i == 0 || i == n - 1 || (sched[i].age % 5 == 0))
            {
                cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.78);
                cairo_set_font_size(cr, 11.0);
                char age_buf[16];
                std::snprintf(age_buf, sizeof(age_buf), "%d", sched[i].age);
                cairo_move_to(cr, x_at(i) - 6.0, pad_t + plot_h + 18.0);
                cairo_show_text(cr, age_buf);
            }
        }
    }
    else
    {
        const double bar_step = plot_w / static_cast<double>(n);
        const double bar_w = std::max(2.0, bar_step * 0.72);

        for (size_t i = 0; i < n; ++i)
        {
            const auto& yp = sched[i];
            const double div = use_real ? std::pow(1.0 + inf_rate, static_cast<double>(i)) : 1.0;
            double x = pad_l + i * bar_step + (bar_step - bar_w) * 0.5;
            double y_cur = pad_t + plot_h;

            auto draw_seg = [&](double val, double r, double g, double b) {
                if (val <= 0.0) return;
                double seg_h = ((val / div) / max_val) * plot_h;
                cairo_set_source_rgba(cr, r, g, b, 0.88);
                cairo_rectangle(cr, x, y_cur - seg_h, bar_w, seg_h);
                cairo_fill(cr);
                y_cur -= seg_h;
            };

            if (mode == ChartMode::PortfolioBuckets)
            {
                draw_seg(yp.end_taxable,      0.21, 0.52, 0.89);
                draw_seg(yp.end_tax_deferred, 0.90, 0.65, 0.04);
                draw_seg(yp.end_tax_free,     0.18, 0.76, 0.49);
                draw_seg(yp.end_hsa,          0.57, 0.25, 0.67);
            }
            else if (mode == ChartMode::IncomeAndWithdrawals)
            {
                draw_seg(yp.ss_income + yp.pension_income, 0.45, 0.55, 0.65);
                draw_seg(yp.withdraw_taxable,              0.21, 0.52, 0.89);
                draw_seg(yp.withdraw_tax_deferred,         0.90, 0.65, 0.04);
                draw_seg(yp.withdraw_tax_free + yp.withdraw_hsa, 0.18, 0.76, 0.49);
            }
            else if (mode == ChartMode::TaxesRmdAndRoth)
            {
                draw_seg(yp.ordinary_tax + yp.ltcg_tax + yp.niit_tax, 0.85, 0.22, 0.22);
                draw_seg(yp.state_tax,       0.75, 0.35, 0.55);
                draw_seg(yp.irmaa_surcharge + yp.aca_premium, 0.90, 0.45, 0.10);
                draw_seg(yp.rmd_amount,      0.90, 0.65, 0.04);
                draw_seg(yp.roth_conversion, 0.18, 0.76, 0.49);
            }
            else
            {
                draw_seg(yp.ordinary_tax,               0.85, 0.22, 0.22);
                draw_seg(yp.ltcg_tax + yp.niit_tax,     0.72, 0.20, 0.55);
                draw_seg(yp.state_tax,                  0.42, 0.35, 0.78);
                draw_seg(yp.irmaa_surcharge,            0.90, 0.45, 0.10);
                draw_seg(yp.aca_premium,                0.10, 0.65, 0.68);
            }

            if (i == 0 || i == n - 1 || (yp.age % 5 == 0))
            {
                cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.78);
                cairo_set_font_size(cr, 11.0);
                char age_buf[16];
                std::snprintf(age_buf, sizeof(age_buf), "%d", yp.age);
                cairo_move_to(cr, x, pad_t + plot_h + 18.0);
                cairo_show_text(cr, age_buf);
            }
        }
    }

    struct LegendItem { const char* name; double r, g, b; };
    std::vector<LegendItem> items;
    if (mode == ChartMode::PortfolioBuckets)
    {
        items = {
            { "Taxable",    0.21, 0.52, 0.89 },
            { "401k / IRA", 0.90, 0.65, 0.04 },
            { "Roth",       0.18, 0.76, 0.49 },
            { "HSA",        0.57, 0.25, 0.67 },
        };
    }
    else if (mode == ChartMode::IncomeAndWithdrawals)
    {
        items = {
            { "SS + Pension", 0.45, 0.55, 0.65 },
            { "Taxable Wd",   0.21, 0.52, 0.89 },
            { "401k/IRA Wd",  0.90, 0.65, 0.04 },
            { "Roth/HSA Wd",  0.18, 0.76, 0.49 },
        };
    }
    else if (mode == ChartMode::TaxesRmdAndRoth)
    {
        items = {
            { "Fed Tax",    0.85, 0.22, 0.22 },
            { "State Tax",  0.75, 0.35, 0.55 },
            { "IRMAA/ACA",  0.90, 0.45, 0.10 },
            { "RMD",        0.90, 0.65, 0.04 },
            { "Roth Conv",  0.18, 0.76, 0.49 },
        };
    }
    else if (mode == ChartMode::MonteCarloFan)
    {
        items = {
            { "P10-P90",       0.21, 0.52, 0.89 },
            { "P25-P75",       0.21, 0.52, 0.89 },
            { "Median (P50)",  0.15, 0.45, 0.85 },
            { "Deterministic", 0.18, 0.76, 0.49 },
        };
    }
    else
    {
        items = {
            { "Fed Ord",     0.85, 0.22, 0.22 },
            { "LTCG+NIIT",   0.72, 0.20, 0.55 },
            { "State Tax",   0.42, 0.35, 0.78 },
            { "IRMAA",       0.90, 0.45, 0.10 },
            { "ACA (<65)",   0.10, 0.65, 0.68 },
        };
    }

    double lx = pad_l;
    for (const auto& it : items)
    {
        cairo_set_source_rgba(cr, it.r, it.g, it.b, 0.9);
        cairo_rectangle(cr, lx, 10.0, 12.0, 12.0);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.9);
        cairo_set_font_size(cr, 11.5);
        cairo_move_to(cr, lx + 17.0, 20.0);
        cairo_show_text(cr, it.name);
        lx += 118.0;
    }

    // Hover crosshair & tooltip overlay
    if (priv->chart_hover_idx >= 0 && priv->chart_hover_idx < static_cast<int>(n))
    {
        const size_t hi = static_cast<size_t>(priv->chart_hover_idx);
        const auto& yp = sched[hi];
        const double div = use_real ? std::pow(1.0 + inf_rate, static_cast<double>(hi)) : 1.0;
        const double mult = use_real ? 1.0 : std::pow(1.0 + inf_rate, static_cast<double>(hi));

        double hx = pad_l;
        if (mode == ChartMode::MonteCarloFan)
            hx = pad_l + (n > 1 ? (static_cast<double>(hi) / static_cast<double>(n - 1)) * plot_w : plot_w * 0.5);
        else
            hx = pad_l + (static_cast<double>(hi) + 0.5) * (plot_w / static_cast<double>(n));

        cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.38);
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, hx, pad_t);
        cairo_line_to(cr, hx, pad_t + plot_h);
        cairo_stroke(cr);

        std::vector<std::string> lines;
        char hdr[96];
        if (priv->last_input.has_spouse)
            std::snprintf(hdr, sizeof(hdr), "%d (Age %d / Sp %d)",
                          yp.year, yp.age, priv->last_input.spouse_current_age + static_cast<int>(hi));
        else
            std::snprintf(hdr, sizeof(hdr), "%d (Age %d)", yp.year, yp.age);
        lines.emplace_back(hdr);

        auto add_line = [&](const char* k, double v) {
            lines.push_back(std::string(k) + ": " + fmt_currency(v));
        };

        if (mode == ChartMode::PortfolioBuckets)
        {
            add_line("Total",      yp.end_total_nominal / div);
            add_line("Taxable",    yp.end_taxable / div);
            add_line("401k / IRA", yp.end_tax_deferred / div);
            add_line("Roth",       yp.end_tax_free / div);
            add_line("HSA",        yp.end_hsa / div);
        }
        else if (mode == ChartMode::IncomeAndWithdrawals)
        {
            add_line("SS + Pension", (yp.ss_income + yp.pension_income) / div);
            add_line("Taxable Wd",   yp.withdraw_taxable / div);
            add_line("401k/IRA Wd",  yp.withdraw_tax_deferred / div);
            add_line("Roth/HSA Wd",  (yp.withdraw_tax_free + yp.withdraw_hsa) / div);
            add_line("Net Spend",    yp.net_spending_nominal / div);
        }
        else if (mode == ChartMode::TaxesRmdAndRoth)
        {
            add_line("Fed Tax",   (yp.ordinary_tax + yp.ltcg_tax + yp.niit_tax) / div);
            add_line("State Tax", yp.state_tax / div);
            add_line("IRMAA/ACA", (yp.irmaa_surcharge + yp.aca_premium) / div);
            add_line("RMD",       yp.rmd_amount / div);
            add_line("Roth Conv", yp.roth_conversion / div);
        }
        else if (mode == ChartMode::MonteCarloFan)
        {
            if (hi < mc.p90_real.size())
            {
                add_line("P90",          mc.p90_real[hi] * mult);
                add_line("P75",          mc.p75_real[hi] * mult);
                add_line("Median (P50)", mc.p50_real[hi] * mult);
                add_line("P25",          mc.p25_real[hi] * mult);
                add_line("P10",          mc.p10_real[hi] * mult);
            }
            add_line("Deterministic", yp.end_total_nominal / div);
        }
        else
        {
            add_line("Fed Ord",   yp.ordinary_tax / div);
            add_line("LTCG+NIIT", (yp.ltcg_tax + yp.niit_tax) / div);
            add_line("State Tax", yp.state_tax / div);
            add_line("IRMAA",     yp.irmaa_surcharge / div);
            add_line("ACA (<65)", yp.aca_premium / div);
            add_line("Total Tax", yp.total_tax / div);
        }

        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11.5);
        double max_tw = 0.0;
        for (const auto& ln : lines)
        {
            cairo_text_extents_t ext;
            cairo_text_extents(cr, ln.c_str(), &ext);
            max_tw = std::max(max_tw, ext.width);
        }

        const double line_h = 16.0;
        const double box_w = max_tw + 18.0;
        const double box_h = static_cast<double>(lines.size()) * line_h + 12.0;
        double bx = hx + 12.0;
        if (bx + box_w > w - 8.0)
            bx = hx - box_w - 12.0;
        bx = std::max(8.0, bx);
        double by = std::clamp(priv->chart_hover_y - box_h * 0.5, pad_t, pad_t + plot_h - box_h);

        cairo_set_source_rgba(cr, 0.11, 0.13, 0.16, 0.92);
        cairo_rectangle(cr, bx, by, box_w, box_h);
        cairo_fill(cr);

        cairo_set_source_rgba(cr, 0.55, 0.65, 0.78, 0.65);
        cairo_set_line_width(cr, 1.0);
        cairo_rectangle(cr, bx, by, box_w, box_h);
        cairo_stroke(cr);

        for (size_t li = 0; li < lines.size(); ++li)
        {
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   li == 0 ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_source_rgba(cr, 0.96, 0.97, 0.99, 1.0);
            cairo_move_to(cr, bx + 9.0, by + 18.0 + static_cast<double>(li) * line_h);
            cairo_show_text(cr, lines[li].c_str());
        }
    }

    return FALSE;
}

static gboolean
on_chart_motion(GtkWidget* widget, GdkEventMotion* event, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    const auto& sched = priv->last_result.schedule;
    if (sched.empty())
        return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    const double pad_l = 72.0, pad_r = 20.0;
    const double plot_w = alloc.width - pad_l - pad_r;
    if (plot_w <= 10.0)
        return FALSE;

    int new_idx = -1;
    if (event->x >= pad_l && event->x <= alloc.width - pad_r)
    {
        const ChartMode mode = static_cast<ChartMode>(
            std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_chart_mode)), 0, 4));
        const size_t n = sched.size();
        if (mode == ChartMode::MonteCarloFan && n > 1)
            new_idx = static_cast<int>(std::lround(((event->x - pad_l) / plot_w) * static_cast<double>(n - 1)));
        else
            new_idx = static_cast<int>(std::floor(((event->x - pad_l) / plot_w) * static_cast<double>(n)));
        new_idx = std::clamp(new_idx, 0, static_cast<int>(n) - 1);
    }

    if (new_idx != priv->chart_hover_idx || std::abs(event->y - priv->chart_hover_y) > 4.0)
    {
        priv->chart_hover_idx = new_idx;
        priv->chart_hover_x = event->x;
        priv->chart_hover_y = event->y;
        gtk_widget_queue_draw(widget);
    }
    return FALSE;
}

static gboolean
on_chart_leave(GtkWidget* widget, GdkEventCrossing*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (priv->chart_hover_idx != -1)
    {
        priv->chart_hover_idx = -1;
        gtk_widget_queue_draw(widget);
    }
    return FALSE;
}

static void
on_run_clicked(GtkButton*, gpointer user_data)
{
    run_planner(static_cast<GncPluginPageRetirementPrivate*>(user_data));
}

static void
on_refresh_accounts_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    struct SavedAcct { std::string name; TaxBucket bucket; bool included; };
    std::vector<SavedAcct> saved;
    saved.reserve(priv->accounts.size());
    for (const auto& a : priv->accounts)
        saved.push_back({a.full_name, a.bucket, a.included});

    refresh_accounts_from_book(priv);
    for (auto& a : priv->accounts)
    {
        for (const auto& s : saved)
        {
            if (a.full_name == s.name)
            {
                a.bucket = s.bucket;
                a.included = s.included;
                break;
            }
        }
    }
    sync_acct_store_from_accounts(priv);
    save_state_to_book(priv);
    run_planner(priv);
}

static GtkWindow*
get_parent_window(GncPluginPageRetirementPrivate* priv)
{
    if (!priv || !priv->root_widget)
        return nullptr;
    GtkWidget* top = gtk_widget_get_toplevel(priv->root_widget);
    return GTK_IS_WINDOW(top) ? GTK_WINDOW(top) : nullptr;
}

static void
show_error_dialog(GtkWindow* parent, const char* msg)
{
    GtkWidget* dlg = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                            "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void
show_retirement_help_dialog(GtkWindow* parent)
{
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        _("Retirement Planner (Owl) — Detailed Help"), parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        _("_Close"), GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 820, 620);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 8);
    gtk_box_pack_start(GTK_BOX(content), nb, TRUE, TRUE, 0);

    auto add_help_tab = [&](const char* tab_title, const char* body_text) {
        GtkWidget* tv = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
        gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 12);
        gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 12);
        gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 10);
        gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 10);

        GtkTextBuffer* buf = gtk_text_buffer_new(nullptr);
        gtk_text_buffer_set_text(buf, body_text, -1);
        gtk_text_view_set_buffer(GTK_TEXT_VIEW(tv), buf);
        g_object_unref(buf);

        GtkWidget* scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), tv);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), scroll, gtk_label_new(tab_title));
    };

    add_help_tab(_("Getting Started"),
        "RETIREMENT PLANNER (OWL) — QUICK START GUIDE\n"
        "============================================================\n\n"
        "Note: This is an experimental personal planning module inspired by mdlacasse/Owl.\n"
        "It is not affiliated with official GnuCash or Owl and is not financial/tax advice.\n\n"
        "1. MAP YOUR GNUCASH ACCOUNTS (Account Mapping tab)\n"
        "   • On opening, the planner imports Asset, Bank, Stock, and Mutual Fund balances.\n"
        "   • Accounts are auto-classified into:\n"
        "       - Taxable (brokerage, checking, savings)\n"
        "       - Tax-Deferred (401k, 403b, Traditional IRA, TSP, SEP)\n"
        "       - Tax-Free (Roth IRA, Roth 401k)\n"
        "       - HSA (Health Savings Account)\n"
        "       - Excluded (ignored in projections)\n"
        "   • Use the Include checkboxes, search filter, Select All / Deselect All, or\n"
        "     Auto-Classify to choose which accounts feed your starting balances.\n"
        "   • Your Include selections and bucket overrides persist automatically per book.\n\n"
        "2. CONFIGURE ASSUMPTIONS (Left Panel Tabs)\n"
        "   • Adjust Profile, Income, Market/Tax, Spouse, and Phases & Events.\n"
        "   • Click 'Run Optimization' in the top bar to re-solve.\n\n"
        "3. SAVE, LOAD & EXPORT\n"
        "   • Plan -> Save Plan As... / Load Plan... (*.owlplan / *.ini)\n"
        "     The planner automatically remembers and reloads your last .owlplan file.\n"
        "   • Export CSV... exports the full year-by-year projection table.\n"
        "   • Report (HTML)... writes a self-contained HTML report with KPIs, an SVG\n"
        "     chart, and the schedule table (open in a browser and press Ctrl+P for PDF).\n"
        "   • Press F1 anytime while on this tab to reopen this Help window.\n");

    add_help_tab(_("Plan Options"),
        "LEFT PANEL: PLANNER OPTIONS & ASSUMPTIONS\n"
        "============================================================\n\n"
        "PROFILE TAB\n"
        "  • Current / Retirement / Horizon Age: Defines the accumulation and\n"
        "    retirement projection window.\n"
        "  • Filing Status: Single or Married Filing Jointly (sets federal/state\n"
        "    brackets, standard deductions, LTCG thresholds, NIIT, and IRMAA).\n"
        "  • Objective:\n"
        "      - Maximize Spending: Finds the highest sustainable real annual spending\n"
        "        that meets your Target Bequest with zero shortfalls.\n"
        "      - Maximize Bequest: Holds Target Spend fixed and maximizes after-tax\n"
        "        estate value.\n"
        "  • Strategy: Choose Auto-Optimize (Owl) to search Standard, Tax-Deferred First,\n"
        "    and Roth Conversion bracket-fill (12%, 22%, 24%) strategies.\n\n"
        "INCOME TAB\n"
        "  • Social Security FRA ($/yr) & Claim Age (62–70): Check 'Auto-optimize SS age'\n"
        "    to search 62–70 for the highest objective score.\n"
        "  • Pension ($/yr), Age, & Pension COLA: Toggle COLA for inflation-adjusted vs.\n"
        "    fixed nominal pensions.\n"
        "  • TIPS Floor ($/yr): Optional inflation-adjusted guaranteed income floor.\n"
        "  • Pre-retirement annual contributions to Taxable, 401k/IRA, Roth, and HSA.\n\n"
        "MARKET / TAX TAB\n"
        "  • Expected Return, Volatility, Inflation: Baseline market assumptions.\n"
        "  • Taxable Basis (%): Initial cost-basis percentage for LTCG tax estimation.\n"
        "  • Per-Bucket Return (% 0=auto): Override expected return for Taxable,\n"
        "    401k/IRA, Roth, or HSA independently (0 inherits Expected Return).\n"
        "  • RMD Start Age (72–75) & State Tax (50 states + DC).\n"
        "  • Model ACA Marketplace (<65) & 400% FPL Cliff: Pre-Medicare subsidy modeling.\n"
        "  • Re-invest RMD Surplus in Taxable: Re-invests excess after-tax RMD cash.\n"
        "  • Guyton-Klinger Guardrails (+/-10%): Dynamically trims/boosts spending when\n"
        "    withdrawal rate crosses 6.0% / 4.0% guardrails.\n"
        "  • Use Equity/Bond Glide Path: Blends Pre-Retire and In-Retire Equity (%)\n"
        "    allocations to drive return and Monte Carlo volatility.\n");

    add_help_tab(_("Spouse, Phases & Events"),
        "SPOUSE, SPENDING PHASES, DEBT, LTC & EVENTS\n"
        "============================================================\n\n"
        "SPOUSE TAB\n"
        "  • Enable 'Include Spouse / Partner' for two-person household modeling.\n"
        "  • Set spouse ages, independent SS FRA & claiming age (with joint SS\n"
        "    optimization), spouse pension + COLA, RMD age, and contributions.\n"
        "  • Survivor Benefit: When one spouse reaches their horizon age first, the\n"
        "    surviving spouse keeps the higher of the two Social Security benefits\n"
        "    and transitions to Single tax brackets.\n\n"
        "PHASES & EVENTS TAB\n"
        "  • Go-Go / Slow-Go / No-Go Phases: Scale base retirement spending at\n"
        "    Slow-Go Age (e.g. 85% at 75) and No-Go Age (e.g. 70% at 85).\n"
        "  • Fixed Debt / Mortgage ($/yr) & Payoff Age: Models a fixed nominal\n"
        "    payment (not inflated by CPI) that ends when you reach Payoff Age.\n"
        "  • LTC Start Age, Duration (yrs), & Cost ($/yr): Models a late-life\n"
        "    Long-Term Care expense window in today's dollars.\n"
        "  • One-Time Events: Add lump-sum windfalls (+) such as home downsizing or\n"
        "    inheritances, or lump-sum expenses (-) routed to a specific tax bucket.\n");

    add_help_tab(_("Charts, Stress & SS Break-Even"),
        "RIGHT PANEL: CHARTS, SCHEDULES & STRESS TESTING\n"
        "============================================================\n\n"
        "OVERVIEW & CHART (5 MODES + HOVER TOOLTIPS)\n"
        "  • Move your mouse over any chart to inspect exact year-by-year values.\n"
        "  • Modes:\n"
        "      1. Portfolio by Tax Bucket (Total, Taxable, 401k/IRA, Roth, HSA)\n"
        "      2. Income & Withdrawals (SS, Pension, bucket draws, net spending)\n"
        "      3. Taxes, RMDs & Roth Conv\n"
        "      4. Monte Carlo Fan (P10 / P25 / P50 / P75 / P90 percentiles)\n"
        "      5. Tax & Healthcare Breakdown (Federal Ord, LTCG, NIIT, State, IRMAA, ACA)\n\n"
        "YEAR-BY-YEAR SCHEDULE\n"
        "  • Toggle 'Today's $' to switch between real (inflation-adjusted) and nominal.\n"
        "  • Includes Floor Gap (guaranteed income minus target spend), RMD Reinv,\n"
        "    and Bracket / IRMAA (marginal federal bracket % and Medicare IRMAA tier).\n\n"
        "STRESS & SENSITIVITY\n"
        "  • Sequence-of-Returns Stress Tests: Early Crash, Lost Decade, Stagflation.\n"
        "  • 5x5 Return vs. Spending Sensitivity Matrix: Color-coded feasibility grid.\n"
        "  • Historical Market Backtests (1928–2024): Tests 97 rolling historical\n"
        "    market cycles plus 1929, 1966, 1973, 2000, and 2008 crisis starts.\n\n"
        "SS BREAK-EVEN\n"
        "  • Compares Social Security claiming ages 62–70, showing annual benefit,\n"
        "    break-even age vs. 62 and vs. 67, and cumulative real dollars at ages 65–95.\n");

    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void
on_retirement_help_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    show_retirement_help_dialog(get_parent_window(priv));
}

static void
on_page_f1_help(GncPluginPage* page)
{
    auto* priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(GNC_PLUGIN_PAGE_RETIREMENT(page));
    show_retirement_help_dialog(get_parent_window(priv));
}

struct ExpenseSumCtx
{
    time64 start_t = 0;
    time64 end_t = 0;
    double total = 0.0;
};

static void
sum_expense_cb(Account* acct, gpointer data)
{
    auto* ctx = static_cast<ExpenseSumCtx*>(data);
    if (!acct || xaccAccountGetType(acct) != ACCT_TYPE_EXPENSE)
        return;
    gnc_numeric chg = xaccAccountGetNoclosingBalanceChangeInCurrencyForPeriod(
        acct, ctx->start_t, ctx->end_t, FALSE);
    ctx->total += gnc_numeric_to_double(chg);
}

static void
on_estimate_spending_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    QofBook* book = gnc_get_current_book();
    if (!book)
        return;
    Account* root = gnc_book_get_root_account(book);
    if (!root)
        return;

    ExpenseSumCtx ctx;
    ctx.end_t = gnc_time(nullptr);
    ctx.start_t = ctx.end_t - static_cast<time64>(365) * 86400;
    gnc_account_foreach_descendant(root, sum_expense_cb, &ctx);

    double annual = std::round(std::max(0.0, ctx.total) / 100.0) * 100.0;
    if (annual > 0.0)
    {
        gtk_spin_button_set_value(priv->spin_target_spend, annual);
        run_planner(priv);
    }
    else
    {
        show_error_dialog(get_parent_window(priv),
                          _("No trailing 12-month Expense transactions found in this book."));
    }
}

static void
on_pin_snapshot_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    priv->snap_input = priv->last_input;
    priv->snap_result = priv->last_result;
    priv->has_snapshot = true;
    populate_schedule_and_comparison(priv);
}

static void
on_clear_snapshot_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    priv->has_snapshot = false;
    populate_schedule_and_comparison(priv);
}

static void
set_ai_response_text(GncPluginPageRetirementPrivate* priv, const std::string& text)
{
    if (!priv->tv_ai_response)
        return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(priv->tv_ai_response);
    gtk_text_buffer_set_text(buf, text.c_str(), -1);
}

static void
on_ai_provider_changed(GtkComboBox*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider));
    if (idx == 0)
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "http://localhost:11434/api/generate");
        gtk_entry_set_text(priv->entry_ai_model, "llama3.2");
    }
    else if (idx == 1)
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "https://generativelanguage.googleapis.com/v1beta");
        gtk_entry_set_text(priv->entry_ai_model, "gemini-2.5-flash");
    }
    else
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "https://api.openai.com/v1/chat/completions");
        gtk_entry_set_text(priv->entry_ai_model, "gpt-4o-mini");
    }
}

struct AiCallCtx
{
    GncPluginPageRetirementPrivate* priv = nullptr;
    AiProvider provider = AiProvider::Ollama;
};

static void
on_ai_http_finished(GObject* source, GAsyncResult* res, gpointer user_data)
{
    std::unique_ptr<AiCallCtx> ctx(static_cast<AiCallCtx*>(user_data));
    GncPluginPageRetirementPrivate* priv = ctx->priv;
    GError* err = nullptr;
    GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);

    if (!priv || !priv->root_widget)
    {
        if (bytes) g_bytes_unref(bytes);
        if (err) g_error_free(err);
        return;
    }

    if (!bytes)
    {
        std::string msg = std::string("Request failed: ") + (err ? err->message : "Unknown network error");
        if (err) g_error_free(err);
        set_ai_response_text(priv, msg);
        if (priv->lbl_ai_status)
            gtk_label_set_text(priv->lbl_ai_status, _("Error contacting AI provider."));
        return;
    }

    gsize len = 0;
    const char* raw = static_cast<const char*>(g_bytes_get_data(bytes, &len));
    std::string resp_str(raw ? raw : "", len);
    g_bytes_unref(bytes);

    std::string reply = extract_ai_response_text(ctx->provider, resp_str);
    set_ai_response_text(priv, reply);

    PlanInput suggested = priv->last_input;
    std::vector<std::string> changes;
    if (parse_ai_whatif_json(reply, suggested, changes) && !changes.empty())
    {
        priv->ai_suggested_input = suggested;
        priv->has_ai_suggestion = true;
        std::string summary = "Detected What-If changes: ";
        for (size_t i = 0; i < changes.size(); ++i)
        {
            if (i > 0) summary += ", ";
            summary += changes[i];
        }
        if (priv->lbl_ai_status)
            gtk_label_set_text(priv->lbl_ai_status, summary.c_str());
        if (priv->btn_ai_apply)
            gtk_widget_set_sensitive(priv->btn_ai_apply, TRUE);
    }
    else
    {
        priv->has_ai_suggestion = false;
        if (priv->lbl_ai_status)
            gtk_label_set_text(priv->lbl_ai_status, _("Ready."));
        if (priv->btn_ai_apply)
            gtk_widget_set_sensitive(priv->btn_ai_apply, FALSE);
    }
}

static void
send_ai_prompt(GncPluginPageRetirementPrivate* priv, const std::string& user_question)
{
    if (!priv->soup_session)
        priv->soup_session = soup_session_new();

    int p_idx = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)), 0, 2);
    auto provider = static_cast<AiProvider>(p_idx);
    std::string endpoint = gtk_entry_get_text(priv->entry_ai_endpoint);
    std::string model = gtk_entry_get_text(priv->entry_ai_model);
    std::string api_key = gtk_entry_get_text(priv->entry_ai_key);
    if (api_key.empty())
    {
        const char* env_var = (provider == AiProvider::Gemini) ? g_getenv("GEMINI_API_KEY") : g_getenv("OPENAI_API_KEY");
        if (env_var) api_key = env_var;
    }

    if (provider != AiProvider::Ollama && api_key.empty())
    {
        set_ai_response_text(priv, "Please enter an API key (or set GEMINI_API_KEY / OPENAI_API_KEY), or switch Provider to Local Ollama (Offline).");
        return;
    }

    std::string url = endpoint;
    if (provider == AiProvider::Gemini && endpoint.find(":generateContent") == std::string::npos)
    {
        if (!url.empty() && url.back() == '/') url.pop_back();
        url += "/models/" + model + ":generateContent";
    }

    std::string sys_prompt =
        "You are an expert retirement financial planner advising on an Owl-inspired retirement projection. "
        "Be concise, practical, and clear (not formal legal/tax advice). "
        "Whenever the user asks a What-If question or you recommend parameter changes, include a ```json { ... } ``` block "
        "with only the changed numeric/boolean fields from: retirement_age, horizon_age, ss_claim_age, target_annual_spending, "
        "target_bequest, bridge_income_annual, bridge_start_age, bridge_end_age, enable_guardrails, enable_glide_path, equity_pct_in_ret, ltc_years.";
    std::string full_prompt = build_ai_plan_summary(priv->last_input, priv->last_result)
                            + "\n=== User Question ===\n" + user_question;
    std::string body = build_ai_request_json(provider, model, sys_prompt, full_prompt);

    SoupMessage* msg = soup_message_new("POST", url.c_str());
    if (!msg)
    {
        set_ai_response_text(priv, "Invalid endpoint URL: " + url);
        return;
    }

    if (provider == AiProvider::Gemini)
        soup_message_headers_append(soup_message_get_request_headers(msg), "x-goog-api-key", api_key.c_str());
    else if (provider == AiProvider::OpenAICompatible && !api_key.empty())
    {
        std::string auth = "Bearer " + api_key;
        soup_message_headers_append(soup_message_get_request_headers(msg), "Authorization", auth.c_str());
    }

    GBytes* req_bytes = g_bytes_new(body.data(), body.size());
    soup_message_set_request_body_from_bytes(msg, "application/json", req_bytes);
    g_bytes_unref(req_bytes);

    priv->has_ai_suggestion = false;
    if (priv->btn_ai_apply)
        gtk_widget_set_sensitive(priv->btn_ai_apply, FALSE);
    if (priv->lbl_ai_status)
        gtk_label_set_text(priv->lbl_ai_status, _("Contacting AI advisor..."));
    set_ai_response_text(priv, "Thinking...");

    auto* ctx = new AiCallCtx{priv, provider};
    soup_session_send_and_read_async(priv->soup_session, msg, G_PRIORITY_DEFAULT, nullptr,
                                     on_ai_http_finished, ctx);
    g_object_unref(msg);
}

static void
on_ai_critique_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    send_ai_prompt(priv, "Critique my current retirement plan: highlight strengths, IRMAA/ACA cliff or sequence-of-returns risks, and suggest 1-2 concrete What-If improvements.");
}

static void
on_ai_ask_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    std::string q = priv->entry_ai_question ? gtk_entry_get_text(priv->entry_ai_question) : "";
    if (q.empty())
        q = "What is the biggest risk in my plan and how can I improve it?";
    send_ai_prompt(priv, q);
}

static void
on_ai_copy_summary_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    std::string summary = build_ai_plan_summary(priv->last_input, priv->last_result);
    GtkClipboard* cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, summary.c_str(), -1);
    if (priv->lbl_ai_status)
        gtk_label_set_text(priv->lbl_ai_status, _("Copied anonymized summary to clipboard."));
}

static void
on_ai_apply_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (!priv->has_ai_suggestion)
        return;
    apply_plan_input_to_ui(priv, priv->ai_suggested_input);
    save_state_to_book(priv);
    run_planner(priv);
    priv->has_ai_suggestion = false;
    if (priv->btn_ai_apply)
        gtk_widget_set_sensitive(priv->btn_ai_apply, FALSE);
    if (priv->lbl_ai_status)
        gtk_label_set_text(priv->lbl_ai_status, _("Applied AI What-If parameters and re-optimized."));
}

static void
on_export_csv_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkWindow* parent = get_parent_window(priv);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Export Retirement Schedule to CSV"), parent,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_Save"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "retirement-schedule.csv");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("CSV files (*.csv)"));
    gtk_file_filter_add_pattern(filter, "*.csv");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        const bool use_real = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars));
        std::string csv = export_schedule_csv(priv->last_input, priv->last_result, use_real);
        GError* err = nullptr;
        if (!g_file_set_contents(filename, csv.c_str(), static_cast<gssize>(csv.size()), &err))
        {
            show_error_dialog(parent, err ? err->message : _("Failed to write CSV file."));
            if (err) g_error_free(err);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void
on_export_html_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkWindow* parent = get_parent_window(priv);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Export Retirement Report (HTML / Print to PDF)"), parent,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_Save"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "retirement-report.html");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("HTML files (*.html)"));
    gtk_file_filter_add_pattern(filter, "*.html");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        std::string html = export_report_html(priv->last_input, priv->last_result);
        GError* err = nullptr;
        if (!g_file_set_contents(filename, html.c_str(), static_cast<gssize>(html.size()), &err))
        {
            show_error_dialog(parent, err ? err->message : _("Failed to write HTML report."));
            if (err) g_error_free(err);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void
on_save_plan_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkWindow* parent = get_parent_window(priv);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Save Retirement Plan"), parent,
        GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_Save"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    if (!priv->last_plan_path.empty())
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dlg), priv->last_plan_path.c_str());
    else
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "retirement-plan.owlplan");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("Owl Plan files (*.owlplan, *.ini)"));
    gtk_file_filter_add_pattern(filter, "*.owlplan");
    gtk_file_filter_add_pattern(filter, "*.ini");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        std::string ini = serialize_plan_ini(read_plan_input_from_ui(priv),
                                             collect_account_overrides(priv),
                                             collect_excluded_accounts(priv));
        GError* err = nullptr;
        if (!g_file_set_contents(filename, ini.c_str(), static_cast<gssize>(ini.size()), &err))
        {
            show_error_dialog(parent, err ? err->message : _("Failed to save plan file."));
            if (err) g_error_free(err);
        }
        else
        {
            priv->last_plan_path = filename;
            save_state_to_book(priv);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void
on_load_plan_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkWindow* parent = get_parent_window(priv);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Load Retirement Plan"), parent,
        GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_Open"), GTK_RESPONSE_ACCEPT, nullptr);

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, _("Owl Plan files (*.owlplan, *.ini)"));
    gtk_file_filter_add_pattern(filter, "*.owlplan");
    gtk_file_filter_add_pattern(filter, "*.ini");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gchar* contents = nullptr;
        gsize length = 0;
        GError* err = nullptr;
        if (g_file_get_contents(filename, &contents, &length, &err))
        {
            PlanInput loaded = read_plan_input_from_ui(priv);
            std::vector<std::pair<std::string, TaxBucket>> overrides;
            std::vector<std::string> excluded;
            if (deserialize_plan_ini(std::string(contents, length), loaded, overrides, &excluded))
            {
                priv->last_plan_path = filename;
                apply_plan_input_to_ui(priv, loaded);
                apply_account_overrides_to_ui(priv, overrides, excluded);
                save_state_to_book(priv);
                run_planner(priv);
            }
            else
            {
                show_error_dialog(parent, _("Selected file is not a valid retirement plan."));
            }
            g_free(contents);
        }
        else
        {
            show_error_dialog(parent, err ? err->message : _("Failed to read plan file."));
            if (err) g_error_free(err);
        }
        g_free(filename);
    }
    gtk_widget_destroy(dlg);
}

static void
on_add_event_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkWindow* parent = get_parent_window(priv);
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        _("Add One-Time Cashflow Event"), parent,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("_Add"), GTK_RESPONSE_ACCEPT, nullptr);

    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkGrid* grid = GTK_GRID(gtk_grid_new());
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(grid, 6);
    gtk_grid_set_column_spacing(grid, 8);
    gtk_box_pack_start(GTK_BOX(area), GTK_WIDGET(grid), TRUE, TRUE, 0);

    GtkWidget* ent_name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(ent_name), _("Home Sale / Mortgage Payoff"));
    gtk_grid_attach(grid, gtk_label_new(_("Event Name")), 0, 0, 1, 1);
    gtk_grid_attach(grid, ent_name, 1, 0, 1, 1);

    GtkWidget* sp_age = gtk_spin_button_new_with_range(18, 110, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_age), 65);
    gtk_grid_attach(grid, gtk_label_new(_("At Primary Age")), 0, 1, 1, 1);
    gtk_grid_attach(grid, sp_age, 1, 1, 1, 1);

    GtkWidget* sp_amt = gtk_spin_button_new_with_range(-5000000, 5000000, 5000);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp_amt), 100000);
    gtk_grid_attach(grid, gtk_label_new(_("Amount ($ today, +/-)")), 0, 2, 1, 1);
    gtk_grid_attach(grid, sp_amt, 1, 2, 1, 1);

    GtkWidget* cb_bkt = gtk_combo_box_text_new();
    for (int b = 0; b <= static_cast<int>(TaxBucket::HSA); ++b)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cb_bkt), bucket_label(static_cast<TaxBucket>(b)));
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb_bkt), 0);
    gtk_grid_attach(grid, gtk_label_new(_("Account Bucket")), 0, 3, 1, 1);
    gtk_grid_attach(grid, cb_bkt, 1, 3, 1, 1);

    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        CashflowEvent ev;
        const gchar* txt = gtk_entry_get_text(GTK_ENTRY(ent_name));
        ev.name = (txt && *txt) ? txt : _("Event");
        ev.age = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(sp_age));
        ev.amount_real = gtk_spin_button_get_value(GTK_SPIN_BUTTON(sp_amt));
        ev.bucket = static_cast<TaxBucket>(std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(cb_bkt)), 0, 3));
        priv->events.push_back(std::move(ev));
        sync_event_store(priv);
        run_planner(priv);
    }
    gtk_widget_destroy(dlg);
}

static void
on_remove_event_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (!priv->event_tv)
        return;
    GtkTreeSelection* sel = gtk_tree_view_get_selection(priv->event_tv);
    GtkTreeIter iter;
    GtkTreeModel* model = nullptr;
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
    {
        GtkTreePath* path = gtk_tree_model_get_path(model, &iter);
        int* indices = gtk_tree_path_get_indices(path);
        int idx = indices ? indices[0] : -1;
        gtk_tree_path_free(path);
        if (idx >= 0 && idx < static_cast<int>(priv->events.size()))
        {
            priv->events.erase(priv->events.begin() + idx);
            sync_event_store(priv);
            run_planner(priv);
        }
    }
}

static void
on_reset_defaults_clicked(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    apply_plan_input_to_ui(priv, PlanInput{});
    refresh_accounts_from_book(priv);
    run_planner(priv);
}

static void
on_toggle_params(GtkToggleButton* btn, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (priv->left_panel)
        gtk_widget_set_visible(priv->left_panel, gtk_toggle_button_get_active(btn));
}

static void
on_display_option_changed(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    populate_schedule_and_comparison(priv);
    if (priv->chart_area)
        gtk_widget_queue_draw(GTK_WIDGET(priv->chart_area));
    save_state_to_book(priv);
}

static gboolean
acct_filter_visible_func(GtkTreeModel* model, GtkTreeIter* iter, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (!priv->entry_acct_filter)
        return TRUE;
    const gchar* query = gtk_entry_get_text(GTK_ENTRY(priv->entry_acct_filter));
    if (!query || !*query)
        return TRUE;

    gchar* name = nullptr;
    gchar* bucket = nullptr;
    gtk_tree_model_get(model, iter, ACCT_COL_NAME, &name, ACCT_COL_BUCKET_STR, &bucket, -1);

    gchar* q_lc = g_utf8_strdown(query, -1);
    gchar* n_lc = name ? g_utf8_strdown(name, -1) : g_strdup("");
    gchar* b_lc = bucket ? g_utf8_strdown(bucket, -1) : g_strdup("");
    gboolean match = (std::strstr(n_lc, q_lc) != nullptr) || (std::strstr(b_lc, q_lc) != nullptr);

    g_free(q_lc);
    g_free(n_lc);
    g_free(b_lc);
    g_free(name);
    g_free(bucket);
    return match;
}

static void
on_acct_filter_changed(GtkSearchEntry*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    if (priv->acct_filter)
        gtk_tree_model_filter_refilter(priv->acct_filter);
}

static void
on_account_include_toggled(GtkCellRendererToggle*, gchar* path_str, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkTreePath* filt_path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter filt_iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->acct_filter), &filt_iter, filt_path))
    {
        GtkTreeIter child_iter;
        gtk_tree_model_filter_convert_iter_to_child_iter(priv->acct_filter, &child_iter, &filt_iter);
        int orig_idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(priv->acct_store), &child_iter,
                           ACCT_COL_ORIG_IDX, &orig_idx, -1);
        if (orig_idx >= 0 && orig_idx < static_cast<int>(priv->accounts.size()))
        {
            auto& ma = priv->accounts[static_cast<size_t>(orig_idx)];
            ma.included = !ma.included;
            if (ma.included && ma.bucket == TaxBucket::Excluded)
                ma.bucket = TaxBucket::Taxable;
            gtk_list_store_set(priv->acct_store, &child_iter,
                               ACCT_COL_INCLUDE, ma.included ? TRUE : FALSE,
                               ACCT_COL_BUCKET_STR, bucket_label(ma.bucket),
                               ACCT_COL_BUCKET_ID, static_cast<int>(ma.bucket),
                               -1);
            update_bucket_summary_label(priv);
            save_state_to_book(priv);
            run_planner(priv);
        }
    }
    gtk_tree_path_free(filt_path);
}

static void
set_visible_accounts_included(GncPluginPageRetirementPrivate* priv, bool include)
{
    GtkTreeIter filt_iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->acct_filter), &filt_iter))
    {
        do
        {
            GtkTreeIter child_iter;
            gtk_tree_model_filter_convert_iter_to_child_iter(priv->acct_filter, &child_iter, &filt_iter);
            int orig_idx = -1;
            gtk_tree_model_get(GTK_TREE_MODEL(priv->acct_store), &child_iter,
                               ACCT_COL_ORIG_IDX, &orig_idx, -1);
            if (orig_idx >= 0 && orig_idx < static_cast<int>(priv->accounts.size()))
            {
                auto& ma = priv->accounts[static_cast<size_t>(orig_idx)];
                ma.included = include;
                if (include && ma.bucket == TaxBucket::Excluded)
                    ma.bucket = TaxBucket::Taxable;
                gtk_list_store_set(priv->acct_store, &child_iter,
                                   ACCT_COL_INCLUDE, ma.included ? TRUE : FALSE,
                                   ACCT_COL_BUCKET_STR, bucket_label(ma.bucket),
                                   ACCT_COL_BUCKET_ID, static_cast<int>(ma.bucket),
                                   -1);
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->acct_filter), &filt_iter));
    }
    update_bucket_summary_label(priv);
    save_state_to_book(priv);
    run_planner(priv);
}

static void
on_select_all_accounts(GtkButton*, gpointer user_data)
{
    set_visible_accounts_included(static_cast<GncPluginPageRetirementPrivate*>(user_data), true);
}

static void
on_deselect_all_accounts(GtkButton*, gpointer user_data)
{
    set_visible_accounts_included(static_cast<GncPluginPageRetirementPrivate*>(user_data), false);
}

static void
on_auto_classify_accounts(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    for (auto& ma : priv->accounts)
    {
        ma.bucket = classify_account_name(ma.full_name);
        ma.included = (ma.bucket != TaxBucket::Excluded);
    }
    sync_acct_store_from_accounts(priv);
    save_state_to_book(priv);
    run_planner(priv);
}

static void
on_bucket_edited(GtkCellRendererText*, gchar* path_str, gchar* new_text, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkTreePath* filt_path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter filt_iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->acct_filter), &filt_iter, filt_path))
    {
        GtkTreeIter child_iter;
        gtk_tree_model_filter_convert_iter_to_child_iter(priv->acct_filter, &child_iter, &filt_iter);
        int orig_idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(priv->acct_store), &child_iter,
                           ACCT_COL_ORIG_IDX, &orig_idx, -1);
        TaxBucket chosen = TaxBucket::Taxable;
        for (int b = 0; b <= static_cast<int>(TaxBucket::Excluded); ++b)
        {
            auto tb = static_cast<TaxBucket>(b);
            if (g_strcmp0(new_text, bucket_label(tb)) == 0)
            {
                chosen = tb;
                break;
            }
        }
        bool inc = (chosen != TaxBucket::Excluded);
        if (orig_idx >= 0 && orig_idx < static_cast<int>(priv->accounts.size()))
        {
            priv->accounts[static_cast<size_t>(orig_idx)].bucket = chosen;
            priv->accounts[static_cast<size_t>(orig_idx)].included = inc;
        }

        gtk_list_store_set(priv->acct_store, &child_iter,
                           ACCT_COL_INCLUDE, inc ? TRUE : FALSE,
                           ACCT_COL_BUCKET_STR, bucket_label(chosen),
                           ACCT_COL_BUCKET_ID, static_cast<int>(chosen),
                           -1);
        update_bucket_summary_label(priv);
        save_state_to_book(priv);
        run_planner(priv);
    }
    gtk_tree_path_free(filt_path);
}

static GtkSpinButton*
add_spin_row(GtkGrid* grid, int& row, const char* label_txt,
             double min_v, double max_v, double step, double init_v, int digits = 0)
{
    GtkWidget* lbl = gtk_label_new(label_txt);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    GtkWidget* spin = gtk_spin_button_new_with_range(min_v, max_v, step);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), digits);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), init_v);
    gtk_widget_set_hexpand(spin, TRUE);
    gtk_grid_attach(grid, lbl, 0, row, 1, 1);
    gtk_grid_attach(grid, spin, 1, row, 1, 1);
    ++row;
    return GTK_SPIN_BUTTON(spin);
}

static GtkComboBoxText*
add_combo_row(GtkGrid* grid, int& row, const char* lbl_txt,
              std::initializer_list<const char*> items, int active)
{
    GtkWidget* lbl = gtk_label_new(lbl_txt);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkComboBoxText* cb = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (const char* it : items)
        gtk_combo_box_text_append_text(cb, it);
    gtk_combo_box_set_active(GTK_COMBO_BOX(cb), active);
    gtk_widget_set_hexpand(GTK_WIDGET(cb), TRUE);
    gtk_grid_attach(grid, lbl, 0, row, 1, 1);
    gtk_grid_attach(grid, GTK_WIDGET(cb), 1, row, 1, 1);
    ++row;
    return cb;
}

static GtkWidget*
make_kpi_card(const char* title, GtkLabel** out_title_lbl, GtkLabel** out_val_lbl)
{
    GtkWidget* frame = gtk_frame_new(nullptr);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_container_add(GTK_CONTAINER(frame), box);

    GtkWidget* t = gtk_label_new(title);
    gtk_widget_set_halign(t, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(t), "gnc-class-lighter-grey-mix");

    GtkWidget* v = gtk_label_new("—");
    gtk_widget_set_halign(v, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(v), "gnc-class-strong");

    gtk_box_pack_start(GTK_BOX(box), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), v, FALSE, FALSE, 0);
    if (out_title_lbl) *out_title_lbl = GTK_LABEL(t);
    if (out_val_lbl) *out_val_lbl = GTK_LABEL(v);
    return frame;
}

static GtkWidget*
gnc_plugin_page_retirement_create_widget(GncPluginPage* plugin_page)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);

    if (priv->root_widget)
        return priv->root_widget;

    GtkWidget* outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    // Top display options & actions bar (compact, horizontally scrollable if narrow)
    GtkWidget* top_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(top_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    GtkWidget* top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(top_scroll), top_bar);
    gtk_box_pack_start(GTK_BOX(outer), top_scroll, FALSE, FALSE, 0);

    priv->btn_toggle_params = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("Planner Options")));
    gtk_toggle_button_set_active(priv->btn_toggle_params, TRUE);
    g_signal_connect(priv->btn_toggle_params, "toggled", G_CALLBACK(on_toggle_params), priv);
    gtk_box_pack_start(GTK_BOX(top_bar), GTK_WIDGET(priv->btn_toggle_params), FALSE, FALSE, 0);

    GtkWidget* lbl_chart = gtk_label_new(_("Chart:"));
    gtk_box_pack_start(GTK_BOX(top_bar), lbl_chart, FALSE, FALSE, 2);

    priv->combo_chart_mode = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(priv->combo_chart_mode, _("Portfolio by Tax Bucket"));
    gtk_combo_box_text_append_text(priv->combo_chart_mode, _("Income & Withdrawals"));
    gtk_combo_box_text_append_text(priv->combo_chart_mode, _("Taxes, RMDs & Roth Conv"));
    gtk_combo_box_text_append_text(priv->combo_chart_mode, _("Monte Carlo Fan (P10-P90)"));
    gtk_combo_box_text_append_text(priv->combo_chart_mode, _("Tax & Healthcare Breakdown"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_chart_mode), 0);
    g_signal_connect(priv->combo_chart_mode, "changed", G_CALLBACK(on_display_option_changed), priv);
    gtk_box_pack_start(GTK_BOX(top_bar), GTK_WIDGET(priv->combo_chart_mode), FALSE, FALSE, 0);

    priv->chk_real_dollars = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Today's $ (Real)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars), TRUE);
    g_signal_connect(priv->chk_real_dollars, "toggled", G_CALLBACK(on_display_option_changed), priv);
    gtk_box_pack_start(GTK_BOX(top_bar), GTK_WIDGET(priv->chk_real_dollars), FALSE, FALSE, 0);

    GtkWidget* btn_run = gtk_button_new_with_label(_("Run Optimization"));
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_run), "suggested-action");
    g_signal_connect(btn_run, "clicked", G_CALLBACK(on_run_clicked), priv);
    gtk_box_pack_end(GTK_BOX(top_bar), btn_run, FALSE, FALSE, 0);

    GtkWidget* btn_refresh = gtk_button_new_with_label(_("Refresh Accounts"));
    g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh_accounts_clicked), priv);
    gtk_box_pack_end(GTK_BOX(top_bar), btn_refresh, FALSE, FALSE, 0);

    GtkWidget* btn_help = gtk_button_new_with_label(_("Help (F1)"));
    g_signal_connect(btn_help, "clicked", G_CALLBACK(on_retirement_help_clicked), priv);
    gtk_box_pack_end(GTK_BOX(top_bar), btn_help, FALSE, FALSE, 0);

    GtkWidget* plan_menu = gtk_menu_new();
    GtkWidget* mi_save = gtk_menu_item_new_with_label(_("Save Plan As..."));
    GtkWidget* mi_load = gtk_menu_item_new_with_label(_("Load Plan..."));
    GtkWidget* mi_sep1 = gtk_separator_menu_item_new();
    GtkWidget* mi_exp_csv = gtk_menu_item_new_with_label(_("Export CSV..."));
    GtkWidget* mi_exp_html = gtk_menu_item_new_with_label(_("Export HTML Report..."));
    GtkWidget* mi_sep2 = gtk_separator_menu_item_new();
    GtkWidget* mi_pin_snap = gtk_menu_item_new_with_label(_("Pin Snapshot (Plan A)"));
    GtkWidget* mi_clr_snap = gtk_menu_item_new_with_label(_("Clear Snapshot"));
    GtkWidget* mi_sep = gtk_separator_menu_item_new();
    GtkWidget* mi_reset = gtk_menu_item_new_with_label(_("Reset to Defaults"));
    g_signal_connect(mi_save, "activate", G_CALLBACK(on_save_plan_clicked), priv);
    g_signal_connect(mi_load, "activate", G_CALLBACK(on_load_plan_clicked), priv);
    g_signal_connect(mi_exp_csv, "activate", G_CALLBACK(on_export_csv_clicked), priv);
    g_signal_connect(mi_exp_html, "activate", G_CALLBACK(on_export_html_clicked), priv);
    g_signal_connect(mi_pin_snap, "activate", G_CALLBACK(on_pin_snapshot_clicked), priv);
    g_signal_connect(mi_clr_snap, "activate", G_CALLBACK(on_clear_snapshot_clicked), priv);
    g_signal_connect(mi_reset, "activate", G_CALLBACK(on_reset_defaults_clicked), priv);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_save);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_load);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_sep1);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_exp_csv);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_exp_html);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_sep2);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_pin_snap);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_clr_snap);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_reset);
    gtk_widget_show_all(plan_menu);

    GtkWidget* btn_plan_menu = gtk_menu_button_new();
    gtk_button_set_label(GTK_BUTTON(btn_plan_menu), _("Plan ▾"));
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(btn_plan_menu), plan_menu);
    gtk_box_pack_end(GTK_BOX(top_bar), btn_plan_menu, FALSE, FALSE, 0);

    // Mapped balances summary strip (ellipsized so it never forces window width)
    priv->lbl_bucket_summary = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(priv->lbl_bucket_summary), GTK_ALIGN_START);
    gtk_label_set_ellipsize(priv->lbl_bucket_summary, PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(outer), GTK_WIDGET(priv->lbl_bucket_summary), FALSE, FALSE, 2);

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 310);
    gtk_box_pack_start(GTK_BOX(outer), paned, TRUE, TRUE, 0);

    // Left panel: Tabbed parameter groups so everything is compact & reachable
    GtkWidget* left_nb = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(left_nb), TRUE);
    gtk_widget_set_size_request(left_nb, 260, 200);
    priv->left_panel = left_nb;

    auto make_param_tab = [&](const char* title, GtkGrid** out_grid) {
        GtkWidget* sc = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sc), 240);
        GtkGrid* g = GTK_GRID(gtk_grid_new());
        gtk_container_set_border_width(GTK_CONTAINER(g), 8);
        gtk_grid_set_row_spacing(g, 6);
        gtk_grid_set_column_spacing(g, 8);
        gtk_container_add(GTK_CONTAINER(sc), GTK_WIDGET(g));
        gtk_notebook_append_page(GTK_NOTEBOOK(left_nb), sc, gtk_label_new(title));
        *out_grid = g;
    };

    // Sub-tab 1: Profile & Strategy
    GtkGrid* g_prof = nullptr;
    make_param_tab(_("Profile"), &g_prof);
    int r1 = 0;
    priv->spin_cur_age = add_spin_row(g_prof, r1, _("Current Age"), 18, 95, 1, 55);
    priv->spin_ret_age = add_spin_row(g_prof, r1, _("Retirement Age"), 30, 95, 1, 65);
    priv->spin_hor_age = add_spin_row(g_prof, r1, _("Horizon Age"), 65, 110, 1, 95);
    priv->combo_filing = add_combo_row(g_prof, r1, _("Filing"), {"Single", "Married Joint"}, 1);
    priv->combo_objective = add_combo_row(g_prof, r1, _("Objective"), {"Maximize Spending", "Maximize Bequest"}, 0);
    priv->combo_strategy = add_combo_row(g_prof, r1, _("Strategy"), {
        strategy_label(WithdrawalStrategy::AutoOptimize),
        strategy_label(WithdrawalStrategy::StandardTaxableFirst),
        strategy_label(WithdrawalStrategy::TaxDeferredFirst),
        strategy_label(WithdrawalStrategy::RothBracketFill12),
        strategy_label(WithdrawalStrategy::RothBracketFill22),
        strategy_label(WithdrawalStrategy::RothBracketFill24)
    }, 0);
    priv->spin_target_spend = add_spin_row(g_prof, r1, _("Target Spend ($/yr)"), 0, 1000000, 1000, 80000);
    {
        GtkWidget* btn_est = gtk_button_new_with_label(_("Est. Spend from GnuCash Book"));
        g_signal_connect(btn_est, "clicked", G_CALLBACK(on_estimate_spending_clicked), priv);
        gtk_grid_attach(g_prof, btn_est, 0, r1++, 2, 1);
    }
    priv->spin_target_bequest = add_spin_row(g_prof, r1, _("Target Bequest ($)"), 0, 10000000, 5000, 100000);

    // Sub-tab 2: Income & Contributions
    GtkGrid* g_inc = nullptr;
    make_param_tab(_("Income"), &g_inc);
    int r2 = 0;
    priv->spin_ss_fra = add_spin_row(g_inc, r2, _("SS FRA ($/yr)"), 0, 120000, 500, 32000);
    priv->spin_ss_age = add_spin_row(g_inc, r2, _("SS Claim Age"), 62, 70, 1, 67);
    priv->chk_opt_ss = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Auto-optimize SS age (62-70)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_opt_ss), TRUE);
    gtk_grid_attach(g_inc, GTK_WIDGET(priv->chk_opt_ss), 0, r2++, 2, 1);
    priv->spin_pension = add_spin_row(g_inc, r2, _("Pension ($/yr)"), 0, 500000, 500, 0);
    priv->spin_pension_age = add_spin_row(g_inc, r2, _("Pension Age"), 50, 85, 1, 65);
    priv->chk_pension_cola = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Pension COLA (Inflation-Adjusted)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_pension_cola), TRUE);
    gtk_grid_attach(g_inc, GTK_WIDGET(priv->chk_pension_cola), 0, r2++, 2, 1);
    priv->spin_tips_floor = add_spin_row(g_inc, r2, _("TIPS Floor ($/yr)"), 0, 500000, 1000, 0);
    priv->spin_bridge_inc = add_spin_row(g_inc, r2, _("Bridge Work ($/yr)"), 0, 500000, 1000, 0);
    priv->spin_bridge_start = add_spin_row(g_inc, r2, _("Bridge Start Age"), 30, 95, 1, 60);
    priv->spin_bridge_end = add_spin_row(g_inc, r2, _("Bridge End Age"), 30, 100, 1, 65);
    priv->spin_contrib_tax = add_spin_row(g_inc, r2, _("Taxable Contrib/yr"), 0, 500000, 500, 10000);
    priv->spin_contrib_def = add_spin_row(g_inc, r2, _("401k/IRA Contrib/yr"), 0, 100000, 500, 23000);
    priv->spin_contrib_roth = add_spin_row(g_inc, r2, _("Roth Contrib/yr"), 0, 100000, 500, 7000);
    priv->spin_contrib_hsa = add_spin_row(g_inc, r2, _("HSA Contrib/yr"), 0, 20000, 250, 4150);

    // Sub-tab 3: Market & Tax Rules
    GtkGrid* g_mkt = nullptr;
    make_param_tab(_("Market/Tax"), &g_mkt);
    int r3 = 0;
    priv->spin_return_pct = add_spin_row(g_mkt, r3, _("Expected Return (%)"), 0.0, 15.0, 0.25, 6.5, 2);
    priv->spin_vol_pct = add_spin_row(g_mkt, r3, _("Volatility (%)"), 0.0, 35.0, 0.5, 12.0, 1);
    priv->spin_inf_pct = add_spin_row(g_mkt, r3, _("Inflation (%)"), 0.0, 10.0, 0.25, 2.5, 2);
    priv->spin_basis_pct = add_spin_row(g_mkt, r3, _("Taxable Basis (%)"), 0.0, 100.0, 5.0, 70.0, 1);
    priv->spin_ret_taxable = add_spin_row(g_mkt, r3, _("Taxable Return (% 0=auto)"), 0.0, 15.0, 0.25, 0.0, 2);
    priv->spin_ret_tax_def = add_spin_row(g_mkt, r3, _("401k/IRA Return (% 0=auto)"), 0.0, 15.0, 0.25, 0.0, 2);
    priv->spin_ret_tax_free = add_spin_row(g_mkt, r3, _("Roth Return (% 0=auto)"), 0.0, 15.0, 0.25, 0.0, 2);
    priv->spin_ret_hsa = add_spin_row(g_mkt, r3, _("HSA Return (% 0=auto)"), 0.0, 15.0, 0.25, 0.0, 2);
    priv->spin_rmd_age = add_spin_row(g_mkt, r3, _("RMD Start Age"), 72, 75, 1, 73);

    {
        GtkWidget* lbl_st = gtk_label_new(_("State Tax"));
        gtk_widget_set_halign(lbl_st, GTK_ALIGN_START);
        priv->combo_state = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
        for (int s = 0; s <= static_cast<int>(USState::WY); ++s)
            gtk_combo_box_text_append_text(priv->combo_state, state_code(static_cast<USState>(s)));
        gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state), 0);
        gtk_widget_set_hexpand(GTK_WIDGET(priv->combo_state), TRUE);
        gtk_grid_attach(g_mkt, lbl_st, 0, r3, 1, 1);
        gtk_grid_attach(g_mkt, GTK_WIDGET(priv->combo_state), 1, r3, 1, 1);
        ++r3;
    }

    priv->chk_enable_aca = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Model ACA Marketplace (<65)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_enable_aca), TRUE);
    gtk_grid_attach(g_mkt, GTK_WIDGET(priv->chk_enable_aca), 0, r3++, 2, 1);

    priv->spin_aca_bench = add_spin_row(g_mkt, r3, _("ACA Bench ($/adult)"), 0, 30000, 500, 8500);

    priv->chk_aca_cliff = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Enforce 400% FPL ACA Cliff")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_aca_cliff), TRUE);
    gtk_grid_attach(g_mkt, GTK_WIDGET(priv->chk_aca_cliff), 0, r3++, 2, 1);

    priv->chk_reinvest_rmd = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Re-invest RMD Surplus in Taxable")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_reinvest_rmd), TRUE);
    gtk_grid_attach(g_mkt, GTK_WIDGET(priv->chk_reinvest_rmd), 0, r3++, 2, 1);

    priv->chk_enable_guardrails = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Guyton-Klinger Guardrails (+/-10%)")));
    gtk_grid_attach(g_mkt, GTK_WIDGET(priv->chk_enable_guardrails), 0, r3++, 2, 1);
    priv->spin_gr_upper = add_spin_row(g_mkt, r3, _("Guardrail Upper WR (%)"), 2.0, 20.0, 0.25, 6.0, 2);
    priv->spin_gr_lower = add_spin_row(g_mkt, r3, _("Guardrail Lower WR (%)"), 1.0, 15.0, 0.25, 4.0, 2);
    priv->spin_gr_adj = add_spin_row(g_mkt, r3, _("Guardrail Adj (%)"), 1.0, 50.0, 1.0, 10.0, 1);

    priv->chk_enable_glide = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Use Equity/Bond Glide Path")));
    gtk_grid_attach(g_mkt, GTK_WIDGET(priv->chk_enable_glide), 0, r3++, 2, 1);
    priv->spin_eq_pre = add_spin_row(g_mkt, r3, _("Pre-Retire Equity (%)"), 0, 100, 5, 80);
    priv->spin_eq_in = add_spin_row(g_mkt, r3, _("In-Retire Equity (%)"), 0, 100, 5, 55);
    priv->spin_eq_ret = add_spin_row(g_mkt, r3, _("Equity Return (%)"), 0.0, 20.0, 0.25, 8.5, 2);
    priv->spin_bd_ret = add_spin_row(g_mkt, r3, _("Bond Return (%)"), 0.0, 15.0, 0.25, 4.0, 2);
    priv->spin_eq_vol = add_spin_row(g_mkt, r3, _("Equity Volatility (%)"), 0.0, 40.0, 0.5, 16.0, 1);
    priv->spin_bd_vol = add_spin_row(g_mkt, r3, _("Bond Volatility (%)"), 0.0, 25.0, 0.5, 5.0, 1);
    priv->spin_heir_tax = add_spin_row(g_mkt, r3, _("Heir Tax Rate (%)"), 0.0, 60.0, 1.0, 18.0, 1);

    priv->spin_mc_trials = add_spin_row(g_mkt, r3, _("Monte Carlo Trials"), 50, 1000, 50, 300);

    // Sub-tab 4: Spouse / Partner
    GtkGrid* g_sp = nullptr;
    make_param_tab(_("Spouse"), &g_sp);
    int r4 = 0;
    priv->chk_has_spouse = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Include Spouse / Partner")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_has_spouse), FALSE);
    g_signal_connect(priv->chk_has_spouse, "toggled",
                     G_CALLBACK(+[](GtkToggleButton*, gpointer ud) {
                         update_spouse_sensitivity(static_cast<GncPluginPageRetirementPrivate*>(ud));
                     }), priv);
    gtk_grid_attach(g_sp, GTK_WIDGET(priv->chk_has_spouse), 0, r4++, 2, 1);

    priv->spin_sp_cur_age = add_spin_row(g_sp, r4, _("Spouse Age"), 18, 95, 1, 53);
    priv->spin_sp_ret_age = add_spin_row(g_sp, r4, _("Spouse Retire Age"), 30, 95, 1, 65);
    priv->spin_sp_hor_age = add_spin_row(g_sp, r4, _("Spouse Horizon Age"), 65, 110, 1, 95);
    priv->spin_sp_ss_fra = add_spin_row(g_sp, r4, _("Spouse SS FRA ($/yr)"), 0, 120000, 500, 22000);
    priv->spin_sp_ss_age = add_spin_row(g_sp, r4, _("Spouse SS Claim Age"), 62, 70, 1, 67);
    priv->chk_sp_opt_ss = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Auto-optimize Spouse SS (62-70)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_sp_opt_ss), TRUE);
    gtk_grid_attach(g_sp, GTK_WIDGET(priv->chk_sp_opt_ss), 0, r4++, 2, 1);
    priv->spin_sp_pension = add_spin_row(g_sp, r4, _("Spouse Pension ($/yr)"), 0, 500000, 500, 0);
    priv->spin_sp_pension_age = add_spin_row(g_sp, r4, _("Spouse Pension Age"), 50, 85, 1, 65);
    priv->chk_sp_pension_cola = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Spouse Pension COLA (Inflation-Adjusted)")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_sp_pension_cola), TRUE);
    gtk_grid_attach(g_sp, GTK_WIDGET(priv->chk_sp_pension_cola), 0, r4++, 2, 1);
    priv->spin_sp_rmd_age = add_spin_row(g_sp, r4, _("Spouse RMD Age"), 72, 75, 1, 73);
    priv->spin_sp_contrib_def = add_spin_row(g_sp, r4, _("Spouse 401k Contrib"), 0, 100000, 500, 0);
    priv->spin_sp_contrib_roth = add_spin_row(g_sp, r4, _("Spouse Roth Contrib"), 0, 100000, 500, 0);
    update_spouse_sensitivity(priv);

    // Sub-tab 5: Spending Phases & One-Time Events
    {
        GtkWidget* ph_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(ph_box), 8);

        GtkGrid* g_ph = GTK_GRID(gtk_grid_new());
        gtk_grid_set_row_spacing(g_ph, 6);
        gtk_grid_set_column_spacing(g_ph, 8);
        int r5 = 0;
        priv->spin_slow_go_age = add_spin_row(g_ph, r5, _("Slow-Go Age"), 50, 110, 1, 75);
        priv->spin_slow_go_pct = add_spin_row(g_ph, r5, _("Slow-Go Spend (%)"), 20, 150, 5, 100);
        priv->spin_no_go_age = add_spin_row(g_ph, r5, _("No-Go Age"), 55, 110, 1, 85);
        priv->spin_no_go_pct = add_spin_row(g_ph, r5, _("No-Go Spend (%)"), 20, 150, 5, 100);
        priv->spin_debt_pmt = add_spin_row(g_ph, r5, _("Fixed Debt/Mort ($/yr)"), 0, 200000, 1000, 0);
        priv->spin_debt_age = add_spin_row(g_ph, r5, _("Debt Payoff Age"), 30, 110, 1, 68);
        priv->spin_ltc_age = add_spin_row(g_ph, r5, _("LTC Start Age"), 50, 110, 1, 82);
        priv->spin_ltc_yrs = add_spin_row(g_ph, r5, _("LTC Duration (yrs)"), 0, 20, 1, 0);
        priv->spin_ltc_cost = add_spin_row(g_ph, r5, _("LTC Cost ($/yr)"), 0, 300000, 5000, 65000);
        gtk_box_pack_start(GTK_BOX(ph_box), GTK_WIDGET(g_ph), FALSE, FALSE, 0);

        GtkWidget* ev_hdr = gtk_label_new(_("One-Time Events (Windfalls / Expenses)"));
        gtk_widget_set_halign(ev_hdr, GTK_ALIGN_START);
        gtk_style_context_add_class(gtk_widget_get_style_context(ev_hdr), "gnc-class-strong");
        gtk_box_pack_start(GTK_BOX(ph_box), ev_hdr, FALSE, FALSE, 4);

        priv->event_store = gtk_list_store_new(4, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
        GtkWidget* ev_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->event_store));
        priv->event_tv = GTK_TREE_VIEW(ev_tv);
        const char* ev_titles[] = { "Age", "Name", "Amount", "Bucket" };
        for (int c = 0; c < 4; ++c)
        {
            GtkCellRenderer* r = gtk_cell_renderer_text_new();
            if (c == 1) g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
            if (c == 2) g_object_set(r, "xalign", 1.0f, nullptr);
            GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(ev_titles[c], r, "text", c, nullptr);
            if (c == 1) gtk_tree_view_column_set_expand(col, TRUE);
            gtk_tree_view_append_column(GTK_TREE_VIEW(ev_tv), col);
        }

        GtkWidget* ev_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ev_scroll),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ev_scroll), 120);
        gtk_container_add(GTK_CONTAINER(ev_scroll), ev_tv);
        gtk_box_pack_start(GTK_BOX(ph_box), ev_scroll, TRUE, TRUE, 0);

        GtkWidget* ev_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* btn_add_ev = gtk_button_new_with_label(_("Add Event..."));
        GtkWidget* btn_rem_ev = gtk_button_new_with_label(_("Remove"));
        g_signal_connect(btn_add_ev, "clicked", G_CALLBACK(on_add_event_clicked), priv);
        g_signal_connect(btn_rem_ev, "clicked", G_CALLBACK(on_remove_event_clicked), priv);
        gtk_box_pack_start(GTK_BOX(ev_btns), btn_add_ev, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ev_btns), btn_rem_ev, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(ph_box), ev_btns, FALSE, FALSE, 0);

        GtkWidget* ph_scroll = gtk_scrolled_window_new(nullptr, nullptr);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ph_scroll),
                                       GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(ph_scroll), ph_box);
        gtk_notebook_append_page(GTK_NOTEBOOK(left_nb), ph_scroll, gtk_label_new(_("Phases/Events")));
    }

    gtk_paned_pack1(GTK_PANED(paned), left_nb, FALSE, TRUE);

    // Right panel: Notebook with Overview/Chart, Schedule, Baseline vs Owl, Account Mapping
    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);
    gtk_paned_pack2(GTK_PANED(paned), nb, TRUE, TRUE);

    // Tab 1: Overview & Chart
    GtkWidget* tab_overview = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_overview), 6);

    GtkWidget* kpi_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(kpi_grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(kpi_grid), 6);
    gtk_grid_set_column_homogeneous(GTK_GRID(kpi_grid), TRUE);

    gtk_grid_attach(GTK_GRID(kpi_grid), make_kpi_card(_("Max Spending / yr"),
        &priv->lbl_kpi_primary_title, &priv->lbl_kpi_primary_val), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(kpi_grid), make_kpi_card(_("Owl Advantage"),
        nullptr, &priv->lbl_kpi_adv_val), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(kpi_grid), make_kpi_card(_("Lifetime Tax + ACA"),
        nullptr, &priv->lbl_kpi_tax_val), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(kpi_grid), make_kpi_card(_("Monte Carlo"),
        nullptr, &priv->lbl_kpi_mc_val), 3, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(tab_overview), kpi_grid, FALSE, FALSE, 0);

    priv->lbl_kpi_rec_val = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(priv->lbl_kpi_rec_val), GTK_ALIGN_START);
    gtk_label_set_ellipsize(priv->lbl_kpi_rec_val, PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(priv->lbl_kpi_rec_val)), "gnc-class-strong");
    gtk_box_pack_start(GTK_BOX(tab_overview), GTK_WIDGET(priv->lbl_kpi_rec_val), FALSE, FALSE, 2);

    priv->chart_area = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_size_request(GTK_WIDGET(priv->chart_area), 300, 200);
    gtk_widget_set_vexpand(GTK_WIDGET(priv->chart_area), TRUE);
    gtk_widget_add_events(GTK_WIDGET(priv->chart_area),
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(priv->chart_area, "draw", G_CALLBACK(on_chart_draw), priv);
    g_signal_connect(priv->chart_area, "motion-notify-event", G_CALLBACK(on_chart_motion), priv);
    g_signal_connect(priv->chart_area, "leave-notify-event", G_CALLBACK(on_chart_leave), priv);
    gtk_box_pack_start(GTK_BOX(tab_overview), GTK_WIDGET(priv->chart_area), TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tab_overview, gtk_label_new(_("Overview & Chart")));

    // Tab 2: Year-by-Year Schedule
    priv->sched_store = gtk_list_store_new(NUM_SCHED_COLS,
        G_TYPE_INT, G_TYPE_INT,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* sched_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->sched_store));
    const char* sched_titles[] = {
        "Year", "Age", "Total", "Taxable", "401k/IRA", "Roth",
        "HSA", "SS+Pens", "RMD", "Roth Conv", "Tax", "Net Spend",
        "Floor Gap", "RMD Reinv", "Bracket / IRMAA"
    };
    for (int c = 0; c < NUM_SCHED_COLS; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c >= 2) ? 1.0f : 0.5f, nullptr);
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
            sched_titles[c], rend, "text", c, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(sched_tv), col);
    }
    GtkWidget* sched_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sched_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(sched_scroll), 200);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sched_scroll), 200);
    gtk_container_add(GTK_CONTAINER(sched_scroll), sched_tv);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), sched_scroll, gtk_label_new(_("Year-by-Year Schedule")));

    // Tab 3: Baseline vs Owl Comparison
    priv->cmp_store = gtk_list_store_new(NUM_CMP_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* cmp_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->cmp_store));
    const char* cmp_titles[] = { "Metric", "Baseline (Taxable First)", "Owl Optimized", "Difference" };
    for (int c = 0; c < NUM_CMP_COLS; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c >= 1) ? 1.0f : 0.0f, nullptr);
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
            cmp_titles[c], rend, "text", c, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(cmp_tv), col);
    }
    GtkWidget* cmp_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cmp_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(cmp_scroll), 200);
    gtk_container_add(GTK_CONTAINER(cmp_scroll), cmp_tv);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), cmp_scroll, gtk_label_new(_("Baseline vs Owl")));

    // Tab 4: Account Mapping (filterable, with Include checkbox & bulk selection)
    priv->acct_store = gtk_list_store_new(NUM_ACCT_COLS,
        G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_INT);
    priv->acct_filter = GTK_TREE_MODEL_FILTER(
        gtk_tree_model_filter_new(GTK_TREE_MODEL(priv->acct_store), nullptr));
    gtk_tree_model_filter_set_visible_func(priv->acct_filter, acct_filter_visible_func, priv, nullptr);

    GtkWidget* tab_acct = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_acct), 6);

    GtkWidget* acct_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->entry_acct_filter = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_entry_set_placeholder_text(GTK_ENTRY(priv->entry_acct_filter), _("Filter accounts..."));
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_acct_filter), TRUE);
    g_signal_connect(priv->entry_acct_filter, "search-changed", G_CALLBACK(on_acct_filter_changed), priv);
    gtk_box_pack_start(GTK_BOX(acct_bar), GTK_WIDGET(priv->entry_acct_filter), TRUE, TRUE, 0);

    GtkWidget* btn_sel_all = gtk_button_new_with_label(_("Select All"));
    g_signal_connect(btn_sel_all, "clicked", G_CALLBACK(on_select_all_accounts), priv);
    gtk_box_pack_start(GTK_BOX(acct_bar), btn_sel_all, FALSE, FALSE, 0);

    GtkWidget* btn_desel_all = gtk_button_new_with_label(_("Deselect All"));
    g_signal_connect(btn_desel_all, "clicked", G_CALLBACK(on_deselect_all_accounts), priv);
    gtk_box_pack_start(GTK_BOX(acct_bar), btn_desel_all, FALSE, FALSE, 0);

    GtkWidget* btn_auto_cls = gtk_button_new_with_label(_("Auto-Classify"));
    g_signal_connect(btn_auto_cls, "clicked", G_CALLBACK(on_auto_classify_accounts), priv);
    gtk_box_pack_start(GTK_BOX(acct_bar), btn_auto_cls, FALSE, FALSE, 0);

    priv->lbl_acct_count = GTK_LABEL(gtk_label_new(""));
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(priv->lbl_acct_count)),
                                "gnc-class-lighter-grey-mix");
    gtk_box_pack_end(GTK_BOX(acct_bar), GTK_WIDGET(priv->lbl_acct_count), FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(tab_acct), acct_bar, FALSE, FALSE, 0);

    GtkWidget* acct_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->acct_filter));

    GtkCellRenderer* inc_rend = gtk_cell_renderer_toggle_new();
    g_signal_connect(inc_rend, "toggled", G_CALLBACK(on_account_include_toggled), priv);
    gtk_tree_view_append_column(GTK_TREE_VIEW(acct_tv),
        gtk_tree_view_column_new_with_attributes(_("Include"),
            inc_rend, "active", ACCT_COL_INCLUDE, nullptr));

    GtkCellRenderer* acct_name_rend = gtk_cell_renderer_text_new();
    g_object_set(acct_name_rend, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, nullptr);
    GtkTreeViewColumn* acct_name_col = gtk_tree_view_column_new_with_attributes(
        _("GnuCash Account"), acct_name_rend, "text", ACCT_COL_NAME, nullptr);
    gtk_tree_view_column_set_expand(acct_name_col, TRUE);
    gtk_tree_view_column_set_min_width(acct_name_col, 160);
    gtk_tree_view_column_set_resizable(acct_name_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(acct_tv), acct_name_col);

    GtkCellRenderer* bal_rend = gtk_cell_renderer_text_new();
    g_object_set(bal_rend, "xalign", 1.0f, nullptr);
    gtk_tree_view_append_column(GTK_TREE_VIEW(acct_tv),
        gtk_tree_view_column_new_with_attributes(_("Balance"),
            bal_rend, "text", ACCT_COL_BAL, nullptr));

    GtkListStore* combo_model = gtk_list_store_new(1, G_TYPE_STRING);
    for (int b = 0; b <= static_cast<int>(TaxBucket::Excluded); ++b)
    {
        GtkTreeIter it;
        gtk_list_store_append(combo_model, &it);
        gtk_list_store_set(combo_model, &it, 0, bucket_label(static_cast<TaxBucket>(b)), -1);
    }
    GtkCellRenderer* combo_rend = gtk_cell_renderer_combo_new();
    g_object_set(combo_rend, "model", combo_model, "text-column", 0,
                 "editable", TRUE, "has-entry", FALSE, nullptr);
    g_object_unref(combo_model);
    g_signal_connect(combo_rend, "edited", G_CALLBACK(on_bucket_edited), priv);
    gtk_tree_view_append_column(GTK_TREE_VIEW(acct_tv),
        gtk_tree_view_column_new_with_attributes(_("Tax Bucket (Click to Override)"),
            combo_rend, "text", ACCT_COL_BUCKET_STR, nullptr));

    GtkWidget* acct_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(acct_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(acct_scroll), 200);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(acct_scroll), 200);
    gtk_container_add(GTK_CONTAINER(acct_scroll), acct_tv);
    gtk_box_pack_start(GTK_BOX(tab_acct), acct_scroll, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tab_acct, gtk_label_new(_("Account Mapping")));

    // Tab 5: Sequence-of-Returns Stress Test & Sensitivity Matrix
    GtkWidget* tab_stress = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(tab_stress), 8);

    GtkWidget* lbl_st_hdr = gtk_label_new(_("Sequence-of-Returns & Inflation Stress Scenarios"));
    gtk_widget_set_halign(lbl_st_hdr, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_st_hdr), "gnc-class-strong");
    gtk_box_pack_start(GTK_BOX(tab_stress), lbl_st_hdr, FALSE, FALSE, 0);

    priv->stress_store = gtk_list_store_new(6,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
    GtkWidget* stress_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->stress_store));
    const char* stress_titles[] = {
        "Scenario", "Status", "Min Spend / yr", "Final Bequest (Today $)", "Shortfall Yrs"
    };
    for (int c = 0; c < 5; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c >= 2) ? 1.0f : 0.0f, nullptr);
        GtkTreeViewColumn* col = nullptr;
        if (c == 1)
        {
            col = gtk_tree_view_column_new_with_attributes(
                stress_titles[c], rend, "text", c, "foreground", 5, nullptr);
        }
        else
        {
            col = gtk_tree_view_column_new_with_attributes(
                stress_titles[c], rend, "text", c, nullptr);
        }
        if (c == 0) gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(stress_tv), col);
    }
    GtkWidget* stress_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stress_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(stress_scroll), 140);
    gtk_container_add(GTK_CONTAINER(stress_scroll), stress_tv);
    gtk_box_pack_start(GTK_BOX(tab_stress), stress_scroll, FALSE, TRUE, 0);

    GtkWidget* lbl_sens_hdr = gtk_label_new(_("Sensitivity Matrix: Expected Return vs. Annual Spending (Bequest or Shortfall Years)"));
    gtk_widget_set_halign(lbl_sens_hdr, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl_sens_hdr), "gnc-class-strong");
    gtk_box_pack_start(GTK_BOX(tab_stress), lbl_sens_hdr, FALSE, FALSE, 4);

    priv->sens_store = gtk_list_store_new(11,
        G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* sens_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->sens_store));
    const char* sens_init_titles[6] = { "Return \\ Spend", "85%", "92.5%", "100%", "107.5%", "115%" };
    for (int c = 0; c < 6; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c >= 1) ? 1.0f : 0.5f, nullptr);
        GtkTreeViewColumn* col = nullptr;
        if (c == 0)
        {
            col = gtk_tree_view_column_new_with_attributes(sens_init_titles[c], rend, "text", 0, nullptr);
        }
        else
        {
            g_object_set(rend, "foreground", "#1b1b1b", nullptr);
            col = gtk_tree_view_column_new_with_attributes(
                sens_init_titles[c], rend, "text", c, "cell-background", 5 + c, nullptr);
        }
        gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(sens_tv), col);
        priv->sens_cols[c] = col;
    }
    GtkWidget* sens_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sens_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sens_scroll), 160);
    gtk_container_add(GTK_CONTAINER(sens_scroll), sens_tv);
    gtk_box_pack_start(GTK_BOX(tab_stress), sens_scroll, TRUE, TRUE, 0);

    priv->lbl_hist_summary = GTK_LABEL(gtk_label_new(_("Historical Backtests (1928-2024)")));
    gtk_widget_set_halign(GTK_WIDGET(priv->lbl_hist_summary), GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(GTK_WIDGET(priv->lbl_hist_summary)), "gnc-class-strong");
    gtk_box_pack_start(GTK_BOX(tab_stress), GTK_WIDGET(priv->lbl_hist_summary), FALSE, FALSE, 4);

    priv->hist_store = gtk_list_store_new(7,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* hist_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->hist_store));
    const char* hist_titles[6] = { "Historical Cycle", "Start", "Status", "Min Spend/yr", "Final Bequest", "Shortfall Yrs" };
    for (int c = 0; c < 6; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c >= 3) ? 1.0f : 0.0f, nullptr);
        GtkTreeViewColumn* col = nullptr;
        if (c == 2)
        {
            g_object_set(rend, "foreground", "#1b1b1b", nullptr);
            col = gtk_tree_view_column_new_with_attributes(
                hist_titles[c], rend, "text", c, "cell-background", 6, nullptr);
        }
        else
        {
            col = gtk_tree_view_column_new_with_attributes(hist_titles[c], rend, "text", c, nullptr);
        }
        gtk_tree_view_column_set_expand(col, c == 0);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(hist_tv), col);
    }
    GtkWidget* hist_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hist_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(hist_scroll), 150);
    gtk_container_add(GTK_CONTAINER(hist_scroll), hist_tv);
    gtk_box_pack_start(GTK_BOX(tab_stress), hist_scroll, FALSE, TRUE, 0);

    GtkWidget* stress_tab_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(stress_tab_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(stress_tab_scroll), tab_stress);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), stress_tab_scroll, gtk_label_new(_("Stress & Sensitivity")));

    // Tab 6: Social Security Break-Even Analysis
    priv->ss_be_store = gtk_list_store_new(11,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* be_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->ss_be_store));
    const char* be_titles[11] = {
        "Claim Age", "Annual Benefit", "BE vs 62", "BE vs 67",
        "Age 65", "Age 70", "Age 75", "Age 80", "Age 85", "Age 90", "Age 95"
    };
    for (int c = 0; c < 11; ++c)
    {
        GtkCellRenderer* rend = gtk_cell_renderer_text_new();
        g_object_set(rend, "xalign", (c == 0) ? 0.5f : 1.0f, nullptr);
        GtkTreeViewColumn* col = gtk_tree_view_column_new_with_attributes(
            be_titles[c], rend, "text", c, nullptr);
        gtk_tree_view_column_set_expand(col, TRUE);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(be_tv), col);
    }
    GtkWidget* be_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(be_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(be_scroll), be_tv);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), be_scroll, gtk_label_new(_("SS Break-Even")));

    // Tab 7: Ask Owl AI (Advisor & What-If Assistant)
    GtkWidget* tab_ai = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ai), 8);

    GtkWidget* ai_cfg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ai_cfg), 4);
    gtk_grid_set_column_spacing(GTK_GRID(ai_cfg), 6);

    priv->combo_ai_provider = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("Local Ollama (Offline)"));
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("Google Gemini"));
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("OpenAI-Compatible"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ai_provider), 0);
    g_signal_connect(priv->combo_ai_provider, "changed", G_CALLBACK(on_ai_provider_changed), priv);

    priv->entry_ai_endpoint = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(priv->entry_ai_endpoint, "http://localhost:11434/api/generate");
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ai_endpoint), TRUE);

    priv->entry_ai_model = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(priv->entry_ai_model, "llama3.2");

    priv->entry_ai_key = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_visibility(priv->entry_ai_key, FALSE);
    gtk_entry_set_placeholder_text(priv->entry_ai_key, _("Optional for Ollama; or set GEMINI_API_KEY / OPENAI_API_KEY"));

    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Provider")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->combo_ai_provider), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Model")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->entry_ai_model), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Endpoint")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->entry_ai_endpoint), 1, 1, 3, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("API Key")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->entry_ai_key), 1, 2, 3, 1);
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_cfg, FALSE, FALSE, 0);

    GtkWidget* ai_qbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->entry_ai_question = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(priv->entry_ai_question, _("Ask a What-If question (e.g. 'What if I retire at 62 and take SS at 70?')"));
    g_signal_connect(priv->entry_ai_question, "activate", G_CALLBACK(on_ai_ask_clicked), priv);
    GtkWidget* btn_ai_ask = gtk_button_new_with_label(_("Ask Owl AI"));
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_ai_ask), "suggested-action");
    g_signal_connect(btn_ai_ask, "clicked", G_CALLBACK(on_ai_ask_clicked), priv);
    GtkWidget* btn_ai_critique = gtk_button_new_with_label(_("Critique My Plan"));
    g_signal_connect(btn_ai_critique, "clicked", G_CALLBACK(on_ai_critique_clicked), priv);
    GtkWidget* btn_ai_copy = gtk_button_new_with_label(_("Copy Summary"));
    g_signal_connect(btn_ai_copy, "clicked", G_CALLBACK(on_ai_copy_summary_clicked), priv);
    gtk_box_pack_start(GTK_BOX(ai_qbar), GTK_WIDGET(priv->entry_ai_question), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), btn_ai_ask, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), btn_ai_critique, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), btn_ai_copy, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_qbar, FALSE, FALSE, 0);

    priv->tv_ai_response = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(priv->tv_ai_response, FALSE);
    gtk_text_view_set_wrap_mode(priv->tv_ai_response, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(priv->tv_ai_response, 8);
    gtk_text_view_set_right_margin(priv->tv_ai_response, 8);
    gtk_text_view_set_top_margin(priv->tv_ai_response, 6);
    set_ai_response_text(priv, "Privacy-first AI Advisor: Only an anonymized numeric summary (no account names or PII) is sent.\n"
                               "Click 'Critique My Plan' or ask a What-If question above.");
    GtkWidget* ai_scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ai_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(ai_scroll), GTK_WIDGET(priv->tv_ai_response));
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_scroll, TRUE, TRUE, 0);

    GtkWidget* ai_foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->lbl_ai_status = GTK_LABEL(gtk_label_new(_("Ready.")));
    gtk_widget_set_halign(GTK_WIDGET(priv->lbl_ai_status), GTK_ALIGN_START);
    gtk_label_set_ellipsize(priv->lbl_ai_status, PANGO_ELLIPSIZE_END);
    priv->btn_ai_apply = gtk_button_new_with_label(_("Apply Suggested Changes"));
    gtk_widget_set_sensitive(priv->btn_ai_apply, FALSE);
    g_signal_connect(priv->btn_ai_apply, "clicked", G_CALLBACK(on_ai_apply_clicked), priv);
    gtk_box_pack_start(GTK_BOX(ai_foot), GTK_WIDGET(priv->lbl_ai_status), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(ai_foot), priv->btn_ai_apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_foot, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tab_ai, gtk_label_new(_("Ask Owl AI")));

    priv->root_widget = outer;
    g_object_set_data(G_OBJECT(plugin_page), "gnc-page-help-fn",
                      reinterpret_cast<gpointer>(on_page_f1_help));
    gtk_widget_show_all(priv->root_widget);

    refresh_accounts_from_book(priv);
    load_state_from_book(priv);
    run_planner(priv);
    return priv->root_widget;
}

static void
gnc_plugin_page_retirement_save_page(GncPluginPage* plugin_page, GKeyFile*, const gchar*)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);
    save_state_to_book(priv, false);
}

static GncPluginPage*
gnc_plugin_page_retirement_recreate_page(GtkWidget*, GKeyFile*, const gchar*)
{
    return gnc_plugin_page_retirement_new();
}

static void
gnc_plugin_page_retirement_destroy_widget(GncPluginPage* plugin_page)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);

    if (priv->root_widget)
    {
        save_state_to_book(priv);
        g_object_unref(priv->sched_store);
        g_object_unref(priv->cmp_store);
        g_object_unref(priv->acct_filter);
        g_object_unref(priv->acct_store);
        g_object_unref(priv->event_store);
        g_object_unref(priv->stress_store);
        g_object_unref(priv->sens_store);
        g_object_unref(priv->hist_store);
        g_object_unref(priv->ss_be_store);
        if (priv->soup_session)
            g_object_unref(priv->soup_session);
        priv->soup_session = nullptr;
        priv->tv_ai_response = nullptr;
        priv->lbl_ai_status = nullptr;
        priv->btn_ai_apply = nullptr;
        priv->sched_store = nullptr;
        priv->cmp_store = nullptr;
        priv->acct_filter = nullptr;
        priv->acct_store = nullptr;
        priv->event_store = nullptr;
        priv->stress_store = nullptr;
        priv->sens_store = nullptr;
        priv->hist_store = nullptr;
        priv->lbl_hist_summary = nullptr;
        priv->ss_be_store = nullptr;
        priv->event_tv = nullptr;
        priv->entry_acct_filter = nullptr;
        priv->lbl_acct_count = nullptr;
        priv->chart_area = nullptr;
        priv->left_panel = nullptr;
        priv->root_widget = nullptr;
    }
}
