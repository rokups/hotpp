// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The identity key of a lambda. The key survives an edit of the script, so a later step can
// find the same lambda in a new generation.
//
// The key text is "<base>/<part>". The base is "<unit>:<Class>::<function id>", or
// "<unit>:<function id>" for a free function. The function id is the id of the slot of the
// function: the key of an overload ("run__ov_<signature>"), the id of a native slot for an
// override, "ctor__ov_<signature>" for a constructor. A lambda inside a lambda adds its own
// "/<part>" after the part of its parent. A part is one of these, in this order:
//   n:<name>      the lambda has [name("<name>")]
//   v:<variable>  the lambda is the direct initializer of a local, or the direct right side of an
//                 assignment to a local or to a field ("x = ..." or "this.x = ...")
//   #<ordinal>    else. The ordinal counts the lambdas of the function that have no name and no
//                 variable, in source order, nested lambdas included.
// Each [alias("old")] gives an alias key: the same key with the part "n:old".
//
// A field initializer has no key: the lowering does not allow a lambda there.
//
// A bound method group (doc/lambdas.md, "Method groups") is a lambda of the lowering. Its part is
// "m:<object>.<Method>": "m:this.Hit" for "Hit" and "this.Hit", "m:o.Hit" for the local "o".
// Two subscriptions of one method are legal, so two equal "m:" parts are not an error. The
// second equal part gets the suffix "#1", the third "#2", and so on, in source order. The parts
// "m:" and "#<ordinal>" do not share a counter.
#pragma once

#include "ast.h"
#include "diag.h"

#include <map>
#include <string>
#include <vector>

namespace hotc
{

// Sets LambdaExpr::key and LambdaExpr::alias_keys for each lambda of the body. Reports the
// collisions and an alias without a name as errors. Two keys of one function are equal when
// they have the same parents and the same part. "all" (optional) gets the key of each lambda
// and then its alias keys, in source order.
void assign_lambda_keys(
    ast::BlockStmt& body, const std::string& base, const LineMap* map, Diagnostics& diag,
    std::vector<std::string>* all = nullptr);

// The key of a bound method group: "<prefix>/m:<object>.<method>", with the ordinal suffix of an
// equal part. "prefix" is the base of the function, or the key of the lambda around the group.
// "seen" counts the equal parts of one function.
std::string method_group_key(const std::string& prefix, const std::string& object, const std::string& method,
    std::map<std::string, int>& seen);

// The error for a method group outside a call argument.
std::string method_group_error(const std::string& method);

} // namespace hotc
