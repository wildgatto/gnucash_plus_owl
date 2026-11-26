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
    ASSERT_EQ(res.monte_carlo.p10_real.size(), res.schedule.size());
    ASSERT_EQ(res.monte_carlo.p50_real.size(), res.schedule.size());
    ASSERT_EQ(res.monte_carlo.p90_real.size(), res.schedule.size());
    for (size_t i = 0; i < res.schedule.size(); ++i)
    {
        EXPECT_LE(res.monte_carlo.p10_real[i], res.monte_carlo.p25_real[i]);
        EXPECT_LE(res.monte_carlo.p25_real[i], res.monte_carlo.p50_real[i]);
        EXPECT_LE(res.monte_carlo.p50_real[i], res.monte_carlo.p75_real[i]);
        EXPECT_LE(res.monte_carlo.p75_real[i], res.monte_carlo.p90_real[i]);
    }
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

TEST(RetirementEngine, SpouseTwoPersonHouseholdAndSurvivorBenefit)
{
    PlanInput single_in;
    single_in.current_age = 62;
    single_in.retirement_age = 65;
    single_in.horizon_age = 90;
    single_in.filing_status = FilingStatus::MarriedFilingJointly;
    single_in.initial_balances.taxable = 300000.0;
    single_in.initial_balances.tax_deferred = 700000.0;
    single_in.ss_fra_annual = 34000.0;
    single_in.optimize_ss_age = false;
    single_in.strategy = WithdrawalStrategy::StandardTaxableFirst;
    single_in.monte_carlo_trials = 20;

    PlanResult r_one = optimize_plan(single_in);

    PlanInput couple_in = single_in;
    couple_in.has_spouse = true;
    couple_in.spouse_current_age = 60;
    couple_in.spouse_retirement_age = 65;
    couple_in.spouse_horizon_age = 94; // Spouse outlives primary by 6 years (age 88..94)
    couple_in.spouse_ss_fra_annual = 24000.0;
    couple_in.spouse_optimize_ss_age = true;
    couple_in.spouse_pension_annual = 10000.0;
    couple_in.spouse_pension_start_age = 65;

    PlanResult r_two = optimize_plan(couple_in);
    EXPECT_TRUE(r_two.plan_feasible);
    // Horizon extends to spouse's horizon (94 - 60 + 1 = 35 years)
    EXPECT_EQ(r_two.schedule.size(), 35u);
    EXPECT_GE(r_two.chosen_spouse_ss_age, 62);
    EXPECT_LE(r_two.chosen_spouse_ss_age, 70);
    // Spouse's SS + pension increases sustainable spending vs 1-earner
    EXPECT_GT(r_two.sustainable_spending_real, r_one.sustainable_spending_real);

    // Round-trip serialization of spouse fields
    std::string ini = serialize_plan_ini(couple_in);
    PlanInput roundtrip;
    std::vector<std::pair<std::string, TaxBucket>> ov;
    ASSERT_TRUE(deserialize_plan_ini(ini, roundtrip, ov));
    EXPECT_TRUE(roundtrip.has_spouse);
    EXPECT_EQ(roundtrip.spouse_current_age, 60);
    EXPECT_EQ(roundtrip.spouse_horizon_age, 94);
    EXPECT_DOUBLE_EQ(roundtrip.spouse_ss_fra_annual, 24000.0);
    EXPECT_DOUBLE_EQ(roundtrip.spouse_pension_annual, 10000.0);
}

TEST(RetirementEngine, SpendingPhasesAndLumpSumEvents)
{
    PlanInput base;
    base.current_age = 62;
    base.retirement_age = 65;
    base.horizon_age = 90;
    base.initial_balances.taxable = 400000.0;
    base.initial_balances.tax_deferred = 600000.0;
    base.ss_fra_annual = 30000.0;
    base.optimize_ss_age = false;
    base.strategy = WithdrawalStrategy::StandardTaxableFirst;
    base.monte_carlo_trials = 20;

    PlanResult r_flat = optimize_plan(base);

    // Slow-Go (80% at 75) & No-Go (65% at 85) allow higher initial Go-Go spending
    PlanInput phased = base;
    phased.slow_go_age = 75;
    phased.slow_go_pct = 80.0;
    phased.no_go_age = 85;
    phased.no_go_pct = 65.0;
    PlanResult r_phased = optimize_plan(phased);
    EXPECT_GT(r_phased.sustainable_spending_real, r_flat.sustainable_spending_real);

    // Windfall at age 68 boosts sustainable spending; expense at age 66 reduces it
    PlanInput windfall = base;
    windfall.events.push_back({"Home Downsize", 68, 250000.0, TaxBucket::Taxable});
    PlanResult r_windfall = optimize_plan(windfall);
    EXPECT_GT(r_windfall.sustainable_spending_real, r_flat.sustainable_spending_real);

    PlanInput expense = base;
    expense.events.push_back({"Mortgage Payoff", 66, -150000.0, TaxBucket::Taxable});
    PlanResult r_expense = optimize_plan(expense);
    EXPECT_LT(r_expense.sustainable_spending_real, r_flat.sustainable_spending_real);

    // INI round-trip for phases and events
    phased.events = {
        {"Home Downsize", 68, 250000.0, TaxBucket::Taxable},
        {"Mortgage Payoff", 66, -150000.0, TaxBucket::TaxFree}
    };
    std::string ini = serialize_plan_ini(phased);
    PlanInput out;
    std::vector<std::pair<std::string, TaxBucket>> ov;
    ASSERT_TRUE(deserialize_plan_ini(ini, out, ov));
    EXPECT_EQ(out.slow_go_age, 75);
    EXPECT_DOUBLE_EQ(out.slow_go_pct, 80.0);
    EXPECT_EQ(out.no_go_age, 85);
    EXPECT_DOUBLE_EQ(out.no_go_pct, 65.0);
    ASSERT_EQ(out.events.size(), 2u);
    EXPECT_EQ(out.events[0].name, "Home Downsize");
    EXPECT_EQ(out.events[0].age, 68);
    EXPECT_DOUBLE_EQ(out.events[0].amount_real, 250000.0);
    EXPECT_EQ(out.events[1].bucket, TaxBucket::TaxFree);
}

