/********************************************************************
 * gnc-retirement-engine.hpp -- Owl-inspired standalone retirement  *
 * planning and tax-aware withdrawal optimization engine.           *
 *                                                                  *
 * Inspired by mdlacasse/Owl (https://github.com/mdlacasse/Owl,     *
 * GPL-3.0). Original C++17 implementation for GnuCash; see         *
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

#ifndef GNC_RETIREMENT_ENGINE_HPP
#define GNC_RETIREMENT_ENGINE_HPP

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gnc::retirement {

enum class TaxBucket
{
    Taxable = 0,
    TaxDeferred = 1,
    TaxFree = 2,
    HSA = 3,
    Excluded = 4
};

enum class FilingStatus
{
    Single = 0,
    MarriedFilingJointly = 1
};

enum class PlanObjective
{
    MaximizeSpending = 0,
    MaximizeBequest = 1
};

enum class WithdrawalStrategy
{
    AutoOptimize = 0,
    StandardTaxableFirst = 1,
    TaxDeferredFirst = 2,
    RothBracketFill12 = 3,
    RothBracketFill22 = 4,
    RothBracketFill24 = 5
};

enum class USState
{
    None = 0,
    AL, AK, AZ, AR, CA, CO, CT, DE, DC, FL,
    GA, HI, ID, IL, IN, IA, KS, KY, LA, ME,
    MD, MA, MI, MN, MS, MO, MT, NE, NV, NH,
    NJ, NM, NY, NC, ND, OH, OK, OR, PA, RI,
    SC, SD, TN, TX, UT, VT, VA, WA, WV, WI, WY
};

struct CashflowEvent
{
    std::string name;
    int age = 65;
    double amount_real = 0.0; // + = windfall/inflow, - = lump-sum expense
    TaxBucket bucket = TaxBucket::Taxable;
};

struct AccountBuckets
{
    double taxable = 0.0;
    double taxable_basis = 0.0; // Cost basis for capital gains estimation
    double tax_deferred = 0.0;  // 401(k), Traditional IRA, 403(b)
    double tax_free = 0.0;      // Roth IRA, Roth 401(k)
    double hsa = 0.0;           // Health Savings Account

    double total() const noexcept
    {
        return taxable + tax_deferred + tax_free + hsa;
    }
};

struct PlanInput
{
    int start_year = 2026;
    int current_age = 55;
    int retirement_age = 65;
    int horizon_age = 95;
    FilingStatus filing_status = FilingStatus::MarriedFilingJointly;

    AccountBuckets initial_balances;

    // Annual pre-retirement contributions (in today's dollars)
    double contrib_taxable = 10000.0;
    double contrib_tax_deferred = 23000.0;
    double contrib_tax_free = 7000.0;
    double contrib_hsa = 4150.0;

    // Goals (in today's dollars)
    PlanObjective objective = PlanObjective::MaximizeSpending;
    double target_annual_spending = 80000.0;
    double target_bequest = 100000.0;

    // Social Security & Pension (in today's dollars)
    double ss_fra_annual = 32000.0; // Annual benefit at age 67 (FRA)
    int ss_claim_age = 67;          // 62..70
    bool optimize_ss_age = true;    // Allow optimizer to search 62..70
    double pension_annual = 0.0;
    int pension_start_age = 65;
    bool pension_cola = true;

    // Market & Economic assumptions
    double nominal_return = 0.065;  // 6.5% blended (used as fallback for per-bucket rates)
    double volatility = 0.12;       // 12% std dev for Monte Carlo
    double inflation = 0.025;       // 2.5%

    // Per-bucket return overrides (0.0 = inherit nominal_return)
    double return_taxable     = 0.0; // Taxable brokerage expected return
    double return_tax_deferred= 0.0; // 401k/IRA expected return
    double return_tax_free    = 0.0; // Roth expected return
    double return_hsa         = 0.0; // HSA expected return

    // Cost-basis percentage for the taxable bucket (used for LTCG calculation)
    double taxable_basis_pct  = 0.70; // 70% = 30% embedded gain on initial taxable balance

    // Strategy
    WithdrawalStrategy strategy = WithdrawalStrategy::AutoOptimize;
    int rmd_start_age = 73;         // SECURE 2.0: 73 (or 75 for born 1960+)
    USState state = USState::None;

    // ACA Marketplace (pre-Medicare, retired & age < 65)
    bool enable_aca = true;
    double aca_benchmark_premium = 8500.0; // Annual SLCSP per adult (today's $)
    bool aca_enforce_400_cliff = true;

    int monte_carlo_trials = 300;
    uint32_t random_seed = 42;

    // Optional Spouse / Partner (Two-Person Household)
    bool has_spouse = false;
    int spouse_current_age = 53;
    int spouse_retirement_age = 65;
    int spouse_horizon_age = 95;
    double spouse_ss_fra_annual = 22000.0;
    int spouse_ss_claim_age = 67;
    bool spouse_optimize_ss_age = true;
    double spouse_pension_annual = 0.0;
    int spouse_pension_start_age = 65;
    bool spouse_pension_cola = true;
    int spouse_rmd_start_age = 73;
    double spouse_contrib_tax_deferred = 0.0;
    double spouse_contrib_tax_free = 0.0;

    // Spending Phases (Go-Go / Slow-Go / No-Go as % of base retirement spending)
    int slow_go_age = 75;
    double slow_go_pct = 100.0;
    int no_go_age = 85;
    double no_go_pct = 100.0;

    // One-time windfalls (+) or lump-sum expenses (-)
    std::vector<CashflowEvent> events;

    // RMD & Income Floor enhancements
    bool reinvest_rmd_surplus = true;  // Re-invest after-tax RMD/income surplus into taxable bucket
    double tips_floor_annual = 0.0;    // Optional inflation-adjusted TIPS/annuity floor (today's $)

    // Guyton-Klinger Guardrails (dynamic spending adjustment)
    bool enable_guardrails = false;
    double guardrail_upper_wr = 0.060; // Trim spending if withdrawal rate > 6.0%
    double guardrail_lower_wr = 0.040; // Boost spending if withdrawal rate < 4.0%
    double guardrail_adj_pct = 0.10;   // +/- 10% adjustment per trigger

    // Fixed Nominal Debt / Mortgage Payoff (non-inflating)
    double debt_annual_payment = 0.0;  // Fixed nominal $/yr
    int debt_payoff_age = 68;          // Stops when primary reaches this age

    // Long-Term Care (LTC) Late-Life Shock
    int ltc_start_age = 82;
    int ltc_years = 0;                 // 0 = disabled
    double ltc_annual_cost_real = 65000.0;

    // Asset Allocation Glide Path (optional)
    bool enable_glide_path = false;
    double equity_pct_pre_ret = 80.0;  // % equities before retirement
    double equity_pct_in_ret = 55.0;   // % equities in retirement
    double equity_return = 0.085;      // 8.5% nominal equity return
    double bond_return = 0.040;        // 4.0% nominal bond return
    double equity_vol = 0.16;          // 16% equity std dev
    double bond_vol = 0.05;            // 5% bond std dev
};

struct YearProjection
{
    int year = 0;
    int age = 0;
    bool is_retired = false;

    // Ending balances (nominal & real today's dollars)
    double end_taxable = 0.0;
    double end_tax_deferred = 0.0;
    double end_tax_free = 0.0;
    double end_hsa = 0.0;
    double end_total_nominal = 0.0;
    double end_total_real = 0.0;

    // Cash flows (nominal)
    double contributions = 0.0;
    double ss_income = 0.0;
    double pension_income = 0.0;
    double rmd_amount = 0.0;
    double roth_conversion = 0.0;

    double withdraw_taxable = 0.0;
    double withdraw_tax_deferred = 0.0;
    double withdraw_tax_free = 0.0;
    double withdraw_hsa = 0.0;

    // Taxes & Healthcare (nominal)
    double ordinary_tax = 0.0;
    double ltcg_tax = 0.0;
    double niit_tax = 0.0;
    double state_tax = 0.0;
    double irmaa_surcharge = 0.0;
    double aca_premium = 0.0;
    double total_tax = 0.0;

    // Net spending achieved (nominal & real)
    double net_spending_nominal = 0.0;
    double net_spending_real = 0.0;
    double lump_sum_net = 0.0;             // Net one-time event cashflow in year (nominal)
    double rmd_reinvested = 0.0;           // After-tax RMD/income surplus reinvested in taxable (nominal)
    double guaranteed_floor_nominal = 0.0; // SS + Pension + TIPS floor (nominal)
    double floor_surplus_nominal = 0.0;    // guaranteed_floor - target_spending (nominal; <0 = gap)
    int marginal_bracket_pct = 0;          // Federal ordinary marginal bracket (10..37%)
    int irmaa_tier = 0;                    // Medicare IRMAA tier (0 = none, 1..4)
    bool shortfall = false;
};

struct MonteCarloSummary
{
    int trials = 0;
    double success_rate = 0.0; // Fraction [0..1] finishing without shortfall
    double p10_end_real = 0.0;
    double p50_end_real = 0.0;
    double p90_end_real = 0.0;

    // Per-year portfolio percentiles in today's dollars (size = schedule.size())
    std::vector<double> p10_real;
    std::vector<double> p25_real;
    std::vector<double> p50_real;
    std::vector<double> p75_real;
    std::vector<double> p90_real;
};

struct StressScenarioResult
{
    std::string name;
    bool feasible = true;
    double min_spending_real = 0.0;
    double final_bequest_real = 0.0;
    int shortfall_years = 0;
};

struct SensitivityCell
{
    bool feasible = true;
    double final_bequest_real = 0.0;
    int shortfall_years = 0;
};

struct SensitivityMatrix
{
    std::vector<double> return_rates;    // 5 rows (nominal return)
    std::vector<double> spending_levels; // 5 cols (annual spending in today's $)
    std::vector<std::vector<SensitivityCell>> cells; // [row][col]
};

struct SsBreakEvenRow
{
    int claim_age = 67;
    double annual_benefit_real = 0.0;
    int breakeven_vs_62 = 0;                // Age where cumulative >= claiming at 62 (0 if never)
    int breakeven_vs_67 = 0;                // Age where cumulative >= claiming at 67 (0 if never)
    std::vector<double> cumulative_at_ages; // Cumulative real $ at ages {65,70,75,80,85,90,95}
};

struct HistoricalBacktestResult
{
    std::string name;
    int start_year = 1928;
    bool feasible = true;
    double min_spending_real = 0.0;
    double final_bequest_real = 0.0;
    int shortfall_years = 0;
};

struct HistoricalSummary
{
    int total_cycles = 0;
    double rolling_success_rate = 0.0;
    std::vector<HistoricalBacktestResult> scenarios; // Key historical crisis starts + worst cycle
};

struct PlanResult
{
    WithdrawalStrategy chosen_strategy = WithdrawalStrategy::StandardTaxableFirst;
    int chosen_ss_age = 67;
    int chosen_spouse_ss_age = 67;

    double sustainable_spending_real = 0.0;
    double final_bequest_real = 0.0;
    double lifetime_taxes_real = 0.0;

    // Baseline comparison (StandardTaxableFirst at user's SS age)
    double baseline_spending_real = 0.0;
    double baseline_bequest_real = 0.0;
    double baseline_taxes_real = 0.0;
    double advantage_real = 0.0; // Spending or bequest improvement in today's $

    bool plan_feasible = true;
    std::vector<YearProjection> schedule;
    MonteCarloSummary monte_carlo;
};

/** Classify an account name/description into an Owl TaxBucket. */
TaxBucket classify_account_name(const std::string& full_name,
                                const std::string& description = "");

