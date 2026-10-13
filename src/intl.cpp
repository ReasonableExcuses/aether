#include "aether/intl.h"
#include "aether/runtime.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace aether {

// ============================================================================
// BCP 47 Language Tag Parsing and Canonicalization (RFC 5646 & ECMA-402 §6.2.2)
// ============================================================================

static bool IsAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool IsAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool IsAsciiAlphanum(char c) {
    return IsAsciiAlpha(c) || IsAsciiDigit(c);
}

static std::string ToLowerAscii(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) res.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return res;
}

static std::string ToUpperAscii(std::string_view s) {
    std::string res;
    res.reserve(s.size());
    for (char c : s) res.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    return res;
}

static std::string ToTitleAscii(std::string_view s) {
    if (s.empty()) return "";
    std::string res = ToLowerAscii(s);
    res[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(res[0])));
    return res;
}

BCP47Locale BCP47Locale::Parse(std::string_view tag) {
    BCP47Locale result;
    if (tag.empty()) {
        result.is_valid = false;
        return result;
    }

    // Split by '-'
    std::vector<std::string> subtags;
    size_t start = 0;
    while (start < tag.size()) {
        size_t next = tag.find('-', start);
        if (next == std::string::npos) next = tag.size();
        if (next == start) {
            // Leading, trailing, or double hyphen -> invalid!
            result.is_valid = false;
            return result;
        }
        std::string sub(tag.substr(start, next - start));
        // Check alphanumeric only
        for (char c : sub) {
            if (!IsAsciiAlphanum(c)) {
                result.is_valid = false;
                return result;
            }
        }
        subtags.push_back(std::move(sub));
        start = next + 1;
    }

    if (subtags.empty()) {
        result.is_valid = false;
        return result;
    }

    // 1. Language subtag: 2-3 letters (or 4 for reserved, or 5-8)
    std::string first = subtags[0];
    if (first.size() < 2 || first.size() > 8) {
        result.is_valid = false;
        return result;
    }
    for (char c : first) {
        if (!IsAsciiAlpha(c)) {
            result.is_valid = false;
            return result;
        }
    }
    result.language = ToLowerAscii(first);

    size_t idx = 1;

    // 2. Script subtag: 4 letters
    if (idx < subtags.size() && subtags[idx].size() == 4) {
        bool all_alpha = true;
        for (char c : subtags[idx]) {
            if (!IsAsciiAlpha(c)) { all_alpha = false; break; }
        }
        if (all_alpha) {
            result.script = ToTitleAscii(subtags[idx]);
            idx++;
        }
    }

    // 3. Region subtag: 2 letters or 3 digits
    if (idx < subtags.size()) {
        if (subtags[idx].size() == 2) {
            bool all_alpha = true;
            for (char c : subtags[idx]) {
                if (!IsAsciiAlpha(c)) { all_alpha = false; break; }
            }
            if (all_alpha) {
                result.region = ToUpperAscii(subtags[idx]);
                idx++;
            }
        } else if (subtags[idx].size() == 3) {
            bool all_digits = true;
            for (char c : subtags[idx]) {
                if (!IsAsciiDigit(c)) { all_digits = false; break; }
            }
            if (all_digits) {
                result.region = subtags[idx];
                idx++;
            }
        }
    }

    // 4. Variants: 5-8 chars or 4 chars starting with digit
    while (idx < subtags.size()) {
        const auto& s = subtags[idx];
        if (s.size() == 1) break; // Start of extension or privateuse
        if ((s.size() >= 5 && s.size() <= 8) || (s.size() == 4 && IsAsciiDigit(s[0]))) {
            result.variants.push_back(ToLowerAscii(s));
            idx++;
        } else {
            break;
        }
    }

    // 5. Extensions and Private Use
    while (idx < subtags.size()) {
        const auto& s = subtags[idx];
        if (s.size() != 1) {
            result.is_valid = false;
            return result;
        }
        char singleton = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
        idx++;
        if (singleton == 'x') {
            // Private use
            std::string priv;
            while (idx < subtags.size() && subtags[idx].size() >= 1 && subtags[idx].size() <= 8) {
                if (!priv.empty()) priv += "-";
                priv += ToLowerAscii(subtags[idx]);
                idx++;
            }
            result.privateuse = priv;
            break;
        } else if (singleton == 'u') {
            // Unicode locale extension
            while (idx < subtags.size() && subtags[idx].size() >= 2) {
                std::string key = ToLowerAscii(subtags[idx++]);
                std::string val = "";
                if (idx < subtags.size() && subtags[idx].size() >= 3) {
                    val = ToLowerAscii(subtags[idx++]);
                }
                result.extensions[key] = val;
            }
        } else {
            // Other extension
            while (idx < subtags.size() && subtags[idx].size() >= 2) {
                idx++;
            }
        }
    }

    // Build canonical tag
    std::string canon = result.language;
    if (!result.script.empty()) {
        canon += "-" + result.script;
    }
    if (!result.region.empty()) {
        canon += "-" + result.region;
    }
    for (const auto& v : result.variants) {
        canon += "-" + v;
    }
    if (!result.extensions.empty()) {
        canon += "-u";
        for (const auto& [k, v] : result.extensions) {
            canon += "-" + k;
            if (!v.empty()) canon += "-" + v;
        }
    }
    if (!result.privateuse.empty()) {
        canon += "-x-" + result.privateuse;
    }

    result.canonical_tag = canon;
    result.is_valid = true;
    return result;
}

std::string BCP47Locale::Canonicalize(std::string_view tag) {
    auto parsed = Parse(tag);
    if (!parsed.is_valid) return "";
    return parsed.canonical_tag;
}

bool BCP47Locale::IsValid(std::string_view tag) {
    return Parse(tag).is_valid;
}

// ============================================================================
// Supported Values of Key (ECMA-402 §19.2.2)
// ============================================================================

static const std::vector<std::string>& GetSupportedCalendars() {
    static const std::vector<std::string> list = {
        "buddhist", "chinese", "coptic", "dangi", "ethioaa", "ethiopic", "gregory",
        "hebrew", "indian", "islamic", "islamic-civil", "islamic-rgsa", "islamic-tbla",
        "islamic-umalqura", "iso8601", "japanese", "persian", "roc"
    };
    return list;
}

static const std::vector<std::string>& GetSupportedCollations() {
    static const std::vector<std::string> list = {
        "compat", "dict", "ducet", "emoji", "eor", "phonebk", "phonetic",
        "pinyin", "reformed", "search", "searchjl", "stroke", "trad", "unihan", "zhuyin"
    };
    return list;
}

static const std::vector<std::string>& GetSupportedCurrencies() {
    static const std::vector<std::string> list = {
        "AED", "AFN", "ALL", "AMD", "ANG", "AOA", "ARS", "AUD", "AWG", "AZN",
        "BAM", "BBD", "BDT", "BGN", "BHD", "BIF", "BMD", "BND", "BOB", "BRL",
        "BSD", "BTN", "BWP", "BYN", "BZD", "CAD", "CDF", "CHF", "CLP", "CNY",
        "COP", "CRC", "CUC", "CUP", "CVE", "CZK", "DJF", "DKK", "DOP", "DZD",
        "EGP", "ERN", "ETB", "EUR", "FJD", "FKP", "GBP", "GEL", "GHS", "GIP",
        "GMD", "GNF", "GTQ", "GYD", "HKD", "HNL", "HRK", "HTG", "HUF", "IDR",
        "ILS", "INR", "IQD", "IRR", "ISK", "JMD", "JOD", "JPY", "KES", "KGS",
        "KHR", "KMF", "KPW", "KRW", "KWD", "KYD", "KZT", "LAK", "LBP", "LKR",
        "LRD", "LSL", "LYD", "MAD", "MDL", "MGA", "MKD", "MMK", "MNT", "MOP",
        "MRU", "MUR", "MVR", "MWK", "MXN", "MYR", "MZN", "NAD", "NGN", "NIO",
        "NOK", "NPR", "NZD", "OMR", "PAB", "PEN", "PGK", "PHP", "PKR", "PLN",
        "PYG", "QAR", "RON", "RSD", "RUB", "RWF", "SAR", "SBD", "SCR", "SDG",
        "SEK", "SGD", "SHP", "SLL", "SOS", "SRD", "SSP", "STN", "SVC", "SYP",
        "SZL", "THB", "TJX", "TJS", "TMT", "TND", "TOP", "TRY", "TTD", "TWD",
        "TZS", "UAH", "UGX", "USD", "UYU", "UZS", "VES", "VND", "VUV", "WST",
        "XAF", "XCD", "XOF", "XPF", "YER", "ZAR", "ZMW", "ZWL"
    };
    return list;
}

static const std::vector<std::string>& GetSupportedNumberingSystems() {
    static const std::vector<std::string> list = {
        "arab", "arabext", "bali", "beng", "deva", "fullwide", "gujr", "guru",
        "hanidec", "khmr", "knda", "laoo", "latn", "limb", "mlym", "mong",
        "mymr", "orya", "tamldec", "telu", "thai", "tibt"
    };
    return list;
}

static const std::vector<std::string>& GetSupportedTimeZones() {
    static const std::vector<std::string> list = {
        "Africa/Cairo", "Africa/Johannesburg", "Africa/Lagos", "America/Anchorage",
        "America/Argentina/Buenos_Aires", "America/Chicago", "America/Denver",
        "America/Los_Angeles", "America/New_York", "America/Sao_Paulo", "America/Toronto",
        "America/Vancouver", "Asia/Bangkok", "Asia/Dubai", "Asia/Hong_Kong",
        "Asia/Kolkata", "Asia/Seoul", "Asia/Shanghai", "Asia/Singapore", "Asia/Tokyo",
        "Australia/Sydney", "Europe/Berlin", "Europe/London", "Europe/Madrid",
        "Europe/Paris", "Europe/Rome", "Pacific/Auckland", "Pacific/Honolulu", "UTC"
    };
    return list;
}

