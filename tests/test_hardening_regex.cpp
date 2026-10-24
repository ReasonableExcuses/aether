#include "aether/bounded_regex.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace aether;

namespace {

size_t checks = 0;

void Check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

template <typename Action>
void ExpectError(RegexErrorKind kind, Action action, std::string_view description) {
    try {
        action();
    } catch (const RegexError& error) {
        Check(error.ErrorKind() == kind, description);
        Check(error.what()[0] != '\0', "errors include an explanatory message");
        return;
    }
    Check(false, description);
}

void CheckRange(const std::optional<RegexRange>& actual, size_t begin, size_t end,
                std::string_view description) {
    Check(actual && actual->begin == begin && actual->end == end, description);
}

RegexMatch Match(std::string_view pattern, std::string_view input,
                 std::string_view flags = {}, size_t start = 0) {
    auto result = RegexSearch(pattern, flags, input, start);
    Check(result.has_value(), std::string("expected match: ") + std::string(pattern));
    return *result;
}

void TestCommonMatching() {
    auto match = Match(R"((ab)(?:c|d)(\d+))", "--abc123!");
    Check(match.captures.size() == 3, "noncapturing groups do not get a slot");
    CheckRange(match.captures[0], 2, 8, "full match offsets");
    CheckRange(match.captures[1], 2, 4, "first capture offsets");
    CheckRange(match.captures[2], 5, 8, "second capture offsets");

    match = Match("(a)|(bc)|(def)", "def");
    CheckRange(match.captures[0], 0, 3, "third alternative");
    Check(!match.captures[1] && !match.captures[2], "unmatched alternatives are unset");
    CheckRange(match.captures[3], 0, 3, "third capture");
    CheckRange(Match("a|ab", "ab").captures[0], 0, 1, "leftmost-first alternatives");
    CheckRange(Match("(?:a|ab)c", "abc").captures[0], 0, 3, "alternative backtracking");
    CheckRange(Match("cat|dog|fox|ant", "--ant").captures[0], 2, 5, "four alternatives");
    CheckRange(Match("a|", "x").captures[0], 0, 0, "empty last alternative");
    CheckRange(Match("|a", "a").captures[0], 0, 0, "empty first alternative");
    CheckRange(Match(R"(a\+b\.\[c\]\/)", "a+b.[c]/").captures[0], 0, 8,
               "escaped metacharacters");
    CheckRange(Match(R"(\x41\u0042\cC)", std::string("AB\x03", 3)).captures[0],
               0, 3, "hex Unicode-byte and control escapes");
    CheckRange(Match(R"(\n\r\t\f\v\0)", std::string("\n\r\t\f\v\0", 6)).captures[0],
               0, 6, "control and NUL escapes");
    CheckRange(Match(std::string("a\0b", 3), std::string("xa\0b", 4)).captures[0],
               1, 4, "embedded NUL bytes are not string terminators");
    Check(!RegexSearch("z", "", "abcdef"), "a failed search returns nullopt");
    CheckRange(Match("a", "aba", "", 1).captures[0], 2, 3, "start offset skips earlier match");
    Check(!RegexSearch("", "", "a", 2), "start beyond input returns no match");
    Check(!RegexSearch("", "", "", std::numeric_limits<size_t>::max()),
          "huge offset does not overflow");
}

void TestClassesAndFlags() {
    CheckRange(Match("[a-c]+[^0-9]", "--abc!").captures[0], 2, 6,
               "ranges negation and greedy classes");
    CheckRange(Match(R"(\d\D\w\W\s\S)", "1a_! X").captures[0], 0, 6,
               "predefined classes and complements");
    CheckRange(Match(R"([\D]+)", "ab!12").captures[0], 0, 3,
               "class complement inside brackets");
    CheckRange(Match(R"([^\W]+)", "--a_2!").captures[0], 2, 5,
               "negation of a complemented class");
    CheckRange(Match(R"([\dA-F]+)", "--A92Fz").captures[0], 2, 6,
               "class range and category union");
    CheckRange(Match("[-a]+", "a--").captures[0], 0, 3, "leading class hyphen");
    CheckRange(Match("[a-]+", "a--").captures[0], 0, 3, "trailing class hyphen");
    Check(!RegexSearch("[]", "", "anything"), "empty class never matches");
    CheckRange(Match("[^]", "\n").captures[0], 0, 1, "negated empty class matches newline");
    CheckRange(Match(R"([\b])", "\b").captures[0], 0, 1, "class backspace");
    CheckRange(Match("abc[a-z]+", "--AbCDeF", "ig").captures[0], 2, 8,
               "case-insensitive literal and class matching");
    Check(!RegexSearch("[^a]", "i", "A"), "case folding before class negation");
    Check(!RegexSearch("a.b", "", "a\nb"), "dot excludes line terminators");
    CheckRange(Match("a.b", "a\nb", "s").captures[0], 0, 3, "dotAll flag");
    Check(!RegexSearch("^abc", "", "xabc", 1), "anchor uses whole input not suffix");
    Check(!RegexSearch("abc$", "", "abc\n"), "non-multiline dollar means end of input");
    CheckRange(Match("^abc$", "x\nabc\ny", "m").captures[0], 2, 5, "multiline anchors");
    CheckRange(Match("^b$", "a\rb\r", "m").captures[0], 2, 3, "CR multiline anchors");
    CheckRange(Match(R"(\bword\b)", "a word!").captures[0], 2, 6, "word boundaries");
    CheckRange(Match(R"(\Boo\B)", "food").captures[0], 1, 3, "non-word boundaries");
    Check(!RegexSearch(R"(\ba)", "", "ba", 1), "word boundary sees prefix before offset");
    Check(!RegexSearch("a", "y", "ba", 0), "sticky search cannot skip a byte");
    CheckRange(Match("a", "ba", "y", 1).captures[0], 1, 2, "sticky matches at offset");
}

void TestQuantifiersAndCaptures() {
    CheckRange(Match("a*", "aaa").captures[0], 0, 3, "greedy star");
    CheckRange(Match("a*?", "aaa").captures[0], 0, 0, "lazy star");
    CheckRange(Match("a+?", "aaa").captures[0], 0, 1, "lazy plus");
    CheckRange(Match("a??a", "aa").captures[0], 0, 1, "lazy optional");
    CheckRange(Match("a?a", "aa").captures[0], 0, 2, "greedy optional");
    CheckRange(Match("a{2,4}", "aaaaa").captures[0], 0, 4, "bounded greedy repeat");
    CheckRange(Match("a{2,4}?", "aaaaa").captures[0], 0, 2, "bounded lazy repeat");
    CheckRange(Match("a{2,}", "aaaa").captures[0], 0, 4, "open upper quantifier");
    CheckRange(Match("a{2}", "aaa").captures[0], 0, 2, "exact quantifier");
    CheckRange(Match("a{0}b", "b").captures[0], 0, 1, "zero-count quantifier");
    Check(!RegexSearch("a{3}", "", "aa"), "required count cannot be skipped");
    CheckRange(Match("a*a", "aaa").captures[0], 0, 3, "greedy repeat can give back a byte");
    CheckRange(Match("a*?b", "aaab").captures[0], 0, 4, "lazy repeat expands as needed");

    auto match = Match("(a(b)?)+", "aba");
    CheckRange(match.captures[1], 2, 3, "outer repeat keeps last capture");
    Check(!match.captures[2], "nested capture is cleared on each iteration");
    match = Match("(a|(b))+", "ba");
    CheckRange(match.captures[1], 1, 2, "capture reset across alternatives in a repeat");
    Check(!match.captures[2], "skipped capture in last alternative is unset");
    match = Match("(a+)(a)", "aaa");
    CheckRange(match.captures[1], 0, 2, "capture restored after greedy backtracking");
    CheckRange(match.captures[2], 2, 3, "capture after greedy backtracking");
    match = Match("(a(b)|ac)d", "acd");
    CheckRange(match.captures[1], 0, 2, "capture restored after alternative failure");
    Check(!match.captures[2], "failed branch does not leak capture");
    Check(BoundedRegex("(a)(?:b)(c)").CaptureCount() == 2, "compiled capture count");
}

void TestEmptyProgress() {
    auto match = Match("()", "");
    CheckRange(match.captures[0], 0, 0, "empty full match");
    CheckRange(match.captures[1], 0, 0, "participating empty capture is set");
    match = Match("(a*)*", "");
    Check(!match.captures[1], "optional empty iteration must not set captures");
    CheckRange(Match("(a*)+", "").captures[1], 0, 0, "required empty iteration sets capture");
    CheckRange(Match("(a*){2}", "").captures[1], 0, 0, "multiple required empty iterations");
    CheckRange(Match("(?:|a)*b", "ab").captures[0], 0, 2,
               "empty iteration failure can backtrack to nonempty alternative");
    CheckRange(Match("(?:|a){1,2}", "a").captures[0], 0, 1,
               "required empty iteration may be followed by a consuming alternative");
    CheckRange(Match("(?:(?:)*)*", "").captures[0], 0, 0, "nested empty stars terminate");
    CheckRange(Match("(?:^)*", "").captures[0], 0, 0, "grouped empty assertion repeat");

    RegexBudget budget;
    const auto regex = BoundedRegex::Compile("", "g", budget);
    std::vector<size_t> positions;
    std::optional<size_t> offset = 0;
    while (offset) {
        auto result = regex.Search("ab", *offset, budget);
        Check(result.has_value(), "global empty match exists");
        positions.push_back(result->Start());
        offset = NextRegexSearchOffset(*result, 2);
    }
    Check(positions == std::vector<size_t>({0, 1, 2}), "global empty matches make progress");
    match = Match("a", "ab");
    Check(NextRegexSearchOffset(match, 2) == std::optional<size_t>(1),
          "nonempty match continues at its end without skipping");
    Check(!NextRegexSearchOffset(RegexMatch{}, 0), "progress helper handles absent full match");
    Check(!NextRegexSearchOffset(Match("$", "ab"), 2), "empty end match terminates iteration");
}

void TestInvalidAndUnsupported() {
    for (const std::string_view pattern : {
             "(", ")", "[", "[z-a]", "\\", "*a", "+", "?", "a**", "a+*",
             "a???", "a{2,1}", "a{,2}", "a{1", "a{1,x}", "a{1,2", "^*", "$?",
             R"(\b+)", R"(\x0Z)", R"(\u00q0)", R"(\c1)", R"([a-\d])"}) {
        ExpectError(RegexErrorKind::InvalidPattern,
                    [pattern] { (void)BoundedRegex(pattern); }, pattern);
    }
    for (const std::string_view pattern : {
             "a(?=b)", "a(?!b)", "(?<=a)b", "(?<!a)b", "(?<name>a)", "(?i:a)",
             R"((a)\1)", R"(\k<name>)", R"(\p{Letter})", R"(\P{ASCII})",
             R"(\u{1234})", R"(\u0100)", R"(\012)"}) {
        ExpectError(RegexErrorKind::UnsupportedPattern,
                    [pattern] { (void)BoundedRegex(pattern); }, pattern);
    }
    for (const std::string_view flags : {"ii", "gg", "q", "z"}) {
        ExpectError(RegexErrorKind::InvalidPattern,
                    [flags] { (void)BoundedRegex("a", flags); }, "invalid flags");
    }
    for (const std::string_view flags : {"u", "v", "d"}) {
        ExpectError(RegexErrorKind::UnsupportedPattern,
                    [flags] { (void)BoundedRegex("a", flags); }, "unsupported flags");
    }
}

void TestBudgets() {
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [] { (void)RegexSearch("", "", "", 0, 0); }, "zero budget fails even for empty regex");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [] { (void)BoundedRegex(std::string(2000, 'a'), "", 32); },
                "parsing consumes a real budget");

    const BoundedRegex catastrophic("^(a+)+$");
    const std::string hostile = std::string(40, 'a') + '!';
    for (int attempt = 0; attempt != 3; ++attempt) {
        RegexBudget budget(500);
        ExpectError(RegexErrorKind::StepBudgetExceeded,
                    [&] { (void)catastrophic.Search(hostile, 0, budget); },
                    "catastrophic backtracking deterministically exhausts tiny budget");
        Check(budget.Remaining() == 0, "exhausted budget stays at zero");
        ExpectError(RegexErrorKind::StepBudgetExceeded,
                    [&] { (void)catastrophic.Search("a", 0, budget); },
                    "exhausted shared budget cannot be reused");
    }
    CheckRange(catastrophic.Search("aaaa")->captures[0], 0, 4,
               "same nested quantifier succeeds for a valid input");

    const BoundedRegex scan("z");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [&] { (void)scan.Search(std::string(10000, 'a'), 0, 100); },
                "candidate scanning is budgeted");
    const BoundedRegex class_scan("[" + std::string(200, 'a') + "]");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [&] { (void)class_scan.Search("z", 0, 30); }, "class member scanning is budgeted");
    const BoundedRegex empty_repeat("(?:){1000000}");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [&] { (void)empty_repeat.Search("", 0, 100); },
                "large quantifier is not expanded and consumes execution steps");

    const BoundedRegex one("a");
    RegexBudget measuring;
    Check(one.Search("aa", 0, measuring).has_value(), "measure one search");
    const size_t first_cost = kMaxRegexSteps - measuring.Remaining();
    RegexBudget exact(first_cost);
    Check(one.Search("aa", 0, exact).has_value(), "exact budget is sufficient");
    Check(exact.Exhausted(), "exact budget consumed completely");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [&] { (void)one.Search("aa", 1, exact); }, "second search shares consumed budget");

    RegexBudget compile_and_run;
    (void)RegexSearch("a", "g", "aa", 0, compile_and_run);
    const size_t total_cost = kMaxRegexSteps - compile_and_run.Remaining();
    RegexBudget shared(total_cost);
    (void)RegexSearch("a", "g", "aa", 0, shared);
    Check(shared.Exhausted(), "convenience API charges both parse and run");
    ExpectError(RegexErrorKind::StepBudgetExceeded,
                [&] { (void)RegexSearch("a", "g", "aa", 1, shared); },
                "convenience repeated search cannot reset shared budget");
}

