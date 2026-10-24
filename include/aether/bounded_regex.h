#ifndef AETHER_BOUNDED_REGEX_H
#define AETHER_BOUNDED_REGEX_H

#include "aether/common.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aether {

/// Hard limits for the portable bounded regex implementation.
inline constexpr size_t kMaxBoundedRegexPatternLength = 1ULL << 20;
inline constexpr size_t kMaxBoundedRegexInputLength = kMaxStringLength;
inline constexpr size_t kMaxBoundedRegexNesting = 1024;
inline constexpr size_t kMaxBoundedRegexCaptures = 256;
inline constexpr size_t kMaxBoundedRegexQuantifier = 1'000'000;
inline constexpr size_t kMaxBoundedRegexInstructions = 1ULL << 20;
inline constexpr size_t kMaxBoundedRegexBacktrackStates = 262'144;
inline constexpr size_t kMaxBoundedRegexCallDepth = 4096;
inline constexpr size_t kMaxBoundedRegexClassRanges = 8192;
inline constexpr size_t kMaxBoundedRegexStateBytes = 32ULL * 1024ULL * 1024ULL;

enum class RegexErrorKind {
    InvalidPattern,
    UnsupportedPattern,
    StepBudgetExceeded,
    ResourceLimitExceeded,
};

/**
 * Error raised by parsing or bounded execution.
 *
 * No path in the implementation falls back to std::regex. Callers can map
 * InvalidPattern/UnsupportedPattern to a JavaScript SyntaxError and
 * StepBudgetExceeded/ResourceLimitExceeded to a JavaScript RangeError.
 */
class RegexError final : public std::runtime_error {
public:
    RegexError(RegexErrorKind kind, std::string message,
               size_t position = std::string_view::npos);

    RegexErrorKind ErrorKind() const noexcept { return kind_; }
    size_t Position() const noexcept { return position_; }

private:
    RegexErrorKind kind_;
    size_t position_;
};

/** A consumable execution/parse budget that can be shared by many searches. */
class RegexBudget {
public:
    explicit RegexBudget(size_t steps_limit = kMaxRegexSteps) noexcept
        : remaining_(steps_limit) {}

    size_t Remaining() const noexcept { return remaining_; }
    bool Exhausted() const noexcept { return remaining_ == 0; }

    /// Consumes real parser or VM work and throws when the budget is exhausted.
    void Consume(size_t steps = 1);

private:
    size_t remaining_;
};

/// Half-open byte range [begin, end) in the original, unsliced input.
struct RegexRange {
    size_t begin = 0;
    size_t end = 0;
};

/**
 * A successful search result. captures[0] is the full match; captures[n]
 * contains the byte range for capture group n, or nullopt when that group did
 * not participate in the selected alternative.
 */
struct RegexMatch {
    std::vector<std::optional<RegexRange>> captures;

    const RegexRange& Full() const noexcept { return *captures[0]; }
    size_t Start() const noexcept { return Full().begin; }
    size_t End() const noexcept { return Full().end; }
};

/**
 * Immutable parsed bounded regex program.
 *
 * Matching is byte-oriented, which is the same representation used by the
 * current AetherVM string APIs. Capture offsets are therefore byte/code-unit
 * offsets into the supplied std::string_view. Case folding and \d/\w/\s are
 * ASCII-only. This is not a Unicode/UTF-16 regex implementation. Supported
 * flags: i, m, s, y; g is accepted but iteration belongs to the caller.
 * Unsupported syntax is rejected: lookaround, backreferences, named groups,
 * Unicode properties/code points, u/v/d flags, inline modifiers, and legacy
 * octal escapes. Escaped Unicode values above 255 are rejected, not truncated.
 * Legacy permissive malformed brace/hex escapes are rejected rather than
 * interpreted as literals. \B in a character class is also rejected.
 * All parser and execution stacks are explicit heap vectors; no
 * pattern-controlled native recursion is used.
 */
class BoundedRegex {
public:
    explicit BoundedRegex(std::string_view pattern,
                          std::string_view flags = {},
                          size_t parse_steps_limit = kMaxRegexSteps);

    /** Compile while charging parsing and compilation to the supplied budget. */
    static BoundedRegex Compile(std::string_view pattern,
                                std::string_view flags,
                                RegexBudget& budget);

    size_t CaptureCount() const noexcept;

    std::optional<RegexMatch> Search(std::string_view input,
                                    size_t start_offset,
                                    RegexBudget& budget) const;

    std::optional<RegexMatch> Search(
        std::string_view input,
        size_t start_offset = 0,
        size_t steps_limit = kMaxRegexSteps) const;

private:
    struct Impl;

    explicit BoundedRegex(std::shared_ptr<const Impl> impl) noexcept;

    std::shared_ptr<const Impl> impl_;
};

/**
 * Convenience API requested by runtime callers. The size-limit overload
 * charges both compilation and execution to one fresh budget. The budget
 * overload lets callers share one total budget over repeated searches.
 */
std::optional<RegexMatch> RegexSearch(
    std::string_view pattern,
    std::string_view flags,
    std::string_view input,
    size_t start_offset = 0,
    size_t steps_limit = kMaxRegexSteps);

std::optional<RegexMatch> RegexSearch(
    std::string_view pattern,
    std::string_view flags,
    std::string_view input,
    size_t start_offset,
    RegexBudget& budget);

/**
 * Progress for global matching/replacement. Nonempty matches continue at end;
 * empty matches advance one byte, or return nullopt when already at input end.
 * Search itself does not advance an empty result or maintain JS lastIndex.
 */
std::optional<size_t> NextRegexSearchOffset(const RegexMatch& match,
                                          size_t input_length) noexcept;

} // namespace aether

#endif // AETHER_BOUNDED_REGEX_H
