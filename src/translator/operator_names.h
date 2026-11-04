// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The names of the script-defined operators.
//
// The parser maps an operator member "T operator+(in T o)" to a normal method. The identifier
// "op_add" is the name of the method. The symbol "+" is in "Method::op". The arity is the number
// of parameters. The left operand is "this".
#pragma once

#include "ast.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hotc
{

namespace op_names_detail
{

struct OpRow
{
    std::string_view sym;
    std::string_view ident;
    int arity; // the parameter count
    bool binary; // an arithmetic or bit operator
    bool compare;
};

inline constexpr OpRow kOpRows[] = {
    {"+", "op_add", 1, true, false},
    {"-", "op_sub", 1, true, false},
    {"*", "op_mul", 1, true, false},
    {"/", "op_div", 1, true, false},
    {"%", "op_mod", 1, true, false},
    {"&", "op_and", 1, true, false},
    {"|", "op_or", 1, true, false},
    {"^", "op_xor", 1, true, false},
    {"<<", "op_shl", 1, true, false},
    {">>", "op_shr", 1, true, false},
    {"==", "op_eq", 1, false, true},
    {"<", "op_lt", 1, false, true},
    {">", "op_gt", 1, false, true},
    {"<=", "op_le", 1, false, true},
    {">=", "op_ge", 1, false, true},
    {"[]", "op_index", 1, false, false},
    {"!", "op_not", 0, false, false},
    {"~", "op_compl", 0, false, false},
};

struct CppOpRow
{
    std::string_view sym;
    std::string_view ident;
    bool any_arity; // true: the operator takes any parameter count
};

// The operators that only a .cpp script class defines. A .hot class does not define them.
inline constexpr CppOpRow kCppOpRows[] = {
    {"!=", "op_ne", false},
    {"()", "op_call", true},
    {"+=", "op_add_assign", false},
    {"-=", "op_sub_assign", false},
    {"*=", "op_mul_assign", false},
    {"/=", "op_div_assign", false},
    {"%=", "op_mod_assign", false},
    {"&=", "op_and_assign", false},
    {"|=", "op_or_assign", false},
    {"^=", "op_xor_assign", false},
    {"<<=", "op_shl_assign", false},
    {">>=", "op_shr_assign", false},
};

} // namespace op_names_detail

// The identifier of an operator symbol with the given parameter count. Empty when the script
// cannot define the operator (the checker reports it).
inline std::optional<std::string> op_ident(std::string_view sym, size_t arity)
{
    if (arity == 0)
    {
        if (sym == "-")
            return std::string("op_neg");
        if (sym == "+")
            return std::string("op_pos");
    }
    for (const op_names_detail::OpRow& r : op_names_detail::kOpRows)
        if (r.sym == sym && static_cast<size_t>(r.arity) == arity)
            return std::string(r.ident);
    return std::nullopt;
}

// The symbol of an operator identifier. Empty when the text is not an operator identifier.
inline std::string op_symbol(std::string_view ident)
{
    if (ident == "op_neg")
        return "-";
    if (ident == "op_pos")
        return "+";
    for (const op_names_detail::OpRow& r : op_names_detail::kOpRows)
        if (r.ident == ident)
            return std::string(r.sym);
    return {};
}

// The identifier of an operator of a .cpp script class. The .hot operators come first. Empty when
// the frontend cannot define the operator.
inline std::optional<std::string> cpp_op_ident(std::string_view sym, size_t arity)
{
    if (std::optional<std::string> id = op_ident(sym, arity))
        return id;
    for (const op_names_detail::CppOpRow& r : op_names_detail::kCppOpRows)
        if (r.sym == sym && (r.any_arity || arity == 1))
            return std::string(r.ident);
    return std::nullopt;
}

// The symbol of an operator identifier of a .cpp script class. Empty when the text is not an
// operator identifier.
inline std::string cpp_op_symbol(std::string_view ident)
{
    std::string sym = op_symbol(ident);
    if (!sym.empty())
        return sym;
    for (const op_names_detail::CppOpRow& r : op_names_detail::kCppOpRows)
        if (r.ident == ident)
            return std::string(r.sym);
    return {};
}

// True for the identifiers of the arithmetic and bit operators ("op_add" ... "op_shr").
inline bool is_binary_op_ident(std::string_view ident)
{
    for (const op_names_detail::OpRow& r : op_names_detail::kOpRows)
        if (r.ident == ident)
            return r.binary;
    return false;
}

// True for the identifiers of the compare operators ("op_eq", "op_lt", "op_gt", "op_le", "op_ge").
inline bool is_compare_op_ident(std::string_view ident)
{
    for (const op_names_detail::OpRow& r : op_names_detail::kOpRows)
        if (r.ident == ident)
            return r.compare;
    return false;
}

// The name of the method in C++: "operator+" for an operator, else the name of the method.
inline std::string cxx_member_name(const ast::Method& m)
{
    return m.op.empty() ? m.name : "operator" + m.op;
}

} // namespace hotc