/** IRS Uniform Lifetime Table RMD divisor for a given age. */
double rmd_divisor(int age) noexcept;

/** Compute Social Security annual benefit adjusted for claiming age (62..70). */
double adjusted_social_security(double fra_annual, int claim_age) noexcept;

/** Run a single deterministic projection for the given input, strategy, and SS age(s). */
PlanResult simulate_plan(const PlanInput& input,
                         WithdrawalStrategy strategy,
                         int ss_claim_age,
                         double annual_spending_real,
                         int spouse_ss_claim_age = -1);

/** Optimize withdrawal strategy, Roth conversions, and optionally SS claiming age. */
PlanResult optimize_plan(const PlanInput& input);

/** Run Monte Carlo stress-testing on an already-chosen strategy and spending level. */
MonteCarloSummary run_monte_carlo(const PlanInput& input,
                                  WithdrawalStrategy strategy,
                                  int ss_claim_age,
                                  double annual_spending_real,
                                  int spouse_ss_claim_age = -1);

/** Run sequence-of-returns and stagflation stress scenarios. */
std::vector<StressScenarioResult> run_sequence_stress_tests(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double annual_spending_real,
    int spouse_ss_claim_age = -1);

/** Compute a 5x5 Return vs. Spending sensitivity matrix. */
SensitivityMatrix run_sensitivity_matrix(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double base_spending_real,
    int spouse_ss_claim_age = -1);