static const std::vector<std::string>& GetSupportedUnits() {
    static const std::vector<std::string> list = {
        "acre", "bit", "byte", "celsius", "centimeter", "day", "degree",
        "fahrenheit", "foot", "gallon", "gigabit", "gigabyte", "gram", "hectare",
        "hour", "inch", "kilobit", "kilobyte", "kilogram", "kilometer", "liter",
        "megabit", "megabyte", "meter", "mile", "mile-scandinavian", "milliliter",
        "millimeter", "millisecond", "minute", "month", "ounce", "percent",
        "petabyte", "pound", "second", "stone", "terabit", "terabyte", "week",
        "yard", "year"
    };
    return list;
}

// Helper to make a JSArray from std::vector<std::string>
static JSArray* MakeStringArray(const std::vector<std::string>& strings, Realm& realm) {
    Shape* root = realm.GetRootShape();
    auto* arr = AllocateRuntime<JSArray>(root, strings.size());
    if (realm.GetArrayPrototype()) {
        arr->SetPrototype(realm.GetArrayPrototype());
    }
    for (size_t i = 0; i < strings.size(); ++i) {
        arr->SetElement(i, TaggedValue::FromHeapObject(AllocateRuntime<JSString>(strings[i])));
    }
    return arr;
}

// Canonicalize and deduplicate input locales
static std::vector<std::string> CanonicalizeLocaleList(TaggedValue locales_val) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;

    if (locales_val.IsUndefined()) {
        return result;
    }

    auto process_tag = [&](std::string_view tag) {
        auto bcp = BCP47Locale::Parse(tag);
        if (!bcp.is_valid) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("RangeError", "Invalid language tag: " + std::string(tag))));
            }
            return false;
        }
        if (seen.find(bcp.canonical_tag) == seen.end()) {
            seen.insert(bcp.canonical_tag);
            result.push_back(bcp.canonical_tag);
        }
        return true;
    };

    if (locales_val.IsHeapObject()) {
        auto* ho = locales_val.AsHeapObject();
        if (ho->IsString()) {
            process_tag(static_cast<JSString*>(ho)->View());
        } else if (ho->IsArray()) {
            auto* arr = static_cast<JSArray*>(ho);
            for (size_t i = 0; i < arr->Length(); ++i) {
                TaggedValue el = arr->GetElement(i);
                std::string s = el.ToString();
                if (!process_tag(s)) return {};
            }
        } else {
            std::string s = locales_val.ToString();
            process_tag(s);
        }
    } else if (locales_val.IsString()) {
        process_tag(locales_val.ToString());
    }

    return result;
}

// ============================================================================
// Number Formatting Implementation
// ============================================================================

struct LocaleNumberSymbols {
    std::string decimal_sep{"."};
    std::string group_sep{","};
    std::string currency_symbol{"$"};
    bool currency_prefix{true};
    bool indian_grouping{false};
};

static LocaleNumberSymbols GetLocaleSymbols(std::string_view locale, std::string_view currency = "USD") {
    LocaleNumberSymbols s;
    std::string lang = "en";
    if (locale.size() >= 2) lang = ToLowerAscii(locale.substr(0, 2));

    if (lang == "de") {
        s.decimal_sep = ",";
        s.group_sep = ".";
        s.currency_prefix = false;
    } else if (lang == "fr") {
        s.decimal_sep = ",";
        s.group_sep = " ";
        s.currency_prefix = false;
    } else if (lang == "es" || lang == "it") {
        s.decimal_sep = ",";
        s.group_sep = ".";
        s.currency_prefix = false;
    } else if (lang == "hi" || (lang == "en" && locale.find("IN") != std::string::npos)) {
        s.decimal_sep = ".";
        s.group_sep = ",";
        s.indian_grouping = true;
    } else {
        s.decimal_sep = ".";
        s.group_sep = ",";
        s.currency_prefix = true;
    }

    if (currency == "EUR") {
        s.currency_symbol = "\xE2\x82\xAC"; // €
    } else if (currency == "GBP") {
        s.currency_symbol = "\xC2\xA3"; // £
    } else if (currency == "JPY" || currency == "CNY") {
        s.currency_symbol = "\xC2\xA5"; // ¥
    } else if (currency == "INR") {
        s.currency_symbol = "\xE2\x82\xB9"; // ₹
    } else if (currency == "USD") {
        s.currency_symbol = "$";
    } else if (!currency.empty()) {
        s.currency_symbol = std::string(currency);
    }
    return s;
}

static std::string FormatDoubleWithDigits(double val, int min_frac, int max_frac, int min_int) {
    if (std::isnan(val)) return "NaN";
    if (std::isinf(val)) return val < 0 ? "-Infinity" : "Infinity";

    bool is_neg = std::signbit(val);
    double abs_val = std::abs(val);

    // Round according to max_frac
    double factor = std::pow(10.0, max_frac);
    double rounded = std::round(abs_val * factor) / factor;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(max_frac) << rounded;
    std::string s = ss.str();

    // Trim unnecessary trailing zeros down to min_frac
    size_t dot = s.find('.');
    if (dot != std::string::npos) {
        size_t end = s.size();
        while (end > dot + 1 + static_cast<size_t>(min_frac) && s[end - 1] == '0') {
            end--;
        }
        if (end == dot + 1 && min_frac == 0) {
            end = dot;
        }
        s = s.substr(0, end);
    }

    // Pad integer part with leading zeros if needed
    size_t int_len = (dot == std::string::npos) ? s.size() : dot;
    if (int_len < static_cast<size_t>(min_int)) {
        std::string pad(static_cast<size_t>(min_int) - int_len, '0');
        s = pad + s;
    }

    return (is_neg ? "-" : "") + s;
}

static std::string ApplyGrouping(std::string_view int_str, std::string_view group_sep, bool indian_grouping) {
    if (int_str.empty()) return "";
    bool is_neg = (int_str[0] == '-');
    std::string digits = is_neg ? std::string(int_str.substr(1)) : std::string(int_str);

    if (digits.size() <= 3) {
        return (is_neg ? "-" : "") + digits;
    }

    std::string res;
    if (indian_grouping) {
        // Last 3 digits, then groups of 2
        size_t n = digits.size();
        res = digits.substr(n - 3);
        size_t pos = n - 3;
        while (pos > 0) {
            size_t take = (pos >= 2) ? 2 : pos;
            res = digits.substr(pos - take, take) + std::string(group_sep) + res;
            pos -= take;
        }
    } else {
        // Standard groups of 3 from right to left
        int count = 0;
        for (int i = static_cast<int>(digits.size()) - 1; i >= 0; --i) {
            if (count == 3) {
                res = std::string(group_sep) + res;
                count = 0;
            }
            res = digits[static_cast<size_t>(i)] + res;
            count++;
        }
    }

    return (is_neg ? "-" : "") + res;
}

static std::string FormatNumberInternal(double val, const NumberFormatOptions& opts) {
    if (std::isnan(val)) return "NaN";
    if (std::isinf(val)) return val < 0 ? "-Infinity" : "Infinity";

    LocaleNumberSymbols sym = GetLocaleSymbols(opts.locale, opts.currency);

    double target_val = val;
    if (opts.style == "percent") {
        target_val = val * 100.0;
    }

    bool is_neg = std::signbit(target_val);
    double abs_val = std::abs(target_val);

    std::string formatted_num = FormatDoubleWithDigits(abs_val, opts.minimum_fraction_digits, opts.maximum_fraction_digits, opts.minimum_integer_digits);

    std::string int_part = formatted_num;
    std::string frac_part = "";
    size_t dot_pos = formatted_num.find('.');
    if (dot_pos != std::string::npos) {
        int_part = formatted_num.substr(0, dot_pos);
        frac_part = formatted_num.substr(dot_pos + 1);
    }

    if (opts.use_grouping) {
        int_part = ApplyGrouping(int_part, sym.group_sep, sym.indian_grouping);
    }

    std::string number_str = int_part;
    if (!frac_part.empty()) {
        number_str += sym.decimal_sep + frac_part;
    }

    std::string sign_str = "";
    if (is_neg) {
        sign_str = "-";
    } else if (opts.sign_display == "always") {
        sign_str = "+";
    }

    if (opts.style == "currency") {
        std::string curr = sym.currency_symbol;
        if (opts.currency_display == "code") curr = opts.currency;
        if (sym.currency_prefix) {
            return sign_str + curr + (opts.currency_display == "code" ? " " : "") + number_str;
        } else {
            return sign_str + number_str + " " + curr;
        }
    } else if (opts.style == "percent") {
        if (!sym.currency_prefix) {
            return sign_str + number_str + " %";
        }
        return sign_str + number_str + "%";
    } else if (opts.style == "unit" && !opts.unit.empty()) {
        return sign_str + number_str + " " + opts.unit;
    }

    return sign_str + number_str;
}

// ============================================================================
// Date Formatting Helpers
// ============================================================================

struct BrokenDownDate {
    int year{1970};
    int month{1}; // 1..12
    int day{1};   // 1..31
    int hour{0};  // 0..23
    int minute{0};// 0..59
    int second{0};// 0..59
    int weekday{4}; // 0 = Sun, 1 = Mon, ..., 6 = Sat
    int ms{0};
};

