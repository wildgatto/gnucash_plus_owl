/********************************************************************
 * gnc-retirement-engine.cpp -- Owl-inspired standalone retirement  *
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

#include "gnc-retirement-engine.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <numeric>
#include <random>
#include <sstream>

namespace gnc::retirement {

namespace {

std::string to_lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains_any(const std::string& haystack,
                  std::initializer_list<const char*> needles)
{
    for (const char* n : needles)
    {
        if (haystack.find(n) != std::string::npos)
            return true;
    }
    return false;
}

struct TaxBracket
{
    double upper; // in base-year (2025/2026) dollars
    double rate;
};

// 2025/2026 Federal ordinary income brackets (base-year dollars, inflation-indexed)
constexpr std::array<TaxBracket, 7> kSingleOrdinary {{
    {  11925.0, 0.10 },
    {  48475.0, 0.12 },
    { 103350.0, 0.22 },
    { 197300.0, 0.24 },
    { 250525.0, 0.32 },
    { 626350.0, 0.35 },
    { std::numeric_limits<double>::infinity(), 0.37 }
}};

constexpr std::array<TaxBracket, 7> kMfjOrdinary {{
    {  23850.0, 0.10 },
    {  96950.0, 0.12 },
    { 206700.0, 0.22 },
    { 394600.0, 0.24 },
    { 501050.0, 0.32 },
    { 751600.0, 0.35 },
    { std::numeric_limits<double>::infinity(), 0.37 }
}};

// 2025/2026 LTCG brackets
constexpr std::array<TaxBracket, 3> kSingleLtcg {{
    {  48350.0, 0.00 },
    { 533400.0, 0.15 },
    { std::numeric_limits<double>::infinity(), 0.20 }
}};

constexpr std::array<TaxBracket, 3> kMfjLtcg {{
    {  96700.0, 0.00 },
    { 600050.0, 0.15 },
    { std::numeric_limits<double>::infinity(), 0.20 }
}};

double standard_deduction(FilingStatus status, int age, double inf_factor)
{
    double base = (status == FilingStatus::MarriedFilingJointly) ? 30000.0 : 15000.0;
    if (age >= 65)
        base += (status == FilingStatus::MarriedFilingJointly) ? 3200.0 : 2000.0;
    return base * inf_factor;
}

double bracket_ceiling_taxable(FilingStatus status, double target_rate, double inf_factor)
{
    const auto& table = (status == FilingStatus::MarriedFilingJointly)
                        ? kMfjOrdinary : kSingleOrdinary;
    for (const auto& b : table)
    {
        if (std::abs(b.rate - target_rate) < 1e-6)
            return b.upper * inf_factor;
    }
    return 0.0;
}

double compute_ordinary_tax(double taxable_ordinary, FilingStatus status, double inf_factor)
{
    if (taxable_ordinary <= 0.0)
        return 0.0;
    const auto& table = (status == FilingStatus::MarriedFilingJointly)
                        ? kMfjOrdinary : kSingleOrdinary;
    double tax = 0.0;
    double prev = 0.0;
    for (const auto& b : table)
    {
        double upper = std::isinf(b.upper) ? b.upper : b.upper * inf_factor;
        if (taxable_ordinary <= prev)
            break;
        double slice = std::min(taxable_ordinary, upper) - prev;
        tax += slice * b.rate;
        prev = upper;
    }
    return tax;
}

double compute_ltcg_tax(double taxable_ordinary, double cap_gains,
                        FilingStatus status, double inf_factor)
{
    if (cap_gains <= 0.0)
        return 0.0;
    const auto& table = (status == FilingStatus::MarriedFilingJointly)
                        ? kMfjLtcg : kSingleLtcg;
    double tax = 0.0;
    double start = std::max(0.0, taxable_ordinary);
    double end = start + cap_gains;
    double prev = 0.0;
    for (const auto& b : table)
    {
        double upper = std::isinf(b.upper) ? b.upper : b.upper * inf_factor;
        double overlap_lo = std::max(start, prev);
        double overlap_hi = std::min(end, upper);
        if (overlap_hi > overlap_lo)
            tax += (overlap_hi - overlap_lo) * b.rate;
        prev = upper;
    }
    return tax;
}

double compute_niit(double magi, double investment_income,
                    FilingStatus status, double inf_factor)
{
    // NIIT threshold is not inflation-indexed by statute, though we keep statutory $200k/$250k
    (void)inf_factor;
    double threshold = (status == FilingStatus::MarriedFilingJointly) ? 250000.0 : 200000.0;
    if (magi <= threshold || investment_income <= 0.0)
        return 0.0;
    return 0.038 * std::min(investment_income, magi - threshold);
}

// Approximate annual Medicare Part B + D IRMAA surcharge per person (x2 for MFJ)
double compute_irmaa(double magi, FilingStatus status, int age, double inf_factor)
{
    if (age < 65)
        return 0.0;
    bool mfj = (status == FilingStatus::MarriedFilingJointly);
    double mult = mfj ? 2.0 : 1.0;
    double t1 = (mfj ? 212000.0 : 106000.0) * inf_factor;
    double t2 = (mfj ? 266000.0 : 133000.0) * inf_factor;
    double t3 = (mfj ? 334000.0 : 167000.0) * inf_factor;
    double t4 = (mfj ? 400000.0 : 200000.0) * inf_factor;

    double per_person = 0.0;
    if (magi > t4)      per_person = 5500.0;
    else if (magi > t3) per_person = 4000.0;
    else if (magi > t2) per_person = 2500.0;
    else if (magi > t1) per_person = 1050.0;
    return per_person * mult * inf_factor;
}

double taxable_ss_portion(double ss_benefit, double other_agi, double cap_gains,
                          FilingStatus status)
{
    if (ss_benefit <= 0.0)
        return 0.0;
    double provisional = other_agi + cap_gains + 0.5 * ss_benefit;
    bool mfj = (status == FilingStatus::MarriedFilingJointly);
    double t1 = mfj ? 32000.0 : 25000.0;
    double t2 = mfj ? 44000.0 : 34000.0;
    if (provisional <= t1)
        return 0.0;
    if (provisional <= t2)
        return std::min(0.5 * ss_benefit, 0.5 * (provisional - t1));
    double base_part = 0.5 * (t2 - t1);
    return std::min(0.85 * ss_benefit, base_part + 0.85 * (provisional - t2));
}

struct TaxBreakdown
{
    double ordinary = 0.0;
    double ltcg = 0.0;
    double niit = 0.0;
    double state = 0.0;
    double irmaa = 0.0;
    double aca = 0.0;
    int marginal_bracket_pct = 10;
    int irmaa_tier = 0;
    double total() const noexcept { return ordinary + ltcg + niit + state + irmaa + aca; }
};

int marginal_ordinary_pct(double taxable_ordinary, FilingStatus status, double inf_factor)
{
    const auto& table = (status == FilingStatus::MarriedFilingJointly)
                        ? kMfjOrdinary : kSingleOrdinary;
    for (const auto& b : table)
    {
        double upper = std::isinf(b.upper) ? b.upper : b.upper * inf_factor;
        if (taxable_ordinary <= upper)
            return static_cast<int>(std::lround(b.rate * 100.0));
    }
    return 37;
}

int irmaa_tier_from_magi(double magi, FilingStatus status, int age, double inf_factor)
{
    if (age < 65)
        return 0;
    bool mfj = (status == FilingStatus::MarriedFilingJointly);
    double t1 = (mfj ? 212000.0 : 106000.0) * inf_factor;
    double t2 = (mfj ? 266000.0 : 133000.0) * inf_factor;
    double t3 = (mfj ? 334000.0 : 167000.0) * inf_factor;
    double t4 = (mfj ? 400000.0 : 200000.0) * inf_factor;
    if (magi > t4) return 4;
    if (magi > t3) return 3;
    if (magi > t2) return 2;
    if (magi > t1) return 1;
    return 0;
}

std::pair<double, double> glide_path_return_vol(const PlanInput& input, int age) noexcept
{
    if (!input.enable_glide_path)
        return { input.nominal_return, input.volatility };
    double eq_pct = (age < input.retirement_age)
                    ? input.equity_pct_pre_ret
                    : input.equity_pct_in_ret;
    double eq = std::clamp(eq_pct / 100.0, 0.0, 1.0);
    double bd = 1.0 - eq;
    double ret = eq * input.equity_return + bd * input.bond_return;
    double var = eq * eq * input.equity_vol * input.equity_vol
               + bd * bd * input.bond_vol * input.bond_vol
               + 2.0 * 0.20 * eq * bd * input.equity_vol * input.bond_vol;
    return { ret, std::sqrt(std::max(0.0, var)) };
}

TaxBreakdown compute_year_taxes(const PlanInput& input,
                                FilingStatus year_status,
                                double ordinary_non_ss, double ss_benefit,
                                double cap_gains, double retirement_dist,
                                bool retired, int age, int aca_adults,
                                double inf_factor)
{
    double ss_taxable = taxable_ss_portion(ss_benefit, ordinary_non_ss, cap_gains, year_status);
    double agi = ordinary_non_ss + ss_taxable + cap_gains;
    double ded = standard_deduction(year_status, age, inf_factor);
    double taxable_ord = std::max(0.0, ordinary_non_ss + ss_taxable - ded);
    double leftover_ded = std::max(0.0, ded - (ordinary_non_ss + ss_taxable));
    double taxable_cg = std::max(0.0, cap_gains - leftover_ded);

    TaxBreakdown tb;
    tb.ordinary = compute_ordinary_tax(taxable_ord, year_status, inf_factor);
    tb.ltcg = compute_ltcg_tax(taxable_ord, taxable_cg, year_status, inf_factor);
    tb.niit = compute_niit(agi, cap_gains, year_status, inf_factor);
    tb.state = compute_state_tax(input.state, ordinary_non_ss, ss_benefit, cap_gains,
                                 retirement_dist, year_status, age, inf_factor);
    tb.irmaa = compute_irmaa(agi, year_status, age, inf_factor);
    tb.marginal_bracket_pct = marginal_ordinary_pct(taxable_ord, year_status, inf_factor);
    tb.irmaa_tier = irmaa_tier_from_magi(agi, year_status, age, inf_factor);
    if (retired && input.enable_aca && aca_adults > 0)
    {
        FilingStatus aca_status = (aca_adults >= 2) ? FilingStatus::MarriedFilingJointly : FilingStatus::Single;
        tb.aca = compute_aca_premium(agi, aca_status, 60,
                                     input.aca_benchmark_premium,
                                     input.aca_enforce_400_cliff, inf_factor);
    }
    return tb;
}

double roth_conversion_target_rate(WithdrawalStrategy s) noexcept
{
    switch (s)
    {
        case WithdrawalStrategy::RothBracketFill12: return 0.12;
        case WithdrawalStrategy::RothBracketFill22: return 0.22;
        case WithdrawalStrategy::RothBracketFill24: return 0.24;
        default: return 0.0;
    }
}

int plan_num_years(const PlanInput& input) noexcept
{
    int yrs = input.horizon_age - input.current_age + 1;
    if (input.has_spouse)
        yrs = std::max(yrs, input.spouse_horizon_age - input.spouse_current_age + 1);
    return std::max(1, yrs);
}

PlanResult simulate_with_returns(const PlanInput& input,
                                 WithdrawalStrategy strategy,
                                 int ss_claim_age,
                                 int spouse_ss_claim_age,
                                 double annual_spending_real,
                                 const std::vector<double>& annual_returns)
{
    PlanResult res;
    res.chosen_strategy = strategy;
    res.chosen_ss_age = ss_claim_age;
    res.chosen_spouse_ss_age = spouse_ss_claim_age;

    const int n_years = plan_num_years(input);
    res.schedule.reserve(static_cast<size_t>(n_years));

    double bal_tax = std::max(0.0, input.initial_balances.taxable);
    // Prefer explicitly-set basis over auto-computed basis; fall back to taxable_basis_pct
    double bal_basis = (input.initial_balances.taxable_basis > 0.0)
        ? std::clamp(input.initial_balances.taxable_basis, 0.0, bal_tax)
        : bal_tax * std::clamp(input.taxable_basis_pct, 0.0, 1.0);
    double bal_def = std::max(0.0, input.initial_balances.tax_deferred);
    double bal_roth = std::max(0.0, input.initial_balances.tax_free);
    double bal_hsa = std::max(0.0, input.initial_balances.hsa);

    // Resolve effective per-bucket returns (0 -> inherit nominal_return)
    auto effective_ret = [&](double bucket_rate, double year_ret) -> double {
        // year_ret is the nominal_return-based draw; scale by bucket rate / nominal_return
        // when bucket rate != 0.  For Monte Carlo, year_ret carries the volatility shock.
        if (bucket_rate <= 0.0 || input.nominal_return <= 0.0)
            return year_ret;
        double ratio = bucket_rate / input.nominal_return;
        // Preserve sign: shift from nominal to bucket by scaling the return
        return ratio * year_ret + (bucket_rate - input.nominal_return * ratio);
    };

    double cum_taxes_real = 0.0;
    double min_spending_real = std::numeric_limits<double>::infinity();
    bool any_shortfall = false;
    double guardrail_mult = 1.0;

    for (int i = 0; i < n_years; ++i)
    {
        const int age = input.current_age + i;
        const int sp_age = input.spouse_current_age + i;
        const bool p_alive = (age <= input.horizon_age);
        const bool s_alive = input.has_spouse && (sp_age <= input.spouse_horizon_age);
        const int year = input.start_year + i;
        const double inf = std::pow(1.0 + input.inflation, i);
        const bool retired = (!p_alive || age >= input.retirement_age);
        const double ret = (i < static_cast<int>(annual_returns.size()))
                           ? annual_returns[static_cast<size_t>(i)]
                           : input.nominal_return;

        const FilingStatus year_status = (p_alive && s_alive)
            ? input.filing_status
            : (input.has_spouse ? FilingStatus::Single : input.filing_status);
        const int tax_age = std::max(p_alive ? age : 0, s_alive ? sp_age : 0);
        const int aca_adults = (p_alive && age < 65 ? 1 : 0) + (s_alive && sp_age < 65 ? 1 : 0);

        YearProjection yp;
        yp.year = year;
        yp.age = age;
        yp.is_retired = retired;

        // 1. Pre-retirement contributions
        double c_total = 0.0;
        if (p_alive && age < input.retirement_age)
        {
            double c_tax = input.contrib_taxable * inf;
            double c_def = input.contrib_tax_deferred * inf;
            double c_roth = input.contrib_tax_free * inf;
            double c_hsa = input.contrib_hsa * inf;
            bal_tax += c_tax;
            bal_basis += c_tax;
            bal_def += c_def;
            bal_roth += c_roth;
            bal_hsa += c_hsa;
            c_total += c_tax + c_def + c_roth + c_hsa;
        }
        if (s_alive && sp_age < input.spouse_retirement_age)
        {
            double sc_def = input.spouse_contrib_tax_deferred * inf;
            double sc_roth = input.spouse_contrib_tax_free * inf;
            bal_def += sc_def;
            bal_roth += sc_roth;
            c_total += sc_def + sc_roth;
        }
        yp.contributions = c_total;

        // 2. Fixed income: Social Security (with survivor benefit) & Pension(s)
        const double p_ss_full = (age >= ss_claim_age)
            ? adjusted_social_security(input.ss_fra_annual, ss_claim_age) * inf : 0.0;
        const double s_ss_full = (input.has_spouse && sp_age >= spouse_ss_claim_age)
            ? adjusted_social_security(input.spouse_ss_fra_annual, spouse_ss_claim_age) * inf : 0.0;
        double ss = 0.0;
        if (p_alive && s_alive)
            ss = p_ss_full + s_ss_full;
        else if (p_alive)
            ss = input.has_spouse ? std::max(p_ss_full, s_ss_full) : p_ss_full;
        else if (s_alive)
            ss = std::max(p_ss_full, s_ss_full);

        double pension = 0.0;
        if (p_alive && age >= input.pension_start_age && input.pension_annual > 0.0)
        {
            double p_inf = input.pension_cola ? inf
                : std::pow(1.0 + input.inflation, std::max(0, input.pension_start_age - input.current_age));
            pension += input.pension_annual * p_inf;
        }
        if (s_alive && sp_age >= input.spouse_pension_start_age && input.spouse_pension_annual > 0.0)
        {
            double sp_inf = input.spouse_pension_cola ? inf
                : std::pow(1.0 + input.inflation, std::max(0, input.spouse_pension_start_age - input.spouse_current_age));
            pension += input.spouse_pension_annual * sp_inf;
        }
        yp.ss_income = ss;
        yp.pension_income = pension;

        // 3. Required Minimum Distribution (RMD)
        double rmd = 0.0;
        if (bal_def > 0.0)
        {
            if (input.has_spouse && p_alive && s_alive)
            {
                if (age >= input.rmd_start_age)
                    rmd += (0.5 * bal_def) / rmd_divisor(age);
                if (sp_age >= input.spouse_rmd_start_age)
                    rmd += (0.5 * bal_def) / rmd_divisor(sp_age);
            }
            else if (p_alive && age >= input.rmd_start_age)
            {
                rmd = bal_def / rmd_divisor(age);
            }
            else if (s_alive && sp_age >= input.spouse_rmd_start_age)
            {
                rmd = bal_def / rmd_divisor(sp_age);
            }
            rmd = std::min(bal_def, rmd);
        }
        yp.rmd_amount = rmd;

        // 4. Withdrawals & Roth Conversions
        double w_tax = 0.0;
        double w_def = rmd;
        double w_roth = 0.0;
        double w_hsa = 0.0;
        double roth_conv = 0.0;

        auto gain_ratio = [&]() -> double {
            if (bal_tax <= 1e-6) return 0.0;
            return std::clamp((bal_tax - bal_basis) / bal_tax, 0.0, 1.0);
        };

        auto draw_taxable = [&](double gross_needed) -> double {
            double take = std::min(bal_tax, std::max(0.0, gross_needed));
            if (take > 0.0 && bal_tax > 1e-9)
            {
                double frac = take / bal_tax;
                bal_basis *= (1.0 - frac);
                bal_tax -= take;
                w_tax += take;
            }
            return take;
        };

        auto draw_deferred = [&](double gross_needed) -> double {
            double avail = std::max(0.0, bal_def - w_def);
            double take = std::min(avail, std::max(0.0, gross_needed));
            w_def += take;
            return take;
        };

        auto draw_hsa = [&](double gross_needed) -> double {
            double take = std::min(bal_hsa, std::max(0.0, gross_needed));
            bal_hsa -= take;
            w_hsa += take;
            return take;
        };

        auto draw_roth = [&](double gross_needed) -> double {
            double take = std::min(bal_roth, std::max(0.0, gross_needed));
            bal_roth -= take;
            w_roth += take;
            return take;
        };

        // Apply RMD from tax-deferred first
        bal_def -= w_def;

        // 3b. One-time lump-sum events at this age
        for (const auto& ev : input.events)
        {
            if (ev.age != age || std::abs(ev.amount_real) < 1e-6)
                continue;
            double amt = ev.amount_real * inf;
            yp.lump_sum_net += amt;
            if (amt >= 0.0)
            {
                switch (ev.bucket)
                {
                    case TaxBucket::TaxFree:     bal_roth += amt; break;
                    case TaxBucket::TaxDeferred: bal_def += amt; break;
                    case TaxBucket::HSA:         bal_hsa += amt; break;
                    default:                     bal_tax += amt; bal_basis += amt; break;
                }
            }
            else
            {
                double need = -amt;
                if (ev.bucket == TaxBucket::TaxFree)          need -= draw_roth(need);
                else if (ev.bucket == TaxBucket::HSA)         need -= draw_hsa(need);
                else if (ev.bucket == TaxBucket::TaxDeferred) { double td = std::min(bal_def, need); bal_def -= td; w_def += td; need -= td; }
                else                                          need -= draw_taxable(need);

                if (need > 1.0) need -= draw_taxable(need);
                if (need > 1.0) { double td = std::min(bal_def, need); bal_def -= td; w_def += td; need -= td; }
                if (need > 1.0) need -= draw_hsa(need);
                if (need > 1.0) need -= draw_roth(need);
                if (need > 1.0) { yp.shortfall = true; any_shortfall = true; }
            }
        }

        double spend_mult = 1.0;
        if (age >= input.no_go_age)
            spend_mult = std::clamp(input.no_go_pct / 100.0, 0.1, 2.0);
        else if (age >= input.slow_go_age)
            spend_mult = std::clamp(input.slow_go_pct / 100.0, 0.1, 2.0);

        const double tips = (retired && input.tips_floor_annual > 0.0)
                            ? input.tips_floor_annual * inf : 0.0;
        const double debt_nom = (retired && p_alive && input.debt_annual_payment > 0.0 && age < input.debt_payoff_age)
                            ? input.debt_annual_payment : 0.0;
        const double ltc_nom = (retired && p_alive && input.ltc_years > 0
                                && age >= input.ltc_start_age
                                && age < input.ltc_start_age + input.ltc_years)
                            ? (input.ltc_annual_cost_real * inf) : 0.0;

        if (retired && input.enable_guardrails && annual_spending_real * spend_mult > 0.0)
        {
            double port_total = bal_tax + bal_def + bal_roth + bal_hsa;
            double base_need = std::max(0.0, annual_spending_real * spend_mult * guardrail_mult * inf - (ss + pension + tips));
            double cur_wr = (port_total > 1.0) ? (base_need / port_total) : 1.0;
            if (cur_wr > input.guardrail_upper_wr && (input.horizon_age - age) > 5)
                guardrail_mult = std::max(0.60, guardrail_mult * (1.0 - input.guardrail_adj_pct));
            else if (cur_wr < input.guardrail_lower_wr)
                guardrail_mult = std::min(1.50, guardrail_mult * (1.0 + input.guardrail_adj_pct));
        }

        const double target_net_spend = retired
            ? (annual_spending_real * spend_mult * guardrail_mult * inf + debt_nom + ltc_nom)
            : 0.0;
        yp.guaranteed_floor_nominal = ss + pension + tips;
        yp.floor_surplus_nominal = retired ? (yp.guaranteed_floor_nominal - target_net_spend) : 0.0;

        if (retired)
        {
            const double target_net = target_net_spend;

            // Fixed-point / iterative gross-up (converges in 4-6 iterations)
            for (int iter = 0; iter < 6; ++iter)
            {
                double ord_inc = pension + tips + w_def + roth_conv;
                double cg = w_tax * gain_ratio();
                TaxBreakdown tb = compute_year_taxes(input, year_status, ord_inc, ss, cg, ord_inc, true, tax_age, aca_adults, inf);
                double event_outflow = std::max(0.0, -yp.lump_sum_net);
                double current_net = ss + pension + tips + w_tax + w_def + w_hsa + w_roth - event_outflow - tb.total();
                double gap = target_net - current_net;
                if (gap <= 1.0)
                    break;

                // Draw in strategy order (inflate gap slightly for marginal tax)
                double rem = gap * 1.12;
                if (strategy == WithdrawalStrategy::TaxDeferredFirst)
                {
                    // Take from tax-deferred (excluding already-taken w_def)
                    double take_d = std::min(bal_def, rem);
                    bal_def -= take_d;
                    w_def += take_d;
                    rem -= take_d;

                    if (rem > 0.0) rem -= draw_taxable(rem);
                    if (rem > 0.0) rem -= draw_hsa(rem);
                    if (rem > 0.0) rem -= draw_roth(rem);
                }
                else
                {
                    // StandardTaxableFirst or RothBracketFill*
                    rem -= draw_taxable(rem);
                    if (rem > 0.0)
                    {
                        double take_d = std::min(bal_def, rem);
                        bal_def -= take_d;
                        w_def += take_d;
                        rem -= take_d;
                    }
                    if (rem > 0.0) rem -= draw_hsa(rem);
                    if (rem > 0.0) rem -= draw_roth(rem);
                }
            }
            (void)draw_deferred; // helper kept for clarity

            // Roth conversion to fill target bracket (if strategy requests it and funds exist)
            double conv_rate = roth_conversion_target_rate(strategy);
            if (conv_rate > 0.0 && bal_def > 0.0)
            {
                double ded = standard_deduction(year_status, tax_age, inf);
                double ceiling = bracket_ceiling_taxable(year_status, conv_rate, inf);
                double ss_tax = taxable_ss_portion(ss, pension + w_def, w_tax * gain_ratio(),
                                                   year_status);
                double current_taxable_ord = std::max(0.0, pension + w_def + ss_tax - ded);
                double headroom = std::max(0.0, ceiling - current_taxable_ord);
                double conv = std::min(bal_def, headroom);

                // Pay conversion tax from taxable if available, otherwise cap conversion
                if (conv > 0.0)
                {
                    TaxBreakdown tb_before = compute_year_taxes(
                        input, year_status, pension + w_def, ss, w_tax * gain_ratio(), pension + w_def, true, tax_age, aca_adults, inf);
                    TaxBreakdown tb_after = compute_year_taxes(
                        input, year_status, pension + w_def + conv, ss, w_tax * gain_ratio(), pension + w_def + conv, true, tax_age, aca_adults, inf);
                    double extra_tax = std::max(0.0, tb_after.total() - tb_before.total());
                    if (extra_tax <= bal_tax)
                    {
                        draw_taxable(extra_tax);
                        bal_def -= conv;
                        bal_roth += conv;
                        roth_conv = conv;
                    }
                    else if (bal_tax > 0.0)
                    {
                        // Scale conversion so taxable can cover the tax
                        double scale = bal_tax / extra_tax;
                        double scaled_conv = conv * scale * 0.95;
                        bal_def -= scaled_conv;
                        bal_roth += scaled_conv;
                        roth_conv = scaled_conv;
                        draw_taxable(bal_tax);
                    }
                }
            }
        }

        // Compute final taxes for the year
        double cap_gains = w_tax * gain_ratio();
        double ord_inc = pension + tips + w_def + roth_conv;
        TaxBreakdown tb = compute_year_taxes(input, year_status, ord_inc, ss, cap_gains, ord_inc, retired, tax_age, aca_adults, inf);

        // Exclude one-time lump-sum expense outflows from regular spending / RMD surplus
        double event_outflow = std::max(0.0, -yp.lump_sum_net);
        double gross_inflow = ss + pension + tips + w_tax + w_def + w_hsa + w_roth - event_outflow;
        double net_spend = retired ? std::max(0.0, gross_inflow - tb.total()) : 0.0;
        if (retired && input.reinvest_rmd_surplus && net_spend > target_net_spend)
        {
            double surplus = net_spend - target_net_spend;
            bal_tax += surplus;
            bal_basis += surplus;
            yp.rmd_reinvested = surplus;
            net_spend = target_net_spend;
        }
        double net_spend_real = net_spend / inf;

        if (retired && net_spend_real + 1.0 < annual_spending_real * spend_mult)
        {
            yp.shortfall = true;
            any_shortfall = true;
        }
        if (retired && spend_mult > 0.0)
            min_spending_real = std::min(min_spending_real, net_spend_real / spend_mult);

        // 5. Apply market return to remaining end-of-year balances (per-bucket rates)
        bal_tax  *= (1.0 + effective_ret(input.return_taxable,      ret));
        bal_def  *= (1.0 + effective_ret(input.return_tax_deferred, ret));
        bal_roth *= (1.0 + effective_ret(input.return_tax_free,     ret));
        bal_hsa  *= (1.0 + effective_ret(input.return_hsa,          ret));
        // Grow basis at the same rate as the taxable bucket (maintains gain ratio)
        if (bal_tax > 1e-9)
            bal_basis *= (1.0 + effective_ret(input.return_taxable, ret));

        double total_nom = bal_tax + bal_def + bal_roth + bal_hsa;
        double total_real = total_nom / inf;
        cum_taxes_real += tb.total() / inf;

        yp.end_taxable = bal_tax;
        yp.end_tax_deferred = bal_def;
        yp.end_tax_free = bal_roth;
        yp.end_hsa = bal_hsa;
        yp.end_total_nominal = total_nom;
        yp.end_total_real = total_real;

        yp.roth_conversion = roth_conv;
        yp.withdraw_taxable = w_tax;
        yp.withdraw_tax_deferred = w_def;
        yp.withdraw_tax_free = w_roth;
        yp.withdraw_hsa = w_hsa;

        yp.ordinary_tax = tb.ordinary;
        yp.ltcg_tax = tb.ltcg;
        yp.niit_tax = tb.niit;
        yp.state_tax = tb.state;
        yp.irmaa_surcharge = tb.irmaa;
        yp.aca_premium = tb.aca;
        yp.total_tax = tb.total();
        yp.marginal_bracket_pct = tb.marginal_bracket_pct;
        yp.irmaa_tier = tb.irmaa_tier;

        yp.net_spending_nominal = net_spend;
        yp.net_spending_real = net_spend_real;

        res.schedule.push_back(yp);
    }

    const double last_inf = std::pow(1.0 + input.inflation, std::max(0, n_years - 1));
    // Approximate after-tax bequest (tax-deferred discounted by ~18% estate/heir tax)
    double after_tax_bequest_nom = bal_tax + bal_roth + bal_hsa * 0.85 + bal_def * 0.82;
    res.final_bequest_real = after_tax_bequest_nom / last_inf;
    res.lifetime_taxes_real = cum_taxes_real;
    res.sustainable_spending_real = std::isfinite(min_spending_real)
                                    ? min_spending_real
                                    : annual_spending_real;
    res.plan_feasible = !any_shortfall && (res.final_bequest_real + 1.0 >= input.target_bequest);
    return res;
}

double find_max_spending(const PlanInput& input, WithdrawalStrategy s, int ss_age, int spouse_ss_age)
{
    double lo = 0.0;
    double hi = std::max(50000.0, input.initial_balances.total() * 0.25 + input.ss_fra_annual +
                         input.spouse_ss_fra_annual + input.pension_annual + input.spouse_pension_annual + 100000.0);
    for (int iter = 0; iter < 32; ++iter)
    {
        double mid = 0.5 * (lo + hi);
        PlanResult r = simulate_plan(input, s, ss_age, mid, spouse_ss_age);
        if (r.plan_feasible)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

} // namespace

TaxBucket classify_account_name(const std::string& full_name,
                                const std::string& description)
{
    std::string text = to_lower(full_name + " " + description);
    if (contains_any(text, {"roth", "tax-free", "tax free"}))
        return TaxBucket::TaxFree;
    if (contains_any(text, {"hsa", "health savings"}))
        return TaxBucket::HSA;
    if (contains_any(text, {"401k", "401(k)", "403b", "403(b)", "457",
                            "ira", "tsp", "sep", "simple", "pension",
                            "deferred", "retire"}))
        return TaxBucket::TaxDeferred;
    return TaxBucket::Taxable;
}

double rmd_divisor(int age) noexcept
{
    // IRS Uniform Lifetime Table (effective 2022+) for ages 72..100+
    static constexpr double kTable[] = {
        27.4, 26.5, 25.5, 24.6, 23.7, 22.9, 22.0, 21.1, 20.2, 19.4, // 72..81
        18.5, 17.7, 16.8, 16.0, 15.2, 14.4, 13.7, 12.9, 12.2, 11.5, // 82..91
        10.8, 10.1,  9.5,  8.9,  8.4,  7.8,  7.3,  6.8,  6.4        // 92..100
    };
    if (age < 72) return 27.4;
    int idx = age - 72;
    if (idx < static_cast<int>(sizeof(kTable) / sizeof(kTable[0])))
        return kTable[idx];
    return std::max(2.0, 6.4 - 0.3 * (age - 100));
}

double adjusted_social_security(double fra_annual, int claim_age) noexcept
{
    int age = std::clamp(claim_age, 62, 70);
    int diff = age - 67; // FRA = 67
    if (diff == 0)
        return fra_annual;
    if (diff > 0)
        return fra_annual * (1.0 + 0.08 * diff);
    // Early claiming: -6.67%/yr for first 3 years, -5%/yr for remaining up to 2 years
    int early_years = -diff;
    double reduction = 0.0;
    if (early_years <= 3)
        reduction = early_years * (20.0 / 300.0);
    else
        reduction = 3.0 * (20.0 / 300.0) + (early_years - 3) * 0.05;
    return fra_annual * (1.0 - reduction);
}

PlanResult simulate_plan(const PlanInput& input,
                         WithdrawalStrategy strategy,
                         int ss_claim_age,
                         double annual_spending_real,
                         int spouse_ss_claim_age)
{
    const int sp_ss = (spouse_ss_claim_age >= 62 && spouse_ss_claim_age <= 70)
                      ? spouse_ss_claim_age : input.spouse_ss_claim_age;
    const int n_years = plan_num_years(input);
    std::vector<double> returns(static_cast<size_t>(n_years), input.nominal_return);
    if (input.enable_glide_path)
    {
        for (int i = 0; i < n_years; ++i)
            returns[static_cast<size_t>(i)] = glide_path_return_vol(input, input.current_age + i).first;
    }
    return simulate_with_returns(input, strategy, ss_claim_age, sp_ss, annual_spending_real, returns);
}

MonteCarloSummary run_monte_carlo(const PlanInput& input,
                                  WithdrawalStrategy strategy,
                                  int ss_claim_age,
                                  double annual_spending_real,
                                  int spouse_ss_claim_age)
{
    const int sp_ss = (spouse_ss_claim_age >= 62 && spouse_ss_claim_age <= 70)
                      ? spouse_ss_claim_age : input.spouse_ss_claim_age;
    MonteCarloSummary mc;
    mc.trials = std::max(1, input.monte_carlo_trials);
    const int n_years = plan_num_years(input);

    std::mt19937 rng(input.random_seed);
    std::normal_distribution<double> dist(input.nominal_return, std::max(0.0, input.volatility));

    std::vector<double> end_balances;
    end_balances.reserve(static_cast<size_t>(mc.trials));
    std::vector<std::vector<double>> yearly_real(static_cast<size_t>(n_years));
    for (auto& yr : yearly_real)
        yr.reserve(static_cast<size_t>(mc.trials));
    std::vector<double> returns(static_cast<size_t>(n_years));

    int successes = 0;
    for (int t = 0; t < mc.trials; ++t)
    {
        for (int y = 0; y < n_years; ++y)
        {
            if (input.enable_glide_path)
            {
                auto [gp_ret, gp_vol] = glide_path_return_vol(input, input.current_age + y);
                std::normal_distribution<double> gp_dist(gp_ret, gp_vol);
                returns[static_cast<size_t>(y)] = std::clamp(gp_dist(rng), -0.60, 0.80);
            }
            else
            {
                returns[static_cast<size_t>(y)] = std::clamp(dist(rng), -0.60, 0.80);
            }
        }

        PlanResult r = simulate_with_returns(
            input, strategy, ss_claim_age, sp_ss, annual_spending_real, returns);
        if (!r.schedule.empty())
        {
            end_balances.push_back(r.final_bequest_real);
            const size_t yrs = std::min(yearly_real.size(), r.schedule.size());
            for (size_t y = 0; y < yrs; ++y)
                yearly_real[y].push_back(r.schedule[y].end_total_real);
        }
        if (r.plan_feasible)
            ++successes;
    }

    auto calc_pct = [](std::vector<double>& vals, double p) -> double {
        if (vals.empty())
            return 0.0;
        size_t idx = static_cast<size_t>(
            std::clamp(p * (vals.size() - 1), 0.0,
                       static_cast<double>(vals.size() - 1)));
        return vals[idx];
    };

    std::sort(end_balances.begin(), end_balances.end());
    mc.success_rate = static_cast<double>(successes) / static_cast<double>(mc.trials);
    if (!end_balances.empty())
    {
        mc.p10_end_real = calc_pct(end_balances, 0.10);
        mc.p50_end_real = calc_pct(end_balances, 0.50);
        mc.p90_end_real = calc_pct(end_balances, 0.90);
    }

    mc.p10_real.resize(static_cast<size_t>(n_years), 0.0);
    mc.p25_real.resize(static_cast<size_t>(n_years), 0.0);
    mc.p50_real.resize(static_cast<size_t>(n_years), 0.0);
    mc.p75_real.resize(static_cast<size_t>(n_years), 0.0);
    mc.p90_real.resize(static_cast<size_t>(n_years), 0.0);
    for (int y = 0; y < n_years; ++y)
    {
        auto& vals = yearly_real[static_cast<size_t>(y)];
        std::sort(vals.begin(), vals.end());
        mc.p10_real[static_cast<size_t>(y)] = calc_pct(vals, 0.10);
        mc.p25_real[static_cast<size_t>(y)] = calc_pct(vals, 0.25);
        mc.p50_real[static_cast<size_t>(y)] = calc_pct(vals, 0.50);
        mc.p75_real[static_cast<size_t>(y)] = calc_pct(vals, 0.75);
        mc.p90_real[static_cast<size_t>(y)] = calc_pct(vals, 0.90);
    }
    return mc;
}

std::vector<StressScenarioResult> run_sequence_stress_tests(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double annual_spending_real,
    int spouse_ss_claim_age)
{
    const int sp_ss = (spouse_ss_claim_age >= 62 && spouse_ss_claim_age <= 70)
                      ? spouse_ss_claim_age : input.spouse_ss_claim_age;
    const int n_years = plan_num_years(input);
    const int ret_idx = std::clamp(input.retirement_age - input.current_age, 0, std::max(0, n_years - 1));

    auto eval = [&](const char* name, const PlanInput& sc_in, const std::vector<double>& rets) {
        PlanResult r = simulate_with_returns(sc_in, strategy, ss_claim_age, sp_ss, annual_spending_real, rets);
        StressScenarioResult sr;
        sr.name = name;
        sr.feasible = r.plan_feasible;
        sr.min_spending_real = r.sustainable_spending_real;
        sr.final_bequest_real = r.final_bequest_real;
        for (const auto& yp : r.schedule)
            if (yp.shortfall) ++sr.shortfall_years;
        return sr;
    };

    std::vector<StressScenarioResult> out;
    // 1. Baseline expected return
    std::vector<double> r_base(static_cast<size_t>(n_years), input.nominal_return);
    out.push_back(eval("Baseline (Expected Return)", input, r_base));

    // 2. Early Retirement Crash (-25% Yr 1, -10% Yr 2)
    std::vector<double> r_crash = r_base;
    if (ret_idx < n_years) r_crash[static_cast<size_t>(ret_idx)] = -0.25;
    if (ret_idx + 1 < n_years) r_crash[static_cast<size_t>(ret_idx + 1)] = -0.10;
    out.push_back(eval("Early Crash (-25% / -10% at Retirement)", input, r_crash));

    // 3. Lost Decade (0% nominal return for 5 yrs at retirement)
    std::vector<double> r_flat = r_base;
    for (int k = 0; k < 5 && ret_idx + k < n_years; ++k)
        r_flat[static_cast<size_t>(ret_idx + k)] = 0.0;
    out.push_back(eval("Lost Decade (0% for 5 yrs at Retirement)", input, r_flat));

    // 4. Stagflation (+2% inflation, -1.5% return)
    PlanInput stag = input;
    stag.inflation = std::min(0.15, input.inflation + 0.02);
    stag.nominal_return = std::max(0.0, input.nominal_return - 0.015);
    std::vector<double> r_stag(static_cast<size_t>(n_years), stag.nominal_return);
    out.push_back(eval("Stagflation (+2% Inflation, -1.5% Return)", stag, r_stag));

    return out;
}

SensitivityMatrix run_sensitivity_matrix(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double base_spending_real,
    int spouse_ss_claim_age)
{
    const int sp_ss = (spouse_ss_claim_age >= 62 && spouse_ss_claim_age <= 70)
                      ? spouse_ss_claim_age : input.spouse_ss_claim_age;
    SensitivityMatrix mat;
    const double ret_deltas[5] = { -0.02, -0.01, 0.0, 0.01, 0.02 };
    const double sp_mults[5] = { 0.85, 0.925, 1.00, 1.075, 1.15 };

    for (double d : ret_deltas)
        mat.return_rates.push_back(std::max(0.0, input.nominal_return + d));
    for (double m : sp_mults)
        mat.spending_levels.push_back(std::max(0.0, base_spending_real * m));

    mat.cells.resize(5, std::vector<SensitivityCell>(5));
    for (size_t r = 0; r < 5; ++r)
    {
        PlanInput mod = input;
        mod.nominal_return = mat.return_rates[r];
        for (size_t c = 0; c < 5; ++c)
        {
            PlanResult pr = simulate_plan(mod, strategy, ss_claim_age, mat.spending_levels[c], sp_ss);
            SensitivityCell cell;
            cell.feasible = pr.plan_feasible;
            cell.final_bequest_real = pr.final_bequest_real;
            for (const auto& yp : pr.schedule)
                if (yp.shortfall) ++cell.shortfall_years;
            mat.cells[r][c] = cell;
        }
    }
    return mat;
}

HistoricalSummary run_historical_backtests(
    const PlanInput& input,
    WithdrawalStrategy strategy,
    int ss_claim_age,
    double annual_spending_real,
    int spouse_ss_claim_age)
{
    // Historical US S&P 500 & 10-Yr Treasury total returns (1928-2024, 97 years)
    struct HistYear { int yr; double eq; double bd; };
    static constexpr HistYear kHist[] = {
        {1928,  0.438,  0.008}, {1929, -0.083,  0.042}, {1930, -0.251,  0.045}, {1931, -0.438, -0.026},
        {1932, -0.086,  0.088}, {1933,  0.500,  0.019}, {1934, -0.012,  0.080}, {1935,  0.467,  0.045},
        {1936,  0.319,  0.050}, {1937, -0.353,  0.014}, {1938,  0.293,  0.042}, {1939, -0.011,  0.044},
        {1940, -0.107,  0.054}, {1941, -0.128, -0.020}, {1942,  0.192,  0.023}, {1943,  0.251,  0.025},
        {1944,  0.190,  0.026}, {1945,  0.358,  0.038}, {1946, -0.084,  0.031}, {1947,  0.052,  0.009},
        {1948,  0.057,  0.020}, {1949,  0.183,  0.047}, {1950,  0.308,  0.004}, {1951,  0.237, -0.003},
        {1952,  0.182,  0.023}, {1953, -0.012,  0.041}, {1954,  0.526,  0.033}, {1955,  0.326, -0.013},
        {1956,  0.074, -0.023}, {1957, -0.105,  0.068}, {1958,  0.437, -0.021}, {1959,  0.121, -0.026},
        {1960,  0.003,  0.116}, {1961,  0.266,  0.021}, {1962, -0.088,  0.057}, {1963,  0.226,  0.017},
        {1964,  0.164,  0.037}, {1965,  0.124,  0.007}, {1966, -0.100,  0.029}, {1967,  0.238, -0.016},
        {1968,  0.108,  0.033}, {1969, -0.082, -0.050}, {1970,  0.036,  0.168}, {1971,  0.142,  0.098},
        {1972,  0.188,  0.028}, {1973, -0.143,  0.037}, {1974, -0.259,  0.020}, {1975,  0.370,  0.036},
        {1976,  0.238,  0.160}, {1977, -0.070,  0.013}, {1978,  0.065, -0.008}, {1979,  0.185,  0.007},
        {1980,  0.317, -0.030}, {1981, -0.047,  0.082}, {1982,  0.204,  0.328}, {1983,  0.223,  0.032},
        {1984,  0.061,  0.137}, {1985,  0.312,  0.257}, {1986,  0.185,  0.243}, {1987,  0.058, -0.050},
        {1988,  0.165,  0.082}, {1989,  0.315,  0.177}, {1990, -0.031,  0.062}, {1991,  0.302,  0.150},
        {1992,  0.075,  0.094}, {1993,  0.100,  0.142}, {1994,  0.013, -0.080}, {1995,  0.372,  0.235},
        {1996,  0.227,  0.014}, {1997,  0.331,  0.099}, {1998,  0.283,  0.149}, {1999,  0.209, -0.083},
        {2000, -0.090,  0.167}, {2001, -0.119,  0.056}, {2002, -0.220,  0.151}, {2003,  0.284,  0.004},
        {2004,  0.107,  0.045}, {2005,  0.048,  0.029}, {2006,  0.156,  0.020}, {2007,  0.055,  0.102},
        {2008, -0.366,  0.201}, {2009,  0.259, -0.111}, {2010,  0.148,  0.085}, {2011,  0.021,  0.160},
        {2012,  0.159,  0.030}, {2013,  0.321, -0.091}, {2014,  0.135,  0.107}, {2015,  0.014,  0.013},
        {2016,  0.118,  0.007}, {2017,  0.216,  0.028}, {2018, -0.042, -0.000}, {2019,  0.312,  0.096},
        {2020,  0.180,  0.113}, {2021,  0.285, -0.044}, {2022, -0.180, -0.178}, {2023,  0.261,  0.039},
        {2024,  0.249,  0.015}
    };
    static constexpr int kHistCount = static_cast<int>(sizeof(kHist) / sizeof(kHist[0]));
    const int sp_ss = (spouse_ss_claim_age >= 62 && spouse_ss_claim_age <= 70)
                      ? spouse_ss_claim_age : input.spouse_ss_claim_age;
    const int n_years = plan_num_years(input);
    const int ret_idx = std::clamp(input.retirement_age - input.current_age, 0, std::max(0, n_years - 1));

    auto run_from_start = [&](const char* label, int start_idx) -> HistoricalBacktestResult {
        std::vector<double> rets(static_cast<size_t>(n_years));
        for (int i = 0; i < n_years; ++i)
        {
            double eq_wt = input.enable_glide_path
                ? std::clamp(((input.current_age + i < input.retirement_age)
                              ? input.equity_pct_pre_ret : input.equity_pct_in_ret) / 100.0, 0.0, 1.0)
                : 0.60;
            // Align retirement year to start_idx for crisis stress, or chronological from start_idx
            int hidx = (start_idx + std::max(0, i - ret_idx)) % kHistCount;
            if (i < ret_idx)
                hidx = (start_idx + i) % kHistCount;
            rets[static_cast<size_t>(i)] = eq_wt * kHist[hidx].eq + (1.0 - eq_wt) * kHist[hidx].bd;
        }
        PlanResult r = simulate_with_returns(input, strategy, ss_claim_age, sp_ss, annual_spending_real, rets);
        HistoricalBacktestResult hr;
        hr.name = label;
        hr.start_year = kHist[start_idx % kHistCount].yr;
        hr.feasible = r.plan_feasible;
        hr.min_spending_real = r.sustainable_spending_real;
        hr.final_bequest_real = r.final_bequest_real;
        for (const auto& yp : r.schedule)
            if (yp.shortfall) ++hr.shortfall_years;
        return hr;
    };

    HistoricalSummary out;
    int successes = 0;
    HistoricalBacktestResult worst;
    worst.final_bequest_real = std::numeric_limits<double>::infinity();
    for (int s = 0; s < kHistCount; ++s)
    {
        auto hr = run_from_start("Rolling", s);
        ++out.total_cycles;
        if (hr.feasible) ++successes;
        if (hr.final_bequest_real < worst.final_bequest_real)
        {
            worst = hr;
            worst.name = "Worst Historical Cycle (" + std::to_string(hr.start_year) + ")";
        }
    }
    out.rolling_success_rate = (out.total_cycles > 0)
        ? (static_cast<double>(successes) / static_cast<double>(out.total_cycles))
        : 0.0;

    out.scenarios.push_back(run_from_start("1929 Great Depression", 1));
    out.scenarios.push_back(run_from_start("1966 Stagflation Era", 38));
    out.scenarios.push_back(run_from_start("1973 Oil Shock", 45));
    out.scenarios.push_back(run_from_start("2000 Dot-Com Crash", 72));
    out.scenarios.push_back(run_from_start("2008 Global Financial Crisis", 80));
    out.scenarios.push_back(std::move(worst));
    return out;
}

PlanResult optimize_plan(const PlanInput& input)
{
    // Baseline: StandardTaxableFirst at user's specified SS age(s)
    PlanResult baseline;
    if (input.objective == PlanObjective::MaximizeSpending)
    {
        double base_spend = find_max_spending(
            input, WithdrawalStrategy::StandardTaxableFirst, input.ss_claim_age, input.spouse_ss_claim_age);
        baseline = simulate_plan(
            input, WithdrawalStrategy::StandardTaxableFirst, input.ss_claim_age, base_spend, input.spouse_ss_claim_age);
    }
    else
    {
        baseline = simulate_plan(
            input, WithdrawalStrategy::StandardTaxableFirst, input.ss_claim_age,
            input.target_annual_spending, input.spouse_ss_claim_age);
    }

    std::vector<WithdrawalStrategy> candidates;
    if (input.strategy == WithdrawalStrategy::AutoOptimize)
    {
        candidates = {
            WithdrawalStrategy::StandardTaxableFirst,
            WithdrawalStrategy::TaxDeferredFirst,
            WithdrawalStrategy::RothBracketFill12,
            WithdrawalStrategy::RothBracketFill22,
            WithdrawalStrategy::RothBracketFill24
        };
    }
    else
    {
        candidates = { input.strategy };
    }

    int ss_min = input.optimize_ss_age ? 62 : input.ss_claim_age;
    int ss_max = input.optimize_ss_age ? 70 : input.ss_claim_age;

    std::vector<int> sp_ss_candidates;
    if (input.has_spouse && input.spouse_optimize_ss_age)
        sp_ss_candidates = {62, 65, 67, 70};
    else
        sp_ss_candidates = {input.spouse_ss_claim_age};

    PlanResult best = baseline;
    double best_score = (input.objective == PlanObjective::MaximizeSpending)
                        ? baseline.sustainable_spending_real
                        : baseline.final_bequest_real;

    auto eval_candidate = [&](WithdrawalStrategy s, int ss_age, int sp_ss_age) {
        PlanResult cand;
        if (input.objective == PlanObjective::MaximizeSpending)
        {
            double max_sp = find_max_spending(input, s, ss_age, sp_ss_age);
            cand = simulate_plan(input, s, ss_age, max_sp, sp_ss_age);
            if (cand.sustainable_spending_real > best_score)
            {
                best_score = cand.sustainable_spending_real;
                best = std::move(cand);
            }
        }
        else
        {
            cand = simulate_plan(input, s, ss_age, input.target_annual_spending, sp_ss_age);
            double score = cand.plan_feasible
                           ? cand.final_bequest_real
                           : (cand.sustainable_spending_real - input.target_annual_spending);
            if (score > best_score)
            {
                best_score = score;
                best = std::move(cand);
            }
        }
    };

    for (WithdrawalStrategy s : candidates)
    {
        for (int ss_age = ss_min; ss_age <= ss_max; ++ss_age)
        {
            for (int sp_ss_age : sp_ss_candidates)
                eval_candidate(s, ss_age, sp_ss_age);
        }
    }

    // Fine-tune spouse SS age across 62..70 at the winning strategy & primary SS age
    if (input.has_spouse && input.spouse_optimize_ss_age)
    {
        for (int sp_ss_age = 62; sp_ss_age <= 70; ++sp_ss_age)
            eval_candidate(best.chosen_strategy, best.chosen_ss_age, sp_ss_age);
    }

    best.baseline_spending_real = baseline.sustainable_spending_real;
    best.baseline_bequest_real = baseline.final_bequest_real;
    best.baseline_taxes_real = baseline.lifetime_taxes_real;

    if (input.objective == PlanObjective::MaximizeSpending)
    {
        int ret_years = std::max(1, plan_num_years(input) - std::max(0, input.retirement_age - input.current_age));
        best.advantage_real = (best.sustainable_spending_real - baseline.sustainable_spending_real) * ret_years;
    }
    else
    {
        best.advantage_real = best.final_bequest_real - baseline.final_bequest_real;
    }

    best.monte_carlo = run_monte_carlo(
        input, best.chosen_strategy, best.chosen_ss_age,
        best.sustainable_spending_real, best.chosen_spouse_ss_age);

    return best;
}

const char* strategy_label(WithdrawalStrategy s) noexcept
{
    switch (s)
    {
        case WithdrawalStrategy::AutoOptimize:         return "Auto-Optimize (Owl)";
        case WithdrawalStrategy::StandardTaxableFirst: return "Standard (Taxable First)";
        case WithdrawalStrategy::TaxDeferredFirst:     return "Tax-Deferred First";
        case WithdrawalStrategy::RothBracketFill12:    return "Roth Conversion (12% Bracket)";
        case WithdrawalStrategy::RothBracketFill22:    return "Roth Conversion (22% Bracket)";
        case WithdrawalStrategy::RothBracketFill24:    return "Roth Conversion (24% Bracket)";
    }
    return "Unknown";
}

const char* bucket_label(TaxBucket b) noexcept
{
    switch (b)
    {
        case TaxBucket::Taxable:     return "Taxable";
        case TaxBucket::TaxDeferred: return "Tax-Deferred (401k/IRA)";
        case TaxBucket::TaxFree:     return "Tax-Free (Roth)";
        case TaxBucket::HSA:         return "HSA";
        case TaxBucket::Excluded:    return "Exclude";
    }
    return "Unknown";
}

const char* state_code(USState st) noexcept
{
    static constexpr const char* kCodes[] = {
        "None (0%)",
        "AL", "AK", "AZ", "AR", "CA", "CO", "CT", "DE", "DC", "FL",
        "GA", "HI", "ID", "IL", "IN", "IA", "KS", "KY", "LA", "ME",
        "MD", "MA", "MI", "MN", "MS", "MO", "MT", "NE", "NV", "NH",
        "NJ", "NM", "NY", "NC", "ND", "OH", "OK", "OR", "PA", "RI",
        "SC", "SD", "TN", "TX", "UT", "VT", "VA", "WA", "WV", "WI", "WY"
    };
    size_t idx = static_cast<size_t>(st);
    if (idx < sizeof(kCodes) / sizeof(kCodes[0]))
        return kCodes[idx];
    return "None (0%)";
}

namespace {

struct StateTaxRule
{
    double std_ded_single;
    double low_rate;        // Effective lower-bracket rate
    double mid_rate;        // Mid-bracket rate (above ~$50k Single / ~$100k MFJ)
    double top_rate;        // Top marginal rate (above ~$150k Single / ~$300k MFJ)
    bool taxes_ss;          // Whether state taxes Social Security benefits
    bool exempts_ret_dist;  // Whether state broadly exempts 401k/IRA/pension distributions
};

constexpr StateTaxRule kStateRules[] = {
    /* None */ { 0.0,     0.000, 0.000, 0.000, false, true },
    /* AL */   { 3000.0,  0.020, 0.040, 0.050, false, false },
    /* AK */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* AZ */   { 14600.0, 0.025, 0.025, 0.025, false, false },
    /* AR */   { 2340.0,  0.020, 0.039, 0.039, false, false },
    /* CA */   { 5540.0,  0.040, 0.080, 0.113, false, false },
    /* CO */   { 14600.0, 0.044, 0.044, 0.044, true,  false },
    /* CT */   { 15000.0, 0.030, 0.050, 0.065, true,  false },
    /* DE */   { 3250.0,  0.035, 0.055, 0.066, false, false },
    /* DC */   { 14600.0, 0.040, 0.065, 0.095, false, false },
    /* FL */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* GA */   { 12000.0, 0.0539,0.0539,0.0539,false, false },
    /* HI */   { 4400.0,  0.040, 0.072, 0.090, false, false },
    /* ID */   { 14600.0, 0.05695,0.05695,0.05695,false,false },
    /* IL */   { 2775.0,  0.0495,0.0495,0.0495,false, true },
    /* IN */   { 1000.0,  0.030, 0.030, 0.030, false, false },
    /* IA */   { 0.0,     0.038, 0.038, 0.038, false, true },
    /* KS */   { 3500.0,  0.052, 0.0558,0.0558,false, false },
    /* KY */   { 3160.0,  0.040, 0.040, 0.040, false, false },
    /* LA */   { 12500.0, 0.030, 0.030, 0.030, false, false },
    /* ME */   { 14600.0, 0.058, 0.0675,0.0715,false, false },
    /* MD */   { 2700.0,  0.040, 0.0475,0.0575,false, false },
    /* MA */   { 4400.0,  0.050, 0.050, 0.050, false, false },
    /* MI */   { 5600.0,  0.0425,0.0425,0.0425,false, false },
    /* MN */   { 14575.0, 0.0535,0.068, 0.0985,true,  false },
    /* MS */   { 2300.0,  0.044, 0.044, 0.044, false, true },
    /* MO */   { 14600.0, 0.035, 0.047, 0.047, false, false },
    /* MT */   { 14600.0, 0.047, 0.059, 0.059, true,  false },
    /* NE */   { 8250.0,  0.035, 0.052, 0.052, false, false },
    /* NV */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* NH */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* NJ */   { 1000.0,  0.0175,0.0552,0.0897,false, false },
    /* NM */   { 14600.0, 0.032, 0.049, 0.059, true,  false },
    /* NY */   { 8000.0,  0.045, 0.0585,0.0685,false, false },
    /* NC */   { 12750.0, 0.0425,0.0425,0.0425,false, false },
    /* ND */   { 14600.0, 0.0195,0.025, 0.025, false, false },
    /* OH */   { 2400.0,  0.0275,0.035, 0.035, false, false },
    /* OK */   { 6350.0,  0.035, 0.0475,0.0475,false, false },
    /* OR */   { 2745.0,  0.0675,0.0875,0.099, false, false },
    /* PA */   { 0.0,     0.0307,0.0307,0.0307,false, true },
    /* RI */   { 10550.0, 0.0375,0.0475,0.0599,true,  false },
    /* SC */   { 14600.0, 0.030, 0.062, 0.062, false, false },
    /* SD */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* TN */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* TX */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* UT */   { 14600.0, 0.0455,0.0455,0.0455,true,  false },
    /* VT */   { 7250.0,  0.0335,0.066, 0.0875,true,  false },
    /* VA */   { 8500.0,  0.035, 0.0575,0.0575,false, false },
    /* WA */   { 0.0,     0.000, 0.000, 0.000, false, true },
    /* WV */   { 2000.0,  0.035, 0.0482,0.0482,true,  false },
    /* WI */   { 13230.0, 0.044, 0.053, 0.0765,false, false },
    /* WY */   { 0.0,     0.000, 0.000, 0.000, false, true }
};

} // namespace

