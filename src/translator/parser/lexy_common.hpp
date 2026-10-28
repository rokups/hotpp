// Shared lexy parts of the script grammars. Only .cpp files and this file include lexy.
//
// All grammars parse the same text buffer with lexy::string_input. A position is a pointer
// into this buffer. The offset of a position is "ptr - state.base". A sub-grammar in another
// translation unit works with "span then call": the parent grammar scans the span of a child
// construct, stores the begin and end offsets and calls a plain function for the span.
//
// The scan helpers (scan_balanced, scan_until, scan_type) never run past the end of the
// input. At the end of the input they raise the error "unexpected end of file".
#pragma once

#include <lexy/action/parse.hpp>
#include <lexy/callback.hpp>
#include <lexy/dsl.hpp>
#include <lexy/input/string_input.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "../ast.h"
#include "../builtin_types.h"
#include "../diag.h"
#include "span.h"

namespace sppc::parser {

namespace dsl = lexy::dsl;

// The state that lexy passes to the callbacks and to the error sink.
struct ParseState {
    const char* base;
    const LineMap* map;
    Diagnostics* diag;
    size_t size = 0;  // the size of the whole buffer that starts at base

    uint32_t offset(const char* p) const { return static_cast<uint32_t>(p - base); }
    void error(const char* p, std::string msg) const { diag->add(offset(p), *map, std::move(msg)); }
};

// The input is bytes. No byte is wrong: the old lexer accepted any byte.
using Encoding = lexy::default_encoding;

// One byte of any value.
constexpr auto any_unit = -dsl::lit_b<0xFF> / dsl::lit_b<0xFF>;

// ---------------------------------------------------------------- error tags
// The name member is the text of the error.
#define SPP_ERROR_TAG(NAME, TEXT) \
    struct NAME {                 \
        static constexpr auto name = TEXT; \
    }

SPP_ERROR_TAG(e_unterminated_comment, "unterminated comment");
SPP_ERROR_TAG(e_unterminated_literal, "unterminated string or character literal");
SPP_ERROR_TAG(e_unexpected_eof, "unexpected end of file");
SPP_ERROR_TAG(e_interpolation, "string interpolation is not supported");
SPP_ERROR_TAG(e_use_dot_colons, "use '.' instead of '::'");
// The parse reads the "->" token before it raises this error. "rewind" is the size of the token:
// the report of the error (detail::report) moves back to the start of the token.
struct e_use_dot_arrow {
    static constexpr auto name = "use '.' instead of '->'";
    static constexpr std::size_t rewind = 2;
};
// The removed prefix "&". Same as above: the report moves back to the "&".
struct e_amp_prefix {
    static constexpr auto name = "use 'ref' instead of '&'";
    static constexpr std::size_t rewind = 1;
};

// ---------------------------------------------------------------- white space
constexpr auto line_comment = LEXY_LIT("//") >> dsl::until(dsl::newline).or_eof();
constexpr auto block_comment =
    dsl::peek(LEXY_LIT("/*")) >>
    dsl::token(LEXY_LIT("/*") + dsl::until(LEXY_LIT("*/"))).error<e_unterminated_comment>;
// Space and the two comment forms.
constexpr auto ws_rule = dsl::ascii::space | line_comment | block_comment;

// ---------------------------------------------------------------- identifiers
// The same characters as the old lexer.
constexpr auto id = dsl::identifier(dsl::ascii::alpha_underscore, dsl::ascii::word);

constexpr auto kw_using = LEXY_KEYWORD("using", id);
// "import" is removed. The parser still knows the word to give a clear error.
constexpr auto kw_import = LEXY_KEYWORD("import", id);
constexpr auto kw_class = LEXY_KEYWORD("class", id);
constexpr auto kw_override = LEXY_KEYWORD("override", id);
constexpr auto kw_function = LEXY_KEYWORD("function", id);
constexpr auto kw_virtual = LEXY_KEYWORD("virtual", id);
constexpr auto kw_new = LEXY_KEYWORD("new", id);
constexpr auto kw_public = LEXY_KEYWORD("public", id);
constexpr auto kw_private = LEXY_KEYWORD("private", id);
constexpr auto kw_protected = LEXY_KEYWORD("protected", id);
constexpr auto kw_var = LEXY_KEYWORD("var", id);
constexpr auto kw_foreach = LEXY_KEYWORD("foreach", id);
constexpr auto kw_in = LEXY_KEYWORD("in", id);
// "get" and "set" are the accessor words of a property. They are not names.
// "finally" starts the last block of a try statement.
constexpr auto kw_get = LEXY_KEYWORD("get", id);
constexpr auto kw_set = LEXY_KEYWORD("set", id);
constexpr auto kw_finally = LEXY_KEYWORD("finally", id);
constexpr auto kw_void = LEXY_KEYWORD("void", id);
// "ref" and "out" mark a reference parameter, a reference argument or a reference local.
constexpr auto kw_ref = LEXY_KEYWORD("ref", id);
constexpr auto kw_out = LEXY_KEYWORD("out", id);

// ---------------------------------------------------------------- literals
// A string or character literal. It has two values: the position after the opening quote
// and the position of the closing quote. A backslash hides the next char. No newline inside.
template <char Quote>
constexpr auto quoted =
    dsl::lit_c<Quote> >>
    (dsl::position +
        dsl::loop(dsl::peek(dsl::lit_c<Quote>) >> dsl::break_ |
                  dsl::lit_c<'\\'> >> dsl::if_(any_unit) |
                  dsl::peek(dsl::newline) >> dsl::error<e_unterminated_literal> |
                  dsl::eof >> dsl::error<e_unterminated_literal> |
                  dsl::else_ >> any_unit) +
        dsl::position + dsl::lit_c<Quote>);

constexpr auto string_lit = quoted<'"'>;
constexpr auto char_lit = quoted<'\''>;

// ---------------------------------------------------------------- values
// The value of a scan production: the span between the two positions that it produces.
constexpr auto span_value = lexy::bind(
    lexy::callback<Span>([](const ParseState& st, const char* b, const char* e) {
        return Span{st.offset(b), st.offset(e)};
    }),
    lexy::parse_state, lexy::values);

// ---------------------------------------------------------------- scan helpers
// scan_balanced: skips one item inside braces. It counts braces only and skips strings,
// characters and comments. The item is a nested block or one code point.
struct brace_item : lexy::token_production {
    static constexpr auto rule = [] {
        auto nested =
            dsl::lit_c<'{'> >> dsl::loop(dsl::lit_c<'}'> >> dsl::break_ |
                                         dsl::eof >> dsl::error<e_unexpected_eof> |
                                         dsl::else_ >> dsl::recurse<brace_item>);
        return string_lit | char_lit | line_comment | block_comment | nested |
               dsl::else_ >> any_unit;
    }();
    static constexpr auto value = lexy::noop;
};

// Skips one item for an error recovery: a block, a literal, a comment, a word or a code point.
struct skip_item : lexy::token_production {
    static constexpr auto rule = dsl::peek(dsl::lit_c<'{'>) >> dsl::p<brace_item> | string_lit |
                                 char_lit | line_comment | block_comment | dsl::token(id) |
                                 dsl::else_ >> any_unit;
    static constexpr auto value = lexy::noop;
};

// Matches "{ ... }". The value is the span between the braces.
struct scan_balanced : lexy::token_production {
    static constexpr auto rule =
        dsl::lit_c<'{'> + dsl::position +
        dsl::loop(dsl::peek(dsl::lit_c<'}'>) >> dsl::break_ |
                  dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<brace_item>) +
        dsl::position + dsl::lit_c<'}'>;
    static constexpr auto value = span_value;
};

// scan_until: skips one item of an initializer. The depth counts (), [] and {} together.
struct until_item : lexy::token_production {
    static constexpr auto rule = [] {
        auto open = dsl::lit_c<'('> / dsl::lit_c<'['> / dsl::lit_c<'{'>;
        auto close = dsl::lit_c<')'> / dsl::lit_c<']'> / dsl::lit_c<'}'>;
        auto nested = open >> dsl::loop(close >> dsl::break_ |
                                        dsl::eof >> dsl::error<e_unexpected_eof> |
                                        dsl::else_ >> dsl::recurse<until_item>);
        return string_lit | char_lit | line_comment | block_comment | nested |
               dsl::else_ >> any_unit;
    }();
    static constexpr auto value = lexy::noop;
};

// Matches "<Open> ... " up to a terminator at depth zero. The Open rule is a literal.
// The terminator is the ";" of the Cfg type or a closer at depth zero. It stays in the input.
// The value is the span after the opener and before the terminator.
template <typename Cfg>
struct scan_until : lexy::token_production {
    static constexpr auto rule =
        Cfg::open + dsl::position +
        dsl::loop(dsl::peek(Cfg::term) >> dsl::break_ |
                  dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<until_item>) +
        dsl::position;
    static constexpr auto value = span_value;
};

// scan_type: skips one item of a type. The depth counts <>, () and [] on its own.
// A ">>" counts as two closers. The tokens "->", ">=", ">>=" and so on are not closers.
constexpr auto type_ops = dsl::literal_set(
    LEXY_LIT("<<="), LEXY_LIT(">>="), LEXY_LIT("..."), LEXY_LIT("::"), LEXY_LIT("->"),
    LEXY_LIT("++"), LEXY_LIT("--"), LEXY_LIT("+="), LEXY_LIT("-="), LEXY_LIT("*="),
    LEXY_LIT("/="), LEXY_LIT("%="), LEXY_LIT("=="), LEXY_LIT("!="), LEXY_LIT("<="),
    LEXY_LIT(">="), LEXY_LIT("&&"), LEXY_LIT("||"), LEXY_LIT("<<"), LEXY_LIT("&="),
    LEXY_LIT("|="), LEXY_LIT("^="), LEXY_LIT("=>"));

struct type_item : lexy::token_production {
    static constexpr auto rule = [] {
        auto nest = [](auto open, auto close) {
            return open >> dsl::loop(close >> dsl::break_ |
                                     dsl::eof >> dsl::error<e_unexpected_eof> |
                                     dsl::else_ >> dsl::recurse<type_item>);
        };
        return string_lit | char_lit | line_comment | block_comment | type_ops |
               nest(dsl::lit_c<'('>, dsl::lit_c<')'>) | nest(dsl::lit_c<'['>, dsl::lit_c<']'>) |
               nest(dsl::lit_c<'<'>, dsl::lit_c<'>'>) |
               dsl::lit_c<'`'> >> dsl::error<e_interpolation> | dsl::else_ >> any_unit;
    }();
    static constexpr auto value = lexy::noop;
};

// Matches a type up to a terminator at depth zero. The terminator stays in the input.
// The value is the span of the type. The span can be empty.
template <typename Cfg>
struct scan_type : lexy::token_production {
    static constexpr auto rule =
        dsl::position +
        dsl::loop(type_ops | dsl::peek(Cfg::term) >> dsl::break_ |
                  dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<type_item>) +
        dsl::position;
    static constexpr auto value = span_value;
};

// The name of a "Type name" parameter: an identifier before a "," or a ")".
constexpr auto param_name_ahead =
    id + dsl::whitespace(ws_rule) + (dsl::lit_c<','> / dsl::lit_c<')'>);
// The old form "name: type". A "::" is not the old form.
constexpr auto old_param_ahead =
    id + dsl::whitespace(ws_rule) + dsl::lit_c<':'> + dsl::peek_not(dsl::lit_c<':'>);

// Matches the type of a "Type name" parameter. The scan stops before the name.
// The name is the last identifier before a "," or a ")" at depth zero.
// An identifier after a "." is part of the type, so "std.string" is a type and not a name.
// The value is the span of the type. The span is empty when the parameter has no type.
struct scan_param_type : lexy::token_production {
    static constexpr auto rule =
        dsl::position +
        dsl::loop(dsl::peek(param_name_ahead) >> dsl::break_ |
                  dsl::peek(dsl::lit_c<','> / dsl::lit_c<')'>) >> dsl::break_ | type_ops |
                  dsl::lit_c<'.'> >> dsl::if_(dsl::token(id)) | dsl::token(id) |
                  dsl::eof >> dsl::error<e_unexpected_eof> | dsl::else_ >> dsl::p<type_item>) +
        dsl::position;
    static constexpr auto value = span_value;
};

// The optional word "ref" or "out" before a parameter. The value is the pass mode.
struct pass_mode_p {
    static constexpr auto rule = dsl::opt(dsl::capture(dsl::literal_set(kw_ref, kw_out)));
    static constexpr auto value = lexy::callback<ast::PassMode>(
        [](lexy::nullopt) { return ast::PassMode::Value; },
        [](const auto& word) {
            return *word.begin() == 'r' ? ast::PassMode::Ref : ast::PassMode::Out;
        });
};

// ---------------------------------------------------------------- plain text helpers
inline bool is_id0(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
inline bool is_id(char c) { return is_id0(c) || std::isdigit(static_cast<unsigned char>(c)); }
inline bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// True when the text has only white space and comments.
inline bool only_blank(std::string_view s)
{
    size_t i = 0;
    while (i < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[i]))) {
            ++i;
        } else if (s.compare(i, 2, "//") == 0) {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (s.compare(i, 2, "/*") == 0) {
            size_t e = s.find("*/", i + 2);
            i = e == std::string_view::npos ? s.size() : e + 2;
        } else {
            return false;
        }
    }
    return true;
}

