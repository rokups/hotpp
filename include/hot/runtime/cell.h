// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// The shared cell of a captured variable (G06). A lambda that writes a captured local or a
// captured parameter makes the variable a shared cell. The variable lives on the heap. The
// function and each lambda hold a copy of the cell handle, so all of them see one value.
// The translator writes "auto __cell_x = hot::make_cell<T>(init); T& x = *__cell_x;". A lambda
// captures "__cell_x" by value and binds "auto& x = *__cell_x;" again in its body.
// The cell uses no RTTI and no virtual call of its own, so it is AOT safe.
#pragma once

#include <memory>
#include <utility>

namespace hot
{

template <class T>
using Cell = std::shared_ptr<T>;

// A cell with the value-initialized T: zero, null or the default constructor.
template <class T>
Cell<T> make_cell()
{
    return std::make_shared<T>();
}

// A cell with the initial value "v". With an explicit T, "v" can be a braced list.
template <class T>
Cell<T> make_cell(T v)
{
    return std::make_shared<T>(std::move(v));
}

} // namespace hot
