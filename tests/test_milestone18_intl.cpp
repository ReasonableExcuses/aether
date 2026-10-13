#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"
#include "aether/intl.h"

#include <iostream>
#ifdef _WIN32
#include <crtdbg.h>
#endif

using namespace aether;

void Test1_GetCanonicalLocales() {
    std::cout << "[Test 1] Verifying Intl.getCanonicalLocales validation and canonicalization...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let canon1 = Intl.getCanonicalLocales("EN-us");
        let valid1 = (canon1.length === 1 && canon1[0] === "en-US");

        let canon2 = Intl.getCanonicalLocales(["ZH-hAns-cn", "ja-jp", "zh-hans-CN"]);
        let valid2 = (canon2.length === 2 && canon2[0] === "zh-Hans-CN" && canon2[1] === "ja-JP");

        let caught = false;
        try {
            Intl.getCanonicalLocales("en_US"); // invalid underscore
        } catch (e) {
            caught = true;
        }

        let caught2 = false;
        try {
            Intl.getCanonicalLocales("-invalid-");
        } catch (e) {
            caught2 = true;
        }

        valid1 && valid2 && caught && caught2;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.getCanonicalLocales verified.\n" << std::flush;
}

void Test2_SupportedValuesOf() {
    std::cout << "[Test 2] Verifying Intl.supportedValuesOf for all standard categories...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let cal = Intl.supportedValuesOf("calendar");
        let hasGreg = cal.indexOf("gregory") !== -1;

        let curr = Intl.supportedValuesOf("currency");
        let hasUSD = curr.indexOf("USD") !== -1 && curr.indexOf("EUR") !== -1;

        let num = Intl.supportedValuesOf("numberingSystem");
        let hasLatn = num.indexOf("latn") !== -1;

        let tz = Intl.supportedValuesOf("timeZone");
        let hasUTC = tz.indexOf("UTC") !== -1;

        let units = Intl.supportedValuesOf("unit");
        let hasMeter = units.indexOf("meter") !== -1;

        let coll = Intl.supportedValuesOf("collation");
        let hasEmoji = coll.indexOf("emoji") !== -1;

        let caught = false;
        try {
            Intl.supportedValuesOf("invalidKey");
        } catch (e) {
            caught = true;
        }

        hasGreg && hasUSD && hasLatn && hasUTC && hasMeter && hasEmoji && caught;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.supportedValuesOf verified.\n" << std::flush;
}

void Test3_NumberFormat_DecimalAndCurrency() {
    std::cout << "[Test 3] Verifying Intl.NumberFormat decimal and currency formatting...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let nf_us = new Intl.NumberFormat("en-US");
        let str_us = nf_us.format(1234567.89);

        let nf_de = new Intl.NumberFormat("de-DE");
        let str_de = nf_de.format(1234567.89);

        let nf_curr_us = new Intl.NumberFormat("en-US", { style: "currency", currency: "USD" });
        let str_curr_us = nf_curr_us.format(1234.5);

        let nf_curr_de = new Intl.NumberFormat("de-DE", { style: "currency", currency: "EUR" });
        let str_curr_de = nf_curr_de.format(1234.5);

        let opts = nf_curr_us.resolvedOptions();
        let valid_opts = (opts.locale === "en-US" && opts.style === "currency" && opts.currency === "USD");

        // Verify standard separators
        let check_us = (str_us.indexOf(",") !== -1 && str_us.indexOf(".") !== -1);
        let check_de = (str_de.indexOf(".") !== -1 && str_de.indexOf(",") !== -1);
        let check_curr_us = (str_curr_us.indexOf("$") !== -1);
        let check_curr_de = (str_curr_de.indexOf("€") !== -1);

        valid_opts && check_us && check_de && check_curr_us && check_curr_de;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.NumberFormat decimal & currency verified.\n" << std::flush;
}

