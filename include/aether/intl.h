#ifndef AETHER_INTL_H
#define AETHER_INTL_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace aether {

/**
 * @brief Parsed BCP 47 Language Tag according to RFC 5646 & ECMA-402 §6.2.2.
 */
struct BCP47Locale {
    std::string language{"und"};
    std::string script{""};
    std::string region{""};
    std::vector<std::string> variants;
    std::unordered_map<std::string, std::string> extensions; // e.g. "nu" -> "latn", "ca" -> "gregory"
    std::string privateuse{""};

    bool is_valid{false};
    std::string canonical_tag{""};

    static BCP47Locale Parse(std::string_view tag);
    static std::string Canonicalize(std::string_view tag);
    static bool IsValid(std::string_view tag);
};

/**
 * @brief ECMA-402 Intl.NumberFormat resolved configuration.
 */
struct NumberFormatOptions {
    std::string locale{"en-US"};
    std::string numbering_system{"latn"};
    std::string style{"decimal"}; // "decimal", "currency", "percent", "unit"
    std::string currency{""};     // e.g. "USD", "EUR"
    std::string currency_display{"symbol"}; // "symbol", "narrowSymbol", "code", "name"
    std::string currency_sign{"standard"};  // "standard", "accounting"
    std::string unit{""};
    std::string unit_display{"short"};
    bool use_grouping{true};
    int minimum_integer_digits{1};
    int minimum_fraction_digits{0};
    int maximum_fraction_digits{3};
    int minimum_significant_digits{0};
    int maximum_significant_digits{0};
    std::string notation{"standard"}; // "standard", "scientific", "engineering", "compact"
    std::string compact_display{"short"}; // "short", "long"
    std::string sign_display{"auto"}; // "auto", "always", "never", "exceptZero"
};

/**
 * @brief ECMA-402 Intl.DateTimeFormat resolved configuration.
 */
struct DateTimeFormatOptions {
    std::string locale{"en-US"};
    std::string calendar{"gregory"};
    std::string numbering_system{"latn"};
    std::string time_zone{"UTC"};
    bool hour12{true};
    bool hour12_specified{false};
    std::string date_style{""}; // "full", "long", "medium", "short"
    std::string time_style{""}; // "full", "long", "medium", "short"
    std::string weekday{""};
    std::string era{""};
    std::string year{""};
    std::string month{""};
    std::string day{""};
    std::string hour{""};
    std::string minute{""};
    std::string second{""};
    std::string time_zone_name{""};
};

/**
 * @brief ECMA-402 Intl.Collator resolved configuration.
 */
struct CollatorOptions {
    std::string locale{"en-US"};
    std::string usage{"sort"}; // "sort", "search"
    std::string sensitivity{"variant"}; // "base", "accent", "case", "variant"
    bool numeric{false};
    std::string case_first{"false"}; // "upper", "lower", "false"
    bool ignore_punctuation{false};
};

/**
 * @brief ECMA-402 Intl.PluralRules resolved configuration.
 */
struct PluralRulesOptions {
    std::string locale{"en-US"};
    std::string type{"cardinal"}; // "cardinal", "ordinal"
    int minimum_integer_digits{1};
    int minimum_fraction_digits{0};
    int maximum_fraction_digits{3};
};

/**
 * @brief ECMA-402 Intl.ListFormat resolved configuration.
 */
struct ListFormatOptions {
    std::string locale{"en-US"};
    std::string type{"conjunction"}; // "conjunction", "disjunction", "unit"
    std::string style{"long"}; // "long", "short", "narrow"
};

/**
 * @brief ECMA-402 Intl.RelativeTimeFormat resolved configuration.
 */
struct RelativeTimeFormatOptions {
    std::string locale{"en-US"};
    std::string numeric{"always"}; // "always", "auto"
    std::string style{"long"}; // "long", "short", "narrow"
};

/**
 * @brief ECMA-402 Intl.Segmenter resolved configuration.
 */
struct SegmenterOptions {
    std::string locale{"en-US"};
    std::string granularity{"grapheme"}; // "grapheme", "word", "sentence"
};

/**
 * @brief Initializes the complete ECMA-402 Intl namespace and constructors.
 */
void InitializeIntl(Realm& realm);

/**
 * @brief Initializes the Date constructor and prototype methods with Intl integration.
 */
void InitializeDate(Realm& realm);

/**
 * @brief Initializes the Number constructor and prototype methods with Intl integration.
 */
void InitializeNumber(Realm& realm);

/**
 * @brief Initializes the Boolean constructor and prototype methods.
 */
void InitializeBoolean(Realm& realm);

/**
 * @brief Initializes the BigInt constructor and prototype methods.
 */
void InitializeBigInt(Realm& realm);

} // namespace aether

#endif // AETHER_INTL_H
