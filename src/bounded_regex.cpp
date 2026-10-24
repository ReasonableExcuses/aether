#include "aether/bounded_regex.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aether {

RegexError::RegexError(RegexErrorKind kind, std::string message, size_t position)
    : std::runtime_error(std::move(message)), kind_(kind), position_(position) {}

void RegexBudget::Consume(size_t steps) {
    if (steps > remaining_) {
        remaining_ = 0;
        throw RegexError(RegexErrorKind::StepBudgetExceeded,
                         "bounded regular-expression step budget exhausted");
    }
    remaining_ -= steps;
}

namespace {

using NodeId = uint32_t;
constexpr NodeId kInvalidNode = std::numeric_limits<NodeId>::max();
constexpr size_t kUnbounded = std::numeric_limits<size_t>::max();

struct ByteRange {
    unsigned begin = 0;
    unsigned end = 0;
};

struct ClassSpec {
    bool negated = false;
    std::vector<ByteRange> ranges;

    bool Matches(unsigned char value, bool ignore_case, RegexBudget& budget) const {
        auto in_ranges = [this, &budget](unsigned char candidate) {
            for (const ByteRange& range : ranges) {
                budget.Consume();
                if (candidate >= range.begin && candidate <= range.end) {
                    return true;
                }
            }
            return false;
        };

        bool matched = in_ranges(value);
        if (ignore_case && value >= 'A' && value <= 'Z') {
            matched = matched || in_ranges(static_cast<unsigned char>(value + ('a' - 'A')));
        } else if (ignore_case && value >= 'a' && value <= 'z') {
            matched = matched || in_ranges(static_cast<unsigned char>(value - ('a' - 'A')));
        }
        return negated ? !matched : matched;
    }
};

enum class NodeKind : uint8_t {
    Empty,
    Literal,
    Any,
    CharacterClass,
    Begin,
    End,
    Boundary,
    Sequence,
    Alternation,
    Capture,
    Repeat,
};

struct Node {
    NodeKind kind = NodeKind::Empty;
    unsigned char literal = 0;
    ClassSpec character_class;
    std::vector<NodeId> children;
    uint32_t capture = 0;
    size_t minimum = 0;
    size_t maximum = 0;
    bool lazy = false;
    bool boundary_positive = true;
};

struct Ast {
    std::vector<Node> nodes;
    uint32_t capture_count = 0;
};

struct RegexFlags {
    bool ignore_case = false;
    bool global = false;
    bool multiline = false;
    bool dot_all = false;
    bool sticky = false;
};

RegexFlags ParseFlags(std::string_view flags) {
    if (flags.size() > 16) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression flags exceed the parser limit");
    }

    RegexFlags parsed;
    for (size_t i = 0; i < flags.size(); ++i) {
        bool* slot = nullptr;
        switch (flags[i]) {
        case 'i': slot = &parsed.ignore_case; break;
        case 'g': slot = &parsed.global; break;
        case 'm': slot = &parsed.multiline; break;
        case 's': slot = &parsed.dot_all; break;
        case 'y': slot = &parsed.sticky; break;
        case 'u':
        case 'v':
            throw RegexError(RegexErrorKind::UnsupportedPattern,
                             "Unicode regular-expression flags are unsupported", i);
        case 'd':
            throw RegexError(RegexErrorKind::UnsupportedPattern,
                             "the regexp indices flag is unsupported", i);
        default:
            throw RegexError(RegexErrorKind::InvalidPattern,
                             "unknown regular-expression flag", i);
        }
        if (*slot) {
            throw RegexError(RegexErrorKind::InvalidPattern,
                             "duplicate regular-expression flag", i);
        }
        *slot = true;
    }
    return parsed;
}

class Parser {
public:
    Parser(std::string_view pattern, RegexBudget& budget)
        : pattern_(pattern), budget_(budget) {
        if (pattern_.size() > kMaxBoundedRegexPatternLength) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression pattern is too large");
        }
        ast_.nodes.reserve(std::min<size_t>(pattern_.size() + 1, 256));
    }

    Ast Parse() {
        std::vector<Context> contexts;
        contexts.reserve(16);
        contexts.push_back(Context{});

        while (position_ < pattern_.size()) {
            budget_.Consume();
            const char current = pattern_[position_];

            if (current == '(') {
                ParseGroupOpen(contexts);
                continue;
            }
            if (current == ')') {
                ParseGroupClose(contexts);
                continue;
            }
            if (current == '|') {
                FinishSequence(contexts.back());
                ++position_;
                continue;
            }
            if (current == '*' || current == '+' || current == '?' || current == '{') {
                ParseQuantifier(contexts.back());
                continue;
            }

            NodeId atom = ParseAtom();
            AppendAtom(contexts.back(), atom);
        }

        if (contexts.size() != 1) {
            ThrowInvalid("unterminated capture or group");
        }
        FinishSequence(contexts.back());
        NodeId root = MakeAlternation(std::move(contexts.back().alternatives));
        if (root == kInvalidNode) {
            ThrowInvalid("empty regular-expression parse");
        }
        root_ = root;
        return std::move(ast_);
    }

    NodeId Root() const noexcept { return root_; }