/** Compute Social Security break-even analysis across claiming ages 62..70. */
std::vector<SsBreakEvenRow> compute_ss_breakeven(const PlanInput& input);

/** Run historical market backtests (rolling 1928-2024 cycles + major crisis starts). */
HistoricalSummary run_historical_backtests(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double annual_spending_real,
    int spouse_ss_claim_age = -1);

/** Human-readable name for a WithdrawalStrategy. */
const char* strategy_label(WithdrawalStrategy s) noexcept;

/** Human-readable name for a TaxBucket. */
const char* bucket_label(TaxBucket b) noexcept;

/** Two-letter code / label for a USState. */
const char* state_code(USState st) noexcept;

/** Parse a two-letter USState code (case-insensitive); returns USState::None if unknown. */
USState state_from_code(const std::string& code) noexcept;

/** Parse a TaxBucket label or integer string; returns fallback if unknown. */
TaxBucket bucket_from_label(const std::string& label,
                            TaxBucket fallback = TaxBucket::Taxable) noexcept;

/** Serialize PlanInput, optional account bucket overrides, and excluded accounts to an INI-format string. */
std::string serialize_plan_ini(
    const PlanInput& input,
    const std::vector<std::pair<std::string, TaxBucket>>& account_overrides = {},
    const std::vector<std::string>& excluded_accounts = {});