double compute_state_tax(USState st,
                         double ordinary_non_ss,
                         double ss_benefit,
                         double cap_gains,
                         double retirement_dist,
                         FilingStatus status,
                         int age,
                         double inf_factor) noexcept
{
    size_t idx = static_cast<size_t>(st);
    if (idx == 0 || idx >= sizeof(kStateRules) / sizeof(kStateRules[0]))
        return 0.0;

    const StateTaxRule& rule = kStateRules[idx];
    if (rule.top_rate <= 0.0)
        return 0.0;

    double mfj_mult = (status == FilingStatus::MarriedFilingJointly) ? 2.0 : 1.0;
    double taxable_ord = ordinary_non_ss;
    if (rule.exempts_ret_dist)
        taxable_ord = std::max(0.0, taxable_ord - retirement_dist);
    if (rule.taxes_ss)
        taxable_ord += taxable_ss_portion(ss_benefit, ordinary_non_ss, cap_gains, status);

    double ded = rule.std_ded_single * mfj_mult * inf_factor;
    if (age >= 65)
        ded += 1500.0 * mfj_mult * inf_factor;

    double net_taxable = std::max(0.0, taxable_ord + cap_gains - ded);
    if (net_taxable <= 0.0)
        return 0.0;

    double b1 = 50000.0 * mfj_mult * inf_factor;
    double b2 = 150000.0 * mfj_mult * inf_factor;

    double tax = 0.0;
    tax += std::min(net_taxable, b1) * rule.low_rate;
    if (net_taxable > b1)
        tax += std::min(net_taxable - b1, b2 - b1) * rule.mid_rate;
    if (net_taxable > b2)
        tax += (net_taxable - b2) * rule.top_rate;
    return tax;
}

