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
    double nominal_return = 0.065;  // 6.5%
    double volatility = 0.12;       // 12% std dev for Monte Carlo
    double inflation = 0.025;       // 2.5%

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

struct PlanResult
{
    WithdrawalStrategy chosen_strategy = WithdrawalStrategy::StandardTaxableFirst;
    int chosen_ss_age = 67;

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

/** Run a single deterministic projection for the given input, strategy, and SS age. */
PlanResult simulate_plan(const PlanInput& input,
                         WithdrawalStrategy strategy,
                         int ss_claim_age,
                         double annual_spending_real);

/** Optimize withdrawal strategy, Roth conversions, and optionally SS claiming age. */
PlanResult optimize_plan(const PlanInput& input);

/** Run Monte Carlo stress-testing on an already-chosen strategy and spending level. */
MonteCarloSummary run_monte_carlo(const PlanInput& input,
                                  WithdrawalStrategy strategy,
                                  int ss_claim_age,
                                  double annual_spending_real);

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

/** Serialize PlanInput and optional account bucket overrides to an INI-format string. */
std::string serialize_plan_ini(
    const PlanInput& input,
    const std::vector<std::pair<std::string, TaxBucket>>& account_overrides = {});

/** Deserialize PlanInput and account bucket overrides from an INI-format string. */
bool deserialize_plan_ini(
    const std::string& ini_text,
    PlanInput& out_input,
    std::vector<std::pair<std::string, TaxBucket>>& out_overrides);

/** Export the year-by-year projection schedule to RFC-4180 CSV text. */
std::string export_schedule_csv(const PlanInput& input,
                                const PlanResult& result,
                                bool real_dollars = true);

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