private:
    struct Context {
        std::vector<NodeId> terms;
        std::vector<NodeId> alternatives;
        uint32_t capture = 0;
        bool can_quantify = false;
        bool last_quantifier = false;
    };

    enum class EscapeKind : uint8_t {
        Literal,
        CharacterClass,
        Boundary,
    };

    struct EscapeResult {
        EscapeKind kind = EscapeKind::Literal;
        unsigned char literal = 0;
        ClassSpec character_class;
        bool boundary_positive = true;
    };

    struct ClassAtom {
        bool single = false;
        unsigned char literal = 0;
        std::vector<ByteRange> ranges;
    };

    [[noreturn]] void ThrowInvalid(const char* message) const {
        throw RegexError(RegexErrorKind::InvalidPattern, message, position_);
    }

    [[noreturn]] void ThrowUnsupported(const char* message) const {
        throw RegexError(RegexErrorKind::UnsupportedPattern, message, position_);
    }

    NodeId AddNode(Node node) {
        budget_.Consume();
        if (ast_.nodes.size() >= kMaxBoundedRegexInstructions / 2) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression parse tree is too large", position_);
        }
        try {
            ast_.nodes.push_back(std::move(node));
        } catch (const std::bad_alloc&) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression parse tree allocation failed", position_);
        }
        return static_cast<NodeId>(ast_.nodes.size() - 1);
    }

    NodeId MakeEmpty() {
        Node node;
        node.kind = NodeKind::Empty;
        return AddNode(std::move(node));
    }

    NodeId MakeAlternation(std::vector<NodeId> alternatives) {
        if (alternatives.empty()) {
            return MakeEmpty();
        }
        if (alternatives.size() == 1) {
            return alternatives[0];
        }
        Node node;
        node.kind = NodeKind::Alternation;
        node.children = std::move(alternatives);
        return AddNode(std::move(node));
    }

    NodeId MakeSequence(std::vector<NodeId> terms) {
        if (terms.empty()) {
            return MakeEmpty();
        }
        if (terms.size() == 1) {
            return terms[0];
        }
        Node node;
        node.kind = NodeKind::Sequence;
        node.children = std::move(terms);
        return AddNode(std::move(node));
    }

    void AppendAtom(Context& context, NodeId atom) {
        if (context.terms.size() >= kMaxBoundedRegexInstructions / 2) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression sequence is too large", position_);
        }
        context.terms.push_back(atom);
        const NodeKind kind = ast_.nodes[atom].kind;
        context.can_quantify = kind != NodeKind::Begin && kind != NodeKind::End &&
                               kind != NodeKind::Boundary;
        context.last_quantifier = false;
    }

    void FinishSequence(Context& context) {
        context.alternatives.push_back(MakeSequence(std::move(context.terms)));
        context.terms.clear();
        context.can_quantify = false;
        context.last_quantifier = false;
    }

    void ParseGroupOpen(std::vector<Context>& contexts) {
        ++position_;
        bool capturing = true;
        if (position_ < pattern_.size() && pattern_[position_] == '?') {
            ++position_;
            if (position_ >= pattern_.size()) {
                ThrowInvalid("incomplete group prefix");
            }
            if (pattern_[position_] == ':') {
                capturing = false;
                ++position_;
            } else if (pattern_[position_] == '=' || pattern_[position_] == '!') {
                ThrowUnsupported("lookahead groups are unsupported");
            } else if (pattern_[position_] == '<') {
                ThrowUnsupported("lookbehind and named groups are unsupported");
            } else {
                ThrowUnsupported("this regular-expression group prefix is unsupported");
            }
        }

        if (contexts.size() >= kMaxBoundedRegexNesting + 1) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression nesting limit exceeded", position_);
        }
        Context nested;
        if (capturing) {
            if (ast_.capture_count >= kMaxBoundedRegexCaptures) {
                throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                 "regular-expression capture limit exceeded", position_);
            }
            nested.capture = ++ast_.capture_count;
        }
        contexts.push_back(std::move(nested));
    }

    void ParseGroupClose(std::vector<Context>& contexts) {
        if (contexts.size() == 1) {
            ThrowInvalid("unmatched closing group");
        }
        ++position_;
        Context nested = std::move(contexts.back());
        contexts.pop_back();
        FinishSequence(nested);
        NodeId group = MakeAlternation(std::move(nested.alternatives));
        if (nested.capture != 0) {
            Node node;
            node.kind = NodeKind::Capture;
            node.capture = nested.capture;
            node.children.push_back(group);
            group = AddNode(std::move(node));
        }
        AppendAtom(contexts.back(), group);
        // A grouped assertion is quantifiable; a bare assertion is not.
        contexts.back().can_quantify = true;
    }

    void ParseQuantifier(Context& context) {
        const char quantifier = pattern_[position_];
        if (!context.can_quantify) {
            if (quantifier == '?' && context.last_quantifier && !context.terms.empty()) {
                Node& previous = ast_.nodes[context.terms.back()];
                if (previous.kind != NodeKind::Repeat) {
                    ThrowInvalid("lazy modifier does not follow a quantifier");
                }
                previous.lazy = true;
                context.last_quantifier = false;
                ++position_;
                return;
            }
            ThrowInvalid("quantifier has no preceding atom");
        }

        size_t minimum = 0;
        size_t maximum = 0;
        if (quantifier == '*') {
            minimum = 0;
            maximum = kUnbounded;
            ++position_;
        } else if (quantifier == '+') {
            minimum = 1;
            maximum = kUnbounded;
            ++position_;
        } else if (quantifier == '?') {
            minimum = 0;
            maximum = 1;
            ++position_;
        } else {
            ParseBraceQuantifier(minimum, maximum);
        }

        Node repeat;
        repeat.kind = NodeKind::Repeat;
        repeat.children.push_back(context.terms.back());
        repeat.minimum = minimum;
        repeat.maximum = maximum;
        context.terms.back() = AddNode(std::move(repeat));
        context.can_quantify = false;
        context.last_quantifier = true;
    }

    size_t ParseNumber() {
        if (position_ >= pattern_.size() ||
            !std::isdigit(static_cast<unsigned char>(pattern_[position_]))) {
            ThrowInvalid("quantifier number is missing");
        }
        size_t value = 0;
        while (position_ < pattern_.size() &&
               std::isdigit(static_cast<unsigned char>(pattern_[position_]))) {
            budget_.Consume();
            const unsigned digit = static_cast<unsigned>(pattern_[position_] - '0');
            if (value > (kMaxBoundedRegexQuantifier - digit) / 10) {
                throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                 "regular-expression quantifier is too large", position_);
            }
            value = value * 10 + digit;
            ++position_;
        }
        return value;
    }

    void ParseBraceQuantifier(size_t& minimum, size_t& maximum) {
        ++position_; // {
        minimum = ParseNumber();
        if (position_ >= pattern_.size()) {
            ThrowInvalid("unterminated quantifier");
        }
        if (pattern_[position_] == '}') {
            ++position_;
            maximum = minimum;
            return;
        }
        if (pattern_[position_] != ',') {
            ThrowInvalid("invalid quantifier");
        }
        ++position_;
        if (position_ < pattern_.size() && pattern_[position_] == '}') {
            ++position_;
            maximum = kUnbounded;
            return;
        }
        maximum = ParseNumber();
        if (position_ >= pattern_.size() || pattern_[position_] != '}') {
            ThrowInvalid("unterminated quantifier");
        }
        ++position_;
        if (maximum < minimum) {
            ThrowInvalid("quantifier maximum is smaller than its minimum");
        }
    }

    void AddClassRange(ClassSpec& spec, unsigned begin, unsigned end) {
        budget_.Consume();
        if (begin > end) {
            ThrowInvalid("character-class range is reversed");
        }
        if (spec.ranges.size() >= kMaxBoundedRegexClassRanges) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression character class is too large", position_);
        }
        spec.ranges.push_back(ByteRange{begin, end});
    }

    static ClassSpec Category(char category) {
        ClassSpec spec;
        switch (category) {
        case 'd':
            spec.ranges.push_back(ByteRange{'0', '9'});
            break;
        case 'w':
            spec.ranges.push_back(ByteRange{'0', '9'});
            spec.ranges.push_back(ByteRange{'A', 'Z'});
            spec.ranges.push_back(ByteRange{'_', '_'});
            spec.ranges.push_back(ByteRange{'a', 'z'});
            break;
        case 's':
            spec.ranges.push_back(ByteRange{'\t', '\r'});
            spec.ranges.push_back(ByteRange{' ', ' '});
            break;
        default:
            break;
        }
        return spec;
    }

    static ClassSpec Complement(ClassSpec positive) {
        ClassSpec complement;
        unsigned next = 0;
        for (const ByteRange& range : positive.ranges) {
            if (next < range.begin) {
                complement.ranges.push_back(ByteRange{next, range.begin - 1});
            }
            if (range.end == 255) {
                next = 256;
                break;
            }
            next = range.end + 1;
        }
        if (next <= 255) {
            complement.ranges.push_back(ByteRange{next, 255});
        }
        return complement;
    }

    unsigned char ParseHexEscape(size_t digits) {
        unsigned value = 0;
        for (size_t i = 0; i < digits; ++i) {
            if (position_ >= pattern_.size()) {
                ThrowInvalid("incomplete hexadecimal escape");
            }
            budget_.Consume();
            const char c = pattern_[position_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
            else ThrowInvalid("invalid hexadecimal escape");
            value = (value << 4) | digit;
        }
        if (value > 255) {
            ThrowUnsupported("Unicode escapes above 255 require a Unicode regex engine");
        }
        return static_cast<unsigned char>(value);
    }

    EscapeResult ParseEscape(bool in_class) {
        ++position_; // backslash
        if (position_ >= pattern_.size()) {
            ThrowInvalid("trailing escape");
        }
        budget_.Consume();
        const char escaped = pattern_[position_++];

        if (escaped == 'd' || escaped == 'w' || escaped == 's' ||
            escaped == 'D' || escaped == 'W' || escaped == 'S') {
            ClassSpec positive = Category(static_cast<char>(std::tolower(
                static_cast<unsigned char>(escaped))));
            EscapeResult result;
            result.kind = EscapeKind::CharacterClass;
            result.character_class = (std::isupper(static_cast<unsigned char>(escaped)))
                ? Complement(std::move(positive)) : std::move(positive);
            return result;
        }

        if (escaped == 'b') {
            EscapeResult result;
            if (in_class) {
                result.literal = '\b';
            } else {
                result.kind = EscapeKind::Boundary;
                result.boundary_positive = true;
            }
            return result;
        }
        if (escaped == 'B') {
            if (in_class) {
                ThrowUnsupported("\\B inside a character class is unsupported");
            }
            EscapeResult result;
            result.kind = EscapeKind::Boundary;
            result.boundary_positive = false;
            return result;
        }

        switch (escaped) {
        case 'n': return EscapeResult{EscapeKind::Literal, '\n'};
        case 'r': return EscapeResult{EscapeKind::Literal, '\r'};
        case 't': return EscapeResult{EscapeKind::Literal, '\t'};
        case 'f': return EscapeResult{EscapeKind::Literal, '\f'};
        case 'v': return EscapeResult{EscapeKind::Literal, '\v'};
        case '0':
            if (position_ < pattern_.size() &&
                std::isdigit(static_cast<unsigned char>(pattern_[position_]))) {
                ThrowUnsupported("octal escapes and backreferences are unsupported");
            }
            return EscapeResult{EscapeKind::Literal, '\0'};
        case 'x': {
            if (in_class && position_ + 2 > pattern_.size()) {
                ThrowInvalid("incomplete hexadecimal escape");
            }
            return EscapeResult{EscapeKind::Literal, ParseHexEscape(2)};
        }
        case 'u': {
            if (position_ < pattern_.size() && pattern_[position_] == '{') {
                ThrowUnsupported("Unicode code-point escapes are unsupported");
            }
            const unsigned char value = ParseHexEscape(4);
            return EscapeResult{EscapeKind::Literal, value};
        }
        case 'c': {
            if (position_ >= pattern_.size() ||
                !((pattern_[position_] >= 'a' && pattern_[position_] <= 'z') ||
                  (pattern_[position_] >= 'A' && pattern_[position_] <= 'Z'))) {
                ThrowInvalid("invalid control escape");
            }
            budget_.Consume();
            const unsigned char value = static_cast<unsigned char>(
                std::toupper(static_cast<unsigned char>(pattern_[position_++])) & 0x1f);
            return EscapeResult{EscapeKind::Literal, value};
        }
        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            ThrowUnsupported("backreferences are unsupported");
        case 'p': case 'P':
            ThrowUnsupported("Unicode property escapes are unsupported");
        case 'k':
            ThrowUnsupported("named backreferences are unsupported");
        default:
            // Escaped syntax punctuation and identity escapes are literals in
            // the supported non-Unicode byte-oriented mode.
            return EscapeResult{EscapeKind::Literal,
                                 static_cast<unsigned char>(escaped)};
        }
    }

    ClassAtom ParseClassAtom() {
        if (position_ >= pattern_.size()) {
            ThrowInvalid("unterminated character class");
        }
        if (pattern_[position_] == '\\') {
            EscapeResult escaped = ParseEscape(true);
            if (escaped.kind == EscapeKind::Boundary) {
                ThrowInvalid("boundary escape is not a character-class atom");
            }
            if (escaped.kind == EscapeKind::Literal) {
                return ClassAtom{true, escaped.literal, {}};
            }
            return ClassAtom{false, 0, std::move(escaped.character_class.ranges)};
        }
        budget_.Consume();
        const unsigned char literal = static_cast<unsigned char>(pattern_[position_++]);
        return ClassAtom{true, literal, {}};
    }

    NodeId ParseCharacterClass() {
        ++position_; // [
        ClassSpec spec;
        bool negated = false;
        if (position_ < pattern_.size() && pattern_[position_] == '^') {
            negated = true;
            ++position_;
        }

        bool closed = false;
        while (position_ < pattern_.size()) {
            if (pattern_[position_] == ']') {
                ++position_;
                closed = true;
                break;
            }

            ClassAtom left = ParseClassAtom();
            if (position_ < pattern_.size() && pattern_[position_] == '-' &&
                position_ + 1 < pattern_.size() && pattern_[position_ + 1] != ']') {
                ++position_;
                ClassAtom right = ParseClassAtom();
                if (!left.single || !right.single) {
                    ThrowInvalid("character-class range endpoint is not a character");
                }
                AddClassRange(spec, left.literal, right.literal);
            } else if (left.single) {
                AddClassRange(spec, left.literal, left.literal);
            } else {
                for (const ByteRange& range : left.ranges) {
                    AddClassRange(spec, range.begin, range.end);
                }
            }
        }
        if (!closed) {
            ThrowInvalid("unterminated character class");
        }
        spec.negated = negated;

        Node node;
        node.kind = NodeKind::CharacterClass;
        node.character_class = std::move(spec);
        return AddNode(std::move(node));
    }

    NodeId ParseAtom() {
        const char atom = pattern_[position_];
        if (atom == '[') {
            return ParseCharacterClass();
        }
        if (atom == '.') {
            ++position_;
            Node node;
            node.kind = NodeKind::Any;
            return AddNode(std::move(node));
        }
        if (atom == '^' || atom == '$') {
            ++position_;
            Node node;
            node.kind = atom == '^' ? NodeKind::Begin : NodeKind::End;
            return AddNode(std::move(node));
        }
        if (atom == '\\') {
            EscapeResult escaped = ParseEscape(false);
            Node node;
            if (escaped.kind == EscapeKind::Boundary) {
                node.kind = NodeKind::Boundary;
                node.boundary_positive = escaped.boundary_positive;
            } else if (escaped.kind == EscapeKind::CharacterClass) {
                node.kind = NodeKind::CharacterClass;
                node.character_class = std::move(escaped.character_class);
            } else {
                node.kind = NodeKind::Literal;
                node.literal = escaped.literal;
            }
            return AddNode(std::move(node));
        }

        ++position_;
        Node node;
        node.kind = NodeKind::Literal;
        node.literal = static_cast<unsigned char>(atom);
        return AddNode(std::move(node));
    }

    std::string_view pattern_;
    RegexBudget& budget_;
    size_t position_ = 0;
    NodeId root_ = kInvalidNode;
    Ast ast_;
};