double compute_aca_premium(double magi,
                           FilingStatus status,
                           int age,
                           double benchmark_per_adult_real,
                           bool enforce_400_cliff,
                           double inf_factor) noexcept
{
    if (age >= 65 || benchmark_per_adult_real <= 0.0)
        return 0.0;

    const int adults = (status == FilingStatus::MarriedFilingJointly) ? 2 : 1;
    const double benchmark = benchmark_per_adult_real * adults * inf_factor;
    const double fpl = (15650.0 + (adults - 1) * 5500.0) * inf_factor;
    const double fpl_pct = (fpl > 0.0) ? (magi / fpl) : 10.0;

    if (fpl_pct <= 1.5)
        return 0.0;
    if (fpl_pct > 4.0 && enforce_400_cliff)
        return benchmark;

    // Piecewise linear ACA applicable percentage of MAGI
    double pct = 0.085;
    if (fpl_pct <= 2.0)
        pct = 0.0 + (fpl_pct - 1.5) / 0.5 * 0.020;
    else if (fpl_pct <= 2.5)
        pct = 0.020 + (fpl_pct - 2.0) / 0.5 * 0.020;
    else if (fpl_pct <= 3.0)
        pct = 0.040 + (fpl_pct - 2.5) / 0.5 * 0.020;
    else if (fpl_pct <= 4.0)
        pct = 0.060 + (fpl_pct - 3.0) / 1.0 * 0.025;

    return std::min(benchmark, std::max(0.0, magi * pct));
}

