/********************************************************************
 * gnc-plugin-page-estate.cpp -- Estate Planning & Continuity       *
 * Tracker for GnuCash.                                             *
 *                                                                  *
 * Tracks Florida Legal Documents, Account Titling & Beneficiary    *
 * Audits, Tax & Inherited-Account Continuity Rules, and ICE /      *
 * Digital Continuity Directories.                                  *
 *                                                                  *
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the   *
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
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "Account.h"
#include "gnc-filepath-utils.h"
#include "gnc-gobject-utils.h"
#include "gnc-icons.h"
#include "gnc-plugin-page-estate.h"
#include "gnc-plugin-retirement.h"
#include "gnc-retirement-engine.hpp"
#include "gnc-session.h"
#include "gnc-state.h"
#include "gnc-ui-util.h"

namespace {

enum EstateSection
{
    SEC_LEGAL = 0,
    SEC_TAX   = 1,
    SEC_ICE   = 2,
};

struct EstateChecklistItem
{
    bool done = false;
    int section = SEC_LEGAL;
    std::string title;
    std::string status = "Not Started";
    std::string due_trigger;
    std::string owner_agent;
    std::string completed_date;
    std::string details;
    std::string doc_path;
    std::string sp1_status = "Not Started";
    std::string sp1_date;
    std::string sp2_status = "Not Started";
    std::string sp2_date;
};

struct BeneficiaryAuditRow
{
    bool verified = false;
    std::string account_name;
    std::string approx_value;
    std::string primary_beneficiary;
    std::string contingent_structure;
    std::string recheck_trigger;
    std::string last_verified_date;
    std::string notes;
    std::string linked_gnucash_acct;
    std::string doc_path;
};

struct FiduciaryContact
{
    std::string role;
    std::string name;
    std::string firm_org;
    std::string phone;
    std::string email;
    std::string notes;
};

struct IceDirectoryRow
{
    bool verified = false;
    std::string category;
    std::string institution;
    std::string account_policy_id;
    std::string recovery_location;
    std::string notes;
};

struct SectionTabWidgets
{
    int section = SEC_LEGAL;
    GtkListStore* store = nullptr;
    GtkTreeView* tv = nullptr;
    GtkEntry* entry_title = nullptr;
    GtkComboBoxText* combo_status = nullptr;
    GtkEntry* entry_trigger = nullptr;
    GtkEntry* entry_owner = nullptr;
    GtkEntry* entry_date = nullptr;
    GtkEntry* entry_doc = nullptr;
    GtkComboBoxText* combo_sp1_status = nullptr;
    GtkEntry* entry_sp1_date = nullptr;
    GtkComboBoxText* combo_sp2_status = nullptr;
    GtkEntry* entry_sp2_date = nullptr;
    GtkTextView* tv_details = nullptr;
    GtkProgressBar* pbar = nullptr;
    int selected_idx = -1;
};

struct GncPluginPageEstatePrivate
{
    GtkWidget* root_widget = nullptr;
    GtkLabel* lbl_header = nullptr;
    GtkLabel* lbl_status = nullptr;
    GtkLabel* lbl_state_note = nullptr;

    // Metadata
    GtkEntry* entry_prepared_for = nullptr;
    GtkEntry* entry_scenarios = nullptr;
    GtkEntry* entry_heirs = nullptr;
    GtkComboBoxText* combo_state = nullptr;
    GtkEntry* entry_jurisdiction = nullptr;
    GtkEntry* entry_attorney = nullptr;
    GtkEntry* entry_safe_location = nullptr;
    GtkEntry* entry_last_audit = nullptr;
    GtkEntry* entry_next_audit = nullptr;

    // Overview dashboard
    GtkProgressBar* pbar_overall = nullptr;
    GtkProgressBar* pbar_legal = nullptr;
    GtkProgressBar* pbar_beneficiary = nullptr;
    GtkProgressBar* pbar_tax = nullptr;
    GtkProgressBar* pbar_ice = nullptr;
    GtkTextView* tv_triggers = nullptr;

    // Checklist sections (0=Legal, 1=Tax, 2=ICE)
    SectionTabWidgets sec_tabs[3];

    // Section 2: Beneficiary Audit
    GtkListStore* ben_store = nullptr;
    GtkTreeView* ben_tv = nullptr;
    GtkEntry* entry_ben_acct = nullptr;
    GtkEntry* entry_ben_val = nullptr;
    GtkEntry* entry_ben_primary = nullptr;
    GtkEntry* entry_ben_contingent = nullptr;
    GtkEntry* entry_ben_trigger = nullptr;
    GtkEntry* entry_ben_date = nullptr;
    GtkEntry* entry_ben_notes = nullptr;
    GtkComboBoxText* combo_ben_gnc_acct = nullptr;
    GtkEntry* entry_ben_doc = nullptr;
    GtkLabel* lbl_ben_unmapped = nullptr;
    int selected_ben_idx = -1;

    // Section 4: ICE Directory
    GtkListStore* ice_store = nullptr;
    GtkTreeView* ice_tv = nullptr;
    GtkComboBoxText* combo_ice_cat = nullptr;
    GtkEntry* entry_ice_inst = nullptr;
    GtkEntry* entry_ice_acct = nullptr;
    GtkEntry* entry_ice_recovery = nullptr;
    GtkEntry* entry_ice_notes = nullptr;
    int selected_ice_idx = -1;

    // Tab 6: Fiduciary & Contacts Roster
    GtkListStore* contact_store = nullptr;
    GtkTreeView* contact_tv = nullptr;
    GtkEntry* entry_ct_role = nullptr;
    GtkEntry* entry_ct_name = nullptr;
    GtkEntry* entry_ct_firm = nullptr;
    GtkEntry* entry_ct_phone = nullptr;
    GtkEntry* entry_ct_email = nullptr;
    GtkEntry* entry_ct_notes = nullptr;
    int selected_contact_idx = -1;

    // Tab 7: Heir 10-Yr Tax Estimator
    GtkSpinButton* spin_ht_trad = nullptr;
    GtkSpinButton* spin_ht_roth = nullptr;
    GtkSpinButton* spin_ht_heirs = nullptr;
    GtkSpinButton* spin_ht_rate = nullptr;
    GtkSpinButton* spin_ht_growth = nullptr;
    GtkComboBoxText* combo_ht_strategy = nullptr;
    GtkTextView* tv_ht_report = nullptr;

    // Tab 8: Ask Estate AI
    GtkComboBoxText* combo_ai_provider = nullptr;
    GtkEntry* entry_ai_endpoint = nullptr;
    GtkComboBoxText* combo_ai_model = nullptr;
    GtkEntry* entry_ai_model = nullptr;
    GtkEntry* entry_ai_key = nullptr;
    GtkEntry* entry_ai_question = nullptr;
    GtkTextView* tv_ai_system = nullptr;
    GtkTextView* tv_ai_response = nullptr;
    GtkLabel* lbl_ai_status = nullptr;
    GtkWidget* btn_ai_stop = nullptr;
    SoupSession* soup_session = nullptr;
    GCancellable* ai_cancel = nullptr;

    // Data
    std::vector<EstateChecklistItem> items;
    std::vector<BeneficiaryAuditRow> ben_rows;
    std::vector<IceDirectoryRow> ice_rows;
    std::vector<FiduciaryContact> contacts;
    bool updating_ui = false;
};

#define GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(o) \
    ((GncPluginPageEstatePrivate*)gnc_plugin_page_estate_get_instance_private((GncPluginPageEstate*)(o)))

static constexpr const char* ESTATE_GROUP_META = "EstateMeta";
static constexpr const char* ESTATE_GROUP_ITEMS = "EstateChecklistItems";
static constexpr const char* ESTATE_GROUP_BEN = "EstateBeneficiaries";
static constexpr const char* ESTATE_GROUP_ICE = "EstateIceDirectory";
static constexpr const char* ESTATE_GROUP_CONTACTS = "EstateContacts";
static constexpr const char* ESTATE_GROUP_HEIR_TAX = "EstateHeirTax";

static std::string
today_iso_date()
{
    std::time_t t = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&t, &tm_now);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
    return buf;
}

static bool
is_date_stale(const std::string& iso_date)
{
    if (iso_date.size() < 10)
        return true;
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso_date.c_str(), "%d-%d-%d", &y, &m, &d) != 3)
        return true;
    std::tm tm_d{};
    tm_d.tm_year = y - 1900;
    tm_d.tm_mon = m - 1;
    tm_d.tm_mday = d;
    tm_d.tm_isdst = -1;
    std::time_t t_d = std::mktime(&tm_d);
    if (t_d == static_cast<std::time_t>(-1))
        return true;
    double days = std::difftime(std::time(nullptr), t_d) / 86400.0;
    return days > 365.0;
}

static std::string
fmt_money_str(double v)
{
    long long iv = static_cast<long long>(std::llround(v));
    bool neg = iv < 0;
    if (neg) iv = -iv;
    std::string digits = std::to_string(iv);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return (neg ? "-$" : "$") + out;
}

struct GncAssetAccountInfo
{
    std::string full_name;
    double balance = 0.0;
    bool is_leaf = true;
};

static std::vector<GncAssetAccountInfo>
collect_gnucash_asset_accounts()
{
    std::vector<GncAssetAccountInfo> out;
    QofBook* book = gnc_get_current_book();
    if (!book) return out;
    Account* root = gnc_book_get_root_account(book);
    if (!root) return out;
    const gnc_commodity* def_curr = gnc_default_currency();
    GList* all_accts = gnc_account_get_descendants_sorted(root);
    for (GList* n = all_accts; n; n = n->next)
    {
        Account* a = GNC_ACCOUNT(n->data);
        GNCAccountType t = xaccAccountGetType(a);
        if (t == ACCT_TYPE_BANK || t == ACCT_TYPE_CASH || t == ACCT_TYPE_STOCK ||
            t == ACCT_TYPE_MUTUAL || t == ACCT_TYPE_ASSET)
        {
            gchar* fname = gnc_account_get_full_name(a);
            double bal = gnc_numeric_to_double(xaccAccountGetBalanceInCurrency(a, def_curr, TRUE));
            bool leaf = (gnc_account_n_children(a) == 0);
            if (fname)
            {
                out.push_back({fname, bal, leaf});
                g_free(fname);
            }
        }
    }
    g_list_free(all_accts);
    return out;
}

static void
browse_and_set_doc_path(GtkEntry* entry)
{
    if (!entry) return;
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Select Document or Scan"), nullptr, GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Open"), GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (fn)
        {
            gtk_entry_set_text(entry, fn);
            g_free(fn);
        }
    }
    gtk_widget_destroy(dlg);
}

static void
open_doc_path(GtkEntry* entry, GtkLabel* status_lbl)
{
    if (!entry) return;
    std::string p = gtk_entry_get_text(entry);
    if (p.empty()) return;
    std::string uri = p;
    if (p.find("://") == std::string::npos)
    {
        gchar* u = g_filename_to_uri(p.c_str(), nullptr, nullptr);
        if (u)
        {
            uri = u;
            g_free(u);
        }
    }
    GError* err = nullptr;
    if (!gtk_show_uri_on_window(nullptr, uri.c_str(), GDK_CURRENT_TIME, &err))
    {
        if (status_lbl)
            gtk_label_set_text(status_lbl, err ? err->message : _("Could not open document."));
        if (err) g_error_free(err);
    }
}

static std::string
estate_config_file_path()
{
    gchar* p = gnc_build_userdata_path("estate-plan.conf");
    std::string out = p ? p : "";
    g_free(p);
    return out;
}

static std::string
sanitize_field(std::string s)
{
    for (char& c : s)
    {
        if (c == '|' || c == '\n' || c == '\r')
            c = ' ';
    }
    return s;
}

static std::string
escape_multiline(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '|') out += "/";
        else if (c != '\r') out += c;
    }
    return out;
}

static std::string
unescape_multiline(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            if (s[i + 1] == 'n') { out += '\n'; ++i; continue; }
            if (s[i + 1] == '\\') { out += '\\'; ++i; continue; }
        }
        out += s[i];
    }
    return out;
}

static std::vector<std::string>
split_pipes(const std::string& s, size_t expected_min)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (true)
    {
        size_t pos = s.find('|', start);
        if (pos == std::string::npos)
        {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    while (parts.size() < expected_min)
        parts.emplace_back();
    return parts;
}

static std::string
html_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
    {
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

static void
populate_default_florida_checklist(GncPluginPageEstatePrivate* priv)
{
    priv->items.clear();
    priv->ben_rows.clear();
    priv->ice_rows.clear();

    // Section 1: Core Florida Legal Documents
    priv->items.push_back({
        false, SEC_LEGAL,
        "Last Will & Testament (with Self-Proving Affidavit)",
        "Not Started", "2026 / Pre-Retirement", "Florida Estate Attorney", "",
        "Names personal representative (executor), distributes assets lacking a beneficiary designation, "
        "and serves as a \"pour-over\" safety net if using a trust. Confirm two witnesses and a notarized "
        "self-proving affidavit so witnesses do not have to appear in probate court."
    });
    priv->items.push_back({
        false, SEC_LEGAL,
        "Revocable Living Trust OR Florida Lady Bird Deed (Wood Duck Home)",
        "Not Started", "2026 / Pre-Retirement", "Florida Estate Attorney", "",
        "Avoids Florida probate at the second spouse's death while transferring equally to Hannah & Peyton (50/50) "
        "outside probate, preserving full step-up in cost basis and Hillsborough County Homestead Exemption.\n"
        "- Lady Bird Deed (Enhanced Life Estate Deed): Lower cost & simple; retains full control to sell/refinance during lifetimes.\n"
        "- Revocable Living Trust: Preferred for unified management during incapacity, staged distributions, or multi-property coverage."
    });
    priv->items.push_back({
        false, SEC_LEGAL,
        "Florida Durable Power of Attorney (Financial)",
        "Not Started", "2026 / Pre-Retirement", "Florida Estate Attorney", "",
        "Appoints spouse plus backup agent (evaluate Hannah [age 26 Apr 2027] and/or Peyton as successor/co-agents) "
        "to manage financial accounts, tax filings, and retirement rollovers if incapacitated. Confirm all \"superpowers\" "
        "(gifting, beneficiary changes, trust creation/amendment) are separately initialed per post-2011 Florida statute "
        "(Florida does not recognize springing POAs)."
    });
    priv->items.push_back({
        false, SEC_LEGAL,
        "Designation of Health Care Surrogate & HIPAA Release",
        "Not Started", "2026 / Pre-Retirement", "Florida Estate Attorney", "",
        "Names who makes medical decisions if you cannot and grants immediate access to medical records "
        "(evaluate Hannah [age 26 in Apr 2027] and/or Peyton as backup or successor surrogates)."
    });
    priv->items.push_back({
        false, SEC_LEGAL,
        "Living Will (Florida Statutory End-of-Life Directive)",
        "Not Started", "2026 / Pre-Retirement", "Florida Estate Attorney", "",
        "Documents end-of-life care preferences under Florida statutes."
    });

    // Section 3: Tax & Inherited-Account Continuity Rules
    priv->items.push_back({
        false, SEC_TAX,
        "Avoid the HSA Non-Spouse Tax Trap",
        "Not Started", "2027/2028 (Upon HSA Opening)", "Greg & Tammy", "",
        "A surviving spouse inherits an HSA tax-free as their own HSA, whereas a non-spouse (or estate) faces "
        "100% ordinary income taxation in the year of death. Never name \"My Estate\" as beneficiary, and spend down "
        "HSAs first later in life if only one spouse remains."
    });
    priv->items.push_back({
        false, SEC_TAX,
        "Plan Around the SECURE Act 10-Year Rule (Roth Conversion Ladder)",
        "Not Started", "2036–2043 Conversion Window", "Greg & Tammy", "",
        "Surviving spouses can roll inherited IRAs into their own IRA, while non-spouse heirs (Hannah & Peyton) "
        "must empty inherited Traditional IRAs within 10 years. Execute the 2036–2043 Roth conversion ladder so "
        "inherited Roth balances grow and distribute tax-free over that 10-year window (~95.5% starting portfolio is pre-tax)."
    });
    priv->items.push_back({
        false, SEC_TAX,
        "Confirm \"Per Stirpes\" Designations Across All Accounts",
        "Not Started", "Late 2026 / Ongoing", "Greg & Tammy", "",
        "Select per stirpes (not per capita) across all retirement and TOD/POD accounts so a predeceased "
        "contingent beneficiary's share passes to their descendants."
    });
    priv->items.push_back({
        false, SEC_TAX,
        "File the Survivor Social Security Playbook in Estate Binder",
        "Not Started", "2026 / Pre-Retirement", "Greg & Tammy", "",
        "Place a one-page instruction sheet in the estate binder noting that upon the first spouse's death, "
        "the survivor should contact SSA immediately to switch to the higher benefit (Greg's verified $45,900/yr "
        "stopped-work FRA benefit at 67, or $56,916/yr if delayed to 70) rather than keeping the smaller check."
    });
    priv->items.push_back({
        false, SEC_TAX,
        "April 2027 Health Coverage Check (Hannah Turns 26)",
        "Not Started", "April 2027", "Greg & Tammy / Hannah", "",
        "Confirm Hannah's transition to her own health insurance when she turns 26 in April 2027 "
        "(employer plans typically end coverage at the end of the birth month; ACA Marketplace plans allow "
        "coverage through Dec 31 of the year turning 26)."
    });

    // Section 4: Operational ICE & Digital Continuity
    priv->items.push_back({
        false, SEC_ICE,
        "Master Account & Bill-Pay Directory (Encrypted / Safe Copy)",
        "Not Started", "Late 2026", "Greg & Tammy", "",
        "Maintain one encrypted (or fireproof-safe) sheet listing every financial institution, account number, "
        "insurance policy (home, auto, umbrella, ACA/Medicare), Hillsborough County property tax parcel ID, "
        "and recurring auto-pay bills."
    });
    priv->items.push_back({
        false, SEC_ICE,
        "Password Manager & 2FA Recovery Keys in Physical Safe",
        "Not Started", "Late 2026", "Greg & Tammy", "",
        "Enable emergency access or store master credentials, authenticator recovery codes, and hardware backup keys "
        "in the physical safe so Fidelity, NFCU, SSA, and HealthCare.gov remain accessible."
    });
    priv->items.push_back({
        false, SEC_ICE,
        "Email & Personal Domain Continuity Documentation",
        "Not Started", "Late 2026", "Greg & Tammy", "",
        "Document registrar logins, renewal dates, and admin credentials for personal email domain and cloud "
        "storage so account-recovery emails never lapse."
    });
    priv->items.push_back({
        false, SEC_ICE,
        "Annual November 15 Estate, Beneficiary & ICE Audit",
        "Not Started", "Annual Nov 15", "Greg & Tammy", "",
        "Add a 15-minute beneficiary, titling, and ICE directory review to the annual November 15 Year-End Tax "
        "& MAGI Audit to catch life events, new accounts, or Florida law updates."
    });

    // Section 2: Account Titling & Beneficiary Audit
    priv->ben_rows.push_back({
        false, "Greg's 401(k)", "$1,144,849",
        "Tammy (100%)", "Hannah & Peyton (50/50, per stirpes)",
        "Feb 2029: Re-enter immediately upon Fidelity Traditional IRA rollover", "", ""
    });
    priv->ben_rows.push_back({
        false, "Tammy's 403(b) & 401(a)", "$584,862",
        "Greg (100%)", "Hannah & Peyton (50/50, per stirpes)",
        "Jul 2030: Re-enter immediately upon Fidelity Traditional IRA rollover", "", ""
    });
    priv->ben_rows.push_back({
        false, "Fidelity Traditional & Roth IRAs", "$54,100+ (legacy Roths)",
        "Spouse (100%)", "Hannah & Peyton (50/50, per stirpes)",
        "Late 2026 / 2027: Verify after moving Greg's $26k & Tammy's $28k legacy ~2011 Roth IRAs to Fidelity & 2026/2027 conversions", "", ""
    });
    priv->ben_rows.push_back({
        false, "Future HSAs (Separate per Spouse)", "TBD (2027/2028)",
        "Spouse MUST be Primary (100%)", "Hannah & Peyton (50/50, per stirpes)",
        "2027/2028: Immediately upon opening each Bronze+HSA account", "", ""
    });
    priv->ben_rows.push_back({
        false, "NFCU Accounts (-3190, Checking, CDs & Travel Cash)", "Cash Reserve",
        "JTWROS (Spouse)", "POD to Hannah & Peyton (50/50, per stirpes)",
        "Late 2026 / Nov 2027: Verify POD active on every sub-account (incl. pre-59.5 travel cash)", "", ""
    });
    priv->ben_rows.push_back({
        false, "Fidelity Taxable Brokerage", "Brokerage",
        "JTWROS (Spouse)", "TOD to Hannah & Peyton (50/50, per stirpes)",
        "Late 2026: Confirm TOD form on file after resolving cash/margin artifact", "", ""
    });
    priv->ben_rows.push_back({
        false, "Peyton's 529 Plan", "$12,416",
        "Owner (Current Spouse)", "Successor Owner: Other Spouse",
        "Late 2026: Prevents probate freeze before May 2028 graduation", "", ""
    });
    priv->ben_rows.push_back({
        false, "Wood Duck Home (Primary Residence)", "$626,300",
        "Tenants by the Entirety (TBE)", "Lady Bird Deed or Revocable Trust (50/50)",
        "2027: Pull Hillsborough County recorded deed to confirm exact wording", "", ""
    });
    priv->ben_rows.push_back({
        false, "Paid-Off Vehicles (Tacoma, Z4, etc.)", "Vehicles",
        "Joint Title with \"OR\" (not \"AND\")", "Florida exempt-property rules",
        "2027: Check Florida DMV titles for \"OR\" survivorship wording", "", ""
    });

    // Section 4: Starter ICE Directory
    priv->ice_rows.push_back({false, "Financial", "Fidelity Investments", "IRAs / Brokerage / 401k Rollovers", "Password Manager + Safe Backup", "Verify TOD/Beneficiaries"});
    priv->ice_rows.push_back({false, "Financial", "Navy Federal Credit Union (NFCU)", "-3190, Checking, CDs, Travel Cash", "Password Manager + Safe Backup", "Verify POD on all sub-accounts"});
    priv->ice_rows.push_back({false, "Property/Tax", "Hillsborough County Property Appraiser / Tax", "Wood Duck Home Parcel ID", "Estate Binder + Safe", "Verify Homestead & TBE/Lady Bird Deed"});
    priv->ice_rows.push_back({false, "Insurance", "Home / Auto / Umbrella / ACA / Medicare", "Policy #s & Carrier Portals", "Estate Binder + Password Manager", "Check auto-pay & Hannah Apr 2027 transition"});
    priv->ice_rows.push_back({false, "Password/2FA", "Password Manager & Hardware 2FA Keys", "Master Vault + Recovery Codes", "Home Fireproof Safe", "Test emergency access annually"});
    priv->ice_rows.push_back({false, "Email/Domain", "Personal Email & Domain Registrar", "Domain Admin & Cloud Storage", "Password Manager + Safe Backup", "Check renewal dates & recovery email"});
    priv->ice_rows.push_back({false, "Financial", "SSA.gov / Login.gov", "Greg & Tammy Benefit Accounts", "Password Manager + Safe Backup", "Includes Survivor SS Playbook"});

    priv->contacts.clear();
    priv->contacts.push_back({"Personal Representative (Executor)", "Tammy / Greg (Spouse)", "", "", "", "Primary executor named in Will"});
    priv->contacts.push_back({"Backup Executor", "", "", "", "", "Alternate if spouse unable to serve"});
    priv->contacts.push_back({"Financial POA Agent", "Spouse", "", "", "", "Immediate durable financial authority"});
    priv->contacts.push_back({"Backup Financial POA Agent", "Hannah / Peyton (evaluate)", "", "", "", "Successor agent when age-eligible"});
    priv->contacts.push_back({"Health Care Surrogate", "Spouse", "", "", "", "Primary medical decision-maker"});
    priv->contacts.push_back({"Backup Health Care Surrogate", "Hannah / Peyton (evaluate)", "", "", "", "Successor medical surrogate"});
    priv->contacts.push_back({"Florida Estate Planning Attorney", "", "", "", "", "Drafts/reviews Will, Trust/Lady Bird, POA, Surrogate"});
    priv->contacts.push_back({"CPA / Tax Advisor", "", "", "", "", "Roth ladder, MAGI, and estate/final return"});
    priv->contacts.push_back({"Insurance Agent / Broker", "", "", "", "", "Home, auto, umbrella, ACA/Medicare"});
}

static void
populate_default_contacts_only(GncPluginPageEstatePrivate* priv)
{
    priv->contacts.clear();
    priv->contacts.push_back({"Personal Representative (Executor)", "Spouse / Primary", "", "", "", "Primary executor named in Will"});
    priv->contacts.push_back({"Backup Executor", "", "", "", "", "Alternate executor"});
    priv->contacts.push_back({"Financial POA Agent", "Spouse / Primary", "", "", "", "Durable financial power of attorney"});
    priv->contacts.push_back({"Backup Financial POA Agent", "", "", "", "", "Successor financial agent"});
    priv->contacts.push_back({"Health Care Surrogate / Proxy", "Spouse / Primary", "", "", "", "Medical decision-maker"});
    priv->contacts.push_back({"Estate Planning Attorney", "", "", "", "", "Drafts/reviews state estate documents"});
    priv->contacts.push_back({"CPA / Tax Advisor", "", "", "", "", "Tax & inherited IRA planning"});
    priv->contacts.push_back({"Insurance Agent / Broker", "", "", "", "", "Home, auto, umbrella, health"});
}

static void
populate_generic_us_template(GncPluginPageEstatePrivate* priv, const std::string& state_name)
{
    priv->items.clear();
    priv->ben_rows.clear();
    priv->ice_rows.clear();

    priv->items.push_back({false, SEC_LEGAL, "Last Will & Testament (Self-Proving Affidavit)", "Not Started", "Pre-Retirement", state_name + " Estate Attorney", "", "Names executor/personal representative, guardianship if needed, and pour-over provisions.", ""});
    priv->items.push_back({false, SEC_LEGAL, "Revocable Living Trust or Transfer-on-Death (TOD) Deed", "Not Started", "Pre-Retirement", state_name + " Estate Attorney", "", "Avoids probate in " + state_name + " and provides incapacity management.", ""});
    priv->items.push_back({false, SEC_LEGAL, "Durable Power of Attorney (Financial)", "Not Started", "Pre-Retirement", state_name + " Estate Attorney", "", "Verify state-specific statutory POA requirements in " + state_name + ".", ""});
    priv->items.push_back({false, SEC_LEGAL, "Health Care Proxy / Advance Directive & HIPAA Release", "Not Started", "Pre-Retirement", state_name + " Estate Attorney", "", "Appoints medical agent and grants HIPAA record access.", ""});
    priv->items.push_back({false, SEC_LEGAL, "Living Will", "Not Started", "Pre-Retirement", state_name + " Estate Attorney", "", "Documents end-of-life care preferences.", ""});

    priv->items.push_back({false, SEC_TAX, "Avoid HSA Non-Spouse Tax Trap", "Not Started", "Ongoing", "Account Owner", "", "Name spouse as primary HSA beneficiary; non-spouse heirs face 100% ordinary income tax at death.", ""});
    priv->items.push_back({false, SEC_TAX, "Plan Around SECURE Act 10-Year Rule", "Not Started", "Ongoing", "Account Owner", "", "Non-spouse heirs must empty inherited IRAs within 10 years; evaluate Roth conversions.", ""});
    priv->items.push_back({false, SEC_TAX, "Confirm Per Stirpes Designations", "Not Started", "Annual Audit", "Account Owner", "", "Use per stirpes across retirement and TOD/POD accounts.", ""});
    priv->items.push_back({false, SEC_TAX, "Survivor Social Security & Pension Playbook", "Not Started", "Pre-Retirement", "Spouse / Executor", "", "Document exact claiming instructions so surviving spouse switches to higher benefit.", ""});

    priv->items.push_back({false, SEC_ICE, "Master Account & Bill-Pay Directory", "Not Started", "Annual Audit", "Household", "", "Encrypted/safe sheet listing institutions, policies, property tax IDs, and auto-pay bills.", ""});
    priv->items.push_back({false, SEC_ICE, "Password Manager & 2FA Recovery Keys", "Not Started", "Annual Audit", "Household", "", "Store emergency access and hardware 2FA backup codes in physical safe.", ""});
    priv->items.push_back({false, SEC_ICE, "Email & Domain Continuity", "Not Started", "Annual Audit", "Household", "", "Document registrar logins and recovery emails.", ""});

    priv->ben_rows.push_back({false, "Primary 401(k) / 403(b)", "", "Spouse (100%)", "Heirs (per stirpes)", "Re-verify upon IRA rollover", "", "", "", ""});
    priv->ben_rows.push_back({false, "Traditional & Roth IRAs", "", "Spouse (100%)", "Heirs (per stirpes)", "Verify after annual contributions/conversions", "", "", "", ""});
    priv->ben_rows.push_back({false, "HSA(s)", "", "Spouse (100%)", "Heirs (per stirpes)", "Verify upon opening", "", "", "", ""});
    priv->ben_rows.push_back({false, "Checking / Savings / CDs", "", "JTWROS", "POD to Heirs", "Annual audit", "", "", "", ""});
    priv->ben_rows.push_back({false, "Taxable Brokerage", "", "JTWROS", "TOD to Heirs", "Annual audit", "", "", "", ""});
    priv->ben_rows.push_back({false, "Primary Residence", "", "Joint / TBE / Trust", "Trust or TOD/Lady Bird Deed", "Verify recorded deed", "", "", "", ""});

    priv->ice_rows.push_back({false, "Financial", "Primary Brokerage / IRA Custodian", "", "Password Manager + Safe", ""});
    priv->ice_rows.push_back({false, "Financial", "Primary Bank / Credit Union", "", "Password Manager + Safe", ""});
    priv->ice_rows.push_back({false, "Insurance", "Home / Auto / Umbrella / Health", "", "Estate Binder", ""});
    priv->ice_rows.push_back({false, "Password/2FA", "Password Manager & 2FA Backup Keys", "", "Fireproof Safe", ""});

    populate_default_contacts_only(priv);
}

static const char* const US_STATES[] = {
    "Alabama", "Alaska", "Arizona", "Arkansas", "California",
    "Colorado", "Connecticut", "Delaware", "District of Columbia", "Florida",
    "Georgia", "Hawaii", "Idaho", "Illinois", "Indiana",
    "Iowa", "Kansas", "Kentucky", "Louisiana", "Maine",
    "Maryland", "Massachusetts", "Michigan", "Minnesota", "Mississippi",
    "Missouri", "Montana", "Nebraska", "Nevada", "New Hampshire",
    "New Jersey", "New Mexico", "New York", "North Carolina", "North Dakota",
    "Ohio", "Oklahoma", "Oregon", "Pennsylvania", "Rhode Island",
    "South Carolina", "South Dakota", "Tennessee", "Texas", "Utah",
    "Vermont", "Virginia", "Washington", "West Virginia", "Wisconsin",
    "Wyoming"
};
static constexpr int DEFAULT_STATE_INDEX = 9; // Florida

static std::string
selected_state_name(GncPluginPageEstatePrivate* priv)
{
    if (priv && priv->combo_state)
    {
        gchar* s = gtk_combo_box_text_get_active_text(priv->combo_state);
        if (s)
        {
            std::string out(s);
            g_free(s);
            if (!out.empty()) return out;
        }
    }
    return "Florida";
}

static std::string
state_estate_guidance(const std::string& st)
{
    if (st == "Florida")
        return "Florida Notes: No state estate or inheritance tax. Strong Homestead protections, Tenants by the Entirety (TBE), Lady Bird (Enhanced Life Estate) deeds, and post-2011 non-springing Durable POA with separately initialed superpowers.";
    if (st == "Massachusetts" || st == "Oregon" || st == "Washington" || st == "New York" ||
        st == "Illinois" || st == "Connecticut" || st == "Minnesota" || st == "Vermont" ||
        st == "Rhode Island" || st == "Maine" || st == "Hawaii" || st == "District of Columbia")
        return st + " Notes: Has a STATE ESTATE TAX (with thresholds often much lower than the federal exemption). Review credit-shelter / bypass trust planning and gifting with a " + st + " estate attorney.";
    if (st == "Maryland")
        return "Maryland Notes: Only state with BOTH a state estate tax AND a state inheritance tax (exempts spouses/children/lineal descendants, taxes collateral heirs). Consider revocable trust & beneficiary review.";
    if (st == "Pennsylvania" || st == "New Jersey" || st == "Kentucky" || st == "Nebraska" || st == "Iowa")
        return st + " Notes: Has a STATE INHERITANCE TAX depending on heir relationship (spouses usually exempt; siblings/nieces/nephews/non-relatives often taxed). Verify beneficiary classes.";
    if (st == "Texas" || st == "California" || st == "Arizona" || st == "Nevada" ||
        st == "New Mexico" || st == "Idaho" || st == "Louisiana" || st == "Wisconsin")
        return st + " Notes: Community Property state (no state estate/inheritance tax, except check state-specific probate/TOD rules; California probate fees are high so Revocable Living Trust is strongly favored).";
    return st + " Notes: No state estate or inheritance tax. Verify state-specific POA statutory form, Transfer-on-Death (TOD) deed availability, and probate thresholds with a local attorney.";
}

static void
update_state_labels(GncPluginPageEstatePrivate* priv)
{
    std::string st = selected_state_name(priv);
    if (priv->lbl_header)
    {
        std::string mk = "<b>" + html_escape(st) + " Estate &amp; Continuity Tracker</b>";
        gtk_label_set_markup(priv->lbl_header, mk.c_str());
    }
    if (priv->lbl_state_note)
        gtk_label_set_text(priv->lbl_state_note, state_estate_guidance(st).c_str());
}

static void
write_estate_to_keyfile(GncPluginPageEstatePrivate* priv, GKeyFile* kf)
{
    if (!priv || !kf)
        return;

    auto set_entry = [&](const char* key, GtkEntry* e) {
        g_key_file_set_string(kf, ESTATE_GROUP_META, key, e ? gtk_entry_get_text(e) : "");
    };
    set_entry("prepared_for", priv->entry_prepared_for);
    set_entry("scenarios", priv->entry_scenarios);
    set_entry("heirs", priv->entry_heirs);
    g_key_file_set_string(kf, ESTATE_GROUP_META, "state", selected_state_name(priv).c_str());
    set_entry("jurisdiction", priv->entry_jurisdiction);
    set_entry("attorney", priv->entry_attorney);
    set_entry("safe_location", priv->entry_safe_location);
    set_entry("last_audit", priv->entry_last_audit);
    set_entry("next_audit", priv->entry_next_audit);

    g_key_file_remove_group(kf, ESTATE_GROUP_ITEMS, nullptr);
    g_key_file_set_integer(kf, ESTATE_GROUP_ITEMS, "count", static_cast<int>(priv->items.size()));
    for (size_t i = 0; i < priv->items.size(); ++i)
    {
        const auto& it = priv->items[i];
        std::string val = std::string(it.done ? "1" : "0") + "|" +
                          std::to_string(it.section) + "|" +
                          sanitize_field(it.title) + "|" +
                          sanitize_field(it.status) + "|" +
                          sanitize_field(it.due_trigger) + "|" +
                          sanitize_field(it.owner_agent) + "|" +
                          sanitize_field(it.completed_date) + "|" +
                          escape_multiline(it.details) + "|" +
                          sanitize_field(it.doc_path) + "|" +
                          sanitize_field(it.sp1_status) + "|" +
                          sanitize_field(it.sp1_date) + "|" +
                          sanitize_field(it.sp2_status) + "|" +
                          sanitize_field(it.sp2_date);
        std::string key = "item_" + std::to_string(i);
        g_key_file_set_string(kf, ESTATE_GROUP_ITEMS, key.c_str(), val.c_str());
    }

    g_key_file_remove_group(kf, ESTATE_GROUP_BEN, nullptr);
    g_key_file_set_integer(kf, ESTATE_GROUP_BEN, "count", static_cast<int>(priv->ben_rows.size()));
    for (size_t i = 0; i < priv->ben_rows.size(); ++i)
    {
        const auto& b = priv->ben_rows[i];
        std::string val = std::string(b.verified ? "1" : "0") + "|" +
                          sanitize_field(b.account_name) + "|" +
                          sanitize_field(b.approx_value) + "|" +
                          sanitize_field(b.primary_beneficiary) + "|" +
                          sanitize_field(b.contingent_structure) + "|" +
                          sanitize_field(b.recheck_trigger) + "|" +
                          sanitize_field(b.last_verified_date) + "|" +
                          escape_multiline(b.notes) + "|" +
                          sanitize_field(b.linked_gnucash_acct) + "|" +
                          sanitize_field(b.doc_path);
        std::string key = "ben_" + std::to_string(i);
        g_key_file_set_string(kf, ESTATE_GROUP_BEN, key.c_str(), val.c_str());
    }

    g_key_file_remove_group(kf, ESTATE_GROUP_ICE, nullptr);
    g_key_file_set_integer(kf, ESTATE_GROUP_ICE, "count", static_cast<int>(priv->ice_rows.size()));
    for (size_t i = 0; i < priv->ice_rows.size(); ++i)
    {
        const auto& r = priv->ice_rows[i];
        std::string val = std::string(r.verified ? "1" : "0") + "|" +
                          sanitize_field(r.category) + "|" +
                          sanitize_field(r.institution) + "|" +
                          sanitize_field(r.account_policy_id) + "|" +
                          sanitize_field(r.recovery_location) + "|" +
                          escape_multiline(r.notes);
        std::string key = "ice_" + std::to_string(i);
        g_key_file_set_string(kf, ESTATE_GROUP_ICE, key.c_str(), val.c_str());
    }

    g_key_file_remove_group(kf, ESTATE_GROUP_CONTACTS, nullptr);
    g_key_file_set_integer(kf, ESTATE_GROUP_CONTACTS, "count", static_cast<int>(priv->contacts.size()));
    for (size_t i = 0; i < priv->contacts.size(); ++i)
    {
        const auto& c = priv->contacts[i];
        std::string val = sanitize_field(c.role) + "|" +
                          sanitize_field(c.name) + "|" +
                          sanitize_field(c.firm_org) + "|" +
                          sanitize_field(c.phone) + "|" +
                          sanitize_field(c.email) + "|" +
                          escape_multiline(c.notes);
        std::string key = "contact_" + std::to_string(i);
        g_key_file_set_string(kf, ESTATE_GROUP_CONTACTS, key.c_str(), val.c_str());
    }

    if (priv->spin_ht_trad)
        g_key_file_set_double(kf, ESTATE_GROUP_HEIR_TAX, "trad_bal", gtk_spin_button_get_value(priv->spin_ht_trad));
    if (priv->spin_ht_roth)
        g_key_file_set_double(kf, ESTATE_GROUP_HEIR_TAX, "roth_bal", gtk_spin_button_get_value(priv->spin_ht_roth));
    if (priv->spin_ht_heirs)
        g_key_file_set_integer(kf, ESTATE_GROUP_HEIR_TAX, "heirs", gtk_spin_button_get_value_as_int(priv->spin_ht_heirs));
    if (priv->spin_ht_rate)
        g_key_file_set_double(kf, ESTATE_GROUP_HEIR_TAX, "tax_rate", gtk_spin_button_get_value(priv->spin_ht_rate));
    if (priv->spin_ht_growth)
        g_key_file_set_double(kf, ESTATE_GROUP_HEIR_TAX, "growth_rate", gtk_spin_button_get_value(priv->spin_ht_growth));
    if (priv->combo_ht_strategy)
        g_key_file_set_integer(kf, ESTATE_GROUP_HEIR_TAX, "strategy", gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ht_strategy)));
}

static void
save_estate_state(GncPluginPageEstatePrivate* priv, bool flush_book = true)
{
    if (!priv || !priv->root_widget || priv->updating_ui)
        return;

    std::string cfg_path = estate_config_file_path();
    if (!cfg_path.empty())
    {
        GKeyFile* kf = g_key_file_new();
        g_key_file_load_from_file(kf, cfg_path.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
        write_estate_to_keyfile(priv, kf);
        if (gchar* dir = g_path_get_dirname(cfg_path.c_str()))
        {
            g_mkdir_with_parents(dir, 0700);
            g_free(dir);
        }
        g_key_file_save_to_file(kf, cfg_path.c_str(), nullptr);
        g_chmod(cfg_path.c_str(), 0600);
        g_key_file_free(kf);
    }

    if (GKeyFile* state = gnc_state_get_current())
    {
        write_estate_to_keyfile(priv, state);
        if (flush_book && gnc_current_session_exist())
        {
            if (QofSession* sess = gnc_get_current_session())
                gnc_state_save(sess);
        }
    }
}

static bool
load_estate_from_keyfile(GncPluginPageEstatePrivate* priv, GKeyFile* kf)
{
    if (!priv || !kf || !g_key_file_has_group(kf, ESTATE_GROUP_ITEMS))
        return false;

    auto get_str = [&](const char* group, const char* key, const char* def) -> std::string {
        if (gchar* v = g_key_file_get_string(kf, group, key, nullptr))
        {
            std::string s(v);
            g_free(v);
            return s;
        }
        return def ? def : "";
    };

    priv->updating_ui = true;
    if (priv->entry_prepared_for)
        gtk_entry_set_text(priv->entry_prepared_for, get_str(ESTATE_GROUP_META, "prepared_for", "Greg & Tammy").c_str());
    if (priv->entry_scenarios)
        gtk_entry_set_text(priv->entry_scenarios, get_str(ESTATE_GROUP_META, "scenarios", "Scenario A (Jan 2027) & Scenario B (Jan 2028)").c_str());
    if (priv->entry_heirs)
        gtk_entry_set_text(priv->entry_heirs, get_str(ESTATE_GROUP_META, "heirs", "Hannah (turns 26 Apr 2027) & Peyton (turns 21 Jan 2027) — 50% / 50% equal split, per stirpes").c_str());
    if (priv->combo_state)
    {
        std::string st = get_str(ESTATE_GROUP_META, "state", "Florida");
        int idx = DEFAULT_STATE_INDEX;
        for (int i = 0; i < static_cast<int>(G_N_ELEMENTS(US_STATES)); ++i)
        {
            if (st == US_STATES[i]) { idx = i; break; }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state), idx);
    }
    if (priv->entry_jurisdiction)
        gtk_entry_set_text(priv->entry_jurisdiction, get_str(ESTATE_GROUP_META, "jurisdiction", "Hillsborough County").c_str());
    if (priv->entry_attorney)
        gtk_entry_set_text(priv->entry_attorney, get_str(ESTATE_GROUP_META, "attorney", "").c_str());
    if (priv->entry_safe_location)
        gtk_entry_set_text(priv->entry_safe_location, get_str(ESTATE_GROUP_META, "safe_location", "Home Fireproof Safe").c_str());
    if (priv->entry_last_audit)
        gtk_entry_set_text(priv->entry_last_audit, get_str(ESTATE_GROUP_META, "last_audit", "").c_str());
    if (priv->entry_next_audit)
        gtk_entry_set_text(priv->entry_next_audit, get_str(ESTATE_GROUP_META, "next_audit", "Nov 15 (Annual Tax & MAGI Audit)").c_str());

    priv->items.clear();
    int n_items = g_key_file_get_integer(kf, ESTATE_GROUP_ITEMS, "count", nullptr);
    for (int i = 0; i < n_items; ++i)
    {
        std::string raw = get_str(ESTATE_GROUP_ITEMS, ("item_" + std::to_string(i)).c_str(), "");
        if (raw.empty()) continue;
        auto p = split_pipes(raw, 13);
        EstateChecklistItem it;
        it.done = (p[0] == "1");
        it.section = std::clamp(std::atoi(p[1].c_str()), 0, 2);
        it.title = p[2];
        it.status = p[3].empty() ? (it.done ? "Completed" : "Not Started") : p[3];
        it.due_trigger = p[4];
        it.owner_agent = p[5];
        it.completed_date = p[6];
        it.details = unescape_multiline(p[7]);
        it.doc_path = p[8];
        it.sp1_status = p[9].empty() ? it.status : p[9];
        it.sp1_date = p[10];
        it.sp2_status = p[11].empty() ? it.status : p[11];
        it.sp2_date = p[12];
        priv->items.push_back(std::move(it));
    }

    priv->ben_rows.clear();
    int n_ben = g_key_file_get_integer(kf, ESTATE_GROUP_BEN, "count", nullptr);
    for (int i = 0; i < n_ben; ++i)
    {
        std::string raw = get_str(ESTATE_GROUP_BEN, ("ben_" + std::to_string(i)).c_str(), "");
        if (raw.empty()) continue;
        auto p = split_pipes(raw, 10);
        BeneficiaryAuditRow b;
        b.verified = (p[0] == "1");
        b.account_name = p[1];
        b.approx_value = p[2];
        b.primary_beneficiary = p[3];
        b.contingent_structure = p[4];
        b.recheck_trigger = p[5];
        b.last_verified_date = p[6];
        b.notes = unescape_multiline(p[7]);
        b.linked_gnucash_acct = p[8];
        b.doc_path = p[9];
        priv->ben_rows.push_back(std::move(b));
    }

    priv->ice_rows.clear();
    int n_ice = g_key_file_get_integer(kf, ESTATE_GROUP_ICE, "count", nullptr);
    for (int i = 0; i < n_ice; ++i)
    {
        std::string raw = get_str(ESTATE_GROUP_ICE, ("ice_" + std::to_string(i)).c_str(), "");
        if (raw.empty()) continue;
        auto p = split_pipes(raw, 6);
        IceDirectoryRow r;
        r.verified = (p[0] == "1");
        r.category = p[1];
        r.institution = p[2];
        r.account_policy_id = p[3];
        r.recovery_location = p[4];
        r.notes = unescape_multiline(p[5]);
        priv->ice_rows.push_back(std::move(r));
    }

    priv->contacts.clear();
    if (g_key_file_has_group(kf, ESTATE_GROUP_CONTACTS))
    {
        int n_ct = g_key_file_get_integer(kf, ESTATE_GROUP_CONTACTS, "count", nullptr);
        for (int i = 0; i < n_ct; ++i)
        {
            std::string raw = get_str(ESTATE_GROUP_CONTACTS, ("contact_" + std::to_string(i)).c_str(), "");
            if (raw.empty()) continue;
            auto p = split_pipes(raw, 6);
            FiduciaryContact c;
            c.role = p[0];
            c.name = p[1];
            c.firm_org = p[2];
            c.phone = p[3];
            c.email = p[4];
            c.notes = unescape_multiline(p[5]);
            priv->contacts.push_back(std::move(c));
        }
    }
    if (priv->contacts.empty())
        populate_default_contacts_only(priv);

    if (g_key_file_has_group(kf, ESTATE_GROUP_HEIR_TAX))
    {
        auto get_dbl = [&](const char* k, double def) {
            GError* err = nullptr;
            double v = g_key_file_get_double(kf, ESTATE_GROUP_HEIR_TAX, k, &err);
            if (err) { g_error_free(err); return def; }
            return v;
        };
        if (priv->spin_ht_trad) gtk_spin_button_set_value(priv->spin_ht_trad, get_dbl("trad_bal", 1729711.0));
        if (priv->spin_ht_roth) gtk_spin_button_set_value(priv->spin_ht_roth, get_dbl("roth_bal", 54100.0));
        if (priv->spin_ht_heirs) gtk_spin_button_set_value(priv->spin_ht_heirs, get_dbl("heirs", 2.0));
        if (priv->spin_ht_rate) gtk_spin_button_set_value(priv->spin_ht_rate, get_dbl("tax_rate", 24.0));
        if (priv->spin_ht_growth) gtk_spin_button_set_value(priv->spin_ht_growth, get_dbl("growth_rate", 6.0));
        if (priv->combo_ht_strategy)
        {
            int st = g_key_file_get_integer(kf, ESTATE_GROUP_HEIR_TAX, "strategy", nullptr);
            gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ht_strategy), std::clamp(st, 0, 1));
        }
    }

    priv->updating_ui = false;
    return !priv->items.empty();
}

static void
update_progress_bars_and_triggers(GncPluginPageEstatePrivate* priv)
{
    if (!priv)
        return;

    int sec_tot[3] = {0, 0, 0};
    int sec_done[3] = {0, 0, 0};
    for (const auto& it : priv->items)
    {
        int s = std::clamp(it.section, 0, 2);
        sec_tot[s]++;
        if (it.done) sec_done[s]++;
    }

    int ben_tot = static_cast<int>(priv->ben_rows.size());
    int ben_done = 0;
    for (const auto& b : priv->ben_rows)
        if (b.verified) ben_done++;

    int ice_tot = static_cast<int>(priv->ice_rows.size());
    int ice_done = 0;
    for (const auto& r : priv->ice_rows)
        if (r.verified) ice_done++;

    auto set_pbar = [](GtkProgressBar* pb, int done, int tot, const char* label) {
        if (!pb) return;
        double frac = (tot > 0) ? static_cast<double>(done) / tot : 0.0;
        gtk_progress_bar_set_fraction(pb, frac);
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s: %d / %d (%.0f%%)", label, done, tot, frac * 100.0);
        gtk_progress_bar_set_text(pb, buf);
        gtk_progress_bar_set_show_text(pb, TRUE);
    };

    int all_done = sec_done[0] + sec_done[1] + sec_done[2] + ben_done + ice_done;
    int all_tot  = sec_tot[0] + sec_tot[1] + sec_tot[2] + ben_tot + ice_tot;
    set_pbar(priv->pbar_overall, all_done, all_tot, "Overall Estate & Continuity Readiness");
    set_pbar(priv->pbar_legal, sec_done[SEC_LEGAL], sec_tot[SEC_LEGAL], "1. Core Florida Legal Documents");
    set_pbar(priv->pbar_beneficiary, ben_done, ben_tot, "2. Account Titling & Beneficiary Audit");
    set_pbar(priv->pbar_tax, sec_done[SEC_TAX], sec_tot[SEC_TAX], "3. Tax & Inherited-Account Continuity");
    set_pbar(priv->pbar_ice, sec_done[SEC_ICE] + ice_done, sec_tot[SEC_ICE] + ice_tot, "4. ICE & Digital Continuity");

    for (int s = 0; s < 3; ++s)
    {
        static const char* slabels[3] = {
            "Legal Docs Progress", "Tax Continuity Progress", "ICE Checklist Progress"
        };
        set_pbar(priv->sec_tabs[s].pbar, sec_done[s], sec_tot[s], slabels[s]);
    }

    if (priv->tv_triggers)
    {
        std::ostringstream os;
        os << "OPEN ACTION ITEMS & MANDATORY RE-CHECK TRIGGERS\n";
        os << "==================================================\n\n";
        for (const auto& it : priv->items)
        {
            if (!it.done)
            {
                const char* sec_name = (it.section == SEC_LEGAL) ? "Legal"
                                     : (it.section == SEC_TAX)   ? "Tax Continuity"
                                                                 : "ICE & Digital";
                os << "[ ] [" << sec_name << "] " << it.title;
                if (!it.due_trigger.empty()) os << "  —  Due/Trigger: " << it.due_trigger;
                os << " (" << it.status << ")\n";
            }
        }
        os << "\nBENEFICIARY & TITLING AUDITS PENDING VERIFICATION\n";
        os << "--------------------------------------------------\n";
        for (const auto& b : priv->ben_rows)
        {
            if (!b.verified)
            {
                os << "[ ] " << b.account_name;
                if (!b.recheck_trigger.empty()) os << "  —  " << b.recheck_trigger;
                os << "\n";
            }
        }
        if (all_done == all_tot && all_tot > 0)
            os << "\nAll tracked estate items and beneficiary audits are currently verified!\n";

        gtk_text_buffer_set_text(gtk_text_view_get_buffer(priv->tv_triggers), os.str().c_str(), -1);
    }
}

static void
refresh_section_store(GncPluginPageEstatePrivate* priv, int sec)
{
    auto& tab = priv->sec_tabs[sec];
    if (!tab.store) return;
    gtk_list_store_clear(tab.store);
    for (size_t i = 0; i < priv->items.size(); ++i)
    {
        const auto& it = priv->items[i];
        if (it.section != sec) continue;
        std::string sp1 = it.sp1_status + (it.sp1_date.empty() ? "" : " (" + it.sp1_date + ")");
        std::string sp2 = it.sp2_status + (it.sp2_date.empty() ? "" : " (" + it.sp2_date + ")");
        GtkTreeIter iter;
        gtk_list_store_append(tab.store, &iter);
        gtk_list_store_set(tab.store, &iter,
                           0, static_cast<gint>(i),
                           1, it.done ? TRUE : FALSE,
                           2, it.title.c_str(),
                           3, it.status.c_str(),
                           4, it.due_trigger.c_str(),
                           5, it.owner_agent.c_str(),
                           6, it.completed_date.c_str(),
                           7, sp1.c_str(),
                           8, sp2.c_str(),
                           -1);
    }
}

static void
refresh_ben_store(GncPluginPageEstatePrivate* priv)
{
    if (!priv->ben_store) return;
    gtk_list_store_clear(priv->ben_store);
    for (size_t i = 0; i < priv->ben_rows.size(); ++i)
    {
        const auto& b = priv->ben_rows[i];
        bool stale = !b.verified || is_date_stale(b.last_verified_date);
        const char* color = !b.verified ? "#a34815" : (stale ? "#b45309" : "#1b7a3d");
        std::string status_date = b.last_verified_date;
        if (b.verified && stale)
            status_date += status_date.empty() ? " (Stale >12m)" : " (Stale)";

        GtkTreeIter iter;
        gtk_list_store_append(priv->ben_store, &iter);
        gtk_list_store_set(priv->ben_store, &iter,
                           0, static_cast<gint>(i),
                           1, b.verified ? TRUE : FALSE,
                           2, b.account_name.c_str(),
                           3, b.approx_value.c_str(),
                           4, b.primary_beneficiary.c_str(),
                           5, b.contingent_structure.c_str(),
                           6, b.recheck_trigger.c_str(),
                           7, status_date.c_str(),
                           8, color,
                           -1);
    }

    if (priv->lbl_ben_unmapped)
    {
        auto accts = collect_gnucash_asset_accounts();
        std::vector<std::string> unlinked;
        for (const auto& ac : accts)
        {
            if (!ac.is_leaf || ac.balance <= 0.01) continue;
            bool found = false;
            for (const auto& b : priv->ben_rows)
            {
                if (b.linked_gnucash_acct.empty()) continue;
                if (b.linked_gnucash_acct == ac.full_name ||
                    ac.full_name.rfind(b.linked_gnucash_acct + ":", 0) == 0)
                {
                    found = true;
                    break;
                }
            }
            if (!found) unlinked.push_back(ac.full_name + " (" + fmt_money_str(ac.balance) + ")");
        }
        if (unlinked.empty())
            gtk_label_set_text(priv->lbl_ben_unmapped, _("All positive-balance GnuCash asset/investment accounts are linked."));
        else
        {
            std::string msg = "Unlinked GnuCash Accounts (" + std::to_string(unlinked.size()) + "): ";
            for (size_t i = 0; i < std::min<size_t>(unlinked.size(), 4); ++i)
            {
                if (i > 0) msg += "; ";
                msg += unlinked[i];
            }
            if (unlinked.size() > 4) msg += " ...";
            gtk_label_set_text(priv->lbl_ben_unmapped, msg.c_str());
        }
    }
}

static void
refresh_contact_store(GncPluginPageEstatePrivate* priv)
{
    if (!priv->contact_store) return;
    gtk_list_store_clear(priv->contact_store);
    for (size_t i = 0; i < priv->contacts.size(); ++i)
    {
        const auto& c = priv->contacts[i];
        GtkTreeIter iter;
        gtk_list_store_append(priv->contact_store, &iter);
        gtk_list_store_set(priv->contact_store, &iter,
                           0, static_cast<gint>(i),
                           1, c.role.c_str(),
                           2, c.name.c_str(),
                           3, c.firm_org.c_str(),
                           4, c.phone.c_str(),
                           5, c.email.c_str(),
                           6, c.notes.c_str(),
                           -1);
    }
}

static void
recompute_heir_tax_estimator(GncPluginPageEstatePrivate* priv)
{
    if (!priv || !priv->tv_ht_report || !priv->spin_ht_trad)
        return;

    double trad0 = gtk_spin_button_get_value(priv->spin_ht_trad);
    double roth0 = gtk_spin_button_get_value(priv->spin_ht_roth);
    int heirs = std::max(1, gtk_spin_button_get_value_as_int(priv->spin_ht_heirs));
    double rate = gtk_spin_button_get_value(priv->spin_ht_rate) / 100.0;
    double growth = gtk_spin_button_get_value(priv->spin_ht_growth) / 100.0;
    int strat = priv->combo_ht_strategy ? gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ht_strategy)) : 0;

    std::ostringstream os;
    os << "SECURE ACT 10-YEAR INHERITED ACCOUNT TAX PROJECTION\n";
    os << "==================================================\n";
    os << "Starting Inherited Tax-Deferred: " << fmt_money_str(trad0)
       << "  |  Starting Inherited Roth: " << fmt_money_str(roth0) << "\n";
    os << "Heirs: " << heirs << "  |  Est. Heir Marginal Rate: " << (rate * 100.0)
       << "%  |  Growth: " << (growth * 100.0) << "%/yr\n";
    os << "Strategy: " << (strat == 0 ? "Even 10-Year Spread" : "Year-10 Lump Sum (Deferral)") << "\n\n";

    double bal_trad = trad0;
    double bal_roth = roth0;
    double tot_tax_trad = 0.0;
    double tot_net_trad = 0.0;
    double tot_net_roth = 0.0;

    char line[160];
    std::snprintf(line, sizeof(line), "%-4s  %-14s  %-14s  %-14s  %-14s\n",
                  "Yr", "Trad Dist", "Heir Tax", "Net Trad", "Net Roth");
    os << line;
    os << "----  --------------  --------------  --------------  --------------\n";

    for (int yr = 1; yr <= 10; ++yr)
    {
        bal_trad *= (1.0 + growth);
        bal_roth *= (1.0 + growth);

        double dist_trad = 0.0;
        double dist_roth = 0.0;
        if (strat == 0)
        {
            int rem = 11 - yr;
            dist_trad = bal_trad / rem;
            dist_roth = bal_roth / rem;
        }
        else if (yr == 10)
        {
            dist_trad = bal_trad;
            dist_roth = bal_roth;
        }

        bal_trad -= dist_trad;
        bal_roth -= dist_roth;

        double tax = dist_trad * rate;
        double net_t = dist_trad - tax;
        tot_tax_trad += tax;
        tot_net_trad += net_t;
        tot_net_roth += dist_roth;

        std::snprintf(line, sizeof(line), "%2d    %-14s  %-14s  %-14s  %-14s\n",
                      yr,
                      fmt_money_str(dist_trad).c_str(),
                      fmt_money_str(tax).c_str(),
                      fmt_money_str(net_t).c_str(),
                      fmt_money_str(dist_roth).c_str());
        os << line;
    }

    os << "\nSUMMARY OVER 10-YEAR WINDOW\n";
    os << "--------------------------------------------------\n";
    os << "Total Tax Paid by Heirs on Inherited Traditional: " << fmt_money_str(tot_tax_trad) << "\n";
    os << "Total Net After-Tax Inheritance (Trad + Roth):    " << fmt_money_str(tot_net_trad + tot_net_roth) << "\n";
    os << "Per-Heir Net After-Tax (" << heirs << " heirs):                 "
       << fmt_money_str((tot_net_trad + tot_net_roth) / heirs) << "\n\n";

    double all_roth_net = (trad0 + roth0) * std::pow(1.0 + growth, strat == 0 ? 5.5 : 10.0);
    (void)all_roth_net;
    os << "Note: Every $100,000 converted to Roth during your 12%/22% retirement window\n"
       << "saves heirs " << fmt_money_str(100000.0 * rate) << " in tax at " << (rate * 100.0)
       << "% (before compounding over 10 years).\n";

    gtk_text_buffer_set_text(gtk_text_view_get_buffer(priv->tv_ht_report), os.str().c_str(), -1);
}

static void
refresh_ice_store(GncPluginPageEstatePrivate* priv)
{
    if (!priv->ice_store) return;
    gtk_list_store_clear(priv->ice_store);
    for (size_t i = 0; i < priv->ice_rows.size(); ++i)
    {
        const auto& r = priv->ice_rows[i];
        GtkTreeIter iter;
        gtk_list_store_append(priv->ice_store, &iter);
        gtk_list_store_set(priv->ice_store, &iter,
                           0, static_cast<gint>(i),
                           1, r.verified ? TRUE : FALSE,
                           2, r.category.c_str(),
                           3, r.institution.c_str(),
                           4, r.account_policy_id.c_str(),
                           5, r.recovery_location.c_str(),
                           6, r.notes.c_str(),
                           -1);
    }
}

static void
refresh_all_views(GncPluginPageEstatePrivate* priv)
{
    for (int s = 0; s < 3; ++s)
        refresh_section_store(priv, s);
    refresh_ben_store(priv);
    refresh_ice_store(priv);
    refresh_contact_store(priv);
    update_progress_bars_and_triggers(priv);
    recompute_heir_tax_estimator(priv);
}

static void
load_estate_state(GncPluginPageEstatePrivate* priv)
{
    std::string cfg_path = estate_config_file_path();
    bool loaded = false;
    if (!cfg_path.empty())
    {
        GKeyFile* kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, cfg_path.c_str(), G_KEY_FILE_NONE, nullptr))
            loaded = load_estate_from_keyfile(priv, kf);
        g_key_file_free(kf);
    }
    if (!loaded)
    {
        if (GKeyFile* state = gnc_state_get_current())
            loaded = load_estate_from_keyfile(priv, state);
    }
    if (!loaded)
    {
        priv->updating_ui = true;
        populate_default_florida_checklist(priv);
        if (priv->entry_prepared_for) gtk_entry_set_text(priv->entry_prepared_for, "Greg & Tammy");
        if (priv->entry_scenarios) gtk_entry_set_text(priv->entry_scenarios, "Scenario A (Jan 2027) & Scenario B (Jan 2028)");
        if (priv->entry_heirs) gtk_entry_set_text(priv->entry_heirs, "Hannah (turns 26 Apr 2027) & Peyton (turns 21 Jan 2027) — 50% / 50% equal split, per stirpes");
        if (priv->combo_state) gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state), DEFAULT_STATE_INDEX);
        if (priv->entry_jurisdiction) gtk_entry_set_text(priv->entry_jurisdiction, "Hillsborough County");
        if (priv->entry_safe_location) gtk_entry_set_text(priv->entry_safe_location, "Home Fireproof Safe");
        if (priv->entry_next_audit) gtk_entry_set_text(priv->entry_next_audit, "Nov 15 (Annual Tax & MAGI Audit)");
        priv->updating_ui = false;
        save_estate_state(priv, false);
    }
    update_state_labels(priv);
    refresh_all_views(priv);
}

static void
on_meta_changed(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    save_estate_state(priv, false);
}

static void
on_state_changed(GtkComboBox*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    update_state_labels(priv);
    save_estate_state(priv, true);
}

static void
on_sec_toggle(GtkCellRendererToggle*, gchar* path_str, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    auto* priv = pair->first;
    int sec = pair->second;
    auto& tab = priv->sec_tabs[sec];
    GtkTreePath* path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(tab.store), &iter, path))
    {
        gint idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(tab.store), &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->items.size()))
        {
            auto& it = priv->items[idx];
            it.done = !it.done;
            it.status = it.done ? "Completed" : "Not Started";
            if (it.done && it.completed_date.empty())
                it.completed_date = today_iso_date();
            else if (!it.done)
                it.completed_date.clear();
            refresh_section_store(priv, sec);
            update_progress_bars_and_triggers(priv);
            save_estate_state(priv, true);
        }
    }
    gtk_tree_path_free(path);
}

static void
on_sec_selection_changed(GtkTreeSelection* sel, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    auto* priv = pair->first;
    int sec = pair->second;
    auto& tab = priv->sec_tabs[sec];
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
    {
        gint idx = -1;
        gtk_tree_model_get(model, &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->items.size()))
        {
            tab.selected_idx = idx;
            const auto& it = priv->items[idx];
            gtk_entry_set_text(tab.entry_title, it.title.c_str());
            int st = 0;
            if (it.status == "In Progress") st = 1;
            else if (it.status == "Attorney Review") st = 2;
            else if (it.status == "Completed") st = 3;
            gtk_combo_box_set_active(GTK_COMBO_BOX(tab.combo_status), st);
            gtk_entry_set_text(tab.entry_trigger, it.due_trigger.c_str());
            gtk_entry_set_text(tab.entry_owner, it.owner_agent.c_str());
            gtk_entry_set_text(tab.entry_date, it.completed_date.c_str());
            if (tab.entry_doc) gtk_entry_set_text(tab.entry_doc, it.doc_path.c_str());
            auto status_idx = [](const std::string& s) {
                if (s == "In Progress") return 1;
                if (s == "Attorney Review") return 2;
                if (s == "Completed") return 3;
                return 0;
            };
            if (tab.combo_sp1_status) gtk_combo_box_set_active(GTK_COMBO_BOX(tab.combo_sp1_status), status_idx(it.sp1_status));
            if (tab.entry_sp1_date) gtk_entry_set_text(tab.entry_sp1_date, it.sp1_date.c_str());
            if (tab.combo_sp2_status) gtk_combo_box_set_active(GTK_COMBO_BOX(tab.combo_sp2_status), status_idx(it.sp2_status));
            if (tab.entry_sp2_date) gtk_entry_set_text(tab.entry_sp2_date, it.sp2_date.c_str());
            gtk_text_buffer_set_text(gtk_text_view_get_buffer(tab.tv_details), it.details.c_str(), -1);
        }
    }
}

static std::string
read_text_view(GtkTextView* tv)
{
    if (!tv) return "";
    GtkTextBuffer* buf = gtk_text_view_get_buffer(tv);
    GtkTextIter i0, i1;
    gtk_text_buffer_get_bounds(buf, &i0, &i1);
    gchar* s = gtk_text_buffer_get_text(buf, &i0, &i1, FALSE);
    std::string out = s ? s : "";
    g_free(s);
    return out;
}

static void
on_sec_update_clicked(GtkButton*, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    auto* priv = pair->first;
    int sec = pair->second;
    auto& tab = priv->sec_tabs[sec];
    if (tab.selected_idx < 0 || tab.selected_idx >= static_cast<gint>(priv->items.size()))
        return;

    auto& it = priv->items[tab.selected_idx];
    it.title = gtk_entry_get_text(tab.entry_title);
    gchar* st_txt = gtk_combo_box_text_get_active_text(tab.combo_status);
    it.status = st_txt ? st_txt : "Not Started";
    g_free(st_txt);
    it.done = (it.status == "Completed");
    it.due_trigger = gtk_entry_get_text(tab.entry_trigger);
    it.owner_agent = gtk_entry_get_text(tab.entry_owner);
    it.completed_date = gtk_entry_get_text(tab.entry_date);
    if (it.done && it.completed_date.empty())
        it.completed_date = today_iso_date();
    if (tab.entry_doc) it.doc_path = gtk_entry_get_text(tab.entry_doc);
    if (tab.combo_sp1_status)
    {
        gchar* s = gtk_combo_box_text_get_active_text(tab.combo_sp1_status);
        it.sp1_status = s ? s : "Not Started";
        g_free(s);
    }
    if (tab.entry_sp1_date) it.sp1_date = gtk_entry_get_text(tab.entry_sp1_date);
    if (tab.combo_sp2_status)
    {
        gchar* s = gtk_combo_box_text_get_active_text(tab.combo_sp2_status);
        it.sp2_status = s ? s : "Not Started";
        g_free(s);
    }
    if (tab.entry_sp2_date) it.sp2_date = gtk_entry_get_text(tab.entry_sp2_date);
    it.details = read_text_view(tab.tv_details);

    refresh_section_store(priv, sec);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_sec_add_clicked(GtkButton*, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    auto* priv = pair->first;
    int sec = pair->second;
    auto& tab = priv->sec_tabs[sec];

    std::string title = gtk_entry_get_text(tab.entry_title);
    if (title.empty()) title = "New Estate Checklist Item";

    EstateChecklistItem it;
    it.section = sec;
    it.title = title;
    gchar* st_txt = gtk_combo_box_text_get_active_text(tab.combo_status);
    it.status = st_txt ? st_txt : "Not Started";
    g_free(st_txt);
    it.done = (it.status == "Completed");
    it.due_trigger = gtk_entry_get_text(tab.entry_trigger);
    it.owner_agent = gtk_entry_get_text(tab.entry_owner);
    it.completed_date = gtk_entry_get_text(tab.entry_date);
    if (tab.entry_doc) it.doc_path = gtk_entry_get_text(tab.entry_doc);
    if (tab.combo_sp1_status)
    {
        gchar* s = gtk_combo_box_text_get_active_text(tab.combo_sp1_status);
        it.sp1_status = s ? s : "Not Started";
        g_free(s);
    }
    if (tab.entry_sp1_date) it.sp1_date = gtk_entry_get_text(tab.entry_sp1_date);
    if (tab.combo_sp2_status)
    {
        gchar* s = gtk_combo_box_text_get_active_text(tab.combo_sp2_status);
        it.sp2_status = s ? s : "Not Started";
        g_free(s);
    }
    if (tab.entry_sp2_date) it.sp2_date = gtk_entry_get_text(tab.entry_sp2_date);
    it.details = read_text_view(tab.tv_details);

    priv->items.push_back(std::move(it));
    tab.selected_idx = static_cast<int>(priv->items.size()) - 1;
    refresh_section_store(priv, sec);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_sec_delete_clicked(GtkButton*, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    auto* priv = pair->first;
    int sec = pair->second;
    auto& tab = priv->sec_tabs[sec];
    if (tab.selected_idx < 0 || tab.selected_idx >= static_cast<gint>(priv->items.size()))
        return;
    priv->items.erase(priv->items.begin() + tab.selected_idx);
    tab.selected_idx = -1;
    refresh_section_store(priv, sec);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_sec_browse_doc_clicked(GtkButton*, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    browse_and_set_doc_path(pair->first->sec_tabs[pair->second].entry_doc);
}

static void
on_sec_open_doc_clicked(GtkButton*, gpointer user_data)
{
    auto* pair = static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(user_data);
    open_doc_path(pair->first->sec_tabs[pair->second].entry_doc, pair->first->lbl_status);
}

static void
on_ben_toggle(GtkCellRendererToggle*, gchar* path_str, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkTreePath* path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->ben_store), &iter, path))
    {
        gint idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(priv->ben_store), &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->ben_rows.size()))
        {
            auto& b = priv->ben_rows[idx];
            b.verified = !b.verified;
            b.last_verified_date = b.verified ? today_iso_date() : "";
            refresh_ben_store(priv);
            update_progress_bars_and_triggers(priv);
            save_estate_state(priv, true);
        }
    }
    gtk_tree_path_free(path);
}

static std::string
get_selected_ben_gnc_acct(GncPluginPageEstatePrivate* priv)
{
    if (!priv->combo_ben_gnc_acct) return "";
    gchar* t = gtk_combo_box_text_get_active_text(priv->combo_ben_gnc_acct);
    std::string s = t ? t : "";
    g_free(t);
    if (s == "(None / Manual)") return "";
    return s;
}

static void
set_selected_ben_gnc_acct(GncPluginPageEstatePrivate* priv, const std::string& acct)
{
    if (!priv->combo_ben_gnc_acct) return;
    if (acct.empty())
    {
        gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ben_gnc_acct), 0);
        return;
    }
    GtkTreeModel* m = gtk_combo_box_get_model(GTK_COMBO_BOX(priv->combo_ben_gnc_acct));
    GtkTreeIter iter;
    int idx = 0;
    bool valid = gtk_tree_model_get_iter_first(m, &iter);
    while (valid)
    {
        gchar* val = nullptr;
        gtk_tree_model_get(m, &iter, 0, &val, -1);
        if (val && acct == val)
        {
            g_free(val);
            gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ben_gnc_acct), idx);
            return;
        }
        g_free(val);
        valid = gtk_tree_model_iter_next(m, &iter);
        ++idx;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ben_gnc_acct), 0);
}

static void
on_ben_selection_changed(GtkTreeSelection* sel, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
    {
        gint idx = -1;
        gtk_tree_model_get(model, &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->ben_rows.size()))
        {
            priv->selected_ben_idx = idx;
            const auto& b = priv->ben_rows[idx];
            gtk_entry_set_text(priv->entry_ben_acct, b.account_name.c_str());
            gtk_entry_set_text(priv->entry_ben_val, b.approx_value.c_str());
            gtk_entry_set_text(priv->entry_ben_primary, b.primary_beneficiary.c_str());
            gtk_entry_set_text(priv->entry_ben_contingent, b.contingent_structure.c_str());
            gtk_entry_set_text(priv->entry_ben_trigger, b.recheck_trigger.c_str());
            gtk_entry_set_text(priv->entry_ben_date, b.last_verified_date.c_str());
            gtk_entry_set_text(priv->entry_ben_notes, b.notes.c_str());
            priv->updating_ui = true;
            set_selected_ben_gnc_acct(priv, b.linked_gnucash_acct);
            priv->updating_ui = false;
            if (priv->entry_ben_doc) gtk_entry_set_text(priv->entry_ben_doc, b.doc_path.c_str());
        }
    }
}

static void
on_ben_gnc_acct_changed(GtkComboBox*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->updating_ui)
        return;
    std::string acct = get_selected_ben_gnc_acct(priv);
    if (acct.empty())
    {
        if (priv->selected_ben_idx >= 0 && priv->selected_ben_idx < static_cast<gint>(priv->ben_rows.size()))
        {
            priv->ben_rows[priv->selected_ben_idx].linked_gnucash_acct.clear();
            refresh_ben_store(priv);
            save_estate_state(priv, true);
        }
        return;
    }

    for (const auto& ac : collect_gnucash_asset_accounts())
    {
        if (ac.full_name == acct)
        {
            std::string val_str = fmt_money_str(ac.balance);
            if (priv->entry_ben_val)
                gtk_entry_set_text(priv->entry_ben_val, val_str.c_str());
            if (priv->selected_ben_idx >= 0 && priv->selected_ben_idx < static_cast<gint>(priv->ben_rows.size()))
            {
                auto& b = priv->ben_rows[priv->selected_ben_idx];
                b.linked_gnucash_acct = acct;
                b.approx_value = val_str;
                refresh_ben_store(priv);
                save_estate_state(priv, true);
            }
            if (priv->lbl_status)
                gtk_label_set_text(priv->lbl_status, ("Linked " + acct + " (" + val_str + ")").c_str());
            break;
        }
    }
}

static void
on_ben_update_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_ben_idx < 0 || priv->selected_ben_idx >= static_cast<gint>(priv->ben_rows.size()))
        return;
    auto& b = priv->ben_rows[priv->selected_ben_idx];
    b.account_name = gtk_entry_get_text(priv->entry_ben_acct);
    b.approx_value = gtk_entry_get_text(priv->entry_ben_val);
    b.primary_beneficiary = gtk_entry_get_text(priv->entry_ben_primary);
    b.contingent_structure = gtk_entry_get_text(priv->entry_ben_contingent);
    b.recheck_trigger = gtk_entry_get_text(priv->entry_ben_trigger);
    b.last_verified_date = gtk_entry_get_text(priv->entry_ben_date);
    b.notes = gtk_entry_get_text(priv->entry_ben_notes);
    b.linked_gnucash_acct = get_selected_ben_gnc_acct(priv);
    if (priv->entry_ben_doc) b.doc_path = gtk_entry_get_text(priv->entry_ben_doc);
    refresh_ben_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_ben_add_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    std::string acct = gtk_entry_get_text(priv->entry_ben_acct);
    if (acct.empty()) acct = "New Account / Asset";
    BeneficiaryAuditRow b;
    b.account_name = acct;
    b.approx_value = gtk_entry_get_text(priv->entry_ben_val);
    b.primary_beneficiary = gtk_entry_get_text(priv->entry_ben_primary);
    b.contingent_structure = gtk_entry_get_text(priv->entry_ben_contingent);
    b.recheck_trigger = gtk_entry_get_text(priv->entry_ben_trigger);
    b.last_verified_date = gtk_entry_get_text(priv->entry_ben_date);
    b.notes = gtk_entry_get_text(priv->entry_ben_notes);
    b.linked_gnucash_acct = get_selected_ben_gnc_acct(priv);
    if (priv->entry_ben_doc) b.doc_path = gtk_entry_get_text(priv->entry_ben_doc);
    priv->ben_rows.push_back(std::move(b));
    priv->selected_ben_idx = static_cast<int>(priv->ben_rows.size()) - 1;
    refresh_ben_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_ben_browse_doc_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    browse_and_set_doc_path(priv->entry_ben_doc);
}

static void
on_ben_open_doc_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    open_doc_path(priv->entry_ben_doc, priv->lbl_status);
}

static void
on_ben_delete_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_ben_idx < 0 || priv->selected_ben_idx >= static_cast<gint>(priv->ben_rows.size()))
        return;
    priv->ben_rows.erase(priv->ben_rows.begin() + priv->selected_ben_idx);
    priv->selected_ben_idx = -1;
    refresh_ben_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_ben_sync_book_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    auto accts = collect_gnucash_asset_accounts();
    double total_assets = 0.0;
    int updated = 0;
    for (const auto& ac : accts)
        if (ac.is_leaf) total_assets += ac.balance;

    for (auto& b : priv->ben_rows)
    {
        if (b.linked_gnucash_acct.empty()) continue;
        for (const auto& ac : accts)
        {
            if (ac.full_name == b.linked_gnucash_acct)
            {
                b.approx_value = fmt_money_str(ac.balance);
                ++updated;
                break;
            }
        }
    }
    if (updated > 0)
    {
        refresh_ben_store(priv);
        save_estate_state(priv, true);
    }

    if (priv->lbl_status)
    {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      _("Synced %d linked row(s). GnuCash book asset total: %s"),
                      updated, fmt_money_str(total_assets).c_str());
        gtk_label_set_text(priv->lbl_status, buf);
    }
}

static void
on_ice_toggle(GtkCellRendererToggle*, gchar* path_str, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkTreePath* path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(priv->ice_store), &iter, path))
    {
        gint idx = -1;
        gtk_tree_model_get(GTK_TREE_MODEL(priv->ice_store), &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->ice_rows.size()))
        {
            priv->ice_rows[idx].verified = !priv->ice_rows[idx].verified;
            refresh_ice_store(priv);
            update_progress_bars_and_triggers(priv);
            save_estate_state(priv, true);
        }
    }
    gtk_tree_path_free(path);
}

static void
on_ice_selection_changed(GtkTreeSelection* sel, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
    {
        gint idx = -1;
        gtk_tree_model_get(model, &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->ice_rows.size()))
        {
            priv->selected_ice_idx = idx;
            const auto& r = priv->ice_rows[idx];
            int cat = 0;
            if (r.category == "Insurance") cat = 1;
            else if (r.category == "Property/Tax") cat = 2;
            else if (r.category == "Password/2FA") cat = 3;
            else if (r.category == "Email/Domain") cat = 4;
            else if (r.category == "Auto-Pay Bill") cat = 5;
            gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ice_cat), cat);
            gtk_entry_set_text(priv->entry_ice_inst, r.institution.c_str());
            gtk_entry_set_text(priv->entry_ice_acct, r.account_policy_id.c_str());
            gtk_entry_set_text(priv->entry_ice_recovery, r.recovery_location.c_str());
            gtk_entry_set_text(priv->entry_ice_notes, r.notes.c_str());
        }
    }
}

static void
on_ice_update_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_ice_idx < 0 || priv->selected_ice_idx >= static_cast<gint>(priv->ice_rows.size()))
        return;
    auto& r = priv->ice_rows[priv->selected_ice_idx];
    gchar* c = gtk_combo_box_text_get_active_text(priv->combo_ice_cat);
    r.category = c ? c : "Financial";
    g_free(c);
    r.institution = gtk_entry_get_text(priv->entry_ice_inst);
    r.account_policy_id = gtk_entry_get_text(priv->entry_ice_acct);
    r.recovery_location = gtk_entry_get_text(priv->entry_ice_recovery);
    r.notes = gtk_entry_get_text(priv->entry_ice_notes);
    refresh_ice_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_ice_add_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    IceDirectoryRow r;
    gchar* c = gtk_combo_box_text_get_active_text(priv->combo_ice_cat);
    r.category = c ? c : "Financial";
    g_free(c);
    r.institution = gtk_entry_get_text(priv->entry_ice_inst);
    if (r.institution.empty()) r.institution = "New Institution / Service";
    r.account_policy_id = gtk_entry_get_text(priv->entry_ice_acct);
    r.recovery_location = gtk_entry_get_text(priv->entry_ice_recovery);
    r.notes = gtk_entry_get_text(priv->entry_ice_notes);
    priv->ice_rows.push_back(std::move(r));
    priv->selected_ice_idx = static_cast<int>(priv->ice_rows.size()) - 1;
    refresh_ice_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_ice_delete_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_ice_idx < 0 || priv->selected_ice_idx >= static_cast<gint>(priv->ice_rows.size()))
        return;
    priv->ice_rows.erase(priv->ice_rows.begin() + priv->selected_ice_idx);
    priv->selected_ice_idx = -1;
    refresh_ice_store(priv);
    update_progress_bars_and_triggers(priv);
    save_estate_state(priv, true);
}

static void
on_contact_selection_changed(GtkTreeSelection* sel, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkTreeModel* model = nullptr;
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, &model, &iter))
    {
        gint idx = -1;
        gtk_tree_model_get(model, &iter, 0, &idx, -1);
        if (idx >= 0 && idx < static_cast<gint>(priv->contacts.size()))
        {
            priv->selected_contact_idx = idx;
            const auto& c = priv->contacts[idx];
            gtk_entry_set_text(priv->entry_ct_role, c.role.c_str());
            gtk_entry_set_text(priv->entry_ct_name, c.name.c_str());
            gtk_entry_set_text(priv->entry_ct_firm, c.firm_org.c_str());
            gtk_entry_set_text(priv->entry_ct_phone, c.phone.c_str());
            gtk_entry_set_text(priv->entry_ct_email, c.email.c_str());
            gtk_entry_set_text(priv->entry_ct_notes, c.notes.c_str());
        }
    }
}

static void
on_contact_update_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_contact_idx < 0 || priv->selected_contact_idx >= static_cast<gint>(priv->contacts.size()))
        return;
    auto& c = priv->contacts[priv->selected_contact_idx];
    c.role = gtk_entry_get_text(priv->entry_ct_role);
    c.name = gtk_entry_get_text(priv->entry_ct_name);
    c.firm_org = gtk_entry_get_text(priv->entry_ct_firm);
    c.phone = gtk_entry_get_text(priv->entry_ct_phone);
    c.email = gtk_entry_get_text(priv->entry_ct_email);
    c.notes = gtk_entry_get_text(priv->entry_ct_notes);
    refresh_contact_store(priv);
    save_estate_state(priv, true);
}

static void
on_contact_add_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    FiduciaryContact c;
    c.role = gtk_entry_get_text(priv->entry_ct_role);
    if (c.role.empty()) c.role = "New Contact / Role";
    c.name = gtk_entry_get_text(priv->entry_ct_name);
    c.firm_org = gtk_entry_get_text(priv->entry_ct_firm);
    c.phone = gtk_entry_get_text(priv->entry_ct_phone);
    c.email = gtk_entry_get_text(priv->entry_ct_email);
    c.notes = gtk_entry_get_text(priv->entry_ct_notes);
    priv->contacts.push_back(std::move(c));
    priv->selected_contact_idx = static_cast<int>(priv->contacts.size()) - 1;
    refresh_contact_store(priv);
    save_estate_state(priv, true);
}

static void
on_contact_delete_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->selected_contact_idx < 0 || priv->selected_contact_idx >= static_cast<gint>(priv->contacts.size()))
        return;
    priv->contacts.erase(priv->contacts.begin() + priv->selected_contact_idx);
    priv->selected_contact_idx = -1;
    refresh_contact_store(priv);
    save_estate_state(priv, true);
}

static void
on_ht_changed(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    recompute_heir_tax_estimator(priv);
    save_estate_state(priv, false);
}

static void
on_ht_pull_book_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    auto accts = collect_gnucash_asset_accounts();
    double trad = 0.0, roth = 0.0;
    for (const auto& ac : accts)
    {
        if (!ac.is_leaf) continue;
        std::string low = ac.full_name;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low.find("roth") != std::string::npos)
            roth += ac.balance;
        else if (low.find("401") != std::string::npos || low.find("403") != std::string::npos ||
                 low.find("ira") != std::string::npos || low.find("457") != std::string::npos)
            trad += ac.balance;
    }
    if (priv->spin_ht_trad) gtk_spin_button_set_value(priv->spin_ht_trad, trad);
    if (priv->spin_ht_roth) gtk_spin_button_set_value(priv->spin_ht_roth, roth);
    recompute_heir_tax_estimator(priv);
    save_estate_state(priv, true);
    if (priv->lbl_status)
    {
        std::string msg = "Pulled from GnuCash book: Tax-Deferred " + fmt_money_str(trad) + ", Roth " + fmt_money_str(roth);
        gtk_label_set_text(priv->lbl_status, msg.c_str());
    }
}

static void
on_ht_pull_planner_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GKeyFile* st = gnc_state_get_current();
    if (!st || !g_key_file_has_group(st, "RetirementPlanner"))
    {
        on_ht_pull_book_clicked(nullptr, user_data);
        return;
    }
    auto get_d = [&](const char* k, double def) {
        GError* err = nullptr;
        double v = g_key_file_get_double(st, "RetirementPlanner", k, &err);
        if (err) { g_error_free(err); return def; }
        return v;
    };
    double trad = get_d("last_end_tax_deferred", -1.0);
    double roth = get_d("last_end_tax_free", -1.0);
    double hrate = get_d("heir_tax_rate", -1.0);
    if (trad < 0.0 && roth < 0.0)
    {
        on_ht_pull_book_clicked(nullptr, user_data);
        if (hrate >= 0.0 && priv->spin_ht_rate)
            gtk_spin_button_set_value(priv->spin_ht_rate, hrate * 100.0);
        return;
    }
    if (trad >= 0.0 && priv->spin_ht_trad) gtk_spin_button_set_value(priv->spin_ht_trad, trad);
    if (roth >= 0.0 && priv->spin_ht_roth) gtk_spin_button_set_value(priv->spin_ht_roth, roth);
    if (hrate >= 0.0 && priv->spin_ht_rate) gtk_spin_button_set_value(priv->spin_ht_rate, hrate * 100.0);
    recompute_heir_tax_estimator(priv);
    save_estate_state(priv, true);
    if (priv->lbl_status)
    {
        std::string msg = "Synced from Owl Retirement Planner: Tax-Deferred " + fmt_money_str(std::max(0.0, trad))
                        + ", Roth " + fmt_money_str(std::max(0.0, roth));
        gtk_label_set_text(priv->lbl_status, msg.c_str());
    }
}

static void
on_stamp_audit_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    std::string today = today_iso_date();
    if (priv->entry_last_audit)
        gtk_entry_set_text(priv->entry_last_audit, today.c_str());
    for (auto& b : priv->ben_rows)
    {
        if (b.verified)
            b.last_verified_date = today;
    }
    refresh_ben_store(priv);
    save_estate_state(priv, true);
    if (priv->lbl_status)
        gtk_label_set_text(priv->lbl_status, ("Stamped annual audit date: " + today).c_str());
}

static void
on_export_html_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Export Estate Binder Report (HTML)"), nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Save"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "Florida_Estate_Binder_Report.html");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path)
        {
            std::string st = selected_state_name(priv);
            std::ofstream f(path);
            f << "<!DOCTYPE html><html><head><meta charset='utf-8'>"
              << "<title>" << html_escape(st) << " Estate & Continuity Binder</title>"
              << "<style>body{font-family:sans-serif;max-width:960px;margin:24px auto;color:#222}"
              << "h1,h2{color:#1b4d3e}table{border-collapse:collapse;width:100%;margin:12px 0}"
              << "th,td{border:1px solid #ccc;padding:6px 8px;text-align:left;font-size:13px}"
              << "th{background:#f0f4f2}.done{color:#1b7a3d;font-weight:bold}.todo{color:#a34815}</style></head><body>\n";
            f << "<h1>" << html_escape(st) << " Estate &amp; Continuity Checklist</h1>\n";
            f << "<p><strong>Prepared For:</strong> " << html_escape(gtk_entry_get_text(priv->entry_prepared_for))
              << " | <strong>Scenarios:</strong> " << html_escape(gtk_entry_get_text(priv->entry_scenarios)) << "<br>\n"
              << "<strong>Heirs:</strong> " << html_escape(gtk_entry_get_text(priv->entry_heirs)) << "<br>\n"
              << "<strong>State / County:</strong> " << html_escape(st) << " (" << html_escape(gtk_entry_get_text(priv->entry_jurisdiction)) << ")"
              << " | <strong>Safe Location:</strong> " << html_escape(gtk_entry_get_text(priv->entry_safe_location)) << "<br>\n"
              << "<strong>Attorney:</strong> " << html_escape(gtk_entry_get_text(priv->entry_attorney))
              << " | <strong>Last Audit:</strong> " << html_escape(gtk_entry_get_text(priv->entry_last_audit))
              << " | <strong>Next Audit:</strong> " << html_escape(gtk_entry_get_text(priv->entry_next_audit)) << "</p>\n";

            auto emit_sec = [&](int sec, const char* heading) {
                f << "<h2>" << heading << "</h2><table><tr><th>Status</th><th>Item</th><th>Trigger / Due</th><th>Owner</th><th>Date</th><th>Details &amp; Document</th></tr>\n";
                for (const auto& it : priv->items)
                {
                    if (it.section != sec) continue;
                    f << "<tr><td class='" << (it.done ? "done" : "todo") << "'>"
                      << (it.done ? "&#x2714; " : "&#x2610; ") << html_escape(it.status) << "</td>"
                      << "<td><strong>" << html_escape(it.title) << "</strong></td>"
                      << "<td>" << html_escape(it.due_trigger) << "</td>"
                      << "<td>" << html_escape(it.owner_agent) << "</td>"
                      << "<td>" << html_escape(it.completed_date) << "</td>"
                      << "<td>" << html_escape(it.details);
                    if (!it.doc_path.empty())
                        f << "<br><em>Doc: " << html_escape(it.doc_path) << "</em>";
                    f << "</td></tr>\n";
                }
                f << "</table>\n";
            };

            emit_sec(SEC_LEGAL, "1. Core Florida Legal Documents");

            f << "<h2>2. Account Titling &amp; Beneficiary Audit</h2><table>"
              << "<tr><th>Verified</th><th>Account / Asset</th><th>Approx. Value</th><th>Primary</th><th>Contingent / Structure</th><th>Re-Check Trigger</th><th>Date</th></tr>\n";
            for (const auto& b : priv->ben_rows)
            {
                f << "<tr><td class='" << (b.verified ? "done" : "todo") << "'>"
                  << (b.verified ? "&#x2714; Verified" : "&#x2610; Pending") << "</td>"
                  << "<td><strong>" << html_escape(b.account_name) << "</strong></td>"
                  << "<td>" << html_escape(b.approx_value) << "</td>"
                  << "<td>" << html_escape(b.primary_beneficiary) << "</td>"
                  << "<td>" << html_escape(b.contingent_structure) << "</td>"
                  << "<td>" << html_escape(b.recheck_trigger) << "</td>"
                  << "<td>" << html_escape(b.last_verified_date) << "</td></tr>\n";
            }
            f << "</table>\n";

            emit_sec(SEC_TAX, "3. Tax &amp; Inherited-Account Continuity Rules");
            emit_sec(SEC_ICE, "4. Operational ICE &amp; Digital Continuity");

            f << "<h2>ICE &amp; Digital Continuity Directory</h2><table>"
              << "<tr><th>Verified</th><th>Category</th><th>Institution / Service</th><th>Account / Policy #</th><th>Recovery / Safe Location</th><th>Notes</th></tr>\n";
            for (const auto& r : priv->ice_rows)
            {
                f << "<tr><td class='" << (r.verified ? "done" : "todo") << "'>"
                  << (r.verified ? "&#x2714;" : "&#x2610;") << "</td>"
                  << "<td>" << html_escape(r.category) << "</td>"
                  << "<td><strong>" << html_escape(r.institution) << "</strong></td>"
                  << "<td>" << html_escape(r.account_policy_id) << "</td>"
                  << "<td>" << html_escape(r.recovery_location) << "</td>"
                  << "<td>" << html_escape(r.notes) << "</td></tr>\n";
            }
            f << "</table>\n";

            f << "<h2>Fiduciary &amp; Key Contacts Roster</h2><table>"
              << "<tr><th>Role</th><th>Name</th><th>Firm / Org</th><th>Phone</th><th>Email</th><th>Notes</th></tr>\n";
            for (const auto& c : priv->contacts)
            {
                f << "<tr><td><strong>" << html_escape(c.role) << "</strong></td>"
                  << "<td>" << html_escape(c.name) << "</td>"
                  << "<td>" << html_escape(c.firm_org) << "</td>"
                  << "<td>" << html_escape(c.phone) << "</td>"
                  << "<td>" << html_escape(c.email) << "</td>"
                  << "<td>" << html_escape(c.notes) << "</td></tr>\n";
            }
            f << "</table></body></html>\n";
            if (priv->lbl_status)
                gtk_label_set_text(priv->lbl_status, _("Exported HTML estate binder report."));
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void
on_export_md_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        _("Export Estate Checklist (Markdown)"), nullptr, GTK_FILE_CHOOSER_ACTION_SAVE,
        _("_Cancel"), GTK_RESPONSE_CANCEL, _("_Save"), GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "Florida_Estate_Continuity_Checklist.md");
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT)
    {
        gchar* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path)
        {
            std::string st = selected_state_name(priv);
            std::ofstream f(path);
            f << "# " << st << " ESTATE & CONTINUITY CHECKLIST\n\n"
              << "**Prepared for:** " << gtk_entry_get_text(priv->entry_prepared_for)
              << " | **Scenarios:** " << gtk_entry_get_text(priv->entry_scenarios) << "\n"
              << "**State / County:** " << st << " (" << gtk_entry_get_text(priv->entry_jurisdiction) << ")\n"
              << "**Heirs:** " << gtk_entry_get_text(priv->entry_heirs) << "\n\n";

            auto emit_md_sec = [&](int sec, const char* heading) {
                f << "## " << heading << "\n\n";
                for (const auto& it : priv->items)
                {
                    if (it.section != sec) continue;
                    f << "- [" << (it.done ? "x" : " ") << "] **" << it.title << "**"
                      << " (" << it.status;
                    if (!it.due_trigger.empty()) f << ", Trigger: " << it.due_trigger;
                    if (!it.completed_date.empty()) f << ", Completed: " << it.completed_date;
                    f << ")\n";
                    if (!it.details.empty())
                        f << "  - " << it.details << "\n";
                }
                f << "\n";
            };

            emit_md_sec(SEC_LEGAL, "1. Core Florida Legal Documents");

            f << "## 2. Account Titling & Beneficiary Audit\n\n"
              << "| Account / Asset | Value | Primary | Contingent / Structure | Re-Check Trigger | Status |\n"
              << "| :--- | :--- | :--- | :--- | :--- | :---: |\n";
            for (const auto& b : priv->ben_rows)
            {
                f << "| " << b.account_name << " | " << b.approx_value << " | " << b.primary_beneficiary
                  << " | " << b.contingent_structure << " | " << b.recheck_trigger
                  << " | [" << (b.verified ? "x" : " ") << "] |\n";
            }
            f << "\n";

            emit_md_sec(SEC_TAX, "3. Tax & Inherited-Account Continuity Rules");
            emit_md_sec(SEC_ICE, "4. Operational ICE & Digital Continuity");

            if (priv->lbl_status)
                gtk_label_set_text(priv->lbl_status, _("Exported Markdown checklist."));
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
}

static void
on_reset_defaults_clicked(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        _("Load Estate Checklist Template"), nullptr, GTK_DIALOG_MODAL,
        _("_Cancel"), GTK_RESPONSE_CANCEL,
        _("Generic US Template"), 1,
        _("Florida Template"), 2,
        nullptr);
    GtkWidget* area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget* lbl = gtk_label_new(
        _("Choose a checklist template to load (replaces current checklist & beneficiary items):"));
    gtk_container_set_border_width(GTK_CONTAINER(area), 12);
    gtk_box_pack_start(GTK_BOX(area), lbl, FALSE, FALSE, 6);
    gtk_widget_show_all(dlg);

    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == 2)
    {
        populate_default_florida_checklist(priv);
        refresh_all_views(priv);
        save_estate_state(priv, true);
        if (priv->lbl_status)
            gtk_label_set_text(priv->lbl_status, _("Loaded Florida Estate & Continuity template."));
    }
    else if (resp == 1)
    {
        populate_generic_us_template(priv, selected_state_name(priv));
        refresh_all_views(priv);
        save_estate_state(priv, true);
        if (priv->lbl_status)
            gtk_label_set_text(priv->lbl_status, _("Loaded Generic US Estate template."));
    }
    gtk_widget_destroy(dlg);
}

static const char*
estate_ai_default_system_prompt()
{
    return
        "You are an analytical personal estate-continuity and tax-planning advisor "
        "(note: remind the user to confirm state-specific legal documents with a licensed "
        "attorney in their state).\n\n"
        "OPERATING RULES:\n"
        "1. Show Your Math: When analyzing SECURE Act 10-year inherited IRA taxes, Roth "
        "conversion savings, or estate thresholds, show the step-by-step numbers.\n"
        "2. State Specificity: Tailor probate, homestead, TBE, Lady Bird / TOD deed, POA, "
        "and state estate/inheritance tax guidance to the user's selected state.\n"
        "3. Prioritize Gaps: Highlight missing legal documents, stale (>12 month) or "
        "unverified beneficiary audits, unlinked accounts, and upcoming triggers first.\n"
        "4. Privacy: Do not ask for raw account numbers, passwords, or SSNs.\n"
        "5. Structure every response into: Gaps & Risks, Step-by-Step Analysis, "
        "Actionable Next Steps, and Questions for Your Attorney/CPA.";
}

static std::string
shared_ai_config_path()
{
    gchar* p = gnc_build_userdata_path("retirement-ai.conf");
    std::string out = p ? p : "";
    g_free(p);
    return out;
}

static void
populate_estate_ai_models(GncPluginPageEstatePrivate* priv,
                          const std::vector<std::string>& models,
                          const char* preferred = nullptr)
{
    if (!priv->combo_ai_model) return;
    std::string cur = priv->entry_ai_model ? gtk_entry_get_text(priv->entry_ai_model) : "";
    gtk_combo_box_text_remove_all(priv->combo_ai_model);
    for (const auto& m : models)
        gtk_combo_box_text_append_text(priv->combo_ai_model, m.c_str());
    if (preferred && preferred[0] != '\0')
        gtk_entry_set_text(priv->entry_ai_model, preferred);
    else if (!cur.empty() && std::find(models.begin(), models.end(), cur) != models.end())
        gtk_entry_set_text(priv->entry_ai_model, cur.c_str());
    else if (!models.empty())
        gtk_entry_set_text(priv->entry_ai_model, models.front().c_str());
}

static void
apply_estate_ai_provider_defaults(GncPluginPageEstatePrivate* priv, int idx)
{
    if (idx == 0)
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "http://localhost:11434/api/chat");
        populate_estate_ai_models(priv, {"llama3.2", "qwen2.5", "mistral", "gemma2"}, "llama3.2");
    }
    else if (idx == 1)
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "https://generativelanguage.googleapis.com/v1beta");
        populate_estate_ai_models(priv, {"gemini-2.5-flash", "gemini-2.5-pro", "gemini-2.0-flash"}, "gemini-2.5-flash");
    }
    else
    {
        gtk_entry_set_text(priv->entry_ai_endpoint, "https://api.openai.com/v1/chat/completions");
        populate_estate_ai_models(priv, {"gpt-4o-mini", "gpt-4o", "o3-mini"}, "gpt-4o-mini");
    }
}

static void
load_shared_ai_config(GncPluginPageEstatePrivate* priv)
{
    if (!priv || !priv->combo_ai_provider) return;
    std::string cfg = shared_ai_config_path();
    GKeyFile* kf = g_key_file_new();
    bool ok = !cfg.empty() && g_key_file_load_from_file(kf, cfg.c_str(), G_KEY_FILE_NONE, nullptr);

    priv->updating_ui = true;
    int p_idx = 0;
    if (ok && g_key_file_has_key(kf, "RetirementAI", "ai_provider", nullptr))
        p_idx = std::clamp(g_key_file_get_integer(kf, "RetirementAI", "ai_provider", nullptr), 0, 2);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ai_provider), p_idx);
    apply_estate_ai_provider_defaults(priv, p_idx);

    auto load_s = [&](const char* k, GtkEntry* e) {
        if (!ok || !e) return;
        if (gchar* v = g_key_file_get_string(kf, "RetirementAI", k, nullptr))
        {
            if (v[0] != '\0') gtk_entry_set_text(e, v);
            g_free(v);
        }
    };
    load_s("ai_endpoint", priv->entry_ai_endpoint);
    load_s("ai_model", priv->entry_ai_model);
    load_s("ai_api_key", priv->entry_ai_key);
    priv->updating_ui = false;
    g_key_file_free(kf);
}

static void
save_shared_ai_config(GncPluginPageEstatePrivate* priv)
{
    if (!priv || !priv->root_widget || priv->updating_ui || !priv->combo_ai_provider)
        return;
    std::string cfg = shared_ai_config_path();
    if (cfg.empty()) return;
    GKeyFile* kf = g_key_file_new();
    g_key_file_load_from_file(kf, cfg.c_str(), G_KEY_FILE_KEEP_COMMENTS, nullptr);
    int p_idx = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)), 0, 2);
    g_key_file_set_integer(kf, "RetirementAI", "ai_provider", p_idx);
    auto save_s = [&](const char* k, const char* prefix, GtkEntry* e) {
        if (!e) return;
        const char* v = gtk_entry_get_text(e);
        g_key_file_set_string(kf, "RetirementAI", k, v ? v : "");
        std::string pk = std::string(prefix) + "_" + std::to_string(p_idx);
        g_key_file_set_string(kf, "RetirementAI", pk.c_str(), v ? v : "");
    };
    save_s("ai_endpoint", "ai_endpoint", priv->entry_ai_endpoint);
    save_s("ai_model", "ai_model", priv->entry_ai_model);
    save_s("ai_api_key", "ai_api_key", priv->entry_ai_key);
    g_key_file_save_to_file(kf, cfg.c_str(), nullptr);
    g_chmod(cfg.c_str(), 0600);
    g_key_file_free(kf);
}

static std::string
build_estate_ai_summary(GncPluginPageEstatePrivate* priv)
{
    std::ostringstream os;
    std::string st = selected_state_name(priv);
    os << "=== ANONYMIZED ESTATE & CONTINUITY SUMMARY ===\n";
    os << "State: " << st << " (" << gtk_entry_get_text(priv->entry_jurisdiction) << ")\n";
    os << "State Guidance: " << state_estate_guidance(st) << "\n";
    os << "Scenarios: " << gtk_entry_get_text(priv->entry_scenarios) << "\n";
    os << "Last Audit: " << gtk_entry_get_text(priv->entry_last_audit)
       << " | Next Audit: " << gtk_entry_get_text(priv->entry_next_audit) << "\n\n";

    os << "Checklist Items:\n";
    for (const auto& it : priv->items)
    {
        const char* sname = (it.section == SEC_LEGAL) ? "Legal"
                          : (it.section == SEC_TAX)   ? "Tax"
                                                      : "ICE";
        os << "- [" << (it.done ? "x" : " ") << "] [" << sname << "] " << it.title
           << " | Status: " << it.status;
        if (!it.due_trigger.empty()) os << " | Trigger: " << it.due_trigger;
        os << "\n";
    }

    os << "\nBeneficiary & Asset Audit (no account numbers):\n";
    for (const auto& b : priv->ben_rows)
    {
        bool stale = !b.verified || is_date_stale(b.last_verified_date);
        os << "- [" << (b.verified ? "x" : " ") << "] " << b.account_name
           << " | Value: " << b.approx_value
           << " | Primary: " << b.primary_beneficiary
           << " | Contingent: " << b.contingent_structure
           << " | Trigger: " << b.recheck_trigger
           << " | Audit: " << (b.verified ? (stale ? "Stale >12m" : "Current") : "Pending") << "\n";
    }

    if (priv->spin_ht_trad)
    {
        os << "\nSECURE Act 10-Yr Inherited Account Inputs:\n"
           << "- Tax-Deferred: " << fmt_money_str(gtk_spin_button_get_value(priv->spin_ht_trad))
           << " | Roth: " << fmt_money_str(gtk_spin_button_get_value(priv->spin_ht_roth))
           << " | Heirs: " << gtk_spin_button_get_value_as_int(priv->spin_ht_heirs)
           << " | Heir Rate: " << gtk_spin_button_get_value(priv->spin_ht_rate) << "%\n";
    }
    return os.str();
}

static void
set_estate_ai_response(GncPluginPageEstatePrivate* priv, const std::string& txt)
{
    if (priv->tv_ai_response)
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(priv->tv_ai_response), txt.c_str(), -1);
}

static void
append_estate_ai_response(GncPluginPageEstatePrivate* priv, const std::string& tok)
{
    if (!priv->tv_ai_response || tok.empty()) return;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(priv->tv_ai_response);
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_insert(buf, &end, tok.c_str(), static_cast<gint>(tok.size()));
}

static void
ensure_estate_soup(GncPluginPageEstatePrivate* priv)
{
    if (!priv->soup_session)
    {
        priv->soup_session = soup_session_new();
        soup_session_set_timeout(priv->soup_session, 600);
        soup_session_set_idle_timeout(priv->soup_session, 600);
    }
}

struct EstateAiCtx
{
    GncPluginPageEstatePrivate* priv = nullptr;
    gnc::retirement::AiProvider provider = gnc::retirement::AiProvider::Ollama;
    GInputStream* stream = nullptr;
    std::string line_buf;
    std::string full_reply;
    bool cleared_thinking = false;

    ~EstateAiCtx()
    {
        if (stream) g_object_unref(stream);
    }
};

static void
finish_estate_ai_stream(std::unique_ptr<EstateAiCtx> ctx, const char* err_msg = nullptr)
{
    auto* priv = ctx->priv;
    if (!priv || !priv->root_widget) return;
    if (priv->btn_ai_stop) gtk_widget_set_sensitive(priv->btn_ai_stop, FALSE);
    if (err_msg)
    {
        if (!ctx->cleared_thinking)
            set_estate_ai_response(priv, err_msg);
        else
            append_estate_ai_response(priv, std::string("\n\n[") + err_msg + "]");
        if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, err_msg);
        return;
    }
    if (!ctx->line_buf.empty())
    {
        std::string tok = gnc::retirement::extract_ai_stream_line(ctx->provider, ctx->line_buf);
        if (!tok.empty())
        {
            if (!ctx->cleared_thinking) { set_estate_ai_response(priv, ""); ctx->cleared_thinking = true; }
            ctx->full_reply += tok;
            append_estate_ai_response(priv, tok);
        }
    }
    if (ctx->full_reply.empty())
        set_estate_ai_response(priv, "No response tokens received from provider.");
    if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Done."));
}

static void
on_estate_ai_stream_read(GObject* source, GAsyncResult* res, gpointer user_data)
{
    std::unique_ptr<EstateAiCtx> ctx(static_cast<EstateAiCtx*>(user_data));
    auto* priv = ctx->priv;
    GError* err = nullptr;
    GBytes* bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), res, &err);
    if (!priv || !priv->root_widget)
    {
        if (bytes) g_bytes_unref(bytes);
        if (err) g_error_free(err);
        return;
    }
    if (!bytes)
    {
        std::string msg = std::string("Stream failed: ") + (err ? err->message : "Unknown error");
        if (err) g_error_free(err);
        finish_estate_ai_stream(std::move(ctx), msg.c_str());
        return;
    }
    gsize len = 0;
    const char* raw = static_cast<const char*>(g_bytes_get_data(bytes, &len));
    if (len == 0)
    {
        g_bytes_unref(bytes);
        finish_estate_ai_stream(std::move(ctx));
        return;
    }
    ctx->line_buf.append(raw, len);
    g_bytes_unref(bytes);

    size_t nl = 0;
    while ((nl = ctx->line_buf.find('\n')) != std::string::npos)
    {
        std::string line = ctx->line_buf.substr(0, nl);
        ctx->line_buf.erase(0, nl + 1);
        std::string tok = gnc::retirement::extract_ai_stream_line(ctx->provider, line);
        if (!tok.empty())
        {
            if (!ctx->cleared_thinking)
            {
                set_estate_ai_response(priv, "");
                ctx->cleared_thinking = true;
                if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Streaming response..."));
            }
            ctx->full_reply += tok;
            append_estate_ai_response(priv, tok);
        }
    }
    GInputStream* stream = ctx->stream;
    g_input_stream_read_bytes_async(stream, 4096, G_PRIORITY_DEFAULT,
                                    priv->ai_cancel, on_estate_ai_stream_read, ctx.release());
}

static void
on_estate_ai_send_opened(GObject* source, GAsyncResult* res, gpointer user_data)
{
    std::unique_ptr<EstateAiCtx> ctx(static_cast<EstateAiCtx*>(user_data));
    auto* priv = ctx->priv;
    GError* err = nullptr;
    GInputStream* in_stream = soup_session_send_finish(SOUP_SESSION(source), res, &err);
    if (!priv || !priv->root_widget)
    {
        if (in_stream) g_object_unref(in_stream);
        if (err) g_error_free(err);
        return;
    }
    if (!in_stream)
    {
        std::string msg = std::string("Request failed: ") + (err ? err->message : "Unknown network error");
        if (err) g_error_free(err);
        finish_estate_ai_stream(std::move(ctx), msg.c_str());
        return;
    }
    SoupMessage* msg = soup_session_get_async_result_message(SOUP_SESSION(source), res);
    guint status = msg ? soup_message_get_status(msg) : 0;
    if (status == 404 && ctx->provider == gnc::retirement::AiProvider::Ollama)
    {
        g_object_unref(in_stream);
        finish_estate_ai_stream(std::move(ctx), "Model not found on Ollama (HTTP 404). Click 'Refresh Models'.");
        return;
    }
    ctx->stream = in_stream;
    g_input_stream_read_bytes_async(in_stream, 4096, G_PRIORITY_DEFAULT,
                                    priv->ai_cancel, on_estate_ai_stream_read, ctx.release());
}

static void
on_estate_ai_gemini_finished(GObject* source, GAsyncResult* res, gpointer user_data)
{
    std::unique_ptr<EstateAiCtx> ctx(static_cast<EstateAiCtx*>(user_data));
    auto* priv = ctx->priv;
    GError* err = nullptr;
    GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);
    if (!priv || !priv->root_widget)
    {
        if (bytes) g_bytes_unref(bytes);
        if (err) g_error_free(err);
        return;
    }
    if (priv->btn_ai_stop) gtk_widget_set_sensitive(priv->btn_ai_stop, FALSE);
    if (!bytes)
    {
        std::string msg = std::string("Request failed: ") + (err ? err->message : "Unknown network error");
        if (err) g_error_free(err);
        set_estate_ai_response(priv, msg);
        if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, msg.c_str());
        return;
    }
    gsize len = 0;
    const char* raw = static_cast<const char*>(g_bytes_get_data(bytes, &len));
    std::string resp_str(raw ? raw : "", len);
    g_bytes_unref(bytes);
    std::string reply = gnc::retirement::extract_ai_response_text(ctx->provider, resp_str);
    set_estate_ai_response(priv, reply);
    if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Done."));
}

static void
on_estate_ai_models_finished(GObject* source, GAsyncResult* res, gpointer user_data)
{
    std::unique_ptr<EstateAiCtx> ctx(static_cast<EstateAiCtx*>(user_data));
    auto* priv = ctx->priv;
    GError* err = nullptr;
    GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);
    if (!priv || !priv->root_widget)
    {
        if (bytes) g_bytes_unref(bytes);
        if (err) g_error_free(err);
        return;
    }
    if (!bytes)
    {
        std::string msg = std::string("Model list failed: ") + (err ? err->message : "Unknown error");
        if (err) g_error_free(err);
        if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, msg.c_str());
        return;
    }
    gsize len = 0;
    const char* raw = static_cast<const char*>(g_bytes_get_data(bytes, &len));
    std::string resp_str(raw ? raw : "", len);
    g_bytes_unref(bytes);
    auto models = gnc::retirement::extract_ai_models_list(ctx->provider, resp_str);
    if (models.empty())
    {
        if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("No models returned by provider."));
        return;
    }
    populate_estate_ai_models(priv, models);
    save_shared_ai_config(priv);
    if (priv->lbl_ai_status)
    {
        std::string st = "Loaded " + std::to_string(models.size()) + " models.";
        gtk_label_set_text(priv->lbl_ai_status, st.c_str());
    }
}

static void
on_estate_ai_refresh_models(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    ensure_estate_soup(priv);
    int p_idx = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)), 0, 2);
    auto provider = static_cast<gnc::retirement::AiProvider>(p_idx);
    std::string endpoint = gtk_entry_get_text(priv->entry_ai_endpoint);
    std::string api_key = gtk_entry_get_text(priv->entry_ai_key);
    if (api_key.empty())
    {
        const char* env_var = (provider == gnc::retirement::AiProvider::Gemini) ? g_getenv("GEMINI_API_KEY") : g_getenv("OPENAI_API_KEY");
        if (env_var) api_key = env_var;
    }
    std::string url;
    if (provider == gnc::retirement::AiProvider::Ollama)
    {
        size_t pos = endpoint.find("/api/");
        url = (pos != std::string::npos ? endpoint.substr(0, pos) : endpoint) + "/api/tags";
    }
    else if (provider == gnc::retirement::AiProvider::Gemini)
    {
        if (api_key.empty()) { if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Enter Gemini API key first.")); return; }
        url = "https://generativelanguage.googleapis.com/v1beta/models";
    }
    else
    {
        size_t pos = endpoint.find("/chat/completions");
        url = (pos != std::string::npos ? endpoint.substr(0, pos) : endpoint) + "/models";
    }
    SoupMessage* msg = soup_message_new("GET", url.c_str());
    if (!msg) return;
    if (provider == gnc::retirement::AiProvider::Gemini)
        soup_message_headers_append(soup_message_get_request_headers(msg), "x-goog-api-key", api_key.c_str());
    else if (provider == gnc::retirement::AiProvider::OpenAICompatible && !api_key.empty())
        soup_message_headers_append(soup_message_get_request_headers(msg), "Authorization", ("Bearer " + api_key).c_str());
    if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Fetching models..."));
    auto* ctx = new EstateAiCtx{priv, provider};
    soup_session_send_and_read_async(priv->soup_session, msg, G_PRIORITY_DEFAULT, nullptr, on_estate_ai_models_finished, ctx);
    g_object_unref(msg);
}

static void
on_estate_ai_provider_changed(GtkComboBox*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->updating_ui) return;
    int idx = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)), 0, 2);
    priv->updating_ui = true;
    apply_estate_ai_provider_defaults(priv, idx);
    priv->updating_ui = false;
    save_shared_ai_config(priv);
}

static void
on_estate_ai_setting_changed(GtkWidget*, gpointer user_data)
{
    save_shared_ai_config(static_cast<GncPluginPageEstatePrivate*>(user_data));
}

static void
on_estate_ai_stop(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    if (priv->ai_cancel) g_cancellable_cancel(priv->ai_cancel);
}

static void
send_estate_ai_prompt(GncPluginPageEstatePrivate* priv, const std::string& question)
{
    save_shared_ai_config(priv);
    ensure_estate_soup(priv);
    if (priv->ai_cancel) { g_cancellable_cancel(priv->ai_cancel); g_object_unref(priv->ai_cancel); }
    priv->ai_cancel = g_cancellable_new();

    int p_idx = std::clamp(gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo_ai_provider)), 0, 2);
    auto provider = static_cast<gnc::retirement::AiProvider>(p_idx);
    std::string endpoint = gtk_entry_get_text(priv->entry_ai_endpoint);
    std::string model = gtk_entry_get_text(priv->entry_ai_model);
    std::string api_key = gtk_entry_get_text(priv->entry_ai_key);
    if (api_key.empty())
    {
        const char* env_var = (provider == gnc::retirement::AiProvider::Gemini) ? g_getenv("GEMINI_API_KEY") : g_getenv("OPENAI_API_KEY");
        if (env_var) api_key = env_var;
    }
    if (provider != gnc::retirement::AiProvider::Ollama && api_key.empty())
    {
        set_estate_ai_response(priv, "Please enter an API key (or set GEMINI_API_KEY / OPENAI_API_KEY), or switch Provider to Local Ollama.");
        return;
    }

    std::string url = endpoint;
    if (provider == gnc::retirement::AiProvider::Ollama)
    {
        size_t gp = url.find("/api/generate");
        if (gp != std::string::npos) url.replace(gp, 13, "/api/chat");
    }
    else if (provider == gnc::retirement::AiProvider::Gemini && endpoint.find(":generateContent") == std::string::npos)
    {
        if (!url.empty() && url.back() == '/') url.pop_back();
        url += "/models/" + model + ":generateContent";
    }

    std::string sys_prompt = read_text_view(priv->tv_ai_system);
    if (sys_prompt.empty()) sys_prompt = estate_ai_default_system_prompt();
    std::string full_prompt = build_estate_ai_summary(priv) + "\nQuestion: " + question;
    const bool use_stream = (provider != gnc::retirement::AiProvider::Gemini);
    std::string body = gnc::retirement::build_ai_request_json(provider, model, sys_prompt, full_prompt, use_stream, 2048, 1200);

    SoupMessage* msg = soup_message_new("POST", url.c_str());
    if (!msg) { set_estate_ai_response(priv, "Invalid endpoint URL: " + url); return; }
    if (provider == gnc::retirement::AiProvider::Gemini)
        soup_message_headers_append(soup_message_get_request_headers(msg), "x-goog-api-key", api_key.c_str());
    else if (provider == gnc::retirement::AiProvider::OpenAICompatible && !api_key.empty())
        soup_message_headers_append(soup_message_get_request_headers(msg), "Authorization", ("Bearer " + api_key).c_str());

    GBytes* req_bytes = g_bytes_new(body.data(), body.size());
    soup_message_set_request_body_from_bytes(msg, "application/json", req_bytes);
    g_bytes_unref(req_bytes);

    if (priv->btn_ai_stop) gtk_widget_set_sensitive(priv->btn_ai_stop, TRUE);
    if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, use_stream ? _("Streaming response...") : _("Contacting Gemini..."));
    set_estate_ai_response(priv, use_stream ? "Thinking (streaming tokens)..." : "Thinking...");

    auto* ctx = new EstateAiCtx{priv, provider};
    if (use_stream)
        soup_session_send_async(priv->soup_session, msg, G_PRIORITY_DEFAULT, priv->ai_cancel, on_estate_ai_send_opened, ctx);
    else
        soup_session_send_and_read_async(priv->soup_session, msg, G_PRIORITY_DEFAULT, priv->ai_cancel, on_estate_ai_gemini_finished, ctx);
    g_object_unref(msg);
}

static void
on_estate_ai_audit_gaps(GtkButton*, gpointer user_data)
{
    send_estate_ai_prompt(static_cast<GncPluginPageEstatePrivate*>(user_data),
        "Audit my estate readiness gaps: identify missing legal documents, stale or pending beneficiary audits, "
        "and upcoming mandatory re-check triggers in chronological order.");
}

static void
on_estate_ai_state_risks(GtkButton*, gpointer user_data)
{
    send_estate_ai_prompt(static_cast<GncPluginPageEstatePrivate*>(user_data),
        "Review my state-specific probate, homestead/TBE, Lady Bird vs. Revocable Trust, POA, and state estate/inheritance tax risks.");
}

static void
on_estate_ai_heir_tax(GtkButton*, gpointer user_data)
{
    send_estate_ai_prompt(static_cast<GncPluginPageEstatePrivate*>(user_data),
        "Analyze my SECURE Act 10-year inherited IRA vs. Roth numbers and HSA/Social Security continuity rules. Show the math.");
}

static void
on_estate_ai_ask(GtkWidget*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    std::string q = priv->entry_ai_question ? gtk_entry_get_text(priv->entry_ai_question) : "";
    if (q.empty()) q = "What are my top 3 highest-priority estate continuity action items right now?";
    send_estate_ai_prompt(priv, q);
}

static void
on_estate_ai_copy(GtkButton*, gpointer user_data)
{
    auto* priv = static_cast<GncPluginPageEstatePrivate*>(user_data);
    std::string s = build_estate_ai_summary(priv);
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), s.c_str(), -1);
    if (priv->lbl_ai_status) gtk_label_set_text(priv->lbl_ai_status, _("Copied anonymized estate summary to clipboard."));
}

static gint
cmp_currency_or_text(GtkTreeModel* model, GtkTreeIter* a, GtkTreeIter* b, gpointer data)
{
    int col = GPOINTER_TO_INT(data);
    gchar* sa = nullptr;
    gchar* sb = nullptr;
    gtk_tree_model_get(model, a, col, &sa, -1);
    gtk_tree_model_get(model, b, col, &sb, -1);
    auto parse_num = [](const char* s, double& out) -> bool {
        if (!s) return false;
        std::string clean;
        for (const char* p = s; *p; ++p)
        {
            if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-')
                clean += *p;
        }
        if (clean.empty() || clean == "-" || clean == ".")
            return false;
        char* end = nullptr;
        out = std::strtod(clean.c_str(), &end);
        return end != clean.c_str();
    };
    double da = 0.0, db = 0.0;
    int res = 0;
    if (parse_num(sa, da) && parse_num(sb, db))
        res = (da < db) ? -1 : ((da > db) ? 1 : 0);
    else
        res = g_strcmp0(sa, sb);
    g_free(sa);
    g_free(sb);
    return res;
}

static GtkWidget*
wrap_in_scrolled(GtkWidget* child)
{
    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), child);
    return sw;
}

static GtkWidget*
build_checklist_section_tab(GncPluginPageEstatePrivate* priv, int sec)
{
    auto& tab = priv->sec_tabs[sec];
    tab.section = sec;

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);

    tab.pbar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(tab.pbar), FALSE, FALSE, 0);

    tab.store = gtk_list_store_new(9, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                   G_TYPE_STRING, G_TYPE_STRING);
    tab.tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(tab.store)));
    gtk_tree_view_set_grid_lines(tab.tv, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);

    auto* pair = new std::pair<GncPluginPageEstatePrivate*, int>(priv, sec);
    g_object_set_data_full(G_OBJECT(tab.tv), "sec_pair", pair,
                           [](gpointer p) { delete static_cast<std::pair<GncPluginPageEstatePrivate*, int>*>(p); });

    GtkCellRenderer* r_tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(r_tog, "toggled", G_CALLBACK(on_sec_toggle), pair);
    GtkTreeViewColumn* c_done = gtk_tree_view_column_new_with_attributes(_("Done"), r_tog, "active", 1, nullptr);
    gtk_tree_view_column_set_sort_column_id(c_done, 1);
    gtk_tree_view_append_column(tab.tv, c_done);

    auto add_txt_col = [&](const char* title, int col_id, int min_w) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(title, r, "text", col_id, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, min_w);
        gtk_tree_view_column_set_sort_column_id(c, col_id);
        gtk_tree_view_append_column(tab.tv, c);
    };
    add_txt_col(_("Checklist Item"), 2, 260);
    add_txt_col(_("Status"), 3, 110);
    add_txt_col(_("Due / Trigger"), 4, 150);
    add_txt_col(_("Owner / Agent"), 5, 140);
    add_txt_col(_("Completed"), 6, 95);
    if (sec == SEC_LEGAL)
    {
        add_txt_col(_("Spouse 1"), 7, 130);
        add_txt_col(_("Spouse 2"), 8, 130);
    }

    GtkTreeSelection* sel = gtk_tree_view_get_selection(tab.tv);
    g_signal_connect(sel, "changed", G_CALLBACK(on_sec_selection_changed), pair);

    GtkWidget* sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 150);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(tab.tv));
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    // Editor frame
    GtkWidget* frame = gtk_frame_new(_("Selected Item Details & Notes"));
    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);

    tab.entry_title = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(tab.entry_title), TRUE);
    tab.combo_status = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(tab.combo_status, "Not Started");
    gtk_combo_box_text_append_text(tab.combo_status, "In Progress");
    gtk_combo_box_text_append_text(tab.combo_status, "Attorney Review");
    gtk_combo_box_text_append_text(tab.combo_status, "Completed");
    gtk_combo_box_set_active(GTK_COMBO_BOX(tab.combo_status), 0);

    tab.entry_trigger = GTK_ENTRY(gtk_entry_new());
    tab.entry_owner = GTK_ENTRY(gtk_entry_new());
    tab.entry_date = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab.entry_date, "YYYY-MM-DD");

    tab.tv_details = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(tab.tv_details, GTK_WRAP_WORD_CHAR);
    GtkWidget* dsw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(dsw), 65);
    gtk_container_add(GTK_CONTAINER(dsw), GTK_WIDGET(tab.tv_details));

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Title")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_title), 1, 0, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Status")), 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.combo_status), 5, 0, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Due / Trigger")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_trigger), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Owner / Agent")), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_owner), 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Completed Date")), 4, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_date), 5, 1, 1, 1);

    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Guidance / Notes")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dsw, 1, 2, 5, 1);

    tab.entry_doc = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(tab.entry_doc, _("Optional path to scanned PDF / document"));
    gtk_widget_set_hexpand(GTK_WIDGET(tab.entry_doc), TRUE);
    GtkWidget* b_doc_browse = gtk_button_new_with_label(_("Browse…"));
    GtkWidget* b_doc_open   = gtk_button_new_with_label(_("Open"));
    g_signal_connect(b_doc_browse, "clicked", G_CALLBACK(on_sec_browse_doc_clicked), pair);
    g_signal_connect(b_doc_open, "clicked", G_CALLBACK(on_sec_open_doc_clicked), pair);
    GtkWidget* doc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(doc_box), GTK_WIDGET(tab.entry_doc), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(doc_box), b_doc_browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(doc_box), b_doc_open, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Document Link")), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), doc_box, 1, 3, 5, 1);

    int btn_row = 4;
    if (sec == SEC_LEGAL)
    {
        auto make_sp_combo = []() {
            GtkComboBoxText* c = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
            for (const char* s : {"Not Started", "In Progress", "Attorney Review", "Completed"})
                gtk_combo_box_text_append_text(c, s);
            gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
            return c;
        };
        tab.combo_sp1_status = make_sp_combo();
        tab.entry_sp1_date = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(tab.entry_sp1_date, "YYYY-MM-DD");
        tab.combo_sp2_status = make_sp_combo();
        tab.entry_sp2_date = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(tab.entry_sp2_date, "YYYY-MM-DD");
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Spouse 1")), 0, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.combo_sp1_status), 1, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_sp1_date), 2, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(_("Spouse 2")), 3, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.combo_sp2_status), 4, 4, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(tab.entry_sp2_date), 5, 4, 1, 1);
        btn_row = 5;
    }

    GtkWidget* bbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* b_upd = gtk_button_new_with_label(_("Update Item"));
    GtkWidget* b_add = gtk_button_new_with_label(_("Add New Item"));
    GtkWidget* b_del = gtk_button_new_with_label(_("Delete Item"));
    g_signal_connect(b_upd, "clicked", G_CALLBACK(on_sec_update_clicked), pair);
    g_signal_connect(b_add, "clicked", G_CALLBACK(on_sec_add_clicked), pair);
    g_signal_connect(b_del, "clicked", G_CALLBACK(on_sec_delete_clicked), pair);
    gtk_box_pack_start(GTK_BOX(bbox), b_upd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bbox), b_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bbox), b_del, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), bbox, 1, btn_row, 5, 1);

    gtk_container_add(GTK_CONTAINER(frame), grid);
    gtk_box_pack_start(GTK_BOX(vbox), frame, FALSE, FALSE, 0);
    return vbox;
}

} // anonymous namespace

static void gnc_plugin_page_estate_finalize(GObject* object);
static GtkWidget* gnc_plugin_page_estate_create_widget(GncPluginPage* plugin_page);
static void gnc_plugin_page_estate_destroy_widget(GncPluginPage* plugin_page);
static void gnc_plugin_page_estate_save_page(GncPluginPage* plugin_page, GKeyFile* key_file, const gchar* group_name);
static GncPluginPage* gnc_plugin_page_estate_recreate_page(GtkWidget* window, GKeyFile* key_file, const gchar* group_name);

G_DEFINE_TYPE_WITH_PRIVATE(GncPluginPageEstate, gnc_plugin_page_estate, GNC_TYPE_PLUGIN_PAGE)

static void
gnc_plugin_page_estate_class_init(GncPluginPageEstateClass* klass)
{
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    GncPluginPageClass* gnc_plugin_class = GNC_PLUGIN_PAGE_CLASS(klass);

    object_class->finalize = gnc_plugin_page_estate_finalize;

    gnc_plugin_class->tab_icon        = GNC_ICON_ACCOUNT;
    gnc_plugin_class->plugin_name     = GNC_PLUGIN_PAGE_ESTATE_NAME;
    gnc_plugin_class->create_widget   = gnc_plugin_page_estate_create_widget;
    gnc_plugin_class->destroy_widget  = gnc_plugin_page_estate_destroy_widget;
    gnc_plugin_class->save_page       = gnc_plugin_page_estate_save_page;
    gnc_plugin_class->recreate_page   = gnc_plugin_page_estate_recreate_page;
}

static void
gnc_plugin_page_estate_init(GncPluginPageEstate* page)
{
    auto priv = GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(page);
    new (&priv->items) std::vector<EstateChecklistItem>();
    new (&priv->ben_rows) std::vector<BeneficiaryAuditRow>();
    new (&priv->ice_rows) std::vector<IceDirectoryRow>();
    new (&priv->contacts) std::vector<FiduciaryContact>();

    gnc_plugin_page_add_book(GNC_PLUGIN_PAGE(page), gnc_get_current_book());
}

static void
gnc_plugin_page_estate_finalize(GObject* object)
{
    g_return_if_fail(GNC_IS_PLUGIN_PAGE_ESTATE(object));
    auto page = GNC_PLUGIN_PAGE_ESTATE(object);
    auto priv = GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(page);
    priv->items.~vector();
    priv->ben_rows.~vector();
    priv->ice_rows.~vector();
    priv->contacts.~vector();
    G_OBJECT_CLASS(gnc_plugin_page_estate_parent_class)->finalize(object);
}

static GtkWidget*
gnc_plugin_page_estate_create_widget(GncPluginPage* plugin_page)
{
    auto page = GNC_PLUGIN_PAGE_ESTATE(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(page);
    if (priv->root_widget)
        return priv->root_widget;

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(root), 8);
    priv->root_widget = root;

    // Toolbar
    GtkWidget* tbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->lbl_header = GTK_LABEL(gtk_label_new(nullptr));
    gtk_label_set_markup(priv->lbl_header,
                         _("<b>Florida Estate &amp; Continuity Tracker</b>"));
    gtk_box_pack_start(GTK_BOX(tbar), GTK_WIDGET(priv->lbl_header), FALSE, FALSE, 4);

    GtkWidget* btn_audit = gtk_button_new_with_label(_("Audit Complete (Today)"));
    GtkWidget* btn_html = gtk_button_new_with_label(_("Export Binder Report (HTML)"));
    GtkWidget* btn_md = gtk_button_new_with_label(_("Export Checklist (Markdown)"));
    GtkWidget* btn_reset = gtk_button_new_with_label(_("Load Template…"));
    g_signal_connect(btn_audit, "clicked", G_CALLBACK(on_stamp_audit_clicked), priv);
    g_signal_connect(btn_html, "clicked", G_CALLBACK(on_export_html_clicked), priv);
    g_signal_connect(btn_md, "clicked", G_CALLBACK(on_export_md_clicked), priv);
    g_signal_connect(btn_reset, "clicked", G_CALLBACK(on_reset_defaults_clicked), priv);

    priv->lbl_status = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(priv->lbl_status, 1.0f);
    gtk_box_pack_end(GTK_BOX(tbar), GTK_WIDGET(priv->lbl_status), TRUE, TRUE, 4);
    gtk_box_pack_end(GTK_BOX(tbar), btn_reset, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), btn_md, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), btn_html, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(tbar), btn_audit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(root), tbar, FALSE, FALSE, 0);

    GtkWidget* nb = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), TRUE);

    // Tab 0: Overview & Readiness Dashboard
    GtkWidget* tab_ov = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ov), 8);

    GtkWidget* meta_frame = gtk_frame_new(_("Household, Heirs & Audit Metadata"));
    GtkWidget* mgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(mgrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(mgrid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(mgrid), 6);

    auto make_meta_entry = [&](const char* lbl, int r, int c, int w) -> GtkEntry* {
        gtk_grid_attach(GTK_GRID(mgrid), gtk_label_new(lbl), c, r, 1, 1);
        GtkEntry* e = GTK_ENTRY(gtk_entry_new());
        gtk_widget_set_hexpand(GTK_WIDGET(e), TRUE);
        g_signal_connect(e, "changed", G_CALLBACK(on_meta_changed), priv);
        gtk_grid_attach(GTK_GRID(mgrid), GTK_WIDGET(e), c + 1, r, w, 1);
        return e;
    };
    priv->entry_prepared_for = make_meta_entry(_("Prepared For"), 0, 0, 1);
    priv->entry_scenarios    = make_meta_entry(_("Scenarios"), 0, 2, 1);

    gtk_grid_attach(GTK_GRID(mgrid), gtk_label_new(_("State")), 0, 1, 1, 1);
    priv->combo_state = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (const char* st : US_STATES)
        gtk_combo_box_text_append_text(priv->combo_state, st);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_state), DEFAULT_STATE_INDEX);
    g_signal_connect(priv->combo_state, "changed", G_CALLBACK(on_state_changed), priv);
    gtk_grid_attach(GTK_GRID(mgrid), GTK_WIDGET(priv->combo_state), 1, 1, 1, 1);

    priv->entry_jurisdiction = make_meta_entry(_("County / Local Area"), 1, 2, 1);
    priv->entry_safe_location = make_meta_entry(_("Safe / Binder Location"), 2, 0, 1);
    priv->entry_attorney     = make_meta_entry(_("Estate Attorney"), 2, 2, 1);
    priv->entry_heirs        = make_meta_entry(_("Heirs / Contingent"), 3, 0, 3);
    priv->entry_last_audit   = make_meta_entry(_("Last Audit Date"), 4, 0, 1);
    priv->entry_next_audit   = make_meta_entry(_("Next Audit Target"), 4, 2, 1);

    priv->lbl_state_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_line_wrap(priv->lbl_state_note, TRUE);
    gtk_label_set_xalign(priv->lbl_state_note, 0.0f);
    gtk_grid_attach(GTK_GRID(mgrid), GTK_WIDGET(priv->lbl_state_note), 0, 5, 4, 1);

    gtk_container_add(GTK_CONTAINER(meta_frame), mgrid);
    gtk_box_pack_start(GTK_BOX(tab_ov), meta_frame, FALSE, FALSE, 0);

    GtkWidget* prog_frame = gtk_frame_new(_("Readiness Progress"));
    GtkWidget* pbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(pbox), 6);
    priv->pbar_overall     = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    priv->pbar_legal       = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    priv->pbar_beneficiary = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    priv->pbar_tax         = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    priv->pbar_ice         = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_box_pack_start(GTK_BOX(pbox), GTK_WIDGET(priv->pbar_overall), FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(pbox), GTK_WIDGET(priv->pbar_legal), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pbox), GTK_WIDGET(priv->pbar_beneficiary), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pbox), GTK_WIDGET(priv->pbar_tax), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pbox), GTK_WIDGET(priv->pbar_ice), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(prog_frame), pbox);
    gtk_box_pack_start(GTK_BOX(tab_ov), prog_frame, FALSE, FALSE, 0);

    priv->tv_triggers = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(priv->tv_triggers, FALSE);
    gtk_text_view_set_wrap_mode(priv->tv_triggers, GTK_WRAP_WORD_CHAR);
    GtkWidget* trig_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(trig_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(trig_sw), 100);
    gtk_container_add(GTK_CONTAINER(trig_sw), GTK_WIDGET(priv->tv_triggers));
    gtk_box_pack_start(GTK_BOX(tab_ov), trig_sw, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ov), gtk_label_new(_("Overview & Triggers")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(build_checklist_section_tab(priv, SEC_LEGAL)), gtk_label_new(_("1. Legal Documents")));

    // Tab 2: Account Titling & Beneficiary Audit
    GtkWidget* tab_ben = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ben), 8);
    priv->lbl_ben_unmapped = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_line_wrap(priv->lbl_ben_unmapped, TRUE);
    gtk_label_set_xalign(priv->lbl_ben_unmapped, 0.0f);
    gtk_box_pack_start(GTK_BOX(tab_ben), GTK_WIDGET(priv->lbl_ben_unmapped), FALSE, FALSE, 0);

    priv->ben_store = gtk_list_store_new(9, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    priv->ben_tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->ben_store)));
    gtk_tree_view_set_grid_lines(priv->ben_tv, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    GtkCellRenderer* br_tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(br_tog, "toggled", G_CALLBACK(on_ben_toggle), priv);
    GtkTreeViewColumn* bc_ver = gtk_tree_view_column_new_with_attributes(_("Verified"), br_tog, "active", 1, nullptr);
    gtk_tree_view_column_set_sort_column_id(bc_ver, 1);
    gtk_tree_view_append_column(priv->ben_tv, bc_ver);
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(priv->ben_store), 3,
                                    cmp_currency_or_text, GINT_TO_POINTER(3), nullptr);
    auto add_ben_col = [&](const char* title, int col_id, int w) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(title, r, "text", col_id, "foreground", 8, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, w);
        gtk_tree_view_column_set_sort_column_id(c, col_id);
        gtk_tree_view_append_column(priv->ben_tv, c);
    };
    add_ben_col(_("Account / Asset"), 2, 180);
    add_ben_col(_("Approx. Value"), 3, 100);
    add_ben_col(_("Primary / Titling"), 4, 150);
    add_ben_col(_("Contingent / Structure"), 5, 190);
    add_ben_col(_("Mandatory Re-Check Trigger"), 6, 220);
    add_ben_col(_("Verified Date"), 7, 90);
    g_signal_connect(gtk_tree_view_get_selection(priv->ben_tv), "changed",
                     G_CALLBACK(on_ben_selection_changed), priv);

    GtkWidget* ben_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ben_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ben_sw), 150);
    gtk_container_add(GTK_CONTAINER(ben_sw), GTK_WIDGET(priv->ben_tv));
    gtk_box_pack_start(GTK_BOX(tab_ben), ben_sw, TRUE, TRUE, 0);

    GtkWidget* ben_frame = gtk_frame_new(_("Edit Selected Account / Asset Audit Row"));
    GtkWidget* bgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(bgrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(bgrid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(bgrid), 6);
    priv->entry_ben_acct       = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_val        = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_primary    = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_contingent = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_trigger    = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_date       = GTK_ENTRY(gtk_entry_new());
    priv->entry_ben_notes      = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ben_acct), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ben_trigger), TRUE);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Account / Asset")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_acct), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Approx. Value")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_val), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Primary / Titling")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_primary), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Contingent / Structure")), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_contingent), 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Re-Check Trigger")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_trigger), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Verified Date")), 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_date), 3, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Notes")), 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->entry_ben_notes), 1, 3, 3, 1);

    priv->combo_ben_gnc_acct = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(priv->combo_ben_gnc_acct, "(None / Manual)");
    for (const auto& ac : collect_gnucash_asset_accounts())
        gtk_combo_box_text_append_text(priv->combo_ben_gnc_acct, ac.full_name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ben_gnc_acct), 0);
    g_signal_connect(priv->combo_ben_gnc_acct, "changed", G_CALLBACK(on_ben_gnc_acct_changed), priv);

    priv->entry_ben_doc = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(priv->entry_ben_doc, _("Optional path to TOD/POD/Deed PDF"));
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ben_doc), TRUE);
    GtkWidget* bb_doc_br = gtk_button_new_with_label(_("Browse…"));
    GtkWidget* bb_doc_op = gtk_button_new_with_label(_("Open"));
    g_signal_connect(bb_doc_br, "clicked", G_CALLBACK(on_ben_browse_doc_clicked), priv);
    g_signal_connect(bb_doc_op, "clicked", G_CALLBACK(on_ben_open_doc_clicked), priv);
    GtkWidget* bdoc_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(bdoc_box), GTK_WIDGET(priv->entry_ben_doc), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bdoc_box), bb_doc_br, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bdoc_box), bb_doc_op, FALSE, FALSE, 0);

    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("GnuCash Account Link")), 0, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), GTK_WIDGET(priv->combo_ben_gnc_acct), 1, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), gtk_label_new(_("Document Link")), 2, 4, 1, 1);
    gtk_grid_attach(GTK_GRID(bgrid), bdoc_box, 3, 4, 1, 1);

    GtkWidget* ben_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* bb_upd = gtk_button_new_with_label(_("Update Row"));
    GtkWidget* bb_add = gtk_button_new_with_label(_("Add Asset / Account"));
    GtkWidget* bb_del = gtk_button_new_with_label(_("Delete Row"));
    GtkWidget* bb_sync = gtk_button_new_with_label(_("Sync Linked GnuCash Values"));
    g_signal_connect(bb_upd, "clicked", G_CALLBACK(on_ben_update_clicked), priv);
    g_signal_connect(bb_add, "clicked", G_CALLBACK(on_ben_add_clicked), priv);
    g_signal_connect(bb_del, "clicked", G_CALLBACK(on_ben_delete_clicked), priv);
    g_signal_connect(bb_sync, "clicked", G_CALLBACK(on_ben_sync_book_clicked), priv);
    gtk_box_pack_start(GTK_BOX(ben_btns), bb_upd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ben_btns), bb_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ben_btns), bb_del, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ben_btns), bb_sync, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(bgrid), ben_btns, 1, 5, 3, 1);
    gtk_container_add(GTK_CONTAINER(ben_frame), bgrid);
    gtk_box_pack_start(GTK_BOX(tab_ben), ben_frame, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ben), gtk_label_new(_("2. Beneficiary Audit")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(build_checklist_section_tab(priv, SEC_TAX)), gtk_label_new(_("3. Tax Continuity")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(build_checklist_section_tab(priv, SEC_ICE)), gtk_label_new(_("4. ICE Checklist")));

    // Tab 5: ICE & Digital Directory
    GtkWidget* tab_ice = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ice), 8);
    priv->ice_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_BOOLEAN, G_TYPE_STRING,
                                         G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    priv->ice_tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->ice_store)));
    gtk_tree_view_set_grid_lines(priv->ice_tv, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    GtkCellRenderer* ir_tog = gtk_cell_renderer_toggle_new();
    g_signal_connect(ir_tog, "toggled", G_CALLBACK(on_ice_toggle), priv);
    GtkTreeViewColumn* ic_ver = gtk_tree_view_column_new_with_attributes(_("Verified"), ir_tog, "active", 1, nullptr);
    gtk_tree_view_column_set_sort_column_id(ic_ver, 1);
    gtk_tree_view_append_column(priv->ice_tv, ic_ver);
    auto add_ice_col = [&](const char* title, int col_id, int w) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(title, r, "text", col_id, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, w);
        gtk_tree_view_column_set_sort_column_id(c, col_id);
        gtk_tree_view_append_column(priv->ice_tv, c);
    };
    add_ice_col(_("Category"), 2, 100);
    add_ice_col(_("Institution / Service"), 3, 190);
    add_ice_col(_("Account / Policy #"), 4, 180);
    add_ice_col(_("Recovery / Safe Location"), 5, 180);
    add_ice_col(_("Notes"), 6, 200);
    g_signal_connect(gtk_tree_view_get_selection(priv->ice_tv), "changed",
                     G_CALLBACK(on_ice_selection_changed), priv);

    GtkWidget* ice_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ice_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ice_sw), 150);
    gtk_container_add(GTK_CONTAINER(ice_sw), GTK_WIDGET(priv->ice_tv));
    gtk_box_pack_start(GTK_BOX(tab_ice), ice_sw, TRUE, TRUE, 0);

    GtkWidget* ice_frame = gtk_frame_new(_("ICE Directory Entry (Do Not Store Raw Passwords Here — Reference Safe / Vault)"));
    GtkWidget* igrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(igrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(igrid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(igrid), 6);
    priv->combo_ice_cat = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    for (const char* c : {"Financial", "Insurance", "Property/Tax", "Password/2FA", "Email/Domain", "Auto-Pay Bill"})
        gtk_combo_box_text_append_text(priv->combo_ice_cat, c);
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ice_cat), 0);
    priv->entry_ice_inst     = GTK_ENTRY(gtk_entry_new());
    priv->entry_ice_acct     = GTK_ENTRY(gtk_entry_new());
    priv->entry_ice_recovery = GTK_ENTRY(gtk_entry_new());
    priv->entry_ice_notes    = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ice_inst), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ice_notes), TRUE);
    gtk_grid_attach(GTK_GRID(igrid), gtk_label_new(_("Category")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), GTK_WIDGET(priv->combo_ice_cat), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), gtk_label_new(_("Institution / Service")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), GTK_WIDGET(priv->entry_ice_inst), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), gtk_label_new(_("Account / Policy #")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), GTK_WIDGET(priv->entry_ice_acct), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), gtk_label_new(_("Recovery / Safe Location")), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), GTK_WIDGET(priv->entry_ice_recovery), 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), gtk_label_new(_("Notes")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(igrid), GTK_WIDGET(priv->entry_ice_notes), 1, 2, 3, 1);

    GtkWidget* ice_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* ib_upd = gtk_button_new_with_label(_("Update Entry"));
    GtkWidget* ib_add = gtk_button_new_with_label(_("Add Entry"));
    GtkWidget* ib_del = gtk_button_new_with_label(_("Delete Entry"));
    g_signal_connect(ib_upd, "clicked", G_CALLBACK(on_ice_update_clicked), priv);
    g_signal_connect(ib_add, "clicked", G_CALLBACK(on_ice_add_clicked), priv);
    g_signal_connect(ib_del, "clicked", G_CALLBACK(on_ice_delete_clicked), priv);
    gtk_box_pack_start(GTK_BOX(ice_btns), ib_upd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ice_btns), ib_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ice_btns), ib_del, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(igrid), ice_btns, 1, 3, 3, 1);
    gtk_container_add(GTK_CONTAINER(ice_frame), igrid);
    gtk_box_pack_start(GTK_BOX(tab_ice), ice_frame, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ice), gtk_label_new(_("5. ICE Directory")));

    // Tab 6: Fiduciary & Key Contacts Roster
    GtkWidget* tab_ct = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ct), 8);
    priv->contact_store = gtk_list_store_new(7, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    priv->contact_tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(priv->contact_store)));
    gtk_tree_view_set_grid_lines(priv->contact_tv, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);
    auto add_ct_col = [&](const char* title, int col_id, int w) {
        GtkCellRenderer* r = gtk_cell_renderer_text_new();
        g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn* c = gtk_tree_view_column_new_with_attributes(title, r, "text", col_id, nullptr);
        gtk_tree_view_column_set_resizable(c, TRUE);
        gtk_tree_view_column_set_min_width(c, w);
        gtk_tree_view_column_set_sort_column_id(c, col_id);
        gtk_tree_view_append_column(priv->contact_tv, c);
    };
    add_ct_col(_("Fiduciary / Role"), 1, 200);
    add_ct_col(_("Name"), 2, 160);
    add_ct_col(_("Firm / Organization"), 3, 160);
    add_ct_col(_("Phone"), 4, 120);
    add_ct_col(_("Email"), 5, 160);
    add_ct_col(_("Notes"), 6, 180);
    g_signal_connect(gtk_tree_view_get_selection(priv->contact_tv), "changed",
                     G_CALLBACK(on_contact_selection_changed), priv);
    GtkWidget* ct_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ct_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ct_sw), 150);
    gtk_container_add(GTK_CONTAINER(ct_sw), GTK_WIDGET(priv->contact_tv));
    gtk_box_pack_start(GTK_BOX(tab_ct), ct_sw, TRUE, TRUE, 0);

    GtkWidget* ct_frame = gtk_frame_new(_("Edit Contact / Fiduciary"));
    GtkWidget* cgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(cgrid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(cgrid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(cgrid), 6);
    priv->entry_ct_role  = GTK_ENTRY(gtk_entry_new());
    priv->entry_ct_name  = GTK_ENTRY(gtk_entry_new());
    priv->entry_ct_firm  = GTK_ENTRY(gtk_entry_new());
    priv->entry_ct_phone = GTK_ENTRY(gtk_entry_new());
    priv->entry_ct_email = GTK_ENTRY(gtk_entry_new());
    priv->entry_ct_notes = GTK_ENTRY(gtk_entry_new());
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ct_role), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ct_name), TRUE);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Role")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_role), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Name")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_name), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Firm / Org")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_firm), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Phone")), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_phone), 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Email")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_email), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), gtk_label_new(_("Notes")), 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(cgrid), GTK_WIDGET(priv->entry_ct_notes), 3, 2, 1, 1);
    GtkWidget* ct_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* cb_upd = gtk_button_new_with_label(_("Update Contact"));
    GtkWidget* cb_add = gtk_button_new_with_label(_("Add Contact"));
    GtkWidget* cb_del = gtk_button_new_with_label(_("Delete Contact"));
    g_signal_connect(cb_upd, "clicked", G_CALLBACK(on_contact_update_clicked), priv);
    g_signal_connect(cb_add, "clicked", G_CALLBACK(on_contact_add_clicked), priv);
    g_signal_connect(cb_del, "clicked", G_CALLBACK(on_contact_delete_clicked), priv);
    gtk_box_pack_start(GTK_BOX(ct_btns), cb_upd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ct_btns), cb_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ct_btns), cb_del, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(cgrid), ct_btns, 1, 3, 3, 1);
    gtk_container_add(GTK_CONTAINER(ct_frame), cgrid);
    gtk_box_pack_start(GTK_BOX(tab_ct), ct_frame, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ct), gtk_label_new(_("6. Contacts & Fiduciaries")));

    // Tab 7: Heir 10-Yr Tax Estimator (SECURE Act)
    GtkWidget* tab_ht = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ht), 8);
    GtkWidget* ht_frame = gtk_frame_new(_("SECURE Act 10-Year Inherited Account Tax Estimator"));
    GtkWidget* hgrid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(hgrid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(hgrid), 10);
    gtk_container_set_border_width(GTK_CONTAINER(hgrid), 8);

    auto make_spin = [&](double val, double lo, double hi, double step) {
        GtkSpinButton* s = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(lo, hi, step));
        gtk_spin_button_set_value(s, val);
        g_signal_connect(s, "value-changed", G_CALLBACK(on_ht_changed), priv);
        return s;
    };
    priv->spin_ht_trad   = make_spin(1729711.0, 0.0, 50000000.0, 10000.0);
    priv->spin_ht_roth   = make_spin(54100.0, 0.0, 50000000.0, 5000.0);
    priv->spin_ht_heirs  = make_spin(2.0, 1.0, 20.0, 1.0);
    gtk_spin_button_set_digits(priv->spin_ht_heirs, 0);
    priv->spin_ht_rate   = make_spin(24.0, 0.0, 60.0, 1.0);
    priv->spin_ht_growth = make_spin(6.0, 0.0, 15.0, 0.5);
    priv->combo_ht_strategy = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(priv->combo_ht_strategy, _("Even 10-Year Spread"));
    gtk_combo_box_text_append_text(priv->combo_ht_strategy, _("Year-10 Lump Sum (Max Deferral)"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ht_strategy), 0);
    g_signal_connect(priv->combo_ht_strategy, "changed", G_CALLBACK(on_ht_changed), priv);

    GtkWidget* btn_ht_pull = gtk_button_new_with_label(_("Pull IRA/401k/Roth from GnuCash Book"));
    GtkWidget* btn_ht_owl  = gtk_button_new_with_label(_("Pull from Owl Retirement Planner"));
    g_signal_connect(btn_ht_pull, "clicked", G_CALLBACK(on_ht_pull_book_clicked), priv);
    g_signal_connect(btn_ht_owl, "clicked", G_CALLBACK(on_ht_pull_planner_clicked), priv);
    GtkWidget* ht_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(ht_btns), btn_ht_pull, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ht_btns), btn_ht_owl, TRUE, TRUE, 0);

    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("Inherited Tax-Deferred ($)")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->spin_ht_trad), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("Inherited Roth ($)")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->spin_ht_roth), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("Number of Heirs")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->spin_ht_heirs), 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("Heir Marginal Tax Rate (%)")), 2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->spin_ht_rate), 3, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("10-Yr Growth Rate (%/yr)")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->spin_ht_growth), 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), gtk_label_new(_("Distribution Pattern")), 2, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), GTK_WIDGET(priv->combo_ht_strategy), 3, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(hgrid), ht_btns, 0, 3, 4, 1);
    gtk_container_add(GTK_CONTAINER(ht_frame), hgrid);
    gtk_box_pack_start(GTK_BOX(tab_ht), ht_frame, FALSE, FALSE, 0);

    priv->tv_ht_report = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(priv->tv_ht_report, FALSE);
    gtk_text_view_set_monospace(priv->tv_ht_report, TRUE);
    GtkWidget* ht_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ht_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ht_sw), 140);
    gtk_container_add(GTK_CONTAINER(ht_sw), GTK_WIDGET(priv->tv_ht_report));
    gtk_box_pack_start(GTK_BOX(tab_ht), ht_sw, TRUE, TRUE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ht), gtk_label_new(_("7. Heir 10-Yr Tax")));

    // Tab 8: Ask Estate AI
    GtkWidget* tab_ai = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(tab_ai), 8);

    GtkWidget* ai_cfg = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(ai_cfg), 4);
    gtk_grid_set_column_spacing(GTK_GRID(ai_cfg), 8);
    priv->combo_ai_provider = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("Local Ollama (Offline)"));
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("Google Gemini"));
    gtk_combo_box_text_append_text(priv->combo_ai_provider, _("OpenAI-Compatible"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo_ai_provider), 0);
    g_signal_connect(priv->combo_ai_provider, "changed", G_CALLBACK(on_estate_ai_provider_changed), priv);

    priv->combo_ai_model = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new_with_entry());
    priv->entry_ai_model = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(priv->combo_ai_model)));
    populate_estate_ai_models(priv, {"llama3.2", "qwen2.5", "mistral", "gemma2"}, "llama3.2");
    g_signal_connect(priv->entry_ai_model, "changed", G_CALLBACK(on_estate_ai_setting_changed), priv);
    GtkWidget* btn_ai_ref = gtk_button_new_with_label(_("Refresh Models"));
    g_signal_connect(btn_ai_ref, "clicked", G_CALLBACK(on_estate_ai_refresh_models), priv);

    priv->entry_ai_endpoint = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_text(priv->entry_ai_endpoint, "http://localhost:11434/api/chat");
    gtk_widget_set_hexpand(GTK_WIDGET(priv->entry_ai_endpoint), TRUE);
    g_signal_connect(priv->entry_ai_endpoint, "changed", G_CALLBACK(on_estate_ai_setting_changed), priv);

    priv->entry_ai_key = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_visibility(priv->entry_ai_key, FALSE);
    gtk_entry_set_placeholder_text(priv->entry_ai_key, _("Shared with Retirement Planner; or set GEMINI_API_KEY / OPENAI_API_KEY"));
    g_signal_connect(priv->entry_ai_key, "changed", G_CALLBACK(on_estate_ai_setting_changed), priv);

    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Provider")), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->combo_ai_provider), 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Model")), 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->combo_ai_model), 3, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), btn_ai_ref, 4, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("Endpoint")), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->entry_ai_endpoint), 1, 1, 4, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), gtk_label_new(_("API Key")), 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(ai_cfg), GTK_WIDGET(priv->entry_ai_key), 1, 2, 4, 1);
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_cfg, FALSE, FALSE, 0);

    GtkWidget* sys_exp = gtk_expander_new(_("Estate AI System Prompt (Editable)"));
    gtk_expander_set_expanded(GTK_EXPANDER(sys_exp), FALSE);
    priv->tv_ai_system = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(priv->tv_ai_system, GTK_WRAP_WORD_CHAR);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(priv->tv_ai_system), estate_ai_default_system_prompt(), -1);
    GtkWidget* sys_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sys_sw), 90);
    gtk_container_add(GTK_CONTAINER(sys_sw), GTK_WIDGET(priv->tv_ai_system));
    gtk_container_add(GTK_CONTAINER(sys_exp), sys_sw);
    gtk_box_pack_start(GTK_BOX(tab_ai), sys_exp, FALSE, FALSE, 0);

    GtkWidget* ai_qbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    priv->entry_ai_question = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(priv->entry_ai_question, _("Ask about your estate checklist, probate, beneficiaries, or 10-yr inherited IRA tax..."));
    g_signal_connect(priv->entry_ai_question, "activate", G_CALLBACK(on_estate_ai_ask), priv);
    GtkWidget* b_ask = gtk_button_new_with_label(_("Ask"));
    priv->btn_ai_stop = gtk_button_new_with_label(_("Stop"));
    gtk_widget_set_sensitive(priv->btn_ai_stop, FALSE);
    GtkWidget* b_gaps = gtk_button_new_with_label(_("Audit Estate Gaps"));
    GtkWidget* b_st   = gtk_button_new_with_label(_("State & Probate Risks"));
    GtkWidget* b_ht   = gtk_button_new_with_label(_("Heir 10-Yr Tax"));
    GtkWidget* b_cp   = gtk_button_new_with_label(_("Copy Summary"));
    g_signal_connect(b_ask, "clicked", G_CALLBACK(on_estate_ai_ask), priv);
    g_signal_connect(priv->btn_ai_stop, "clicked", G_CALLBACK(on_estate_ai_stop), priv);
    g_signal_connect(b_gaps, "clicked", G_CALLBACK(on_estate_ai_audit_gaps), priv);
    g_signal_connect(b_st, "clicked", G_CALLBACK(on_estate_ai_state_risks), priv);
    g_signal_connect(b_ht, "clicked", G_CALLBACK(on_estate_ai_heir_tax), priv);
    g_signal_connect(b_cp, "clicked", G_CALLBACK(on_estate_ai_copy), priv);
    gtk_box_pack_start(GTK_BOX(ai_qbar), GTK_WIDGET(priv->entry_ai_question), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), b_ask, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), priv->btn_ai_stop, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), b_gaps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), b_st, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), b_ht, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ai_qbar), b_cp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_qbar, FALSE, FALSE, 0);

    priv->lbl_ai_status = GTK_LABEL(gtk_label_new(_("Shares your saved AI settings from Ask Owl AI. Privacy: account numbers, safe locations & contacts are excluded.")));
    gtk_label_set_xalign(priv->lbl_ai_status, 0.0f);
    gtk_box_pack_start(GTK_BOX(tab_ai), GTK_WIDGET(priv->lbl_ai_status), FALSE, FALSE, 0);

    priv->tv_ai_response = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(priv->tv_ai_response, FALSE);
    gtk_text_view_set_wrap_mode(priv->tv_ai_response, GTK_WRAP_WORD_CHAR);
    GtkWidget* ai_sw = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ai_sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(ai_sw), 160);
    gtk_container_add(GTK_CONTAINER(ai_sw), GTK_WIDGET(priv->tv_ai_response));
    gtk_box_pack_start(GTK_BOX(tab_ai), ai_sw, TRUE, TRUE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), wrap_in_scrolled(tab_ai), gtk_label_new(_("8. Ask Estate AI")));

    gtk_box_pack_start(GTK_BOX(root), nb, TRUE, TRUE, 0);
    gtk_widget_show_all(root);

    load_estate_state(priv);
    load_shared_ai_config(priv);
    return root;
}

static void
gnc_plugin_page_estate_save_page(GncPluginPage* plugin_page, GKeyFile*, const gchar*)
{
    auto page = GNC_PLUGIN_PAGE_ESTATE(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(page);
    save_estate_state(priv, false);
}

static GncPluginPage*
gnc_plugin_page_estate_recreate_page(GtkWidget*, GKeyFile*, const gchar*)
{
    return gnc_plugin_page_estate_new();
}

static void
gnc_plugin_page_estate_destroy_widget(GncPluginPage* plugin_page)
{
    auto page = GNC_PLUGIN_PAGE_ESTATE(plugin_page);
    auto priv = GNC_PLUGIN_PAGE_ESTATE_GET_PRIVATE(page);
    if (priv->root_widget)
    {
        save_estate_state(priv, true);
        if (priv->ai_cancel)
        {
            g_cancellable_cancel(priv->ai_cancel);
            g_object_unref(priv->ai_cancel);
            priv->ai_cancel = nullptr;
        }
        if (priv->soup_session)
        {
            g_object_unref(priv->soup_session);
            priv->soup_session = nullptr;
        }
        for (int s = 0; s < 3; ++s)
        {
            if (priv->sec_tabs[s].store)
                g_object_unref(priv->sec_tabs[s].store);
            priv->sec_tabs[s].store = nullptr;
            priv->sec_tabs[s].tv = nullptr;
        }
        if (priv->ben_store) g_object_unref(priv->ben_store);
        if (priv->ice_store) g_object_unref(priv->ice_store);
        if (priv->contact_store) g_object_unref(priv->contact_store);
        priv->ben_store = nullptr;
        priv->ice_store = nullptr;
        priv->contact_store = nullptr;
        priv->root_widget = nullptr;
    }
}

extern "C" GncPluginPage*
gnc_plugin_page_estate_new(void)
{
    auto page = GNC_PLUGIN_PAGE_ESTATE(g_object_new(GNC_TYPE_PLUGIN_PAGE_ESTATE, nullptr));
    g_object_set(G_OBJECT(page), "page-name", _("Estate Planning & Continuity"), nullptr);
    return GNC_PLUGIN_PAGE(page);
}