enum class Op : uint8_t {
    Char,
    Any,
    CharacterClass,
    Begin,
    End,
    Boundary,
    SaveStart,
    SaveEnd,
    Call,
    Split,
    Jump,
    Repeat,
    Ret,
    Match,
};

struct Instruction {
    Op op = Op::Ret;
    size_t x = 0;
    size_t y = 0;
    uint32_t arg = 0;
    size_t minimum = 0;
    size_t maximum = 0;
    bool greedy = true;
    bool boundary_positive = true;
    unsigned char literal = 0;
    uint32_t capture_first = 0;
    uint32_t capture_end = 0;
};

struct Program {
    std::vector<Instruction> code;
    std::vector<ClassSpec> classes;
    RegexFlags flags;
    size_t entry = 0;
    uint32_t capture_count = 0;
    size_t repeat_count = 0;
    size_t call_depth = 0;
};

class Compiler {
public:
    Compiler(Ast ast, RegexFlags flags, NodeId root, RegexBudget& budget)
        : ast_(std::move(ast)), flags_(flags), root_(root), budget_(budget),
          targets_(ast_.nodes.size(), std::numeric_limits<size_t>::max()),
          capture_first_(ast_.nodes.size(), 0), capture_end_(ast_.nodes.size(), 0),
          depths_(ast_.nodes.size(), 0) {}

