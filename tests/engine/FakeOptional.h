// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A minimal optional of a host. It is not std::optional. The runtime tests use it as "[optional]".
#pragma once
#include <utility>

namespace fake
{
struct nullopt_t
{
};
inline constexpr nullopt_t nullopt{};

template <class T> struct optional
{
    using value_type = T;
    optional() = default;
    optional(nullopt_t) {}
    optional(T v) : value_(std::move(v)), has_(true) {}
    bool has_value() const { return has_; }
    explicit operator bool() const { return has_; }
    T& operator*() & { return value_; }
    const T& operator*() const& { return value_; }
    T&& operator*() && { return std::move(value_); }
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    T& value() { return value_; }
    const T& value() const { return value_; }

private:
    T value_{};
    bool has_ = false;
};
} // namespace fake