// The text of a type: the tokens, joined like the old join_type function.
// A space goes between two words and after a comma. The comments and the white space go away.
// A "." between two names is a scope separator in the script. The result has "::" in its place.
// So the type text is C++ text. The rest of the translator compares it with the spelling
// of libclang, which uses "::". The script has no "::". If the text has one, *colons is the
// offset of the first one. The caller reports the error.
inline std::string type_text(std::string_view s, std::size_t* colons = nullptr)
{
    static const char* const p3[] = {"<<=", ">>=", "...", nullptr};
    static const char* const p2[] = {"::", "->", "++", "--", "+=", "-=", "*=", "/=", "%=", "==",
                                     "!=", "<=", ">=", "&&", "||", "<<", ">>", "&=", "|=", "^=",
                                     "=>", nullptr};
    std::string out;
    bool prev_word = false, prev_comma = false, have_prev = false, prev_operand = false;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (s.compare(i, 2, "//") == 0) {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (s.compare(i, 2, "/*") == 0) {
            size_t e = s.find("*/", i + 2);
            i = e == std::string_view::npos ? n : e + 2;
            continue;
        }
        const size_t b = i;
        bool word = false;
        if (is_id0(c)) {
            while (i < n && is_id(s[i])) ++i;
            word = true;
        } else if (is_digit(c) || (c == '.' && i + 1 < n && is_digit(s[i + 1]) && !prev_operand)) {
            const bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            while (i < n) {
                const char d = s[i];
                if (is_id(d) || d == '.')
                    ++i;
                else if ((d == '+' || d == '-') && !hex && (s[i - 1] == 'e' || s[i - 1] == 'E'))
                    ++i;
                else
                    break;
            }
            word = true;
        } else if (c == '"' || c == '\'') {
            ++i;
            while (i < n && s[i] != c) {
                if (s[i] == '\\') ++i;
                ++i;
            }
            i = i < n ? i + 1 : n;
        } else {
            size_t len = 1;
            for (int k = 0; p3[k]; ++k)
                if (s.compare(i, 3, p3[k]) == 0) len = 3;
            if (len == 1)
                for (int k = 0; p2[k]; ++k)
                    if (s.compare(i, 2, p2[k]) == 0) len = 2;
            i += len;
        }
        std::string_view tok = s.substr(b, i - b);
        if (tok == "::" && colons && *colons == std::string_view::npos) *colons = b;
        if (tok == ".") tok = "::";
        if (have_prev && ((prev_word && word) || prev_comma)) out += ' ';
        out += tok;
        have_prev = true;
        prev_word = word;
        prev_comma = tok == ",";
        prev_operand = word || tok == ")" || tok == "]";
    }
    return out;
}