    std::shared_ptr<const Program> Compile() {
        struct Task {
            NodeId node;
            bool postorder;
        };

        std::vector<Task> tasks;
        tasks.reserve(ast_.nodes.size());
        tasks.push_back(Task{root_, false});
        while (!tasks.empty()) {
            budget_.Consume();
            Task task = tasks.back();
            tasks.pop_back();
            if (targets_[task.node] != std::numeric_limits<size_t>::max()) {
                continue;
            }
            const Node& node = ast_.nodes[task.node];
            if (!task.postorder) {
                tasks.push_back(Task{task.node, true});
                for (auto it = node.children.rbegin(); it != node.children.rend(); ++it) {
                    budget_.Consume();
                    if (targets_[*it] == std::numeric_limits<size_t>::max()) {
                        tasks.push_back(Task{*it, false});
                    }
                }
                if (tasks.size() > kMaxBoundedRegexInstructions) {
                    throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                     "regular-expression compiler stack limit exceeded");
                }
                continue;
            }
            for (NodeId child : node.children) {
                budget_.Consume();
                if (capture_first_[child] != 0 &&
                    (capture_first_[task.node] == 0 ||
                     capture_first_[child] < capture_first_[task.node])) {
                    capture_first_[task.node] = capture_first_[child];
                }
                capture_end_[task.node] = std::max(capture_end_[task.node], capture_end_[child]);
                depths_[task.node] = std::max(depths_[task.node], depths_[child] + 1);
            }
            if (node.kind == NodeKind::Capture) {
                capture_first_[task.node] = node.capture;
                capture_end_[task.node] = std::max(capture_end_[task.node], node.capture + 1);
            }
            targets_[task.node] = CompileNode(task.node);
        }