TEST(RetirementEngine, SequenceStressTestsAndSensitivityMatrix)
{
    PlanInput in;
    in.current_age = 62;
    in.retirement_age = 65;
    in.horizon_age = 90;
    in.initial_balances.taxable = 400000.0;
    in.initial_balances.tax_deferred = 700000.0;
    in.ss_fra_annual = 32000.0;
    in.optimize_ss_age = false;
    in.strategy = WithdrawalStrategy::StandardTaxableFirst;
    in.monte_carlo_trials = 20;

    PlanResult res = optimize_plan(in);
    ASSERT_TRUE(res.plan_feasible);

    auto stress = run_sequence_stress_tests(
        in, res.chosen_strategy, res.chosen_ss_age, res.sustainable_spending_real);
    ASSERT_EQ(stress.size(), 4u);
    EXPECT_TRUE(stress[0].feasible);
    // Severe early crash or stagflation should reduce bequest vs baseline expected return
    EXPECT_LT(stress[1].final_bequest_real, stress[0].final_bequest_real);
    EXPECT_LT(stress[3].final_bequest_real, stress[0].final_bequest_real);

    auto mat = run_sensitivity_matrix(
        in, res.chosen_strategy, res.chosen_ss_age, res.sustainable_spending_real);
    ASSERT_EQ(mat.return_rates.size(), 5u);
    ASSERT_EQ(mat.spending_levels.size(), 5u);
    ASSERT_EQ(mat.cells.size(), 5u);
    // Higher return & lower spending (row 4, col 0) should be feasible with larger bequest than center (2,2)
    EXPECT_TRUE(mat.cells[4][0].feasible);
    EXPECT_GT(mat.cells[4][0].final_bequest_real, mat.cells[2][2].final_bequest_real);
    // Lower return & higher spending (row 0, col 4) should show shortfall
    EXPECT_FALSE(mat.cells[0][4].feasible);
    EXPECT_GT(mat.cells[0][4].shortfall_years, 0);
}

TEST(RetirementEngine, CustomCostBasisAndPerBucketReturns)
{
    PlanInput base;
    base.current_age = 60;
    base.retirement_age = 65;
    base.horizon_age = 85;
    base.initial_balances.taxable = 400000.0;
    base.initial_balances.tax_free = 200000.0;
    base.contrib_taxable = 0.0;
    base.contrib_tax_deferred = 0.0;
    base.contrib_tax_free = 0.0;
    base.contrib_hsa = 0.0;
    base.nominal_return = 0.06;
    base.ss_fra_annual = 0.0;
    base.optimize_ss_age = false;
    base.strategy = WithdrawalStrategy::StandardTaxableFirst;
    base.monte_carlo_trials = 10;

    // 1. Lower cost basis (20%) means more embedded gains -> higher lifetime taxes than 95% basis
    PlanInput low_basis = base;
    low_basis.initial_balances.taxable = 2000000.0;
    low_basis.pension_annual = 80000.0;
    low_basis.pension_start_age = 65;
    low_basis.enable_aca = false;
    low_basis.taxable_basis_pct = 0.20;
    PlanInput high_basis = low_basis;
    high_basis.taxable_basis_pct = 0.95;

    PlanResult r_low_b = simulate_plan(low_basis, WithdrawalStrategy::StandardTaxableFirst, 67, 160000.0);
    PlanResult r_high_b = simulate_plan(high_basis, WithdrawalStrategy::StandardTaxableFirst, 67, 160000.0);
    EXPECT_GT(r_low_b.lifetime_taxes_real, r_high_b.lifetime_taxes_real);

    // 2. Per-bucket return: boosting Roth return increases ending tax-free balance
    PlanInput high_roth = base;
    high_roth.return_tax_free = 0.09;
    high_roth.return_taxable = 0.04;
    PlanResult r_base = simulate_plan(base, WithdrawalStrategy::StandardTaxableFirst, 67, 25000.0);
    PlanResult r_per_b = simulate_plan(high_roth, WithdrawalStrategy::StandardTaxableFirst, 67, 25000.0);
    EXPECT_GT(r_per_b.schedule.back().end_tax_free, r_base.schedule.back().end_tax_free);

    // 3. INI round-trip preserves cost basis % and per-bucket returns
    high_roth.taxable_basis_pct = 0.45;
    high_roth.return_tax_deferred = 0.07;
    high_roth.return_hsa = 0.05;
    std::string ini = serialize_plan_ini(high_roth);
    PlanInput rt;
    std::vector<std::pair<std::string, TaxBucket>> ov;
    ASSERT_TRUE(deserialize_plan_ini(ini, rt, ov));
    EXPECT_DOUBLE_EQ(rt.taxable_basis_pct, 0.45);
    EXPECT_DOUBLE_EQ(rt.return_taxable, 0.04);
    EXPECT_DOUBLE_EQ(rt.return_tax_deferred, 0.07);
    EXPECT_DOUBLE_EQ(rt.return_tax_free, 0.09);
    EXPECT_DOUBLE_EQ(rt.return_hsa, 0.05);
}