static BrokenDownDate EpochMsToUTC(double epoch_ms) {
    BrokenDownDate d;
    if (std::isnan(epoch_ms) || std::isinf(epoch_ms)) return d;

    int64_t total_ms = static_cast<int64_t>(std::floor(epoch_ms));
    int64_t total_sec = total_ms / 1000;
    int ms = static_cast<int>(total_ms % 1000);
    if (ms < 0) { ms += 1000; total_sec--; }
    d.ms = ms;

    // Day calculations per Gregorian calendar
    int64_t days = total_sec / 86400;
    int64_t day_sec = total_sec % 86400;
    if (day_sec < 0) { day_sec += 86400; days--; }

    d.hour = static_cast<int>(day_sec / 3600);
    d.minute = static_cast<int>((day_sec % 3600) / 60);
    d.second = static_cast<int>(day_sec % 60);

    // 1970-01-01 was Thursday (4)
    int wd = static_cast<int>((days + 4) % 7);
    if (wd < 0) wd += 7;
    d.weekday = wd;

    // Gregorian year calculation
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1024 + doe / 1460 - doe / 36524) / 365;
    int64_t y = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned day = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp < 10 ? mp + 3 : mp - 9;
    if (month <= 2) y++;

    d.year = static_cast<int>(y);
    d.month = static_cast<int>(month);
    d.day = static_cast<int>(day);
    return d;
}