        program_.flags = flags_;
        program_.capture_count = ast_.capture_count;
        program_.call_depth = depths_[root_] + 1;
        if (program_.call_depth > kMaxBoundedRegexCallDepth) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression execution call-depth limit exceeded");
        }
        Instruction call;
        call.op = Op::Call;
        call.x = targets_[root_];
        program_.entry = Emit(call);
        Instruction match;
        match.op = Op::Match;
        Emit(match);
        return std::make_shared<const Program>(std::move(program_));
    }

private:
    size_t Emit(Instruction instruction) {
        budget_.Consume();
        if (program_.code.size() >= kMaxBoundedRegexInstructions) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression instruction limit exceeded");
        }
        try {
            program_.code.push_back(instruction);
        } catch (const std::bad_alloc&) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression instruction allocation failed");
        }
        return program_.code.size() - 1;
    }

    uint32_t AddClass(const ClassSpec& spec) {
        if (program_.classes.size() >= std::numeric_limits<uint32_t>::max()) {
            throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                             "regular-expression class table is too large");
        }
        budget_.Consume(spec.ranges.size() + 1);
        program_.classes.push_back(spec);
        return static_cast<uint32_t>(program_.classes.size() - 1);
    }

    size_t CompileNode(NodeId id) {
        budget_.Consume();
        const Node& node = ast_.nodes[id];
        switch (node.kind) {
        case NodeKind::Empty: {
            Instruction ret;
            ret.op = Op::Ret;
            return Emit(ret);
        }
        case NodeKind::Literal: {
            Instruction literal;
            literal.op = Op::Char;
            literal.literal = node.literal;
            const size_t start = Emit(literal);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Any: {
            Instruction any;
            any.op = Op::Any;
            const size_t start = Emit(any);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::CharacterClass: {
            Instruction character_class;
            character_class.op = Op::CharacterClass;
            character_class.arg = AddClass(node.character_class);
            const size_t start = Emit(character_class);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Begin:
        case NodeKind::End: {
            Instruction anchor;
            anchor.op = node.kind == NodeKind::Begin ? Op::Begin : Op::End;
            const size_t start = Emit(anchor);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Boundary: {
            Instruction boundary;
            boundary.op = Op::Boundary;
            boundary.boundary_positive = node.boundary_positive;
            const size_t start = Emit(boundary);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Sequence: {
            const size_t start = program_.code.size();
            for (NodeId child : node.children) {
                Instruction call;
                call.op = Op::Call;
                call.x = targets_[child];
                Emit(call);
            }
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Alternation: {
            const size_t start = program_.code.size();
            std::vector<size_t> jumps;
            jumps.reserve(node.children.size());
            size_t previous_split = std::numeric_limits<size_t>::max();

            for (size_t i = 0; i + 1 < node.children.size(); ++i) {
                Instruction split;
                split.op = Op::Split;
                const size_t split_position = Emit(split);
                if (previous_split != std::numeric_limits<size_t>::max()) {
                    program_.code[previous_split].y = split_position;
                }
                previous_split = split_position;

                Instruction call;
                call.op = Op::Call;
                call.x = targets_[node.children[i]];
                Emit(call);
                Instruction jump;
                jump.op = Op::Jump;
                jumps.push_back(Emit(jump));
                program_.code[split_position].x =
                    split_position + 1;
            }

            const size_t last_branch = program_.code.size();
            Instruction last_call;
            last_call.op = Op::Call;
            last_call.x = targets_[node.children.back()];
            Emit(last_call);
            Instruction last_jump;
            last_jump.op = Op::Jump;
            jumps.push_back(Emit(last_jump));

            const size_t end = program_.code.size();
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);

            // The final split target is the last branch. Earlier split
            // targets were chained to the next split while emitting.
            if (previous_split != std::numeric_limits<size_t>::max()) {
                program_.code[previous_split].y = last_branch;
            }
            for (size_t jump : jumps) {
                budget_.Consume();
                program_.code[jump].x = end;
            }
            return start;
        }
        case NodeKind::Capture: {
            Instruction save_start;
            save_start.op = Op::SaveStart;
            save_start.arg = node.capture;
            const size_t start = Emit(save_start);
            Instruction call;
            call.op = Op::Call;
            call.x = targets_[node.children[0]];
            Emit(call);
            Instruction save_end;
            save_end.op = Op::SaveEnd;
            save_end.arg = node.capture;
            Emit(save_end);
            Instruction ret;
            ret.op = Op::Ret;
            Emit(ret);
            return start;
        }
        case NodeKind::Repeat: {
            Instruction repeat;
            repeat.op = Op::Repeat;
            repeat.x = targets_[node.children[0]];
            repeat.minimum = node.minimum;
            repeat.maximum = node.maximum;
            repeat.greedy = !node.lazy;
            repeat.arg = static_cast<uint32_t>(program_.repeat_count++);
            repeat.capture_first = capture_first_[id];
            repeat.capture_end = capture_end_[id];
            const size_t start = Emit(repeat);
            Instruction ret;
            ret.op = Op::Ret;
            const size_t after = Emit(ret);
            program_.code[start].y = after;
            return start;
        }
        }
        throw RegexError(RegexErrorKind::InvalidPattern,
                         "internal regular-expression compiler error");
    }

    Ast ast_;
    RegexFlags flags_;
    NodeId root_;
    RegexBudget& budget_;
    Program program_;
    std::vector<size_t> targets_;
    std::vector<uint32_t> capture_first_;
    std::vector<uint32_t> capture_end_;
    std::vector<size_t> depths_;
};

struct CaptureState {
    size_t start = 0;
    size_t end = 0;
    bool matched = false;
};

struct RepeatState {
    size_t count = 0;
    size_t iteration_start = 0;
    bool in_progress = false;
    bool zero_progress = false;
};

struct CallFrame {
    size_t return_pc = 0;
};

struct StateMemory {
    size_t live_bytes = 0;
};

struct ExecutionData {
    std::vector<CaptureState> captures;
    std::vector<RepeatState> repeats;
    // Fixed-size slots avoid an input-dependent allocation during Call/Ret.
    std::vector<CallFrame> calls;
    size_t call_depth = 0;
    StateMemory* memory = nullptr;
    size_t bytes = 0;

    size_t WorkSize() const noexcept {
        return captures.size() + repeats.size() + calls.size() + 1;
    }
};

struct MachineState {
    size_t pc = 0;
    size_t position = 0;
    size_t search_start = 0;
    std::shared_ptr<ExecutionData> data;
};

void ReserveStateMemory(StateMemory& memory, size_t bytes) {
    if (bytes > kMaxBoundedRegexStateBytes - memory.live_bytes) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression live execution state memory limit exceeded");
    }
    memory.live_bytes += bytes;
}

void DeleteData(ExecutionData* data) noexcept {
    data->memory->live_bytes -= data->bytes;
    delete data;
}

std::shared_ptr<ExecutionData> MakeData(const Program& program,
                                      StateMemory& memory, RegexBudget& budget) {
    budget.Consume(static_cast<size_t>(program.capture_count) +
                   program.repeat_count + program.call_depth + 2);
    // Include vector elements, object and shared-pointer bookkeeping. Vector
    // sizes stay fixed for the lifetime of a snapshot.
    const size_t bytes = sizeof(ExecutionData) + 64 +
        (static_cast<size_t>(program.capture_count) + 1) * sizeof(CaptureState) +
        program.repeat_count * sizeof(RepeatState) + program.call_depth * sizeof(CallFrame);
    ReserveStateMemory(memory, bytes);
    std::unique_ptr<ExecutionData> data;
    try {
        data = std::make_unique<ExecutionData>();
        data->captures.resize(static_cast<size_t>(program.capture_count) + 1);
        data->repeats.resize(program.repeat_count);
        data->calls.resize(program.call_depth);
        data->memory = &memory;
        data->bytes = bytes;
    } catch (...) {
        memory.live_bytes -= bytes;
        throw;
    }
    // shared_ptr invokes the deleter if allocation of its control block fails.
    return std::shared_ptr<ExecutionData>(data.release(), &DeleteData);
}

std::shared_ptr<ExecutionData> CopyData(
    const std::shared_ptr<ExecutionData>& data, RegexBudget& budget) {
    budget.Consume(data->WorkSize());
    ReserveStateMemory(*data->memory, data->bytes);
    ExecutionData* copy = nullptr;
    try {
        copy = new ExecutionData(*data);
    } catch (...) {
        data->memory->live_bytes -= data->bytes;
        throw;
    }
    return std::shared_ptr<ExecutionData>(copy, &DeleteData);
}

void EnsureUnique(MachineState& state, RegexBudget& budget) {
    if (state.data.use_count() != 1) {
        state.data = CopyData(state.data, budget);
    }
}

void PushBacktrack(std::vector<MachineState>& backtrack, MachineState state) {
    if (backtrack.size() >= kMaxBoundedRegexBacktrackStates) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression backtracking state limit exceeded");
    }
    try {
        backtrack.push_back(std::move(state));
    } catch (const std::bad_alloc&) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression backtracking stack allocation failed");
    }
}

unsigned char FoldAscii(unsigned char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<unsigned char>(value + ('a' - 'A'));
    }
    return value;
}

