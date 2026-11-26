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

#include <algorithm>
#include <cmath>
#include <cstdio>
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
};

enum class ChartMode
{
    PortfolioBuckets = 0,
    IncomeAndWithdrawals = 1,
    TaxesRmdAndRoth = 2
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
    NUM_SCHED_COLS
};

enum
{
    ACCT_COL_NAME = 0,
    ACCT_COL_BAL,
    ACCT_COL_BUCKET_STR,
    ACCT_COL_BUCKET_ID,
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

    GtkSpinButton* spin_contrib_tax = nullptr;
    GtkSpinButton* spin_contrib_def = nullptr;
    GtkSpinButton* spin_contrib_roth = nullptr;
    GtkSpinButton* spin_contrib_hsa = nullptr;

    GtkSpinButton* spin_return_pct = nullptr;
    GtkSpinButton* spin_vol_pct = nullptr;
    GtkSpinButton* spin_inf_pct = nullptr;
    GtkSpinButton* spin_rmd_age = nullptr;
    GtkComboBoxText* combo_state = nullptr;
    GtkCheckButton* chk_enable_aca = nullptr;
    GtkSpinButton* spin_aca_bench = nullptr;
    GtkCheckButton* chk_aca_cliff = nullptr;
    GtkSpinButton* spin_mc_trials = nullptr;

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
    GtkListStore* sched_store = nullptr;
    GtkListStore* cmp_store = nullptr;
    GtkListStore* acct_store = nullptr;

    std::vector<MappedAccount> accounts;
    PlanInput last_input;
    PlanResult last_result;
};

G_DEFINE_TYPE_WITH_PRIVATE(GncPluginPageRetirement, gnc_plugin_page_retirement, GNC_TYPE_PLUGIN_PAGE)

#define GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(o) \
    ((GncPluginPageRetirementPrivate*)gnc_plugin_page_retirement_get_instance_private((GncPluginPageRetirement*)(o)))

static void gnc_plugin_page_retirement_finalize(GObject* object);
static GtkWidget* gnc_plugin_page_retirement_create_widget(GncPluginPage* plugin_page);
static void gnc_plugin_page_retirement_destroy_widget(GncPluginPage* plugin_page);

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
}

static void
gnc_plugin_page_retirement_init(GncPluginPageRetirement* page)
{
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);
    new (&priv->accounts) std::vector<MappedAccount>();
    new (&priv->last_input) PlanInput();
    new (&priv->last_result) PlanResult();

    gnc_plugin_page_add_book(GNC_PLUGIN_PAGE(page), gnc_get_current_book());
}

static void
gnc_plugin_page_retirement_finalize(GObject* object)
{
    auto page = GNC_PLUGIN_PAGE_RETIREMENT(object);
    auto priv = GNC_PLUGIN_PAGE_RETIREMENT_GET_PRIVATE(page);
    priv->accounts.~vector();
    priv->last_input.~PlanInput();
    priv->last_result.~PlanResult();
    G_OBJECT_CLASS(gnc_plugin_page_retirement_parent_class)->finalize(object);
}