// The canonical text of a type: the aliases become the C++ names ("ulong" and "uint64" give
// "uint64_t"). An old C++ spelling such as "unsigned int" is an error with a hint at "pos".
inline std::string canonical_type_at(const ParseState& st, const char* pos, const std::string& t)
{
    std::string error;
    std::string r = canonical_type(t, &error);
    if (!error.empty()) st.error(pos, error);
    return r;
}

// The type text of a span. An empty type is an error. A "::" is an error: the script uses ".".
inline std::string type_of_span(const ParseState& st, Span sp)
{
    const std::string_view raw(st.base + sp.begin, sp.end - sp.begin);
    if (only_blank(raw)) st.error(st.base + sp.begin, "expected a type");
    std::size_t colons = std::string_view::npos;
    std::string t = type_text(raw, &colons);
    if (colons != std::string_view::npos) st.error(st.base + sp.begin + colons, e_use_dot_colons::name);
    return canonical_type_at(st, st.base + sp.begin, t);
}

// Rewrites the scope separators of the raw text of template arguments: "." between two names
// becomes "::". A number such as "1.5" and a pack "..." stay. If the text has a "::", the
// function reports the error at the first one. "begin" is the position of the text.
inline std::string scope_text(const ParseState& st, const char* begin, std::string_view t)
{
    const std::size_t colons = t.find("::");
    if (colons != std::string_view::npos) st.error(begin + colons, e_use_dot_colons::name);
    std::string out;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const bool scope = t[i] == '.' && i > 0 && i + 1 < t.size() &&
                           (is_id(t[i - 1]) || t[i - 1] == '>') && is_id0(t[i + 1]);
        if (scope)
            out += "::";
        else
            out += t[i];
    }
    return out;
}

