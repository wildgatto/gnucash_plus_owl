# Upstream Attributions & License Accounting

This repository (`gnucash_plus_owl`) extends **GnuCash** with standalone, native C++/GTK3 retirement planning and tax-optimization capabilities.

## Primary Upstream Repositories

### 1. GnuCash (`Gnucash/gnucash`)
- **Repository:** https://github.com/Gnucash/gnucash
- **License:** GNU General Public License, Version 2.0 or later (`GPL-2.0-or-later`)
- **Scope:** Core double-entry accounting engine, GTK3 user interface, Guile reports, CMake build system, and existing plugins.
- **Notice:** See `LICENSE` and `AUTHORS` for full GnuCash licensing terms and contributor credits.

### 2. Owl — Optimal Wealth Lab (`mdlacasse/Owl`)
- **Repository:** https://github.com/mdlacasse/Owl
- **Author:** Martin-D. Lacasse
- **Upstream Licensing:**
  - Source code (`.py`, `.sh`): GNU General Public License v3.0 (`GPL-3.0`)
  - Documentation (`.md`, `.qmd`, `.ipynb`, `.tex`): Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (`CC-BY-NC-SA-4.0`)
  - Name, logo, and image assets: All Rights Reserved (Copyright © Martin-D. Lacasse)
- **Scope in `gnucash_plus_owl`:**
  - The standalone C++17 retirement planning engine (`gnucash/gnome/gnc-retirement-engine.hpp`, `gnucash/gnome/gnc-retirement-engine.cpp`), GTK3 plugin/UI (`gnucash/gnome/gnc-plugin-retirement.*`, `gnucash/gnome/gnc-plugin-page-retirement.*`, `gnucash/ui/gnc-plugin-retirement.ui`), and unit tests (`gnucash/gnome/test/gtest-retirement-engine.cpp`) are original C++17 implementations inspired by the retirement planning concepts, tax/RMD/Roth-conversion workflows, and baseline comparison methodology of `mdlacasse/Owl`.
  - No Owl name/logo/image assets or `CC-BY-NC-SA-4.0` documentation files are redistributed in this repository.
  - All new C/C++ source files in this repository are licensed under `GPL-2.0-or-later` (compatible with `GPL-3.0`).

## Bundled Third-Party Components (from GnuCash)
- `borrowed/chartjs-2/`: Chart.js (MIT License)
- `borrowed/ctre/`: Compile Time Regular Expressions (Apache-2.0 with LLVM-exception)
- `borrowed/goffice/`: GOffice utilities (GPL-2.0-or-later)
- `borrowed/libc/`: Portability helpers (BSD / LGPL / Public Domain; see individual files)