static AccountBuckets
compute_buckets_from_accounts(const std::vector<MappedAccount>& accounts)
{
    AccountBuckets b;
    for (const auto& a : accounts)
    {
        if (a.balance <= 0.0)
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
    g_free(full);

    list->push_back(std::move(ma));
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

    gtk_list_store_clear(priv->acct_store);
    for (const auto& ma : priv->accounts)
    {
        GtkTreeIter iter;
        gtk_list_store_append(priv->acct_store, &iter);
        std::string bal_str = fmt_table_dollar(ma.balance);
        gtk_list_store_set(priv->acct_store, &iter,
                           ACCT_COL_NAME, ma.full_name.c_str(),
                           ACCT_COL_BAL, bal_str.c_str(),
                           ACCT_COL_BUCKET_STR, bucket_label(ma.bucket),
                           ACCT_COL_BUCKET_ID, static_cast<int>(ma.bucket),
                           -1);
    }
    update_bucket_summary_label(priv);
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

    in.contrib_taxable = gtk_spin_button_get_value(priv->spin_contrib_tax);
    in.contrib_tax_deferred = gtk_spin_button_get_value(priv->spin_contrib_def);
    in.contrib_tax_free = gtk_spin_button_get_value(priv->spin_contrib_roth);
    in.contrib_hsa = gtk_spin_button_get_value(priv->spin_contrib_hsa);

    in.nominal_return = gtk_spin_button_get_value(priv->spin_return_pct) / 100.0;
    in.volatility = gtk_spin_button_get_value(priv->spin_vol_pct) / 100.0;
    in.inflation = gtk_spin_button_get_value(priv->spin_inf_pct) / 100.0;
    in.rmd_start_age = gtk_spin_button_get_value_as_int(priv->spin_rmd_age);
    in.state = static_cast<USState>(
        std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_state)), 0, 51));
    in.enable_aca = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_enable_aca));
    in.aca_benchmark_premium = gtk_spin_button_get_value(priv->spin_aca_bench);
    in.aca_enforce_400_cliff = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_aca_cliff));
    in.monte_carlo_trials = gtk_spin_button_get_value_as_int(priv->spin_mc_trials);
    return in;
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

    gtk_spin_button_set_value(priv->spin_contrib_tax, in.contrib_taxable);
    gtk_spin_button_set_value(priv->spin_contrib_def, in.contrib_tax_deferred);
    gtk_spin_button_set_value(priv->spin_contrib_roth, in.contrib_tax_free);
    gtk_spin_button_set_value(priv->spin_contrib_hsa, in.contrib_hsa);

    gtk_spin_button_set_value(priv->spin_return_pct, in.nominal_return * 100.0);
    gtk_spin_button_set_value(priv->spin_vol_pct, in.volatility * 100.0);
    gtk_spin_button_set_value(priv->spin_inf_pct, in.inflation * 100.0);
    gtk_spin_button_set_value(priv->spin_rmd_age, in.rmd_start_age);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state),
                             std::clamp(static_cast<int>(in.state), 0, 51));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_enable_aca), in.enable_aca);
    gtk_spin_button_set_value(priv->spin_aca_bench, in.aca_benchmark_premium);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_aca_cliff), in.aca_enforce_400_cliff);
    gtk_spin_button_set_value(priv->spin_mc_trials, in.monte_carlo_trials);
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

static void
apply_account_overrides_to_ui(GncPluginPageRetirementPrivate* priv,
                              const std::vector<std::pair<std::string, TaxBucket>>& overrides)
{
    if (overrides.empty())
        return;
    for (auto& a : priv->accounts)
    {
        for (const auto& kv : overrides)
        {
            if (a.full_name == kv.first)
            {
                a.bucket = kv.second;
                break;
            }
        }
    }
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->acct_store), &iter))
    {
        size_t idx = 0;
        do
        {
            if (idx < priv->accounts.size())
            {
                TaxBucket b = priv->accounts[idx].bucket;
                gtk_list_store_set(priv->acct_store, &iter,
                                   ACCT_COL_BUCKET_STR, bucket_label(b),
                                   ACCT_COL_BUCKET_ID, static_cast<int>(b),
                                   -1);
            }
            ++idx;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(priv->acct_store), &iter));
    }
    update_bucket_summary_label(priv);
}

static constexpr const char* STATE_GROUP_PLAN = "RetirementPlanner";
static constexpr const char* STATE_GROUP_ACCTS = "RetirementPlannerAccounts";