// ---------------------------------------------------------------- error sink
namespace detail {

inline void add_unique(const ParseState& st, const char* pos, std::string msg)
{
    const uint32_t off = st.offset(pos);
    // A recovery rule can raise the same error again at the same place. Keep one.
    if (!st.diag->items.empty() && st.diag->items.back().pos == off &&
        st.diag->items.back().msg == msg)
        return;
    st.diag->add(off, *st.map, std::move(msg));
}

template <typename Reader>
void report(const ParseState& st, const lexy::error<Reader, lexy::expected_literal>& e)
{
    add_unique(st, e.position(), "expected '" + std::string(e.string(), e.length()) + "'");
}

template <typename Reader>
void report(const ParseState& st, const lexy::error<Reader, lexy::expected_keyword>& e)
{
    add_unique(st, e.position(), "expected '" + std::string(e.string(), e.length()) + "'");
}

template <typename Reader>
void report(const ParseState& st, const lexy::error<Reader, lexy::expected_char_class>& e)
{
    add_unique(st, e.position(), std::string("expected ") + e.name());
}

// A generic error. A range that is not empty is the found text.
template <typename Reader, typename Tag>
void report(const ParseState& st, const lexy::error<Reader, Tag>& e)
{
    std::string msg = e.message();
    // The found text is left out when it is empty or has a control char (a NUL or a line end).
    const std::string found(e.begin(), e.end());
    const bool plain = !found.empty() && std::none_of(found.begin(), found.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x20 || c == 0x7f;
    });
    if (plain) msg += ", found '" + found + "'";
    const char* pos = e.position();
    // The sink gets a generic error, so the code finds the tag by its text.
    if (e.template is<e_use_dot_arrow>()) pos -= e_use_dot_arrow::rewind;
    if (e.template is<e_amp_prefix>()) pos -= e_amp_prefix::rewind;
    add_unique(st, pos, std::move(msg));
}

}  // namespace detail

// Parses the whole text with the root production. Each error goes to the diagnostics.
// The result has a value when the parse recovered from all errors.
template <typename Production, typename Input>
auto parse_with_diag(const Input& input, const ParseState& st)
{
    auto sink = lexy::callback<void>(
        [&st](const auto&, const auto& e) { detail::report(st, e); });
    return lexy::parse<Production>(input, st, sink);
}

}  // namespace sppc::parser