static const char* kEnglishMonthsLong[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char* kEnglishMonthsShort[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char* kGermanMonthsLong[] = {
    "Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni",
    "Juli", "August", "September", "Oktober", "November", "Dezember"
};
static const char* kGermanMonthsShort[] = {
    "Jan.", "Feb.", "M\xC3\xA4rz", "Apr.", "Mai", "Juni",
    "Juli", "Aug.", "Sept.", "Okt.", "Nov.", "Dez."
};

static std::string FormatDateTimeInternal(double epoch_ms, const DateTimeFormatOptions& opts) {
    if (std::isnan(epoch_ms)) return "Invalid Date";
    BrokenDownDate d = EpochMsToUTC(epoch_ms);

    std::string lang = "en";
    if (opts.locale.size() >= 2) lang = ToLowerAscii(opts.locale.substr(0, 2));

    std::string year_str = std::to_string(d.year);
    std::string month_num = std::to_string(d.month);
    std::string month_2d = (d.month < 10 ? "0" : "") + month_num;
    std::string day_num = std::to_string(d.day);
    std::string day_2d = (d.day < 10 ? "0" : "") + day_num;

    std::string month_name = kEnglishMonthsLong[d.month - 1];
    std::string month_short = kEnglishMonthsShort[d.month - 1];
    if (lang == "de") {
        month_name = kGermanMonthsLong[d.month - 1];
        month_short = kGermanMonthsShort[d.month - 1];
    }

    std::string date_part = "";
    if (opts.date_style == "full" || opts.date_style == "long") {
        if (lang == "de") {
            date_part = day_num + ". " + month_name + " " + year_str;
        } else {
            date_part = month_name + " " + day_num + ", " + year_str;
        }
    } else if (opts.date_style == "medium") {
        if (lang == "de") {
            date_part = day_num + ". " + month_short + " " + year_str;
        } else {
            date_part = month_short + " " + day_num + ", " + year_str;
        }
    } else if (opts.date_style == "short" || (opts.date_style.empty() && opts.time_style.empty())) {
        if (lang == "de") {
            date_part = day_num + "." + month_num + "." + year_str;
        } else if (lang == "fr" || (opts.locale.find("GB") != std::string::npos)) {
            date_part = day_2d + "/" + month_2d + "/" + year_str;
        } else if (lang == "ja" || lang == "zh") {
            date_part = year_str + "/" + month_2d + "/" + day_2d;
        } else {
            date_part = month_num + "/" + day_num + "/" + year_str;
        }
    }

    std::string time_part = "";
    if (!opts.time_style.empty()) {
        int h = d.hour;
        bool pm = (h >= 12);
        std::string am_pm = pm ? " PM" : " AM";
        if (opts.hour12) {
            h = h % 12;
            if (h == 0) h = 12;
        }
        std::string h_str = std::to_string(h);
        std::string m_str = (d.minute < 10 ? "0" : "") + std::to_string(d.minute);
        std::string s_str = (d.second < 10 ? "0" : "") + std::to_string(d.second);

        time_part = h_str + ":" + m_str + ":" + s_str;
        if (opts.hour12) {
            time_part += am_pm;
        }
    }

    if (!date_part.empty() && !time_part.empty()) {
        return date_part + ", " + time_part;
    }
    if (!date_part.empty()) return date_part;
    if (!time_part.empty()) return time_part;
    return date_part;
}

// ============================================================================
// Collator Implementation
// ============================================================================

static int NaturalCompare(std::string_view a, std::string_view b, bool numeric, bool case_sensitive) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (numeric && IsAsciiDigit(a[i]) && IsAsciiDigit(b[j])) {
            // Extract integer sequences
            size_t start_a = i;
            while (i < a.size() && IsAsciiDigit(a[i])) i++;
            size_t start_b = j;
            while (j < b.size() && IsAsciiDigit(b[j])) j++;

            std::string_view num_a = a.substr(start_a, i - start_a);
            std::string_view num_b = b.substr(start_b, j - start_b);

            // Strip leading zeros
            while (num_a.size() > 1 && num_a[0] == '0') num_a.remove_prefix(1);
            while (num_b.size() > 1 && num_b[0] == '0') num_b.remove_prefix(1);

            if (num_a.size() != num_b.size()) {
                return num_a.size() < num_b.size() ? -1 : 1;
            }
            if (num_a != num_b) {
                return num_a < num_b ? -1 : 1;
            }
            continue;
        }

        char ca = a[i];
        char cb = b[j];
        if (!case_sensitive) {
            ca = static_cast<char>(std::tolower(static_cast<unsigned char>(ca)));
            cb = static_cast<char>(std::tolower(static_cast<unsigned char>(cb)));
        }
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        i++;
        j++;
    }

    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// ============================================================================
// PluralRules Implementation
// ============================================================================

static std::string SelectPlural(double val, const PluralRulesOptions& opts) {
    double abs_val = std::abs(val);
    int64_t n = static_cast<int64_t>(abs_val);
    std::string lang = "en";
    if (opts.locale.size() >= 2) lang = ToLowerAscii(opts.locale.substr(0, 2));

    if (opts.type == "ordinal") {
        if (lang == "en") {
            int64_t mod10 = n % 10;
            int64_t mod100 = n % 100;
            if (mod10 == 1 && mod100 != 11) return "one";
            if (mod10 == 2 && mod100 != 12) return "two";
            if (mod10 == 3 && mod100 != 13) return "few";
            return "other";
        }
        return "other";
    }

    // Cardinal
    if (lang == "fr") {
        return (n == 0 || n == 1) ? "one" : "other";
    }
    if (lang == "ru") {
        int64_t mod10 = n % 10;
        int64_t mod100 = n % 100;
        if (mod10 == 1 && mod100 != 11) return "one";
        if (mod10 >= 2 && mod10 <= 4 && !(mod100 >= 12 && mod100 <= 14)) return "few";
        if (mod10 == 0 || (mod10 >= 5 && mod10 <= 9) || (mod100 >= 11 && mod100 <= 14)) return "many";
        return "other";
    }
    if (lang == "ar") {
        if (n == 0) return "zero";
        if (n == 1) return "one";
        if (n == 2) return "two";
        int64_t mod100 = n % 100;
        if (mod100 >= 3 && mod100 <= 10) return "few";
        if (mod100 >= 11 && mod100 <= 99) return "many";
        return "other";
    }

    // Default English / German
    return (n == 1) ? "one" : "other";
}

// ============================================================================
// RelativeTimeFormat Implementation
// ============================================================================

static std::string FormatRelativeTime(double val, std::string_view unit_in, const RelativeTimeFormatOptions& opts) {
    std::string unit = ToLowerAscii(unit_in);
    if (!unit.empty() && unit.back() == 's') unit.pop_back(); // "days" -> "day"

    int64_t v = static_cast<int64_t>(val);
    if (opts.numeric == "auto") {
        if (unit == "day") {
            if (v == 0) return "today";
            if (v == -1) return "yesterday";
            if (v == 1) return "tomorrow";
        } else if (unit == "year") {
            if (v == 0) return "this year";
            if (v == -1) return "last year";
            if (v == 1) return "next year";
        } else if (unit == "second" && v == 0) {
            return "now";
        }
    }

    std::string u_display = unit + (std::abs(v) == 1 ? "" : "s");
    if (val < 0) {
        return std::to_string(-v) + " " + u_display + " ago";
    } else {
        return "in " + std::to_string(v) + " " + u_display;
    }
}

// ============================================================================
// ListFormat Implementation
// ============================================================================

static std::string FormatListInternal(const std::vector<std::string>& list, const ListFormatOptions& opts) {
    if (list.empty()) return "";
    if (list.size() == 1) return list[0];

    std::string conj = " and ";
    if (opts.type == "disjunction") {
        conj = " or ";
    } else if (opts.type == "unit") {
        conj = ", ";
    }

    std::string lang = "en";
    if (opts.locale.size() >= 2) lang = ToLowerAscii(opts.locale.substr(0, 2));
    if (lang == "de") {
        conj = (opts.type == "disjunction") ? " oder " : " und ";
    } else if (lang == "fr") {
        conj = (opts.type == "disjunction") ? " ou " : " et ";
    } else if (lang == "es") {
        conj = (opts.type == "disjunction") ? " o " : " y ";
    }

    if (list.size() == 2) {
        return list[0] + conj + list[1];
    }

    std::string res;
    for (size_t i = 0; i < list.size(); ++i) {
        if (i > 0) {
            if (i == list.size() - 1) {
                if (opts.type == "unit" || lang == "de" || lang == "fr") {
                    res += conj;
                } else {
                    res += "," + conj;
                }
            } else {
                res += ", ";
            }
        }
        res += list[i];
    }
    return res;
}

// ============================================================================
// Realm Intrinsics Initialization: Intl, Date, Number
// ============================================================================

void InitializeIntl(Realm& realm) {
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();

    auto make_native = [&realm, root](std::string_view name, auto cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto make_obj = [&realm, root]() -> JSObject* {
        return AllocateRuntime<JSObject>(root);
    };

    auto* intl_obj = make_obj();

    // 1. Intl.getCanonicalLocales
    intl_obj->SetNamedProperty("getCanonicalLocales", TaggedValue::FromHeapObject(
        make_native("getCanonicalLocales", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                return TaggedValue::Undefined();
            }
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);

    // 2. Intl.supportedValuesOf
    intl_obj->SetNamedProperty("supportedValuesOf", TaggedValue::FromHeapObject(
        make_native("supportedValuesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty()) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(
                        TaggedValue::FromHeapObject(new JSError("TypeError", "missing key argument to Intl.supportedValuesOf")));
                }
                return TaggedValue::Undefined();
            }
            std::string key = args[0].ToString();
            if (key == "calendar") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedCalendars(), realm));
            } else if (key == "collation") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedCollations(), realm));
            } else if (key == "currency") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedCurrencies(), realm));
            } else if (key == "numberingSystem") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedNumberingSystems(), realm));
            } else if (key == "timeZone") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedTimeZones(), realm));
            } else if (key == "unit") {
                return TaggedValue::FromHeapObject(MakeStringArray(GetSupportedUnits(), realm));
            }

            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("RangeError", "Invalid key: " + key)));
            }
            return TaggedValue::Undefined();
        })), next_sid);

    // 3. Intl.NumberFormat
    auto* num_fmt_proto = make_obj();
    auto* num_fmt_ctor = make_native("NumberFormat", [&realm, num_fmt_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(num_fmt_proto);

        NumberFormatOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("style")) opts.style = o->GetNamedProperty("style").ToString();
            if (o->HasNamedProperty("currency")) opts.currency = ToUpperAscii(o->GetNamedProperty("currency").ToString());
            if (o->HasNamedProperty("currencyDisplay")) opts.currency_display = o->GetNamedProperty("currencyDisplay").ToString();
            if (o->HasNamedProperty("useGrouping")) opts.use_grouping = o->GetNamedProperty("useGrouping").ToBoolean();
            if (o->HasNamedProperty("minimumFractionDigits")) {
                opts.minimum_fraction_digits = static_cast<int>(o->GetNamedProperty("minimumFractionDigits").AsNumber());
            }
            if (o->HasNamedProperty("maximumFractionDigits")) {
                opts.maximum_fraction_digits = static_cast<int>(o->GetNamedProperty("maximumFractionDigits").AsNumber());
            }
            if (o->HasNamedProperty("notation")) opts.notation = o->GetNamedProperty("notation").ToString();
            if (o->HasNamedProperty("signDisplay")) opts.sign_display = o->GetNamedProperty("signDisplay").ToString();
            if (o->HasNamedProperty("unit")) opts.unit = o->GetNamedProperty("unit").ToString();
        }

        if (opts.style == "currency" && opts.currency.empty()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(
                    TaggedValue::FromHeapObject(new JSError("TypeError", "currency option is required when style is currency")));
            }
            return TaggedValue::Undefined();
        }

        if (opts.style == "currency" && opts.minimum_fraction_digits == 0 && opts.maximum_fraction_digits == 3) {
            opts.minimum_fraction_digits = 2;
            opts.maximum_fraction_digits = 2;
        }

        // Store options in properties
        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__style__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.style)), next_sid);
        instance->SetNamedProperty("__currency__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.currency)), next_sid);
        instance->SetNamedProperty("__currencyDisplay__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.currency_display)), next_sid);
        instance->SetNamedProperty("__useGrouping__", TaggedValue::FromBoolean(opts.use_grouping), next_sid);
        instance->SetNamedProperty("__minFrac__", TaggedValue::FromSmi(opts.minimum_fraction_digits), next_sid);
        instance->SetNamedProperty("__maxFrac__", TaggedValue::FromSmi(opts.maximum_fraction_digits), next_sid);
        instance->SetNamedProperty("__notation__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.notation)), next_sid);
        instance->SetNamedProperty("__signDisplay__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.sign_display)), next_sid);
        instance->SetNamedProperty("__unit__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.unit)), next_sid);

        return TaggedValue::FromHeapObject(instance);
    });

    auto extract_num_opts = [](JSObject* inst) -> NumberFormatOptions {
        NumberFormatOptions opts;
        if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
        if (inst->HasNamedProperty("__style__")) opts.style = inst->GetNamedProperty("__style__").ToString();
        if (inst->HasNamedProperty("__currency__")) opts.currency = inst->GetNamedProperty("__currency__").ToString();
        if (inst->HasNamedProperty("__currencyDisplay__")) opts.currency_display = inst->GetNamedProperty("__currencyDisplay__").ToString();
        if (inst->HasNamedProperty("__useGrouping__")) opts.use_grouping = inst->GetNamedProperty("__useGrouping__").ToBoolean();
        if (inst->HasNamedProperty("__minFrac__")) opts.minimum_fraction_digits = static_cast<int>(inst->GetNamedProperty("__minFrac__").AsNumber());
        if (inst->HasNamedProperty("__maxFrac__")) opts.maximum_fraction_digits = static_cast<int>(inst->GetNamedProperty("__maxFrac__").AsNumber());
        if (inst->HasNamedProperty("__notation__")) opts.notation = inst->GetNamedProperty("__notation__").ToString();
        if (inst->HasNamedProperty("__signDisplay__")) opts.sign_display = inst->GetNamedProperty("__signDisplay__").ToString();
        if (inst->HasNamedProperty("__unit__")) opts.unit = inst->GetNamedProperty("__unit__").ToString();
        return opts;
    };

    num_fmt_proto->SetNamedProperty("format", TaggedValue::FromHeapObject(
        make_native("format", [extract_num_opts](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            NumberFormatOptions opts = extract_num_opts(inst);
            double val = args.empty() ? 0.0 : args[0].AsNumber();
            std::string res = FormatNumberInternal(val, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        })), next_sid);

    num_fmt_proto->SetNamedProperty("formatToParts", TaggedValue::FromHeapObject(
        make_native("formatToParts", [&realm, root, extract_num_opts, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            NumberFormatOptions opts = extract_num_opts(inst);
            double val = args.empty() ? 0.0 : args[0].AsNumber();
            std::string formatted = FormatNumberInternal(val, opts);

            auto* arr = AllocateRuntime<JSArray>(root);
            if (realm.GetArrayPrototype()) arr->SetPrototype(realm.GetArrayPrototype());

            auto add_part = [root, arr, &next_sid](std::string_view type, std::string_view v) {
                auto* p = AllocateRuntime<JSObject>(root);
                p->SetNamedProperty("type", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(type)), next_sid);
                p->SetNamedProperty("value", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(v)), next_sid);
                arr->Push(TaggedValue::FromHeapObject(p));
            };

            LocaleNumberSymbols sym = GetLocaleSymbols(opts.locale, opts.currency);
            // Parse formatted string into parts
            size_t i = 0;
            while (i < formatted.size()) {
                if (!sym.currency_symbol.empty() && formatted.compare(i, sym.currency_symbol.size(), sym.currency_symbol) == 0) {
                    add_part("currency", sym.currency_symbol);
                    i += sym.currency_symbol.size();
                } else if (!sym.group_sep.empty() && formatted.compare(i, sym.group_sep.size(), sym.group_sep) == 0) {
                    add_part("group", sym.group_sep);
                    i += sym.group_sep.size();
                } else if (!sym.decimal_sep.empty() && formatted.compare(i, sym.decimal_sep.size(), sym.decimal_sep) == 0) {
                    add_part("decimal", sym.decimal_sep);
                    i += sym.decimal_sep.size();
                } else if (formatted[i] == '-' || formatted[i] == '+') {
                    add_part(formatted[i] == '-' ? "minusSign" : "plusSign", std::string(1, formatted[i]));
                    i++;
                } else if (formatted[i] == '%') {
                    add_part("percentSign", "%");
                    i++;
                } else if (formatted[i] == ' ') {
                    add_part("literal", " ");
                    i++;
                } else if (IsAsciiDigit(formatted[i])) {
                    size_t start = i;
                    while (i < formatted.size() && IsAsciiDigit(formatted[i])) i++;
                    add_part("integer", formatted.substr(start, i - start));
                } else {
                    add_part("literal", formatted.substr(i, 1));
                    i++;
                }
            }
            return TaggedValue::FromHeapObject(arr);
        })), next_sid);

    num_fmt_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, extract_num_opts, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            NumberFormatOptions opts = extract_num_opts(inst);

            auto* res = AllocateRuntime<JSObject>(root);
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
            res->SetNamedProperty("numberingSystem", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.numbering_system)), next_sid);
            res->SetNamedProperty("style", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.style)), next_sid);
            if (!opts.currency.empty()) {
                res->SetNamedProperty("currency", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.currency)), next_sid);
                res->SetNamedProperty("currencyDisplay", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.currency_display)), next_sid);
            }
            res->SetNamedProperty("minimumIntegerDigits", TaggedValue::FromSmi(opts.minimum_integer_digits), next_sid);
            res->SetNamedProperty("minimumFractionDigits", TaggedValue::FromSmi(opts.minimum_fraction_digits), next_sid);
            res->SetNamedProperty("maximumFractionDigits", TaggedValue::FromSmi(opts.maximum_fraction_digits), next_sid);
            res->SetNamedProperty("useGrouping", TaggedValue::FromBoolean(opts.use_grouping), next_sid);
            res->SetNamedProperty("notation", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.notation)), next_sid);
            res->SetNamedProperty("signDisplay", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.sign_display)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    num_fmt_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    num_fmt_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(num_fmt_proto), next_sid);
    intl_obj->SetNamedProperty("NumberFormat", TaggedValue::FromHeapObject(num_fmt_ctor), next_sid);

    // 4. Intl.DateTimeFormat
    auto* dt_fmt_proto = make_obj();
    auto* dt_fmt_ctor = make_native("DateTimeFormat", [&realm, dt_fmt_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(dt_fmt_proto);

        DateTimeFormatOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("dateStyle")) opts.date_style = o->GetNamedProperty("dateStyle").ToString();
            if (o->HasNamedProperty("timeStyle")) opts.time_style = o->GetNamedProperty("timeStyle").ToString();
            if (o->HasNamedProperty("timeZone")) opts.time_zone = o->GetNamedProperty("timeZone").ToString();
            if (o->HasNamedProperty("hour12")) {
                opts.hour12 = o->GetNamedProperty("hour12").ToBoolean();
                opts.hour12_specified = true;
            }
            if (o->HasNamedProperty("year")) opts.year = o->GetNamedProperty("year").ToString();
            if (o->HasNamedProperty("month")) opts.month = o->GetNamedProperty("month").ToString();
            if (o->HasNamedProperty("day")) opts.day = o->GetNamedProperty("day").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__calendar__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.calendar)), next_sid);
        instance->SetNamedProperty("__timeZone__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.time_zone)), next_sid);
        instance->SetNamedProperty("__dateStyle__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.date_style)), next_sid);
        instance->SetNamedProperty("__timeStyle__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.time_style)), next_sid);
        instance->SetNamedProperty("__hour12__", TaggedValue::FromBoolean(opts.hour12), next_sid);

        return TaggedValue::FromHeapObject(instance);
    });

    auto extract_dt_opts = [](JSObject* inst) -> DateTimeFormatOptions {
        DateTimeFormatOptions opts;
        if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
        if (inst->HasNamedProperty("__calendar__")) opts.calendar = inst->GetNamedProperty("__calendar__").ToString();
        if (inst->HasNamedProperty("__timeZone__")) opts.time_zone = inst->GetNamedProperty("__timeZone__").ToString();
        if (inst->HasNamedProperty("__dateStyle__")) opts.date_style = inst->GetNamedProperty("__dateStyle__").ToString();
        if (inst->HasNamedProperty("__timeStyle__")) opts.time_style = inst->GetNamedProperty("__timeStyle__").ToString();
        if (inst->HasNamedProperty("__hour12__")) opts.hour12 = inst->GetNamedProperty("__hour12__").ToBoolean();
        return opts;
    };

    dt_fmt_proto->SetNamedProperty("format", TaggedValue::FromHeapObject(
        make_native("format", [extract_dt_opts](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            DateTimeFormatOptions opts = extract_dt_opts(inst);

            double epoch_ms = 0.0;
            if (args.empty() || args[0].IsUndefined()) {
                epoch_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            } else if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
                auto* obj = static_cast<JSObject*>(args[0].AsHeapObject());
                if (obj->HasNamedProperty("__time__")) {
                    epoch_ms = obj->GetNamedProperty("__time__").AsNumber();
                } else {
                    epoch_ms = args[0].AsNumber();
                }
            } else {
                epoch_ms = args[0].AsNumber();
            }

            std::string res = FormatDateTimeInternal(epoch_ms, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        })), next_sid);

    dt_fmt_proto->SetNamedProperty("formatToParts", TaggedValue::FromHeapObject(
        make_native("formatToParts", [&realm, root, extract_dt_opts, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            DateTimeFormatOptions opts = extract_dt_opts(inst);

            double epoch_ms = args.empty() ? 0.0 : args[0].AsNumber();
            BrokenDownDate d = EpochMsToUTC(epoch_ms);

            auto* arr = AllocateRuntime<JSArray>(root);
            if (realm.GetArrayPrototype()) arr->SetPrototype(realm.GetArrayPrototype());

            auto add_part = [root, arr, &next_sid](std::string_view type, std::string_view v) {
                auto* p = AllocateRuntime<JSObject>(root);
                p->SetNamedProperty("type", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(type)), next_sid);
                p->SetNamedProperty("value", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(v)), next_sid);
                arr->Push(TaggedValue::FromHeapObject(p));
            };

            add_part("month", std::to_string(d.month));
            add_part("literal", "/");
            add_part("day", std::to_string(d.day));
            add_part("literal", "/");
            add_part("year", std::to_string(d.year));

            return TaggedValue::FromHeapObject(arr);
        })), next_sid);

    dt_fmt_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, extract_dt_opts, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            DateTimeFormatOptions opts = extract_dt_opts(inst);

            auto* res = AllocateRuntime<JSObject>(root);
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
            res->SetNamedProperty("calendar", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.calendar)), next_sid);
            res->SetNamedProperty("numberingSystem", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.numbering_system)), next_sid);
            res->SetNamedProperty("timeZone", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.time_zone)), next_sid);
            if (!opts.date_style.empty()) {
                res->SetNamedProperty("dateStyle", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.date_style)), next_sid);
            }
            if (!opts.time_style.empty()) {
                res->SetNamedProperty("timeStyle", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.time_style)), next_sid);
            }
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    dt_fmt_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    dt_fmt_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(dt_fmt_proto), next_sid);
    intl_obj->SetNamedProperty("DateTimeFormat", TaggedValue::FromHeapObject(dt_fmt_ctor), next_sid);

    // 5. Intl.Collator
    auto* collator_proto = make_obj();
    auto* collator_ctor = make_native("Collator", [&realm, collator_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(collator_proto);

        CollatorOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("usage")) opts.usage = o->GetNamedProperty("usage").ToString();
            if (o->HasNamedProperty("sensitivity")) opts.sensitivity = o->GetNamedProperty("sensitivity").ToString();
            if (o->HasNamedProperty("numeric")) opts.numeric = o->GetNamedProperty("numeric").ToBoolean();
            if (o->HasNamedProperty("caseFirst")) opts.case_first = o->GetNamedProperty("caseFirst").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__usage__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.usage)), next_sid);
        instance->SetNamedProperty("__sensitivity__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.sensitivity)), next_sid);
        instance->SetNamedProperty("__numeric__", TaggedValue::FromBoolean(opts.numeric), next_sid);
        instance->SetNamedProperty("__caseFirst__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.case_first)), next_sid);

        return TaggedValue::FromHeapObject(instance);
    });

    auto extract_col_opts = [](JSObject* inst) -> CollatorOptions {
        CollatorOptions opts;
        if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
        if (inst->HasNamedProperty("__usage__")) opts.usage = inst->GetNamedProperty("__usage__").ToString();
        if (inst->HasNamedProperty("__sensitivity__")) opts.sensitivity = inst->GetNamedProperty("__sensitivity__").ToString();
        if (inst->HasNamedProperty("__numeric__")) opts.numeric = inst->GetNamedProperty("__numeric__").ToBoolean();
        if (inst->HasNamedProperty("__caseFirst__")) opts.case_first = inst->GetNamedProperty("__caseFirst__").ToString();
        return opts;
    };

    collator_proto->SetNamedProperty("compare", TaggedValue::FromHeapObject(
        make_native("compare", [extract_col_opts](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            CollatorOptions opts = extract_col_opts(inst);

            std::string a = args.empty() ? "" : args[0].ToString();
            std::string b = args.size() > 1 ? args[1].ToString() : "";

            bool case_sensitive = (opts.sensitivity == "case" || opts.sensitivity == "variant");
            int cmp = NaturalCompare(a, b, opts.numeric, case_sensitive);
            return TaggedValue::FromSmi(cmp);
        })), next_sid);

    collator_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, extract_col_opts, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            CollatorOptions opts = extract_col_opts(inst);

            auto* res = AllocateRuntime<JSObject>(root);
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
            res->SetNamedProperty("usage", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.usage)), next_sid);
            res->SetNamedProperty("sensitivity", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.sensitivity)), next_sid);
            res->SetNamedProperty("numeric", TaggedValue::FromBoolean(opts.numeric), next_sid);
            res->SetNamedProperty("caseFirst", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.case_first)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    collator_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    collator_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(collator_proto), next_sid);
    intl_obj->SetNamedProperty("Collator", TaggedValue::FromHeapObject(collator_ctor), next_sid);

    // 6. Intl.PluralRules
    auto* plural_proto = make_obj();
    auto* plural_ctor = make_native("PluralRules", [&realm, plural_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(plural_proto);

        PluralRulesOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("type")) opts.type = o->GetNamedProperty("type").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__type__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.type)), next_sid);
        return TaggedValue::FromHeapObject(instance);
    });

    plural_proto->SetNamedProperty("select", TaggedValue::FromHeapObject(
        make_native("select", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            PluralRulesOptions opts;
            if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
            if (inst->HasNamedProperty("__type__")) opts.type = inst->GetNamedProperty("__type__").ToString();

            double n = args.empty() ? 0.0 : args[0].AsNumber();
            std::string sel = SelectPlural(n, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(sel));
        })), next_sid);

    plural_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            auto* res = AllocateRuntime<JSObject>(root);
            std::string loc = inst->HasNamedProperty("__locale__") ? inst->GetNamedProperty("__locale__").ToString() : "en-US";
            std::string typ = inst->HasNamedProperty("__type__") ? inst->GetNamedProperty("__type__").ToString() : "cardinal";
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(loc)), next_sid);
            res->SetNamedProperty("type", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(typ)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    plural_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    plural_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(plural_proto), next_sid);
    intl_obj->SetNamedProperty("PluralRules", TaggedValue::FromHeapObject(plural_ctor), next_sid);

    // 7. Intl.ListFormat
    auto* list_proto = make_obj();
    auto* list_ctor = make_native("ListFormat", [&realm, list_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(list_proto);

        ListFormatOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("type")) opts.type = o->GetNamedProperty("type").ToString();
            if (o->HasNamedProperty("style")) opts.style = o->GetNamedProperty("style").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__type__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.type)), next_sid);
        instance->SetNamedProperty("__style__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.style)), next_sid);
        return TaggedValue::FromHeapObject(instance);
    });

    list_proto->SetNamedProperty("format", TaggedValue::FromHeapObject(
        make_native("format", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            ListFormatOptions opts;
            if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
            if (inst->HasNamedProperty("__type__")) opts.type = inst->GetNamedProperty("__type__").ToString();
            if (inst->HasNamedProperty("__style__")) opts.style = inst->GetNamedProperty("__style__").ToString();

            std::vector<std::string> items;
            if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsArray()) {
                auto* arr = static_cast<JSArray*>(args[0].AsHeapObject());
                for (size_t i = 0; i < arr->Length(); ++i) items.push_back(arr->GetElement(i).ToString());
            }

            std::string res = FormatListInternal(items, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        })), next_sid);

    list_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            auto* res = AllocateRuntime<JSObject>(root);
            std::string loc = inst->HasNamedProperty("__locale__") ? inst->GetNamedProperty("__locale__").ToString() : "en-US";
            std::string typ = inst->HasNamedProperty("__type__") ? inst->GetNamedProperty("__type__").ToString() : "conjunction";
            std::string sty = inst->HasNamedProperty("__style__") ? inst->GetNamedProperty("__style__").ToString() : "long";
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(loc)), next_sid);
            res->SetNamedProperty("type", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(typ)), next_sid);
            res->SetNamedProperty("style", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(sty)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    list_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    list_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(list_proto), next_sid);
    intl_obj->SetNamedProperty("ListFormat", TaggedValue::FromHeapObject(list_ctor), next_sid);

    // 8. Intl.RelativeTimeFormat
    auto* rel_proto = make_obj();
    auto* rel_ctor = make_native("RelativeTimeFormat", [&realm, rel_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(rel_proto);

        RelativeTimeFormatOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("numeric")) opts.numeric = o->GetNamedProperty("numeric").ToString();
            if (o->HasNamedProperty("style")) opts.style = o->GetNamedProperty("style").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__numeric__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.numeric)), next_sid);
        instance->SetNamedProperty("__style__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.style)), next_sid);
        return TaggedValue::FromHeapObject(instance);
    });

    rel_proto->SetNamedProperty("format", TaggedValue::FromHeapObject(
        make_native("format", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            RelativeTimeFormatOptions opts;
            if (inst->HasNamedProperty("__locale__")) opts.locale = inst->GetNamedProperty("__locale__").ToString();
            if (inst->HasNamedProperty("__numeric__")) opts.numeric = inst->GetNamedProperty("__numeric__").ToString();
            if (inst->HasNamedProperty("__style__")) opts.style = inst->GetNamedProperty("__style__").ToString();

            double val = args.empty() ? 0.0 : args[0].AsNumber();
            std::string unit = args.size() > 1 ? args[1].ToString() : "day";
            std::string res = FormatRelativeTime(val, unit, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        })), next_sid);

    rel_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            auto* res = AllocateRuntime<JSObject>(root);
            std::string loc = inst->HasNamedProperty("__locale__") ? inst->GetNamedProperty("__locale__").ToString() : "en-US";
            std::string num = inst->HasNamedProperty("__numeric__") ? inst->GetNamedProperty("__numeric__").ToString() : "always";
            std::string sty = inst->HasNamedProperty("__style__") ? inst->GetNamedProperty("__style__").ToString() : "long";
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(loc)), next_sid);
            res->SetNamedProperty("numeric", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(num)), next_sid);
            res->SetNamedProperty("style", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(sty)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    rel_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    rel_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(rel_proto), next_sid);
    intl_obj->SetNamedProperty("RelativeTimeFormat", TaggedValue::FromHeapObject(rel_ctor), next_sid);

    // 9. Intl.Segmenter
    auto* seg_proto = make_obj();
    auto* seg_ctor = make_native("Segmenter", [&realm, seg_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(seg_proto);

        SegmenterOptions opts;
        if (!args.empty() && !args[0].IsUndefined()) {
            auto canon = CanonicalizeLocaleList(args[0]);
            if (!canon.empty()) opts.locale = canon[0];
        }

        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
            auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
            if (o->HasNamedProperty("granularity")) opts.granularity = o->GetNamedProperty("granularity").ToString();
        }

        instance->SetNamedProperty("__locale__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.locale)), next_sid);
        instance->SetNamedProperty("__granularity__", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(opts.granularity)), next_sid);
        return TaggedValue::FromHeapObject(instance);
    });

    seg_proto->SetNamedProperty("segment", TaggedValue::FromHeapObject(
        make_native("segment", [&realm, root, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            std::string gran = inst->HasNamedProperty("__granularity__") ? inst->GetNamedProperty("__granularity__").ToString() : "grapheme";
            std::string text = args.empty() ? "" : args[0].ToString();

            // Create segments object
            auto* segments_obj = AllocateRuntime<JSObject>(root);
            auto* segments_arr = AllocateRuntime<JSArray>(root);
            if (realm.GetArrayPrototype()) segments_arr->SetPrototype(realm.GetArrayPrototype());

            if (gran == "word") {
                size_t pos = 0;
                while (pos < text.size()) {
                    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
                        auto* p = AllocateRuntime<JSObject>(root);
                        p->SetNamedProperty("segment", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(text.substr(pos, 1))), next_sid);
                        p->SetNamedProperty("index", TaggedValue::FromSmi(static_cast<int32_t>(pos)), next_sid);
                        p->SetNamedProperty("isWordLike", TaggedValue::FromBoolean(false), next_sid);
                        segments_arr->Push(TaggedValue::FromHeapObject(p));
                        pos++;
                    }
                    if (pos >= text.size()) break;
                    size_t start = pos;
                    while (pos < text.size() && !std::isspace(static_cast<unsigned char>(text[pos]))) pos++;
                    auto* p = AllocateRuntime<JSObject>(root);
                    p->SetNamedProperty("segment", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(text.substr(start, pos - start))), next_sid);
                    p->SetNamedProperty("index", TaggedValue::FromSmi(static_cast<int32_t>(start)), next_sid);
                    p->SetNamedProperty("isWordLike", TaggedValue::FromBoolean(true), next_sid);
                    segments_arr->Push(TaggedValue::FromHeapObject(p));
                }
            } else {
                for (size_t i = 0; i < text.size(); ++i) {
                    auto* p = AllocateRuntime<JSObject>(root);
                    p->SetNamedProperty("segment", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(text.substr(i, 1))), next_sid);
                    p->SetNamedProperty("index", TaggedValue::FromSmi(static_cast<int32_t>(i)), next_sid);
                    p->SetNamedProperty("isWordLike", TaggedValue::FromBoolean(IsAsciiAlphanum(text[i])), next_sid);
                    segments_arr->Push(TaggedValue::FromHeapObject(p));
                }
            }

            segments_obj->SetNamedProperty("__array__", TaggedValue::FromHeapObject(segments_arr), next_sid);

            // [Symbol.iterator]
            auto* iter_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [segments_arr](TaggedValue, const std::vector<TaggedValue>&) -> TaggedValue {
                return TaggedValue::FromHeapObject(segments_arr);
            }, root);
            segments_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(iter_fn), next_sid);

            return TaggedValue::FromHeapObject(segments_obj);
        })), next_sid);

    seg_proto->SetNamedProperty("resolvedOptions", TaggedValue::FromHeapObject(
        make_native("resolvedOptions", [root, &next_sid](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            auto* res = AllocateRuntime<JSObject>(root);
            std::string loc = inst->HasNamedProperty("__locale__") ? inst->GetNamedProperty("__locale__").ToString() : "en-US";
            std::string grn = inst->HasNamedProperty("__granularity__") ? inst->GetNamedProperty("__granularity__").ToString() : "grapheme";
            res->SetNamedProperty("locale", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(loc)), next_sid);
            res->SetNamedProperty("granularity", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(grn)), next_sid);
            return TaggedValue::FromHeapObject(res);
        })), next_sid);

    seg_ctor->SetNamedProperty("supportedLocalesOf", TaggedValue::FromHeapObject(
        make_native("supportedLocalesOf", [&realm](const std::vector<TaggedValue>& args) -> TaggedValue {
            TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
            auto canon = CanonicalizeLocaleList(val);
            return TaggedValue::FromHeapObject(MakeStringArray(canon, realm));
        })), next_sid);
    seg_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(seg_proto), next_sid);
    intl_obj->SetNamedProperty("Segmenter", TaggedValue::FromHeapObject(seg_ctor), next_sid);

    // Register Intl on Global Object
    realm.GetGlobalObject()->SetNamedProperty("Intl", TaggedValue::FromHeapObject(intl_obj), next_sid);
}