bool LiteralMatches(unsigned char input, unsigned char literal,
                    bool ignore_case) noexcept {
    return input == literal ||
           (ignore_case && FoldAscii(input) == FoldAscii(literal));
}

bool IsLineTerminator(unsigned char value) noexcept {
    return value == '\n' || value == '\r';
}

bool IsWord(unsigned char value) noexcept {
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= 'a' && value <= 'z') || value == '_';
}

bool MatchesBegin(const Program& program, std::string_view input,
                  size_t position) noexcept {
    if (position == 0) {
        return true;
    }
    return program.flags.multiline && IsLineTerminator(
        static_cast<unsigned char>(input[position - 1]));
}

bool MatchesEnd(const Program& program, std::string_view input,
                size_t position) noexcept {
    if (position == input.size()) {
        return true;
    }
    return program.flags.multiline &&
        IsLineTerminator(static_cast<unsigned char>(input[position]));
}

std::optional<RegexMatch> RunCandidate(const Program& program,
                                       std::string_view input,
                                       size_t search_start,
                                       RegexBudget& budget) {
    // Declared before every snapshot so the accounting object outlives them.
    StateMemory memory;
    MachineState state;
    state.pc = program.entry;
    state.position = search_start;
    state.search_start = search_start;
    state.data = MakeData(program, memory, budget);

    std::vector<MachineState> backtrack;
    try {
        backtrack.reserve(64);
    } catch (const std::bad_alloc&) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression backtracking stack allocation failed");
    }

    for (;;) {
        if (state.pc >= program.code.size()) {
            // This is an internal malformed-program guard, rather than an
            // input-dependent failure that could be retried on another start.
            throw RegexError(RegexErrorKind::InvalidPattern,
                             "invalid bounded regular-expression program");
        }

        budget.Consume();
        const Instruction& instruction = program.code[state.pc];
        bool failed = false;

        switch (instruction.op) {
        case Op::Char:
            if (state.position >= input.size() ||
                !LiteralMatches(static_cast<unsigned char>(input[state.position]),
                                instruction.literal, program.flags.ignore_case)) {
                failed = true;
            } else {
                ++state.position;
                ++state.pc;
            }
            break;

        case Op::Any:
            if (state.position >= input.size() ||
                (!program.flags.dot_all &&
                 IsLineTerminator(static_cast<unsigned char>(input[state.position])))) {
                failed = true;
            } else {
                ++state.position;
                ++state.pc;
            }
            break;

        case Op::CharacterClass:
            if (state.position >= input.size() ||
                instruction.arg >= program.classes.size() ||
                !program.classes[instruction.arg].Matches(
                    static_cast<unsigned char>(input[state.position]),
                    program.flags.ignore_case, budget)) {
                failed = true;
            } else {
                ++state.position;
                ++state.pc;
            }
            break;

        case Op::Begin:
            if (!MatchesBegin(program, input, state.position)) {
                failed = true;
            } else {
                ++state.pc;
            }
            break;

        case Op::End:
            if (!MatchesEnd(program, input, state.position)) {
                failed = true;
            } else {
                ++state.pc;
            }
            break;

        case Op::Boundary: {
            const bool before = state.position > 0 &&
                IsWord(static_cast<unsigned char>(input[state.position - 1]));
            const bool after = state.position < input.size() &&
                IsWord(static_cast<unsigned char>(input[state.position]));
            const bool boundary = before != after;
            if (boundary != instruction.boundary_positive) {
                failed = true;
            } else {
                ++state.pc;
            }
            break;
        }

        case Op::SaveStart:
            if (instruction.arg >= state.data->captures.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid capture slot in bounded regular-expression program");
            }
            EnsureUnique(state, budget);
            state.data->captures[instruction.arg].start = state.position;
            state.data->captures[instruction.arg].end = state.position;
            state.data->captures[instruction.arg].matched = false;
            ++state.pc;
            break;

        case Op::SaveEnd:
            if (instruction.arg >= state.data->captures.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid capture slot in bounded regular-expression program");
            }
            EnsureUnique(state, budget);
            state.data->captures[instruction.arg].end = state.position;
            state.data->captures[instruction.arg].matched = true;
            ++state.pc;
            break;

        case Op::Call:
            if (instruction.x >= program.code.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid call target in bounded regular-expression program");
            }
            if (state.data->call_depth >= state.data->calls.size()) {
                throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                 "regular-expression execution call-depth limit exceeded");
            }
            EnsureUnique(state, budget);
            state.data->calls[state.data->call_depth++] = CallFrame{state.pc + 1};
            state.pc = instruction.x;
            break;

        case Op::Split: {
            if (instruction.x >= program.code.size() ||
                instruction.y >= program.code.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid split target in bounded regular-expression program");
            }
            MachineState alternative = state;
            alternative.pc = instruction.y;
            PushBacktrack(backtrack, std::move(alternative));
            state.pc = instruction.x;
            break;
        }

        case Op::Jump:
            if (instruction.x >= program.code.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid jump target in bounded regular-expression program");
            }
            state.pc = instruction.x;
            break;

        case Op::Repeat: {
            if (instruction.arg >= state.data->repeats.size() ||
                instruction.x >= program.code.size() ||
                instruction.y >= program.code.size()) {
                throw RegexError(RegexErrorKind::InvalidPattern,
                                 "invalid repeat in bounded regular-expression program");
            }
            EnsureUnique(state, budget);
            RepeatState& repeat = state.data->repeats[instruction.arg];
            if (repeat.in_progress) {
                repeat.zero_progress = state.position == repeat.iteration_start;
                repeat.in_progress = false;
                // ECMAScript RepeatMatcher rejects an empty optional iteration
                // (including its captures), then tries an earlier alternative.
                if (repeat.zero_progress && repeat.count > instruction.minimum) {
                    failed = true;
                    break;
                }
            }

            const bool can_exit = repeat.count >= instruction.minimum;
            const bool can_body = repeat.count < instruction.maximum;

            auto make_exit = [&]() {
                MachineState exit = state;
                exit.data = CopyData(state.data, budget);
                exit.data->repeats[instruction.arg] = RepeatState{};
                exit.pc = instruction.y;
                return exit;
            };
            auto make_body = [&]() {
                if (state.data->call_depth >= state.data->calls.size()) {
                    throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                     "regular-expression execution call-depth limit exceeded");
                }
                MachineState body = state;
                body.data = CopyData(state.data, budget);
                RepeatState& body_repeat = body.data->repeats[instruction.arg];
                ++body_repeat.count;
                body_repeat.iteration_start = state.position;
                body_repeat.in_progress = true;
                body_repeat.zero_progress = false;
                for (uint32_t capture = instruction.capture_first;
                     capture < instruction.capture_end; ++capture) {
                    budget.Consume();
                    body.data->captures[capture] = CaptureState{};
                }
                body.data->calls[body.data->call_depth++] = CallFrame{state.pc};
                body.pc = instruction.x;
                return body;
            };

            if (!can_body) {
                state = make_exit();
            } else if (instruction.greedy) {
                if (can_exit) {
                    PushBacktrack(backtrack, make_exit());
                }
                state = make_body();
            } else {
                if (can_exit) {
                    PushBacktrack(backtrack, make_body());
                    state = make_exit();
                } else {
                    state = make_body();
                }
            }
            break;
        }

        case Op::Ret:
            if (state.data->call_depth == 0) {
                failed = true;
            } else {
                EnsureUnique(state, budget);
                const CallFrame frame = state.data->calls[--state.data->call_depth];
                state.pc = frame.return_pc;
            }
            break;

        case Op::Match: {
            if (state.data->call_depth != 0) {
                failed = true;
                break;
            }
            RegexMatch result;
            budget.Consume(static_cast<size_t>(program.capture_count) + 1);
            try {
                result.captures.resize(static_cast<size_t>(program.capture_count) + 1);
            } catch (const std::bad_alloc&) {
                throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                                 "regular-expression capture result allocation failed");
            }
            result.captures[0] = RegexRange{state.search_start, state.position};
            for (size_t i = 1; i < result.captures.size(); ++i) {
                const CaptureState& capture = state.data->captures[i];
                if (capture.matched) {
                    result.captures[i] = RegexRange{capture.start, capture.end};
                }
            }
            return result;
        }
        }

        if (failed) {
            if (backtrack.empty()) {
                return std::nullopt;
            }
            state = std::move(backtrack.back());
            backtrack.pop_back();
        }
    }
}