void Test4_NumberFormat_PercentAndFormatToParts() {
    std::cout << "[Test 4] Verifying Intl.NumberFormat percent style and formatToParts...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let nf_pct = new Intl.NumberFormat("en-US", { style: "percent" });
        let pct_str = nf_pct.format(0.75);
        let is_pct = (pct_str.indexOf("75%") !== -1);

        let nf_parts = new Intl.NumberFormat("en-US");
        let parts = nf_parts.formatToParts(1234.56);
        let has_integer = false;
        let has_group = false;
        let has_decimal = false;

        for (let i = 0; i < parts.length; i++) {
            if (parts[i].type === "integer") has_integer = true;
            if (parts[i].type === "group") has_group = true;
            if (parts[i].type === "decimal") has_decimal = true;
        }

        is_pct && has_integer && has_group && has_decimal;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.NumberFormat percent & formatToParts verified.\n" << std::flush;
}

void Test5_DateTimeFormat_FormattingAndParts() {
    std::cout << "[Test 5] Verifying Intl.DateTimeFormat date/time formatting and formatToParts...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        // Timestamp for 2024-12-25T12:00:00Z = 1735128000000
        let ts = 1735128000000;
        let dtf_us = new Intl.DateTimeFormat("en-US", { dateStyle: "short" });
        let str_us = dtf_us.format(ts);

        let dtf_de = new Intl.DateTimeFormat("de-DE", { dateStyle: "short" });
        let str_de = dtf_de.format(ts);

        let check_us = (str_us.indexOf("12") !== -1 && str_us.indexOf("25") !== -1 && str_us.indexOf("2024") !== -1);
        let check_de = (str_de.indexOf("25") !== -1 && str_de.indexOf("12") !== -1 && str_de.indexOf("2024") !== -1);

        let parts = dtf_us.formatToParts(ts);
        let has_month = false;
        let has_day = false;
        let has_year = false;
        for (let i = 0; i < parts.length; i++) {
            if (parts[i].type === "month") has_month = true;
            if (parts[i].type === "day") has_day = true;
            if (parts[i].type === "year") has_year = true;
        }

        check_us && check_de && has_month && has_day && has_year;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.DateTimeFormat verified.\n" << std::flush;
}

void Test6_Collator_StringComparisonAndNaturalSorting() {
    std::cout << "[Test 6] Verifying Intl.Collator locale string comparison and numeric sorting...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let col = new Intl.Collator("en-US");
        let cmp1 = col.compare("apple", "banana");
        let cmp2 = col.compare("banana", "apple");
        let cmp3 = col.compare("apple", "apple");

        let is_basic_ok = (cmp1 === -1 && cmp2 === 1 && cmp3 === 0);

        // Natural numeric sort: file2 < file10
        let col_num = new Intl.Collator("en-US", { numeric: true });
        let cmp_num = col_num.compare("file2", "file10");
        let is_num_ok = (cmp_num === -1);

        // Standard lexicographic sort: "file10" < "file2"
        let col_lex = new Intl.Collator("en-US", { numeric: false });
        let cmp_lex = col_lex.compare("file10", "file2");
        let is_lex_ok = (cmp_lex === -1);

        is_basic_ok && is_num_ok && is_lex_ok;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.Collator comparison & numeric sorting verified.\n" << std::flush;
}

void Test7_PluralRules() {
    std::cout << "[Test 7] Verifying Intl.PluralRules cardinal and ordinal selections...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let pr_card = new Intl.PluralRules("en-US");
        let sel_one = pr_card.select(1);
        let sel_other = pr_card.select(5);

        let pr_ord = new Intl.PluralRules("en-US", { type: "ordinal" });
        let sel_1st = pr_ord.select(1);
        let sel_2nd = pr_ord.select(2);
        let sel_3rd = pr_ord.select(3);
        let sel_4th = pr_ord.select(4);

        let card_ok = (sel_one === "one" && sel_other === "other");
        let ord_ok = (sel_1st === "one" && sel_2nd === "two" && sel_3rd === "few" && sel_4th === "other");

        card_ok && ord_ok;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.PluralRules cardinal & ordinal verified.\n" << std::flush;
}