/** Deserialize PlanInput, account bucket overrides, and optional excluded accounts from an INI-format string. */
bool deserialize_plan_ini(
    const std::string& ini_text,
    PlanInput& out_input,
    std::vector<std::pair<std::string, TaxBucket>>& out_overrides,
    std::vector<std::string>* out_excluded = nullptr);

/** Export the year-by-year projection schedule to RFC-4180 CSV text. */
std::string export_schedule_csv(const PlanInput& input,
                                const PlanResult& result,
                                bool real_dollars = true);

/** Export a self-contained, print-ready HTML retirement report (with embedded SVG chart). */
std::string export_report_html(const PlanInput& input,
                               const PlanResult& result);

/** Compute state income tax for a single year. */
double compute_state_tax(USState st,
                         double ordinary_non_ss,
                         double ss_benefit,
                         double cap_gains,
                         double retirement_dist,
                         FilingStatus status,
                         int age,
                         double inf_factor) noexcept;

/** Compute net annual ACA marketplace premium after subsidies (0 if age >= 65). */
double compute_aca_premium(double magi,
                           FilingStatus status,
                           int age,
                           double benchmark_per_adult_real,
                           bool enforce_400_cliff,
                           double inf_factor) noexcept;

} // namespace gnc::retirement

#endif // GNC_RETIREMENT_ENGINE_HPP