static void
save_state_to_book(GncPluginPageRetirementPrivate* priv)
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
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_taxable", in.contrib_taxable);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_tax_deferred", in.contrib_tax_deferred);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_tax_free", in.contrib_tax_free);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "contrib_hsa", in.contrib_hsa);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "nominal_return", in.nominal_return);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "volatility", in.volatility);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "inflation", in.inflation);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "rmd_start_age", in.rmd_start_age);
    g_key_file_set_string(state, STATE_GROUP_PLAN, "state", state_code(in.state));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "enable_aca", in.enable_aca);
    g_key_file_set_double(state, STATE_GROUP_PLAN, "aca_benchmark_premium", in.aca_benchmark_premium);
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "aca_enforce_400_cliff", in.aca_enforce_400_cliff);
    g_key_file_set_integer(state, STATE_GROUP_PLAN, "monte_carlo_trials", in.monte_carlo_trials);

    g_key_file_set_integer(state, STATE_GROUP_PLAN, "chart_mode",
                           gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_chart_mode)));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "real_dollars",
                           gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars)));
    g_key_file_set_boolean(state, STATE_GROUP_PLAN, "show_params",
                           gtk_toggle_button_get_active(priv->btn_toggle_params));

    g_key_file_remove_group(state, STATE_GROUP_ACCTS, nullptr);
    for (const auto& a : priv->accounts)
        g_key_file_set_string(state, STATE_GROUP_ACCTS, a.full_name.c_str(), bucket_label(a.bucket));
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
    in.contrib_taxable = std::max(0.0, get_dbl("contrib_taxable", in.contrib_taxable));
    in.contrib_tax_deferred = std::max(0.0, get_dbl("contrib_tax_deferred", in.contrib_tax_deferred));
    in.contrib_tax_free = std::max(0.0, get_dbl("contrib_tax_free", in.contrib_tax_free));
    in.contrib_hsa = std::max(0.0, get_dbl("contrib_hsa", in.contrib_hsa));
    in.nominal_return = std::clamp(get_dbl("nominal_return", in.nominal_return), 0.0, 0.25);
    in.volatility = std::clamp(get_dbl("volatility", in.volatility), 0.0, 0.50);
    in.inflation = std::clamp(get_dbl("inflation", in.inflation), 0.0, 0.15);
    in.rmd_start_age = std::clamp(get_int("rmd_start_age", in.rmd_start_age), 72, 75);
    if (gchar* st = g_key_file_get_string(state, STATE_GROUP_PLAN, "state", nullptr))
    {
        in.state = state_from_code(st);
        g_free(st);
    }
    in.enable_aca = get_bool("enable_aca", in.enable_aca);
    in.aca_benchmark_premium = std::max(0.0, get_dbl("aca_benchmark_premium", in.aca_benchmark_premium));
    in.aca_enforce_400_cliff = get_bool("aca_enforce_400_cliff", in.aca_enforce_400_cliff);
    in.monte_carlo_trials = std::clamp(get_int("monte_carlo_trials", in.monte_carlo_trials), 50, 1000);

    apply_plan_input_to_ui(priv, in);

    int chart_mode = std::clamp(get_int("chart_mode", 0), 0, 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_chart_mode), chart_mode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars),
                                 get_bool("real_dollars", true));
    bool show_params = get_bool("show_params", true);
    gtk_toggle_button_set_active(priv->btn_toggle_params, show_params);
    if (priv->left_panel)
        gtk_widget_set_visible(priv->left_panel, show_params);

    if (g_key_file_has_group(state, STATE_GROUP_ACCTS))
    {
        gsize nkeys = 0;
        gchar** keys = g_key_file_get_keys(state, STATE_GROUP_ACCTS, &nkeys, nullptr);
        std::vector<std::pair<std::string, TaxBucket>> overrides;
        for (gsize i = 0; keys && i < nkeys; ++i)
        {
            gchar* val = g_key_file_get_string(state, STATE_GROUP_ACCTS, keys[i], nullptr);
            if (val)
            {
                overrides.emplace_back(keys[i], bucket_from_label(val));
                g_free(val);
            }
        }
        g_strfreev(keys);
        apply_account_overrides_to_ui(priv, overrides);
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

    char rec_buf[160];
    std::snprintf(rec_buf, sizeof(rec_buf), "Recommended: %s  |  SS Claim Age %d",
                  strategy_label(res.chosen_strategy), res.chosen_ss_age);
    gtk_label_set_text(priv->lbl_kpi_rec_val, rec_buf);

    populate_schedule_and_comparison(priv);

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
        std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_chart_mode)), 0, 2));
    const bool use_real = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(priv->chk_real_dollars));
    const double inf_rate = priv->last_input.inflation;

    const double pad_l = 72.0, pad_r = 20.0, pad_t = 34.0, pad_b = 36.0;
    const double plot_w = w - pad_l - pad_r;
    const double plot_h = h - pad_t - pad_b;
    if (plot_w <= 10 || plot_h <= 10)
        return FALSE;

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
        else
        {
            draw_seg(yp.ordinary_tax + yp.ltcg_tax + yp.niit_tax, 0.85, 0.22, 0.22);
            draw_seg(yp.state_tax,       0.75, 0.35, 0.55);
            draw_seg(yp.irmaa_surcharge + yp.aca_premium, 0.90, 0.45, 0.10);
            draw_seg(yp.rmd_amount,      0.90, 0.65, 0.04);
            draw_seg(yp.roth_conversion, 0.18, 0.76, 0.49);
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
    else
    {
        items = {
            { "Fed Tax",    0.85, 0.22, 0.22 },
            { "State Tax",  0.75, 0.35, 0.55 },
            { "IRMAA/ACA",  0.90, 0.45, 0.10 },
            { "RMD",        0.90, 0.65, 0.04 },
            { "Roth Conv",  0.18, 0.76, 0.49 },
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
    auto overrides = collect_account_overrides(priv);
    refresh_accounts_from_book(priv);
    apply_account_overrides_to_ui(priv, overrides);
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
                                             collect_account_overrides(priv));
        GError* err = nullptr;
        if (!g_file_set_contents(filename, ini.c_str(), static_cast<gssize>(ini.size()), &err))
        {
            show_error_dialog(parent, err ? err->message : _("Failed to save plan file."));
            if (err) g_error_free(err);
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
            if (deserialize_plan_ini(std::string(contents, length), loaded, overrides))
            {
                apply_plan_input_to_ui(priv, loaded);
                apply_account_overrides_to_ui(priv, overrides);
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

static void
on_bucket_edited(GtkCellRendererText*, gchar* path_str, gchar* new_text, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageRetirementPrivate*>(user_data);
    GtkTreePath* path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->acct_store), &iter, path))
    {
        int* indices = gtk_tree_path_get_indices(path);
        int idx = indices ? indices[0] : -1;
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
        if (idx >= 0 && idx < static_cast<int>(priv->accounts.size()))
            priv->accounts[static_cast<size_t>(idx)].bucket = chosen;

        gtk_list_store_set(priv->acct_store, &iter,
                           ACCT_COL_BUCKET_STR, bucket_label(chosen),
                           ACCT_COL_BUCKET_ID, static_cast<int>(chosen),
                           -1);
        update_bucket_summary_label(priv);
        run_planner(priv);
    }
    gtk_tree_path_free(path);
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

    // Top display options & actions bar (compact, never overflows)
    GtkWidget* top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(outer), top_bar, FALSE, FALSE, 0);

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

    GtkWidget* btn_export_csv = gtk_button_new_with_label(_("Export CSV..."));
    g_signal_connect(btn_export_csv, "clicked", G_CALLBACK(on_export_csv_clicked), priv);
    gtk_box_pack_end(GTK_BOX(top_bar), btn_export_csv, FALSE, FALSE, 0);

    GtkWidget* plan_menu = gtk_menu_new();
    GtkWidget* mi_save = gtk_menu_item_new_with_label(_("Save Plan As..."));
    GtkWidget* mi_load = gtk_menu_item_new_with_label(_("Load Plan..."));
    GtkWidget* mi_sep = gtk_separator_menu_item_new();
    GtkWidget* mi_reset = gtk_menu_item_new_with_label(_("Reset to Defaults"));
    g_signal_connect(mi_save, "activate", G_CALLBACK(on_save_plan_clicked), priv);
    g_signal_connect(mi_load, "activate", G_CALLBACK(on_load_plan_clicked), priv);
    g_signal_connect(mi_reset, "activate", G_CALLBACK(on_reset_defaults_clicked), priv);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_save);
    gtk_menu_shell_append(GTK_MENU_SHELL(plan_menu), mi_load);
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

    priv->spin_mc_trials = add_spin_row(g_mkt, r3, _("Monte Carlo Trials"), 50, 1000, 50, 300);

    gtk_paned_pack1(GTK_PANED(paned), left_nb, FALSE, TRUE);

    // Right panel: Notebook with Overview/Chart, Schedule, Baseline vs Owl, Account Mapping
    GtkWidget* nb = gtk_notebook_new();
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
    g_signal_connect(priv->chart_area, "draw", G_CALLBACK(on_chart_draw), priv);
    gtk_box_pack_start(GTK_BOX(tab_overview), GTK_WIDGET(priv->chart_area), TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), tab_overview, gtk_label_new(_("Overview & Chart")));

    // Tab 2: Year-by-Year Schedule
    priv->sched_store = gtk_list_store_new(NUM_SCHED_COLS,
        G_TYPE_INT, G_TYPE_INT,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget* sched_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->sched_store));
    const char* sched_titles[] = {
        "Year", "Age", "Total", "Taxable", "401k/IRA", "Roth",
        "HSA", "SS+Pens", "RMD", "Roth Conv", "Tax", "Net Spend"
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

    // Tab 4: Account Mapping (ellipsized account names so long fund names never blow up window width)
    priv->acct_store = gtk_list_store_new(NUM_ACCT_COLS,
        G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    GtkWidget* acct_tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->acct_store));

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
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), acct_scroll, gtk_label_new(_("Account Mapping")));

    priv->root_widget = outer;
    gtk_widget_show_all(priv->root_widget);

    refresh_accounts_from_book(priv);
    load_state_from_book(priv);
    run_planner(priv);
    return priv->root_widget;
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
        g_object_unref(priv->acct_store);
        priv->sched_store = nullptr;
        priv->cmp_store = nullptr;
        priv->acct_store = nullptr;
        priv->chart_area = nullptr;
        priv->left_panel = nullptr;
        priv->root_widget = nullptr;
    }
}
