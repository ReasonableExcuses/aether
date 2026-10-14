#include "aether/lexer.h"
#include <cctype>
#include <charconv>

namespace aether {

const char* TokenKindToString(TokenKind kind) {
    switch (kind) {
        case TokenKind::EndOfFile:        return "EndOfFile";
        case TokenKind::Invalid:          return "Invalid";
        case TokenKind::Number:           return "Number";
        case TokenKind::String:           return "String";
        case TokenKind::True:             return "True";
        case TokenKind::False:            return "False";
        case TokenKind::Null:             return "Null";
        case TokenKind::Undefined:        return "Undefined";
        case TokenKind::Identifier:       return "Identifier";
        case TokenKind::Var:              return "Var";
        case TokenKind::Let:              return "Let";
        case TokenKind::Const:            return "Const";
        case TokenKind::Function:         return "Function";
        case TokenKind::Return:           return "Return";
        case TokenKind::If:               return "If";
        case TokenKind::Else:             return "Else";
        case TokenKind::While:            return "While";
        case TokenKind::For:              return "For";
        case TokenKind::Break:            return "Break";
        case TokenKind::Continue:         return "Continue";
        case TokenKind::New:              return "New";
        case TokenKind::Try:              return "Try";
        case TokenKind::Catch:            return "Catch";
        case TokenKind::Finally:          return "Finally";
        case TokenKind::Throw:            return "Throw";
        case TokenKind::Class:            return "Class";
        case TokenKind::Extends:          return "Extends";
        case TokenKind::This:             return "This";
        case TokenKind::TypeOf:           return "typeof";
        case TokenKind::InstanceOf:       return "instanceof";
        case TokenKind::In:               return "in";
        case TokenKind::Delete:           return "delete";
        case TokenKind::Switch:           return "switch";
        case TokenKind::Case:             return "case";
        case TokenKind::Do:               return "do";
        case TokenKind::Void:             return "void";
        case TokenKind::Debugger:         return "debugger";
        case TokenKind::With:             return "with";
        case TokenKind::Super:            return "super";
        case TokenKind::Static:           return "static";
        case TokenKind::OpenParen:         return "(";
        case TokenKind::CloseParen:        return ")";
        case TokenKind::OpenBrace:         return "{";
        case TokenKind::CloseBrace:        return "}";
        case TokenKind::OpenBracket:       return "[";
        case TokenKind::CloseBracket:      return "]";
        case TokenKind::Semicolon:         return ";";
        case TokenKind::Comma:             return ",";
        case TokenKind::Dot:               return ".";
        case TokenKind::Colon:             return ":";
        case TokenKind::Question:          return "?";
        case TokenKind::Plus:              return "+";
        case TokenKind::Minus:             return "-";
        case TokenKind::Star:              return "*";
        case TokenKind::Slash:             return "/";
        case TokenKind::Percent:           return "%";
        case TokenKind::Equal:             return "=";
        case TokenKind::PlusEqual:         return "+=";
        case TokenKind::MinusEqual:        return "-=";
        case TokenKind::StarEqual:         return "*=";
        case TokenKind::SlashEqual:        return "/=";
        case TokenKind::PercentEqual:      return "%=";
        case TokenKind::StarStar:          return "**";
        case TokenKind::StarStarEqual:     return "**=";
        case TokenKind::EqualEqual:        return "==";
        case TokenKind::StrictEqual:       return "===";
        case TokenKind::NotEqual:          return "!=";
        case TokenKind::StrictNotEqual:    return "!==";
        case TokenKind::Less:              return "<";
        case TokenKind::LessEqual:         return "<=";
        case TokenKind::Greater:           return ">";
        case TokenKind::GreaterEqual:      return ">=";
        case TokenKind::Ampersand:         return "&";
        case TokenKind::Bar:               return "|";
        case TokenKind::Caret:             return "^";
        case TokenKind::Tilde:             return "~";
        case TokenKind::Exclamation:       return "!";
        case TokenKind::AmpAmp:            return "&&";
        case TokenKind::BarBar:            return "||";
        case TokenKind::LessLess:          return "<<";
        case TokenKind::GreaterGreater:    return ">>";
        case TokenKind::GreaterGreaterGreater:      return ">>>";
        case TokenKind::LessLessEqual:              return "<<=";
        case TokenKind::GreaterGreaterEqual:        return ">>=";
        case TokenKind::GreaterGreaterGreaterEqual: return ">>>=";
        case TokenKind::AmpersandEqual:             return "&=";
        case TokenKind::BarEqual:                   return "|=";
        case TokenKind::CaretEqual:                 return "^=";
        case TokenKind::PlusPlus:          return "++";
        case TokenKind::MinusMinus:        return "--";
        case TokenKind::QuestionQuestion:  return "??";
        case TokenKind::QuestionDot:       return "?.";
        case TokenKind::QuestionQuestionEqual: return "??=";
        case TokenKind::AmpAmpEqual:       return "&&=";
        case TokenKind::BarBarEqual:       return "||=";
        case TokenKind::DotDotDot:         return "...";
        case TokenKind::Arrow:             return "=>";
        case TokenKind::Backtick:          return "`";
        case TokenKind::Of:                return "of";
        case TokenKind::Yield:             return "yield";
        case TokenKind::Async:             return "async";
        case TokenKind::Await:             return "await";
        case TokenKind::Import:            return "import";
        case TokenKind::Export:            return "export";
        case TokenKind::Default:           return "default";
        case TokenKind::Hash:              return "#";
        case TokenKind::RegExp:            return "RegExp";
        default:                           return "<Unknown>";
    }
}

static bool CanFollowRegExp(TokenKind prev) {
    switch (prev) {
        case TokenKind::Invalid:
        case TokenKind::EndOfFile:
        case TokenKind::Semicolon:
        case TokenKind::Comma:
        case TokenKind::Colon:
        case TokenKind::OpenParen:
        case TokenKind::OpenBracket:
        case TokenKind::OpenBrace:
        case TokenKind::Equal:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::StarStarEqual:
        case TokenKind::AmpersandEqual:
        case TokenKind::BarEqual:
        case TokenKind::CaretEqual:
        case TokenKind::AmpAmpEqual:
        case TokenKind::BarBarEqual:
        case TokenKind::QuestionQuestionEqual:
        case TokenKind::Question:
        case TokenKind::QuestionQuestion:
        case TokenKind::AmpAmp:
        case TokenKind::BarBar:
        case TokenKind::Exclamation:
        case TokenKind::Tilde:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::StarStar:
        case TokenKind::EqualEqual:
        case TokenKind::StrictEqual:
        case TokenKind::NotEqual:
        case TokenKind::StrictNotEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::Return:
        case TokenKind::Throw:
        case TokenKind::Yield:
        case TokenKind::Await:
        case TokenKind::Case:
        case TokenKind::Delete:
        case TokenKind::Void:
        case TokenKind::TypeOf:
        case TokenKind::InstanceOf:
        case TokenKind::In:
        case TokenKind::Do:
        case TokenKind::Else:
        case TokenKind::Arrow:
            return true;
        default:
            return false;
    }
}

Lexer::Lexer(std::string_view source)
    : source_(source), cursor_(0), line_(1), column_(1) {}

char Lexer::Peek() const noexcept {
    if (IsAtEnd()) return '\0';
    return source_[cursor_];
}

char Lexer::PeekNext() const noexcept {
    if (cursor_ + 1 >= source_.size()) return '\0';
    return source_[cursor_ + 1];
}

char Lexer::Advance() noexcept {
    if (IsAtEnd()) return '\0';
    char c = source_[cursor_++];
    column_++;
    return c;
}

bool Lexer::Match(char expected) noexcept {
    if (IsAtEnd() || source_[cursor_] != expected) return false;
    cursor_++;
    column_++;
    return true;
}

bool Lexer::IsAtEnd() const noexcept {
    return cursor_ >= source_.size();
}

static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void Lexer::SkipWhitespaceAndComments() {
    while (!IsAtEnd()) {
        char c = Peek();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
            case '\v':
            case '\f':
                Advance();
                break;
            case '\n':
                cursor_++;
                line_++;
                column_ = 1;
                break;
            case '/':
                if (PeekNext() == '/') {
                    // Single line comment
                    Advance();
                    Advance();
                    while (!IsAtEnd() && Peek() != '\n') {
                        Advance();
                    }
                } else if (PeekNext() == '*') {
                    // Multi line comment
                    Advance();
                    Advance();
                    while (!IsAtEnd()) {
                        if (Peek() == '\n') {
                            cursor_++;
                            line_++;
                            column_ = 1;
                        } else if (Peek() == '*' && PeekNext() == '/') {
                            Advance();
                            Advance();
                            break;
                        } else {
                            Advance();
                        }
                    }
                } else {
                    return;
                }
                break;
            case '#':
                // Hashbang comment at start of file
                if (cursor_ == 0 && PeekNext() == '!') {
                    Advance();
                    Advance();
                    while (!IsAtEnd() && Peek() != '\n') {
                        Advance();
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

Token Lexer::PeekToken() {
    if (!peeked_token_) {
        peeked_token_ = NextTokenImpl();
    }
    return *peeked_token_;
}

Token Lexer::NextToken() {
    Token tok = NextTokenImpl();
    last_token_kind_ = tok.kind;
    return tok;
}

Token Lexer::NextTokenImpl() {
    if (peeked_token_) {
        Token tok = *peeked_token_;
        peeked_token_.reset();
        return tok;
    }

    SkipWhitespaceAndComments();

    SourceLocation loc{line_, column_};

    if (IsAtEnd()) {
        return Token{TokenKind::EndOfFile, "", 0.0, false, 0, false, loc};
    }

    char c = Peek();

    // Check for leading dot number (.5, .123)
    if (c == '.' && std::isdigit(static_cast<unsigned char>(PeekNext()))) {
        return ScanNumber();
    }

    // Identifiers and keywords
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
        return ScanIdentifierOrKeyword();
    }

    // Numbers
    if (std::isdigit(static_cast<unsigned char>(c))) {
        return ScanNumber();
    }

    // Strings
    if (c == '"' || c == '\'') {
        return ScanString(Advance());
    }

    // Template Literals
    if (c == '`') {
        Advance(); // Consume opening backtick
        return ScanTemplateLiteral();
    }

    // Single / Multi character operators & punctuators
    Advance();
    switch (c) {
        case '#': return Token{TokenKind::Hash, "#", 0, false, 0, false, loc};
        case '(': return Token{TokenKind::OpenParen, "(", 0, false, 0, false, loc};
        case ')': return Token{TokenKind::CloseParen, ")", 0, false, 0, false, loc};
        case '{': return Token{TokenKind::OpenBrace, "{", 0, false, 0, false, loc};
        case '}': return Token{TokenKind::CloseBrace, "}", 0, false, 0, false, loc};
        case '[': return Token{TokenKind::OpenBracket, "[", 0, false, 0, false, loc};
        case ']': return Token{TokenKind::CloseBracket, "]", 0, false, 0, false, loc};
        case ';': return Token{TokenKind::Semicolon, ";", 0, false, 0, false, loc};
        case ',': return Token{TokenKind::Comma, ",", 0, false, 0, false, loc};
        case '.':
            if (Peek() == '.' && PeekNext() == '.') {
                Advance(); Advance();
                return Token{TokenKind::DotDotDot, "...", 0, false, 0, false, loc};
            }
            return Token{TokenKind::Dot, ".", 0, false, 0, false, loc};
        case ':': return Token{TokenKind::Colon, ":", 0, false, 0, false, loc};
        case '?':
            if (Match('?')) {
                if (Match('=')) return Token{TokenKind::QuestionQuestionEqual, "??=", 0, false, 0, false, loc};
                return Token{TokenKind::QuestionQuestion, "??", 0, false, 0, false, loc};
            }
            if (Match('.')) {
                return Token{TokenKind::QuestionDot, "?.", 0, false, 0, false, loc};
            }
            return Token{TokenKind::Question, "?", 0, false, 0, false, loc};
        case '~': return Token{TokenKind::Tilde, "~", 0, false, 0, false, loc};
        case '^':
            if (Match('=')) return Token{TokenKind::CaretEqual, "^=", 0, false, 0, false, loc};
            return Token{TokenKind::Caret, "^", 0, false, 0, false, loc};

        case '+':
            if (Match('+')) return Token{TokenKind::PlusPlus, "++", 0, false, 0, false, loc};
            if (Match('=')) return Token{TokenKind::PlusEqual, "+=", 0, false, 0, false, loc};
            return Token{TokenKind::Plus, "+", 0, false, 0, false, loc};

        case '-':
            if (Match('-')) return Token{TokenKind::MinusMinus, "--", 0, false, 0, false, loc};
            if (Match('=')) return Token{TokenKind::MinusEqual, "-=", 0, false, 0, false, loc};
            return Token{TokenKind::Minus, "-", 0, false, 0, false, loc};

        case '*':
            if (Match('*')) {
                if (Match('=')) return Token{TokenKind::StarStarEqual, "**=", 0, false, 0, false, loc};
                return Token{TokenKind::StarStar, "**", 0, false, 0, false, loc};
            }
            if (Match('=')) return Token{TokenKind::StarEqual, "*=", 0, false, 0, false, loc};
            return Token{TokenKind::Star, "*", 0, false, 0, false, loc};

        case '/':
            if (CanFollowRegExp(last_token_kind_)) {
                return ScanRegExp();
            }
            if (Match('=')) return Token{TokenKind::SlashEqual, "/=", 0, false, 0, false, loc};
            return Token{TokenKind::Slash, "/", 0, false, 0, false, loc};

        case '%':
            if (Match('=')) return Token{TokenKind::PercentEqual, "%=", 0, false, 0, false, loc};
            return Token{TokenKind::Percent, "%", 0, false, 0, false, loc};

        case '=':
            if (Match('>')) {
                return Token{TokenKind::Arrow, "=>", 0, false, 0, false, loc};
            }
            if (Match('=')) {
                if (Match('=')) return Token{TokenKind::StrictEqual, "===", 0, false, 0, false, loc};
                return Token{TokenKind::EqualEqual, "==", 0, false, 0, false, loc};
            }
            return Token{TokenKind::Equal, "=", 0, false, 0, false, loc};

        case '!':
            if (Match('=')) {
                if (Match('=')) return Token{TokenKind::StrictNotEqual, "!==", 0, false, 0, false, loc};
                return Token{TokenKind::NotEqual, "!=", 0, false, 0, false, loc};
            }
            return Token{TokenKind::Exclamation, "!", 0, false, 0, false, loc};

        case '<':
            if (Match('<')) {
                if (Match('=')) return Token{TokenKind::LessLessEqual, "<<=", 0, false, 0, false, loc};
                return Token{TokenKind::LessLess, "<<", 0, false, 0, false, loc};
            }
            if (Match('=')) return Token{TokenKind::LessEqual, "<=", 0, false, 0, false, loc};
            return Token{TokenKind::Less, "<", 0, false, 0, false, loc};

        case '>':
            if (Match('>')) {
                if (Match('>')) {
                    if (Match('=')) return Token{TokenKind::GreaterGreaterGreaterEqual, ">>>=", 0, false, 0, false, loc};
                    return Token{TokenKind::GreaterGreaterGreater, ">>>", 0, false, 0, false, loc};
                }
                if (Match('=')) return Token{TokenKind::GreaterGreaterEqual, ">>=", 0, false, 0, false, loc};
                return Token{TokenKind::GreaterGreater, ">>", 0, false, 0, false, loc};
            }
            if (Match('=')) return Token{TokenKind::GreaterEqual, ">=", 0, false, 0, false, loc};
            return Token{TokenKind::Greater, ">", 0, false, 0, false, loc};

        case '&':
            if (Match('&')) {
                if (Match('=')) return Token{TokenKind::AmpAmpEqual, "&&=", 0, false, 0, false, loc};
                return Token{TokenKind::AmpAmp, "&&", 0, false, 0, false, loc};
            }
            if (Match('=')) return Token{TokenKind::AmpersandEqual, "&=", 0, false, 0, false, loc};
            return Token{TokenKind::Ampersand, "&", 0, false, 0, false, loc};

        case '|':
            if (Match('|')) {
                if (Match('=')) return Token{TokenKind::BarBarEqual, "||=", 0, false, 0, false, loc};
                return Token{TokenKind::BarBar, "||", 0, false, 0, false, loc};
            }
            if (Match('=')) return Token{TokenKind::BarEqual, "|=", 0, false, 0, false, loc};
            return Token{TokenKind::Bar, "|", 0, false, 0, false, loc};

        default:
            return Token{TokenKind::Invalid, std::string(1, c), 0, false, 0, false, loc};
    }
}

Token Lexer::ScanIdentifierOrKeyword() {
    SourceLocation loc{line_, column_};
    size_t start = cursor_;
    while (!IsAtEnd()) {
        char c = Peek();
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            Advance();
        } else {
            break;
        }
    }

    std::string_view text = source_.substr(start, cursor_ - start);
    TokenKind kind = TokenKind::Identifier;

    if (text == "var")             kind = TokenKind::Var;
    else if (text == "let")        kind = TokenKind::Let;
    else if (text == "const")      kind = TokenKind::Const;
    else if (text == "function")   kind = TokenKind::Function;
    else if (text == "return")     kind = TokenKind::Return;
    else if (text == "if")         kind = TokenKind::If;
    else if (text == "else")       kind = TokenKind::Else;
    else if (text == "while")      kind = TokenKind::While;
    else if (text == "for")        kind = TokenKind::For;
    else if (text == "break")      kind = TokenKind::Break;
    else if (text == "continue")   kind = TokenKind::Continue;
    else if (text == "new")        kind = TokenKind::New;
    else if (text == "try")        kind = TokenKind::Try;
    else if (text == "catch")      kind = TokenKind::Catch;
    else if (text == "finally")    kind = TokenKind::Finally;
    else if (text == "throw")      kind = TokenKind::Throw;
    else if (text == "class")      kind = TokenKind::Class;
    else if (text == "extends")    kind = TokenKind::Extends;
    else if (text == "this")       kind = TokenKind::This;
    else if (text == "typeof")     kind = TokenKind::TypeOf;
    else if (text == "instanceof") kind = TokenKind::InstanceOf;
    else if (text == "in")         kind = TokenKind::In;
    else if (text == "delete")     kind = TokenKind::Delete;
    else if (text == "switch")     kind = TokenKind::Switch;
    else if (text == "case")       kind = TokenKind::Case;
    else if (text == "do")         kind = TokenKind::Do;
    else if (text == "void")       kind = TokenKind::Void;
    else if (text == "debugger")   kind = TokenKind::Debugger;
    else if (text == "with")       kind = TokenKind::With;
    else if (text == "super")      kind = TokenKind::Super;
    else if (text == "static")     kind = TokenKind::Static;
    else if (text == "true")       kind = TokenKind::True;
    else if (text == "false")      kind = TokenKind::False;
    else if (text == "null")       kind = TokenKind::Null;
    else if (text == "undefined")  kind = TokenKind::Undefined;
    else if (text == "of")         kind = TokenKind::Of;
    else if (text == "yield")      kind = TokenKind::Yield;
    else if (text == "async")      kind = TokenKind::Async;
    else if (text == "await")      kind = TokenKind::Await;
    else if (text == "import")     kind = TokenKind::Import;
    else if (text == "export")     kind = TokenKind::Export;
    else if (text == "default")    kind = TokenKind::Default;

    return Token{kind, std::string(text), 0, false, 0, false, loc};
}

Token Lexer::ScanNumber() {
    SourceLocation loc{line_, column_};
    size_t start = cursor_;
    bool is_fp = false;
    bool is_bigint = false;

    // 1. Binary check: 0b or 0B
    if (Peek() == '0' && (PeekNext() == 'b' || PeekNext() == 'B')) {
        Advance(); // 0
        Advance(); // b/B
        std::string raw;
        while (!IsAtEnd() && (Peek() == '0' || Peek() == '1' || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
        if (!IsAtEnd() && Peek() == 'n') {
            is_bigint = true;
            Advance();
        }
        std::string text(source_.substr(start, cursor_ - start));
        int64_t val = 0;
        for (char ch : raw) {
            val = (val << 1) | (ch - '0');
        }
        bool is_smi = (!is_bigint && val >= INT32_MIN && val <= INT32_MAX);
        return Token{TokenKind::Number, text, static_cast<double>(val), is_smi, static_cast<int32_t>(val), is_bigint, loc};
    }

    // 2. Octal check: 0o or 0O
    if (Peek() == '0' && (PeekNext() == 'o' || PeekNext() == 'O')) {
        Advance(); // 0
        Advance(); // o/O
        std::string raw;
        while (!IsAtEnd() && ((Peek() >= '0' && Peek() <= '7') || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
        if (!IsAtEnd() && Peek() == 'n') {
            is_bigint = true;
            Advance();
        }
        std::string text(source_.substr(start, cursor_ - start));
        int64_t val = 0;
        for (char ch : raw) {
            val = (val << 3) | (ch - '0');
        }
        bool is_smi = (!is_bigint && val >= INT32_MIN && val <= INT32_MAX);
        return Token{TokenKind::Number, text, static_cast<double>(val), is_smi, static_cast<int32_t>(val), is_bigint, loc};
    }

    // 3. Hex check: 0x or 0X
    if (Peek() == '0' && (PeekNext() == 'x' || PeekNext() == 'X')) {
        Advance(); // 0
        Advance(); // x/X
        std::string raw;
        while (!IsAtEnd() && (std::isxdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
        if (!IsAtEnd() && Peek() == 'n') {
            is_bigint = true;
            Advance();
        }
        std::string text(source_.substr(start, cursor_ - start));
        int64_t val = std::strtoll(raw.c_str(), nullptr, 16);
        bool is_smi = (!is_bigint && val >= INT32_MIN && val <= INT32_MAX);
        return Token{TokenKind::Number, text, static_cast<double>(val), is_smi, static_cast<int32_t>(val), is_bigint, loc};
    }

    // 4. Decimal or leading-dot float
    std::string raw;
    if (Peek() == '.') {
        is_fp = true;
        Advance(); // .
        raw.push_back('.');
        while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
    } else {
        while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
        if (!IsAtEnd() && Peek() == '.' && (std::isdigit(static_cast<unsigned char>(PeekNext())) || PeekNext() == '_')) {
            is_fp = true;
            Advance(); // .
            raw.push_back('.');
            while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
                char ch = Advance();
                if (ch != '_') raw.push_back(ch);
            }
        }
    }

    if (!IsAtEnd() && (Peek() == 'e' || Peek() == 'E')) {
        is_fp = true;
        raw.push_back(Advance()); // e/E
        if (!IsAtEnd() && (Peek() == '+' || Peek() == '-')) {
            raw.push_back(Advance());
        }
        while (!IsAtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '_')) {
            char ch = Advance();
            if (ch != '_') raw.push_back(ch);
        }
    }

    if (!is_fp && !IsAtEnd() && Peek() == 'n') {
        is_bigint = true;
        Advance(); // n
    }

    std::string text(source_.substr(start, cursor_ - start));
    if (!is_fp && !is_bigint) {
        int64_t val = std::strtoll(raw.c_str(), nullptr, 10);
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return Token{TokenKind::Number, text, static_cast<double>(val), true, static_cast<int32_t>(val), false, loc};
        }
    }

    double val = std::strtod(raw.c_str(), nullptr);
    return Token{TokenKind::Number, text, val, false, 0, is_bigint, loc};
}

Token Lexer::ScanString(char quote) {
    SourceLocation loc{line_, column_};
    std::string result;

    while (!IsAtEnd() && Peek() != quote) {
        char c = Advance();
        if (c == '\\') {
            if (IsAtEnd()) break;
            char esc = Advance();
            switch (esc) {
                case '0': result.push_back('\0'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'v': result.push_back('\v'); break;
                case 'n': result.push_back('\n'); break;
                case 't': result.push_back('\t'); break;
                case 'r':
                    if (!IsAtEnd() && Peek() == '\n') Advance();
                    line_++;
                    column_ = 1;
                    break;
                case '\n':
                    line_++;
                    column_ = 1;
                    break;
                case '\\': result.push_back('\\'); break;
                case '\'': result.push_back('\''); break;
                case '"': result.push_back('"'); break;
                case 'x': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 2 && !IsAtEnd() && std::isxdigit(static_cast<unsigned char>(Peek())); ++i) {
                        char hc = Advance();
                        cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                    }
                    result.push_back(static_cast<char>(cp));
                    break;
                }
                case 'u': {
                    if (!IsAtEnd() && Peek() == '{') {
                        Advance(); // '{'
                        uint32_t cp = 0;
                        while (!IsAtEnd() && Peek() != '}') {
                            char hc = Advance();
                            if (std::isxdigit(static_cast<unsigned char>(hc))) {
                                cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                            }
                        }
                        if (!IsAtEnd() && Peek() == '}') Advance();
                        AppendUtf8(result, cp);
                    } else {
                        uint32_t cp = 0;
                        for (int i = 0; i < 4 && !IsAtEnd() && std::isxdigit(static_cast<unsigned char>(Peek())); ++i) {
                            char hc = Advance();
                            cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                        }
                        AppendUtf8(result, cp);
                    }
                    break;
                }
                default: result.push_back(esc); break;
            }
        } else {
            result.push_back(c);
        }
    }

    if (!IsAtEnd() && Peek() == quote) {
        Advance(); // Consume closing quote
    }

    return Token{TokenKind::String, result, 0.0, false, 0, false, loc};
}

Token Lexer::ScanTemplateLiteral() {
    SourceLocation loc{line_, column_};
    Token tok{TokenKind::Backtick, "", 0.0, false, 0, false, loc};

    std::string current_quasi;
    while (!IsAtEnd()) {
        char c = Peek();
        if (c == '`') {
            Advance(); // Consume closing backtick
            tok.template_quasis.push_back(std::move(current_quasi));
            return tok;
        } else if (c == '$' && PeekNext() == '{') {
            Advance(); // Consume '$'
            Advance(); // Consume '{'
            tok.template_quasis.push_back(std::move(current_quasi));
            current_quasi.clear();

            // Scan embedded expression until closing '}'
            int brace_depth = 1;
            std::string expr_text;
            while (!IsAtEnd() && brace_depth > 0) {
                char ec = Peek();
                if (ec == '{') {
                    brace_depth++;
                    expr_text.push_back(Advance());
                } else if (ec == '}') {
                    brace_depth--;
                    if (brace_depth == 0) {
                        Advance(); // Consume closing '}'
                        break;
                    }
                    expr_text.push_back(Advance());
                } else if (ec == '"' || ec == '\'') {
                    char q = Advance();
                    expr_text.push_back(q);
                    while (!IsAtEnd() && Peek() != q) {
                        if (Peek() == '\\') {
                            expr_text.push_back(Advance());
                        }
                        if (!IsAtEnd()) expr_text.push_back(Advance());
                    }
                    if (!IsAtEnd()) expr_text.push_back(Advance());
                } else if (ec == '`') {
                    char q = Advance();
                    expr_text.push_back(q);
                    while (!IsAtEnd() && Peek() != '`') {
                        if (Peek() == '\\') {
                            expr_text.push_back(Advance());
                        }
                        if (!IsAtEnd()) expr_text.push_back(Advance());
                    }
                    if (!IsAtEnd()) expr_text.push_back(Advance());
                } else {
                    if (ec == '\n') {
                        line_++;
                        column_ = 1;
                    }
                    expr_text.push_back(Advance());
                }
            }
            tok.template_exprs.push_back(std::move(expr_text));
        } else if (c == '\\') {
            Advance(); // Consume '\'
            if (IsAtEnd()) break;
            char esc = Advance();
            switch (esc) {
                case '0': current_quasi.push_back('\0'); break;
                case 'b': current_quasi.push_back('\b'); break;
                case 'f': current_quasi.push_back('\f'); break;
                case 'v': current_quasi.push_back('\v'); break;
                case 'n': current_quasi.push_back('\n'); break;
                case 't': current_quasi.push_back('\t'); break;
                case 'r': current_quasi.push_back('\r'); break;
                case '\\': current_quasi.push_back('\\'); break;
                case '`': current_quasi.push_back('`'); break;
                case '$': current_quasi.push_back('$'); break;
                case 'x': {
                    uint32_t cp = 0;
                    for (int i = 0; i < 2 && !IsAtEnd() && std::isxdigit(static_cast<unsigned char>(Peek())); ++i) {
                        char hc = Advance();
                        cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                    }
                    current_quasi.push_back(static_cast<char>(cp));
                    break;
                }
                case 'u': {
                    if (!IsAtEnd() && Peek() == '{') {
                        Advance();
                        uint32_t cp = 0;
                        while (!IsAtEnd() && Peek() != '}') {
                            char hc = Advance();
                            if (std::isxdigit(static_cast<unsigned char>(hc))) {
                                cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                            }
                        }
                        if (!IsAtEnd() && Peek() == '}') Advance();
                        AppendUtf8(current_quasi, cp);
                    } else {
                        uint32_t cp = 0;
                        for (int i = 0; i < 4 && !IsAtEnd() && std::isxdigit(static_cast<unsigned char>(Peek())); ++i) {
                            char hc = Advance();
                            cp = (cp << 4) | (std::isdigit(static_cast<unsigned char>(hc)) ? (hc - '0') : (std::tolower(static_cast<unsigned char>(hc)) - 'a' + 10));
                        }
                        AppendUtf8(current_quasi, cp);
                    }
                    break;
                }
                default: current_quasi.push_back(esc); break;
            }
        } else {
            if (c == '\r') {
                Advance();
                if (!IsAtEnd() && Peek() == '\n') Advance();
                line_++;
                column_ = 1;
                current_quasi.push_back('\n');
            } else if (c == '\n') {
                line_++;
                column_ = 1;
                current_quasi.push_back(Advance());
            } else {
                current_quasi.push_back(Advance());
            }
        }
    }
    tok.template_quasis.push_back(std::move(current_quasi));
    return tok;
}

Token Lexer::ScanRegExp() {
    SourceLocation loc{line_, column_};
    std::string pattern;
    std::string flags;
    bool in_class = false;
    while (!IsAtEnd()) {
        char ch = Advance();
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == '\\') {
            pattern.push_back('\\');
            if (!IsAtEnd()) {
                pattern.push_back(Advance());
            }
            continue;
        }
        if (ch == '[') {
            in_class = true;
            pattern.push_back(ch);
            continue;
        }
        if (ch == ']' && in_class) {
            in_class = false;
            pattern.push_back(ch);
            continue;
        }
        if (ch == '/' && !in_class) {
            while (!IsAtEnd() && std::isalpha(static_cast<unsigned char>(Peek()))) {
                flags.push_back(Advance());
            }
            break;
        }
        pattern.push_back(ch);
    }
    Token tok{TokenKind::RegExp, pattern, 0.0, false, 0, false, loc};
    tok.regex_flags = std::move(flags);
    return tok;
}

} // namespace aether