void Test8_ListFormatAndRelativeTimeFormat() {
    std::cout << "[Test 8] Verifying Intl.ListFormat and Intl.RelativeTimeFormat...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let lf = new Intl.ListFormat("en-US", { type: "conjunction" });
        let lf_str2 = lf.format(["Apple", "Banana"]);
        let lf_str3 = lf.format(["Apple", "Banana", "Orange"]);

        let lf_ok = (lf_str2 === "Apple and Banana" && lf_str3 === "Apple, Banana, and Orange");

        let rtf = new Intl.RelativeTimeFormat("en-US", { numeric: "auto" });
        let rtf_yesterday = rtf.format(-1, "day");
        let rtf_tomorrow = rtf.format(1, "day");

        let rtf_always = new Intl.RelativeTimeFormat("en-US", { numeric: "always" });
        let rtf_day_ago = rtf_always.format(-1, "day");
        let rtf_in_day = rtf_always.format(1, "day");

        let rtf_ok = (rtf_yesterday === "yesterday" && rtf_tomorrow === "tomorrow" &&
                      rtf_day_ago === "1 day ago" && rtf_in_day === "in 1 day");

        lf_ok && rtf_ok;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Intl.ListFormat & Intl.RelativeTimeFormat verified.\n" << std::flush;
}

void Test9_NumberAndStringPrototypeIntegration() {
    std::cout << "[Test 9] Verifying Number.prototype.toLocaleString and String.prototype.localeCompare...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        let num = 1234567.89;
        let num_str = num.toLocaleString("en-US");
        let num_ok = (num_str.indexOf(",") !== -1 && num_str.indexOf(".") !== -1);

        let cmp = "apple".localeCompare("banana");
        let str_ok = (cmp === -1);

        let is_nan_check = Number.isNaN(NaN) && !Number.isNaN(123);
        let is_fin_check = Number.isFinite(123) && !Number.isFinite(Infinity);
        let is_int_check = Number.isInteger(42) && !Number.isInteger(42.5);

        num_ok && str_ok && is_nan_check && is_fin_check && is_int_check;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Number & String prototype integration verified.\n" << std::flush;
}

void Test10_DateBuiltinAndLocalization() {
    std::cout << "[Test 10] Verifying Date constructor, ISO string, and toLocaleDateString...\n" << std::flush;

    Agent agent;
    std::string script = R"(
        // Timestamp 1735128000000 = 2024-12-25T12:00:00.000Z
        let d = new Date(1735128000000);
        let t = d.getTime();
        let time_ok = (t === 1735128000000);

        let iso = d.toISOString();
        let iso_ok = (iso === "2024-12-25T12:00:00.000Z");

        let y = d.getFullYear();
        let m = d.getMonth(); // 11 for December (0-indexed)
        let date_num = d.getDate(); // 25
        let get_ok = (y === 2024 && m === 11 && date_num === 25);

        let loc_date = d.toLocaleDateString("en-US");
        let loc_ok = (loc_date.indexOf("12") !== -1 && loc_date.indexOf("25") !== -1 && loc_date.indexOf("2024") !== -1);

        let now_ms = Date.now();
        let now_ok = (now_ms > 0);

        time_ok && iso_ok && get_ok && loc_ok && now_ok;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());

    std::cout << "  -> Date built-in & localization verified.\n" << std::flush;
}

int main() {
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "=================================================================\n";
    std::cout << "Running Aether Milestone 18 Test Suite: Internationalization (Intl)\n";
    std::cout << "=================================================================\n" << std::flush;

    Test1_GetCanonicalLocales();
    Test2_SupportedValuesOf();
    Test3_NumberFormat_DecimalAndCurrency();
    Test4_NumberFormat_PercentAndFormatToParts();
    Test5_DateTimeFormat_FormattingAndParts();
    Test6_Collator_StringComparisonAndNaturalSorting();
    Test7_PluralRules();
    Test8_ListFormatAndRelativeTimeFormat();
    Test9_NumberAndStringPrototypeIntegration();
    Test10_DateBuiltinAndLocalization();

    std::cout << "=================================================================\n";
    std::cout << "All Milestone 18 Internationalization (Intl) Tests Passed (10/10)!\n";
    std::cout << "=================================================================\n" << std::flush;
    return 0;
}