void InitializeDate(Realm& realm) {
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();

    auto make_native = [&realm, root](std::string_view name, auto cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto* date_proto = AllocateRuntime<JSObject>(root);
    realm.SetDatePrototype(date_proto);

    auto* date_ctor = make_native("Date", [&realm, date_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* instance = AllocateRuntime<JSObject>(root);
        instance->SetPrototype(date_proto);

        double epoch_ms = 0.0;
        if (args.empty()) {
            epoch_ms = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        } else if (args.size() == 1) {
            if (args[0].IsNumber()) {
                epoch_ms = args[0].AsNumber();
            } else if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsString()) {
                // Parse simple ISO date string YYYY-MM-DD or YYYY-MM-DDTHH:mm:ss
                std::string s = static_cast<JSString*>(args[0].AsHeapObject())->Value();
                int y = 1970, m = 1, d = 1, h = 0, min = 0, sec = 0;
                char dash1, dash2, t, colon1, colon2;
                std::istringstream ss(s);
                if (ss >> y >> dash1 >> m >> dash2 >> d) {
                    if (ss >> t >> h >> colon1 >> min) {
                        ss >> colon2 >> sec;
                    }
                    // Calculate epoch ms
                    std::tm tm_struct{};
                    tm_struct.tm_year = y - 1900;
                    tm_struct.tm_mon = m - 1;
                    tm_struct.tm_mday = d;
                    tm_struct.tm_hour = h;
                    tm_struct.tm_min = min;
                    tm_struct.tm_sec = sec;
#ifdef _WIN32
                    std::time_t t_val = _mkgmtime(&tm_struct);
#else
                    std::time_t t_val = timegm(&tm_struct);
#endif
                    epoch_ms = static_cast<double>(t_val) * 1000.0;
                } else {
                    epoch_ms = 0.0;
                }
            } else if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
                auto* src_obj = static_cast<JSObject*>(args[0].AsHeapObject());
                if (src_obj->HasNamedProperty("__time__")) {
                    epoch_ms = src_obj->GetNamedProperty("__time__").AsNumber();
                }
            }
        } else {
            // (year, month, [date, hours, minutes, seconds, ms])
            int y = static_cast<int>(args[0].AsNumber());
            int m = (args.size() > 1) ? static_cast<int>(args[1].AsNumber()) : 0;
            int d = (args.size() > 2) ? static_cast<int>(args[2].AsNumber()) : 1;
            int h = (args.size() > 3) ? static_cast<int>(args[3].AsNumber()) : 0;
            int min = (args.size() > 4) ? static_cast<int>(args[4].AsNumber()) : 0;
            int sec = (args.size() > 5) ? static_cast<int>(args[5].AsNumber()) : 0;
            int ms = (args.size() > 6) ? static_cast<int>(args[6].AsNumber()) : 0;

            if (y < 100 && y >= 0) y += 1900;

            std::tm tm_struct{};
            tm_struct.tm_year = y - 1900;
            tm_struct.tm_mon = m;
            tm_struct.tm_mday = d;
            tm_struct.tm_hour = h;
            tm_struct.tm_min = min;
            tm_struct.tm_sec = sec;
#ifdef _WIN32
            std::time_t t_val = _mkgmtime(&tm_struct);
#else
            std::time_t t_val = timegm(&tm_struct);
#endif
            epoch_ms = static_cast<double>(t_val) * 1000.0 + ms;
        }

        instance->SetNamedProperty("__time__", TaggedValue::FromDouble(epoch_ms), next_sid);
        return TaggedValue::FromHeapObject(instance);
    });

    date_proto->SetNamedProperty("getTime", TaggedValue::FromHeapObject(
        make_native("getTime", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromDouble(0.0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            if (inst->HasNamedProperty("__time__")) return inst->GetNamedProperty("__time__");
            return TaggedValue::FromDouble(0.0);
        })), next_sid);

    date_proto->SetNamedProperty("valueOf", TaggedValue::FromHeapObject(
        make_native("valueOf", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromDouble(0.0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            if (inst->HasNamedProperty("__time__")) return inst->GetNamedProperty("__time__");
            return TaggedValue::FromDouble(0.0);
        })), next_sid);

    date_proto->SetNamedProperty("toISOString", TaggedValue::FromHeapObject(
        make_native("toISOString", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            BrokenDownDate d = EpochMsToUTC(t);

            std::ostringstream ss;
            ss << std::setfill('0') << std::setw(4) << d.year << "-"
               << std::setw(2) << d.month << "-"
               << std::setw(2) << d.day << "T"
               << std::setw(2) << d.hour << ":"
               << std::setw(2) << d.minute << ":"
               << std::setw(2) << d.second << "."
               << std::setw(3) << d.ms << "Z";
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(ss.str()));
        })), next_sid);

    date_proto->SetNamedProperty("getFullYear", TaggedValue::FromHeapObject(
        make_native("getFullYear", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(1970);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).year);
        })), next_sid);

    date_proto->SetNamedProperty("getMonth", TaggedValue::FromHeapObject(
        make_native("getMonth", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).month - 1); // 0-indexed
        })), next_sid);

    date_proto->SetNamedProperty("getDate", TaggedValue::FromHeapObject(
        make_native("getDate", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(1);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).day);
        })), next_sid);

    date_proto->SetNamedProperty("getDay", TaggedValue::FromHeapObject(
        make_native("getDay", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).weekday);
        })), next_sid);

    date_proto->SetNamedProperty("getHours", TaggedValue::FromHeapObject(
        make_native("getHours", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).hour);
        })), next_sid);

    date_proto->SetNamedProperty("getMinutes", TaggedValue::FromHeapObject(
        make_native("getMinutes", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).minute);
        })), next_sid);

    date_proto->SetNamedProperty("getSeconds", TaggedValue::FromHeapObject(
        make_native("getSeconds", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::FromSmi(0);
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            return TaggedValue::FromSmi(EpochMsToUTC(t).second);
        })), next_sid);

    // Localized Date prototype methods per ECMA-402
    date_proto->SetNamedProperty("toLocaleString", TaggedValue::FromHeapObject(
        make_native("toLocaleString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            DateTimeFormatOptions opts;
            if (!args.empty() && !args[0].IsUndefined()) {
                auto canon = CanonicalizeLocaleList(args[0]);
                if (!canon.empty()) opts.locale = canon[0];
            }
            opts.date_style = "short";
            opts.time_style = "medium";
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(FormatDateTimeInternal(t, opts)));
        })), next_sid);

    date_proto->SetNamedProperty("toLocaleDateString", TaggedValue::FromHeapObject(
        make_native("toLocaleDateString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            DateTimeFormatOptions opts;
            if (!args.empty() && !args[0].IsUndefined()) {
                auto canon = CanonicalizeLocaleList(args[0]);
                if (!canon.empty()) opts.locale = canon[0];
            }
            if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
                auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
                if (o->HasNamedProperty("dateStyle")) opts.date_style = o->GetNamedProperty("dateStyle").ToString();
            } else {
                opts.date_style = "short";
            }
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(FormatDateTimeInternal(t, opts)));
        })), next_sid);

    date_proto->SetNamedProperty("toLocaleTimeString", TaggedValue::FromHeapObject(
        make_native("toLocaleTimeString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            DateTimeFormatOptions opts;
            if (!args.empty() && !args[0].IsUndefined()) {
                auto canon = CanonicalizeLocaleList(args[0]);
                if (!canon.empty()) opts.locale = canon[0];
            }
            opts.time_style = "medium";
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(FormatDateTimeInternal(t, opts)));
        })), next_sid);

    date_proto->SetNamedProperty("toString", TaggedValue::FromHeapObject(
        make_native("toString", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) return TaggedValue::Undefined();
            auto* inst = static_cast<JSObject*>(this_val.AsHeapObject());
            double t = inst->HasNamedProperty("__time__") ? inst->GetNamedProperty("__time__").AsNumber() : 0.0;
            DateTimeFormatOptions opts;
            opts.locale = "en-US";
            opts.date_style = "full";
            opts.time_style = "medium";
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(FormatDateTimeInternal(t, opts)));
        })), next_sid);

    // Static Date methods
    date_ctor->SetNamedProperty("now", TaggedValue::FromHeapObject(
        make_native("now", [](const std::vector<TaggedValue>&) -> TaggedValue {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return TaggedValue::FromDouble(static_cast<double>(ms));
        })), next_sid);

    date_ctor->SetNamedProperty("parse", TaggedValue::FromHeapObject(
        make_native("parse", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
            std::string s = args[0].ToString();
            int y = 1970, m = 1, d = 1, h = 0, min = 0, sec = 0;
            char dash1, dash2, t, colon1, colon2;
            std::istringstream ss(s);
            if (ss >> y >> dash1 >> m >> dash2 >> d) {
                if (ss >> t >> h >> colon1 >> min) {
                    ss >> colon2 >> sec;
                }
                std::tm tm_struct{};
                tm_struct.tm_year = y - 1900;
                tm_struct.tm_mon = m - 1;
                tm_struct.tm_mday = d;
                tm_struct.tm_hour = h;
                tm_struct.tm_min = min;
                tm_struct.tm_sec = sec;
#ifdef _WIN32
                std::time_t t_val = _mkgmtime(&tm_struct);
#else
                std::time_t t_val = timegm(&tm_struct);
#endif
                return TaggedValue::FromDouble(static_cast<double>(t_val) * 1000.0);
            }
            return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
        })), next_sid);

    date_ctor->SetNamedProperty("UTC", TaggedValue::FromHeapObject(
        make_native("UTC", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            int y = args.empty() ? 1970 : static_cast<int>(args[0].AsNumber());
            int m = (args.size() > 1) ? static_cast<int>(args[1].AsNumber()) : 0;
            int d = (args.size() > 2) ? static_cast<int>(args[2].AsNumber()) : 1;
            int h = (args.size() > 3) ? static_cast<int>(args[3].AsNumber()) : 0;
            int min = (args.size() > 4) ? static_cast<int>(args[4].AsNumber()) : 0;
            int sec = (args.size() > 5) ? static_cast<int>(args[5].AsNumber()) : 0;
            int ms = (args.size() > 6) ? static_cast<int>(args[6].AsNumber()) : 0;

            if (y < 100 && y >= 0) y += 1900;

            std::tm tm_struct{};
            tm_struct.tm_year = y - 1900;
            tm_struct.tm_mon = m;
            tm_struct.tm_mday = d;
            tm_struct.tm_hour = h;
            tm_struct.tm_min = min;
            tm_struct.tm_sec = sec;
#ifdef _WIN32
            std::time_t t_val = _mkgmtime(&tm_struct);
#else
            std::time_t t_val = timegm(&tm_struct);
#endif
            return TaggedValue::FromDouble(static_cast<double>(t_val) * 1000.0 + ms);
        })), next_sid);

    date_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(date_proto), next_sid);
    date_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(date_ctor), next_sid);
    realm.GetGlobalObject()->SetNamedProperty("Date", TaggedValue::FromHeapObject(date_ctor), next_sid);
}