void TestLimitsAndStackSafety() {
    const std::string too_deep = std::string(kMaxBoundedRegexNesting + 1, '(');
    // Use noncapturing groups to reach nesting rather than the capture limit.
    std::string nested;
    for (size_t i = 0; i < kMaxBoundedRegexNesting; ++i) nested += "(?:";
    nested += 'a';
    nested.append(kMaxBoundedRegexNesting, ')');
    CheckRange(Match(nested, "a").captures[0], 0, 1, "maximum nesting is parsed iteratively");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [&] { (void)BoundedRegex("(?:" + nested + ")"); }, "nesting limit");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [&] { (void)BoundedRegex(too_deep); }, "capture limit before native recursion");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [] { (void)BoundedRegex(std::string(kMaxBoundedRegexPatternLength + 1, 'a')); },
                "pattern byte length limit");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [] { (void)BoundedRegex("a{1000001}"); }, "quantifier bound");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [] { (void)BoundedRegex("a{999999999999999999999999999999999}"); },
                "quantifier integer overflow is prevented");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [] { (void)BoundedRegex("[" + std::string(kMaxBoundedRegexClassRanges + 1, 'a') + "]"); },
                "class member limit");

    // Use actual storage: a fabricated oversized string_view would violate its
    // range precondition even though Search rejects it before accessing bytes.
    const BoundedRegex regex("a");
    const std::string oversized(kMaxBoundedRegexInputLength + 1, 'a');
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [&] { (void)regex.Search(oversized); }, "input byte length limit");
    CheckRange(Match("^a{20000}$", std::string(20000, 'a')).captures[0], 0, 20000,
               "long input uses no per-character native recursion");

    // Force many live snapshots with many captures, rather than attempting an
    // allocator OOM. An unlimited step budget still has a hard heap bound.
    std::string captures;
    for (size_t i = 0; i < kMaxBoundedRegexCaptures; ++i) captures += "()";
    const BoundedRegex memory_limited(captures + "a*");
    ExpectError(RegexErrorKind::ResourceLimitExceeded,
                [&] { (void)memory_limited.Search(std::string(10000, 'a'), 0,
                    std::numeric_limits<size_t>::max()); }, "live snapshot memory limit");
}

