#ifndef AETHER_LEXER_H
#define AETHER_LEXER_H

#include "aether/common.h"
#include <string>
#include <string_view>

namespace aether {

enum class TokenKind {
    EndOfFile,
    Invalid,

    // Literals
    Number,
    String,
    True,
    False,
    Null,
    Undefined,

    // Identifiers & Keywords
    Identifier,
    Var,
    Let,
    Const,
    Function,
    Return,
    If,
    Else,
    While,
    For,
    Break,
    Continue,
    New,
    Try,
    Catch,
    Finally,
    Throw,
    Class,
    Extends,
    This,
    TypeOf,
    InstanceOf,

    // Operators & Punctuators
    OpenParen,          // (
    CloseParen,         // )
    OpenBrace,          // {
    CloseBrace,         // }
    OpenBracket,        // [
    CloseBracket,       // ]
    Semicolon,          // ;
    Comma,              // ,
    Dot,                // .
    Colon,              // :
    Question,           // ?

    Plus,               // +
    Minus,              // -
    Star,               // *
    Slash,              // /
    Percent,            // %
    Equal,              // =
    PlusEqual,          // +=
    MinusEqual,         // -=
    StarEqual,          // *=
    SlashEqual,         // /=

    EqualEqual,         // ==
    StrictEqual,        // ===
    NotEqual,           // !=
    StrictNotEqual,     // !==
    Less,               // <
    LessEqual,          // <=
    Greater,            // >
    GreaterEqual,       // >=

    Ampersand,          // &
    Bar,                // |
    Caret,              // ^
    Tilde,              // ~
    Exclamation,        // !
    AmpAmp,             // &&
    BarBar,             // ||
    LessLess,           // <<
    GreaterGreater,     // >>
    PlusPlus,           // ++
    MinusMinus,         // --

    // Modern ECMAScript operators & punctuators
    QuestionQuestion,       // ??
    QuestionDot,            // ?.
    QuestionQuestionEqual,  // ??=
    AmpAmpEqual,            // &&=
    BarBarEqual,            // ||=
    DotDotDot,              // ...
    Arrow,                  // =>
    Backtick,               // `
    Of,                     // of
    Yield,                  // yield
    Async,                  // async
    Await,                  // await
};

const char* TokenKindToString(TokenKind kind);

struct SourceLocation {
    uint32_t line{1};
    uint32_t column{1};
};

struct Token {
    TokenKind kind{TokenKind::Invalid};
    std::string text;
    double number_value{0.0};
    bool is_integer{false};
    int32_t int_value{0};
    SourceLocation location;
    std::vector<std::string> template_quasis;
    std::vector<std::string> template_exprs;
};

class Lexer {
public:
    explicit Lexer(std::string_view source);

    Token NextToken();
    Token PeekToken();

private:
    char Peek() const noexcept;
    char PeekNext() const noexcept;
    char Advance() noexcept;
    bool Match(char expected) noexcept;
    bool IsAtEnd() const noexcept;

    void SkipWhitespaceAndComments();
    Token ScanIdentifierOrKeyword();
    Token ScanNumber();
    Token ScanString(char quote);
    Token ScanTemplateLiteral();

    std::string_view source_;
    size_t cursor_{0};
    uint32_t line_{1};
    uint32_t column_{1};
    std::optional<Token> peeked_token_;
};

} // namespace aether

#endif // AETHER_LEXER_H
