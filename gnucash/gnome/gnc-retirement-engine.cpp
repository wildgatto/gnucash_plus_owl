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
    double total() const noexcept { return ordinary + ltcg + niit + state + irmaa + aca; }
};

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
    double bal_basis = std::clamp(input.initial_balances.taxable_basis, 0.0, bal_tax);
    double bal_def = std::max(0.0, input.initial_balances.tax_deferred);
    double bal_roth = std::max(0.0, input.initial_balances.tax_free);
    double bal_hsa = std::max(0.0, input.initial_balances.hsa);

    double cum_taxes_real = 0.0;
    double min_spending_real = std::numeric_limits<double>::infinity();
    bool any_shortfall = false;

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

        if (retired)
        {
            const double target_net = annual_spending_real * inf;

            // Fixed-point / iterative gross-up (converges in 4-6 iterations)
            for (int iter = 0; iter < 6; ++iter)
            {
                double ord_inc = pension + w_def + roth_conv;
                double cg = w_tax * gain_ratio();
                TaxBreakdown tb = compute_year_taxes(input, year_status, ord_inc, ss, cg, ord_inc, true, tax_age, aca_adults, inf);
                double current_net = ss + pension + w_tax + w_def + w_hsa + w_roth - tb.total();
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
        double ord_inc = pension + w_def + roth_conv;
        TaxBreakdown tb = compute_year_taxes(input, year_status, ord_inc, ss, cap_gains, ord_inc, retired, tax_age, aca_adults, inf);

        double gross_inflow = ss + pension + w_tax + w_def + w_hsa + w_roth;
        double net_spend = retired ? std::max(0.0, gross_inflow - tb.total()) : 0.0;
        double net_spend_real = net_spend / inf;

        if (retired && net_spend_real + 1.0 < annual_spending_real)
        {
            yp.shortfall = true;
            any_shortfall = true;
        }
        if (retired)
            min_spending_real = std::min(min_spending_real, net_spend_real);

        // 5. Apply market return to remaining end-of-year balances
        bal_tax *= (1.0 + ret);
        bal_def *= (1.0 + ret);
        bal_roth *= (1.0 + ret);
        bal_hsa *= (1.0 + ret);

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
            returns[static_cast<size_t>(y)] = std::clamp(dist(rng), -0.60, 0.80);

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
    const std::vector<std::pair<std::string, TaxBucket>>& account_overrides)
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

    if (!account_overrides.empty())
    {
        oss << "\n[AccountOverrides]\n";
        for (const auto& kv : account_overrides)
            oss << kv.first << "=" << bucket_label(kv.second) << "\n";
    }
    return oss.str();
}

bool deserialize_plan_ini(
    const std::string& ini_text,
    PlanInput& out_input,
    std::vector<std::pair<std::string, TaxBucket>>& out_overrides)
{
    PlanInput in = out_input;
    std::vector<std::pair<std::string, TaxBucket>> overrides;
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
            }
            catch (...)
            {
                // Ignore malformed individual fields
            }
        }
    }

    if (!saw_plan_key && overrides.empty())
        return false;

    in.retirement_age = std::max(in.current_age, in.retirement_age);
    in.horizon_age = std::max(in.retirement_age + 1, in.horizon_age);
    in.spouse_retirement_age = std::max(in.spouse_current_age, in.spouse_retirement_age);
    in.spouse_horizon_age = std::max(in.spouse_retirement_age + 1, in.spouse_horizon_age);
    out_input = in;
    out_overrides = std::move(overrides);
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
           "NetSpending,Shortfall\n";

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
            << (yp.shortfall ? 1 : 0) << "\n";
    }
    return oss.str();
}

} // namespace gnc::retirement