void InitializeNumber(Realm& realm) {
    Shape* root = realm.GetRootShape();
    uint32_t& next_sid = realm.NextShapeId();

    auto make_native = [&realm, root](std::string_view name, auto cb) -> JSNativeFunction* {
        return AllocateRuntime<JSNativeFunction>(name, cb, root);
    };

    auto* number_proto = AllocateRuntime<JSObject>(root);
    realm.SetNumberPrototype(number_proto);

    auto* number_ctor = make_native("Number", [number_proto, root, &next_sid](const std::vector<TaggedValue>& args) -> TaggedValue {
        double d = args.empty() ? 0.0 : args[0].AsNumber();
        return TaggedValue::FromDouble(d);
    });

    // Number.prototype.toLocaleString
    number_proto->SetNamedProperty("toLocaleString", TaggedValue::FromHeapObject(
        make_native("toLocaleString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            double val = this_val.AsNumber();
            NumberFormatOptions opts;
            if (!args.empty() && !args[0].IsUndefined()) {
                auto canon = CanonicalizeLocaleList(args[0]);
                if (!canon.empty()) opts.locale = canon[0];
            }
            if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsJSObject()) {
                auto* o = static_cast<JSObject*>(args[1].AsHeapObject());
                if (o->HasNamedProperty("style")) opts.style = o->GetNamedProperty("style").ToString();
                if (o->HasNamedProperty("currency")) opts.currency = ToUpperAscii(o->GetNamedProperty("currency").ToString());
                if (o->HasNamedProperty("currencyDisplay")) opts.currency_display = o->GetNamedProperty("currencyDisplay").ToString();
                if (o->HasNamedProperty("useGrouping")) opts.use_grouping = o->GetNamedProperty("useGrouping").ToBoolean();
                if (o->HasNamedProperty("minimumFractionDigits")) {
                    opts.minimum_fraction_digits = static_cast<int>(o->GetNamedProperty("minimumFractionDigits").AsNumber());
                }
                if (o->HasNamedProperty("maximumFractionDigits")) {
                    opts.maximum_fraction_digits = static_cast<int>(o->GetNamedProperty("maximumFractionDigits").AsNumber());
                }
            }
            std::string res = FormatNumberInternal(val, opts);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        })), next_sid);

    number_proto->SetNamedProperty("toFixed", TaggedValue::FromHeapObject(
        make_native("toFixed", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            double val = this_val.AsNumber();
            int digits = args.empty() ? 0 : static_cast<int>(args[0].AsNumber());
            if (digits < 0) digits = 0;
            if (digits > 100) digits = 100;

            std::ostringstream ss;
            ss << std::fixed << std::setprecision(digits) << val;
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(ss.str()));
        })), next_sid);

    number_proto->SetNamedProperty("valueOf", TaggedValue::FromHeapObject(
        make_native("valueOf", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
            return TaggedValue::FromDouble(this_val.AsNumber());
        })), next_sid);

    number_proto->SetNamedProperty("toString", TaggedValue::FromHeapObject(
        make_native("toString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            double val = this_val.AsNumber();
            if (std::isnan(val)) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("NaN"));
            if (std::isinf(val)) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(val < 0 ? "-Infinity" : "Infinity"));
            if (val == 0.0) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("0"));

            if (args.empty() || args[0].AsNumber() == 10) {
                if (this_val.IsSmi()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::to_string(this_val.AsSmi())));
                std::ostringstream ss;
                ss << val;
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(ss.str()));
            }
            int radix = static_cast<int>(args[0].AsNumber());
            if (radix == 16) {
                std::ostringstream ss;
                ss << std::hex << static_cast<int64_t>(val);
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(ss.str()));
            }
            std::ostringstream ss;
            ss << val;
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(ss.str()));
        })), next_sid);

    // Number constants and static methods
    number_ctor->SetNamedProperty("MAX_VALUE", TaggedValue::FromDouble(1.7976931348623157e+308), next_sid);
    number_ctor->SetNamedProperty("MIN_VALUE", TaggedValue::FromDouble(5e-324), next_sid);
    number_ctor->SetNamedProperty("NaN", TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN()), next_sid);
    number_ctor->SetNamedProperty("POSITIVE_INFINITY", TaggedValue::FromDouble(std::numeric_limits<double>::infinity()), next_sid);
    number_ctor->SetNamedProperty("NEGATIVE_INFINITY", TaggedValue::FromDouble(-std::numeric_limits<double>::infinity()), next_sid);
    number_ctor->SetNamedProperty("EPSILON", TaggedValue::FromDouble(2.220446049250313e-16), next_sid);
    number_ctor->SetNamedProperty("MAX_SAFE_INTEGER", TaggedValue::FromDouble(9007199254740991.0), next_sid);
    number_ctor->SetNamedProperty("MIN_SAFE_INTEGER", TaggedValue::FromDouble(-9007199254740991.0), next_sid);

    number_ctor->SetNamedProperty("isNaN", TaggedValue::FromHeapObject(
        make_native("isNaN", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty()) return TaggedValue::FromBoolean(false);
            if (!args[0].IsNumber()) return TaggedValue::FromBoolean(false);
            return TaggedValue::FromBoolean(std::isnan(args[0].AsNumber()));
        })), next_sid);

    number_ctor->SetNamedProperty("isFinite", TaggedValue::FromHeapObject(
        make_native("isFinite", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty()) return TaggedValue::FromBoolean(false);
            if (!args[0].IsNumber()) return TaggedValue::FromBoolean(false);
            return TaggedValue::FromBoolean(std::isfinite(args[0].AsNumber()));
        })), next_sid);

    number_ctor->SetNamedProperty("isInteger", TaggedValue::FromHeapObject(
        make_native("isInteger", [](const std::vector<TaggedValue>& args) -> TaggedValue {
            if (args.empty() || !args[0].IsNumber()) return TaggedValue::FromBoolean(false);
            double d = args[0].AsNumber();
            return TaggedValue::FromBoolean(std::isfinite(d) && std::floor(d) == d);
        })), next_sid);

    number_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(number_proto), next_sid);
    number_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(number_ctor), next_sid);
    realm.GetGlobalObject()->SetNamedProperty("Number", TaggedValue::FromHeapObject(number_ctor), next_sid);

    // String.prototype.localeCompare
    if (realm.GetStringPrototype()) {
        realm.GetStringPrototype()->SetNamedProperty("localeCompare", TaggedValue::FromHeapObject(
            make_native("localeCompare", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
                std::string a = this_val.ToString();
                std::string b = args.empty() ? "undefined" : args[0].ToString();

                CollatorOptions opts;
                if (args.size() > 1 && !args[1].IsUndefined()) {
                    auto canon = CanonicalizeLocaleList(args[1]);
                    if (!canon.empty()) opts.locale = canon[0];
                }
                if (args.size() > 2 && args[2].IsHeapObject() && args[2].AsHeapObject()->IsJSObject()) {
                    auto* o = static_cast<JSObject*>(args[2].AsHeapObject());
                    if (o->HasNamedProperty("sensitivity")) opts.sensitivity = o->GetNamedProperty("sensitivity").ToString();
                    if (o->HasNamedProperty("numeric")) opts.numeric = o->GetNamedProperty("numeric").ToBoolean();
                }

                bool case_sensitive = (opts.sensitivity == "case" || opts.sensitivity == "variant");
                int cmp = NaturalCompare(a, b, opts.numeric, case_sensitive);
                return TaggedValue::FromSmi(cmp);
            })), next_sid);
    }

    // String Constructor on global if not already present
    auto* string_ctor = make_native("String", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        std::string s = args.empty() ? "" : args[0].ToString();
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    });
    if (realm.GetStringPrototype()) {
        string_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(realm.GetStringPrototype()), next_sid);
        realm.GetStringPrototype()->SetNamedProperty("constructor", TaggedValue::FromHeapObject(string_ctor), next_sid);
    }
    realm.GetGlobalObject()->SetNamedProperty("String", TaggedValue::FromHeapObject(string_ctor), next_sid);
}

} // namespace aether
