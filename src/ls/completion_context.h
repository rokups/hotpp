// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The completion context of hot-ls (M5, step 16).
//
// classify() is a pure function. It scans the text from the start up to the cursor offset.
// It skips strings, char literals, line comments and block comments. It counts the brackets
// "{ } ( ) [ ]". It counts "<" and ">" only when "<" follows a name in a type position.
// Then it tells what the user can type at the cursor.
#pragma once

#include <cstddef>
#include <string_view>

namespace hot::ls
{

struct CompletionContext
{
    enum class Kind
    {
        None,         // inside a comment or a string, or a position that takes a new name
        Type,         // a type name
        Base,         // a base class or an interface in the list after "class X :"
        Attribute,    // an attribute name after "["
        AttributeArg, // an argument in "[Attr(...)]"
        Using,        // the target of "using", also the path in a string
        Override,     // the method after "override"
        EnumBody,     // an enum member name
        Expr,         // an expression
        Member,       // a member after "." or "::" or "->"
    };

    Kind kind = Kind::None;
    // The offset where the identifier under the cursor starts. It equals the offset when
    // the cursor is not after an identifier character.
    std::size_t prefixStart = 0;
    // True when the position takes a type or an expression, for example a statement start
    // ("No|" is a local declaration or a call) or a cast ("(No|"). "kind" is the first guess.
    // The caller offers the names of both kinds.
    bool exprAndType = false;
    // True when a declaration can start at the word: the statement is empty or has only modifiers,
    // in a file, a namespace, a class or a parameter list. The caller offers declaration keywords.
    bool declStart = false;
};

// Returns the context at "offset" (0 to text.size()). A bigger offset counts as text.size().
CompletionContext classify(std::string_view text, std::size_t offset);

// The name of a kind, for the test messages.
const char* kindName(CompletionContext::Kind kind);

} // namespace hot::ls