// A line-oriented probe for differential verification against JavaScript.
// Each line: hex(pattern) hex(flags) hex(input) start; '-' encodes empty.
// Output: null, or semicolon-separated begin,end / '-' capture ranges.
std::string DecodeHex(std::string_view value) {
    if (value == "-") return {};
    std::string decoded;
    for (size_t i = 0; i < value.size(); i += 2) {
        decoded.push_back(static_cast<char>(std::stoul(std::string(value.substr(i, 2)), nullptr, 16)));
    }
    return decoded;
}

int Probe() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream fields(line);
        std::string pattern, flags, input;
        size_t start = 0;
        if (!(fields >> pattern >> flags >> input >> start)) return 2;
        try {
            const auto match = RegexSearch(DecodeHex(pattern), DecodeHex(flags), DecodeHex(input), start);
            if (!match) {
                std::cout << "null\n";
                continue;
            }
            for (size_t i = 0; i < match->captures.size(); ++i) {
                if (i) std::cout << ';';
                if (match->captures[i]) {
                    std::cout << match->captures[i]->begin << ',' << match->captures[i]->end;
                } else {
                    std::cout << '-';
                }
            }
            std::cout << '\n';
        } catch (const RegexError& error) {
            std::cout << "error:" << static_cast<int>(error.ErrorKind()) << '\n';
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--probe") return Probe();
    TestCommonMatching();
    TestClassesAndFlags();
    TestQuantifiersAndCaptures();
    TestEmptyProgress();
    TestInvalidAndUnsupported();
    TestBudgets();
    TestLimitsAndStackSafety();
    std::cout << "PASS: bounded regex hardening (" << checks << " checks)\n";
    return 0;
}