USState state_from_code(const std::string& code) noexcept
{
    std::string lc = to_lower(code);
    for (int s = 0; s <= static_cast<int>(USState::WY); ++s)
    {
        auto st = static_cast<USState>(s);
        if (to_lower(state_code(st)) == lc)
            return st;
    }
    return USState::None;
}

TaxBucket bucket_from_label(const std::string& label, TaxBucket fallback) noexcept
{
    std::string lc = to_lower(label);
    for (int b = 0; b <= static_cast<int>(TaxBucket::Excluded); ++b)
    {
        auto tb = static_cast<TaxBucket>(b);
        if (to_lower(bucket_label(tb)) == lc)
            return tb;
    }
    if (lc == "taxable" || lc == "0") return TaxBucket::Taxable;
    if (lc == "tax_deferred" || lc == "taxdeferred" || lc == "1") return TaxBucket::TaxDeferred;
    if (lc == "tax_free" || lc == "taxfree" || lc == "roth" || lc == "2") return TaxBucket::TaxFree;
    if (lc == "hsa" || lc == "3") return TaxBucket::HSA;
    if (lc == "excluded" || lc == "4") return TaxBucket::Excluded;
    return fallback;
}

namespace {

std::string trim_ws(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool parse_bool(const std::string& v, bool fallback)
{
    std::string lc = to_lower(trim_ws(v));
    if (lc == "1" || lc == "true" || lc == "yes") return true;
    if (lc == "0" || lc == "false" || lc == "no") return false;
    return fallback;
}

} // namespace

std::string serialize_plan_ini(
    const PlanInput& input,
    const std::vector<std::pair<std::string, TaxBucket>>& account_overrides,
    const std::vector<std::string>& excluded_accounts)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(6);

