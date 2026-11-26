# gnucash_plus_owl *(Experimental)*

> [!WARNING]
> **Experimental & Unofficial Fork:** This repository (`gnucash_plus_owl`) is an **experimental** personal project based on GnuCash 5.x. It is **not affiliated with, endorsed by, or related to** the official [GnuCash](https://www.gnucash.org/) project or the official [Owl](https://github.com/mdlacasse/Owl) project. Do not use it for official financial, tax, or accounting advice.

---

## Table of Contents

- [Overview](#overview)
- [Retirement Planner (Added Features)](#retirement-planner-added-features)
- [Building & Running](#building--running)
- [Upstream References & Licensing](#upstream-references--licensing)

---

## Overview

`gnucash_plus_owl` extends GnuCash (a personal and small-business double-entry accounting application) with a native C++17 / GTK3 retirement planning and tax-optimization module.

- **Official GnuCash Home:** https://www.gnucash.org/
- **Official GnuCash Wiki:** https://wiki.gnucash.org/wiki/GnuCash
- **Official GnuCash Downloads:** https://www.gnucash.org/download

---

## Retirement Planner (Added Features)

Available under **Tools → Retirement Planner (Owl)...**, this standalone module is inspired by the retirement planning concepts of [mdlacasse/Owl](https://github.com/mdlacasse/Owl).

### Account Mapping, Ledger Integration & Persistence
- **Live GnuCash Account Mapping & Expense Estimator:** Imports Asset/Bank/Stock/Mutual balances into `Taxable`, `Tax-Deferred` (401k/IRA), `Tax-Free` (Roth), `HSA`, and `Excluded` buckets, and includes **Est. Spend from GnuCash Book** to estimate annual spending from trailing 12-month Expense transactions.
- **Include Checkboxes, Search & Bulk Selection:** Individual `Include` checkboxes, live search filter, **Select All / Deselect All**, and **Auto-Classify** controls.
- **Preserved Manual Overrides & Persistent Selections:** Custom tax-bucket assignments and `Include` selections are persisted immediately per book (and preserved on **Refresh Accounts**).
- **Plan Files, Snapshots, Exports & In-App Help:**
  - **Plan ▾ → Save Plan As... / Load Plan...** (`*.owlplan`, `*.ini`) — automatically remembers and reloads your last `.owlplan` file on startup.
  - **Plan ▾ → Pin Snapshot (Plan A) / Clear Snapshot** for side-by-side **Plan A vs. Plan B** comparison.
  - **Plan ▾ → Export CSV... / Export HTML Report...** (print-ready HTML/PDF report with KPIs, SVG chart, Monte Carlo percentiles, Stress Tests, Historical Backtests, and Schedule).
  - **Help (F1):** Built-in 4-tab Owl Retirement Planner manual accessible via `F1`, **Help → Contents**, or the toolbar button.

### Tax, Healthcare & Optimization Engine
- **Two-Person Household (Spouse / Partner):** Optional spouse age/horizon/contributions, independent Social Security & pension modeling, survivor benefits, and joint SS claiming optimization.
- **Spending Phases, Debt, LTC & One-Time Events:** Go-Go / Slow-Go / No-Go spending phase multipliers, fixed nominal debt/mortgage payoff schedules, late-life Long-Term Care (LTC) shock modeling, and custom lump-sum windfalls or expenses.
- **Asset Allocation Glide Path & Guardrails:** Optional pre-retirement to in-retirement equity/bond glide path driving expected return/volatility, plus Guyton-Klinger spending guardrails (±10%).
- **Custom Cost Basis & Per-Bucket Returns:** Configurable initial taxable cost-basis percentage (`%`) for LTCG estimation and independent expected return rates per tax bucket (`Taxable`, `401k/IRA`, `Roth`, `HSA`).
- **Federal Taxes, Brackets & Medicare:** Ordinary income tax, LTCG, NIIT, IRMAA Medicare surcharges, taxable Social Security calculation, and year-by-year **Bracket / IRMAA** tier diagnostics.
- **50-State + DC Tax Modeling:** State income tax rules including state retirement-income and Social Security exemptions.
- **Pre-Medicare ACA Modeling:** Pre-65 ACA Marketplace subsidies and optional 400% FPL cliff enforcement.
- **RMD Re-investment, Income Floor & Roth/SS Optimization:** SECURE 2.0 RMD projections with optional after-tax RMD surplus re-investment into the taxable bucket, optional inflation-adjusted TIPS/annuity income floor tracking, Roth conversion bracket-fill strategies (`12%`, `22%`, `24%`), and Social Security claiming age optimization (`62–70`).
- **Interactive Charts, Stress Testing & Break-Even Analysis:**
  - **5 Chart Modes with Hover Tooltips:** `Portfolio by Tax Bucket`, `Income & Withdrawals`, `Taxes, RMDs & Roth Conv`, `Monte Carlo Fan (P10–P90)`, and `Tax & Healthcare Breakdown`.
  - **Stress & Sensitivity Tab:** Sequence-of-returns stress scenarios (Early Crash, Lost Decade, Stagflation), a 5×5 Return vs. Spending sensitivity matrix, and **Historical Market Backtests (1928–2024)** across 97 rolling cycles.
  - **SS Break-Even Tab:** Cumulative Social Security benefits across claiming ages `62–70` with break-even ages vs. 62 and 67.

---

## Building & Running

See [`README.dependencies`](README.dependencies) and [`cmake/README_CMAKE.txt`](cmake/README_CMAKE.txt) for full GnuCash build prerequisites.

```bash
# Configure and build (example)
cmake -B build -G Ninja
cmake --build build --target gnucash test-retirement-engine -j4

# Run the retirement engine unit tests
./build/bin/test-retirement-engine

# Launch GnuCash
./build/bin/gnucash
```

---

## Upstream References & Licensing

- See [`ATTRIBUTIONS.md`](ATTRIBUTIONS.md) for upstream repository attributions and license accounting.
- See [`LICENSE`](LICENSE) for GNU General Public License terms, and [`README`](README) for the original GnuCash plain-text README.
