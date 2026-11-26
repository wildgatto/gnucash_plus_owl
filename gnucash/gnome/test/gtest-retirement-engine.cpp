/********************************************************************
 * gtest-retirement-engine.cpp -- Unit tests for Owl-inspired       *
 * retirement planning engine.                                      *
 *                                                                  *
 * Inspired by mdlacasse/Owl (https://github.com/mdlacasse/Owl,     *
 * GPL-3.0). See ATTRIBUTIONS.md and LICENSE for upstream details.  *
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

#include <gtest/gtest.h>
#include "gnc-retirement-engine.hpp"

using namespace gnc::retirement;

TEST(RetirementEngine, AccountClassification)
{
    EXPECT_EQ(classify_account_name("Assets:Investments:Roth IRA"), TaxBucket::TaxFree);
    EXPECT_EQ(classify_account_name("Assets:Investments:Vanguard 401(k)"), TaxBucket::TaxDeferred);
    EXPECT_EQ(classify_account_name("Assets:Investments:Traditional IRA"), TaxBucket::TaxDeferred);
    EXPECT_EQ(classify_account_name("Assets:Health Savings Account (HSA)"), TaxBucket::HSA);
    EXPECT_EQ(classify_account_name("Assets:Investments:Brokerage"), TaxBucket::Taxable);
}

TEST(RetirementEngine, SocialSecurityClaimingAdjustment)
{
    const double fra = 30000.0;
    EXPECT_DOUBLE_EQ(adjusted_social_security(fra, 67), 30000.0);
    EXPECT_NEAR(adjusted_social_security(fra, 70), 37200.0, 1.0); // +24%
    EXPECT_NEAR(adjusted_social_security(fra, 62), 21000.0, 1.0); // -30%
}

TEST(RetirementEngine, RmdDivisorMonotonic)
{
    EXPECT_GT(rmd_divisor(73), rmd_divisor(80));
    EXPECT_GT(rmd_divisor(80), rmd_divisor(90));
    EXPECT_GT(rmd_divisor(90), rmd_divisor(100));
}

TEST(RetirementEngine, OptimizeMaximizeSpendingAndBaselineComparison)
{
    PlanInput input;
    input.current_age = 60;
    input.retirement_age = 65;
    input.horizon_age = 92;
    input.filing_status = FilingStatus::MarriedFilingJointly;
    input.initial_balances.taxable = 400000.0;
    input.initial_balances.taxable_basis = 280000.0;
    input.initial_balances.tax_deferred = 900000.0;
    input.initial_balances.tax_free = 150000.0;
    input.initial_balances.hsa = 40000.0;
    input.objective = PlanObjective::MaximizeSpending;
    input.target_bequest = 100000.0;
    input.ss_fra_annual = 36000.0;
    input.optimize_ss_age = true;
    input.strategy = WithdrawalStrategy::AutoOptimize;
    input.monte_carlo_trials = 100;

    PlanResult res = optimize_plan(input);
    EXPECT_TRUE(res.plan_feasible);
    EXPECT_EQ(res.schedule.size(), static_cast<size_t>(92 - 60 + 1));
    EXPECT_GT(res.sustainable_spending_real, 60000.0);
    EXPECT_GE(res.sustainable_spending_real, res.baseline_spending_real - 1.0);
    EXPECT_GE(res.advantage_real, -1.0);
    EXPECT_GT(res.monte_carlo.success_rate, 0.0);
    EXPECT_LE(res.monte_carlo.p10_end_real, res.monte_carlo.p50_end_real);
    EXPECT_LE(res.monte_carlo.p50_end_real, res.monte_carlo.p90_end_real);
}

TEST(RetirementEngine, OptimizeMaximizeBequest)
{
    PlanInput input;
    input.current_age = 62;
    input.retirement_age = 65;
    input.horizon_age = 90;
    input.filing_status = FilingStatus::Single;
    input.initial_balances.taxable = 300000.0;
    input.initial_balances.taxable_basis = 200000.0;
    input.initial_balances.tax_deferred = 700000.0;
    input.initial_balances.tax_free = 100000.0;
    input.objective = PlanObjective::MaximizeBequest;
    input.target_annual_spending = 65000.0;
    input.target_bequest = 0.0;
    input.optimize_ss_age = true;
    input.strategy = WithdrawalStrategy::AutoOptimize;
    input.monte_carlo_trials = 80;

    PlanResult res = optimize_plan(input);
    EXPECT_TRUE(res.plan_feasible);
    EXPECT_GE(res.final_bequest_real, res.baseline_bequest_real - 1.0);
}

TEST(RetirementEngine, StateTaxes50StatesAndExemptions)
{
    // No-tax state (FL) vs high-tax state (CA) vs retirement-exempt state (PA)
    double fl = compute_state_tax(USState::FL, 100000.0, 30000.0, 10000.0, 80000.0,
                                  FilingStatus::MarriedFilingJointly, 66, 1.0);
    double ca = compute_state_tax(USState::CA, 100000.0, 30000.0, 10000.0, 80000.0,
                                  FilingStatus::MarriedFilingJointly, 66, 1.0);
    double pa = compute_state_tax(USState::PA, 100000.0, 30000.0, 10000.0, 80000.0,
                                  FilingStatus::MarriedFilingJointly, 66, 1.0);
    EXPECT_DOUBLE_EQ(fl, 0.0);
    EXPECT_GT(ca, 2000.0);
    EXPECT_LT(pa, ca);

    PlanInput in;
    in.current_age = 60;
    in.retirement_age = 65;
    in.horizon_age = 90;
    in.initial_balances.tax_deferred = 1000000.0;
    in.objective = PlanObjective::MaximizeSpending;
    in.optimize_ss_age = false;
    in.strategy = WithdrawalStrategy::StandardTaxableFirst;
    in.monte_carlo_trials = 20;

    in.state = USState::TX;
    PlanResult r_tx = optimize_plan(in);
    in.state = USState::CA;
    PlanResult r_ca = optimize_plan(in);
    EXPECT_GT(r_tx.sustainable_spending_real, r_ca.sustainable_spending_real);
    EXPECT_LT(r_tx.lifetime_taxes_real, r_ca.lifetime_taxes_real);
}

TEST(RetirementEngine, AcaPre65SubsidyAnd400FplCliff)
{
    // Age 65+ has $0 ACA premium (Medicare)
    EXPECT_DOUBLE_EQ(compute_aca_premium(80000.0, FilingStatus::Single, 65, 8500.0, true, 1.0), 0.0);

    // Age 60 Single: 200% FPL (~$31,300) gets subsidized; 500% FPL (~$78,250) hits 400% cliff
    double sub = compute_aca_premium(31300.0, FilingStatus::Single, 60, 8500.0, true, 1.0);
    double cliff = compute_aca_premium(78250.0, FilingStatus::Single, 60, 8500.0, true, 1.0);
    EXPECT_GT(sub, 0.0);
    EXPECT_LT(sub, 8500.0);
    EXPECT_DOUBLE_EQ(cliff, 8500.0);

    // Verify simulation records ACA only before age 65 when retired and MAGI > 150% FPL
    PlanInput in;
    in.current_age = 60;
    in.retirement_age = 60;
    in.horizon_age = 70;
    in.initial_balances.tax_deferred = 800000.0;
    in.enable_aca = true;
    in.aca_benchmark_premium = 8500.0;
    PlanResult res = simulate_plan(in, WithdrawalStrategy::StandardTaxableFirst, 67, 50000.0);
    ASSERT_GE(res.schedule.size(), 7u);
    EXPECT_GT(res.schedule[0].aca_premium, 0.0); // age 60
    EXPECT_DOUBLE_EQ(res.schedule[5].aca_premium, 0.0); // age 65
}

TEST(RetirementEngine, PlanIniSerializationRoundTrip)
{
    PlanInput in;
    in.start_year = 2027;
    in.current_age = 52;
    in.retirement_age = 62;
    in.horizon_age = 96;
    in.filing_status = FilingStatus::Single;
    in.objective = PlanObjective::MaximizeBequest;
    in.strategy = WithdrawalStrategy::RothBracketFill22;
    in.target_annual_spending = 95000.0;
    in.target_bequest = 250000.0;
    in.ss_fra_annual = 34000.0;
    in.ss_claim_age = 70;
    in.optimize_ss_age = false;
    in.pension_annual = 12000.0;
    in.pension_start_age = 62;
    in.pension_cola = false;
    in.contrib_taxable = 15000.0;
    in.contrib_tax_deferred = 23500.0;
    in.contrib_tax_free = 8000.0;
    in.contrib_hsa = 4300.0;
    in.nominal_return = 0.072;
    in.volatility = 0.14;
    in.inflation = 0.028;
    in.rmd_start_age = 75;
    in.state = USState::NC;
    in.enable_aca = false;
    in.aca_benchmark_premium = 9200.0;
    in.aca_enforce_400_cliff = false;
    in.monte_carlo_trials = 500;
    in.random_seed = 99;

    std::vector<std::pair<std::string, TaxBucket>> overrides = {
        {"Assets:Investments:Vanguard 401(k)", TaxBucket::TaxDeferred},
        {"Assets:Investments:Brokerage", TaxBucket::Taxable},
        {"Assets:Cash:Emergency", TaxBucket::Excluded}
    };

    std::string ini = serialize_plan_ini(in, overrides);
    PlanInput out;
    std::vector<std::pair<std::string, TaxBucket>> out_overrides;
    ASSERT_TRUE(deserialize_plan_ini(ini, out, out_overrides));

    EXPECT_EQ(out.start_year, 2027);
    EXPECT_EQ(out.current_age, 52);
    EXPECT_EQ(out.retirement_age, 62);
    EXPECT_EQ(out.horizon_age, 96);
    EXPECT_EQ(out.filing_status, FilingStatus::Single);
    EXPECT_EQ(out.objective, PlanObjective::MaximizeBequest);
    EXPECT_EQ(out.strategy, WithdrawalStrategy::RothBracketFill22);
    EXPECT_DOUBLE_EQ(out.target_annual_spending, 95000.0);
    EXPECT_DOUBLE_EQ(out.target_bequest, 250000.0);
    EXPECT_DOUBLE_EQ(out.ss_fra_annual, 34000.0);
    EXPECT_EQ(out.ss_claim_age, 70);
    EXPECT_FALSE(out.optimize_ss_age);
    EXPECT_DOUBLE_EQ(out.pension_annual, 12000.0);
    EXPECT_EQ(out.pension_start_age, 62);
    EXPECT_FALSE(out.pension_cola);
    EXPECT_NEAR(out.nominal_return, 0.072, 1e-6);
    EXPECT_NEAR(out.volatility, 0.14, 1e-6);
    EXPECT_NEAR(out.inflation, 0.028, 1e-6);
    EXPECT_EQ(out.rmd_start_age, 75);
    EXPECT_EQ(out.state, USState::NC);
    EXPECT_FALSE(out.enable_aca);
    EXPECT_DOUBLE_EQ(out.aca_benchmark_premium, 9200.0);
    EXPECT_FALSE(out.aca_enforce_400_cliff);
    EXPECT_EQ(out.monte_carlo_trials, 500);
    EXPECT_EQ(out.random_seed, 99u);

    ASSERT_EQ(out_overrides.size(), 3u);
    EXPECT_EQ(out_overrides[0].first, "Assets:Investments:Vanguard 401(k)");
    EXPECT_EQ(out_overrides[0].second, TaxBucket::TaxDeferred);
    EXPECT_EQ(out_overrides[2].second, TaxBucket::Excluded);

    PlanInput bad;
    std::vector<std::pair<std::string, TaxBucket>> bad_ov;
    EXPECT_FALSE(deserialize_plan_ini("not a valid plan\n", bad, bad_ov));
}

TEST(RetirementEngine, ExportScheduleCsv)
{
    PlanInput in;
    in.start_year = 2026;
    in.current_age = 60;
    in.retirement_age = 65;
    in.horizon_age = 70;
    in.initial_balances.taxable = 200000.0;
    in.initial_balances.tax_deferred = 500000.0;
    in.monte_carlo_trials = 10;

    PlanResult res = simulate_plan(in, WithdrawalStrategy::StandardTaxableFirst, 67, 45000.0);
    std::string csv = export_schedule_csv(in, res, true);

    EXPECT_NE(csv.find("Year,Age,Retired,EndTotal,EndTaxable,EndTaxDeferred,EndRoth,EndHSA"), std::string::npos);
    EXPECT_NE(csv.find("2026,60,0,"), std::string::npos);
    EXPECT_NE(csv.find("2031,65,1,"), std::string::npos);

    // Count lines: 1 header + 11 projection rows (ages 60..70 inclusive)
    size_t lines = 0;
    for (char c : csv)
        if (c == '\n') ++lines;
    EXPECT_EQ(lines, 1 + res.schedule.size());
}