    oss << "[Plan]\n";
    oss << "start_year=" << input.start_year << "\n";
    oss << "current_age=" << input.current_age << "\n";
    oss << "retirement_age=" << input.retirement_age << "\n";
    oss << "horizon_age=" << input.horizon_age << "\n";
    oss << "filing_status=" << (input.filing_status == FilingStatus::Single ? "single" : "mfj") << "\n";
    oss << "objective=" << (input.objective == PlanObjective::MaximizeSpending ? "maximize_spending" : "maximize_bequest") << "\n";
    oss << "strategy=" << static_cast<int>(input.strategy) << "\n";
    oss << "target_annual_spending=" << input.target_annual_spending << "\n";
    oss << "target_bequest=" << input.target_bequest << "\n";
    oss << "ss_fra_annual=" << input.ss_fra_annual << "\n";
    oss << "ss_claim_age=" << input.ss_claim_age << "\n";
    oss << "optimize_ss_age=" << (input.optimize_ss_age ? "true" : "false") << "\n";
    oss << "pension_annual=" << input.pension_annual << "\n";
    oss << "pension_start_age=" << input.pension_start_age << "\n";
    oss << "pension_cola=" << (input.pension_cola ? "true" : "false") << "\n";
    oss << "contrib_taxable=" << input.contrib_taxable << "\n";
    oss << "contrib_tax_deferred=" << input.contrib_tax_deferred << "\n";
    oss << "contrib_tax_free=" << input.contrib_tax_free << "\n";
    oss << "contrib_hsa=" << input.contrib_hsa << "\n";
    oss << "nominal_return=" << input.nominal_return << "\n";
    oss << "volatility=" << input.volatility << "\n";
    oss << "inflation=" << input.inflation << "\n";
    oss << "return_taxable=" << input.return_taxable << "\n";
    oss << "return_tax_deferred=" << input.return_tax_deferred << "\n";
    oss << "return_tax_free=" << input.return_tax_free << "\n";
    oss << "return_hsa=" << input.return_hsa << "\n";
    oss << "taxable_basis_pct=" << input.taxable_basis_pct << "\n";
    oss << "rmd_start_age=" << input.rmd_start_age << "\n";
    oss << "state=" << state_code(input.state) << "\n";
    oss << "enable_aca=" << (input.enable_aca ? "true" : "false") << "\n";
    oss << "aca_benchmark_premium=" << input.aca_benchmark_premium << "\n";
    oss << "aca_enforce_400_cliff=" << (input.aca_enforce_400_cliff ? "true" : "false") << "\n";
    oss << "monte_carlo_trials=" << input.monte_carlo_trials << "\n";
    oss << "random_seed=" << input.random_seed << "\n";
    oss << "has_spouse=" << (input.has_spouse ? "true" : "false") << "\n";
    oss << "spouse_current_age=" << input.spouse_current_age << "\n";
    oss << "spouse_retirement_age=" << input.spouse_retirement_age << "\n";
    oss << "spouse_horizon_age=" << input.spouse_horizon_age << "\n";
    oss << "spouse_ss_fra_annual=" << input.spouse_ss_fra_annual << "\n";
    oss << "spouse_ss_claim_age=" << input.spouse_ss_claim_age << "\n";
    oss << "spouse_optimize_ss_age=" << (input.spouse_optimize_ss_age ? "true" : "false") << "\n";
    oss << "spouse_pension_annual=" << input.spouse_pension_annual << "\n";
    oss << "spouse_pension_start_age=" << input.spouse_pension_start_age << "\n";
    oss << "spouse_pension_cola=" << (input.spouse_pension_cola ? "true" : "false") << "\n";
    oss << "spouse_rmd_start_age=" << input.spouse_rmd_start_age << "\n";
    oss << "spouse_contrib_tax_deferred=" << input.spouse_contrib_tax_deferred << "\n";
    oss << "spouse_contrib_tax_free=" << input.spouse_contrib_tax_free << "\n";
    oss << "slow_go_age=" << input.slow_go_age << "\n";
    oss << "slow_go_pct=" << input.slow_go_pct << "\n";
    oss << "no_go_age=" << input.no_go_age << "\n";
    oss << "no_go_pct=" << input.no_go_pct << "\n";
    oss << "reinvest_rmd_surplus=" << (input.reinvest_rmd_surplus ? "true" : "false") << "\n";
    oss << "tips_floor_annual=" << input.tips_floor_annual << "\n";
    oss << "enable_guardrails=" << (input.enable_guardrails ? "true" : "false") << "\n";
    oss << "guardrail_upper_wr=" << input.guardrail_upper_wr << "\n";
    oss << "guardrail_lower_wr=" << input.guardrail_lower_wr << "\n";
    oss << "guardrail_adj_pct=" << input.guardrail_adj_pct << "\n";
    oss << "debt_annual_payment=" << input.debt_annual_payment << "\n";
    oss << "debt_payoff_age=" << input.debt_payoff_age << "\n";
    oss << "ltc_start_age=" << input.ltc_start_age << "\n";
    oss << "ltc_years=" << input.ltc_years << "\n";
    oss << "ltc_annual_cost_real=" << input.ltc_annual_cost_real << "\n";
    oss << "enable_glide_path=" << (input.enable_glide_path ? "true" : "false") << "\n";
    oss << "equity_pct_pre_ret=" << input.equity_pct_pre_ret << "\n";
    oss << "equity_pct_in_ret=" << input.equity_pct_in_ret << "\n";
    oss << "equity_return=" << input.equity_return << "\n";
    oss << "bond_return=" << input.bond_return << "\n";
    oss << "equity_vol=" << input.equity_vol << "\n";
    oss << "bond_vol=" << input.bond_vol << "\n";