std::shared_ptr<const Program> BuildProgram(std::string_view pattern,
                                            std::string_view flags,
                                            RegexBudget& budget) {
    if (pattern.size() > kMaxBoundedRegexPatternLength) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression pattern is too large");
    }
    const RegexFlags parsed_flags = ParseFlags(flags);
    budget.Consume(flags.size());
    Parser parser(pattern, budget);
    Ast ast = parser.Parse();
    budget.Consume(ast.nodes.size() * 4);
    Compiler compiler(std::move(ast), parsed_flags, parser.Root(), budget);
    return compiler.Compile();
}

} // namespace

struct BoundedRegex::Impl {
    std::shared_ptr<const Program> program;
};

BoundedRegex::BoundedRegex(std::shared_ptr<const Impl> impl) noexcept
    : impl_(std::move(impl)) {}

BoundedRegex::BoundedRegex(std::string_view pattern, std::string_view flags,
                         size_t parse_steps_limit) {
    RegexBudget budget(parse_steps_limit);
    impl_ = Compile(pattern, flags, budget).impl_;
}

BoundedRegex BoundedRegex::Compile(std::string_view pattern,
                                 std::string_view flags, RegexBudget& budget) {
    try {
        auto impl = std::make_shared<Impl>();
        impl->program = BuildProgram(pattern, flags, budget);
        return BoundedRegex(std::move(impl));
    } catch (const std::bad_alloc&) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression compilation allocation failed");
    }
}

