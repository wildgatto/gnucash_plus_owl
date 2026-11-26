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

### Account Mapping & Persistence
- **Live GnuCash Account Mapping:** Imports Asset/Bank/Stock/Mutual balances and classifies them into `Taxable`, `Tax-Deferred` (401k/IRA), `Tax-Free` (Roth), `HSA`, and `Excluded` buckets.
- **Preserved Manual Overrides:** Custom tax-bucket assignments in the **Account Mapping** tab are preserved when clicking **Refresh Accounts**.
- **Per-Book Auto-Persistence:** Automatically saves and restores planner assumptions, display options (`Chart` mode, `Today's $`, `Planner Options` panel), and account bucket overrides in GnuCash's per-book state (`gnc_state_get_current()`).
- **Plan Files & CSV Export:**
  - **Plan ▾ → Save Plan As... / Load Plan...** (`*.owlplan`, `*.ini`)
  - **Plan ▾ → Reset to Defaults**
  - **Export CSV...** for the full year-by-year projection schedule (in Today's `$` or Nominal `$`).

### Tax, Healthcare & Optimization Engine
- **Federal Taxes & Medicare:** Ordinary income tax, LTCG, NIIT, IRMAA Medicare surcharges, and taxable Social Security calculation.
- **50-State + DC Tax Modeling:** State income tax rules including state retirement-income and Social Security exemptions.
- **Pre-Medicare ACA Modeling:** Pre-65 ACA Marketplace subsidies and optional 400% FPL cliff enforcement.
- **RMD, Roth & Social Security Optimization:** SECURE 2.0 RMD projections, Roth conversion bracket-fill strategies (`12%`, `22%`, `24%`), and Social Security claiming age optimization (`62–70`).
- **Comparison, Stress Testing & Charts:** **Baseline vs. Owl** comparison table, Monte Carlo stress testing (`success rate`, `P10 / P50 / P90`), and interactive Cairo charts (`Portfolio by Tax Bucket`, `Income & Withdrawals`, `Taxes, RMDs & Roth Conv`).

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