    if (!input.events.empty())
    {
        oss << "\n[Events]\n";
        for (size_t i = 0; i < input.events.size(); ++i)
        {
            const auto& ev = input.events[i];
            oss << "event_" << i << "=" << ev.age << "|" << ev.amount_real
                << "|" << bucket_label(ev.bucket) << "|" << ev.name << "\n";
        }
    }

    if (!account_overrides.empty())
    {
        oss << "\n[AccountOverrides]\n";
        for (const auto& kv : account_overrides)
            oss << kv.first << "=" << bucket_label(kv.second) << "\n";
    }
    if (!excluded_accounts.empty())
    {
        oss << "\n[AccountExcluded]\n";
        for (const auto& name : excluded_accounts)
            oss << name << "=true\n";
    }
    return oss.str();
}

bool deserialize_plan_ini(
    const std::string& ini_text,
    PlanInput& out_input,
    std::vector<std::pair<std::string, TaxBucket>>& out_overrides,
    std::vector<std::string>* out_excluded)
{
    PlanInput in = out_input;
    std::vector<std::pair<std::string, TaxBucket>> overrides;
    std::vector<std::string> excluded;
    std::istringstream iss(ini_text);
    iss.imbue(std::locale::classic());

    std::string line;
    std::string section;
    bool saw_plan_key = false;

    while (std::getline(iss, line))
    {
        std::string t = trim_ws(line);
        if (t.empty() || t[0] == '#' || t[0] == ';')
            continue;
        if (t.front() == '[' && t.back() == ']')
        {
            section = to_lower(trim_ws(t.substr(1, t.size() - 2)));
            continue;
        }
        size_t eq = t.rfind('=');
        if (eq == std::string::npos)
            continue;

        std::string key = trim_ws(t.substr(0, eq));
        std::string val = trim_ws(t.substr(eq + 1));

        if (section == "accountoverrides")
        {
            if (!key.empty())
                overrides.emplace_back(key, bucket_from_label(val));
            continue;
        }

        if (section == "accountexcluded")
        {
            if (!key.empty() && parse_bool(val, false))
                excluded.push_back(key);
            continue;
        }

        if (section == "events")
        {
            size_t p1 = val.find('|');
            size_t p2 = (p1 != std::string::npos) ? val.find('|', p1 + 1) : std::string::npos;
            size_t p3 = (p2 != std::string::npos) ? val.find('|', p2 + 1) : std::string::npos;
            if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos)
            {
                try
                {
                    CashflowEvent ev;
                    ev.age = std::clamp(std::stoi(val.substr(0, p1)), 18, 110);
                    ev.amount_real = std::stod(val.substr(p1 + 1, p2 - p1 - 1));
                    ev.bucket = bucket_from_label(val.substr(p2 + 1, p3 - p2 - 1));
                    ev.name = trim_ws(val.substr(p3 + 1));
                    in.events.push_back(std::move(ev));
                    saw_plan_key = true;
                }
                catch (...) {}
            }
            continue;
        }

        if (section.empty() || section == "plan")
        {
            std::string lkey = to_lower(key);
            try
            {
                if (lkey == "start_year") { in.start_year = std::stoi(val); saw_plan_key = true; }
                else if (lkey == "current_age") { in.current_age = std::clamp(std::stoi(val), 18, 95); saw_plan_key = true; }
                else if (lkey == "retirement_age") { in.retirement_age = std::clamp(std::stoi(val), 30, 95); saw_plan_key = true; }
                else if (lkey == "horizon_age") { in.horizon_age = std::clamp(std::stoi(val), 65, 110); saw_plan_key = true; }
                else if (lkey == "filing_status") {
                    std::string lv = to_lower(val);
                    in.filing_status = (lv == "single" || lv == "0") ? FilingStatus::Single : FilingStatus::MarriedFilingJointly;
                    saw_plan_key = true;
                }
                else if (lkey == "objective") {
                    std::string lv = to_lower(val);
                    in.objective = (lv == "maximize_bequest" || lv == "1") ? PlanObjective::MaximizeBequest : PlanObjective::MaximizeSpending;
                    saw_plan_key = true;
                }
                else if (lkey == "strategy") {
                    in.strategy = static_cast<WithdrawalStrategy>(std::clamp(std::stoi(val), 0, 5));
                    saw_plan_key = true;
                }
                else if (lkey == "target_annual_spending") { in.target_annual_spending = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "target_bequest") { in.target_bequest = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "ss_fra_annual") { in.ss_fra_annual = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "ss_claim_age") { in.ss_claim_age = std::clamp(std::stoi(val), 62, 70); saw_plan_key = true; }
                else if (lkey == "optimize_ss_age") { in.optimize_ss_age = parse_bool(val, in.optimize_ss_age); saw_plan_key = true; }
                else if (lkey == "pension_annual") { in.pension_annual = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "pension_start_age") { in.pension_start_age = std::clamp(std::stoi(val), 50, 85); saw_plan_key = true; }
                else if (lkey == "pension_cola") { in.pension_cola = parse_bool(val, in.pension_cola); saw_plan_key = true; }
                else if (lkey == "contrib_taxable") { in.contrib_taxable = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "contrib_tax_deferred") { in.contrib_tax_deferred = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "contrib_tax_free") { in.contrib_tax_free = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "contrib_hsa") { in.contrib_hsa = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "nominal_return") { in.nominal_return = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "volatility") { in.volatility = std::clamp(std::stod(val), 0.0, 0.50); saw_plan_key = true; }
                else if (lkey == "inflation") { in.inflation = std::clamp(std::stod(val), 0.0, 0.15); saw_plan_key = true; }
                else if (lkey == "return_taxable") { in.return_taxable = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "return_tax_deferred") { in.return_tax_deferred = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "return_tax_free") { in.return_tax_free = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "return_hsa") { in.return_hsa = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "taxable_basis_pct") { in.taxable_basis_pct = std::clamp(std::stod(val), 0.0, 1.0); saw_plan_key = true; }
                else if (lkey == "rmd_start_age") { in.rmd_start_age = std::clamp(std::stoi(val), 72, 75); saw_plan_key = true; }
                else if (lkey == "state") { in.state = state_from_code(val); saw_plan_key = true; }
                else if (lkey == "enable_aca") { in.enable_aca = parse_bool(val, in.enable_aca); saw_plan_key = true; }
                else if (lkey == "aca_benchmark_premium") { in.aca_benchmark_premium = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "aca_enforce_400_cliff") { in.aca_enforce_400_cliff = parse_bool(val, in.aca_enforce_400_cliff); saw_plan_key = true; }
                else if (lkey == "monte_carlo_trials") { in.monte_carlo_trials = std::clamp(std::stoi(val), 10, 5000); saw_plan_key = true; }
                else if (lkey == "random_seed") { in.random_seed = static_cast<uint32_t>(std::stoul(val)); saw_plan_key = true; }
                else if (lkey == "has_spouse") { in.has_spouse = parse_bool(val, in.has_spouse); saw_plan_key = true; }
                else if (lkey == "spouse_current_age") { in.spouse_current_age = std::clamp(std::stoi(val), 18, 95); saw_plan_key = true; }
                else if (lkey == "spouse_retirement_age") { in.spouse_retirement_age = std::clamp(std::stoi(val), 30, 95); saw_plan_key = true; }
                else if (lkey == "spouse_horizon_age") { in.spouse_horizon_age = std::clamp(std::stoi(val), 65, 110); saw_plan_key = true; }
                else if (lkey == "spouse_ss_fra_annual") { in.spouse_ss_fra_annual = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "spouse_ss_claim_age") { in.spouse_ss_claim_age = std::clamp(std::stoi(val), 62, 70); saw_plan_key = true; }
                else if (lkey == "spouse_optimize_ss_age") { in.spouse_optimize_ss_age = parse_bool(val, in.spouse_optimize_ss_age); saw_plan_key = true; }
                else if (lkey == "spouse_pension_annual") { in.spouse_pension_annual = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "spouse_pension_start_age") { in.spouse_pension_start_age = std::clamp(std::stoi(val), 50, 85); saw_plan_key = true; }
                else if (lkey == "spouse_pension_cola") { in.spouse_pension_cola = parse_bool(val, in.spouse_pension_cola); saw_plan_key = true; }
                else if (lkey == "spouse_rmd_start_age") { in.spouse_rmd_start_age = std::clamp(std::stoi(val), 72, 75); saw_plan_key = true; }
                else if (lkey == "spouse_contrib_tax_deferred") { in.spouse_contrib_tax_deferred = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "spouse_contrib_tax_free") { in.spouse_contrib_tax_free = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "slow_go_age") { in.slow_go_age = std::clamp(std::stoi(val), 50, 110); saw_plan_key = true; }
                else if (lkey == "slow_go_pct") { in.slow_go_pct = std::clamp(std::stod(val), 10.0, 200.0); saw_plan_key = true; }
                else if (lkey == "no_go_age") { in.no_go_age = std::clamp(std::stoi(val), 55, 110); saw_plan_key = true; }
                else if (lkey == "no_go_pct") { in.no_go_pct = std::clamp(std::stod(val), 10.0, 200.0); saw_plan_key = true; }
                else if (lkey == "reinvest_rmd_surplus") { in.reinvest_rmd_surplus = parse_bool(val, in.reinvest_rmd_surplus); saw_plan_key = true; }
                else if (lkey == "tips_floor_annual") { in.tips_floor_annual = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "enable_guardrails") { in.enable_guardrails = parse_bool(val, in.enable_guardrails); saw_plan_key = true; }
                else if (lkey == "guardrail_upper_wr") { in.guardrail_upper_wr = std::clamp(std::stod(val), 0.02, 0.20); saw_plan_key = true; }
                else if (lkey == "guardrail_lower_wr") { in.guardrail_lower_wr = std::clamp(std::stod(val), 0.01, 0.15); saw_plan_key = true; }
                else if (lkey == "guardrail_adj_pct") { in.guardrail_adj_pct = std::clamp(std::stod(val), 0.01, 0.50); saw_plan_key = true; }
                else if (lkey == "debt_annual_payment") { in.debt_annual_payment = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "debt_payoff_age") { in.debt_payoff_age = std::clamp(std::stoi(val), 30, 110); saw_plan_key = true; }
                else if (lkey == "ltc_start_age") { in.ltc_start_age = std::clamp(std::stoi(val), 50, 110); saw_plan_key = true; }
                else if (lkey == "ltc_years") { in.ltc_years = std::clamp(std::stoi(val), 0, 20); saw_plan_key = true; }
                else if (lkey == "ltc_annual_cost_real") { in.ltc_annual_cost_real = std::max(0.0, std::stod(val)); saw_plan_key = true; }
                else if (lkey == "enable_glide_path") { in.enable_glide_path = parse_bool(val, in.enable_glide_path); saw_plan_key = true; }
                else if (lkey == "equity_pct_pre_ret") { in.equity_pct_pre_ret = std::clamp(std::stod(val), 0.0, 100.0); saw_plan_key = true; }
                else if (lkey == "equity_pct_in_ret") { in.equity_pct_in_ret = std::clamp(std::stod(val), 0.0, 100.0); saw_plan_key = true; }
                else if (lkey == "equity_return") { in.equity_return = std::clamp(std::stod(val), 0.0, 0.25); saw_plan_key = true; }
                else if (lkey == "bond_return") { in.bond_return = std::clamp(std::stod(val), 0.0, 0.20); saw_plan_key = true; }
                else if (lkey == "equity_vol") { in.equity_vol = std::clamp(std::stod(val), 0.0, 0.50); saw_plan_key = true; }
                else if (lkey == "bond_vol") { in.bond_vol = std::clamp(std::stod(val), 0.0, 0.30); saw_plan_key = true; }
            }
            catch (...)
            {
                // Ignore malformed individual fields
            }
        }
    }

    if (!saw_plan_key && overrides.empty() && excluded.empty())
        return false;

    in.retirement_age = std::max(in.current_age, in.retirement_age);
    in.horizon_age = std::max(in.retirement_age + 1, in.horizon_age);
    in.spouse_retirement_age = std::max(in.spouse_current_age, in.spouse_retirement_age);
    in.spouse_horizon_age = std::max(in.spouse_retirement_age + 1, in.spouse_horizon_age);
    out_input = in;
    out_overrides = std::move(overrides);
    if (out_excluded)
        *out_excluded = std::move(excluded);
    return true;
}