TEST(RetirementEngine, AccountSelectionAndBucketOverridePersistence)
{
    PlanInput in;
    std::vector<std::pair<std::string, TaxBucket>> overrides = {
        {"Assets:Brokerage", TaxBucket::Taxable},
        {"Assets:Retirement:401k", TaxBucket::TaxDeferred},
        {"Assets:Checking", TaxBucket::Taxable}
    };
    std::vector<std::string> excluded = {"Assets:Checking"};

    std::string ini = serialize_plan_ini(in, overrides, excluded);
    EXPECT_NE(ini.find("[AccountExcluded]"), std::string::npos);
    EXPECT_NE(ini.find("Assets:Checking=true"), std::string::npos);

    PlanInput rt;
    std::vector<std::pair<std::string, TaxBucket>> rt_ov;
    std::vector<std::string> rt_ex;
    ASSERT_TRUE(deserialize_plan_ini(ini, rt, rt_ov, &rt_ex));
    ASSERT_EQ(rt_ov.size(), 3u);
    EXPECT_EQ(rt_ov[2].first, "Assets:Checking");
    EXPECT_EQ(rt_ov[2].second, TaxBucket::Taxable); // underlying bucket preserved
    ASSERT_EQ(rt_ex.size(), 1u);
    EXPECT_EQ(rt_ex[0], "Assets:Checking");
}

TEST(RetirementEngine, RmdReinvestmentIncomeFloorSsBreakEvenAndHtmlReport)
{
    PlanInput in;
    in.current_age = 72;
    in.retirement_age = 72;
    in.horizon_age = 85;
    in.initial_balances.tax_deferred = 2500000.0; // Large IRA -> RMD exceeds spending
    in.contrib_taxable = 0.0;
    in.contrib_tax_deferred = 0.0;
    in.contrib_tax_free = 0.0;
    in.contrib_hsa = 0.0;
    in.ss_fra_annual = 36000.0;
    in.optimize_ss_age = false;
    in.strategy = WithdrawalStrategy::StandardTaxableFirst;
    in.reinvest_rmd_surplus = true;
    in.tips_floor_annual = 20000.0;
    in.monte_carlo_trials = 10;

    PlanResult r_reinv = simulate_plan(in, WithdrawalStrategy::StandardTaxableFirst, 67, 50000.0);
    EXPECT_GT(r_reinv.schedule.front().rmd_reinvested, 0.0);
    EXPECT_GT(r_reinv.schedule.back().end_taxable, 0.0);
    EXPECT_GT(r_reinv.schedule.front().guaranteed_floor_nominal, 20000.0);

    // SS Break-Even Analysis
    auto be = compute_ss_breakeven(in);
    ASSERT_EQ(be.size(), 9u); // ages 62..70
    EXPECT_EQ(be[0].claim_age, 62);
    EXPECT_EQ(be[8].claim_age, 70);
    EXPECT_GT(be[8].annual_benefit_real, be[0].annual_benefit_real);
    EXPECT_GE(be[8].breakeven_vs_62, 75);
    EXPECT_LE(be[8].breakeven_vs_62, 85);

    // HTML Report Export
    std::string html = export_report_html(in, r_reinv);
    EXPECT_NE(html.find("<!DOCTYPE html>"), std::string::npos);
    EXPECT_NE(html.find("<svg"), std::string::npos);
    EXPECT_NE(html.find("Year-by-Year Schedule"), std::string::npos);

    // INI round-trip for new fields
    std::string ini = serialize_plan_ini(in);
    PlanInput rt;
    std::vector<std::pair<std::string, TaxBucket>> ov;
    ASSERT_TRUE(deserialize_plan_ini(ini, rt, ov));
    EXPECT_TRUE(rt.reinvest_rmd_surplus);
    EXPECT_DOUBLE_EQ(rt.tips_floor_annual, 20000.0);
}