size_t BoundedRegex::CaptureCount() const noexcept {
    return impl_->program->capture_count;
}

std::optional<RegexMatch> BoundedRegex::Search(std::string_view input,
                                             size_t start_offset,
                                             RegexBudget& budget) const {
    if (input.size() > kMaxBoundedRegexInputLength) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression input length limit exceeded");
    }
    budget.Consume();
    if (start_offset > input.size()) {
        return std::nullopt;
    }
    try {
        const Program& program = *impl_->program;
        for (size_t candidate = start_offset; ; ++candidate) {
            budget.Consume();
            auto match = RunCandidate(program, input, candidate, budget);
            if (match) {
                return match;
            }
            if (program.flags.sticky || candidate == input.size()) {
                return std::nullopt;
            }
        }
    } catch (const std::bad_alloc&) {
        throw RegexError(RegexErrorKind::ResourceLimitExceeded,
                         "regular-expression execution allocation failed");
    }
}

std::optional<RegexMatch> BoundedRegex::Search(std::string_view input,
                                             size_t start_offset,
                                             size_t steps_limit) const {
    RegexBudget budget(steps_limit);
    return Search(input, start_offset, budget);
}

std::optional<RegexMatch> RegexSearch(std::string_view pattern,
                                     std::string_view flags,
                                     std::string_view input,
                                     size_t start_offset,
                                     size_t steps_limit) {
    RegexBudget budget(steps_limit);
    return RegexSearch(pattern, flags, input, start_offset, budget);
}

std::optional<RegexMatch> RegexSearch(std::string_view pattern,
                                     std::string_view flags,
                                     std::string_view input,
                                     size_t start_offset,
                                     RegexBudget& budget) {
    const BoundedRegex regex = BoundedRegex::Compile(pattern, flags, budget);
    return regex.Search(input, start_offset, budget);
}

std::optional<size_t> NextRegexSearchOffset(const RegexMatch& match,
                                           size_t input_length) noexcept {
    if (match.captures.empty() || !match.captures[0]) {
        return std::nullopt;
    }
    const RegexRange& full = match.Full();
    if (full.end > input_length || full.begin > full.end) {
        return std::nullopt;
    }
    if (full.begin != full.end) {
        return full.end;
    }
    if (full.end == input_length) {
        return std::nullopt;
    }
    return full.end + 1;
}

} // namespace aether