std::string export_schedule_csv(const PlanInput& input,
                                const PlanResult& result,
                                bool real_dollars)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::fixed << std::setprecision(2);

    oss << "Year,Age,Retired,EndTotal,EndTaxable,EndTaxDeferred,EndRoth,EndHSA,"
           "Contributions,SSIncome,PensionIncome,RMD,RothConversion,"
           "WithdrawTaxable,WithdrawTaxDeferred,WithdrawRoth,WithdrawHSA,"
           "OrdinaryTax,LTCGTax,NIIT,StateTax,IRMAA,ACAPremium,TotalTax,"
           "NetSpending,Shortfall,LumpSumNet,RMDReinvested,GuaranteedFloor,FloorSurplusDeficit,MarginalBracketPct,IRMAATier\n";

    for (size_t i = 0; i < result.schedule.size(); ++i)
    {
        const auto& yp = result.schedule[i];
        const double div = real_dollars
            ? std::pow(1.0 + input.inflation, static_cast<double>(i))
            : 1.0;

        oss << yp.year << ","
            << yp.age << ","
            << (yp.is_retired ? 1 : 0) << ","
            << (yp.end_total_nominal / div) << ","
            << (yp.end_taxable / div) << ","
            << (yp.end_tax_deferred / div) << ","
            << (yp.end_tax_free / div) << ","
            << (yp.end_hsa / div) << ","
            << (yp.contributions / div) << ","
            << (yp.ss_income / div) << ","
            << (yp.pension_income / div) << ","
            << (yp.rmd_amount / div) << ","
            << (yp.roth_conversion / div) << ","
            << (yp.withdraw_taxable / div) << ","
            << (yp.withdraw_tax_deferred / div) << ","
            << (yp.withdraw_tax_free / div) << ","
            << (yp.withdraw_hsa / div) << ","
            << (yp.ordinary_tax / div) << ","
            << (yp.ltcg_tax / div) << ","
            << (yp.niit_tax / div) << ","
            << (yp.state_tax / div) << ","
            << (yp.irmaa_surcharge / div) << ","
            << (yp.aca_premium / div) << ","
            << (yp.total_tax / div) << ","
            << (yp.net_spending_nominal / div) << ","
            << (yp.shortfall ? 1 : 0) << ","
            << (yp.lump_sum_net / div) << ","
            << (yp.rmd_reinvested / div) << ","
            << (yp.guaranteed_floor_nominal / div) << ","
            << (yp.floor_surplus_nominal / div) << ","
            << yp.marginal_bracket_pct << ","
            << yp.irmaa_tier << "\n";
    }
    return oss.str();
}

std::vector<SsBreakEvenRow> compute_ss_breakeven(const PlanInput& input)
{
    static constexpr int kMilestones[] = { 65, 70, 75, 80, 85, 90, 95 };
    std::vector<SsBreakEvenRow> rows;
    rows.reserve(9);

    auto cum_at_age = [&](int claim_age, int end_age) -> double {
        if (end_age < claim_age)
            return 0.0;
        double ann = adjusted_social_security(input.ss_fra_annual, claim_age);
        return ann * static_cast<double>(end_age - claim_age + 1);
    };

    for (int ca = 62; ca <= 70; ++ca)
    {
        SsBreakEvenRow row;
        row.claim_age = ca;
        row.annual_benefit_real = adjusted_social_security(input.ss_fra_annual, ca);

        for (int age = 62; age <= 100; ++age)
        {
            double c_ca = cum_at_age(ca, age);
            if (ca > 62 && row.breakeven_vs_62 == 0 && c_ca >= cum_at_age(62, age) && c_ca > 0.0)
                row.breakeven_vs_62 = age;
            if (ca > 67 && row.breakeven_vs_67 == 0 && c_ca >= cum_at_age(67, age) && c_ca > 0.0)
                row.breakeven_vs_67 = age;
        }
        for (int m : kMilestones)
            row.cumulative_at_ages.push_back(cum_at_age(ca, m));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string export_report_html(const PlanInput& input,
                               const PlanResult& result)
{
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    auto fmt_d = [](double v) -> std::string {
        std::ostringstream s;
        s.imbue(std::locale::classic());
        long long iv = std::llround(v);
        std::string raw = std::to_string(std::abs(iv));
        std::string out;
        int cnt = 0;
        for (auto it = raw.rbegin(); it != raw.rend(); ++it)
        {
            if (cnt && cnt % 3 == 0) out.push_back(',');
            out.push_back(*it);
            ++cnt;
        }
        std::reverse(out.begin(), out.end());
        return (iv < 0 ? "-$" : "$") + out;
    };

    oss << "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
           "<title>GnuCash + Owl Retirement Report</title>"
           "<style>"
           "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;margin:24px;color:#1e293b;background:#f8fafc;}"
           "h1{margin:0 0 4px;font-size:22px;color:#0f172a;} .sub{color:#64748b;font-size:13px;margin-bottom:18px;}"
           ".kpis{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:20px;}"
           ".card{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:12px 14px;}"
           ".card .lbl{font-size:11px;text-transform:uppercase;color:#64748b;letter-spacing:.04em;}"
           ".card .val{font-size:20px;font-weight:700;color:#0f172a;margin-top:4px;}"
           ".box{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:14px;margin-bottom:18px;}"
           "table{width:100%;border-collapse:collapse;font-size:12px;}"
           "th,td{padding:5px 7px;border-bottom:1px solid #e2e8f0;text-align:right;}"
           "th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:center;}"
           "th{background:#f1f5f9;font-weight:600;color:#334155;}"
           ".short{background:#fee2e2;color:#991b1b;}"
           ".disc{font-size:11px;color:#64748b;margin-top:16px;}"
           "@media print{body{background:#fff;margin:8mm;} @page{size:letter landscape;margin:10mm;}}"
           "</style></head><body>";

    oss << "<h1>Retirement Plan Report (GnuCash + Owl Experimental)</h1>"
        << "<div class=\"sub\">Strategy: <b>" << strategy_label(result.chosen_strategy)
        << "</b> &bull; SS Claim Age: <b>" << result.chosen_ss_age << "</b>"
        << (input.has_spouse ? (" &bull; Spouse SS Age: <b>" + std::to_string(result.chosen_spouse_ss_age) + "</b>") : "")
        << " &bull; State: <b>" << state_code(input.state) << "</b>"
        << " &bull; All dollar figures in today's real dollars</div>";

    oss << "<div class=\"kpis\">"
        << "<div class=\"card\"><div class=\"lbl\">"
        << (input.objective == PlanObjective::MaximizeSpending ? "Max Sustainable Spending / yr" : "Projected Final Bequest")
        << "</div><div class=\"val\">"
        << fmt_d(input.objective == PlanObjective::MaximizeSpending ? result.sustainable_spending_real : result.final_bequest_real)
        << "</div></div>"
        << "<div class=\"card\"><div class=\"lbl\">Advantage vs. Baseline</div><div class=\"val\">"
        << fmt_d(result.advantage_real) << "</div></div>"
        << "<div class=\"card\"><div class=\"lbl\">Lifetime Taxes &amp; Healthcare</div><div class=\"val\">"
        << fmt_d(result.lifetime_taxes_real) << "</div></div>"
        << "<div class=\"card\"><div class=\"lbl\">Monte Carlo Success (" << result.monte_carlo.trials << " trials)</div><div class=\"val\">"
        << std::fixed << std::setprecision(1) << (result.monte_carlo.success_rate * 100.0) << "%</div></div>"
        << "</div>";

    // Embedded SVG Portfolio Chart
    if (!result.schedule.empty())
    {
        const double W = 900.0, H = 260.0, PL = 68.0, PR = 20.0, PT = 18.0, PB = 30.0;
        const double PW = W - PL - PR, PH = H - PT - PB;
        double max_y = 1.0;
        for (const auto& yp : result.schedule)
            max_y = std::max(max_y, yp.end_total_real);
        max_y *= 1.08;
        const size_t n = result.schedule.size();
        auto x_at = [&](size_t idx) { return PL + (n > 1 ? (PW * idx / (n - 1)) : 0.0); };
        auto y_at = [&](double v) { return PT + PH - (PH * std::clamp(v / max_y, 0.0, 1.0)); };
        auto poly = [&](auto getter, const char* stroke, double width) {
            oss << "<polyline fill=\"none\" stroke=\"" << stroke << "\" stroke-width=\"" << width << "\" points=\"";
            for (size_t i = 0; i < n; ++i)
                oss << x_at(i) << "," << y_at(getter(result.schedule[i], i)) << " ";
            oss << "\"/>";
        };

        oss << "<div class=\"box\"><b>Portfolio Trajectory (Today's Dollars)</b><br>"
            << "<svg viewBox=\"0 0 " << W << " " << H << "\" style=\"width:100%;max-height:260px;\">"
            << "<line x1=\"" << PL << "\" y1=\"" << PT << "\" x2=\"" << PL << "\" y2=\"" << (PT + PH) << "\" stroke=\"#cbd5e1\"/>"
            << "<line x1=\"" << PL << "\" y1=\"" << (PT + PH) << "\" x2=\"" << (PL + PW) << "\" y2=\"" << (PT + PH) << "\" stroke=\"#cbd5e1\"/>"
            << "<text x=\"4\" y=\"" << (PT + 10) << "\" font-size=\"10\" fill=\"#64748b\">" << fmt_d(max_y) << "</text>"
            << "<text x=\"4\" y=\"" << (PT + PH) << "\" font-size=\"10\" fill=\"#64748b\">$0</text>"
            << "<text x=\"" << PL << "\" y=\"" << (H - 6) << "\" font-size=\"10\" fill=\"#64748b\">Age " << result.schedule.front().age << "</text>"
            << "<text x=\"" << (PL + PW - 45) << "\" y=\"" << (H - 6) << "\" font-size=\"10\" fill=\"#64748b\">Age " << result.schedule.back().age << "</text>";
        poly([](const YearProjection& yp, size_t) { return yp.end_total_real; }, "#0f172a", 2.5);
        poly([&](const YearProjection& yp, size_t i) { return yp.end_tax_deferred / std::pow(1.0 + input.inflation, static_cast<double>(i)); }, "#2563eb", 1.8);
        poly([&](const YearProjection& yp, size_t i) { return yp.end_taxable / std::pow(1.0 + input.inflation, static_cast<double>(i)); }, "#16a34a", 1.8);
        poly([&](const YearProjection& yp, size_t i) { return yp.end_tax_free / std::pow(1.0 + input.inflation, static_cast<double>(i)); }, "#9333ea", 1.8);
        oss << "</svg>"
            << "<div style=\"font-size:11px;color:#475569;margin-top:4px;\">"
            << "<span style=\"color:#0f172a;\">&#9632; Total</span> &nbsp; "
            << "<span style=\"color:#2563eb;\">&#9632; 401k/IRA</span> &nbsp; "
            << "<span style=\"color:#16a34a;\">&#9632; Taxable</span> &nbsp; "
            << "<span style=\"color:#9333ea;\">&#9632; Roth</span>"
            << "</div></div>";
    }

    oss << "<div class=\"box\"><b>Year-by-Year Schedule (Today's Dollars)</b><table><thead><tr>"
           "<th>Year</th><th>Age</th><th>End Total</th><th>Taxable</th><th>401k/IRA</th><th>Roth</th>"
           "<th>SS + Pension</th><th>Floor Gap</th><th>RMD</th><th>Roth Conv</th><th>Total Tax</th><th>Net Spend</th>"
           "</tr></thead><tbody>";

    for (size_t i = 0; i < result.schedule.size(); ++i)
    {
        const auto& yp = result.schedule[i];
        double inf = std::pow(1.0 + input.inflation, static_cast<double>(i));
        oss << "<tr" << (yp.shortfall ? " class=\"short\"" : "") << ">"
            << "<td>" << yp.year << "</td>"
            << "<td>" << yp.age << "</td>"
            << "<td>" << fmt_d(yp.end_total_real) << "</td>"
            << "<td>" << fmt_d(yp.end_taxable / inf) << "</td>"
            << "<td>" << fmt_d(yp.end_tax_deferred / inf) << "</td>"
            << "<td>" << fmt_d(yp.end_tax_free / inf) << "</td>"
            << "<td>" << fmt_d((yp.ss_income + yp.pension_income) / inf) << "</td>"
            << "<td>" << fmt_d(yp.floor_surplus_nominal / inf) << "</td>"
            << "<td>" << fmt_d(yp.rmd_amount / inf) << "</td>"
            << "<td>" << fmt_d(yp.roth_conversion / inf) << "</td>"
            << "<td>" << fmt_d(yp.total_tax / inf) << "</td>"
            << "<td>" << fmt_d(yp.net_spending_real) << "</td>"
            << "</tr>";
    }
    oss << "</tbody></table></div>"
        << "<div class=\"disc\">Experimental feature only. Not affiliated with or endorsed by official GnuCash or Owl (mdlacasse/Owl). "
           "Print this page (Ctrl+P / Save as PDF) to create a PDF copy.</div>"
        << "</body></html>";
    return oss.str();
}

} // namespace gnc::retirement
